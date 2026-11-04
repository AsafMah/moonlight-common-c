#include "EndpointRefreshProtocol.h"
#include <string.h>
#ifdef USE_MBEDTLS
#include <mbedtls/md.h>
#else
#include <openssl/hmac.h>
#endif

static bool hmacSha256(const unsigned char* key, size_t keyLength,
                       const unsigned char* data, size_t length, unsigned char out[32]) {
#ifdef USE_MBEDTLS
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key, keyLength, data, length, out) == 0;
#else
    unsigned int written = 0;
    return HMAC(EVP_sha256(), key, (int)keyLength, data, length, out, &written) != NULL && written == 32;
#endif
}

void LiErWipe(void* data, size_t length) {
    volatile unsigned char* bytes = data;
    while (length--) *bytes++ = 0;
}

static bool equal(const unsigned char* a, const unsigned char* b, size_t length) {
    unsigned char difference = 0;
    while (length--) difference |= *a++ ^ *b++;
    return difference == 0;
}

bool LiErDeriveKey(const unsigned char riKey[16], const unsigned char session[16], unsigned char key[32]) {
    static const unsigned char domain[] = "sunshine/endpoint_auto_refresh/v1";
    unsigned char context[sizeof(domain) - 1 + 16];
    memcpy(context, domain, sizeof(domain) - 1);
    memcpy(context + sizeof(domain) - 1, session, 16);
    return hmacSha256(riKey, 16, context, sizeof(context), key);
}

bool LiErEncode(unsigned char packet[64], uint8_t type, uint8_t stream,
                const unsigned char session[16], const unsigned char key[32],
                uint64_t sequence, const unsigned char nonce[16]) {
    unsigned char tag[32];
    unsigned int i;
    if (!sequence || type < LI_ER_PROBE || type > LI_ER_ACK || stream < LI_ER_VIDEO || stream > LI_ER_AUDIO) return false;
    memset(packet, 0, LI_ER_PACKET_SIZE);
    memcpy(packet, "EAPR", 4);
    packet[4] = LI_ER_VERSION;
    packet[5] = type;
    packet[6] = stream;
    memcpy(packet + 8, session, 16);
    for (i = 0; i < 8; i++) packet[31 - i] = (unsigned char)(sequence >> (i * 8));
    if (nonce) memcpy(packet + 32, nonce, 16);
    if (!hmacSha256(key, 32, packet, 48, tag)) return false;
    memcpy(packet + 48, tag, 16);
    LiErWipe(tag, sizeof(tag));
    return true;
}

bool LiErDecode(const void* data, size_t length, uint8_t stream,
                const unsigned char session[16], const unsigned char key[32], LI_ER_MESSAGE* message) {
    const unsigned char* packet = data;
    unsigned char tag[32];
    uint64_t sequence = 0;
    unsigned int i;
    bool valid;
    if (length != LI_ER_PACKET_SIZE || memcmp(packet, "EAPR", 4) || packet[4] != LI_ER_VERSION ||
        packet[7] || packet[6] != stream || packet[5] < LI_ER_PROBE || packet[5] > LI_ER_ACK ||
        !equal(packet + 8, session, 16)) return false;
    if (!hmacSha256(key, 32, packet, 48, tag)) return false;
    valid = equal(tag, packet + 48, 16);
    LiErWipe(tag, sizeof(tag));
    if (!valid) return false;
    for (i = 24; i < 32; i++) sequence = (sequence << 8) | packet[i];
    if (!sequence) return false;
    message->type = packet[5];
    message->sequence = sequence;
    memcpy(message->nonce, packet + 32, 16);
    return true;
}
