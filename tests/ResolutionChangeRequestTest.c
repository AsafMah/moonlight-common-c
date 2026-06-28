// Drives the production sender over a loopback ENet connection and checks the
// exact bytes that reach the host.
#include "../src/ControlStream.c"

#include <stdio.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static ENetHost* host;

static bool connectLoopback(void) {
    struct sockaddr_in loopback = { 0 };
    ENetAddress bindAddress, hostAddress;
    ENetEvent event;

    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    enet_address_set_address(&bindAddress, (struct sockaddr*)&loopback, (socklen_t)sizeof(loopback));
    host = enet_host_create(AF_INET, &bindAddress, 1, CTRL_CHANNEL_COUNT, 0, 0);
    if (host == NULL || enet_socket_get_address(host->socket, &hostAddress) < 0) {
        return false;
    }

    client = enet_host_create(AF_INET, NULL, 1, CTRL_CHANNEL_COUNT, 0, 0);
    if (client == NULL) {
        return false;
    }
    peer = enet_host_connect(client, &hostAddress, CTRL_CHANNEL_COUNT, 0);
    if (peer == NULL) {
        return false;
    }

    for (int i = 0; i < 400 && peer->state != ENET_PEER_STATE_CONNECTED; i++) {
        enet_host_service(client, &event, 5);
        enet_host_service(host, &event, 5);
    }
    return peer->state == ENET_PEER_STATE_CONNECTED;
}

static ENetPacket* receiveOnHost(void) {
    ENetEvent event;

    for (int i = 0; i < 400; i++) {
        enet_host_service(client, NULL, 0);
        if (enet_host_service(host, &event, 5) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
            return event.packet;
        }
    }
    return NULL;
}

int main(void) {
    static const unsigned char expected[] = {
        0x06, 0x55,             // Packet type 0x5506
        0x00, 0x00, 0x00, 0x00, // SS_DYNAMIC_PARAM_TYPE_RESOLUTION
        0x80, 0x07, 0x00, 0x00, // 1920
        0x38, 0x04, 0x00, 0x00, // 1080
    };
    ENetPacket* packet;

    CHECK(enet_initialize() == 0);

    // A Sunshine host new enough to negotiate the Gen7Enc packet table, which
    // advertised the feature during the RTSP handshake.
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    CHECK(initializeControlStream() == 0);
    CHECK(packetTypes == packetTypesGen7Enc);
    CHECK(LI_FF_CLIENT_RESOLUTION_CHANGE == 0x400); // Must match the host's bit
    SunshineFeatureFlags = LI_FF_CLIENT_RESOLUTION_CHANGE;

    // Plaintext framing lets the host side read the payload without a session key.
    encryptedControlStream = false;

    CHECK(LiSendResolutionChangeRequest(1920, 1080) == LI_RESOLUTION_CHANGE_ERR_NOT_CONNECTED);
    CHECK(connectLoopback());

    // None of these may reach the wire: the host must see the valid request first.
    CHECK(LiSendResolutionChangeRequest(0, 720) == LI_RESOLUTION_CHANGE_ERR_INVALID);
    CHECK(LiSendResolutionChangeRequest(1280, 1) == LI_RESOLUTION_CHANGE_ERR_INVALID);
    CHECK(LiSendResolutionChangeRequest(16386, 720) == LI_RESOLUTION_CHANGE_ERR_INVALID);
    CHECK(LiSendResolutionChangeRequest(1280, 16386) == LI_RESOLUTION_CHANGE_ERR_INVALID);

    SunshineFeatureFlags = ~(uint32_t)LI_FF_CLIENT_RESOLUTION_CHANGE;
    CHECK(LiSendResolutionChangeRequest(1280, 720) == LI_RESOLUTION_CHANGE_ERR_UNSUPPORTED);
    SunshineFeatureFlags = LI_FF_CLIENT_RESOLUTION_CHANGE;

    packetTypes = (short*)packetTypesGen7; // No 0x5506 without the encrypted control stream
    CHECK(LiSendResolutionChangeRequest(1280, 720) == LI_RESOLUTION_CHANGE_ERR_UNSUPPORTED);
    packetTypes = (short*)packetTypesGen7Enc;

    // Odd sizes are rounded down to even ones.
    CHECK(LiSendResolutionChangeRequest(1921, 1081) == LI_RESOLUTION_CHANGE_OK);
    packet = receiveOnHost();
    CHECK(packet != NULL);
    if (packet != NULL) {
        CHECK(packet->dataLength == sizeof(expected));
        CHECK(packet->dataLength == sizeof(expected) && memcmp(packet->data, expected, sizeof(expected)) == 0);
        enet_packet_destroy(packet);
    }

    enet_host_destroy(client);
    client = NULL;
    peer = NULL;
    enet_host_destroy(host);
    stopping = true;
    destroyControlStream();
    enet_deinitialize();

    if (failures != 0) {
        fprintf(stderr, "%d resolution change request checks failed\n", failures);
        return 1;
    }

    puts("Resolution change request tests passed");
    return 0;
}
