/**
 * @file PyrowaveReassembly.h
 * @brief Bounded, transport-only PyroWave frame reassembly.
 */
#pragma once

#include "PyrowaveProtocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LI_PYROWAVE_DEFAULT_MAX_FRAME_SIZE (16u * 1024u * 1024u)

typedef enum _LI_PYROWAVE_REASSEMBLY_RESULT {
    LI_PYROWAVE_REASSEMBLY_ACCEPTED = 0,
    LI_PYROWAVE_REASSEMBLY_COMPLETE = 1,
    LI_PYROWAVE_REASSEMBLY_DUPLICATE = 2,
    LI_PYROWAVE_REASSEMBLY_STALE = 3,
    LI_PYROWAVE_REASSEMBLY_NOT_READY = 4,
    LI_PYROWAVE_REASSEMBLY_NEED_MORE = 5,
    LI_PYROWAVE_REASSEMBLY_EXPIRED = -1,
    LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT = -2,
    LI_PYROWAVE_REASSEMBLY_INVALID_PACKET = -3,
    LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY = -4,
    LI_PYROWAVE_REASSEMBLY_OVERSIZE = -5,
    LI_PYROWAVE_REASSEMBLY_OUTPUT_TOO_SMALL = -6,
} LI_PYROWAVE_REASSEMBLY_RESULT;

typedef struct _LI_PYROWAVE_REASSEMBLY_STATE {
    uint8_t** blocks;
    uint16_t* blockLengths;
    uint8_t* received;
    uint16_t blockCount;       /* Original data block count. */
    uint16_t wireBlockCount;   /* Data plus parity packets. */
    uint16_t receivedCount;
    uint16_t fecGroupCount;
    uint32_t frameId;
    /* Includes block padding; keep it wider than the 32-bit frame length so
       a maximal padded frame cannot wrap before validation/copy. */
    uint64_t totalPayloadLength;
    uint32_t framePayloadLength;
    uint32_t codecPayloadLength;
    uint16_t metadataLength;
    uint16_t metadataFlags;
    uint32_t maxFrameSize;
    uint16_t fecBlockPayloadSize;
    uint8_t fecScheme;
    bool headerReceived;
    bool fecEnabled;
    uint64_t deadlineUs;
    uint8_t* wireBuffer;
    size_t wireLength;
    size_t wireCapacity;
    uint8_t** fecParityBlocks;
    uint16_t* fecParityLengths;
    uint8_t* fecParityReceived;
    /* Data blocks of the current frame rebuilt from parity. */
    uint16_t fecRecoveredCount;
    /* Totals since initialization; Reset and new frames keep them. */
    uint32_t fecParityPacketsReceived;
    uint32_t incompleteFramesDropped;
} LI_PYROWAVE_REASSEMBLY_STATE;

/* A zero maxFrameSize selects LI_PYROWAVE_DEFAULT_MAX_FRAME_SIZE. */
void LiPyrowaveReassemblyInitialize(
    LI_PYROWAVE_REASSEMBLY_STATE* state,
    uint32_t maxFrameSize);

void LiPyrowaveReassemblyReset(LI_PYROWAVE_REASSEMBLY_STATE* state);

void LiPyrowaveReassemblyDestroy(LI_PYROWAVE_REASSEMBLY_STATE* state);

/*
 * Push one already parsed block. nowUs/deadlineUs use the caller's monotonic
 * clock; zero deadlineUs means no deadline. A newer frame replaces an older
 * incomplete frame, while an older frame is ignored.
 */
LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyPush(
    LI_PYROWAVE_REASSEMBLY_STATE* state,
    const LI_PYROWAVE_PACKET_HEADER* header,
    const uint8_t* payload,
    uint64_t nowUs,
    uint64_t deadlineUs);

/* Parse and push one complete wire packet. */
LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyPushPacket(
    LI_PYROWAVE_REASSEMBLY_STATE* state,
    const uint8_t* packet,
    size_t packetLength,
    uint64_t nowUs,
    uint64_t deadlineUs);

/* Accept arbitrary transport fragments; one fragment may contain partial or multiple packets. */
LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyPushBytes(
    LI_PYROWAVE_REASSEMBLY_STATE* state,
    const uint8_t* data,
    size_t dataLength,
    uint64_t nowUs,
    uint64_t deadlineUs);

bool LiPyrowaveReassemblyIsComplete(const LI_PYROWAVE_REASSEMBLY_STATE* state);

/* Copy the protected metadata TLV area without resetting the frame state. */
LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyCopyMetadata(
    const LI_PYROWAVE_REASSEMBLY_STATE* state,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength,
    uint16_t* metadataFlags);

/* Copy blocks in index order; a successful copy clears the assembled frame,
 * retaining any buffered bytes belonging to the next packet. */
LI_PYROWAVE_REASSEMBLY_RESULT LiPyrowaveReassemblyCopyFrame(
    LI_PYROWAVE_REASSEMBLY_STATE* state,
    uint8_t* output,
    size_t outputCapacity,
    size_t* outputLength,
    uint32_t* frameId);

#ifdef __cplusplus
}
#endif
