# Habitat packs — DRAFT

The enclosure is a pack too, separate from the species (`docs/species-pack.md`).
A tank holds one habitat and one species, the species must list the habitat
as compatible, and both must be of the bin's world. The engine reads both;
neither the engine nor the model names a habitat.

## Where they live

- `common/habitat.h`: `habitat_t`, `habitat_get(id)`; `common/habitat.c` the registry and the rules.
- `common/habitat/<name>.c`: one `const habitat_t` per habitat, compiled in: `desert.c` (id 1, the terrarium's) and
  `freshwater.c` (id 2, the aquarium's, P2b). Both are in both bins; a tank holds its world's.
- The save stores `habitat_id` beside the species `pack_id`, and each humid hide's moisture.

## What a habitat owns

| Field | Desert (id 1, the terrarium) | Freshwater (id 2, the aquarium) | Why it is the habitat's |
|---|---|---|---|
| floor temperature profile, lit and dark | 31 → 25 °C warm side → cool side, 5 °C cooler at night | 25 °C everywhere, day and night (a heater) | the same gecko lives warmer in a desert setup than a jungle one would let it |
| basking spot: position, temperature while lit | 0.28 along the floor, 35 °C | none (no lamp: `bask_r_px` 0) | |
| hides: kind, position, temperature | warm, humid, cool | none | a jungle needs no humid hide; fish need none |
| humid hide: starting moisture, dry rate | 0.6, 1/10,800 s | (no humid hide) | |
| `humidity` baseline 0..1 | 0.2 | 1.0 (water) | data only in v1; later it eases sheds in humid habitats |
| backdrop, substrate, plants (P3) | sand, rock, succulents | the engine's grass, decor and critters, as they always were | |
| upkeep chores (P5) | glass, latrine, humid hide | glass, grass | a jungle mists the whole enclosure instead |
| climbing surfaces (later) | none | none | branches arrive with the jungle and the climbers |

## Rules (`--selftest-packs`, `habitat_check`)

- Every habitat: a name, an id in 1..31, a world, a basking spot and hides on the floor, hides at least 0.2 of the
  floor apart and never warmer dark than lit, moisture and humidity in 0..1.
- A terrarium's: the floor's warm end warmer than its cool end, a lamp that warms its spot, at least one hide and a
  humid one among them (a lizard needs it to shed) - thermoregulation is possible. An aquarium's floor may be even,
  and a lamp and hides are its own business (freshwater has neither).
- Every habitat's lit floor reaches each compatible species' comfort band (`t_lo`..`t_hi`) somewhere.
- A tank never holds an incompatible (habitat, species) pair, nor another world's: `tank_set_packs` refuses both, and
  a save of such a pair loads as the world's defaults (`docs/species-pack.md` "Where packs live").

## Planned habitats

| Habitat | Species | Needs |
|---|---|---|
| desert | leopard gecko, bearded dragon, monitor | v1 (now) |
| freshwater (aquarium) | pocket-tank's fish | P2b (now): today's fish tank as a pack |
| jungle | crested gecko, chameleon | climbing (`climb` goal: schema change + retrain), branches, whole-enclosure misting |

## Worlds: one core, a bin per world (PLAN.md D11)

Every habitat and species carries a `world`: `WORLD_AQUARIUM` or
`WORLD_TERRARIUM`, and a compatible pair shares one. The code base is one
reusable core; a build picks its world (`PT_WORLD` in `common/world.h`, P2b,
set when the bin is built - never at run time), and with it the model and
tokenizer (world.h THE BRAIN), the packs a tank may hold and a new tank's pair,
the goal set (`tank.h` THE GOALS: the fish's eight or the lizard's seven), the
locomotion (the swim, or `tank.h` THE WALK on the floor), the toolbox (sponge
and scissors, or those and the mister) and the selftest list. One firmware
cannot carry both brains: the model partition is 8 MB (`firmware/partitions.csv`,
`model` at 0x290000, size 0x800000) and a model is 7.56 MB.

| World | Bin | Model | Habitats |
|---|---|---|---|
| terrarium (the default) | the lizards, walking | the lizard model (schema L1); until P6 the v4 fish model over `advisor_core.c`'s TEMPORARY FISH-MODEL BRIDGE | desert (v1), jungle (planned) |
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
world.h names for the world (`PT_FW_TOKENIZER_*`) as `tokenizer.bin`. The host test `firmware/host_test/decide_host.c`
is the fish model's: build it with `-DPT_WORLD=1` (its header has the line).
