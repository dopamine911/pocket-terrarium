# The world module (issue #4 phase 3, slice D)

The engine files that used to ask `PT_TERRARIUM` (progression, setup, notice, shelf, advisor, the boot show) now ask
`world_cur()` (`common/world.h`) for a `world_t` (`common/world_ops.h`). Each world defines its own descriptor in
`common/worlds/<world>/world.c`; the sim's Makefile and `tankcore/CMakeLists.txt` compile the files of the build's world
(`worlds/<PT_WORLD>/`) by directory, so a world's code and data are carried by its own bin alone. Nothing changes in a
one-world bin; a later build that carries both worlds replaces `world_cur()` with the pick of a tank's world (its
species'/habitat's WORLDS filter) and nothing else here.

## What a world defines

| `world_t` field | engine caller | aquarium | terrarium |
|---|---|---|---|
| `rules`, `goal_friend` | `advisor_rules`, `progression_tick` | `rules.c` (the fish's reflex) | `rules.c` (the lizard's) |
| `unlock_bits`, `can_buy`, `item_bought`, `earn_lines` | `progression_buy`, the save, HOW TO EARN | `shop.c` | `shop.c` |
| `dish_here` | the save's dish worms | `shop.c` (none) | `shop.c` |
| `arrival` (`ARRIVAL_COURT` / `ARRIVAL_HATCH`), `care_gates`, `fry_tip`, `fry_extra`, `nursery_ready`, `court_ok`, `arrival_place` | the fry checklist, the courtship, the hatching | `fry.c` | `fry.c` |
| `scan_fish`, `scan_tank`, `ms_silent`, `tms_silent` (+ `MS_NAMES`, `TMS_NAMES`) | `progression_tick`, `notice.c`, the cards | `milestones.c` | `milestones.c` |
| `light_on`, `slept` | the light-on edge, a long sleep | no-ops | the MEALWORM FARM's mornings |
| `restore` | `load_save` | `restore.c` (shrimp, urchin, crab, seahorse) | none |
| `words` | checklist rows, `SHELF_NAME_*` | in `world.c` | in `world.c` |
| `setup` (`setup_world.h`) | `setup.c`: pages, words, the editor / MORPH page | `setup.c` | `setup.c` |

The boot show is the aquarium's alone: `worlds/aquarium/boot_show.c` (moved verbatim) and a stub in
`worlds/terrarium/boot_show.c` with the same API.

`save_t` and its SAVE LAYOUT LOCK moved verbatim to `common/save_layout.h`: the layout, the offsets and the shop ids are frozen.

## What still picks a world at compile time

`common/world.h` is the one place: `PT_WORLD` -> `PT_WORLD_KEY`, and `PT_PICK(PT_MODEL_TAG_)` etc. choose the model's file and
identity constants (they must stay literals: `tools/model_trailer.py`, the firmware's CMake and the sim's string pastes read them).
`PT_TERRARIUM` and `WORLD_STR` remain for the firmware shell and the tests; the engine files no longer use them.
