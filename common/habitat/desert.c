/* habitat/desert.c - the desert terrarium (a leopard gecko's): a dry floor warm on the left under the lamp
 * and the heat mat, cool on the right, three hides. Starting values, tuned in P2 like the species':
 * the floor 31 -> 25 C by day (a keeper's 30-32 warm side and 24-26 cool side), 5 C cooler at night; the
 * basking spot 35 C under the lamp (a care sheet's top end - hot for the gecko, and the only place the
 * A16 test pack, 6 C warmer, finds its band); the warm hide on the mat (30 C day and night), the humid
 * hide mid-floor (28 / 25 C), the cool hide at the far end (24 / 21 C). The humid hide starts at 0.6 and
 * dries in ~3 tended hours (docs/shed.md "Moisture"). */
#include "habitat.h"

const habitat_t HABITAT_PACK_DESERT = {
    .name = "DESERT", .id = HABITAT_DESERT, .world = WORLD_TERRARIUM,
    .floor_warm_c = 31, .floor_cool_c = 25, .night_drop_c = 5,
    .bask_u = 0.28f, .bask_c = 35, .bask_r_px = 70,
    .hides = {
        { HIDE_WARM,  0.0f,  30, 30 },
        { HIDE_HUMID, 0.55f, 28, 25 },
        { HIDE_COOL,  1.0f,  24, 21 },
    },
    .n_hides = 3,
    .moist_start = 0.6f, .moist_dry_per_s = 1.0f / 10800.0f,
    .humidity = 0.2f,
};
