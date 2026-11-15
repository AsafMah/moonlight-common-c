// Drive LiSendStreamBitrate() and LiSendStreamFramerate() through the production
// control stream over a loopback ENet connection, and decrypt what the host receives.
#include "../src/ControlStream.c"
#include <stdio.h>

static unsigned failures;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value);                            \
        }                                                                                          \
    } while (0)

static ENetHost* server;
static ENetPeer* serverPeer;

static void pump(void) {
    ENetEvent event;

    for (int i = 0; i < 20; i++) {
        while (enet_host_service(server, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                serverPeer = event.peer;
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                // Handled by receiveOne()
                enet_packet_destroy(event.packet);
            }
        }
        enet_host_service(client, &event, 0);
        PltSleepMs(1);
    }
}

// Waits for one packet at the host and decrypts it. Returns the control packet
// type and copies the plaintext payload, or -1 when nothing arrives.
static int receiveOne(uint8_t* payload, int capacity, int* payloadLength) {
    ENetEvent event;

    for (int i = 0; i < 200; i++) {
        enet_host_service(client, &event, 0);
        if (enet_host_service(server, &event, 0) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
            PNVCTL_ENCRYPTED_PACKET_HEADER enc = (PNVCTL_ENCRYPTED_PACKET_HEADER)event.packet->data;
            unsigned char iv[12] = { 0 };
            unsigned char plain[64];
            int plainLength = enc->length - (int)sizeof(enc->seq) - AES_GCM_TAG_LENGTH;
            PNVCTL_ENET_PACKET_HEADER_V2 header = (PNVCTL_ENET_PACKET_HEADER_V2)plain;
            int type;

            // The client encrypts with the client-originated IV.
            iv[0] = (unsigned char)(enc->seq >> 0);
            iv[1] = (unsigned char)(enc->seq >> 8);
            iv[2] = (unsigned char)(enc->seq >> 16);
            iv[3] = (unsigned char)(enc->seq >> 24);
            iv[10] = 'C';
            iv[11] = 'C';

            CHECK(enc->encryptedHeaderType == 0x0001);
            CHECK(plainLength > 0 && plainLength <= (int)sizeof(plain));
            if (plainLength <= 0 || plainLength > (int)sizeof(plain) ||
                    !PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                       (unsigned char*)StreamConfig.remoteInputAesKey,
                                       sizeof(StreamConfig.remoteInputAesKey),
                                       iv, sizeof(iv),
                                       (unsigned char*)(enc + 1), AES_GCM_TAG_LENGTH,
                                       ((unsigned char*)(enc + 1)) + AES_GCM_TAG_LENGTH, plainLength,
                                       plain, &plainLength)) {
                enet_packet_destroy(event.packet);
                return -1;
            }
            type = header->type;
            *payloadLength = header->payloadLength;
            CHECK(*payloadLength <= capacity && plainLength == (int)sizeof(*header) + *payloadLength);
            memcpy(payload, plain + sizeof(*header), (size_t)*payloadLength);
            // Flow channel 0 is the generic control channel.
            CHECK(event.channelID == CTRL_CHANNEL_GENERIC);
            enet_packet_destroy(event.packet);
            return type;
        }
        PltSleepMs(1);
    }
    return -1;
}

