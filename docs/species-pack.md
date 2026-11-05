# Species packs — DRAFT

A species pack is everything that makes a leopard gecko a leopard gecko and
not a bearded dragon - or pocket-tank's fish a fish. The engine
(`common/tank.c`, `common/render.c`, `common/progression.c`) and the model
(`model/schema-lizard.md`) never name a species; they read the pack. Adding a
species that fits its world's goals is a new pack file, no engine change and
no retrain.

## Where packs live

- `common/species.h`: the `species_t` type and `species_get(id)`; `common/species.c` the registry and the rules.
- `common/species/<name>.c`: one `const species_t` per species, compiled in.
  No runtime loading, so no file format to freeze. Today: `gecko.c` (the leopard gecko, id 1, and A16's test pack,
  id 250, never offered to a keeper) and `fish.c` (the aquarium's fish, id 2, P2b).
- Every pack carries a **world** (`WORLD_TERRARIUM` or `WORLD_AQUARIUM`, PLAN.md D11) and is compiled into both
  worlds' bins, but a tank holds only its build's world's (`tank_set_packs` refuses the other world's pairs:
  `docs/habitat-pack.md` "Worlds"). A new tank holds its world's default pair: the gecko in the desert, or the fish in
  freshwater (`SPECIES_DEFAULT`, `HABITAT_DEFAULT`).
