#pragma once

#include "Limelight.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 0x55F0 v1, host -> client, little-endian (docs/protocol-extension-registry.md):
//   u8 version (1), u8 headCount (1..LI_DISPLAY_GROUP_MAX_HEADS), u8 thisHead,
//   u8 reserved (0), u8 groupId[16], then headCount entries of
//   { u8 index, u8 admission, u16 reason, i32 desktopX, i32 desktopY,
//     u32 width, u32 height, u32 refreshMilliHz }.
// Entry i must carry index i. The whole message is rejected otherwise.
#define DISPLAY_GROUP_HEADER_LENGTH 20
#define DISPLAY_GROUP_HEAD_LENGTH 24

static inline uint32_t displayGroupRead32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline bool parseDisplayGroupMessage(const uint8_t* wire, int length,
                                            PDISPLAY_GROUP_INFO info) {
    DISPLAY_GROUP_INFO parsed;

    if (wire == NULL || info == NULL || length < DISPLAY_GROUP_HEADER_LENGTH) {
        return false;
    }
    if (wire[0] != 1 || wire[1] == 0 || wire[1] > LI_DISPLAY_GROUP_MAX_HEADS ||
            wire[2] >= wire[1] || wire[3] != 0 ||
            length != DISPLAY_GROUP_HEADER_LENGTH + DISPLAY_GROUP_HEAD_LENGTH * wire[1]) {
        return false;
    }

    memset(&parsed, 0, sizeof(parsed));
    parsed.version = wire[0];
    parsed.headCount = wire[1];
    parsed.thisHead = wire[2];
    memcpy(parsed.groupId, wire + 4, sizeof(parsed.groupId));
    for (uint8_t i = 0; i < parsed.headCount; i++) {
        const uint8_t* entry = wire + DISPLAY_GROUP_HEADER_LENGTH + DISPLAY_GROUP_HEAD_LENGTH * i;
        PDISPLAY_GROUP_HEAD head = &parsed.heads[i];

        if (entry[0] != i || entry[1] > LI_DISPLAY_HEAD_ENDED) {
            return false;
        }
        head->index = entry[0];
        head->admission = entry[1];
        head->reason = (uint16_t)(entry[2] | (entry[3] << 8));
        head->desktopX = (int32_t)displayGroupRead32(entry + 4);
        head->desktopY = (int32_t)displayGroupRead32(entry + 8);
        head->width = displayGroupRead32(entry + 12);
        head->height = displayGroupRead32(entry + 16);
        head->refreshMilliHz = displayGroupRead32(entry + 20);
    }

    *info = parsed;
    return true;
}
