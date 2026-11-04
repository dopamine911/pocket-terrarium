# Species packs — DRAFT

A species pack is everything that makes a leopard gecko a leopard gecko and
not a bearded dragon. The engine (`common/tank.c`, `common/render.c`,
`common/progression.c`) and the model (`model/schema-lizard.md`) never name a
species; they read the pack. Adding a species that fits the seven goals is a
new pack file, no engine change and no retrain.

## Where packs live

- `common/species.h`: the `species_t` type and `species_get(id)`.
- `common/species/<name>.c`: one `const species_t` per species, compiled in.
  No runtime loading, so no file format to freeze.
- The save stores the pack **id** (a byte) per terrarium. v1 terrariums hold one
  species; mixed terrariums are out of scope (real keepers don't mix either).
- `--selftest-packs` (sim, P2) checks every pack against the rules in "Validation".

## The contract

All numbers are in the pack's own world units: °C, seconds of tended time
(the progression clock), pixels at the 1.8's scale.

```c
typedef enum { SCHED_DAY, SCHED_DUSK, SCHED_NIGHT } sched_t;
typedef enum { PLAN_LIZARD } body_plan_t;                 /* one renderer for now */
typedef enum { SHED_WHOLE, SHED_PATCHES } shed_style_t;

typedef struct {
    const char *name;               /* "LEOPARD GECKO" (shown on cards) */
    uint8_t     id;                 /* stored in the save; never reused */

    /* activity: how the clock and the light map to the model's phase */
    sched_t     schedule;
    float       settle_s;           /* length of the settle phase */

    /* heat: body temperature in degrees C -> the model's heat 0..9 */
    float       t_cold, t_lo, t_hi, t_hot;   /* see "Heat" below */
    float       tau_s[4];           /* body warms/cools toward its surroundings, per stage */

    /* drives (per second awake unless noted; 0..10 like the fish) */
    float       hunger_per_s, energy_per_s, energy_rest_per_s, curiosity_per_s;
    float       hunger_per_cricket; /* how much one catch takes off */
    float       sleep_hunger_per_h, sleep_energy_per_h, sleep_stress_per_h;

    /* life */
    float       stage_s[3];         /* tended age to reach juv, adult, elder */
    uint8_t     pop_cap;            /* <= POP_CAP */
    float       shed_every_s[4];    /* per stage (docs/shed.md) */
    float       shed_soon_s, shed_window_s;
    shed_style_t shed_style;
    uint8_t     diet;               /* DIET_CRICKET | DIET_MEALWORM | DIET_GREENS ... */

    /* the body, for render.c draw_lizard */
    body_plan_t plan;
    lizard_geom_t geom;             /* length, head shape, limb length, tail fat, eye size, eyelids */
    const morph_t *morphs; uint8_t n_morphs;
    const pattern_t *pattern[4];    /* per stage: gecko banded as a hatchling, spotted as an adult */

    /* how the reflex layer performs each goal */
    goal_style_t goal[GOAL_COUNT];  /* speed, urgency gain, strike range, dig rate ... */
} species_t;
```

The exact layout is settled in P2; the fields and their meaning are the contract.

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
heat < 3 slows movement (× 0.5 at heat 0) and spoils strikes; heat ≥ 8 adds stress.

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
Boredom keeps the fish numbers for every pack (`BORED_*`, `tank.h:975`): it is
about how a pastime feels, not about the species.

## The leopard gecko pack (v1 values, tuned in P2)

| Field | Value | Why |
|---|---|---|
| name | LEOPARD GECKO | |
| schedule | `SCHED_DAY` | D2: not species-specific yet (real: crepuscular) |
| settle_s | 600 | ten minutes of winding down after lights-out |
| t_cold / t_lo / t_hi / t_hot | 20 / 28 / 32 / 38 °C | keepers aim for a 28–32 warm side; under 20 they stop eating; over 38 is dangerous |
| tau_s (hatch, juv, adult, elder) | 60, 90, 120, 150 | a hatchling warms in about a minute |
| hunger_per_s | 0.0065 | fed → starving in ~25 min awake (fish: ~17) |
| hunger_per_cricket | 2.0 | about four crickets from starving to full |
| energy_per_s / energy_rest_per_s | 0.006 / 0.05 | |
| curiosity_per_s | 0.004 | |
| sleep_*_per_h | hunger +0.6, energy +2.0, stress −2.0 | the fish's numbers, hunger slower |
| stage_s | juv 1,200, adult 5,400, elder 28,800 | the fish's 20 min / 90 min / 8 h; stages change at a shed (`docs/shed.md`) |
| pop_cap | 3 | solitary; three already crowds a low-social lizard |
| shed_every_s (hatch, juv, adult, elder) | 900, 1,800, 10,800, 21,600 | from the end of one shed to the next pre-shed; hatchlings shed often |
| shed_soon_s / shed_window_s | 300 / 600 | |
| shed_style | `SHED_WHOLE` | geckos shed in one piece and eat it |
| diet | crickets, mealworms | |
| morphs | normal, tangerine, albino, blizzard, mack snow | |
| pattern | banded (hatch, juv) → spotted (adult, elder) | real: hatchling bands break into spots |

These are starting values, not measurements. P2 tunes them in the sim against
A13–A15 (`model/schema-lizard.md`) and against "does it feel alive at the desk".

## Validation (`--selftest-packs`)

For every pack:
- `t_cold < t_lo < t_hi < t_hot`, and the comfort band is at least 2 °C wide.
- Every rate > 0; `stage_s` strictly increasing; `shed_every_s[s] ≥ shed_soon_s + shed_window_s` (a lizard spends
  at most half its life in a shed cycle).
- `1 ≤ pop_cap ≤ POP_CAP`; `id` unique across packs; `n_morphs ≥ 1`.
- The heat map is monotonic and hits 5.0 at `t_lo` and 7.0 at `t_hi`.
- A pack whose goals do not fit the schema's seven (a climber) is refused until
  the schema grows a goal.

## Not in v1

Mixed-species terrariums; climbing and perching (chameleon, crested gecko);
clock-based day for `SCHED_DUSK`/`SCHED_NIGHT`; per-pack sounds.
