#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LI_ER_VERSION 1
#define LI_ER_PACKET_SIZE 64
#define LI_ER_KEY_SIZE 32
#define LI_ER_SESSION_SIZE 16
#define LI_ER_NONCE_SIZE 16
#define LI_ER_VIDEO 1
#define LI_ER_AUDIO 2
#define LI_ER_PROBE 1
#define LI_ER_CHALLENGE 2
#define LI_ER_RESPONSE 3
#define LI_ER_ACK 4

typedef struct _LI_ER_MESSAGE {
    uint8_t type;
    uint64_t sequence;
    unsigned char nonce[LI_ER_NONCE_SIZE];
} LI_ER_MESSAGE;

bool LiErDeriveKey(const unsigned char riKey[16], const unsigned char session[16], unsigned char key[32]);
bool LiErEncode(unsigned char packet[64], uint8_t type, uint8_t stream,
                const unsigned char session[16], const unsigned char key[32],
                uint64_t sequence, const unsigned char nonce[16]);
bool LiErDecode(const void* packet, size_t length, uint8_t stream,
                const unsigned char session[16], const unsigned char key[32], LI_ER_MESSAGE* message);
void LiErWipe(void* data, size_t length);

#ifdef __cplusplus
}
#endif
