/* world_ops.h - THE WORLD MODULE's contract (issue #4 phase 3, slice D): everything the engine files (progression.c, setup.c,
 * notice.c, shelf.c, advisor.c) used to decide with `PT_TERRARIUM`, as one descriptor per world.
 *
 * A world's descriptor is defined in common/worlds/<world>/world.c (with its helpers beside it: milestones.c, shop.c,
 * fry.c, rules.c, setup.c) and the engine reaches it through world_cur() (world.h) - never by asking which world it is.
 * Today a bin has ONE world compiled in (sim/Makefile and tankcore/CMakeLists.txt glob the files of common/worlds/<PT_WORLD>/), so
 * world_cur() is that world's own definition; the step that builds both into one image replaces world_cur() with the
 * pick of a tank's world (its species' and habitat's WORLDS filter) and nothing else here changes.
 *
 * Rules of the descriptor: data first (names, words, masks, a style flag), a hook only where the world RUNS something
 * (a gate table, a purchase's effect, a scan of what an animal did); a hook gets everything it reads as arguments and
 * reaches the engine only through public headers (tank.h, progression.h, advisor.h). */
#ifndef WORLD_OPS_H
#define WORLD_OPS_H
#include "world.h"
#include "tank.h"
#include "progression.h"

struct setup_world_s;   /* setup_world.h: the first-run flow's pages and words, per world */
struct save_s;          /* save_layout.h: the frozen save */

/* how a staged arrival (a fry, a hatchling) comes into the tank (progression.c): COURT = the parents court in the nursery
 * and the fry is born there after the dance; HATCH = it comes out of its place a little after the last gate closed, no
 * dance, and the tank's badges that physics sets are watched for saving */
typedef enum { ARRIVAL_COURT = 0, ARRIVAL_HATCH = 1 } arrival_style_t;

/* ---- the fry's care gates (progression.c care_gates) ---- */
#define CARE_GATES_MAX 4                 /* the population's three + the glass */
typedef struct { int kind; float have, need, frac; bool met; } care_gate_t;
/* what the engine knows that a gate table reads: the lowest trust, the youngest's age (tank seconds) and the meals the
   next fry waits for since a fish was sold (0 = none sold) */
typedef struct { float min_trust; float age; int sale_need; } care_ctx_t;

/* ---- what an animal did this tick (progression_tick): the badges the world detects from it ---- */
typedef struct {
    uint32_t ms;            /* fish_t.ms_bits to set (MS_*) */
    uint32_t tms;           /* tank_ms_bits to set (TMS_*) */
    bool     playing;       /* it is at its pastime (counts toward TMS_FIRST_PLAY_SESSION) */
    bool     reef_visit;    /* it chose to look at the landmark the tank has a first-visit badge for (TMS_FIRST_REEF) */
} world_scan_t;

/* the words the engine says about a tank's animals, per world: whole phrases, never spliced words (world.h) */
typedef struct {
    const char *stage_fry;                           /* "A FRY" / "A HATCHLING": the checklist's first stage */
    const char *trust_words, *trust_done;            /* TRUST gate: "ALL FISH MUST HAVE TRUST", "EVERY FISH DOES" */
    const char *feed_words;                          /* MEALS gate: "FEED THE FISH AT LEAST" */
    const char *hold_words2;                         /* HOLD gate's second line: "UNTIL A FISH SWIMS TO IT" */
    const char *grow_words;                          /* GROW gate: "THE YOUNGEST FISH MUST" */
    const char *shed_words;                          /* SHED gate (the terrarium's): "THE LIZARDS MUST SHED"; NULL where no gate has one */
    const char *shelf_first, *shelf_nth, *shelf_nth_scan;   /* a tank's default names: "MY TANK", "TANK %u", "TANK %u%c" (shelf.h) */
} world_words_t;

typedef struct world_s {
    int         id;                                  /* PT_WORLD_AQUARIUM / PT_WORLD_TERRARIUM (a pack's WORLDS bit) */
    const char *name;                                /* "aquarium" / "terrarium" */

    /* ---- the brain's reflex: its goal rules when the model is not asked (advisor.c) ---- */
    goal_t    (*rules)(const tank_t *t, int animal, bool request);
    goal_id_t   goal_friend;                         /* the goal that is company (a fish follows, a lizard investigates) */

    /* ---- the shop (progression.c): the catalogue's numbering is the engine's (progression.h SD_ITEMS / SD_TERR_ITEMS);
       what a world offers, and what its purchases do, are the world's ---- */
    uint32_t  (*unlock_bits)(void);                  /* every bit a save's sd_unlocks may keep in this world */
    bool      (*can_buy)(int item);                  /* an item this world's shop may sell */
    void      (*item_bought)(tank_t *t, const sd_item_t *it, int item);   /* its effect, once paid for (placements, a dish's first worms) */
    int       (*earn_lines)(char (*lines)[30], int max);                  /* HOW TO EARN: the lines, <= 25 chars each; their count */

    /* ---- the arrival (progression.c) ---- */
    arrival_style_t arrival;
    int       (*care_gates)(const tank_t *t, const care_ctx_t *c, care_gate_t g[CARE_GATES_MAX]);
    const char *const *(*fry_tip)(int kind);         /* the HOW? words behind a gate kind (never NULL: a kind the world has no gate for gets its nearest) */
    int       (*fry_extra)(const tank_t *t, fry_req_t *out);   /* checklist rows after the gates (the nursery's grass); their count */
    bool      (*nursery_ready)(const tank_t *t);     /* a place to be born in: an arrival is earned and a courtship begins only with one */
    bool      (*court_ok)(const tank_t *t);          /* the surroundings let a pair of parents court (the aquarium: a clean glass and a nursery bed) */
    void      (*arrival_place)(tank_t *t, int slot); /* a fry just added at slot: where it is set down */
    void      (*light_on)(tank_t *t);                /* the light comes on (the mealworm farm's morning) */
    void      (*slept)(tank_t *t, float seconds);    /* a sleep of this many seconds is over, for a species that sheds (the farm's night) */
    void      (*restore)(tank_t *t, const struct save_s *sv);   /* the creatures only this world has, put back from a loaded save (the aquarium's school, urchin, crab and seahorse) */
    bool      (*dish_here)(const tank_t *t);         /* the habitat has a mealworm dish (the terrarium's: a bin without one never has worms to keep) */

    /* ---- milestones (progression_tick, notice.c) ---- */
    void      (*scan_fish)(const tank_t *t, const fish_t *f, world_scan_t *out);   /* the badges one animal's tick earns */
    uint32_t  (*scan_tank)(const tank_t *t);         /* the tank's own badges its pieces and creatures earn (TMS_*) */
    uint32_t    ms_silent, tms_silent;               /* the bits never announced: they belong to a birth, or are no badge here (notice.c) */

    world_words_t words;
    const struct setup_world_s *setup;
} world_t;

#endif
