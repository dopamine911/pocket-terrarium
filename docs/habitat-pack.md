# Habitat packs — DRAFT

The enclosure is a pack too, separate from the species (`docs/species-pack.md`).
A tank holds one habitat and one species, and the species must list the habitat
as compatible. The engine reads both; neither the engine nor the model names a
habitat.

## Where they live

- `common/habitat.h`: `habitat_t`, `habitat_get(id)`.
- `common/habitat/<name>.c`: one `const habitat_t` per habitat, compiled in.
- The save stores `habitat_id` beside the species `pack_id`.

## What a habitat owns

| Field | Desert (id 1, v1) | Why it is the habitat's |
|---|---|---|
| floor temperature profile, lit and dark | warm side → cool side, cooler at night | the same gecko lives warmer in a desert setup than a jungle one would let it |
| basking spot: position, temperature while lit | warm end, hottest point | |
| hides: kind, position, temperature | warm, humid, cool | a jungle needs no humid hide |
| humid hide: starting moisture, dry rate | 0.6, 1/10,800 s | |
| `humidity` baseline 0..1 | 0.2 | data only in v1; later it eases sheds in humid habitats |
| backdrop, substrate, plants (P3) | sand, rock, succulents | |
| upkeep chores (P5) | glass, latrine, humid hide | a jungle mists the whole enclosure instead |
| climbing surfaces (later) | none | branches arrive with the jungle and the climbers |

## Rules (`--selftest-packs`)

- Every habitat's lit floor reaches each compatible species' comfort band
  (`t_lo`..`t_hi`) somewhere, and something cooler than `t_lo` somewhere else, so
  thermoregulation is possible.
- A tank never holds an incompatible (habitat, species) pair.

## Planned habitats

| Habitat | Species | Needs |
|---|---|---|
| desert | leopard gecko, bearded dragon, monitor | v1 (now) |
| jungle | crested gecko, chameleon | climbing (`climb` goal: schema change + retrain), branches, whole-enclosure misting |

## Worlds: one core, a bin per world (PLAN.md D11)

Every habitat and species carries a `world`: `WORLD_AQUARIUM` or
`WORLD_TERRARIUM`, and a compatible pair shares one. The code base is one
reusable core; a build picks its world (`PT_WORLD`, P2b), and with it the model
and tokenizer, the packs on offer, the goal set and the locomotion (swimming or
walking). One firmware cannot carry both brains: the model partition is 8 MB
(`firmware/partitions.csv`, `model` at 0x290000, size 0x800000) and a model is
7.56 MB.

| World | Bin | Model | Habitats (planned) |
|---|---|---|---|
| terrarium | this port | the lizard model (schema L1) | desert (v1), jungle |
| aquarium | pocket-tank's fish on the same packs | the fish model (schema v4) | freshwater (today's tank), reef, jellyfish |

Jellyfish drift rather than decide; in an aquarium habitat they would be a
critter (like the shrimp), not a model-driven species.
