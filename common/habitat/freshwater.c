/* habitat/freshwater.c - the aquarium's water (PLAN.md D11, P2b): pocket-tank's tank as a habitat pack. A heater
 * holds it at 25 C everywhere, lit or dark (the fish pack's band is 22..28 C: heat 6, never a pull either way);
 * no lamp, no hides, no humid hide to keep damp - the fish do not shed. Its grass, decor and critters are the
 * engine's as they always were (tank.c); the humidity is the water's. */
#include "habitat.h"

const habitat_t HABITAT_PACK_FRESHWATER = {
    .name = "FRESHWATER", .id = HABITAT_FRESHWATER, .world = WORLD_AQUARIUM,
    .floor_warm_c = 25, .floor_cool_c = 25, .night_drop_c = 0,
    .bask_u = 0.5f, .bask_c = 25, .bask_r_px = 0,              /* no lamp */
    .n_hides = 0,
    .humidity = 1.0f,
};
