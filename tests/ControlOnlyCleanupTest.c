// Include the actual lifecycle implementation to exercise its private stage.
// Startup-only dependencies abort if cleanup accidentally enters them.
#include "../src/Connection.c"
#include <stdio.h>

static int avCalls, controlCalls, inputCalls, platformCalls;
bool VideoFecControlSupported;
void LiClearStreamSockets(void) {}
void destroyMicrophoneStream(void) {}
int stopInputStream(void) { inputCalls++; return 0; }
void destroyInputStream(void) { inputCalls++; }
int stopControlStream(void) { controlCalls++; return 0; }
void destroyControlStream(void) { controlCalls++; }
void stopAudioStream(void) { avCalls++; }
void stopVideoStream(void) { avCalls++; }
void destroyAudioStream(void) { avCalls++; }
void destroyVideoStream(void) { avCalls++; }
void cleanupPlatform(void) { platformCalls++; }

int PltCreateThread(const char* n, ThreadEntry e, void* c, PLT_THREAD* t) { abort(); }
void PltDetachThread(PLT_THREAD* t) { abort(); }
void PltSleepMs(int ms) { abort(); }
int initializePlatform(void) { abort(); }
int extractVersionQuadFromString(const char* s, int* q) { abort(); }
void fixupMissingCallbacks(PDECODER_RENDERER_CALLBACKS* d, PAUDIO_RENDERER_CALLBACKS* a, PCONNECTION_LISTENER_CALLBACKS* c) { abort(); }
int resolveHostName(const char* h, int f, int p, struct sockaddr_storage* a, SOCKADDR_LEN* l) { abort(); }
int getLocalAddressByUdpConnect(const struct sockaddr_storage* r, SOCKADDR_LEN rl, unsigned short p, struct sockaddr_storage* l, SOCKADDR_LEN* ll) { abort(); }
bool isNat64SynthesizedAddress(struct sockaddr_storage* a) { abort(); }
bool isPrivateNetworkAddress(struct sockaddr_storage* a) { abort(); }
int initializeAudioStream(void) { abort(); }
int performRtspHandshake(PSERVER_INFORMATION i) { abort(); }
int initializeControlStream(void) { abort(); }
void initializeVideoStream(void) { abort(); }
int initializeInputStream(void) { abort(); }
int startControlStream(void) { abort(); }
int startVideoStream(void* c, int f) { abort(); }
int startAudioStream(void* c, int f) { abort(); }
int startInputStream(void) { abort(); }
int initializeMicrophoneStream(void) { abort(); }
int LiSendMouseMoveEvent(short x, short y) { abort(); }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    for (unsigned int controlOnly = 0; controlOnly < 2; controlOnly++) {
        avCalls = controlCalls = inputCalls = platformCalls = 0;
        StreamConfig.controlOnly = controlOnly;
        stage = STAGE_INPUT_STREAM_START;
        LiStopConnection();
        CHECK(stage == STAGE_NONE);
        CHECK(avCalls == (controlOnly ? 0 : 4));
        CHECK(controlCalls == 2 && inputCalls == 2 && platformCalls == 1);
        // A failed handshake must likewise avoid resources skipped at startup.
        avCalls = controlCalls = inputCalls = platformCalls = 0;
        stage = STAGE_RTSP_HANDSHAKE;
        LiStopConnection();
        CHECK(stage == STAGE_NONE && controlCalls == 0 && inputCalls == 0 && platformCalls == 1);
        CHECK(avCalls == (controlOnly ? 0 : 1));
    }
    puts("Control-only cleanup: skipped AV resources untouched on stop and failed handshake");
    return 0;
}
