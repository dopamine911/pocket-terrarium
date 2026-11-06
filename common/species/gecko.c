/* species/gecko.c - the leopard gecko (docs/species-pack.md "The leopard gecko
 * pack", v1 values: starting points, tuned in P2 against A13-A15), and A16's
 * test pack: the same lizard with its whole heat band 6 C warmer, compiled in
 * for the pack-independence census and never offered to a keeper. Its walk
 * (P2b, tank.c THE WALK): an unhurried pace - a hatchling's 24 px/s down to an
 * elder's 15 (the 1.8's 388 px floor in about half a minute, pauses and all) -
 * in bouts of 0.8..3 s with 0.5..2.5 s stops between, a stalker's stop-and-go;
 * it turns about on the spot in 0.7 s. What it needs - the capabilities its desert provides - is derived from what the struct
 * already says: it walks (a walk in its LOCOMOTION, the numbers above), on land, to bask under a lamp and to hide (it sheds: a humid
 * hide), and it eats crickets and mealworms (the dish); its goals are the lizard model's seven words.
 *
 * THIS STRUCT IS THE PACK'S SOURCE (docs/ptpk.md "Authoring"): nothing reads it at run time. common/packs/ptpk_write.c encodes
 * it into the PTPK file the loader takes at boot (packs_load_builtin, through packs_add) and tools/ptpk_emit.c writes it to
 * packs/pt.gecko.leopard.ptpk (`make packs`). `.id` is the one-byte id old saves hold (the file's LEGACY_ID record), `.uid` the
 * FNV-1a-32 of "pt.gecko.leopard" (packs/builtin.h; --selftest-ptpk holds it to the hash), and the habitats it lives in are
 * listed by their uids. `habitats` and `builtin` are the registry's, filled in when the pack is loaded. */
#include "species.h"
#include "habitat.h"
#include "packs/builtin.h"

#define GECKO_MORPHS                                                                           \
    { "NORMAL",    0xe3b83f, 0x3b2c22 },   /* yellow, dark spots */                            \
    { "TANGERINE", 0xf0892a, 0x5a3418 },                                                       \
    { "ALBINO",    0xf0d49c, 0xa8744c },   /* pale, brown spots */                             \
    { "BLIZZARD",  0xe6e2da, 0xc9c3b8 },   /* patternless white-grey */                        \
    { "MACK SNOW", 0xefeee6, 0x26262a }    /* white, black spots */

#define GECKO_PACK(NAME, ID, UID, TEST, WARMER) {                                              \
    .name = NAME, .id = ID, .uid = UID, .test_only = TEST,                                     \
    /* (no world: a pack's needs say where it can live - docs/ptpk.md "Capabilities"; `.worlds` below is the shop's filter) */  \
    .n_habitat_uids = (TEST) ? 2 : 1,                          /* (the test pack: also A16's warm desert) */ \
    .habitat_uid = { HABITAT_UID_DESERT, (TEST) ? HABITAT_UID_TEST_WARM : 0 },                 \
    .schedule = SCHED_DAY, .settle_s = 600,                    /* ten minutes winding down */  \
    .t_cold = 20 + (WARMER), .t_lo = 28 + (WARMER), .t_hi = 32 + (WARMER), .t_hot = 38 + (WARMER), \
    .tau_s = { 60, 90, 120, 150 },                             /* a hatchling warms in a minute */ \
    .hunger_per_s = 0.0045f, .energy_per_s = 0.006f, .energy_rest_per_s = 0.05f, .curiosity_per_s = 0.004f, \
    .hunger_per_cricket = 3.0f,                                                                \
    .sleep_hunger_per_h = 0.6f, .sleep_energy_per_h = 2.0f, .sleep_stress_per_h = 2.0f,        \
    .walk_px_s = { 24, 22, 18, 15 }, .bout_s = { 0.8f, 3.0f }, .pause_s = { 0.5f, 2.5f }, .turn_s = 0.7f, \
    .worlds = PT_WORLD_TERRARIUM,                              /* the shop's and the menus' filter: nothing else */ \
    .n_loco = 1, .loco = { { CAP_WALK, CAP_LAND } },           /* it walks, on land (the numbers above are the walk's) */ \
    .media_needs = CAP_LAND | CAP_BASK | CAP_HIDES,            /* land under a lamp's basking spot, and hides (a humid one: it sheds) - what the desert has */ \
    .stage_s = { 1200, 5400, 28800 },                                                          \
    .pop_cap = 3,                                              /* solitary */                  \
    .sheds = true,                                             /* docs/shed.md */              \
    .shed_every_s = { 900, 1800, 10800, 21600 }, .shed_soon_s = 300, .shed_window_s = 600,     \
    .shed_style = SHED_WHOLE,                                  /* one piece, and it eats it */ \
    .diet = DIET_CRICKET | DIET_MEALWORM,                                                      \
    .plan = PLAN_LIZARD,                                                                       \
    .geom = { .length_px = 84, .head_w = 14, .limb_len = 12, .tail_fat = 0.6f, .eye_r = 2.6f, .eyelids = true }, \
    .morphs = { GECKO_MORPHS }, .n_morphs = (uint8_t)(sizeof((morph_t[]){ GECKO_MORPHS }) / sizeof(morph_t)), \
    .pattern = { { "BANDED" }, { "BANDED" }, { "SPOTTED" }, { "SPOTTED" } },                   /* hatchling bands break into spots */ \
    .goal = { LZ_GOAL_WORDS }, .n_goals = LZ_GOALS,           /* all seven, no climbing: the lizard model's words (packs/models.c) */ \
}

const species_t SPECIES_PACK_GECKO     = GECKO_PACK("LEOPARD GECKO", SPECIES_GECKO, SPECIES_UID_GECKO, false, 0);
#if PT_ALL_PACKS
const species_t SPECIES_PACK_TEST_WARM = GECKO_PACK("TEST WARM PACK", SPECIES_TEST_WARM, SPECIES_UID_TEST_WARM, true, 6);
#endif
