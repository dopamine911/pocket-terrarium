/* species.h - the species packs (docs/species-pack.md, docs/ptpk.md).
 *
 * Everything that makes a leopard gecko a leopard gecko and not a bearded
 * dragon is a species pack: a PTPK file (docs/ptpk.md) that common/packs/ptpk.c
 * loads into a `species_t` of the registry below (species.c: a fixed pool, no
 * malloc). A built-in pack's source is a const species_t in C (common/species/
 * <name>.c), which the firmware encodes into such a file at boot
 * (common/packs/ptpk_write.c, builtin.c); other packs come in as files, from
 * a folder (the simulator) or the card. The engine (tank.c, progression.c,
 * the L1 encoder) reads the pack and never names a species.
 * Where it lives is a pack of its own, the habitat (habitat.h). A tank stores
 * both (tank_t.pack_id, tank_t.habitat_id: the registry's one-byte handles;
 * the save adds the packs' 32-bit uids, progression.c). --selftest-packs
 * checks every pack against species_check (the doc's "Validation") and every
 * habitat against it.
 *
 * What a species may live in is a question of CAPABILITIES (packs/caps.h, issue #4 phase 2), not of worlds: it says how it
 * moves (LOCOMOTION), what it needs of its surroundings (MEDIA_NEEDS), what it eats and which goals its reflex performs - by
 * the model's words - and a tank runs it only where everything it needs is provided by the habitat, the firmware and the
 * model (packs/compat.h packs_compatible, tank.h tank_set_packs). The species' own list of habitats stays authoritative:
 * the capability check is in addition. A world (world.h) is a preset and a filter - `worlds`, the WORLDS record: the shop
 * and the menus - and decides nothing here; a release image ships only its world's built-in packs because its firmware
 * links only those (builtin.h).
 *
 * All numbers are in the pack's own units: degrees C, seconds of tended
 * time (the progression clock: awake in full, asleep at SLEEP_GROWTH_FRAC),
 * pixels at the 1.8's scale. */
#ifndef SPECIES_H
#define SPECIES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include "world.h"
#include "packs/caps.h"

/* how the light maps to the model's phase (species_phase, below) */
typedef enum { SCHED_DAY, SCHED_DUSK, SCHED_NIGHT } sched_t;
typedef enum { PLAN_LIZARD, PLAN_FISH } body_plan_t;      /* the lizard: P3's draw_lizard; the fish: render.c's draw_fish */
typedef enum { SHED_WHOLE, SHED_PATCHES } shed_style_t;
enum { DIET_CRICKET = 1u << 0, DIET_MEALWORM = 1u << 1, DIET_GREENS = 1u << 2, DIET_PELLET = 1u << 3 };
_Static_assert(((caps_t)DIET_CRICKET << CAP_FOOD_SHIFT) == CAP_CRICKET && ((caps_t)DIET_MEALWORM << CAP_FOOD_SHIFT) == CAP_MEALWORM &&
               ((caps_t)DIET_GREENS << CAP_FOOD_SHIFT) == CAP_GREENS && ((caps_t)DIET_PELLET << CAP_FOOD_SHIFT) == CAP_PELLET,
               "a diet bit shifted by CAP_FOOD_SHIFT is the food's capability");

/* THE GOALS' WORDS. A model's goals are words (the manifest of packs/manifest.h lists them, in its own candidate order) and a
 * species names the ones its reflex performs by word, so no id of tank.h goes into a file. The lizard model's seven
 * (model/schema-lizard.md "Goals"; tank.h's goal_id_t holds them after the fish's eight, GOAL_LZ(LZ_HUNT) ...) and the fish
 * model's eight (schema v2-v4, tank.h GOAL_SEEK_FOOD .. GOAL_INSPECT_REEF), as initializer lists - a manifest and a built-in
 * species both take them - and as the arrays the engine reads (defined in packs/models.c, with the manifests). A goal past
 * these (a climber needs `climb`) is in no manifest until a model is trained on it: species_check refuses the word. */
