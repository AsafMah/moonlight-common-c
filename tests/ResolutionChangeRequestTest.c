// Drives the production LiRequestResolutionChange() over a loopback ENet
// connection with an encrypted control stream and checks the exact bytes the
// host decrypts.
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

// Receives one control message on the host side and decrypts it the way the
// receive path does. Legacy (non-V2) control encryption uses the same IV in
// both directions, so the client's own decrypt routine can open it.
static PNVCTL_ENET_PACKET_HEADER_V1 receiveOnHost(int* length) {
    ENetEvent event;

    for (int i = 0; i < 400; i++) {
        enet_host_service(client, NULL, 0);
        if (enet_host_service(host, &event, 5) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
            PNVCTL_ENCRYPTED_PACKET_HEADER encHdr = (PNVCTL_ENCRYPTED_PACKET_HEADER)event.packet->data;
            PNVCTL_ENET_PACKET_HEADER_V1 packet = NULL;

            if (event.packet->dataLength >= sizeof(*encHdr) && LE16(encHdr->encryptedHeaderType) == 0x0001) {
                encHdr->encryptedHeaderType = LE16(encHdr->encryptedHeaderType);
                encHdr->length = LE16(encHdr->length);
                encHdr->seq = LE32(encHdr->seq);
                if (!decryptControlMessageToV1(encHdr, (int)event.packet->dataLength, &packet, length)) {
                    packet = NULL;
                }
            }
            enet_packet_destroy(event.packet);
            return packet;
        }
    }
    return NULL;
}

int main(void) {
    static const unsigned char expectedPayload[] = {
        0x00, 0x00, 0x00, 0x00, // SS_DYNAMIC_PARAM_TYPE_RESOLUTION
        0x80, 0x07, 0x00, 0x00, // 1920
        0x38, 0x04, 0x00, 0x00, // 1080
    };
    PNVCTL_ENET_PACKET_HEADER_V1 packet;
    int length = 0;

    CHECK(enet_initialize() == 0);
    CHECK(LI_FF_CLIENT_RESOLUTION_CHANGE == 0x400); // Must match the host's bit

    // A Sunshine host new enough to negotiate the encrypted Gen7Enc packet table
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    CHECK(initializeControlStream() == 0);
    CHECK(packetTypes == packetTypesGen7Enc);
    encryptedControlStream = true;
    EncryptionFeaturesEnabled = 0;

    CHECK(LiRequestResolutionChange(1920, 1080) == -3); // Not connected yet
    CHECK(connectLoopback());

    // None of these may reach the wire: the host must see the valid request first.
    CHECK(LiRequestResolutionChange(0, 720) == -1);
    CHECK(LiRequestResolutionChange(1280, -720) == -1);
    CHECK(LiRequestResolutionChange(1281, 720) == -1);
    CHECK(LiRequestResolutionChange(16386, 720) == -1);

    encryptedControlStream = false;
    CHECK(LiRequestResolutionChange(1280, 720) == -2);
    encryptedControlStream = true;

    packetTypes = (short*)packetTypesGen7; // No 0x5506 without the encrypted control stream
    CHECK(LiRequestResolutionChange(1280, 720) == -2);
    packetTypes = (short*)packetTypesGen7Enc;

    // The host flag is advisory: the request is sent whether or not it's set.
    SunshineFeatureFlags = 0;
    CHECK(LiRequestResolutionChange(1920, 1080) == 0);
    packet = receiveOnHost(&length);
    CHECK(packet != NULL);
    if (packet != NULL) {
        CHECK(LE16(packet->type) == 0x5506);
        CHECK(length == (int)(sizeof(*packet) + sizeof(expectedPayload)));
        CHECK(length == (int)(sizeof(*packet) + sizeof(expectedPayload)) &&
              memcmp(packet + 1, expectedPayload, sizeof(expectedPayload)) == 0);
        free(packet);
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
