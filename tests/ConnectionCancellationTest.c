#include "../src/Limelight-internal.h"
#include "../src/PlatformSockets.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#if defined(_WIN32) && !defined(NXDK)
_Static_assert(ECONNRESET == WSAECONNRESET, "Connection reset errors must use the Winsock namespace");
#endif

static SOCKET listenerSocket = INVALID_SOCKET;
static SOCKET acceptedSocket = INVALID_SOCKET;
static uint16_t rtspPort;
static atomic_bool requestReceived;
static atomic_bool connectionFinished;
static atomic_bool releaseServer;
static atomic_uint fakePollCallCount;
static atomic_uint fakePollMaximumWaitMs;
static int connectionResult;
static SERVER_INFORMATION serverInfo;
static STREAM_CONFIGURATION streamConfig;
static PLT_THREAD serverThread;
static PLT_THREAD connectionThread;
static SOCKET stunServerSocket = INVALID_SOCKET;
static unsigned short stunServerPort;
static atomic_bool stunRequestReceived;
static atomic_bool releaseStunServer;
static PLT_THREAD stunServerThread;
static PLT_THREAD stunConnectionThread;
static atomic_bool stunConnectionFinished;
static int stunConnectionResult;

static int failures;
static int checks;

#define CHECK(condition) \
    do { \
        checks++; \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void closeTestSocket(SOCKET* socket) {
    if (*socket != INVALID_SOCKET) {
        closeSocket(*socket);
        *socket = INVALID_SOCKET;
    }
}

static void serverThreadFunc(void* context) {
    struct sockaddr_in peerAddress;
    SOCKADDR_LEN peerAddressLength;
    char buffer[2048];
    struct pollfd pollFd;

    (void)context;

    pollFd.fd = listenerSocket;
    pollFd.events = POLLIN;
    while (!atomic_load_explicit(&releaseServer, memory_order_acquire)) {
        int ready = pollSockets(&pollFd, 1, 50);
        if (ready <= 0) {
            continue;
        }

        peerAddressLength = sizeof(peerAddress);
        acceptedSocket = accept(listenerSocket, (struct sockaddr*)&peerAddress, &peerAddressLength);
        if (acceptedSocket == INVALID_SOCKET) {
            return;
        }

        int received = (int)recv(acceptedSocket, buffer, sizeof(buffer), 0);
        if (received > 0) {
            static const char partialResponse[] = "RTSP/1.0 200 OK\r\n";
            send(acceptedSocket, partialResponse, sizeof(partialResponse) - 1, 0);
            PltSleepMs(100);
            atomic_store_explicit(&requestReceived, true, memory_order_release);
            while (!atomic_load_explicit(&releaseServer, memory_order_acquire)) {
                PltSleepMs(5);
            }
            closeTestSocket(&acceptedSocket);
            return;
        }

        closeTestSocket(&acceptedSocket);
    }
}

static void connectionThreadFunc(void* context) {
    (void)context;
    connectionResult = LiStartConnection(&serverInfo, &streamConfig, NULL, NULL, NULL, NULL, 0, NULL, 0);
    atomic_store_explicit(&connectionFinished, true, memory_order_release);
}

static void stunServerThreadFunc(void* context) {
    struct pollfd pollFd;
    char buffer[128];

    (void)context;
    pollFd.fd = stunServerSocket;
    pollFd.events = POLLIN;
    while (!atomic_load_explicit(&releaseStunServer, memory_order_acquire)) {
        if (pollSockets(&pollFd, 1, 50) > 0) {
            if (recvfrom(stunServerSocket, buffer, sizeof(buffer), 0, NULL, NULL) > 0) {
                atomic_store_explicit(&stunRequestReceived, true, memory_order_release);
            }
            while (!atomic_load_explicit(&releaseStunServer, memory_order_acquire)) {
                PltSleepMs(5);
            }
            return;
        }
    }
}

static void stunConnectionThreadFunc(void* context) {
    unsigned int wanAddress;
    (void)context;
    stunConnectionResult = LiFindExternalAddressIP4("127.0.0.1", stunServerPort, &wanAddress);
    atomic_store_explicit(&stunConnectionFinished, true, memory_order_release);
}

static int fakePendingConnect(SOCKET socket, const struct sockaddr* address, SOCKADDR_LEN addressLength) {
    (void)socket;
    (void)address;
    (void)addressLength;
    SetLastSocketError(EINPROGRESS);
    return SOCKET_ERROR;
}

static int fakePollTimeout(struct pollfd* pollFds, int pollFdsCount, int timeoutMs) {
    unsigned int previousMaximum;
    (void)pollFds;
    (void)pollFdsCount;

    atomic_fetch_add_explicit(&fakePollCallCount, 1, memory_order_relaxed);
    previousMaximum = atomic_load_explicit(&fakePollMaximumWaitMs, memory_order_relaxed);
    while (previousMaximum < (unsigned int)timeoutMs &&
            !atomic_compare_exchange_weak_explicit(&fakePollMaximumWaitMs, &previousMaximum,
                                                   (unsigned int)timeoutMs,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)) {
    }

    PltSleepMs(timeoutMs);
    return 0;
}

typedef struct _CONNECT_TEST_CONTEXT {
    struct sockaddr_storage address;
    int result;
    atomic_bool finished;
} CONNECT_TEST_CONTEXT;

static void connectTestThreadFunc(void* context) {
    CONNECT_TEST_CONTEXT* test = (CONNECT_TEST_CONTEXT*)context;
    test->result = connectTcpSocket(&test->address, sizeof(struct sockaddr_in), 47984, 10) != INVALID_SOCKET;
    atomic_store_explicit(&test->finished, true, memory_order_release);
}

static atomic_bool resolverEntered;
static atomic_bool resolverRelease;
static atomic_bool resolverFinished;
static atomic_uint resolverCallCount;
static char resolverObservedHost[128];

static int delayedResolver(const char* node, const char* service,
                           const struct addrinfo* hints, struct addrinfo** result) {
    (void)service;
    atomic_fetch_add_explicit(&resolverCallCount, 1, memory_order_relaxed);
    atomic_store_explicit(&resolverEntered, true, memory_order_release);
    while (!atomic_load_explicit(&resolverRelease, memory_order_acquire)) {
        PltSleepMs(5);
    }

    snprintf(resolverObservedHost, sizeof(resolverObservedHost), "%s", node);
    int err = getaddrinfo("127.0.0.1", NULL, hints, result);
    atomic_store_explicit(&resolverFinished, true, memory_order_release);
    return err;
}

typedef struct _RESOLVE_TEST_CONTEXT {
    char* host;
    struct sockaddr_storage address;
    SOCKADDR_LEN addressLength;
    int result;
    atomic_bool finished;
} RESOLVE_TEST_CONTEXT;

typedef struct _NAT64_TEST_CONTEXT {
    struct sockaddr_storage address;
    bool result;
    atomic_bool finished;
} NAT64_TEST_CONTEXT;

static void resolveTestThreadFunc(void* context) {
    RESOLVE_TEST_CONTEXT* test = (RESOLVE_TEST_CONTEXT*)context;
    test->result = resolveHostName(test->host, AF_INET, 0, &test->address, &test->addressLength);
    atomic_store_explicit(&test->finished, true, memory_order_release);
}

static void nat64TestThreadFunc(void* context) {
    NAT64_TEST_CONTEXT* test = (NAT64_TEST_CONTEXT*)context;
    test->result = isNat64SynthesizedAddress(&test->address);
    atomic_store_explicit(&test->finished, true, memory_order_release);
}

static bool startTestServer(void) {
    struct sockaddr_in address;
    SOCKADDR_LEN addressLength;
    int reuseAddress = 1;

    listenerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenerSocket == INVALID_SOCKET) {
        return false;
    }

    setsockopt(listenerSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddress, sizeof(reuseAddress));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listenerSocket, (struct sockaddr*)&address, sizeof(address)) != 0 ||
            listen(listenerSocket, 2) != 0) {
        closeTestSocket(&listenerSocket);
        return false;
    }

    addressLength = sizeof(address);
    if (getsockname(listenerSocket, (struct sockaddr*)&address, &addressLength) != 0) {
        closeTestSocket(&listenerSocket);
        return false;
    }

    rtspPort = ntohs(address.sin_port);
    return true;
}

