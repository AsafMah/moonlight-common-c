// Frames the host encoded but dropped before packetization (superseded in its
// send queue) leave a frame-number gap with no RTP sequence gap. They must be
// reported apart from network loss, and only when the packet numbering proves
// that nothing was sent for them. Genuine loss must still count as loss.
#define queueRtpPacket testQueueRtpPacket
#define connectionSawFrame testConnectionSawFrame
#define connectionHostSkippedFrames testConnectionHostSkippedFrames
#define notifyFrameLost testNotifyFrameLost
#define connectionRecordFecBlock testConnectionRecordFecBlock
#define connectionSendFrameFecStatus testConnectionSendFrameFecStatus
#define recordVideoFecStats testRecordVideoFecStats
#define isReferenceFrameInvalidationEnabled testIsReferenceFrameInvalidationEnabled
#include "../src/RtpVideoQueue.c"

#include <stdio.h>
#include <string.h>

#define PAYLOAD_SIZE ((int)sizeof(NV_VIDEO_PACKET) + 8)
#define PACKET_LENGTH ((int)sizeof(RTP_PACKET) + 4 + PAYLOAD_SIZE)
#define BUFFER_SIZE (PAYLOAD_SIZE + MAX_RTP_HEADER_SIZE)

static RTP_VIDEO_QUEUE queue;

// The connection-status loss accounting of ControlStream.c, which counts every
// frame-number gap between frames it saw as lost.
static uint32_t connLastSeen;
static uint32_t connCountedFrames;

void testQueueRtpPacket(PRTPV_QUEUE_ENTRY entry) {
    free(entry->packet);
}
void testConnectionSawFrame(uint32_t frameIndex) {
    if (connLastSeen != 0) {
        connCountedFrames += frameIndex - connLastSeen;
    }
    connLastSeen = frameIndex;
}
void testConnectionHostSkippedFrames(uint32_t lastSkippedFrame) {
    if (connLastSeen != 0 && isBefore32(connLastSeen, lastSkippedFrame)) {
        connLastSeen = lastSkippedFrame;
    }
}
void testNotifyFrameLost(unsigned int frameNumber, bool speculative) {
    (void)frameNumber;
    (void)speculative;
}
void testConnectionRecordFecBlock(uint32_t frame, uint8_t block, uint16_t data, uint16_t received) {
    (void)frame;
    (void)block;
    (void)data;
    (void)received;
}
void testConnectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus) {
    (void)fecStatus;
}
void testRecordVideoFecStats(uint32_t framesRecovered, uint32_t framesUnrecoverable, uint32_t fecPacketsReceived) {
    (void)framesRecovered;
    (void)framesUnrecoverable;
    (void)fecPacketsReceived;
}
bool testIsReferenceFrameInvalidationEnabled(void) { return false; }

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

// Checks the skipped frame count, the number of ranges and the newest range.
#define CHECK_SKIPS(count, rangeCount, first, last) do { \
    VIDEO_HOST_SKIP_STATS stats_; \
    VIDEO_HOST_SKIP_RANGE newest_ = {0, 0, 0}; \
    LiGetVideoHostSkipStats(&stats_); \
    if (stats_.rangesRecorded > 0) { \
        newest_ = stats_.ranges[(stats_.rangesRecorded - 1) % VIDEO_HOST_SKIP_RANGE_COUNT]; \
    } \
    if (stats_.framesSkipped != (count) || stats_.rangesRecorded != (rangeCount) || \
            newest_.firstFrameNumber != (first) || newest_.lastFrameNumber != (last) || \
            newest_.frames != (unsigned)((rangeCount) ? (last) - (first) + 1 : 0)) { \
        fprintf(stderr, "FAIL line %d: host skips %llu in %llu ranges, newest [%u..%u] %u, " \
                "expected %u in %u ranges, newest [%u..%u]\n", __LINE__, \
                (unsigned long long)stats_.framesSkipped, \
                (unsigned long long)stats_.rangesRecorded, newest_.firstFrameNumber, \
                newest_.lastFrameNumber, newest_.frames, (unsigned)(count), \
                (unsigned)(rangeCount), (unsigned)(first), (unsigned)(last)); \
        return 1; \
    } \
} while (0)

#define CHECK_COUNTED(expected) do { \
    if (connCountedFrames != (expected)) { \
        fprintf(stderr, "FAIL line %d: connection status counted %u frames, expected %u\n", \
                __LINE__, connCountedFrames, (unsigned)(expected)); \
        return 1; \
    } \
} while (0)

