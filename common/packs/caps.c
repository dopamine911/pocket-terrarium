/* caps.c - the capabilities' names, and the simulator's override of what the firmware provides (caps.h) */
#include "caps.h"
#include <stdio.h>
#include <string.h>

static const char *const NAME[32] = {
    [0] = "SWIM", [1] = "WALK", [2] = "SLITHER",
    [4] = "WATER", [5] = "LAND", [6] = "BASK", [7] = "SHORE",
    [8] = "CRICKET", [9] = "MEALWORM", [10] = "GREENS", [11] = "PELLET",
    [12] = "LAMP", [13] = "HIDES",
};

const char *caps_name(unsigned index) { return index < 32 ? NAME[index] : NULL; }

int caps_unknown(caps_t set) {
    const caps_t odd = set & ~CAPS_KNOWN;
    if (!odd) return -1;
    int k = 0;
    while (!((odd >> k) & 1u)) k++;
    return k;
}

void caps_list(caps_t set, char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    size_t at = 0;
    for (unsigned k = 0; k < 32 && at + 1 < n; k++) {
        if (!((set >> k) & 1u)) continue;
        char bit[12];
        const char *w = caps_name(k);
        if (!w) { snprintf(bit, sizeof bit, "bit %u", k); w = bit; }
        const int put = snprintf(out + at, n - at, "%s%s", at ? ", " : "", w);
        if (put < 0) break;
        at += (size_t)put < n - at ? (size_t)put : n - at - 1;
    }
}

caps_t caps_moves_of_areas(caps_t areas) {
    return ((areas & CAP_WATER) ? CAP_SWIM : 0u) | ((areas & CAP_LAND) ? (CAP_WALK | CAP_SLITHER) : 0u);
}

#if PT_ALL_PACKS
static bool   s_over;
static caps_t s_over_caps;
void firmware_caps_override(bool on, caps_t caps) { s_over = on; s_over_caps = caps; }
caps_t firmware_caps(void) { return s_over ? s_over_caps : CAPS_FW; }
#endif
