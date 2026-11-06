/* locomotion.h - the movement modules, dispatched per animal (issue #4 phase 3, docs/ptpk.md "Capabilities").
 *
 * A SPECIES chooses how it moves (species_t.loco: caps.h CAP_SWIM, CAP_WALK, later CAP_SLITHER) and the firmware provides the
 * modules that do it. The engine (tank.c) never asks which world it is built for: for animal i it asks tank_loco(t, i), the
 * module of that animal's species' movement, and calls through it - one function-pointer call per animal per frame, nothing
 * per pixel. A module is one `const loco_ops_t`, defined by the files that hold its mechanics:
 *     loco_swim   common/worlds/aquarium/swim.c   a fish's goal -> target, its steering, its depth, its meals
 *     loco_walk   common/worlds/terrarium/walk.c  a lizard's plan on the ground (terrain.h), its hunt, its hand
 * Which modules a build HAS is listed in one place, common/locomotion/modules.c (a build compiles one world's today; the
 * step that compiles both lists both there and in the Makefile / CMake globs - nothing else changes). A member that a
 * movement has no use for is NULL, and the engine's call sites say what that means.
 *
 * What a module owns is what depends on HOW the animal moves, never on the world around it: the world's own mechanics (its
 * food, its creatures, its pieces) are mechanics.h. */
#ifndef LOCOMOTION_H
#define LOCOMOTION_H
#include "tank.h"

typedef struct loco_ops {
    const char *name;                              /* "swim", "walk" */
    caps_t      mode;                              /* the movement it is (caps.h CAP_SWIM, CAP_WALK ...) */
    /* tank_tick, for each animal: one frame - the goal's target, the steering, the meals, the drives - and its body's pose */
    void     (*update)(tank_t *t, int i, float dt);
    /* an animal new to the tank (made, bought, hatched) set onto the ground where it stands (the walk's floor). NULL: the
       water has no ground to seat it on */
    void     (*seat)(const tank_t *t, fish_t *f);
    /* where a resting finger is for this movement: the finger itself (a swimmer goes to it), or the ground nearest it (the
       walker's hand rests there) - tank.h tank_hand_point */
    void     (*hand_point)(const tank_t *t, float *x, float *y);
    /* animal i has come to the resting finger (tank.h tank_at_finger) */
    bool     (*at_finger)(const tank_t *t, int i);
    /* a settled hold (touch_tick): animal i is d px from the hand; draw_begins is the frame its draw began. Credit the
       hold-approach badge it has earned. NULL: this movement credits it from its own steering (update) */
    void     (*hold_credit)(tank_t *t, int i, float d, bool draw_begins);
    /* squared distance from a tap at (px, py) to animal i as if its middle were at (x, y) (tank.h tank_tap_d2) */
    float    (*tap_d2)(const tank_t *t, int i, float x, float y, float px, float py);
    /* the keeper's rap reaches animal i (tank_startle, tank_touch_tap): what it does besides the engine's stress and trust -
       the walk's dash for cover. NULL: nothing more */
    void     (*spook)(tank_t *t, int i);
    /* the bits this movement adds to the advisor's ask signature for animal i (tank.c state_signature). NULL: none */
    uint32_t (*signature)(const tank_t *t, int i);
    /* the advisor has given animal i a new goal g (tank_tick): what the movement sets up for it - a swimmer's depth.
       NULL: nothing */
    void     (*goal_set)(tank_t *t, fish_t *f, goal_id_t g);
} loco_ops_t;

/* the module of animal i's species' movement (a tank holds one species today; the format keeps room for one on each
   animal). Never NULL: a species whose movement no module here serves is held by the build's own module (a pack whose
   needs the firmware does not meet never gets as far as a tank: packs/compat.h) */
const loco_ops_t *tank_loco(const tank_t *t, int i);
const loco_ops_t *loco_of(const species_t *sp);

#endif
