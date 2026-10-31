#include "../src/ControlStream.c"

#include <stdio.h>

bool parseSdpAttributeToUInt(const char* payload, const char* name, uint32_t* val);

static int failures;
static int checks;
static int terminationCallbackCount;
static int terminationCallbackError;
static ENetHost* server;
static ENetPeer* serverPeer;

#define CHECK(condition) \
    do { \
        checks++; \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void captureTermination(int errorCode) {
    terminationCallbackCount++;
    terminationCallbackError = errorCode;
}

static void destroyLoopback(void) {
    if (client != NULL) {
        enet_host_destroy(client);
        client = NULL;
        peer = NULL;
    }
    if (server != NULL) {
        enet_host_destroy(server);
        server = NULL;
        serverPeer = NULL;
    }
}

static void runControlReceiver(void) {
    if (PltCreateThread("HostRestartTest", controlReceiveThreadFunc, NULL, &controlReceiveThread) != 0) {
        CHECK(false);
        return;
    }
    PltJoinThread(&controlReceiveThread);
}

static bool connectLoopback(void) {
    struct sockaddr_in loopback = { 0 };
    ENetAddress bindAddress;
    ENetAddress serverAddress;
    ENetEvent event;

    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    enet_address_set_address(&bindAddress, (struct sockaddr*)&loopback, (socklen_t)sizeof(loopback));
    server = enet_host_create(AF_INET, &bindAddress, 1, CTRL_CHANNEL_COUNT, 0, 0);
    if (server == NULL || enet_socket_get_address(server->socket, &serverAddress) < 0) {
        return false;
    }

    client = enet_host_create(AF_INET, NULL, 1, CTRL_CHANNEL_COUNT, 0, 0);
    if (client == NULL) {
        return false;
    }

    peer = enet_host_connect(client, &serverAddress, CTRL_CHANNEL_COUNT, 0);
    if (peer == NULL) {
        return false;
    }

    for (int i = 0; i < 400 && serverPeer == NULL; i++) {
        enet_host_service(client, &event, 5);
        if (enet_host_service(server, &event, 5) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
            serverPeer = event.peer;
        }
    }

    return peer->state == ENET_PEER_STATE_CONNECTED && serverPeer != NULL;
}

static void testTerminationPacket(const uint8_t* payload, size_t payloadLength,
                                  uint32_t lastFrame, int expectedResult) {
    ENetPacket* packet;
    uint8_t terminationPacket[sizeof(NVCTL_ENET_PACKET_HEADER_V1) + 8];

    if (!connectLoopback()) {
        CHECK(false);
        destroyLoopback();
        return;
    }
    if (client == NULL || server == NULL || serverPeer == NULL) {
        return;
    }

    terminationPacket[0] = 0x09;
    terminationPacket[1] = 0x01;
    if (payloadLength > sizeof(terminationPacket) - 2) {
        CHECK(false);
        return;
    }
    memcpy(terminationPacket + 2, payload, payloadLength);
    packet = enet_packet_create(terminationPacket, 2 + payloadLength, ENET_PACKET_FLAG_RELIABLE);
    CHECK(packet != NULL);
    if (packet == NULL) {
        return;
    }
    if (enet_peer_send(serverPeer, CTRL_CHANNEL_GENERIC, packet) != 0) {
        CHECK(false);
        enet_packet_destroy(packet);
        destroyLoopback();
        return;
    }
    enet_host_flush(server);

    terminationCallbackCount = 0;
    terminationCallbackError = INT_MIN;
    lastSeenFrame = lastFrame;
    ConnectionInterrupted = false;
    runControlReceiver();

    CHECK(terminationCallbackCount == 1);
    if (terminationCallbackError != expectedResult) {
        fprintf(stderr, "termination result was %d (0x%08x), expected %d (0x%08x)\n",
                terminationCallbackError, (unsigned int)terminationCallbackError,
                expectedResult, (unsigned int)expectedResult);
    }
    CHECK(terminationCallbackError == expectedResult);

    destroyLoopback();
}

static void testEncryptedRestartTermination(void) {
    static const uint8_t restartReason[] = { 0x53, 0x53, 0x52, 0x01 };
    uint8_t plaintext[sizeof(NVCTL_ENET_PACKET_HEADER_V2) + sizeof(restartReason)];
    uint8_t iv[12] = { 0 };
    uint8_t encryptedPacket[sizeof(NVCTL_ENCRYPTED_PACKET_HEADER) + 4 + AES_GCM_TAG_LENGTH +
                            sizeof(plaintext)];
    PNVCTL_ENCRYPTED_PACKET_HEADER encryptedHeader =
        (PNVCTL_ENCRYPTED_PACKET_HEADER)encryptedPacket;
    PNVCTL_ENET_PACKET_HEADER_V2 plaintextHeader =
        (PNVCTL_ENET_PACKET_HEADER_V2)plaintext;
    int encryptedLength = sizeof(plaintext);
    ENetPacket* packet;

    if (!connectLoopback()) {
        CHECK(false);
        destroyLoopback();
        return;
    }
    if (client == NULL || server == NULL || serverPeer == NULL) {
        return;
    }

    memset(StreamConfig.remoteInputAesKey, 0x42, sizeof(StreamConfig.remoteInputAesKey));
    plaintextHeader->type = LE16(0x0109);
    plaintextHeader->payloadLength = LE16(sizeof(restartReason));
    memcpy(plaintext + sizeof(*plaintextHeader), restartReason, sizeof(restartReason));

    encryptedHeader->encryptedHeaderType = LE16(0x0001);
    encryptedHeader->length = LE16(sizeof(encryptedHeader->seq) + AES_GCM_TAG_LENGTH +
                                    sizeof(plaintext));
    encryptedHeader->seq = LE32(1);
    iv[0] = 1;
    iv[10] = 'H';
    iv[11] = 'C';
    if (!PltEncryptMessage(encryptionCtx, ALGORITHM_AES_GCM, 0,
                           StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                           iv, sizeof(iv),
                           (uint8_t*)(encryptedHeader + 1), AES_GCM_TAG_LENGTH,
                           plaintext, sizeof(plaintext),
                           ((uint8_t*)(encryptedHeader + 1)) + AES_GCM_TAG_LENGTH,
                           &encryptedLength)) {
        CHECK(false);
        destroyLoopback();
        return;
    }

    packet = enet_packet_create(encryptedPacket, sizeof(encryptedPacket), ENET_PACKET_FLAG_RELIABLE);
    CHECK(packet != NULL);
    if (packet == NULL) {
        return;
    }
    if (enet_peer_send(serverPeer, CTRL_CHANNEL_GENERIC, packet) != 0) {
        CHECK(false);
        enet_packet_destroy(packet);
        destroyLoopback();
        return;
    }
    enet_host_flush(server);

    HostRestartNegotiated = true;
    encryptedControlStream = true;
    EncryptionFeaturesEnabled = SS_ENC_CONTROL_V2;
    ConnectionInterrupted = false;
    lastSeenFrame = 0;
    terminationCallbackCount = 0;
    terminationCallbackError = INT_MIN;
    runControlReceiver();
    CHECK(terminationCallbackCount == 1);
    CHECK(terminationCallbackError == ML_ERROR_HOST_RESTART);

    HostRestartNegotiated = false;
    encryptedControlStream = false;
    EncryptionFeaturesEnabled = 0;
    destroyLoopback();
}

static void testAllocations(void) {
    CHECK(SS_TERMINATION_HOST_RESTART_V1 == UINT32_C(0x53535201));
    CHECK(LI_FF_HOST_RESTART == UINT32_C(0x00000800));
    CHECK(ML_FF_HOST_RESTART == UINT32_C(0x00000100));
    CHECK(LI_HOST_RESTART_PROTOCOL_VERSION == 1);
    CHECK(ML_ERROR_HOST_RESTART == -5502);
    CHECK(ML_ERROR_HOST_RESTART != LI_ERR_UNSUPPORTED);
    CHECK((LI_FF_HOST_RESTART & (LI_FF_PEN_TOUCH_EVENTS | LI_FF_CONTROLLER_TOUCH_EVENTS |
                                 LI_FF_TOUCHPAD_EVENTS | LI_FF_TOUCHPAD_FRAME_EVENTS |
                                 LI_FF_CURSOR_SHAPE | LI_FF_DS5_HAPTICS_PCM |
                                 LI_FF_DYNAMIC_SDR_WHITE | LI_FF_REMOTE_TEXT_CONTEXT |
                                 LI_FF_CLIENT_RESOLUTION_CHANGE)) == 0);
    CHECK((ML_FF_HOST_RESTART & (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1 |
                                 ML_FF_DS5_HAPTICS_PCM | ML_FF_DS5_HAPTICS_IR_V2 |
                                 ML_FF_REMOTE_TEXT_CONTEXT | ML_FF_PYROWAVE |
                                 ML_FF_DS5_HAPTICS_STATE)) == 0);
    CHECK(SS_TERMINATION_HOST_RESTART_V1 != UINT32_C(0x80030023));
    CHECK(SS_TERMINATION_HOST_RESTART_V1 != UINT32_C(0x800e9403));
    CHECK(SS_TERMINATION_HOST_RESTART_V1 != UINT32_C(0x800e9302));
}

static void testHostRestartNegotiation(void) {
    static const char supportedSdp[] =
        "a=x-ss-general.featureFlags:2048 \r\n"
        "a=x-ss-restart.protocolVersion:1 \r\n";
    static const char missingVersionSdp[] = "a=x-ss-general.featureFlags:2048 \r\n";
    static const char wrongVersionSdp[] =
        "a=x-ss-general.featureFlags:2048 \r\n"
        "a=x-ss-restart.protocolVersion:2 \r\n";
    static const char malformedVersionSdp[] =
        "a=x-ss-general.featureFlags:2048 \r\n"
        "a=x-ss-restart.protocolVersion:1x \r\n";
    static const char duplicateVersionSdp[] =
        "a=x-ss-general.featureFlags:2048 \r\n"
        "a=x-ss-restart.protocolVersion:1 \r\n"
        "a=x-ss-restart.protocolVersion:1 \r\n";
    static const char missingFeatureSdp[] =
        "a=x-ss-general.featureFlags:1024 \r\n"
        "a=x-ss-restart.protocolVersion:1 \r\n";
    static const char malformedFeatureSdp[] =
        "a=x-ss-general.featureFlags:2048garbage \r\n"
        "a=x-ss-restart.protocolVersion:1 \r\n";

    CHECK(liIsHostRestartProtocolSupported(supportedSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(supportedSdp, false));
    CHECK(!liIsHostRestartProtocolSupported(missingVersionSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(wrongVersionSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(malformedVersionSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(duplicateVersionSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(missingFeatureSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(malformedFeatureSdp, true));
    CHECK(!liIsHostRestartProtocolSupported(NULL, true));
}

static void testTerminationClassification(void) {
    HostRestartNegotiated = true;
    encryptedControlStream = true;
    EncryptionFeaturesEnabled = SS_ENC_CONTROL_V2;
    CHECK(mapExtendedTerminationReason(SS_TERMINATION_HOST_RESTART_V1, 6, 0) ==
          ML_ERROR_HOST_RESTART);
    CHECK(mapExtendedTerminationReason(SS_TERMINATION_HOST_RESTART_V1, 6, 100) ==
          ML_ERROR_HOST_RESTART);

    CHECK(mapExtendedTerminationReason(SS_TERMINATION_HOST_RESTART_V1, 7, 100) ==
          ML_ERROR_GRACEFUL_TERMINATION);
    CHECK(mapExtendedTerminationReason(UINT32_C(0x53535202), 6, 100) ==
          ML_ERROR_GRACEFUL_TERMINATION);
    CHECK(mapShortTerminationReason(SS_TERMINATION_HOST_RESTART_V1 >> 16, 100) ==
          ML_ERROR_GRACEFUL_TERMINATION);

    HostRestartNegotiated = false;
    CHECK(mapExtendedTerminationReason(SS_TERMINATION_HOST_RESTART_V1, 6, 100) ==
          ML_ERROR_GRACEFUL_TERMINATION);
    HostRestartNegotiated = true;
    encryptedControlStream = false;
    CHECK(mapExtendedTerminationReason(SS_TERMINATION_HOST_RESTART_V1, 6, 100) ==
          ML_ERROR_GRACEFUL_TERMINATION);
    encryptedControlStream = true;
    EncryptionFeaturesEnabled = 0;
    CHECK(mapExtendedTerminationReason(SS_TERMINATION_HOST_RESTART_V1, 6, 100) ==
          ML_ERROR_GRACEFUL_TERMINATION);

    CHECK(mapExtendedTerminationReason(UINT32_C(0x80030023), 6, 0) ==
          ML_ERROR_UNEXPECTED_EARLY_TERMINATION);
    CHECK(mapExtendedTerminationReason(UINT32_C(0x80030023), 6, 1) ==
          ML_ERROR_GRACEFUL_TERMINATION);
    CHECK(mapShortTerminationReason(0x0100, 0) == ML_ERROR_UNEXPECTED_EARLY_TERMINATION);
    CHECK(mapShortTerminationReason(0x0100, 1) == ML_ERROR_GRACEFUL_TERMINATION);

    HostRestartNegotiated = true;
    encryptedControlStream = true;
    EncryptionFeaturesEnabled = SS_ENC_CONTROL_V2;
    CHECK(mapExtendedTerminationReason(UINT32_C(0x80030023), 6, 0) ==
          ML_ERROR_UNEXPECTED_EARLY_TERMINATION);
    CHECK(mapExtendedTerminationReason(UINT32_C(0x80030023), 6, 1) ==
          ML_ERROR_GRACEFUL_TERMINATION);

    HostRestartNegotiated = false;
    encryptedControlStream = false;
    EncryptionFeaturesEnabled = 0;
}

static void testConnectionOptInLatch(void) {
    SERVER_INFORMATION serverInfo = { 0 };
    STREAM_CONFIGURATION streamConfig;

    LiInitializeStreamConfiguration(&streamConfig);
    CHECK(!HostRestartRecoveryEnabled);

    LiSetHostRestartRecoveryEnabled(true);
    CHECK(LiStartConnection(&serverInfo, &streamConfig, NULL, NULL, NULL, NULL, 0, NULL, 0) != 0);
    CHECK(HostRestartRecoveryEnabled);

    CHECK(LiStartConnection(&serverInfo, &streamConfig, NULL, NULL, NULL, NULL, 0, NULL, 0) != 0);
    CHECK(!HostRestartRecoveryEnabled);
    CHECK(!HostRestartHostSupported);
    CHECK(!HostRestartNegotiated);
}

static bool generateRestartSdp(bool optIn, bool hostSupported, uint32_t encryptionSupported,
                               bool sunshine, uint32_t* clientFeatureFlags, char** payload) {
    int payloadLength;

    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.width = 1280;
    StreamConfig.height = 720;
    StreamConfig.fps = 60;
    StreamConfig.bitrate = 10000;
    StreamConfig.packetSize = 1024;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 430;
    AppVersionQuad[3] = sunshine ? -1 : 0;
    RemoteAddr.ss_family = AF_INET;
    ((struct sockaddr_in*)&RemoteAddr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    RtspPortNumber = 48010;
    VideoPortNumber = 47998;
    ControlPortNumber = 47999;
    AudioPortNumber = 48000;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    HostRestartRecoveryEnabled = optIn;
    HostRestartHostSupported = hostSupported;
    HostRestartNegotiated = false;
    EncryptionFeaturesSupported = encryptionSupported;
    EncryptionFeaturesRequested = 0;
    EncryptionFeaturesEnabled = 0;

    *payload = getSdpPayloadForStreamConfig(1, &payloadLength);
    if (*payload == NULL) {
        return false;
    }

    if (!parseSdpAttributeToUInt(*payload, "x-ml-general.featureFlags", clientFeatureFlags)) {
        *clientFeatureFlags = 0;
    }
    return true;
}

static void testClientAnnouncementNegotiation(void) {
    uint32_t clientFlags = 0;
    char* payload = NULL;

    CHECK(generateRestartSdp(false, true, SS_ENC_CONTROL_V2, true, &clientFlags, &payload));
    if (payload != NULL) {
        CHECK((clientFlags & ML_FF_HOST_RESTART) == 0);
        CHECK(strstr(payload, "x-ml-restart.protocolVersion") == NULL);
        CHECK(!HostRestartNegotiated);
        free(payload);
    }

    CHECK(generateRestartSdp(true, false, SS_ENC_CONTROL_V2, true, &clientFlags, &payload));
    if (payload != NULL) {
        CHECK((clientFlags & ML_FF_HOST_RESTART) == 0);
        CHECK(strstr(payload, "x-ml-restart.protocolVersion") == NULL);
        CHECK(!HostRestartNegotiated);
        free(payload);
    }

    CHECK(generateRestartSdp(true, true, 0, true, &clientFlags, &payload));
    if (payload != NULL) {
        CHECK((clientFlags & ML_FF_HOST_RESTART) == 0);
        CHECK(strstr(payload, "x-ml-restart.protocolVersion") == NULL);
        CHECK(!HostRestartNegotiated);
        free(payload);
    }

    CHECK(generateRestartSdp(true, true, SS_ENC_CONTROL_V2, false, &clientFlags, &payload));
    if (payload != NULL) {
        CHECK((clientFlags & ML_FF_HOST_RESTART) == 0);
        CHECK(strstr(payload, "x-ml-restart.protocolVersion") == NULL);
        CHECK(!HostRestartNegotiated);
        free(payload);
    }

    CHECK(generateRestartSdp(true, true, SS_ENC_CONTROL_V2, true, &clientFlags, &payload));
    if (payload != NULL) {
        CHECK((clientFlags & ML_FF_HOST_RESTART) != 0);
        CHECK(strstr(payload, "a=x-ml-restart.protocolVersion:1") != NULL);
        CHECK(HostRestartNegotiated);
        CHECK((EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) != 0);
        free(payload);
    }

    HostRestartRecoveryEnabled = false;
    HostRestartHostSupported = false;
    HostRestartNegotiated = false;
    EncryptionFeaturesSupported = 0;
    EncryptionFeaturesEnabled = 0;
}

int main(void) {
    static const uint8_t restartReason[] = { 0x53, 0x53, 0x52, 0x01 };
    static const uint8_t unsupportedRestartReason[] = { 0x53, 0x53, 0x52, 0x02 };
    static const uint8_t truncatedRestartReason[] = { 0x53, 0x53, 0x52 };
    static const uint8_t gracefulReason[] = { 0x80, 0x03, 0x00, 0x23 };
    static const uint8_t malformedRestartReason[] = { 0x53, 0x53, 0x52, 0x01, 0x00 };

    CHECK(enet_initialize() == 0);
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 430;
    AppVersionQuad[3] = -1;
    ListenerCallbacks.connectionTerminated = captureTermination;
    CHECK(initializeControlStream() == 0);
    encryptedControlStream = false;
    packetTypes = (short*)packetTypesGen7Enc;

    testAllocations();
    testHostRestartNegotiation();
    testTerminationClassification();
    testConnectionOptInLatch();
    testClientAnnouncementNegotiation();

    testTerminationPacket(restartReason, sizeof(restartReason), 0,
                          ML_ERROR_GRACEFUL_TERMINATION);
    testTerminationPacket(truncatedRestartReason, sizeof(truncatedRestartReason), 1,
                          ML_ERROR_GRACEFUL_TERMINATION);
    testTerminationPacket(malformedRestartReason, sizeof(malformedRestartReason), 1,
                          ML_ERROR_GRACEFUL_TERMINATION);
    testTerminationPacket(unsupportedRestartReason, sizeof(unsupportedRestartReason), 1,
                          ML_ERROR_GRACEFUL_TERMINATION);
    testTerminationPacket(gracefulReason, sizeof(gracefulReason), 0,
                          ML_ERROR_UNEXPECTED_EARLY_TERMINATION);
    testTerminationPacket(gracefulReason, sizeof(gracefulReason), 1,
                          ML_ERROR_GRACEFUL_TERMINATION);
    testEncryptedRestartTermination();

    stopping = true;
    destroyControlStream();
    enet_deinitialize();

    if (failures != 0) {
        fprintf(stderr, "%d host restart protocol checks failed\n", failures);
        return 1;
    }

    printf("Host restart protocol checks passed (%d assertions)\n", checks);
    return 0;
}
