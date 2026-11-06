/* mechanics.h - the food mechanics and the worlds' own mechanics, dispatched without naming a world (issue #4 phase 3).
 *
 * Two more kinds of module the engine (tank.c) calls through, beside the movements (locomotion.h):
 *
 *   FOOD   what a feed gesture drops and what becomes of it - the aquarium's pellets that sink and rest, the terrarium's
 *          crickets that fall, hop and hide, and the mealworm dish. Chosen by the SPECIES' diet (species.h DIET_*,
 *          caps.h CAP_PELLET, CAP_CRICKET, CAP_MEALWORM): tank_food(t). One `const food_ops_t` per kind of food, defined
 *          by the files that hold its mechanics:   food_pellets  worlds/aquarium/pellets.c
 *                                                    food_crickets worlds/terrarium/crickets.c
 *   WORLD  everything else a world adds to the tank besides its animals' movement - the aquarium's snail, urchin, crab,
 *          seahorse and shrimp school; the terrarium's shed, the mister and its decorations. Every module present is called
 *          at the engine's fixed points (a module has nothing to do for what is not in the tank: no crab owned, no lizard
 *          in a shed). One `const mech_ops_t` per world:  mech_aquarium worlds/aquarium/aquarium.c
 *                                                          mech_terrarium worlds/terrarium/terrarium.c
 *
 * Which modules a build has is listed in common/locomotion/modules.c, the one place; a member a module has no use for is NULL
 * and the engine's call sites say what that means (every public function of tank.h that goes through here keeps its name and
 * its answer where no module has the mechanic: the shed of a tank without a shedder, the decorations of a tank with none). */
#ifndef MECHANICS_H
#define MECHANICS_H
#include "tank.h"

typedef struct food_ops {
    const char *name;                                   /* "pellets", "crickets" */
    uint8_t     diet;                                   /* the species.h DIET_* bits it feeds */
    /* tank_feed after the engine's own (the tank is handled): the keeper's gesture at x drops n pieces from the hand - or,
       over the terrarium's dish, fills the dish - emits the plink, and opens a meal (feed_spot_x, feed_open) */
    void  (*feed)(tank_t *t, float x, int n);
    /* tank_scatter_food: the tank's own trickle drops n pieces at a random x */
    void  (*scatter)(tank_t *t, int n);
    /* tank_tick: a frame of the food there is (it sinks, settles, decays; it falls, sits, hops, flies, hides); how many
       pieces are alive, which the trickle's gate counts */
    int   (*tick)(tank_t *t, float dt);
    /* tank_tick_sleep: the night's share (pellets gone stale; crickets live out their time) */
    void  (*sleep)(tank_t *t, float seconds);
    /* tank_food_sight: the food animal idx sees, its kind's - the nearest pellet, or a lizard's prey */
    bool  (*sight)(const tank_t *t, int idx, float *x, float *y, float *dist);
    /* tank_food_count: food there is to eat */
    int   (*count)(const tank_t *t);
    /* tank_clear_food: none of this kind (and no memory of it) */
    void  (*clear)(tank_t *t);
    /* the tank's own trickle holds off now, besides the engine's gates (the lizards' after lights-out). NULL: never */
    bool  (*trickle_hold)(const tank_t *t);
    /* the keeper's rap: the pieces about within r of (x, y) fly. NULL: they sit still */
    void  (*scare)(tank_t *t, float x, float y, float r);
} food_ops_t;

/* a decoration of a world's: its index (the order of tank_t.deco_x), the half of its footprint and its height (what
   tank_decor_half_w and tank_decor_height answer, render.c draws from them), the floor fraction it stands at until placed, the
   floor line it stands on (tank_decor_fy), and its sd_unlocks bit */
typedef struct deco_info { int index; float half_w, height, u, fy; uint32_t bit; } deco_info_t;

typedef struct mech_ops {
    const char *name;                                   /* "aquarium", "terrarium" */
    caps_t      serves;                                 /* the areas of the habitats it is for (caps.h CAP_WATER, CAP_LAND): which tanks it OWNS an answer for */
    /* tank_tick: the world's creatures, a frame - after the upkeep (the grass and the film), before the bubbles rise */
    void  (*tick)(tank_t *t, float dt);
    /* tank_tick_sleep: a night - its part before the slices ... */
    void  (*sleep)(tank_t *t, float seconds);
    /* ... and after each slice's growth (the grazers eat into what grew) */
    void  (*sleep_slice)(tank_t *t, float dt);
    /* the keeper's rap or a spooked tank (tank_startle, tank_touch_tap): what scatters within r of (x, y) */
    void  (*scare)(tank_t *t, float x, float y, float r);
    /* a knock on the glass anywhere - the triple tap (tank_startle alone): what runs for cover whatever the distance */
    void  (*knock)(tank_t *t, float x);
    /* a resting finger (touch_tick): trust is earned this many times as fast (1: as usual) ... */
    float (*trust_x)(const tank_t *t);
    /* ... and what a hitched helper passed on: the trust one animal had and has, over dt seconds */
    void  (*trust_credit)(tank_t *t, float was, float now, float dt);
    /* THE SHED and the mister (tank.h): the tended seconds, a growth shed, a damp hide, a mister's pass and its soak */
    void  (*shed_clock)(tank_t *t, float dt);
    bool  (*shed_growth)(tank_t *t, int i);
    bool  (*in_damp_hide)(const tank_t *t, int i);
    void  (*mist_pass)(tank_t *t, int k);
    bool  (*mist_soak)(tank_t *t, int i);
    /* the decorations this world sells (tank.h, the shop's items past the pieces): what an item is of this world's (NULL:
       none of them), the hit test of a finger on the glass (true: this world owns the answer - asked of a tank in a habitat
       it serves: *item its piece or -1, never another world's pieces from old bits / default spots), and where a piece may stand */
    const deco_info_t *(*deco)(int item);
    bool  (*deco_hit)(const tank_t *t, float x, float y, int *item);
    bool  (*deco_clear)(const tank_t *t, int item, float x);
    float (*deco_slide)(const tank_t *t, int item, float x);
    float (*deco_home)(const tank_t *t, int item);
} mech_ops_t;

/* the food module of the tank's species' diet (never NULL; the build's own when no module's diet matches) */
const food_ops_t *tank_food(const tank_t *t);
const food_ops_t *food_of(const species_t *sp);
int  food_n(void);
const food_ops_t *food_at(int i);
/* the world modules present, to call at each fixed point in turn */
int  mech_n(void);
const mech_ops_t *mech_at(int i);
/* the tank's habitat is one of the areas the module serves */
bool mech_serves(const mech_ops_t *m, const tank_t *t);

#endif