static bool startStunServer(void) {
    struct sockaddr_in address;
    SOCKADDR_LEN addressLength = sizeof(address);

    stunServerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (stunServerSocket == INVALID_SOCKET) {
        return false;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(stunServerSocket, (struct sockaddr*)&address, sizeof(address)) != 0 ||
            getsockname(stunServerSocket, (struct sockaddr*)&address, &addressLength) != 0) {
        closeTestSocket(&stunServerSocket);
        return false;
    }

    stunServerPort = ntohs(address.sin_port);
    return true;
}

static void testStunReceiveInterrupt(void) {
    uint64_t started;

    CHECK(initializePlatformSockets() == 0);
    CHECK(startStunServer());
    if (stunServerSocket == INVALID_SOCKET) {
        cleanupPlatformSockets();
        return;
    }

    atomic_store(&stunRequestReceived, false);
    atomic_store(&releaseStunServer, false);
    atomic_store(&stunConnectionFinished, false);
    LiPrepareConnectionAttempt(5000);
    if (PltCreateThread("StunCancellationServer", stunServerThreadFunc, NULL, &stunServerThread) != 0) {
        CHECK(false);
        closeTestSocket(&stunServerSocket);
        cleanupPlatformSockets();
        return;
    }
    if (PltCreateThread("StunCancellationClient", stunConnectionThreadFunc, NULL, &stunConnectionThread) != 0) {
        CHECK(false);
        atomic_store_explicit(&releaseStunServer, true, memory_order_release);
        PltJoinThread(&stunServerThread);
        closeTestSocket(&stunServerSocket);
        cleanupPlatformSockets();
        return;
    }

    started = PltGetMillis();
    while (!atomic_load_explicit(&stunRequestReceived, memory_order_acquire) &&
            !atomic_load_explicit(&stunConnectionFinished, memory_order_acquire) &&
            PltGetMillis() - started < 2000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&stunRequestReceived, memory_order_acquire));

    if (atomic_load_explicit(&stunRequestReceived, memory_order_acquire)) {
        LiInterruptConnection();
        started = PltGetMillis();
        while (!atomic_load_explicit(&stunConnectionFinished, memory_order_acquire) &&
                PltGetMillis() - started < 500) {
            PltSleepMs(5);
        }
        CHECK(atomic_load_explicit(&stunConnectionFinished, memory_order_acquire));
        CHECK(PltGetMillis() - started < 500);
        CHECK(stunConnectionResult == EINTR);
    }

    atomic_store_explicit(&releaseStunServer, true, memory_order_release);
    if (!atomic_load_explicit(&stunConnectionFinished, memory_order_acquire)) {
        started = PltGetMillis();
        while (!atomic_load_explicit(&stunConnectionFinished, memory_order_acquire) &&
                PltGetMillis() - started < 5000) {
            PltSleepMs(5);
        }
    }
    CHECK(atomic_load_explicit(&stunConnectionFinished, memory_order_acquire));
    PltJoinThread(&stunConnectionThread);
    PltJoinThread(&stunServerThread);
    closeTestSocket(&stunServerSocket);
    cleanupPlatformSockets();
}