enum { LZ_HUNT, LZ_BASK, LZ_HIDE, LZ_EXPLORE, LZ_INVESTIGATE, LZ_DIG, LZ_REST, LZ_GOALS };
#define LZ_GOAL_WORDS   "hunt", "bask", "hide", "explore", "investigate", "dig", "rest"
extern const char *const LZ_GOAL_NAMES[LZ_GOALS];         /* the lizard model's goal words, LZ_* order */
#define FISH_GOALS 8
#define FISH_GOAL_WORDS "seek_food", "flee_shadow", "visit_bubbles", "follow_friend", "explore", "rest", "dart_play", "inspect_reef"
extern const char *const FISH_GOAL_NAMES[FISH_GOALS];     /* the fish model's goal words, in tank.h's order (tank.c GOAL_NAMES holds the same) */

/* the model's phase (model/schema-lizard.md `phase`) */
enum { PHASE_ACTIVE, PHASE_SETTLE, PHASE_SLEEP, PHASE_N };
extern const char *const PHASE_NAMES[PHASE_N];            /* active settle sleep */

/* the body, for draw_lizard (render_lizard.h) - and its size for the walk (tank.c walk_margin: the body drawn stays inside the glass) */
typedef struct {
    float length_px;        /* nose to tail tip, adult, at the 1.8's scale */
    float head_w, limb_len; /* px */
    float tail_fat;         /* 0..1: how much of the fat reserve the tail shows */
    float eye_r;            /* px */
    bool  eyelids;          /* blinks (a leopard gecko has lids, most geckos do not) */
} lizard_geom_t;
/* a PLAN_LIZARD body's size by its stage, the adult's 1 (geom.length_px is the adult's), and half its length drawn - nose to
   the middle, the middle to the tail tip, facing sideways - at a scale (the stage's x its own build: tank.h tank_lizard_scale) */
float lizard_stage_scale(int stage);
static inline float lizard_half_len(const lizard_geom_t *g, float scale) { return 0.5f * g->length_px * scale; }
/* the walk: a foot on the ground LIZARD_DUTY of a cycle, swept back LIZARD_STRIDE x the limb meanwhile (render_lizard.c's
   legs) - so a cycle covers lizard_stride_px of ground with every planted foot still (tank.c THE LOOK steps the phase by it) */
#define LIZARD_DUTY   0.62f
#define LIZARD_STRIDE 0.45f
static inline float lizard_stride_px(const lizard_geom_t *g, float scale) { return LIZARD_STRIDE * g->limb_len * powf(scale, 0.9f) / LIZARD_DUTY; }
/* A pack holds everything inline - names are arrays, nothing points into the file it came from (docs/ptpk.md): the
   texts' sizes below are the format's limits plus the NUL */
#define PACK_NAME_MAX        24      /* a pack's name: 1..23 characters */
#define MORPH_NAME_MAX       12      /* a morph's or a pattern's name: 1..11 */
#define PATTERN_NAME_MAX     12
#define SPECIES_MORPHS_MAX    6      /* setup.h SETUP_MORPH_MAX: the MORPH page's tiles */
#define SPECIES_HABITATS_MAX  4
typedef struct { char name[MORPH_NAME_MAX]; uint32_t body, accent; } morph_t;   /* 0xRRGGBB */
typedef struct { char name[PATTERN_NAME_MAX]; } pattern_t;                      /* banded, spotted ... (P3 draws them) */
#define SPECIES_LOCO_MAX      3      /* a movement mode once each: swim, walk, slither */
#define SPECIES_GOALS_MAX     8      /* the most words a species or a manifest lists */
#define GOAL_NAME_MAX        16      /* a goal's word: 1..15 characters of [a-z0-9_], a letter first */
typedef struct { caps_t mode, area; } loco_t;   /* one way it moves (CAP_SWIM, CAP_WALK, CAP_SLITHER) and the area it moves in (CAP_WATER, CAP_LAND, ...) */

