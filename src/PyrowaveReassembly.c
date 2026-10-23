/**
 * @file PyrowaveReassembly.c
 * @brief Bounded, transport-only PyroWave frame reassembly.
 */
#include "PyrowaveReassembly.h"

#include <stdlib.h>
#include <string.h>

static bool isBefore32(uint32_t value, uint32_t reference) {
    return (uint32_t)(value - reference) > UINT32_MAX / 2u;
}

static void clearFrameStorage(LI_PYROWAVE_REASSEMBLY_STATE* state) {
    uint32_t index;

    if (state == NULL) {
        return;
    }

    if (state->blockCount != 0 && state->receivedCount < state->blockCount) {
        ++state->incompleteFramesDropped;
    }

    if (state->blocks != NULL) {
        for (index = 0; index < state->blockCount; ++index) {
            free(state->blocks[index]);
        }
    }
    if (state->fecParityBlocks != NULL) {
        for (index = 0; index < state->fecGroupCount; ++index) {
            free(state->fecParityBlocks[index]);
        }
    }
    free(state->blocks);
    free(state->blockLengths);
    free(state->received);
    free(state->fecParityBlocks);
    free(state->fecParityLengths);
    free(state->fecParityReceived);
    state->blocks = NULL;
    state->blockLengths = NULL;
    state->received = NULL;
    state->fecParityBlocks = NULL;
    state->fecParityLengths = NULL;
    state->fecParityReceived = NULL;
    state->blockCount = 0;
    state->wireBlockCount = 0;
    state->receivedCount = 0;
    state->fecGroupCount = 0;
    state->frameId = 0;
    state->totalPayloadLength = 0;
    state->framePayloadLength = 0;
    state->codecPayloadLength = 0;
    state->metadataLength = 0;
    state->metadataFlags = 0;
    state->fecBlockPayloadSize = 0;
    state->fecScheme = LI_PYROWAVE_FEC_SCHEME_NONE;
    state->headerReceived = false;
    state->fecEnabled = false;
    state->deadlineUs = 0;
    state->fecRecoveredCount = 0;
}

static void clearStorage(LI_PYROWAVE_REASSEMBLY_STATE* state) {
    if (state == NULL) {
        return;
    }

    clearFrameStorage(state);
    free(state->wireBuffer);
    state->wireBuffer = NULL;
    state->wireLength = 0;
    state->wireCapacity = 0;
}

static LI_PYROWAVE_REASSEMBLY_RESULT beginFrame(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        const LI_PYROWAVE_PACKET_HEADER* header,
        uint64_t deadlineUs) {
    const bool fec = header->fecScheme == LI_PYROWAVE_FEC_SCHEME_XOR;
    const uint16_t dataBlockCount = fec ? header->dataBlockCount : header->blockCount;
    const uint16_t fecGroupCount = fec
        ? (uint16_t)((dataBlockCount + LI_PYROWAVE_FEC_DATA_PER_GROUP - 1u) /
                     LI_PYROWAVE_FEC_DATA_PER_GROUP)
        : 0;

    state->blocks = calloc(dataBlockCount, sizeof(*state->blocks));
    state->blockLengths = calloc(dataBlockCount, sizeof(*state->blockLengths));
    state->received = calloc(dataBlockCount, sizeof(*state->received));
    if (fec) {
        state->fecParityBlocks = calloc(fecGroupCount, sizeof(*state->fecParityBlocks));
        state->fecParityLengths = calloc(fecGroupCount, sizeof(*state->fecParityLengths));
        state->fecParityReceived = calloc(fecGroupCount, sizeof(*state->fecParityReceived));
    }
    if (state->blocks == NULL || state->blockLengths == NULL || state->received == NULL ||
            (fec && (state->fecParityBlocks == NULL || state->fecParityLengths == NULL ||
                     state->fecParityReceived == NULL))) {
        clearFrameStorage(state);
        return LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY;
    }

    state->blockCount = dataBlockCount;
    state->wireBlockCount = header->blockCount;
    state->fecGroupCount = fecGroupCount;
    state->framePayloadLength = header->protectedPayloadLength;
    state->codecPayloadLength = header->codecPayloadLength;
    state->metadataLength = header->metadataLength;
    state->metadataFlags = header->metadataFlags;
    state->fecBlockPayloadSize = header->fecBlockPayloadSize;
    state->fecScheme = header->fecScheme;
    state->headerReceived = false;
    state->fecEnabled = fec;
    state->frameId = header->frameId;
    state->deadlineUs = deadlineUs;
    return LI_PYROWAVE_REASSEMBLY_ACCEPTED;
}