static void testRtspReceiveInterrupt(void) {
    char sessionUrl[128];
    uint64_t interruptStarted;
    uint64_t waitStarted;

    CHECK(initializePlatformSockets() == 0);
    CHECK(startTestServer());
    if (listenerSocket == INVALID_SOCKET) {
        cleanupPlatformSockets();
        return;
    }

    atomic_store(&requestReceived, false);
    atomic_store(&connectionFinished, false);
    atomic_store(&releaseServer, false);

    LiInitializeServerInformation(&serverInfo);
    serverInfo.address = "127.0.0.1";
    serverInfo.serverInfoAppVersion = "7.1.431";
    serverInfo.serverCodecModeSupport = SCM_H264;
    snprintf(sessionUrl, sizeof(sessionUrl), "rtsp://127.0.0.1:%u/stream", rtspPort);
    serverInfo.rtspSessionUrl = sessionUrl;

    LiInitializeStreamConfiguration(&streamConfig);
    streamConfig.width = 1280;
    streamConfig.height = 720;
    streamConfig.fps = 60;
    streamConfig.bitrate = 10000;
    streamConfig.packetSize = 1024;
    streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    streamConfig.audioConfiguration = MAKE_AUDIO_CONFIGURATION(2, 0x3);
    streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;

    LiPrepareConnectionAttempt(5000);
    if (PltCreateThread("CancellationServer", serverThreadFunc, NULL, &serverThread) != 0) {
        CHECK(false);
        closeTestSocket(&listenerSocket);
        cleanupPlatformSockets();
        return;
    }
    if (PltCreateThread("CancellationConnect", connectionThreadFunc, NULL, &connectionThread) != 0) {
        CHECK(false);
        atomic_store_explicit(&releaseServer, true, memory_order_release);
        PltJoinThread(&serverThread);
        closeTestSocket(&listenerSocket);
        cleanupPlatformSockets();
        return;
    }

    waitStarted = PltGetMillis();
    while (!atomic_load_explicit(&requestReceived, memory_order_acquire) &&
            !atomic_load_explicit(&connectionFinished, memory_order_acquire) &&
            PltGetMillis() - waitStarted < 5000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&requestReceived, memory_order_acquire));

    if (atomic_load_explicit(&requestReceived, memory_order_acquire)) {
        interruptStarted = PltGetMillis();
        LiInterruptConnection();
        waitStarted = PltGetMillis();
        while (!atomic_load_explicit(&connectionFinished, memory_order_acquire) &&
                PltGetMillis() - waitStarted < 500) {
            PltSleepMs(5);
        }
        CHECK(atomic_load_explicit(&connectionFinished, memory_order_acquire));
        CHECK(PltGetMillis() - interruptStarted < 500);
    }

    atomic_store_explicit(&releaseServer, true, memory_order_release);
    if (!atomic_load_explicit(&connectionFinished, memory_order_acquire)) {
        waitStarted = PltGetMillis();
        while (!atomic_load_explicit(&connectionFinished, memory_order_acquire) &&
                PltGetMillis() - waitStarted < 20000) {
            PltSleepMs(10);
        }
    }

    CHECK(atomic_load_explicit(&connectionFinished, memory_order_acquire));
    CHECK(connectionResult != 0);
    PltJoinThread(&connectionThread);
    PltJoinThread(&serverThread);
    closeTestSocket(&acceptedSocket);
    closeTestSocket(&listenerSocket);
    cleanupPlatformSockets();
}

