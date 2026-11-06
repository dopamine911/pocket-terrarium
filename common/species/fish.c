/* species/fish.c - the aquarium's fish (PLAN.md D11, P2b): pocket-tank's fish as a pack, so the aquarium's bin is
 * today's fish tank on the same core. Everything here is what the fish already were:
 *  - stages at 2 h / 12 h / 96 h of tended time (pocket-tank 0.3.5; progression.h), changed at the age itself -
 *    the fish do not shed (sheds false: no shed cycle, no growth shed, no mister);
 *  - the drives are tank.h's THE FISH'S DRIVES, read here, so the pack and the swim cannot drift apart: the swim
 *    (tank.c update_fish) runs on those constants, this carries them as data;
 *  - a heat band its water sits in the middle of (the freshwater habitat, 25 C day and night): the body never leaves
 *    heat 6, so the heat field's effects (tank.h HEAT_*) never touch a fish;
 *  - the population cap POP_CAP, the six roster looks (tank.c ROSTER; the keeper's palette since 0.4), the bars
 *    that come in at juvenile (render.c), and the eight goals of the fish schema (v2-v4).
 *
 * THIS STRUCT IS THE PACK'S SOURCE (docs/ptpk.md "Authoring"): nothing reads it at run time. common/packs/ptpk_write.c encodes
 * it into the PTPK file the loader takes at boot (packs_load_builtin, through packs_add) and tools/ptpk_emit.c writes it to
 * packs/pt.fish.roster.ptpk (`make packs`). `.id` is the one-byte id old saves hold (the file's LEGACY_ID record), `.uid` the
 * FNV-1a-32 of "pt.fish.roster" (packs/builtin.h; --selftest-ptpk holds it to the hash), and its habitat is listed by uid.
 * `habitats` and `builtin` are the registry's, filled in when the pack is loaded. */
#include "species.h"
#include "habitat.h"
#include "packs/builtin.h"
#include "tank.h"                       /* THE FISH'S DRIVES, POP_CAP */

const species_t SPECIES_PACK_FISH = {
    .name = "FISH", .id = SPECIES_FISH, .uid = SPECIES_UID_FISH, .test_only = false,
    .world = WORLD_AQUARIUM,
    .n_habitat_uids = 1, .habitat_uid = { HABITAT_UID_FRESHWATER },
    .schedule = SCHED_DAY, .settle_s = 600,                   /* (the fish model sees day / night, not the phase) */
    .t_cold = 18, .t_lo = 22, .t_hi = 28, .t_hot = 32,         /* a tropical fish: the water's 25 C is heat 6 */
    .tau_s = { 60, 90, 120, 150 },
    .hunger_per_s = HUNGER_PER_S, .energy_per_s = FISH_ENERGY_PER_S, .energy_rest_per_s = FISH_ENERGY_REST_PER_S,
    .curiosity_per_s = FISH_CURIOSITY_PER_S, .hunger_per_cricket = FISH_HUNGER_PER_PELLET,
    .sleep_hunger_per_h = SLEEP_HUNGER_PER_H, .sleep_energy_per_h = SLEEP_ENERGY_PER_H, .sleep_stress_per_h = SLEEP_STRESS_PER_H,
    .stage_s = { 2 * 3600, 12 * 3600, 96 * 3600 },             /* 2 h / 12 h / 4 days (0.3.5) */
    .pop_cap = POP_CAP,
    .sheds = false,                                            /* the stage at the age itself; no mister */
    .diet = DIET_PELLET,
    .plan = PLAN_FISH,
    .morphs = { { "ROSTER", 0x38dcc7, 0xffd166 } }, .n_morphs = 1,   /* (looks are the roster's and the genes', 0.4: one here for the rules) */
    .pattern = { { "PLAIN" }, { "BARS" }, { "BARS" }, { "BARS" } },   /* a fry is plain; the pattern comes in at juvenile */
    .goals = FISH_SCHEMA_GOALS,                                /* all eight */
};