void LiPyrowaveReassemblyInitialize(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        uint32_t maxFrameSize) {
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->maxFrameSize = maxFrameSize == 0 ?
        LI_PYROWAVE_DEFAULT_MAX_FRAME_SIZE : maxFrameSize;
}

void LiPyrowaveReassemblyReset(LI_PYROWAVE_REASSEMBLY_STATE* state) {
    uint32_t maxFrameSize;

    if (state == NULL) {
        return;
    }

    maxFrameSize = state->maxFrameSize;
    clearStorage(state);
    state->maxFrameSize = maxFrameSize == 0 ?
        LI_PYROWAVE_DEFAULT_MAX_FRAME_SIZE : maxFrameSize;
}

void LiPyrowaveReassemblyDestroy(LI_PYROWAVE_REASSEMBLY_STATE* state) {
    if (state == NULL) {
        return;
    }

    clearStorage(state);
    memset(state, 0, sizeof(*state));
}

static void tryRecoverGroup(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        uint16_t groupIndex) {
    uint16_t groupStart;
    uint16_t groupCount;
    uint16_t missing = UINT16_MAX;
    uint16_t missingCount = 0;
    uint16_t index;
    uint16_t recoveredLength;
    uint8_t* recovered;

    if (state == NULL || !state->fecEnabled ||
            groupIndex >= state->fecGroupCount ||
            state->fecParityReceived[groupIndex] == 0) {
        return;
    }

    groupStart = (uint16_t)(groupIndex * LI_PYROWAVE_FEC_DATA_PER_GROUP);
    groupCount = (uint16_t)(state->blockCount - groupStart);
    if (groupCount > LI_PYROWAVE_FEC_DATA_PER_GROUP) {
        groupCount = LI_PYROWAVE_FEC_DATA_PER_GROUP;
    }
    for (index = 0; index < groupCount; ++index) {
        if (state->received[groupStart + index] == 0) {
            missing = (uint16_t)(groupStart + index);
            ++missingCount;
        }
    }
    if (missingCount != 1 || state->fecParityBlocks[groupIndex] == NULL ||
            state->fecParityLengths[groupIndex] < state->fecBlockPayloadSize) {
        return;
    }

    recoveredLength = state->fecBlockPayloadSize;
    if (recoveredLength == 0 || recoveredLength > state->fecBlockPayloadSize ||
            recoveredLength > state->fecParityLengths[groupIndex]) {
        return;
    }

    recovered = malloc(recoveredLength);
    if (recovered == NULL) {
        return;
    }
    memcpy(recovered, state->fecParityBlocks[groupIndex], recoveredLength);
    for (index = 0; index < groupCount; ++index) {
        const uint16_t dataIndex = (uint16_t)(groupStart + index);
        uint16_t byteIndex;
        if (dataIndex == missing || state->blocks[dataIndex] == NULL) {
            continue;
        }
        for (byteIndex = 0; byteIndex < recoveredLength; ++byteIndex) {
            if (byteIndex < state->blockLengths[dataIndex]) {
                recovered[byteIndex] ^= state->blocks[dataIndex][byteIndex];
            }
        }
    }

    state->blocks[missing] = recovered;
    state->blockLengths[missing] = recoveredLength;
    state->received[missing] = 1;
    ++state->receivedCount;
    ++state->fecRecoveredCount;
    state->totalPayloadLength += recoveredLength;
}

LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyPush(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        const LI_PYROWAVE_PACKET_HEADER* header,
        const uint8_t* payload,
        uint64_t nowUs,
        uint64_t deadlineUs) {
    LI_PYROWAVE_REASSEMBLY_RESULT result;
    uint64_t nextTotal;
    uint8_t* block;

    if (state == NULL || header == NULL || payload == NULL || state->maxFrameSize == 0) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT;
    }
    if (!LiPyrowaveValidatePacketHeader(header)) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
    }
    if (header->payloadLength > state->maxFrameSize ||
            header->protectedPayloadLength > state->maxFrameSize ||
            header->codecPayloadLength > state->maxFrameSize ||
            header->fecBlockPayloadSize > state->maxFrameSize) {
        return LI_PYROWAVE_REASSEMBLY_OVERSIZE;
    }

    if (state->blockCount != 0 && nowUs != 0 && state->deadlineUs != 0 &&
            nowUs > state->deadlineUs) {
        LiPyrowaveReassemblyReset(state);
        return LI_PYROWAVE_REASSEMBLY_EXPIRED;
    }

    if (state->blockCount == 0) {
        result = beginFrame(state, header, deadlineUs);
        if (result != LI_PYROWAVE_REASSEMBLY_ACCEPTED) {
            return result;
        }
    }
    else if (header->frameId != state->frameId) {
        if (isBefore32(header->frameId, state->frameId)) {
            return LI_PYROWAVE_REASSEMBLY_STALE;
        }

        clearFrameStorage(state);
        result = beginFrame(state, header, deadlineUs);
        if (result != LI_PYROWAVE_REASSEMBLY_ACCEPTED) {
            return result;
        }
    }

    if ((header->fecScheme == LI_PYROWAVE_FEC_SCHEME_XOR) != state->fecEnabled ||
            header->dataBlockCount != state->blockCount ||
            header->blockCount != state->wireBlockCount ||
            header->codecPayloadLength != state->codecPayloadLength ||
            header->metadataLength != state->metadataLength ||
            header->metadataFlags != state->metadataFlags ||
            header->protectedPayloadLength != state->framePayloadLength ||
            header->fecBlockPayloadSize != state->fecBlockPayloadSize ||
            header->fecScheme != state->fecScheme) {
        LiPyrowaveReassemblyReset(state);
        return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
    }
    if (state->deadlineUs == 0 && deadlineUs != 0) {
        state->deadlineUs = deadlineUs;
    }
    if (header->packetKind == LI_PYROWAVE_PACKET_FRAME_HEADER) {
        if (state->headerReceived) {
            return LI_PYROWAVE_REASSEMBLY_DUPLICATE;
        }
        state->headerReceived = true;
        return LI_PYROWAVE_REASSEMBLY_ACCEPTED;
    }
    if ((header->flags & LI_PYROWAVE_FLAG_FEC_PARITY) != 0) {
        if (state->fecParityReceived[header->fecGroupIndex] != 0) {
            return LI_PYROWAVE_REASSEMBLY_DUPLICATE;
        }
        block = malloc(header->payloadLength);
        if (block == NULL) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY;
        }
        memcpy(block, payload, header->payloadLength);
        state->fecParityBlocks[header->fecGroupIndex] = block;
        state->fecParityLengths[header->fecGroupIndex] = header->payloadLength;
        state->fecParityReceived[header->fecGroupIndex] = 1;
        ++state->fecParityPacketsReceived;
        tryRecoverGroup(state, header->fecGroupIndex);
    }
    else {
        if (state->received[header->blockIndex] != 0) {
            return LI_PYROWAVE_REASSEMBLY_DUPLICATE;
        }
        const uint64_t frameStorageLimit = state->fecEnabled
            ? (uint64_t)state->framePayloadLength + state->fecBlockPayloadSize
            : state->maxFrameSize;
        if (state->totalPayloadLength + header->payloadLength > frameStorageLimit) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_OVERSIZE;
        }

        block = malloc(header->payloadLength);
        if (block == NULL) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY;
        }
        memcpy(block, payload, header->payloadLength);
        state->blocks[header->blockIndex] = block;
        state->blockLengths[header->blockIndex] = header->payloadLength;
        state->received[header->blockIndex] = 1;
        state->receivedCount++;
        nextTotal = state->totalPayloadLength + header->payloadLength;
        state->totalPayloadLength = nextTotal;
        if (state->fecEnabled) {
            tryRecoverGroup(state, header->fecGroupIndex);
        }
    }

    if (state->receivedCount == state->blockCount) {
        if (state->fecEnabled &&
                 (state->totalPayloadLength < state->framePayloadLength ||
                 state->totalPayloadLength - state->framePayloadLength >= state->fecBlockPayloadSize)) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
        }
        return LI_PYROWAVE_REASSEMBLY_COMPLETE;
    }
    return LI_PYROWAVE_REASSEMBLY_ACCEPTED;
}

LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyPushPacket(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        const uint8_t* packet,
        size_t packetLength,
        uint64_t nowUs,
        uint64_t deadlineUs) {
    LI_PYROWAVE_PACKET_HEADER header;
    const uint8_t* payload;
    LI_PYROWAVE_PACKET_RESULT parseResult;

    parseResult = LiPyrowaveParsePacket(packet, packetLength, &header, &payload);
    if (parseResult != LI_PYROWAVE_PACKET_OK) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
    }

    return LiPyrowaveReassemblyPush(state, &header, payload, nowUs, deadlineUs);
}

LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyPushBytes(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        const uint8_t* data,
        size_t dataLength,
        uint64_t nowUs,
        uint64_t deadlineUs) {
    LI_PYROWAVE_REASSEMBLY_RESULT lastResult = LI_PYROWAVE_REASSEMBLY_NEED_MORE;

    if (state == NULL || (data == NULL && dataLength != 0) || state->maxFrameSize == 0) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT;
    }
    if (state->wireLength != 0 && nowUs != 0 && deadlineUs != 0 && nowUs > deadlineUs) {
        LiPyrowaveReassemblyReset(state);
        return LI_PYROWAVE_REASSEMBLY_EXPIRED;
    }
    if (dataLength > LI_PYROWAVE_MAX_PACKET_SIZE - state->wireLength) {
        LiPyrowaveReassemblyReset(state);
        return LI_PYROWAVE_REASSEMBLY_OVERSIZE;
    }
    if (dataLength != 0) {
        if (state->wireCapacity < state->wireLength + dataLength) {
            uint8_t* buffer = realloc(state->wireBuffer, LI_PYROWAVE_MAX_PACKET_SIZE);
            if (buffer == NULL) {
                LiPyrowaveReassemblyReset(state);
                return LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY;
            }
            state->wireBuffer = buffer;
            state->wireCapacity = LI_PYROWAVE_MAX_PACKET_SIZE;
        }
        memcpy(state->wireBuffer + state->wireLength, data, dataLength);
        state->wireLength += dataLength;
    }

    while (state->wireLength >= LI_PYROWAVE_WIRE_HEADER_SIZE) {
        size_t packetLength;
        LI_PYROWAVE_PACKET_HEADER header;
        const uint8_t* payload;
        LI_PYROWAVE_PACKET_RESULT parseResult;

        if (state->wireBuffer[0] != 'P' || state->wireBuffer[1] != 'Y' ||
                state->wireBuffer[2] != 'R' || state->wireBuffer[3] != 'F') {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
        }
        const size_t headerLength =
            ((size_t)state->wireBuffer[8] << 8 | state->wireBuffer[9]);
        if (headerLength != LI_PYROWAVE_WIRE_HEADER_SIZE) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
        }
        packetLength = headerLength +
            ((size_t)state->wireBuffer[50] << 8 | state->wireBuffer[51]);
        if (packetLength > LI_PYROWAVE_MAX_PACKET_SIZE) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_OVERSIZE;
        }
        if (state->wireLength < packetLength) {
            return LI_PYROWAVE_REASSEMBLY_NEED_MORE;
        }

        parseResult = LiPyrowaveParsePacket(
            state->wireBuffer, packetLength, &header, &payload);
        if (parseResult != LI_PYROWAVE_PACKET_OK) {
            LiPyrowaveReassemblyReset(state);
            return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
        }
        lastResult = LiPyrowaveReassemblyPush(state, &header, payload, nowUs, deadlineUs);
        if (lastResult < LI_PYROWAVE_REASSEMBLY_ACCEPTED) {
            LiPyrowaveReassemblyReset(state);
            return lastResult;
        }
        state->wireLength -= packetLength;
        if (state->wireLength != 0) {
            memmove(state->wireBuffer, state->wireBuffer + packetLength, state->wireLength);
        }
        if (lastResult == LI_PYROWAVE_REASSEMBLY_COMPLETE) {
            return lastResult;
        }
    }
    return lastResult;
}