// Shard layout as Sunshine sends it: every data and parity shard takes the next
// stream packet index, and its low 16 bits are the RTP sequence number.
typedef struct {
    uint32_t dataShards;
    uint32_t fecPercentage;
    uint8_t blocks;
} FRAME_LAYOUT;

static uint32_t parityShards(const FRAME_LAYOUT* layout) {
    return (layout->dataShards * layout->fecPercentage + 99) / 100;
}

static uint32_t frameShards(const FRAME_LAYOUT* layout) {
    return layout->blocks * (layout->dataShards + parityShards(layout));
}

static void startStream(int videoFormat, uint32_t firstPacketIndex) {
    RtpvCleanupQueue(&queue);
    NegotiatedVideoFormat = videoFormat;
    StreamConfig.packetSize = PAYLOAD_SIZE;
    RtpvInitializeQueue(&queue);
    queue.multiFecCapable = true;
    queue.nextContiguousSequenceNumber = U16(firstPacketIndex);
    connLastSeen = 0;
    connCountedFrames = 0;
}

// Delivers one shard: the packet index is relative to the frame's first shard.
static void receiveShard(uint32_t frame, uint32_t frameStart, const FRAME_LAYOUT* layout,
                         uint8_t block, uint32_t shard, int32_t indexSkew) {
    uint32_t blockShards = layout->dataShards + parityShards(layout);
    uint32_t packetIndex = U24(frameStart + block * blockShards + shard);
    char* buffer = calloc(1, BUFFER_SIZE + sizeof(RTPV_QUEUE_ENTRY));
    PRTP_PACKET packet = (PRTP_PACKET)buffer;
    PNV_VIDEO_PACKET nvPacket = (PNV_VIDEO_PACKET)(buffer + sizeof(RTP_PACKET) + 4);
    bool parity = shard >= layout->dataShards;

    packet->header = 0x80 | FLAG_EXTENSION;
    packet->sequenceNumber = U16(packetIndex);
    // Parity shard headers are Reed-Solomon output
    nvPacket->streamPacketIndex = parity ? 0xA5A5A5A5 : (U24(packetIndex + indexSkew) << 8);
    nvPacket->frameIndex = frame;
    nvPacket->flags = FLAG_CONTAINS_PIC_DATA;
    if (block == 0 && shard == 0) {
        nvPacket->flags |= FLAG_SOF;
    }
    if (block == layout->blocks - 1 && shard == layout->dataShards - 1) {
        nvPacket->flags |= FLAG_EOF;
    }
    nvPacket->multiFecFlags = 0x10;
    nvPacket->multiFecBlocks = (uint8_t)((block << 4) | ((layout->blocks - 1) << 6));
    nvPacket->fecInfo = shard << 12 | layout->dataShards << 22 | layout->fecPercentage << 4;

    if (RtpvAddPacket(&queue, packet, PACKET_LENGTH, (PRTPV_QUEUE_ENTRY)&buffer[BUFFER_SIZE]) != RTPF_RET_QUEUED) {
        free(buffer);
    }
}

// Sends a frame in order, delivering every shard. Returns the next frame start.
static uint32_t receiveFrame(uint32_t frame, uint32_t frameStart, const FRAME_LAYOUT* layout) {
    uint32_t blockShards = layout->dataShards + parityShards(layout);
    for (uint8_t block = 0; block < layout->blocks; block++) {
        for (uint32_t shard = 0; shard < blockShards; shard++) {
            receiveShard(frame, frameStart, layout, block, shard, 0);
        }
    }
    return U24(frameStart + frameShards(layout));
}

// The host sent the frames but the network lost every packet of them.
static uint32_t loseFrames(uint32_t frames, uint32_t frameStart, const FRAME_LAYOUT* layout) {
    return U24(frameStart + frames * frameShards(layout));
}

static int testRecordedSkipBurst(void) {
    // 17:59:29 host log: the send queue dropped 74 superseded frames at once
    const FRAME_LAYOUT layout = {4, 20, 1};
    uint32_t start = 0;
    uint32_t frame;

    startStream(VIDEO_FORMAT_H265, start);
    for (frame = 1; frame <= 10; frame++) {
        start = receiveFrame(frame, start, &layout);
    }
    CHECK_SKIPS(0, 0, 0, 0);

    for (frame = 85; frame <= 90; frame++) {
        start = receiveFrame(frame, start, &layout);
    }
    CHECK_SKIPS(74, 1, 11, 84);
    // Only the 15 frames after the first were sent, and all arrived
    CHECK_COUNTED(15);
    CHECK(queue.currentFrameNumber == 91);
    return 0;
}

