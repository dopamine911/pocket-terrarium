/* habitat/freshwater.c - the aquarium's water (PLAN.md D11, P2b): pocket-tank's tank as a habitat pack. A heater
 * holds it at 25 C everywhere, lit or dark, at every depth (the fish pack's band is 22..28 C: heat 6, never a pull
 * either way); no lamp, no hides, no humid hide to keep damp - the fish do not shed - and no rocks: its ground is the
 * floor alone (terrain.h; the fish swim). Its grass, decor and critters are the engine's as they always were (tank.c);
 * the humidity is the water's.
 *
 * THIS STRUCT IS THE PACK'S SOURCE (docs/ptpk.md "Authoring"): nothing reads it at run time. common/packs/ptpk_write.c encodes
 * it into the PTPK file the loader takes at boot (packs_load_builtin, through packs_add) and tools/ptpk_emit.c writes it to
 * packs/pt.habitat.freshwater.ptpk (`make packs`). `.id` is the one-byte id old saves hold (the file's LEGACY_ID record), `.uid`
 * the FNV-1a-32 of "pt.habitat.freshwater" (packs/builtin.h; --selftest-ptpk holds it to the hash). `builtin` is the registry's. */
#include "habitat.h"
#include "packs/builtin.h"

const habitat_t HABITAT_PACK_FRESHWATER = {
    .name = "FRESHWATER", .id = HABITAT_FRESHWATER, .uid = HABITAT_UID_FRESHWATER, .world = WORLD_AQUARIUM,
    .floor_warm_c = 25, .floor_cool_c = 25, .night_drop_c = 0,
    .bask_u = 0.5f, .bask_c = 25, .bask_r_px = 0,              /* no lamp */
    .n_hides = 0,
    .humidity = 1.0f,
};
