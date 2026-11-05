/* habitat/desert.c - the desert terrarium (a leopard gecko's): a dry floor warm on the left under the lamp and the heat
 * mat, cool on the right, three hides on the floor, and sandstone terraces stepping up the back wall. Starting values,
 * tuned in P2 like the species':
 *   the air      the floor 31 -> 25 C by day (a keeper's 30-32 warm side and 24-26 cool side), 5 C cooler at night; lit,
 *                the air rises rise_c with height under the lamp (4 C at the glass's top: the high rock's ~2.3 C over the
 *                floor below it, so height is thermoregulation too);
 *   the rocks    (terrain.h; fractions of the glass, so every board): a LOW LEDGE on the left (v 0.165), a MID LEDGE on the
 *                right (v 0.34) and the HIGH ROCK on the left (v 0.58: well into the glass's top half on every board, the
 *                schema's zone rows 1-3), climbed by three ramps that switch back so each ledge is walked: from the floor
 *                at 0.84 up left onto the low ledge's right end, along it to its left end, from there up right over it onto
 *                the mid ledge's left end, along it to its right end, and from there up left over it onto the high rock's
 *                right end - at most ~35 deg on the steepest board (the portrait watch), 19-28 deg on the others;
 *   the basking spot on top of the high rock under the lamp (35 C: a care sheet's top end - hot for the gecko, and where
 *                A16's test pack, 6 C warmer, finds its band), real geckos bask on warm rock;
 *   the hides    on the floor: the warm cave on the mat at the left end, under the high rock (30 C day and night), the
 *                humid box under the low ledge's end (28 / 25 C), the cool cave at the right end under the mid ledge
 *                (24 / 21 C).
 * The humid hide starts at 0.6 and dries in ~3 tended hours (docs/shed.md "Moisture"). */
#include "habitat.h"

const habitat_t HABITAT_PACK_DESERT = {
    .name = "DESERT", .id = HABITAT_DESERT, .world = WORLD_TERRARIUM,
    .floor_warm_c = 31, .floor_cool_c = 25, .night_drop_c = 5, .rise_c = 4,
    .bask_surf = 3, .bask_u = 0.20f, .bask_c = 35, .bask_r_px = 70,
    .hides = {
        { HIDE_WARM,  0.0f,  30, 30 },
        { HIDE_HUMID, 0.40f, 28, 25 },
        { HIDE_COOL,  1.0f,  24, 21 },
    },
    .n_hides = 3,
    .rocks = {
        { 2, { 0.114f, 0.42f }, { 0.165f, 0.165f }, false },  /* 1: the low ledge, left: a terrace */
        { 2, { 0.47f,  0.97f }, { 0.34f,  0.34f }, true },    /* 2: the mid ledge, right: a shelf on the back wall, the cool cave under it */
        { 2, { 0.02f,  0.50f }, { 0.58f,  0.58f }, false },   /* 3: the high rock, left, under the lamp: a terrace */
    },
    .n_rocks = 3,
    .ramps = {
        { 0, 1, 0.84f,  0.42f },                       /* the floor up left onto the low ledge's right end */
        { 1, 2, 0.114f, 0.47f },                       /* its left end up right onto the mid ledge's left end */
        { 2, 3, 0.97f,  0.50f },                       /* its right end up left onto the high rock's right end */
    },
    .n_ramps = 3,
    .moist_start = 0.6f, .moist_dry_per_s = 1.0f / 10800.0f,
    .humidity = 0.2f,
};
