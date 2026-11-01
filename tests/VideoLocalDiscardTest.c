// Frames the client receives completely and then discards itself (decode unit
// queue overflow, the DR_NEED_IDR queue flush and the IDR wait that follows)
// must be reported apart from network loss. The decoders otherwise see only a
// frame-number gap and count these frames as network drops.
#define notifyKeyFrameReceived testNotifyKeyFrameReceived
#define connectionReceivedCompleteFrame testConnectionReceivedCompleteFrame
#define connectionDetectedFrameLoss testConnectionDetectedFrameLoss
#define LiGetCurrentHostDisplayHdrMode testGetCurrentHostDisplayHdrMode
#define LiRequestIdrFrame testRequestIdrFrame
#include "../src/VideoDepacketizer.c"

#include <stdio.h>
#include <string.h>

static int idrRequests;

void testNotifyKeyFrameReceived(void) {}
void testConnectionReceivedCompleteFrame(uint32_t frameNumber, bool frameIsLTR) {
    (void)frameNumber;
    (void)frameIsLTR;
}
void testConnectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) {
    (void)startFrame;
    (void)endFrame;
}
bool testGetCurrentHostDisplayHdrMode(void) { return false; }
void testRequestIdrFrame(void) { idrRequests++; }

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

// Checks the discard count, the number of ranges and the newest range.
#define CHECK_DISCARDS(count, rangeCount, first, last, rangeFrames) do { \
    VIDEO_LOCAL_DISCARD_STATS stats_; \
    VIDEO_LOCAL_DISCARD_RANGE newest_ = {0, 0, 0}; \
    LiGetVideoLocalDiscardStats(&stats_); \
    if (stats_.rangesRecorded > 0) { \
        newest_ = stats_.ranges[(stats_.rangesRecorded - 1) % VIDEO_LOCAL_DISCARD_RANGE_COUNT]; \
    } \
    if (stats_.framesDiscarded != (count) || stats_.rangesRecorded != (rangeCount) || \
            newest_.firstFrameNumber != (first) || newest_.lastFrameNumber != (last) || \
            newest_.frames != (rangeFrames)) { \
        fprintf(stderr, "FAIL line %d: local discards %llu in %llu ranges, newest [%u..%u] %u, " \
                "expected %u in %u ranges, newest [%u..%u] %u\n", __LINE__, \
                (unsigned long long)stats_.framesDiscarded, \
                (unsigned long long)stats_.rangesRecorded, newest_.firstFrameNumber, \
                newest_.lastFrameNumber, newest_.frames, (unsigned)(count), \
                (unsigned)(rangeCount), (unsigned)(first), (unsigned)(last), (unsigned)(rangeFrames)); \
        return 1; \
    } \
} while (0)
static bool queuePyrowaveFrame(unsigned frame) {
    static const uint8_t codec[] = {1, 2, 3, 4};
    uint8_t* allocation = malloc(sizeof(PYROWAVE_FRAME_ENTRY) + sizeof(codec));
    if (allocation == NULL) {
        return false;
    }
    memcpy(allocation + sizeof(PYROWAVE_FRAME_ENTRY), codec, sizeof(codec));
    if (!queueOwnedFrame(allocation, sizeof(codec), 0)) {
        return false;
    }
    frameType = FRAME_TYPE_IDR;
    nextFrameNumber = frame + 1;
    reassembleFrame(frame, false);
    return true;
}

// Feed one single-packet AV1 frame through the real RTP payload path.
static void receiveAv1Frame(uint32_t frame, bool idr) {
    static uint32_t streamPacketIndex;
    uint8_t packet[sizeof(NV_VIDEO_PACKET) + 8 + 4];
    PNV_VIDEO_PACKET header = (PNV_VIDEO_PACKET)packet;
    uint8_t* payload = packet + sizeof(*header);

    memset(packet, 0, sizeof(packet));
    header->streamPacketIndex = (streamPacketIndex++ & 0xFFFFFF) << 8;
    header->frameIndex = frame;
    header->flags = FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA;
    payload[0] = 0x01;              // 8 byte frame header
    payload[3] = idr ? 2 : 1;       // IDR or P frame
    payload[4] = 8 + 4;             // last packet payload length, header included
    payload[8] = (uint8_t)frame;
    processRtpPayload(header, (int)sizeof(packet), 1000 * frame, 0, 0, NULL);
}