static int testSameBurstLostOnNetwork(void) {
    const FRAME_LAYOUT layout = {4, 20, 1};
    uint32_t start = 0;
    uint32_t frame;

    startStream(VIDEO_FORMAT_H265, start);
    for (frame = 1; frame <= 10; frame++) {
        start = receiveFrame(frame, start, &layout);
    }
    start = loseFrames(74, start, &layout);
    for (frame = 85; frame <= 90; frame++) {
        start = receiveFrame(frame, start, &layout);
    }
    CHECK_SKIPS(0, 0, 0, 0);
    CHECK_COUNTED(89);
    return 0;
}

static int testMixedSkipsAndLoss(void) {
    const FRAME_LAYOUT layout = {3, 50, 1};
    uint32_t start = 0;
    uint32_t frame;

    startStream(VIDEO_FORMAT_H265, start);
    start = receiveFrame(1, start, &layout);
    // Host skips 2-10
    for (frame = 11; frame <= 20; frame++) {
        start = receiveFrame(frame, start, &layout);
    }
    CHECK_SKIPS(9, 1, 2, 10);
    // Network loses 21-25
    start = loseFrames(5, start, &layout);
    start = receiveFrame(26, start, &layout);
    CHECK_SKIPS(9, 1, 2, 10);
    // Host skips 27-30
    start = receiveFrame(31, start, &layout);
    CHECK_SKIPS(13, 2, 27, 30);
    // One gap holding a skipped frame (32), a lost one (33) and a skipped one
    // (34) can't be split: all of it stays network loss.
    start = loseFrames(1, start, &layout);
    start = receiveFrame(35, start, &layout);
    CHECK_SKIPS(13, 2, 27, 30);
    // 34 frames after the first: 13 proven never sent, 8 lost or unproven, 13 received
    CHECK_COUNTED(34 - 13);
    return 0;
}

static int testPacketIndexWrap(void) {
    // Both the 16-bit sequence number and the 24-bit packet index wrap inside
    // the skipped run's neighbours.
    const FRAME_LAYOUT layout = {3, 34, 2};
    uint32_t start = 0xFFFFF2;

    startStream(VIDEO_FORMAT_H265, start);
    start = receiveFrame(1000, start, &layout);
    start = receiveFrame(1001, start, &layout);
    CHECK(start < 0xFFFFF2);
    start = receiveFrame(1005, start, &layout);
    CHECK_SKIPS(3, 1, 1002, 1004);
    start = receiveFrame(1006, start, &layout);
    CHECK_SKIPS(3, 1, 1002, 1004);
    CHECK_COUNTED(6 - 3);
    return 0;
}

static int testBoundaryReorderAndLoss(void) {
    const FRAME_LAYOUT layout = {4, 25, 1};
    uint32_t start = 0;

    startStream(VIDEO_FORMAT_H265, start);
    start = receiveFrame(1, start, &layout);

    // Frame 2's last data shard is lost: its end is still known from the others
    receiveShard(2, start, &layout, 0, 0, 0);
    receiveShard(2, start, &layout, 0, 1, 0);
    receiveShard(2, start, &layout, 0, 2, 0);
    receiveShard(2, start, &layout, 0, 4, 0);
    start = U24(start + frameShards(&layout));

    // After skipped frames 3-6, frame 7's parity shard arrives first
    receiveShard(7, start, &layout, 0, 4, 0);
    CHECK_SKIPS(0, 0, 0, 0);
    receiveShard(7, start, &layout, 0, 2, 0);
    CHECK_SKIPS(4, 1, 3, 6);
    receiveShard(7, start, &layout, 0, 0, 0);
    receiveShard(7, start, &layout, 0, 1, 0);
    receiveShard(7, start, &layout, 0, 3, 0);
    start = U24(start + frameShards(&layout));

    // Without any shard of frame 8's final block, the gap after it is unproven
    receiveShard(8, start, &layout, 0, 4, 0);
    start = U24(start + frameShards(&layout));
    start = receiveFrame(10, start, &layout);
    CHECK_SKIPS(4, 1, 3, 6);
    return 0;
}

