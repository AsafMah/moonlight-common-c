#include "PyrowaveReassembly.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static LI_PYROWAVE_PACKET_HEADER makeHeader(
        uint32_t frameId, uint16_t blockIndex, uint16_t blockCount, uint16_t payloadLength) {
    LI_PYROWAVE_PACKET_HEADER header = {
        .version = LI_PYROWAVE_PROTOCOL_VERSION,
        .packetKind = LI_PYROWAVE_PACKET_DATA,
        .flags = 0,
        .headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE,
        .frameId = frameId,
        .rtpTimestamp = frameId * 90,
        .codecPayloadLength = (uint32_t)blockCount * payloadLength,
        .protectedPayloadLength = (uint32_t)blockCount * payloadLength,
        .fecScheme = LI_PYROWAVE_FEC_SCHEME_NONE,
        .dataBlockCount = blockCount,
        .fecBlockPayloadSize = payloadLength,
        .blockIndex = blockIndex,
        .blockCount = blockCount,
        .payloadLength = payloadLength,
    };

    if (blockIndex == 0) {
        header.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
    }
    if (blockIndex + 1u == blockCount) {
        header.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
    }
    return header;
}

int main(void) {
    static const uint8_t firstPayload[] = { 0x01, 0x02, 0x03 };
    static const uint8_t secondPayload[] = { 0x03, 0x04, 0x05 };
    static const uint8_t expected[] = { 0x01, 0x02, 0x03, 0x03, 0x04, 0x05 };
    uint8_t firstPacket[LI_PYROWAVE_WIRE_HEADER_SIZE + sizeof(firstPayload)];
    uint8_t secondPacket[LI_PYROWAVE_WIRE_HEADER_SIZE + sizeof(secondPayload)];
    uint8_t output[64];
    size_t firstLength = 0;
    size_t secondLength = 0;
    size_t outputLength = 0;
    uint32_t frameId = 0;
    LI_PYROWAVE_REASSEMBLY_STATE state;
    LI_PYROWAVE_PACKET_HEADER firstHeader = makeHeader(9, 0, 2, sizeof(firstPayload));
    LI_PYROWAVE_PACKET_HEADER secondHeader = makeHeader(9, 1, 2, sizeof(secondPayload));

    CHECK(LiPyrowaveBuildPacket(&firstHeader, firstPayload, sizeof(firstPayload),
                                firstPacket, sizeof(firstPacket), &firstLength) ==
          LI_PYROWAVE_PACKET_OK);
    CHECK(LiPyrowaveBuildPacket(&secondHeader, secondPayload, sizeof(secondPayload),
                                secondPacket, sizeof(secondPacket), &secondLength) ==
          LI_PYROWAVE_PACKET_OK);

    LiPyrowaveReassemblyInitialize(&state, 16u * 1024u * 1024u);
    CHECK(LiPyrowaveReassemblyPushPacket(&state, secondPacket, secondLength, 100, 1000) ==
          LI_PYROWAVE_REASSEMBLY_ACCEPTED);
    CHECK(LiPyrowaveReassemblyPushPacket(&state, secondPacket, secondLength, 110, 1000) ==
          LI_PYROWAVE_REASSEMBLY_DUPLICATE);
    CHECK(LiPyrowaveReassemblyPushPacket(&state, firstPacket, firstLength, 120, 1000) ==
          LI_PYROWAVE_REASSEMBLY_COMPLETE);
    CHECK(LiPyrowaveReassemblyCopyFrame(&state, output, sizeof(output), &outputLength, &frameId) ==
          LI_PYROWAVE_REASSEMBLY_COMPLETE);
    CHECK(outputLength == sizeof(expected));
    CHECK(frameId == 9);
    CHECK(memcmp(output, expected, sizeof(expected)) == 0);
    CHECK(!LiPyrowaveReassemblyIsComplete(&state));

    LiPyrowaveReassemblyReset(&state);
    CHECK(LiPyrowaveReassemblyPushBytes(&state, firstPacket, 5, 600, 2000) ==
          LI_PYROWAVE_REASSEMBLY_NEED_MORE);
    CHECK(LiPyrowaveReassemblyPushBytes(
              &state, firstPacket + 5, firstLength - 5, 610, 2000) ==
          LI_PYROWAVE_REASSEMBLY_ACCEPTED);
    CHECK(LiPyrowaveReassemblyPushBytes(&state, secondPacket, secondLength, 620, 2000) ==
          LI_PYROWAVE_REASSEMBLY_COMPLETE);
    CHECK(LiPyrowaveReassemblyCopyFrame(&state, output, sizeof(output), &outputLength, &frameId) ==
          LI_PYROWAVE_REASSEMBLY_COMPLETE);
    CHECK(outputLength == sizeof(expected));
    CHECK(memcmp(output, expected, sizeof(expected)) == 0);

    LiPyrowaveReassemblyReset(&state);
    CHECK(LiPyrowaveReassemblyPushBytes(&state, firstPacket, 5, 100, 200) ==
          LI_PYROWAVE_REASSEMBLY_NEED_MORE);
    CHECK(LiPyrowaveReassemblyPushBytes(&state, NULL, 0, 201, 200) ==
          LI_PYROWAVE_REASSEMBLY_EXPIRED);

    CHECK(LiPyrowaveReassemblyPushPacket(&state, firstPacket, firstLength, 200, 250) ==
          LI_PYROWAVE_REASSEMBLY_ACCEPTED);
    CHECK(LiPyrowaveReassemblyPushPacket(&state, firstPacket, firstLength, 300, 250) ==
          LI_PYROWAVE_REASSEMBLY_EXPIRED);

    CHECK(LiPyrowaveReassemblyPushPacket(&state, secondPacket, secondLength, 400, 1000) ==
          LI_PYROWAVE_REASSEMBLY_ACCEPTED);
    {
        LI_PYROWAVE_PACKET_HEADER newer = makeHeader(10, 0, 1, sizeof(firstPayload));
        uint8_t newerPacket[LI_PYROWAVE_WIRE_HEADER_SIZE + sizeof(firstPayload)];
        size_t newerLength = 0;

        CHECK(LiPyrowaveBuildPacket(&newer, firstPayload, sizeof(firstPayload),
                                    newerPacket, sizeof(newerPacket), &newerLength) ==
              LI_PYROWAVE_PACKET_OK);
        CHECK(LiPyrowaveReassemblyPushPacket(&state, newerPacket, newerLength, 500, 1000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
    }

    /* A newer frame may arrive in the same transport fragment as an older
       incomplete frame. The parser must replace frame storage without freeing
       the wire buffer that still owns the current payload. */
    LiPyrowaveReassemblyReset(&state);
    {
        LI_PYROWAVE_PACKET_HEADER newer = makeHeader(11, 0, 1, sizeof(firstPayload));
        uint8_t newerPacket[LI_PYROWAVE_WIRE_HEADER_SIZE + sizeof(firstPayload)];
        uint8_t combined[sizeof(firstPacket) + sizeof(newerPacket)];
        size_t newerLength = 0;

        CHECK(LiPyrowaveBuildPacket(&newer, firstPayload, sizeof(firstPayload),
                                    newerPacket, sizeof(newerPacket), &newerLength) ==
              LI_PYROWAVE_PACKET_OK);
        memcpy(combined, firstPacket, firstLength);
        memcpy(combined + firstLength, newerPacket, newerLength);
        CHECK(LiPyrowaveReassemblyPushBytes(
                  &state, combined, firstLength + newerLength, 700, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(LiPyrowaveReassemblyCopyFrame(
                  &state, output, sizeof(output), &outputLength, &frameId) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(outputLength == sizeof(firstPayload));
        CHECK(frameId == 11);
        CHECK(memcmp(output, firstPayload, sizeof(firstPayload)) == 0);
    }
    LiPyrowaveReassemblyReset(&state);
    {
        LI_PYROWAVE_PACKET_HEADER current = makeHeader(11, 0, 2, sizeof(firstPayload));
        LI_PYROWAVE_PACKET_HEADER tail = makeHeader(11, 1, 2, sizeof(secondPayload));
        uint8_t currentPacket[sizeof(firstPacket)];
        uint8_t tailPacket[sizeof(secondPacket)];
        uint8_t combined[sizeof(firstPacket) + sizeof(secondPacket)];
        size_t currentLength = 0;
        size_t tailLength = 0;

        CHECK(LiPyrowaveBuildPacket(&current, firstPayload, sizeof(firstPayload),
                                    currentPacket, sizeof(currentPacket), &currentLength) ==
              LI_PYROWAVE_PACKET_OK);
        CHECK(LiPyrowaveBuildPacket(&tail, secondPayload, sizeof(secondPayload),
                                    tailPacket, sizeof(tailPacket), &tailLength) ==
              LI_PYROWAVE_PACKET_OK);
        CHECK(LiPyrowaveReassemblyPushBytes(&state, currentPacket, currentLength, 700, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        CHECK(LiPyrowaveReassemblyPushBytes(&state, firstPacket, firstLength, 710, 2000) ==
              LI_PYROWAVE_REASSEMBLY_STALE);
        CHECK(state.wireLength == 0 && state.frameId == 11 && state.receivedCount == 1);

        memcpy(combined, firstPacket, firstLength);
        memcpy(combined + firstLength, tailPacket, tailLength);
        CHECK(LiPyrowaveReassemblyPushBytes(
                  &state, combined, firstLength + tailLength, 720, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(LiPyrowaveReassemblyCopyFrame(
                  &state, output, sizeof(output), &outputLength, &frameId) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(frameId == 11 && outputLength == sizeof(expected));
        CHECK(memcmp(output, expected, sizeof(expected)) == 0);
    }

    LiPyrowaveReassemblyReset(&state);
    {
        const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
        LI_PYROWAVE_PACKET_HEADER header = makeHeader(12, 0, 3, sizeof(payload));
        header.codecPayloadLength = header.protectedPayloadLength = 14;
        header.payloadLength = 4;
        CHECK(!LiPyrowaveValidatePacketHeader(&header));
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 730, 2000) ==
              LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);

        header.payloadLength = 5;
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 730, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        header.blockIndex = 1;
        header.flags = 0;
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 740, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        header.blockIndex = 2;
        header.flags = LI_PYROWAVE_FLAG_END_OF_FRAME;
        header.payloadLength = 3;
        CHECK(!LiPyrowaveValidatePacketHeader(&header));
        header.payloadLength = 4;
        CHECK(LiPyrowaveValidatePacketHeader(&header));
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 750, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(LiPyrowaveReassemblyCopyFrame(
                  &state, output, sizeof(output), &outputLength, &frameId) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(outputLength == 14);

        header = makeHeader(13, 0, 2, sizeof(payload));
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 760, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        header.blockIndex = 1;
        header.flags = LI_PYROWAVE_FLAG_END_OF_FRAME;
        header.fecBlockPayloadSize = 6;
        CHECK(LiPyrowaveValidatePacketHeader(&header));
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 770, 2000) ==
              LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);

        header = makeHeader(14, 0, 1, sizeof(payload));
        CHECK(LiPyrowaveReassemblyPush(&state, &header, payload, 780, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        state.blockLengths[0]--;
        CHECK(LiPyrowaveReassemblyCopyFrame(
                  &state, output, sizeof(output), &outputLength, &frameId) ==
              LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);
        CHECK(outputLength == 0 && !LiPyrowaveReassemblyIsComplete(&state));
    }

    LiPyrowaveReassemblyDestroy(&state);
    LiPyrowaveReassemblyInitialize(&state, 8);
    {
        const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
        LI_PYROWAVE_PACKET_HEADER oversized = makeHeader(15, 0, 2, sizeof(payload));
        LI_PYROWAVE_PACKET_HEADER valid = makeHeader(16, 0, 1, sizeof(payload));
        uint8_t packet[LI_PYROWAVE_WIRE_HEADER_SIZE + sizeof(payload)];
        size_t packetLength = 0;

        CHECK(LiPyrowaveReassemblyPush(&state, &valid, payload, 790, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(LiPyrowaveBuildPacket(&oversized, payload, sizeof(payload),
                                    packet, sizeof(packet), &packetLength) ==
              LI_PYROWAVE_PACKET_OK);
        CHECK(LiPyrowaveReassemblyPushBytes(&state, packet, packetLength, 800, 2000) ==
              LI_PYROWAVE_REASSEMBLY_OVERSIZE);
        CHECK(state.wireLength == 0 && state.wireBuffer == NULL && state.blockCount == 0);
        CHECK(state.maxFrameSize == 8);

        CHECK(LiPyrowaveBuildPacket(&valid, payload, sizeof(payload),
                                    packet, sizeof(packet), &packetLength) ==
              LI_PYROWAVE_PACKET_OK);
        CHECK(LiPyrowaveReassemblyPushBytes(&state, packet, packetLength, 810, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(LiPyrowaveReassemblyCopyFrame(
                  &state, output, sizeof(output), &outputLength, &frameId) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(frameId == 16 && outputLength == sizeof(payload));
        CHECK(memcmp(output, payload, sizeof(payload)) == 0);
    }
    LiPyrowaveReassemblyDestroy(&state);
    LiPyrowaveReassemblyInitialize(&state, 0);

    /* Exercise the largest wire block count. A wider loop counter makes the
       boundary explicit; a < 65535 loop does not increment past 65535. */
    LiPyrowaveReassemblyReset(&state);
    {
        const uint8_t payload = 0x7f;
        LI_PYROWAVE_PACKET_HEADER maximum =
            makeHeader(12, 0, UINT16_MAX, sizeof(payload));

        CHECK(LiPyrowaveReassemblyPush(&state, &maximum, &payload, 800, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        LiPyrowaveReassemblyReset(&state);
    }
    /* Block-aware FEC recovers one missing PyroWave data block before the
       complete frame is handed to the decoder. */
    LiPyrowaveReassemblyReset(&state);
    {
        static const uint8_t data0[] = { 1, 2, 3, 4, 5 };
        static const uint8_t data1[] = { 6, 7, 8, 9, 10 };
        static const uint8_t data2[] = { 11, 12, 13, 14, 15 };
        static const uint8_t parity[] = { 0x0c, 0x09, 0x06, 0x03, 0x00 };
        uint8_t packets[4][LI_PYROWAVE_WIRE_FEC_HEADER_SIZE + sizeof(data0)] = { 0 };
        size_t lengths[4] = { 0 };
        LI_PYROWAVE_PACKET_HEADER headers[4] = { 0 };
        const uint8_t *payloads[4] = { data0, data1, data2, parity };
        const uint32_t parityPacketsBefore = state.fecParityPacketsReceived;
        const uint32_t incompleteFramesBefore = state.incompleteFramesDropped;
        size_t index;

        for (index = 0; index < 4; ++index) {
            headers[index].version = LI_PYROWAVE_PROTOCOL_VERSION;
            headers[index].packetKind = index == 3 ? LI_PYROWAVE_PACKET_PARITY : LI_PYROWAVE_PACKET_DATA;
            headers[index].flags = index == 3 ? LI_PYROWAVE_FLAG_FEC_PARITY : 0;
            if (index == 0) headers[index].flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
            if (index == 2) headers[index].flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
            headers[index].headerLength = LI_PYROWAVE_WIRE_FEC_HEADER_SIZE;
            headers[index].frameId = 20;
            headers[index].rtpTimestamp = 1800;
            headers[index].blockIndex = (uint16_t)index;
            headers[index].blockCount = 4;
            headers[index].payloadLength = 5;
            headers[index].dataBlockCount = 3;
            headers[index].parityBlockCount = 1;
            headers[index].codecPayloadLength = 15;
            headers[index].protectedPayloadLength = 15;
            headers[index].fecScheme = LI_PYROWAVE_FEC_SCHEME_XOR;
            headers[index].fecGroupIndex = 0;
            headers[index].fecDataCount = 3;
            headers[index].fecParityCount = 1;
            headers[index].fecShardIndex = index == 3 ? 3 : (uint8_t)index;
            headers[index].fecBlockPayloadSize = 5;
            CHECK(LiPyrowaveBuildPacket(
                      &headers[index], payloads[index], 5, packets[index],
                      sizeof(packets[index]), &lengths[index]) == LI_PYROWAVE_PACKET_OK);
        }

        CHECK(LiPyrowaveReassemblyPushPacket(&state, packets[2], lengths[2], 900, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        CHECK(LiPyrowaveReassemblyPushPacket(&state, packets[3], lengths[3], 910, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        CHECK(LiPyrowaveReassemblyPushPacket(&state, packets[0], lengths[0], 920, 2000) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(state.fecRecoveredCount == 1);
        CHECK(state.fecParityPacketsReceived == parityPacketsBefore + 1);
        CHECK(LiPyrowaveReassemblyCopyFrame(
                  &state, output, sizeof(output), &outputLength, &frameId) ==
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
        CHECK(outputLength == 15);
        CHECK(frameId == 20);
        CHECK(output[0] == 1 && output[5] == 6 && output[14] == 15);
        /* A delivered frame isn't counted as dropped, and the next frame
           starts with no recovered blocks. */
        CHECK(state.fecRecoveredCount == 0);
        CHECK(state.incompleteFramesDropped == incompleteFramesBefore);

        /* Dropping a partial frame is counted; totals survive the reset. */
        headers[2].frameId = 21;
        CHECK(LiPyrowaveBuildPacket(
                  &headers[2], payloads[2], 5, packets[2],
                  sizeof(packets[2]), &lengths[2]) == LI_PYROWAVE_PACKET_OK);
        CHECK(LiPyrowaveReassemblyPushPacket(&state, packets[2], lengths[2], 930, 2000) ==
              LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        LiPyrowaveReassemblyReset(&state);
        CHECK(state.incompleteFramesDropped == incompleteFramesBefore + 1);
        CHECK(state.fecParityPacketsReceived == parityPacketsBefore + 1);
    }
    LiPyrowaveReassemblyDestroy(&state);
    puts("pyrowave reassembly golden vectors passed");
    return 0;
}
