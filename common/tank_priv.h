/* tank_priv.h - what tank.c shares with the module files that serve it (issue #4 phase 3): the worlds' mechanics under
 * common/worlds/<world>/, which the engine reaches only through their ops tables (locomotion.h, mechanics.h) and the public
 * functions of tank.h. Private to the engine: nothing else includes it. What is here is what those files cannot do without
 * from the engine's own statics - its random stream, its clock's face, the glass's corners, the touch constants, the grass
 * beds' geometry, the heat and the shed's physiology - so each of them keeps one definition, in tank.c. */
#ifndef TANK_PRIV_H
#define TANK_PRIV_H
#include "tank.h"
#include "locomotion.h"
#include "mechanics.h"

#ifdef TANK_CORNER_R   /* the watch's and the square's rounded corners (tank.h) */
float corner_in(float x, float y, float *cx, float *cy);   /* how far a point stands off its corner's centre (0: it is in no corner's square) */
void corner_clamp(float *x, float *y, float m);            /* out of a corner: back along its radius to m inside the arc */
#else
#define corner_clamp(x, y, m) ((void)0)            /* the plain rectangle (and the bowl, which has its own): no corners */
#endif

#define TAP_WINDOW      0.5f    /* taps closer than this form a burst */
#define STARTLE_RADIUS  140.0f  /* fish this close to an aggressive tap bolt */
#define HOLD_RADIUS     1000.0f /* a resting finger is seen from anywhere on the
                                 * glass (2026-09-04, was 160: a fish only came
                                 * when you held next to it, which hid the
                                 * trust tell) */
#define HOLD_ATTRACT_S  2.7f    /* hold_time before fish approach; platforms
                                 * report holds ~0.3 s in, so ≈3 s of contact */
#define HOLD_HUNGER_VETO 7.5f   /* this hungry, a fish ignores the finger */
#define TRUST_PER_S        0.012f /* trust a fish earns a second under a resting finger (0.3.6: was 0.02;
                                   * Strato: "a bit slower" so the seahorse's double is worth having) */

uint32_t xr(tank_t *t);                                    /* the tank's random stream, a step (tank_randf is its float face) */
float norm_ang(float a);                                   /* an angle folded to -pi..pi */
float anim(const tank_t *t);                               /* the clock's float face (tank.h tank_anim), worked out once a tick */
int zone_of(float x, float y);                             /* the schema's 3 x 2 zone grid (advisor_core.c encodes the same), 0..5 */
void fish_beat(fish_t *f, float dt);                       /* the tail beat's phase a frame on (both movements run it: render draws a lizard's tail from it) */
void body_follow(const tank_t *t, fish_t *f, float env, float dt);   /* the body's heat toward env over dt seconds, at its pack's tau */
float stuck_floor(const fish_t *f);                        /* a stuck shed's floor under the stress */
void hold_approach(tank_t *t, int i);                      /* animal i has really come to the resting hand: its badge and the hold's count */
void tank_startle(tank_t *t, float x, float y);            /* the triple tap: stress, a flight, the world's scatter */
void court_site(const tank_t *t, float *cx, float *cy, float *rx);   /* where the pair circles: the nursery bed, or the old spot by the reef */
void veg_bed_base(const tank_t *t, int b, float *bx0, int *n);   /* the grass beds' geometry and growth: the upkeep, tank.c */
void veg_sync(tank_t *t);
float veg_cap_mean(const tank_t *t, int b);
bool veg_inside(const tank_t *t, int b, float x, float y);

/* a newcomer (made, bought, hatched) onto its ground where it stands: its movement's seat (the walk's floor; the water has none) */
static inline void tank_seat(const tank_t *t, int slot, fish_t *f) {
    const loco_ops_t *lo = tank_loco(t, slot);
    if (lo->seat) lo->seat(t, f);
}

#endif