static uint32_t word(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void expectSent(uint32_t subtype, uint32_t value) {
    uint8_t payload[16];
    int length = 0;

    CHECK(receiveOne(payload, sizeof(payload), &length) == 0x5506);
    CHECK(length == 8);
    CHECK(word(payload) == subtype);
    CHECK(word(payload + 4) == value);
}

static void expectNothing(void) {
    uint8_t payload[16];
    int length = 0;

    CHECK(receiveOne(payload, sizeof(payload), &length) == -1);
}

static uint32_t floatBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int main(void) {
    ENetAddress listen, target;
    struct sockaddr_in loopback;
    socklen_t loopbackLength = sizeof(loopback);

    CHECK(initializePlatform() == 0);

    // The range limits are part of the public contract and match the host.
    CHECK(LI_STREAM_BITRATE_MAX_KBPS == 800000);
    CHECK(LI_STREAM_FRAMERATE_MAX == 1000);

    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    EncryptionFeaturesEnabled = SS_ENC_CONTROL_V2;
    for (size_t i = 0; i < sizeof(StreamConfig.remoteInputAesKey); i++) {
        StreamConfig.remoteInputAesKey[i] = (char)(0x30 + i);
    }

    // Before any connection exists, there is nothing to send to.
    CHECK(LiSendStreamBitrate(5000) == -2);
    CHECK(LiSendStreamFramerate(60.0f) == -2);

    CHECK(initializeControlStream() == 0);
    CHECK(encryptedControlStream);

    memset(&loopback, 0, sizeof(loopback));
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    memset(&listen, 0, sizeof(listen));
    enet_address_set_address(&listen, (struct sockaddr*)&loopback, sizeof(loopback));
    enet_address_set_port(&listen, 0);
    server = enet_host_create(AF_INET, &listen, 1, CTRL_CHANNEL_COUNT, 0, 0);
    CHECK(server != NULL);
    client = enet_host_create(AF_INET, NULL, 1, CTRL_CHANNEL_COUNT, 0, 0);
    CHECK(client != NULL);
    if (server == NULL || client == NULL) {
        return 1;
    }

    // Not connected yet: the peer does not exist.
    CHECK(getsockname(server->socket, (struct sockaddr*)&loopback, &loopbackLength) == 0);
    memset(&target, 0, sizeof(target));
    enet_address_set_address(&target, (struct sockaddr*)&loopback, sizeof(loopback));
    enet_address_set_port(&target, ntohs(loopback.sin_port));
    peer = enet_host_connect(client, &target, CTRL_CHANNEL_COUNT, 0);
    CHECK(peer != NULL);
    CHECK(LiSendStreamBitrate(5000) == -3);

    pump();
    CHECK(peer->state == ENET_PEER_STATE_CONNECTED);
    CHECK(serverPeer != NULL);

    // Out-of-range values are rejected before anything is sent.
    CHECK(LiSendStreamBitrate(0) == -1);
    CHECK(LiSendStreamBitrate(-1) == -1);
    CHECK(LiSendStreamBitrate(LI_STREAM_BITRATE_MAX_KBPS + 1) == -1);
    CHECK(LiSendStreamFramerate(0.0f) == -1);
    CHECK(LiSendStreamFramerate(-30.0f) == -1);
    CHECK(LiSendStreamFramerate(1000.5f) == -1);
    CHECK(LiSendStreamFramerate(NAN) == -1);
    CHECK(LiSendStreamFramerate(INFINITY) == -1);
    expectNothing();

    // Subtype 2 carries kbps as little-endian i32; subtype 1 carries fps as f32.
    CHECK(LiSendStreamBitrate(1) == 0);
    expectSent(2, 1);
    CHECK(LiSendStreamBitrate(20000) == 0);
    expectSent(2, 20000);
    CHECK(LiSendStreamBitrate(LI_STREAM_BITRATE_MAX_KBPS) == 0);
    expectSent(2, 800000);
    CHECK(LiSendStreamFramerate(60.0f) == 0);
    expectSent(1, floatBits(60.0f));
    CHECK(LiSendStreamFramerate(119.88f) == 0);
    expectSent(1, floatBits(119.88f));
    CHECK(LiSendStreamFramerate((float)LI_STREAM_FRAMERATE_MAX) == 0);
    expectSent(1, floatBits(1000.0f));

    // The resolution request on the same message is unchanged.
    CHECK(LiRequestResolutionChange(1920, 1080) == 0);
    {
        uint8_t payload[16];
        int length = 0;
        CHECK(receiveOne(payload, sizeof(payload), &length) == 0x5506);
        CHECK(length == 12 && word(payload) == 0 && word(payload + 4) == 1920 && word(payload + 8) == 1080);
    }

    // A host that is not Sunshine, or an unencrypted stream, cannot take the message.
    AppVersionQuad[3] = 0;
    CHECK(LiSendStreamBitrate(5000) == -2);
    CHECK(LiSendStreamFramerate(60.0f) == -2);
    AppVersionQuad[3] = -1;
    encryptedControlStream = false;
    CHECK(LiSendStreamBitrate(5000) == -2);
    encryptedControlStream = true;
    expectNothing();

    // Once the peer is gone or the stream is stopping, the send reports it.
    enet_peer_disconnect_now(peer, 0);
    CHECK(LiSendStreamBitrate(5000) == -3);
    CHECK(LiSendStreamFramerate(60.0f) == -3);
    stopping = true;
    CHECK(LiSendStreamBitrate(5000) == -3);

    destroyControlStream();
    enet_host_destroy(client);
    enet_host_destroy(server);
    cleanupPlatform();

    if (failures != 0) {
        fprintf(stderr, "%u stream runtime parameter checks failed\n", failures);
        return 1;
    }
    puts("stream runtime parameter checks passed");
    return 0;
}
