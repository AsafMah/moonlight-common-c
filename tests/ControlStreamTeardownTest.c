// Races an external sender against stopControlStream(). The ENet linger is
// replaced by a shim so the test can observe the sender while teardown runs.
#define gracefullyDisconnectEnetPeer lingerShim
#include "../src/ControlStream.c"
#undef gracefullyDisconnectEnetPeer

#include <stdatomic.h>
#include <stdio.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static const char testPayload[4];
static PLT_THREAD senderThread;
static PLT_EVENT senderStarted;
static atomic_bool senderFinished;
static bool senderResult;
static int lingerCalls;

static bool sendTestMessage(void) {
    return sendMessageEnet(0x0200, sizeof(testPayload), testPayload,
                           CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false);
}

static void racingSender(void* context) {
    PltSetEvent(&senderStarted);
    senderResult = sendTestMessage();
    atomic_store(&senderFinished, true);
}

static void exitImmediately(void* context) {
}

int lingerShim(ENetHost* lingerHost, ENetPeer* lingerPeer, enet_uint32 lingerTimeoutMs) {
    lingerCalls++;

    // A LiSend*() caller that passed its unlocked fast path just as teardown began.
    CHECK(PltCreateThread("Racing sender", racingSender, NULL, &senderThread) == 0);
    PltWaitForEvent(&senderStarted);

    // It must not touch ENet while the linger owns it.
    for (int i = 0; i < 50 && !atomic_load(&senderFinished); i++) {
        PltSleepMs(5);
    }
    CHECK(!atomic_load(&senderFinished));
    return 0;
}

int main(void) {
    ENetPeer fakePeer;

    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 415;
    AppVersionQuad[3] = -1;
    CHECK(initializeControlStream() == 0);

    // Teardown completed between a caller's unlocked check and sendMessageEnet().
    CHECK(peer == NULL && client == NULL);
    CHECK(!sendTestMessage());

    // stopControlStream() joins these threads before tearing down ENet.
    CHECK(PltCreateThread("Loss stats", exitImmediately, NULL, &lossStatsThread) == 0);
    CHECK(PltCreateThread("IDR request", exitImmediately, NULL, &requestIdrFrameThread) == 0);
    CHECK(PltCreateThread("Control receive", exitImmediately, NULL, &controlReceiveThread) == 0);
    CHECK(PltCreateThread("Async callbacks", exitImmediately, NULL, &asyncCallbackThread) == 0);
    CHECK(PltCreateEvent(&senderStarted) == 0);

    // The shim never dereferences the peer, and enet_peer_send() rejects it.
    memset(&fakePeer, 0, sizeof(fakePeer));
    peer = &fakePeer;
    ConnectionInterrupted = true;

    CHECK(stopControlStream() == 0);
    CHECK(lingerCalls == 1);
    CHECK(peer == NULL);

    // Once teardown releases ENet, the racing sender fails cleanly.
    PltJoinThread(&senderThread);
    CHECK(atomic_load(&senderFinished));
    CHECK(!senderResult);

    PltCloseEvent(&senderStarted);
    destroyControlStream();

    if (failures != 0) {
        fprintf(stderr, "%d control stream teardown checks failed\n", failures);
        return 1;
    }

    puts("Control stream teardown tests passed");
    return 0;
}
