# The tank's modules (issue #4 phase 3, slice A)

`common/tank.c` is the engine: events, the random stream, the glass, the roster and the genes, making and removing animals, the
touch / feed / light / orientation machinery, the packs' places and heat, the upkeep (the grass beds, the film on the glass, the
wipe and the scissors), the decor machinery, the night's sleep and the tick. It names **no world**: where the swim, the walk, the
pellets or the crickets, the shed or a critter would be, it calls through a module.

## Three kinds of module

| Contract | Chosen by | One `const` table per | Reached through |
|---|---|---|---|
| `loco_ops_t` (`common/locomotion.h`) | the species' movement (`species.h` `loco`, `caps.h` `CAP_SWIM`, `CAP_WALK`, later `CAP_SLITHER`) | movement: `loco_swim`, `loco_walk` | `tank_loco(t, i)`: one function-pointer call per animal per frame |
| `food_ops_t` (`common/mechanics.h`) | the species' diet (`DIET_*`) | kind of food: `food_pellets`, `food_crickets` | `tank_food(t)` |
| `mech_ops_t` (`common/mechanics.h`) | none: every module present is called at the engine's fixed points | world: `mech_aquarium`, `mech_terrarium` | `mech_n()` / `mech_at(k)`; `mech_serves(m, t)` says whether the tank's habitat provides an area the module serves |

What a hook does is in the headers; a member a module has no use for is `NULL` and the call site says what that means.
`tank_feed`, `tank_food_sight`, `tank_clear_food`, `tank_shed_clock`, `tank_mist_*`, `tank_deco_*`, `tank_hand_point`,
`tank_at_finger` and `tank_tap_d2` keep their names and signatures (`tank.h`) and are thin dispatchers in `tank.c`.

## Where the code lives

```
common/tank.c, tank_priv.h      the engine, and the few statics its modules share (private to the engine and to worlds/)
common/locomotion.h, mechanics.h   the contracts
common/locomotion/modules.c     THE ONE PLACE a build's modules are listed (compiled in every build)
common/worlds/aquarium/         swim.c, depth.c, pellets.c, aquarium.c (the world module), critters/{lanes,snail,urchin,crab,seahorse,shrimp}.c,
                                aquarium_priv.h
common/worlds/terrarium/        walk.c, hunt.c, look.c, crickets.c, shed.c, decor.c, terrarium.c (the world module), terrarium_priv.h
```

(the files of slices B and D - scenes, bodies, shop, setup - sit in the same directories: `docs/world-module.md`.)

`sim/Makefile` and `tankcore/CMakeLists.txt` compile `common/worlds/<world>/**`, `common/bodies/<body>/*` and
`common/locomotion/*.c` for the build's world, so a bin carries its own world's modules alone. `modules.c` lists the same
(`HAVE_AQUARIUM`, `HAVE_TERRARIUM`, each `PT_WORLD == ...`): nothing else in the engine knows.

## The step that builds both worlds into one image

A build-config change, by design: compile both `worlds/` directories (the Makefile and CMake globs), make both `HAVE_*` flags
1 in `modules.c`, and carry `caps.h` `CAPS_FW` as the union it already is under `PT_ALL_PACKS`. Every choice at run time is
made from a species' capabilities (`tank_loco`, `tank_food`) or a habitat's (`mech_serves`), never from `PT_WORLD`.
Still read from the build, on purpose: `tank.h`'s goal set (`GOAL_FIRST`, `GOAL_N`, `GOAL_W_*`) and the milestone counts
(`MS_FISH_COUNT`, `TMS_COUNT`), the advisor's token map, and `world_cur()` (`docs/world-module.md`) - the places a single-world
bin still decides what a tank's animals are called.

## Tests

`make check` runs each world's own list (`SELFTESTS_AQUARIUM`, `SELFTESTS_TERRARIUM`) plus `SELFTESTS_SHARED` in both bins.
Code that names one world's module is fenced in the tests and the firmware's shell with `#if PT_TERRARIUM` /
`#if !PT_TERRARIUM` (a bin never links the other world's symbols; no stand-in is ever defined for them).
