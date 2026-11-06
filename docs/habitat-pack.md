# Habitat packs

The enclosure is a pack too, separate from the species (`docs/species-pack.md`).
A tank holds one habitat and one species; the species must list the habitat,
and the pair must pass the capability check: what the species needs (its
movement, areas, fittings, food, goals) must be provided by the habitat, the
firmware and the model (`docs/ptpk.md` "Capabilities"). The engine reads both;
neither the engine nor the model names a habitat.

## Where they live

A habitat is a **PTPK file** too (`docs/ptpk.md`: the format and the loader; the records that fill `habitat_t`, with their units).

- `common/habitat.h`: `habitat_t` and the registry (`habitat_get(id)`, `habitat_by_uid(uid)`, `habitat_at(i)`); `common/habitat.c`
  its pool and the rules (`habitat_check`). Like a species, everything in it is inline.
- `common/habitat/<name>.c`: one `const habitat_t` per habitat, the **source** of the built-in packs: `desert.c` (id 1, the
  terrarium's, and `pt.test.desert-warm`, id 31, A16's warm desert, tests only) and `freshwater.c` (id 2, the aquarium's, P2b).
  `common/packs/ptpk_write.c` encodes each into a PTPK file at boot and the loader takes it like any other pack;
  `tools/ptpk_emit.c` writes them as `packs/<uid_ns>.ptpk` (`docs/ptpk.md` "Authoring"). A release image carries its world's
  (the firmware links only those); a tank holds a pair whose needs are met.
- `common/terrain.h`: a habitat's ground laid out on a glass (P2c) - the walk, the heat field, the scene and the rules all read it
  from there.
- The save stores the habitat's uid (and its one-byte `habitat_id` when it is built in) beside the species', and each humid hide's moisture.

## What a habitat owns

| Field | Desert (id 1, the terrarium) | Freshwater (id 2, the aquarium) | Why it is the habitat's |
|---|---|---|---|
| floor temperature profile, lit and dark | 31 → 25 °C warm side → cool side, 5 °C cooler at night | 25 °C everywhere, day and night (a heater) | the same gecko lives warmer in a desert setup than a jungle one would let it |
| the air's rise toward the lamp, lit (`rise_c`) | +4 °C at the glass's top, straight up with height: the high rock ~2.3 °C over the floor under it | none | a lamp-heated terrarium is warmer high up; height is thermoregulation |
| the ground: rocks and ramps (`rocks`, `ramps`; below) | a low ledge, a mid shelf and the high rock, three ramps switching back up | the floor alone | |
| basking spot: its surface and position, temperature while lit | on the high rock at 0.20 along it, 35 °C | none (no lamp: `bask_r_px` 0) | |
| hides: kind, position, temperature | warm, humid, cool - on the floor (a hide is entered from the floor) | none | a jungle needs no humid hide; fish need none |
| humid hide: starting moisture, dry rate | 0.6, 1/10,800 s | (no humid hide) | |
| `humidity` baseline 0..1 | 0.2 | 1.0 (water) | data only in v1; later it eases sheds in humid habitats |
| backdrop, substrate, plants (P3) | sand, sandstone terraces, flagstone ramps, succulents | the engine's grass, decor and critters, as they always were | |
| upkeep chores (P5) | glass, latrine, humid hide | glass, grass | a jungle mists the whole enclosure instead |
| climbing surfaces (later) | none | none | branches arrive with the jungle and the climbers (glass and branches are not the ground) |
| `areas` (issue #4 phase 2) | LAND, BASK | WATER | what it offers to move on: WATER, LAND, BASK, SHORE (`AREAS`) |
| `provides` (issue #4 phase 2) | LAMP, HIDES, CRICKET, MEALWORM (the mealworm dish) | PELLET | fittings and foods (`PROVIDES`): LAMP, HIDES, CRICKET, MEALWORM, GREENS, PELLET |
| `worlds` (filter) | terrarium | aquarium | the `WORLDS` filter of the shop and menus only; it decides no pair |

## The ground (P2c)

The terrarium is a side view; the ground is what a lizard walks in it, so the glass's height is used: the floor, and
raised **rocks** the lizards climb onto by **ramps**. The schema's zone rows 1-3 (the top half of the glass) are "up on
the rocks".

**Data (`habitat.h`)**, in fractions of the glass so one habitat lays out on all four boards:

- surface 0 is always the floor; `rocks[k]` is surface k + 1: a walking line through 2..4 points `(u, v)` left to right -
  `v` up the glass (0 the floor's walking line, `PLACE_Y`, .. 1 the glass's top: `y = PLACE_Y (1 - v)`), `u` across it
  at that height (0 the warm side .. 1 the cool side, `TERRAIN_MARGIN` in from the glass: a rock high in the bowl
  spreads as the bowl does). `shelf` is the scene's: a shelf on the back wall (its underside showing) or a terrace
  standing on the sand.
- `ramps[k]`: a straight slope from a point on one surface (`lo`, at `u_lo` along it) to a point on another (`hi`,
  `u_hi`). A lizard changes surface only at a ramp's ends.
- `bask_surf`, `bask_u`: the basking spot's surface and its place along it.

**Laid out (`terrain.h`, `terrain_lay`)** on a glass (`glass_here()` this build's, `glass_of(k)` any board's) the ground
is **paths**: x-monotonic polylines of a lizard's middle - path 0 the floor (glass to glass), 1.. the rocks, then one per
ramp - and **links**, the points where a ramp's end lands on a surface (the two paths share it). A lizard's feet stand
`TERRAIN_FEET_DY` (8 px) under its line. `tank_terrain(t)` is the tank's ground on this board.

**The route (`terrain_route`)**: the quickest way along the paths and through the links from one (path, x) to another -
a Dijkstra over the links' sides (a dozen nodes); a slope costs its length over the pace on it (`terrain_pace`: uphill
x (1 - 0.5 sin a), downhill x (1 - 0.15 sin a)). The walk takes its next leg each frame: along its path to the point,
or to the link where the way steps onto the next path (`tank.h` THE WALK).

**The desert's** (`habitat/desert.c`; v up, u across):

| Piece | Where | On the 1.8 (rect) | On the watch |
|---|---|---|---|
| low ledge (terrace) | v 0.165, u 0.114..0.42 | y 287, x 74..193 | y 399, x 70..177 |
| mid shelf | v 0.34, u 0.47..0.97 | y 227, x 212..406 | y 315, x 194..370 |
| high rock (terrace) | v 0.58, u 0.02..0.50 | y 144, x 38..224 | y 201, x 37..205 |
| ramp 1 | floor 0.84 → low ledge's right end | 19° | 33° |
| ramp 2 | low ledge's left end → shelf's left end | 24° | 34° |
| ramp 3 | shelf's right end → high rock's right end | 24° | 35° |

Each ledge is walked on the way up: up ramp 1 to the low ledge's right end, along it, back up ramp 2 over it, along the
shelf, back up ramp 3 over it to the high rock and the basking spot. The hides stay on the floor: the warm cave under
the high rock, the humid box under the low ledge's end, the cool cave under the shelf.

## Rules (`--selftest-packs`, `habitat_check`)

- Every habitat: a name, an id in 1..31, a basking spot on its surface, hides on the floor, hides at least 0.2
  of the floor apart and never warmer dark than lit, moisture and humidity in 0..1, no air cooler toward the lamp.
- **Capabilities** (`habitat_check`): `areas` non-empty and a subset of the area bits (WATER LAND BASK SHORE), WATER or LAND
  among them, SHORE needing both; `provides` a subset of the fittings and foods. The declaration agrees with the fields:
  BASK in `areas` exactly when `bask_r_px > 0`; LAMP exactly when `bask_r_px > 0`; HIDES exactly when `n_hides > 0`; MEALWORM
  exactly when there is a dish; CRICKET and MEALWORM need LAND; PELLET needs WATER. A bit this firmware does not know is
  dropped at load, not refused.
- A habitat with **LAND** keeps the terrarium's rules: the floor's warm end warmer than its cool end, a lamp that warms its
  spot over the air there (on every board), at least one hide and a humid one among them (a lizard needs it to shed) -
  thermoregulation is possible. Without LAND the floor may be even, and a lamp and hides are the habitat's own business
  (freshwater has neither).
- **Its ground, on all four boards** (`terrain_check`): rocks' points on the glass with u rising; ramps joining two
  surfaces at points on them; no slope past `TERRAIN_SLOPE_MAX` (40°; steeper is a step, not walkable); every path inside
  the glass (`TERRAIN_MARGIN`) with a lizard's height (`TERRAIN_HEAD_PX`) under the glass above it; a ramp's ends
  `TERRAIN_LINK_IN_PX` (40) inside it, so every lizard reaches them; a rock at least `TERRAIN_LIFT_PX` (40) over the
  floor's line, all of it (the floor and its band stay open); two paths over one x at least `TERRAIN_HEADROOM_PX` (32)
  apart, but where a ramp parts from the surface it lands on or near a point both pass; every path reachable from the
  floor; the basking spot the highest ground within the lamp's pool, with `TERRAIN_LAMP_PX` (100) of glass over it; no
  rock or ramp lower than `TERRAIN_HIDE_CLEAR_PX` (44) over the floor within `TERRAIN_HIDE_HW` (36) of a hide.
- Every habitat's lit ground (the floor, its rocks, its ramps) reaches each compatible species' comfort band (`t_lo`..`t_hi`) somewhere.
- A tank never holds an incompatible (habitat, species) pair: `tank_set_packs` refuses it with the reason (the species does
  not list the habitat, the species needs a capability the firmware or the habitat lacks, or a goal the model lacks:
  `docs/ptpk.md` "Capabilities"), and a save of such a pair no longer loads as the world's defaults - the tank keeps the
  saved pair, shows PACK MISSING or PACK NEEDS and holds still until the pack comes or the need is met (`docs/ptpk.md` "A
  tank and its packs").
- The scene (`--lizard-sheet`): every point of every rock's and ramp's walking line stands on that piece's own drawn top,
  on all four glasses.

## Planned habitats

| Habitat | Species | Needs (capability words) |
|---|---|---|
| desert | leopard gecko, bearded dragon, monitor | provides LAND, BASK, LAMP, HIDES, CRICKET, MEALWORM; v1 (now) |
| freshwater (aquarium) | pocket-tank's fish | provides WATER, PELLET; P2b (now): today's fish tank as a pack |
| jungle | crested gecko, chameleon | the `climb` goal (a model with the word: schema change + retrain), branches, whole-enclosure misting |

## Worlds: one core, a bin per world (PLAN.md D11)

A world is a **preset** and a **filter**, not a gate. The code base is one reusable core; a build picks its world
(`PT_WORLD` in `common/world.h`, P2b, set when the bin is built - never at run time) and with it the preset: the model and
tokenizer (world.h THE BRAIN), the firmware modules (the swim, or `tank.h` THE WALK on the habitat's ground), the toolbox
(sponge and scissors, or those and the mister), a new tank's default pair and the selftest list. The `WORLDS` record of a
pack is the filter: the shop and the menus offer a pack when its mask includes the build's world (`packs_offered`).

The **capability check, not the world, decides a pair** (`docs/ptpk.md` "Capabilities"): a tank runs it when the species'
needs are met by the habitat, the firmware and the model, so a crossover habitat (WATER and LAND, SHORE) can host both
kinds of species once a firmware has both modules. What a build still decides: the model (one firmware cannot carry both
brains: the model partition is 8 MB (`firmware/partitions.csv`, `model` at 0x290000, size 0x800000) and a model is 7.56 MB),
the modules (`firmware_caps()`), and the default packs. The simulator keeps both worlds' packs loaded, so a cross-world pair
there is refused at the tank by the model leg (the gecko's goal `hunt` is not in the fish model's manifest).

| World | Bin | Model | Habitats |
|---|---|---|---|
| terrarium (the default) | the lizards, walking | the lizard model `lizard_v1` (schema L1; `model/out/model_q4_lizard.bin`) | desert (v1), jungle (planned) |
| aquarium | pocket-tank's fish, swimming, on the same packs | the fish model (schema v4) | freshwater (today's tank), reef and jellyfish (planned) |

Jellyfish drift rather than decide; in an aquarium habitat they would be a
critter (like the shrimp), not a model-driven species.

### Building each world

The simulator (`sim/`; this machine without `libsdl2-dev`: `/tmp/terr-make.sh` passes the unpacked SDL2's flags):

| | terrarium | aquarium |
|---|---|---|
| build (the 1.8's rectangle) | `make` → `./fishsim` (`build/`) | `make WORLD=aquarium` → `./fishsim-aquarium` (`build-aquarium/`) |
| another board | `make ROUND=1` / `WATCH=1` / `SQUARE=1` → `./fishsim-round` ... | the same with `WORLD=aquarium` → `./fishsim-round-aquarium` ... |
| its selftests | `make check` (the shared list and the terrarium's) | `make WORLD=aquarium check` (the shared list and the aquarium's) |

`make check-worlds` runs both worlds on one board, `make check-all` both worlds on all four boards: the gate for
anything in `common/`. The two bins of a board build side by side, each in its own directory.

The firmware (`firmware/`, the Kconfig choice `POCKET_TANK_WORLD`, default the terrarium; `sdkconfig.aquarium` picks
the aquarium, after a board's overlay):

```sh
idf.py -B <dir> build                                                                    # the terrarium, the 1.8
idf.py -B <dir> -DSDKCONFIG=<dir>/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.aquarium" build
idf.py -B <dir> -DSDKCONFIG=<dir>/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.round;sdkconfig.aquarium" build
```

`tankcore` defines `PT_WORLD` (PUBLIC: `main` and `llm` see the same world) and `main` embeds the tokenizer
world.h names for the world (`PT_FW_TOKENIZER_*`) as `tokenizer.bin`. Each bin runs its own world's model alone
(`advisor_core_init` refuses the other's); the flash scripts write the build's world's model with
`--model` (`tools/model_trailer.py --print-model` names it). The host test `firmware/host_test/decide_host.c`
is the fish model's: build it with `-DPT_WORLD=1` (its header has the line).
