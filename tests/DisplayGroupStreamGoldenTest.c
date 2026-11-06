#include "DisplayGroupStream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Two heads: leader 2560x1440@144 at the host origin, member 1920x1080@60
// at x=-1920 (left of the primary), member refused by the admission budget.
static int buildFixture(uint8_t* wire, uint8_t heads) {
    memset(wire, 0, 20 + 24 * 8);
    wire[0] = 1;
    wire[1] = heads;
    wire[2] = 0;
    wire[3] = 0;
    for (int i = 0; i < 16; i++) wire[4 + i] = (uint8_t)(0xA0 + i);
    for (uint8_t h = 0; h < heads; h++) {
        uint8_t* e = wire + 20 + 24 * h;
        e[0] = h;
        e[1] = h == 0 ? 1 : 2;
        e[2] = h == 0 ? 0 : 1;
        e[3] = 0;
        put32(e + 4, h == 0 ? 0 : (uint32_t)-1920);
        put32(e + 8, h == 0 ? 0 : 180);
        put32(e + 12, h == 0 ? 2560 : 1920);
        put32(e + 16, h == 0 ? 1440 : 1080);
        put32(e + 20, h == 0 ? 144000 : 60000);
    }
    return 20 + 24 * heads;
}

int main(void) {
    uint8_t wire[20 + 24 * 8];
    DISPLAY_GROUP_INFO info;
    int length = buildFixture(wire, 2);

    assert(length == 68);
    assert(parseDisplayGroupMessage(wire, length, &info));
    assert(info.version == 1 && info.headCount == 2 && info.thisHead == 0);
    for (int i = 0; i < 16; i++) assert(info.groupId[i] == (uint8_t)(0xA0 + i));
    assert(info.heads[0].index == 0 && info.heads[0].admission == LI_DISPLAY_HEAD_STREAMING);
    assert(info.heads[0].reason == LI_DISPLAY_HEAD_REASON_NONE);
    assert(info.heads[0].desktopX == 0 && info.heads[0].desktopY == 0);
    assert(info.heads[0].width == 2560 && info.heads[0].height == 1440);
    assert(info.heads[0].refreshMilliHz == 144000);
    assert(info.heads[1].index == 1 && info.heads[1].admission == LI_DISPLAY_HEAD_REFUSED);
    assert(info.heads[1].reason == LI_DISPLAY_HEAD_REASON_ADMISSION_BUDGET);
    assert(info.heads[1].desktopX == -1920 && info.heads[1].desktopY == 180);
    assert(info.heads[1].width == 1920 && info.heads[1].height == 1080);
    assert(info.heads[1].refreshMilliHz == 60000);

    // Every truncation and any trailing byte is rejected.
    for (int l = 0; l < length; l++) assert(!parseDisplayGroupMessage(wire, l, &info));
    assert(!parseDisplayGroupMessage(wire, length + 1, &info));
    assert(!parseDisplayGroupMessage(NULL, length, &info));
    assert(!parseDisplayGroupMessage(wire, length, NULL));

    // Unknown version, zero or too many heads, a nonzero reserved byte,
    // thisHead out of range, duplicate or out-of-range indexes and unknown
    // admission states are all rejected without touching the output.
    const int offsets[] = {0, 1, 1, 2, 3, 20, 44, 21};
    const uint8_t invalid[] = {2, 0, 9, 2, 1, 1, 0, 4};
    for (unsigned int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint8_t packet[sizeof(wire)];
        memcpy(packet, wire, sizeof(wire));
        packet[offsets[i]] = invalid[i];
        memset(&info, 0x5A, sizeof(info));
        assert(!parseDisplayGroupMessage(packet, length, &info));
        assert(info.headCount == 0x5A);
    }

    // The maximum head count parses.
    length = buildFixture(wire, LI_DISPLAY_GROUP_MAX_HEADS);
    assert(parseDisplayGroupMessage(wire, length, &info));
    assert(info.headCount == LI_DISPLAY_GROUP_MAX_HEADS);
    assert(info.heads[7].index == 7);

    // A member's copy names its own head.
    length = buildFixture(wire, 2);
    wire[2] = 1;
    assert(parseDisplayGroupMessage(wire, length, &info));
    assert(info.thisHead == 1);

    puts("Display group golden tests passed");
    return 0;
}