bool LiPyrowaveReassemblyIsComplete(const LI_PYROWAVE_REASSEMBLY_STATE* state) {
    return state != NULL && state->blockCount != 0 &&
           state->receivedCount == state->blockCount;
}

LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyCopyMetadata(
        const LI_PYROWAVE_REASSEMBLY_STATE* state,
        uint8_t* output,
        size_t outputCapacity,
        size_t* outputLength,
        uint16_t* metadataFlags) {
    uint32_t index;
    size_t copied = 0;

    if (state == NULL || outputLength == NULL ||
            (output == NULL && state->metadataLength != 0)) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT;
    }
    *outputLength = 0;
    if (!LiPyrowaveReassemblyIsComplete(state)) {
        return LI_PYROWAVE_REASSEMBLY_NOT_READY;
    }
    if (outputCapacity < state->metadataLength) {
        return LI_PYROWAVE_REASSEMBLY_OUTPUT_TOO_SMALL;
    }
    for (index = 0; index < state->blockCount && copied < state->metadataLength; ++index) {
        const size_t to_copy = (state->metadataLength - copied < state->blockLengths[index])
            ? state->metadataLength - copied : state->blockLengths[index];
        memcpy(output + copied, state->blocks[index], to_copy);
        copied += to_copy;
    }
    if (copied != state->metadataLength) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
    }
    *outputLength = copied;
    if (metadataFlags != NULL) {
        *metadataFlags = state->metadataFlags;
    }
    return LI_PYROWAVE_REASSEMBLY_COMPLETE;
}

LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyCopyFrame(
        LI_PYROWAVE_REASSEMBLY_STATE* state,
        uint8_t* output,
        size_t outputCapacity,
        size_t* outputLength,
        uint32_t* frameId) {
    uint32_t index;
    size_t output_offset = 0;

    if (state == NULL || output == NULL || outputLength == NULL) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT;
    }
    *outputLength = 0;
    if (!LiPyrowaveReassemblyIsComplete(state)) {
        return LI_PYROWAVE_REASSEMBLY_NOT_READY;
    }
    const size_t frameLength = state->codecPayloadLength;
    if (outputCapacity < frameLength) {
        return LI_PYROWAVE_REASSEMBLY_OUTPUT_TOO_SMALL;
    }

    for (index = 0; index < state->blockCount; ++index) {
        const size_t protected_offset = state->metadataLength;
        const size_t block_begin = (size_t)index * state->fecBlockPayloadSize;
        const size_t block_end = block_begin + state->blockLengths[index];
        const size_t copy_begin = protected_offset > block_begin ? protected_offset : block_begin;
        const size_t copy_end = block_end < protected_offset + frameLength
            ? block_end : protected_offset + frameLength;
        const size_t copyLength = copy_end > copy_begin ? copy_end - copy_begin : 0;
        if (copyLength != 0) {
            memcpy(output + output_offset, state->blocks[index] + (copy_begin - block_begin), copyLength);
            output_offset += copyLength;
        }
    }
    if (output_offset != frameLength) {
        clearFrameStorage(state);
        return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
    }
    *outputLength = output_offset;
    if (frameId != NULL) {
        *frameId = state->frameId;
    }
    clearFrameStorage(state);
    return LI_PYROWAVE_REASSEMBLY_COMPLETE;
}