static void testPreparedCancellationAndDeadline(void) {
    SERVER_INFORMATION invalidServer = { 0 };
    STREAM_CONFIGURATION config;
    uint64_t started;
    int result;

    LiInitializeStreamConfiguration(&config);

    LiPrepareConnectionAttempt(UINT32_MAX);
    LiInterruptConnection();
    started = PltGetMillis();
    result = LiStartConnection(&invalidServer, &config, NULL, NULL, NULL, NULL, 0, NULL, 0);
    CHECK(result == EINTR);
    CHECK(atomic_load_explicit(&ConnectionInterrupted, memory_order_acquire));
    CHECK(PltGetMillis() - started < 250);

    LiPrepareConnectionAttempt(0);
    started = PltGetMillis();
    result = LiStartConnection(&invalidServer, &config, NULL, NULL, NULL, NULL, 0, NULL, 0);
    CHECK(result == ETIMEDOUT);
    CHECK(PltGetMillis() - started < 250);

    LiPrepareConnectionAttempt(UINT32_MAX);
    result = LiStartConnection(&invalidServer, &config, NULL, NULL, NULL, NULL, 0, NULL, 0);
    CHECK(result != 0 && result != EINTR);

    LiInterruptConnection();
    result = LiStartConnection(&invalidServer, &config, NULL, NULL, NULL, NULL, 0, NULL, 0);
    CHECK(result != EINTR);
}