typedef struct {
    char        name[PACK_NAME_MAX];/* "LEOPARD GECKO" (shown on cards) */
    uint8_t     id;                 /* the registry's one-byte handle (tank_t.pack_id). A built-in pack's is the id old saves hold,
                                       never reused; any other pack is given a free one when it is loaded and it is never stored */
    bool        builtin;            /* shipped in the firmware: its id is the one a save may hold (progression.c writes 0 for the rest) */
    bool        test_only;          /* compiled in for the tests (A16's warm pack), never offered to a keeper */
    uint8_t     worlds;             /* the WORLDS record: the worlds whose shop and menus offer it (world.h PT_WORLD bits; 0: all). A filter, nothing more */
    /* what it needs of its surroundings (packs/compat.h species_needs adds them up): how it moves, and in which area (the
       LOCOMOTION record - one entry per mode, the walk's numbers below), the areas and fittings it cannot live without
       (media_needs) and the ones it makes use of when they are there (media_wants: known bits only), and its diet's foods */
    uint8_t     n_loco;
    loco_t      loco[SPECIES_LOCO_MAX];
    caps_t      media_needs, media_wants;
    uint32_t    uid;                /* the pack's identity for good: FNV-1a-32 of its uid string (docs/ptpk.md) - what a save stores */
    uint32_t    habitats;           /* bit (1u << habitat id): the habitats it can live in (habitat.h; ids 1..31). Derived from
                                       habitat_uid[] by the registry whenever a pack is added: never read from a file */
    uint8_t     n_habitat_uids;     /* the habitats it lists, by uid (the file's HABITATS record) */
    uint32_t    habitat_uid[SPECIES_HABITATS_MAX];

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

    /* how it walks (the WALK entry of LOCOMOTION - tank.c THE WALK; a pack that does not walk leaves them 0): its pace
     * by stage (px/s at the 1.8's scale), the stop-and-go - a bout of walking, then a pause, each drawn lo..hi
     * seconds - and the seconds a turn about on the spot takes (it stands meanwhile) */
    float       walk_px_s[4];
    float       bout_s[2], pause_s[2];
    float       turn_s;

    /* life */
    float       stage_s[3];         /* tended age to reach juv, adult, elder: starts a growth shed (docs/shed.md) */
    uint8_t     pop_cap;            /* <= POP_CAP: the most a keeper can earn or buy (progression_pop_cap: the arrival gates, the
                                       shop's hatchlings); POP_CAP stays the engine's hard cap (THE CAP's guard, a forced arrival) */
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
    morph_t     morphs[SPECIES_MORPHS_MAX]; uint8_t n_morphs;
    pattern_t   pattern[4];         /* per stage: the gecko banded as a hatchling, spotted as an adult */

    /* the goals its reflex performs, by word (the GOALS record): species_check holds each to the words of a model's manifest
       (manifest.h), packs_compatible to the words of the model this build runs. No bit, no id: an id would depend on a model's order */
    uint8_t     n_goals;
    char        goal[SPECIES_GOALS_MAX][GOAL_NAME_MAX];
} species_t;

/* the built-in packs' one-byte ids: the ones old saves hold - each const species_t's `.id`, and its file's LEGACY_ID record */
#define SPECIES_GECKO     1         /* the leopard gecko (common/species/gecko.c; pt.gecko.leopard) */
#define SPECIES_FISH      2         /* the aquarium's fish: pocket-tank's (common/species/fish.c; pt.fish.roster) */
#define SPECIES_TEST_WARM 250       /* A16's test pack: the gecko with every t_* 6 C warmer (common/species/gecko.c; pt.test.gecko-warm) */
#if PT_TERRARIUM
#define SPECIES_DEFAULT   SPECIES_GECKO   /* a new terrarium's */
#else
#define SPECIES_DEFAULT   SPECIES_FISH    /* a new aquarium's */
#endif