static int pollFrame(int expectedFrame, int drStatus) {
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT decodeUnit;
    if (!LiPollNextVideoFrame(&handle, &decodeUnit)) {
        fprintf(stderr, "FAIL: no frame queued, expected %d\n", expectedFrame);
        return 1;
    }
    if (decodeUnit->frameNumber != expectedFrame) {
        fprintf(stderr, "FAIL: dequeued frame %d, expected %d\n",
                decodeUnit->frameNumber, expectedFrame);
        LiCompleteVideoFrame(handle, DR_OK);
        return 1;
    }
    LiCompleteVideoFrame(handle, drStatus);
    return 0;
}

static int testPyrowave(void) {
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    StreamConfig.fps = 120;
    VideoCallbacks.capabilities = CAPABILITY_PULL_RENDERER;
    initializeVideoDepacketizer(1392);
    CHECK_DISCARDS(0, 0, 0, 0, 0);

    // The queue holds 15 frames; the 16th overflows and flushes all of them.
    for (unsigned frame = 1; frame <= 16; ++frame) {
        CHECK(queuePyrowaveFrame(frame));
    }
    CHECK_DISCARDS(16, 1, 1, 16, 16);

    // A decoder refresh flushes the frames queued behind the failing frame.
    CHECK(queuePyrowaveFrame(17));
    CHECK(queuePyrowaveFrame(18));
    CHECK(queuePyrowaveFrame(19));
    CHECK(pollFrame(17, DR_NEED_IDR) == 0);
    CHECK_DISCARDS(18, 2, 18, 19, 2);

    // PyroWave frames are all intra frames: the next frame decodes at once.
    CHECK(queuePyrowaveFrame(20));
    CHECK(pollFrame(20, DR_OK) == 0);
    CHECK_DISCARDS(18, 2, 18, 19, 2);

    // The network lost frame 22 from a batch that overflows: the range spans
    // it, but only the frames that were discarded are counted.
    CHECK(queuePyrowaveFrame(21));
    for (unsigned frame = 23; frame <= 37; ++frame) {
        CHECK(queuePyrowaveFrame(frame));
    }
    CHECK_DISCARDS(34, 3, 21, 37, 16);

    destroyVideoDepacketizer();
    return 0;
}

static int testAv1(void) {
    NegotiatedVideoFormat = VIDEO_FORMAT_AV1_MAIN8;
    StreamConfig.fps = 60;
    VideoCallbacks.capabilities = CAPABILITY_PULL_RENDERER;
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    initializeVideoDepacketizer(1392);

    receiveAv1Frame(1, true);
    CHECK(pollFrame(1, DR_OK) == 0);

    // Queue overflow: frames 2..16 fill the queue and frame 17 overflows.
    for (uint32_t frame = 2; frame <= 17; ++frame) {
        receiveAv1Frame(frame, false);
    }
    CHECK_DISCARDS(16, 1, 2, 17, 16);

    // The complete P frames received while waiting for the requested IDR
    // frame are discarded because of that local flush. Each one directly
    // follows the flushed batch, so it extends that range.
    receiveAv1Frame(18, false);
    receiveAv1Frame(19, false);
    CHECK_DISCARDS(18, 1, 2, 19, 18);
    receiveAv1Frame(20, true);
    CHECK(pollFrame(20, DR_OK) == 0);

    // A network gap makes the client wait for an IDR frame too. Those frames
    // stay network loss and are not reported as local discards.
    receiveAv1Frame(22, false);
    receiveAv1Frame(23, false);
    CHECK_DISCARDS(18, 1, 2, 19, 18);
    receiveAv1Frame(24, true);
    CHECK(pollFrame(24, DR_OK) == 0);

    // A decoder refresh flushes the queue and drops frames until the IDR.
    receiveAv1Frame(25, false);
    receiveAv1Frame(26, false);
    receiveAv1Frame(27, false);
    CHECK(pollFrame(25, DR_NEED_IDR) == 0);
    CHECK_DISCARDS(20, 2, 26, 27, 2);
    receiveAv1Frame(28, false);
    CHECK_DISCARDS(21, 2, 26, 28, 3);
    // Network loss during that wait takes over: later waiting drops are loss.
    receiveAv1Frame(30, false);
    receiveAv1Frame(31, false);
    CHECK_DISCARDS(21, 2, 26, 28, 3);
    receiveAv1Frame(32, true);
    CHECK(pollFrame(32, DR_OK) == 0);
    receiveAv1Frame(33, false);
    CHECK(pollFrame(33, DR_OK) == 0);
    CHECK_DISCARDS(21, 2, 26, 28, 3);
    CHECK(idrRequests > 0);

    destroyVideoDepacketizer();
    return 0;
}

int main(void) {
    if (testPyrowave() != 0 || testAv1() != 0) {
        return 1;
    }
    puts("PASS: local queue flushes are reported apart from network loss");
    return 0;
}