static void testConnectWaitInterruptAndDeadlineClamp(void) {
    CONNECT_TEST_CONTEXT connectTest = { 0 };
    struct sockaddr_in* address = (struct sockaddr_in*)&connectTest.address;
    PLT_THREAD thread;
    uint64_t started;

    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    atomic_store(&connectTest.finished, false);
    atomic_store(&fakePollCallCount, 0);
    atomic_store(&fakePollMaximumWaitMs, 0);
    liSetConnectionSocketFunctionsForTests(fakePendingConnect, fakePollTimeout);

    CHECK(initializePlatformSockets() == 0);
    LiPrepareConnectionAttempt(5000);
    CHECK(PltCreateThread("ConnectCancellation", connectTestThreadFunc, &connectTest, &thread) == 0);
    started = PltGetMillis();
    while (atomic_load_explicit(&fakePollCallCount, memory_order_acquire) == 0 &&
            PltGetMillis() - started < 1000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&fakePollCallCount, memory_order_acquire) > 0);
    LiInterruptConnection();
    started = PltGetMillis();
    while (!atomic_load_explicit(&connectTest.finished, memory_order_acquire) &&
            PltGetMillis() - started < 500) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&connectTest.finished, memory_order_acquire));
    CHECK(PltGetMillis() - started < 500);
    CHECK(connectTest.result == 0);
    CHECK(atomic_load_explicit(&fakePollMaximumWaitMs, memory_order_relaxed) <= 50);
    PltJoinThread(&thread);
    cleanupPlatformSockets();

    atomic_store(&fakePollCallCount, 0);
    atomic_store(&fakePollMaximumWaitMs, 0);
    LiPrepareConnectionAttempt(125);
    started = PltGetMillis();
    CHECK(pollSocketsInterruptibly(NULL, 0, 5000) == 0);
    CHECK(LastSocketError() == ETIMEDOUT);
    CHECK(PltGetMillis() - started < 350);
    CHECK(atomic_load_explicit(&fakePollMaximumWaitMs, memory_order_relaxed) <= 50);

    liSetConnectionSocketFunctionsForTests(NULL, NULL);
    LiPrepareConnectionAttempt(UINT32_MAX);
}