- The save stores the pack **id** (a byte) per tank beside the habitat's, and each animal's heat and shed (the
  tail at 1956..2095). A tank holds one species; mixed terrariums are out of scope (real keepers don't mix either).
  A save whose pair this bin cannot hold (an id it lacks, a pair that does not fit, another world's) loads as the
  defaults, and its animals start fresh: the tail's heat and sheds are that pack's.
- `--selftest-packs` (sim, both worlds) checks every pack against the rules in "Validation".

## The contract

All numbers are in the pack's own world units: °C, seconds of tended time
(the progression clock), pixels at the 1.8's scale.

```c
typedef enum { WORLD_AQUARIUM, WORLD_TERRARIUM } world_t;
typedef enum { SCHED_DAY, SCHED_DUSK, SCHED_NIGHT } sched_t;
typedef enum { PLAN_LIZARD, PLAN_FISH } body_plan_t;     /* P3's draw_lizard; render.c's draw_fish */
typedef enum { SHED_WHOLE, SHED_PATCHES } shed_style_t;

typedef struct {
    const char *name;               /* "LEOPARD GECKO" (shown on cards) */
    uint8_t     id;                 /* stored in the save; never reused */
    bool        test_only;          /* compiled in for the tests (A16's warm pack), never offered */
    world_t     world;              /* the bin it belongs to; its habitats share it */
    uint32_t    habitats;           /* bit (1u << habitat id): where it can live (ids 1..31) */

    /* activity: how the light maps to the model's phase */
    sched_t     schedule;
    float       settle_s;           /* length of the settle phase */

    /* heat: body temperature in degrees C -> the model's heat 0..9 */
    float       t_cold, t_lo, t_hi, t_hot;   /* see "Heat" below */
    float       tau_s[4];           /* body warms/cools toward its surroundings, per stage */

    /* drives (per second awake unless noted; 0..10 like the fish) */
    float       hunger_per_s, energy_per_s, energy_rest_per_s, curiosity_per_s;
    float       hunger_per_cricket; /* how much one catch takes off */
    float       sleep_hunger_per_h, sleep_energy_per_h, sleep_stress_per_h;

    /* the walk (a terrarium pack's, P2b: tank.h THE WALK; an aquarium pack swims and leaves them 0) */
    float       walk_px_s[4];       /* pace per stage, px/s at the 1.8's scale */
    float       bout_s[2], pause_s[2];   /* stop-and-go: a bout of walking, then a stop, each lo..hi s */
    float       turn_s;             /* a turn about on the spot (it stands meanwhile) */

    /* life */
    float       stage_s[3];         /* tended age to reach juv, adult, elder */
    uint8_t     pop_cap;            /* <= POP_CAP */
    bool        sheds;              /* grows by shedding (docs/shed.md); false: the stage at the age itself */
    float       shed_every_s[4];    /* per stage (docs/shed.md); 0 for a pack that does not shed */
    float       shed_soon_s, shed_window_s;
    shed_style_t shed_style;
    uint8_t     diet;               /* DIET_CRICKET | DIET_MEALWORM | DIET_GREENS | DIET_PELLET */

    /* the body, for render.c */
    body_plan_t plan;
    lizard_geom_t geom;             /* length, head shape, limb length, tail fat, eye size, eyelids */
    const morph_t *morphs; uint8_t n_morphs;
    const pattern_t *pattern[4];    /* per stage: gecko banded as a hatchling, spotted as an adult */

    /* the goals its reflex performs: a bit per goal of its world's set (the lizard's seven, LZ_*;
       the fish's eight, FISH_SCHEMA_GOALS) */
    uint16_t    goals;
} species_t;
```

The layout above is `common/species.h`'s (P2a, the walk and the world P2b); the fields and their meaning are the
contract. How a goal is performed is the engine's per world (tank.h THE WALK for the terrarium, the swim for the
aquarium), read from the pack's numbers.

## Normalisation (the only bridge between a pack and the model)

### Heat

Body temperature `T` follows its surroundings: `dT/dt = (T_env − T) / tau`, with
`T_env` from the heat field (P2) and `tau` from the pack by stage (small bodies
change faster). The model's `heat` is a piecewise-linear map of `T`:

| T | heat (float, before `drive9`) |
|---|---|
| ≤ `t_cold` | 0 |
| `t_cold` → `t_lo` | 0 → 5 |
| `t_lo` → `t_hi` (the comfort band) | 5 → 7 (digits 5–6) |
| `t_hi` → `t_hot` | 7 → 10 (digits 7–9) |
| ≥ `t_hot` | 10 (digit 9) |

So digits 5–6 always mean "comfortable for this species", 0 "as cold as it can
stand", 9 "dangerously hot". Effects in the reflex layer, also normalised:
heat < 3 slows movement (× 0.5 at heat 0, rising straight to × 1 at heat 3) and spoils strikes; heat ≥ 8 adds
stress. As built in P2a (`tank.h` HEAT_*): the engine's stress settles by itself at ~0.18 a second, so the
stress is a pull toward 4 + 2 × (heat − 8) at 0.15 a second (it settles near 3 at heat 8, 7 at heat 10).
`T_env` is the habitat's (`common/habitat.h`: the enclosure is a pack of its own, apart from the species - the
floor's profile and the air's rise toward the lamp, the ground (its rocks and ramps, `docs/habitat-pack.md` "The
ground"), the basking spot up on the high rock, the hides, the humid hide's moisture; the desert's numbers are in
`common/habitat/desert.c`), and a species lists the habitats it lives in (`species_t.habitats`, same `world`).

### Phase

| Schedule | `active` | `settle` | `sleep` |
|---|---|---|---|
| `SCHED_DAY` (all v1 packs, D2) | light on | the first `settle_s` after the light goes off | the rest of the dark |
| `SCHED_DUSK` (later) | the first and last hours of light, and the first hours of dark | between | the middle of the day |
| `SCHED_NIGHT` (later) | dark | the first `settle_s` after the light comes on | light |

The fish tank's own night (`time night`) follows the light, set by the keeper's
double tap or the idle rule (`docs/progression.md`); `SCHED_DAY` keeps exactly that.
The other schedules need a clock-based day, which is a P5+ question.

### Drives

Rates come from the pack; the encoder only ever sees 0..10 clamped to 0..9.
Boredom keeps the fish numbers for every pack (`BORED_*`, `tank.h`): it is
about how a pastime feels, not about the species. The terrarium's lizards live
on their pack's rates (P2b, `tank.h` THE WALK); the aquarium's fish swim on
pocket-tank's own constants (`tank.h` THE FISH'S DRIVES), which the fish pack
reads into its fields, so the pack and the swim cannot drift apart.

## The leopard gecko pack (v1 values, tuned in P2)

| Field | Value | Why |
|---|---|---|
| name | LEOPARD GECKO | |
| schedule | `SCHED_DAY` | D2: not species-specific yet (real: crepuscular) |
| settle_s | 600 | ten minutes of winding down after lights-out |
| t_cold / t_lo / t_hi / t_hot | 20 / 28 / 32 / 38 °C | keepers aim for a 28–32 warm side; under 20 they stop eating; over 38 is dangerous |
| tau_s (hatch, juv, adult, elder) | 60, 90, 120, 150 | a hatchling warms in about a minute |
| hunger_per_s | 0.0045 | fed → starving in ~33 min awake (fish: ~17) |
| hunger_per_cricket | 3.0 | three crickets from starving to full; ~5–6 crickets an awake hour to hold steady |
| energy_per_s / energy_rest_per_s | 0.006 / 0.05 | |
| curiosity_per_s | 0.004 | |
| sleep_*_per_h | hunger rises 0.6, energy rises 2.0, stress falls 2.0 (all stored as positive rates) | the fish's numbers, hunger slower |
| walk_px_s (hatch, juv, adult, elder) | 24, 22, 18, 15 | unhurried: the 1.8's 388 px floor in about half a minute, pauses and all |
| bout_s / pause_s / turn_s | 0.8–3 s / 0.5–2.5 s / 0.7 s | a stalker's stop-and-go; it turns about on the spot |
| stage_s | juv 1,200, adult 5,400, elder 28,800 | 20 min / 90 min / 8 h, a POC's pace (the fish's are 2 h / 12 h / 96 h since pocket-tank 0.3.5, `progression.h`); reaching one starts a growth shed and the stage changes when it comes off clean (`docs/shed.md`) |
| pop_cap | 3 | solitary; three already crowds a low-social lizard |
| sheds | yes | a stage's age starts a growth shed; the stage changes when it comes off clean |
| shed_every_s (hatch, juv, adult, elder) | 900, 1,800, 10,800, 21,600 | from the end of one shed to the next pre-shed; hatchlings shed often |
| shed_soon_s / shed_window_s | 300 / 600 | |
| shed_style | `SHED_WHOLE` | geckos shed in one piece and eat it |
| diet | crickets, mealworms | |
| morphs | normal, tangerine, albino, blizzard, mack snow | |
| pattern | banded (hatch, juv) → spotted (adult, elder) | real: hatchling bands break into spots |

