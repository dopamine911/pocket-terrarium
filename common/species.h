/* species.h - the species packs (docs/species-pack.md).
 *
 * Everything that makes a leopard gecko a leopard gecko and not a bearded
 * dragon is a compiled-in `const species_t` (common/species/<name>.c); the
 * engine (tank.c, progression.c, the L1 encoder) reads the pack and never
 * names a species. Where it lives is a pack of its own, the habitat
 * (habitat.h). A tank stores both ids (tank_t.pack_id, tank_t.habitat_id;
 * saved). --selftest-packs checks every pack against species_check (the
 * doc's "Validation") and every habitat against it.
 *
 * The engine is one core for two worlds: a WORLD_AQUARIUM build (fish) and a
 * WORLD_TERRARIUM build (lizards); a species and its habitat share a world.
 * Only terrarium packs exist so far (the per-world build is P2b).
 *
 * All numbers are in the pack's own world units: degrees C, seconds of tended
 * time (the progression clock: awake in full, asleep at SLEEP_GROWTH_FRAC),
 * pixels at the 1.8's scale. */
#ifndef SPECIES_H
#define SPECIES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { WORLD_AQUARIUM, WORLD_TERRARIUM } world_t;
/* how the light maps to the model's phase (species_phase, below) */
typedef enum { SCHED_DAY, SCHED_DUSK, SCHED_NIGHT } sched_t;
typedef enum { PLAN_LIZARD } body_plan_t;                 /* one renderer for now (P3's draw_lizard) */
typedef enum { SHED_WHOLE, SHED_PATCHES } shed_style_t;
enum { DIET_CRICKET = 1u << 0, DIET_MEALWORM = 1u << 1, DIET_GREENS = 1u << 2 };

/* the schema's seven goals (model/schema-lizard.md "Goals"), as bits of
 * species_t.goals: what a pack's lizards do. A goal past them is a schema
 * change (a climber needs `climb`): species_check refuses such a pack until
 * the schema grows the goal. */
enum { LZ_HUNT, LZ_BASK, LZ_HIDE, LZ_EXPLORE, LZ_INVESTIGATE, LZ_DIG, LZ_REST, LZ_GOALS,
       LZ_CLIMB = LZ_GOALS };                             /* (not in the schema: chameleons, crested geckos) */
#define LZ_SCHEMA_GOALS ((1u << LZ_GOALS) - 1)
extern const char *const LZ_GOAL_NAMES[LZ_GOALS];         /* the schema's goal words, LZ_* order */

/* the model's phase (model/schema-lizard.md `phase`) */
enum { PHASE_ACTIVE, PHASE_SETTLE, PHASE_SLEEP, PHASE_N };
extern const char *const PHASE_NAMES[PHASE_N];            /* active settle sleep */

/* the body, for P3's draw_lizard: plain data, nothing reads it yet */
typedef struct {
    float length_px;        /* nose to tail tip, adult, at the 1.8's scale */
    float head_w, limb_len; /* px */
    float tail_fat;         /* 0..1: how much of the fat reserve the tail shows */
    float eye_r;            /* px */
    bool  eyelids;          /* blinks (a leopard gecko has lids, most geckos do not) */
} lizard_geom_t;
typedef struct { const char *name; uint32_t body, accent; } morph_t;   /* 0xRRGGBB */
typedef struct { const char *name; } pattern_t;                         /* banded, spotted ... (P3 draws them) */

typedef struct {
    const char *name;               /* "LEOPARD GECKO" (shown on cards) */
    uint8_t     id;                 /* stored in the save; never reused */
    bool        test_only;          /* compiled in for the tests (A16's warm pack), never offered to a keeper */
    world_t     world;              /* the build it belongs to; its habitats share it */
    uint32_t    habitats;           /* bit (1u << habitat id): the habitats it can live in (habitat.h; ids 1..31) */

    /* activity: how the light maps to the model's phase */
    sched_t     schedule;
    float       settle_s;           /* length of the settle phase */

    /* heat: body temperature in degrees C -> the model's heat 0..9 (species_heat_of) */
    float       t_cold, t_lo, t_hi, t_hot;
    float       tau_s[4];           /* the body warms/cools toward its surroundings, per stage */

    /* drives (per second awake unless noted; 0..10). Data for now: P2a's lizards
     * still run the fish's drive rates (tank.c); the switch to these is P2b/P2c */
    float       hunger_per_s, energy_per_s, energy_rest_per_s, curiosity_per_s;
    float       hunger_per_cricket; /* how much one catch takes off */
    float       sleep_hunger_per_h, sleep_energy_per_h;
    float       sleep_stress_per_h; /* a MAGNITUDE: stress falls by this an hour asleep (the doc's -2.0) */

    /* life */
    float       stage_s[3];         /* tended age to reach juv, adult, elder: starts a growth shed (docs/shed.md) */
    uint8_t     pop_cap;            /* <= POP_CAP (P5 reads it; the engine's cap is still POP_CAP) */
    float       shed_every_s[4];    /* per stage: from the end of one shed to the next pre-shed */
    float       shed_soon_s, shed_window_s;
    shed_style_t shed_style;
    uint8_t     diet;               /* DIET_* bits */

    /* the body, for render.c (P3) */
    body_plan_t plan;
    lizard_geom_t geom;
    const morph_t *morphs; uint8_t n_morphs;
    const pattern_t *pattern[4];    /* per stage: the gecko banded as a hatchling, spotted as an adult */

    uint16_t    goals;              /* LZ_* bits: the goals its reflex performs (P2b tunes how) */
} species_t;

#define SPECIES_GECKO     1         /* the leopard gecko (common/species/gecko.c) */
#define SPECIES_TEST_WARM 250       /* A16's test pack: the gecko with every t_* 6 C warmer */
#define SPECIES_DEFAULT   SPECIES_GECKO
extern const species_t SPECIES_PACK_GECKO, SPECIES_PACK_TEST_WARM;

const species_t *species_get(uint8_t id);   /* NULL: no such pack */
int  species_count(void);                   /* every compiled-in pack, the test pack too */
const species_t *species_at(int i);

/* the model's heat as a float 0..10 (before drive9), relative to the pack's
 * comfort band: 0 at or under t_cold, 0 -> 5 up to t_lo, 5 -> 7 across the
 * band (digits 5-6), 7 -> 10 up to t_hot, 10 from t_hot on (digit 9). The
 * Python twin is model/gen_traces.py heat_of: the same arithmetic, in order. */
float species_heat_of(const species_t *p, float body_c);
/* the model's phase under the pack's schedule: lit = the light is on, since_s
 * = seconds since the light last switched (either way) */
int  species_phase(const species_t *p, bool lit, float since_s);
/* docs/species-pack.md "Validation" for one pack; false with the first broken
 * rule in why */
bool species_check(const species_t *p, char *why, size_t n);
#endif