static void testResolverCancellationOwnershipAndRecovery(void) {
    RESOLVE_TEST_CONTEXT resolveTest = { 0 };
    NAT64_TEST_CONTEXT nat64Test = { 0 };
    PLT_THREAD thread;
    uint64_t started;
    struct sockaddr_storage address = { 0 };
    SOCKADDR_LEN addressLength = 0;

    resolveTest.host = (char*)malloc(32);
    CHECK(resolveTest.host != NULL);
    if (resolveTest.host == NULL) {
        return;
    }
    snprintf(resolveTest.host, 32, "%s", "resolver.test");
    resolveTest.address.ss_family = AF_UNSPEC;
    resolveTest.addressLength = 123;
    atomic_store(&resolveTest.finished, false);
    atomic_store(&resolverEntered, false);
    atomic_store(&resolverRelease, false);
    atomic_store(&resolverFinished, false);
    atomic_store(&resolverCallCount, 0);
    liSetGetAddrInfoFunctionForTests(delayedResolver);

    LiPrepareConnectionAttempt(5000);
    CHECK(PltCreateThread("ResolverCancellation", resolveTestThreadFunc, &resolveTest, &thread) == 0);
    started = PltGetMillis();
    while (!atomic_load_explicit(&resolverEntered, memory_order_acquire) &&
            PltGetMillis() - started < 1000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&resolverEntered, memory_order_acquire));

    LiInterruptConnection();
    started = PltGetMillis();
    while (!atomic_load_explicit(&resolveTest.finished, memory_order_acquire) &&
            PltGetMillis() - started < 500) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&resolveTest.finished, memory_order_acquire));
    CHECK(PltGetMillis() - started < 500);
    CHECK(resolveTest.result == EINTR);
    CHECK(resolveTest.address.ss_family == AF_UNSPEC);
    CHECK(resolveTest.addressLength == 123);
    PltJoinThread(&thread);

    free(resolveTest.host);
    resolveTest.host = NULL;

    LiPrepareConnectionAttempt(150);
    started = PltGetMillis();
    CHECK(resolveHostName("second.test", AF_INET, 0, &address, &addressLength) == ETIMEDOUT);
    CHECK(PltGetMillis() - started < 400);
    CHECK(atomic_load_explicit(&resolverCallCount, memory_order_relaxed) == 1);
    CHECK(addressLength == 0);

    atomic_store_explicit(&resolverRelease, true, memory_order_release);
    started = PltGetMillis();
    while (!atomic_load_explicit(&resolverFinished, memory_order_acquire) &&
            PltGetMillis() - started < 1000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&resolverFinished, memory_order_acquire));
    CHECK(strcmp(resolverObservedHost, "resolver.test") == 0);

    LiPrepareConnectionAttempt(UINT32_MAX);
    CHECK(resolveHostName("completed.test", AF_INET, 0, &address, &addressLength) == 0);
    CHECK(address.ss_family == AF_INET);
    CHECK(strcmp(resolverObservedHost, "completed.test") == 0);
    CHECK(atomic_load_explicit(&resolverCallCount, memory_order_relaxed) == 2);

    nat64Test.address.ss_family = AF_INET6;
    atomic_store(&resolverEntered, false);
    atomic_store(&resolverRelease, false);
    atomic_store(&resolverFinished, false);
    atomic_store(&nat64Test.finished, false);
    LiPrepareConnectionAttempt(5000);
    CHECK(PltCreateThread("Nat64ResolverCancellation", nat64TestThreadFunc, &nat64Test, &thread) == 0);
    started = PltGetMillis();
    while (!atomic_load_explicit(&resolverEntered, memory_order_acquire) &&
            PltGetMillis() - started < 1000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&resolverEntered, memory_order_acquire));
    LiInterruptConnection();
    started = PltGetMillis();
    while (!atomic_load_explicit(&nat64Test.finished, memory_order_acquire) &&
            PltGetMillis() - started < 500) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&nat64Test.finished, memory_order_acquire));
    CHECK(PltGetMillis() - started < 500);
    CHECK(!nat64Test.result);
    PltJoinThread(&thread);

    atomic_store_explicit(&resolverRelease, true, memory_order_release);
    started = PltGetMillis();
    while (!atomic_load_explicit(&resolverFinished, memory_order_acquire) &&
            PltGetMillis() - started < 1000) {
        PltSleepMs(5);
    }
    CHECK(atomic_load_explicit(&resolverFinished, memory_order_acquire));
    CHECK(strcmp(resolverObservedHost, "ipv4only.arpa.") == 0);

    LiInterruptConnection();
    CHECK(address.ss_family == AF_INET);
    liSetGetAddrInfoFunctionForTests(NULL);
}

int main(void) {
    testPreparedCancellationAndDeadline();
    testConnectWaitInterruptAndDeadlineClamp();
    testResolverCancellationOwnershipAndRecovery();
    testStunReceiveInterrupt();
    testRtspReceiveInterrupt();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