static int testFinalParityLost(void) {
    // The previous frame's end comes from its FEC header (data + parity
    // shards sent), not from the last shard that arrived. Losing that frame's
    // final parity shard is a sequence gap inside a completed frame.
    const FRAME_LAYOUT layout = {4, 50, 1};
    uint32_t start = 0;
    uint32_t shard;

    startStream(VIDEO_FORMAT_H265, start);
    start = receiveFrame(1, start, &layout);
    for (shard = 0; shard < layout.dataShards + parityShards(&layout) - 1; shard++) {
        receiveShard(2, start, &layout, 0, shard, 0);
    }
    start = U24(start + frameShards(&layout));

    // Frames 3 and 4 were sent and lost after it: the gap stays network loss
    start = loseFrames(2, start, &layout);
    start = receiveFrame(5, start, &layout);
    CHECK_SKIPS(0, 0, 0, 0);

    for (shard = 0; shard < layout.dataShards + parityShards(&layout) - 1; shard++) {
        receiveShard(6, start, &layout, 0, shard, 0);
    }
    start = U24(start + frameShards(&layout));
    // Frames 7 and 8 were never sent: no index between 6's last parity and 9
    start = receiveFrame(9, start, &layout);
    CHECK_SKIPS(2, 1, 7, 8);
    CHECK_COUNTED(8 - 2);
    return 0;
}

static int testMultiBlockFrames(void) {
    const FRAME_LAYOUT layout = {5, 20, 3};
    uint32_t start = 0;

    startStream(VIDEO_FORMAT_H265, start);
    start = receiveFrame(1, start, &layout);
    start = receiveFrame(2, start, &layout);
    start = receiveFrame(4, start, &layout);
    CHECK_SKIPS(1, 1, 3, 3);

    // Only block 0 of frame 5 arrives: its final block's end is unknown
    receiveShard(5, start, &layout, 0, 0, 0);
    start = U24(start + frameShards(&layout));
    start = receiveFrame(7, start, &layout);
    CHECK_SKIPS(1, 1, 3, 3);
    CHECK(queue.currentFrameNumber == 8);
    return 0;
}

static int testPyrowaveReorderedBlocks(void) {
    // PyroWave has no outer parity and accepts a frame's blocks in any order
    const FRAME_LAYOUT layout = {6, 0, 2};
    uint32_t start = 0;

    startStream(VIDEO_FORMAT_PYROWAVE, start);
    start = receiveFrame(1, start, &layout);
    start = receiveFrame(2, start, &layout);

    // Frame 9's final block arrives before its first block
    for (uint32_t shard = 0; shard < layout.dataShards; shard++) {
        receiveShard(9, start, &layout, 1, shard, 0);
    }
    CHECK_SKIPS(0, 0, 0, 0);
    for (uint32_t shard = 0; shard < layout.dataShards; shard++) {
        receiveShard(9, start, &layout, 0, shard, 0);
    }
    CHECK_SKIPS(6, 1, 3, 8);
    start = U24(start + frameShards(&layout));

    start = receiveFrame(12, start, &layout);
    CHECK_SKIPS(8, 2, 10, 11);
    // Block 1 of frame 9 made connectionSawFrame() count 3-8 before block 0
    // proved them skipped; frame 12's gap is excluded.
    CHECK_COUNTED(1 + 7 + 1);
    return 0;
}

static int testOtherNumberingNotClassified(void) {
    const FRAME_LAYOUT layout = {4, 20, 1};
    uint32_t start = 0;

    startStream(VIDEO_FORMAT_H265, start);
    for (uint32_t shard = 0; shard < 5; shard++) {
        receiveShard(1, start, &layout, 0, shard, 1);
    }
    start = U24(start + frameShards(&layout));
    for (uint32_t shard = 0; shard < 5; shard++) {
        receiveShard(5, start, &layout, 0, shard, 1);
    }
    CHECK_SKIPS(0, 0, 0, 0);
    CHECK_COUNTED(4);
    return 0;
}

static int testStreamRestartResets(void) {
    const FRAME_LAYOUT layout = {2, 0, 1};
    uint32_t start = 0;

    startStream(VIDEO_FORMAT_H264, start);
    start = receiveFrame(1, start, &layout);
    start = receiveFrame(3, start, &layout);
    CHECK_SKIPS(1, 1, 2, 2);

    startStream(VIDEO_FORMAT_H264, 0);
    CHECK_SKIPS(0, 0, 0, 0);
    // The first frame of a new stream has nothing before it to compare with
    start = receiveFrame(5, 0, &layout);
    CHECK_SKIPS(0, 0, 0, 0);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += testRecordedSkipBurst();
    failures += testSameBurstLostOnNetwork();
    failures += testMixedSkipsAndLoss();
    failures += testPacketIndexWrap();
    failures += testBoundaryReorderAndLoss();
    failures += testFinalParityLost();
    failures += testMultiBlockFrames();
    failures += testPyrowaveReorderedBlocks();
    failures += testOtherNumberingNotClassified();
    failures += testStreamRestartResets();
    RtpvCleanupQueue(&queue);

    if (failures != 0) {
        fprintf(stderr, "%d host-skip test(s) failed\n", failures);
        return 1;
    }
    printf("host-skip tests passed\n");
    return 0;
}
