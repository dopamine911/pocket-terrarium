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
 * Every pack is compiled into both bins; a tank holds only its build's world's
 * (world.h PT_WORLD, tank.h tank_set_packs).
 *
 * All numbers are in the pack's own world units: degrees C, seconds of tended
 * time (the progression clock: awake in full, asleep at SLEEP_GROWTH_FRAC),
 * pixels at the 1.8's scale. */
#ifndef SPECIES_H
#define SPECIES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "world.h"

typedef enum { WORLD_AQUARIUM, WORLD_TERRARIUM } world_t;
#define PT_WORLD_ID (PT_TERRARIUM ? WORLD_TERRARIUM : WORLD_AQUARIUM)   /* this build's world (world.h) as a pack names it */
/* how the light maps to the model's phase (species_phase, below) */
typedef enum { SCHED_DAY, SCHED_DUSK, SCHED_NIGHT } sched_t;
typedef enum { PLAN_LIZARD, PLAN_FISH } body_plan_t;      /* the lizard: P3's draw_lizard; the fish: render.c's draw_fish */
typedef enum { SHED_WHOLE, SHED_PATCHES } shed_style_t;
enum { DIET_CRICKET = 1u << 0, DIET_MEALWORM = 1u << 1, DIET_GREENS = 1u << 2, DIET_PELLET = 1u << 3 };

/* the schema's seven goals (model/schema-lizard.md "Goals"), as bits of
 * species_t.goals: what a pack's lizards do. A goal past them is a schema
 * change (a climber needs `climb`): species_check refuses such a pack until
 * the schema grows the goal. tank.h's goal_id_t holds them after the fish's
 * eight (GOAL_LZ(LZ_HUNT) ...). */
enum { LZ_HUNT, LZ_BASK, LZ_HIDE, LZ_EXPLORE, LZ_INVESTIGATE, LZ_DIG, LZ_REST, LZ_GOALS,
       LZ_CLIMB = LZ_GOALS };                             /* (not in the schema: chameleons, crested geckos) */
#define LZ_SCHEMA_GOALS ((1u << LZ_GOALS) - 1)
extern const char *const LZ_GOAL_NAMES[LZ_GOALS];         /* the schema's goal words, LZ_* order */
/* ... and the fish's eight (schema v2-v4, tank.h GOAL_SEEK_FOOD .. GOAL_INSPECT_REEF): an aquarium pack's goals */
#define FISH_GOALS 8
#define FISH_SCHEMA_GOALS ((1u << FISH_GOALS) - 1)

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

    /* drives (per second awake unless noted; 0..10). Live in the terrarium since P2b (tank.c THE WALK): hunger
     * rises hunger_per_s (x SHED_HUNGER_X in a shed), energy is spent energy_per_s while it walks or digs and
     * comes back energy_rest_per_s while it lies still (resting, hidden, basking; slower cold), curiosity rises
     * curiosity_per_s while it neither explores nor investigates (those spend it), one catch takes
     * hunger_per_cricket off, and device sleep runs the sleep_* rates. The aquarium's fish swim on pocket-tank's
     * own constants (tank.h THE FISH'S DRIVES), which the fish pack carries as data: the same numbers by
     * construction (species/fish.c reads them) - its curiosity_per_s is the fish's rise while it explores */
    float       hunger_per_s, energy_per_s, energy_rest_per_s, curiosity_per_s;
    float       hunger_per_cricket; /* how much one catch takes off */
    float       sleep_hunger_per_h, sleep_energy_per_h;
    float       sleep_stress_per_h; /* a MAGNITUDE: stress falls by this an hour asleep (the doc's -2.0) */

    /* how it walks (P2b, a terrarium pack's: tank.c THE WALK; an aquarium pack swims and leaves them 0): its pace
     * by stage (px/s at the 1.8's scale), the stop-and-go - a bout of walking, then a pause, each drawn lo..hi
     * seconds - and the seconds a turn about on the spot takes (it stands meanwhile) */
    float       walk_px_s[4];
    float       bout_s[2], pause_s[2];
    float       turn_s;

    /* life */
    float       stage_s[3];         /* tended age to reach juv, adult, elder: starts a growth shed (docs/shed.md) */
    uint8_t     pop_cap;            /* <= POP_CAP (P5 reads it; the engine's cap is still POP_CAP) */
    bool        sheds;              /* it grows by shedding (docs/shed.md): the shed cycle runs, a stage's age starts a
                                       growth shed and the stage waits for it, the mister soaks it. false (the
                                       aquarium's fish): no shed at all - the stage changes at the age itself, as
                                       pocket-tank's fish always have, and the mister is no tool (the fields below 0) */
    float       shed_every_s[4];    /* per stage: from the end of one shed to the next pre-shed */
    float       shed_soon_s, shed_window_s;
    shed_style_t shed_style;
    uint8_t     diet;               /* DIET_* bits */

    /* the body, for render.c (P3) */
    body_plan_t plan;
    lizard_geom_t geom;
    const morph_t *morphs; uint8_t n_morphs;
    const pattern_t *pattern[4];    /* per stage: the gecko banded as a hatchling, spotted as an adult */

    uint16_t    goals;              /* the goals its reflex performs, a bit per goal of its world's set: LZ_* bits for a
                                       terrarium pack, the fish's eight (FISH_SCHEMA_GOALS) for an aquarium one */
} species_t;

#define SPECIES_GECKO     1         /* the leopard gecko (common/species/gecko.c) */
#define SPECIES_FISH      2         /* the aquarium's fish: pocket-tank's (common/species/fish.c) */
#define SPECIES_TEST_WARM 250       /* A16's test pack: the gecko with every t_* 6 C warmer */
#if PT_TERRARIUM
#define SPECIES_DEFAULT   SPECIES_GECKO   /* a new terrarium's */
#else
#define SPECIES_DEFAULT   SPECIES_FISH    /* a new aquarium's */
#endif
extern const species_t SPECIES_PACK_GECKO, SPECIES_PACK_TEST_WARM, SPECIES_PACK_FISH;

const species_t *species_get(uint8_t id);   /* NULL: no such pack */
int  species_count(void);                   /* every compiled-in pack - both worlds', the test pack too */
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
