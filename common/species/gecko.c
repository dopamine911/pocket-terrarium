/* species/gecko.c - the leopard gecko (docs/species-pack.md "The leopard gecko
 * pack", v1 values: starting points, tuned in P2 against A13-A15), and A16's
 * test pack: the same lizard with its whole heat band 6 C warmer, compiled in
 * for the pack-independence census and never offered to a keeper. Its walk
 * (P2b, tank.c THE WALK): an unhurried pace - a hatchling's 24 px/s down to an
 * elder's 15 (the 1.8's 388 px floor in about half a minute, pauses and all) -
 * in bouts of 0.8..3 s with 0.5..2.5 s stops between, a stalker's stop-and-go;
 * it turns about on the spot in 0.7 s. */
#include "species.h"
#include "habitat.h"

static const morph_t GECKO_MORPHS[] = {
    { "NORMAL",    0xe3b83f, 0x3b2c22 },   /* yellow, dark spots */
    { "TANGERINE", 0xf0892a, 0x5a3418 },
    { "ALBINO",    0xf0d49c, 0xa8744c },   /* pale, brown spots */
    { "BLIZZARD",  0xe6e2da, 0xc9c3b8 },   /* patternless white-grey */
    { "MACK SNOW", 0xefeee6, 0x26262a },   /* white, black spots */
};
static const pattern_t GECKO_BANDED = { "BANDED" }, GECKO_SPOTTED = { "SPOTTED" };   /* hatchling bands break into spots */

#define GECKO_PACK(NAME, ID, TEST, WARMER) {                                                   \
    .name = NAME, .id = ID, .test_only = TEST,                                                 \
    .world = WORLD_TERRARIUM, .habitats = 1u << HABITAT_DESERT,                                \
    .schedule = SCHED_DAY, .settle_s = 600,                    /* ten minutes winding down */  \
    .t_cold = 20 + (WARMER), .t_lo = 28 + (WARMER), .t_hi = 32 + (WARMER), .t_hot = 38 + (WARMER), \
    .tau_s = { 60, 90, 120, 150 },                             /* a hatchling warms in a minute */ \
    .hunger_per_s = 0.0045f, .energy_per_s = 0.006f, .energy_rest_per_s = 0.05f, .curiosity_per_s = 0.004f, \
    .hunger_per_cricket = 3.0f,                                                                \
    .sleep_hunger_per_h = 0.6f, .sleep_energy_per_h = 2.0f, .sleep_stress_per_h = 2.0f,        \
    .walk_px_s = { 24, 22, 18, 15 }, .bout_s = { 0.8f, 3.0f }, .pause_s = { 0.5f, 2.5f }, .turn_s = 0.7f, \
    .stage_s = { 1200, 5400, 28800 },                                                          \
    .pop_cap = 3,                                              /* solitary */                  \
    .sheds = true,                                             /* docs/shed.md */              \
    .shed_every_s = { 900, 1800, 10800, 21600 }, .shed_soon_s = 300, .shed_window_s = 600,     \
    .shed_style = SHED_WHOLE,                                  /* one piece, and it eats it */ \
    .diet = DIET_CRICKET | DIET_MEALWORM,                                                      \
    .plan = PLAN_LIZARD,                                                                       \
    .geom = { .length_px = 84, .head_w = 14, .limb_len = 12, .tail_fat = 0.6f, .eye_r = 2.6f, .eyelids = true }, \
    .morphs = GECKO_MORPHS, .n_morphs = (uint8_t)(sizeof GECKO_MORPHS / sizeof GECKO_MORPHS[0]), \
    .pattern = { &GECKO_BANDED, &GECKO_BANDED, &GECKO_SPOTTED, &GECKO_SPOTTED },              \
    .goals = LZ_SCHEMA_GOALS,                                  /* all seven, no climbing */    \
}

const species_t SPECIES_PACK_GECKO     = GECKO_PACK("LEOPARD GECKO", SPECIES_GECKO, false, 0);
const species_t SPECIES_PACK_TEST_WARM = GECKO_PACK("TEST WARM PACK", SPECIES_TEST_WARM, true, 6);
