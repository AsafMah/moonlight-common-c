// Exercise the production sender with an ACK that never arrives during shutdown.
#define isControlDataInTransit testControlDataInTransit
#define flushInputOnControlStream testFlushInputOnControlStream
#define sendInputPacketOnControlStream testSendInputPacketOnControlStream
#include "../src/InputStream.c"
#undef isControlDataInTransit
#undef flushInputOnControlStream
#undef sendInputPacketOnControlStream

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

static PLT_EVENT waitingForAck;
static int keyReleases;

bool testControlDataInTransit(void) {
    PltSetEvent(&waitingForAck);
    return true;
}

void testFlushInputOnControlStream(void) {}

int testSendInputPacketOnControlStream(unsigned char* data, int length,
                                      uint8_t channelId, uint32_t flags, bool moreData) {
    NV_INPUT_HEADER* header = (NV_INPUT_HEADER*)data;
    (void)length;
    (void)channelId;
    (void)flags;
    (void)moreData;
    CHECK(header->magic == LE32(KEY_UP_EVENT_MAGIC));
    keyReleases++;
    return 0;
}

int main(void) {
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 500;
    AppVersionQuad[3] = 0; // Exercise GFE's ACK wait (Sunshine may skip it).
    CHECK(PltCreateEvent(&waitingForAck) == 0);
    CHECK(initializeInputStream() == 0);
    initialized = true;

    CHECK(LiSendUtf8TextEvent("ab", 2) == 0);
    CHECK(LiSendKeyboardEvent(0x41, KEY_ACTION_UP, 0) == 0);
    CHECK(PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread) == 0);
    PltWaitForEvent(&waitingForAck);
    LiInterruptConnection();
    stopInputStream();

    CHECK(keyReleases == 1);
    CHECK(LbqGetItemCount(&packetQueue) == 0);

    destroyInputStream();
    PltCloseEvent(&waitingForAck);
    puts("Input stream shutdown regression passed");
    return 0;
}