These are starting values, not measurements. P2 tunes them in the sim against
A13–A15 (`model/schema-lizard.md`) and against "does it feel alive at the desk".

## The fish pack (the aquarium's, P2b)

`common/species/fish.c`, id 2: pocket-tank's fish as a pack, so the aquarium's bin is today's fish tank on the same
core. Everything in it is what the fish already were; nothing here retunes them.

| Field | Value | Why |
|---|---|---|
| name, world, habitats | FISH, `WORLD_AQUARIUM`, freshwater | `docs/habitat-pack.md` |
| schedule / settle_s | `SCHED_DAY` / 600 | the fish model sees day and night, not the phase |
| t_cold / t_lo / t_hi / t_hot | 18 / 22 / 28 / 32 °C | a tropical fish: the water's 25 °C is heat 6, so no heat effect ever touches a fish |
| tau_s | 60, 90, 120, 150 | (the body follows the even water: it never leaves heat 6) |
| drives | `tank.h` THE FISH'S DRIVES: hunger 0.010/s, energy 0.012/s swimming and 0.30/s back resting, curiosity 0.08/s exploring, a pellet 4.3, asleep 0.8 / 2.0 / 2.0 an hour | pocket-tank's numbers (the swim runs on them) |
| walk_* | 0 | it swims (tank.c `update_fish`) |
| stage_s | 7,200, 43,200, 345,600 | 2 h / 12 h / 96 h of tended time, pocket-tank 0.3.5 |
| pop_cap | `POP_CAP` (5) | the keeper's cap |
| sheds | no | the stage changes at the age itself, pocket-tank's growing exactly; no shed clock, no mister (the toolbox keeps the sponge and the scissors in halves) |
| diet / plan | pellets / `PLAN_FISH` | render.c's `draw_fish` |
| morphs / pattern | one (the looks are the roster's and the genes', 0.4) / plain fry, bars from juvenile | |
| goals | the fish's eight (schema v2–v4) | |

## Validation (`--selftest-packs`)

For every pack, of both worlds (`species_check`):
- A name, a world, and at least one habitat (ids from 1).
- `t_cold < t_lo < t_hi < t_hot`, and the comfort band is at least 2 °C wide.
- Every rate > 0; `stage_s` strictly increasing; `SCHED_DAY` (the others need a clock-based day).
- A pack that sheds: every shed time > 0 and `shed_every_s[s] ≥ shed_soon_s + shed_window_s` (a lizard spends at
  most half its life in a shed cycle). A pack that does not shed (the fish) has no shed times to keep.
- A terrarium pack walks: every `walk_px_s` > 0, `bout_s` and `pause_s` spans of seconds, `turn_s` > 0.
- `1 ≤ pop_cap ≤ POP_CAP`; `id` unique across packs; `n_morphs ≥ 1`; a pattern for every stage.
- The heat map is monotonic and hits 0, 5.0, 7.0 and 10 at `t_cold`, `t_lo`, `t_hi`, `t_hot`.
- Its goals are of its own world's set: a terrarium pack whose goals do not fit the schema's seven (a climber) is
  refused until the schema grows a goal; an aquarium pack keeps to the fish's eight.

And for the pairs: a habitat a species lists is of its world; every fitting pair's lit floor reaches the species'
band; a tank holds every fitting pair of its build's world and refuses the other world's (`docs/habitat-pack.md`).

## Not in v1

Mixed-species terrariums; climbing and perching (chameleon, crested gecko);
clock-based day for `SCHED_DUSK`/`SCHED_NIGHT`; per-pack sounds.