/* THE REGISTRY: the packs loaded so far - this build's built-in ones first (packs_init, common/packs/ptpk.c), then any
 * found at run time (the simulator's folder; the card, phase 4) - in a fixed pool, never malloc'd. The first call of any
 * of these loads the built-in ones if nothing has (the firmware calls packs_init at boot, before its tasks). */
const species_t *species_get(uint8_t id);   /* by the one-byte handle; NULL: no such pack */
const species_t *species_by_uid(uint32_t uid);   /* by the pack's uid; NULL: not loaded */
int  species_count(void);                   /* the packs loaded (a release image: those its firmware links; the simulator: both worlds') */
const species_t *species_at(int i);         /* in registry order: built-in packs, then the others in the order they came */
/* what the loader (common/packs/ptpk.c) fills it with. species_slot: the next free pool slot, zeroed (NULL: the pool is
 * full); the loader reads a pack's records into it and calls species_commit, which gives it its id (a built-in pack its
 * legacy_id, which must be free; any other the lowest free 3..249), derives `habitats` from `habitat_uid[]`, runs
 * species_check and registers it - false, with the reason in why, leaves the slot free. species_seen: what the registry
 * knows of a file by uid and body CRC - PACK_NEW, PACK_SAME (that very file is loaded) or PACK_UID_TAKEN (another pack
 * has the uid: *by is its name). species_drop_loaded takes out every pack that is not built in (a test's reset; a tank
 * holding one falls back to the defaults at its next tank_species) */
typedef enum { PACK_NEW, PACK_SAME, PACK_UID_TAKEN } pack_seen_t;
species_t *species_slot(void);
bool species_commit(species_t *slot, bool builtin, uint8_t legacy_id, uint32_t crc, char *why, size_t n);
pack_seen_t species_seen(uint32_t uid, uint32_t crc, const char **by);
void species_drop_loaded(void);
void species_relink(void);                  /* habitat.c: a habitat was added - every species' `habitats` mask again */
/* THE CARD LIBRARY's (issue #4 phase 4, packs/library.h; docs/card-library.md): species_room - a pool slot is free (a dry check
 * asks without taking one); species_admit - the rules species_commit runs once the pack has its id (this firmware's movement
 * modules, the habitats it lives in, species_check), on a species in the CALLER's memory: it derives that copy's `habitats`
 * and touches no pool; species_remove - a pack that is not built in, out of the pool (a REMOVE, or an UPDATE's old version):
 * the packs after it close up and every pack keeps its id (a tank's handle stays good), false for a built-in or unknown uid */
#if PT_CARD
bool species_room(void);
bool species_admit(species_t *p, bool builtin, char *why, size_t n);
bool species_remove(uint32_t uid);
#endif

/* the model's heat as a float 0..10 (before drive9), relative to the pack's
 * comfort band: 0 at or under t_cold, 0 -> 5 up to t_lo, 5 -> 7 across the
 * band (digits 5-6), 7 -> 10 up to t_hot, 10 from t_hot on (digit 9). The
 * Python twin is model/gen_traces.py heat_of: the same arithmetic, in order. */
float species_heat_of(const species_t *p, float body_c);
/* the model's phase under the pack's schedule: lit = the light is on, since_s
 * = seconds since the light last switched (either way) */
int  species_phase(const species_t *p, bool lit, float since_s);
/* docs/species-pack.md "Validation" for one pack; false with the first broken rule in why. The rules a pack keeps whatever its
 * world: the heat, the rates, the stages ..., and the capability ones - a movement mode this firmware has (caps.h firmware_caps),
 * the walk's numbers if it walks, the humid hide if it sheds, goals that some manifest this build carries lists
 * (manifest.h manifests_know_goal). Whether the pack can run with a habitat and the model of this build is packs_compatible's */
bool species_check(const species_t *p, char *why, size_t n);
#endif
