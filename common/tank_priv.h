/* tank_priv.h - what tank.c shares with the modules split out of it (issue #4 phase 3). Private to the engine: nothing outside the tank's own files includes it. */
#ifndef TANK_PRIV_H
#define TANK_PRIV_H
#include "tank.h"


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
/* round the finger, not under it (0.3.9.3, Metalus on the pendant: "the touch and hold hides all my fish
 * under my finger"; Strato: not a defined point either - "swimming in a bit of a circular pattern around ...
 * with a bit of a buffer ... with the ability to cross over to the other side", never "consistently group to
 * the top of the finger"). The fish that come in swim a loose ring round the fingertip as one company: the
 * ring drifts one way, then back after a while; each fish keeps a little behind the one before (close enough
 * to stay nose to tail - the social warmth, SOCIAL_HOLD_GAIN), wanders in and out of its own radius, and now
 * and then one crosses to the far side and comes back. Clamped to the glass by a wall or the sand. */
#define HOLD_RING_R       64.0f  /* px from the touch point: the fingertip (~30) + a buffer + half a fish */
#define HOLD_RING_WOBBLE  10.0f  /* ... in and out of it, each fish its own */
#define HOLD_RING_GAP     0.62f  /* rad between a fish and its partner (~45 px: a body apart, past the separation's push, still company) */
#define VEG_SLOW            0.60f     /* cruise speed factor inside a canopy */

uint32_t xr(tank_t *t);
float norm_ang(float a);
float anim(const tank_t *t);
void fish_beat(fish_t *f, float dt);
void veg_bed_base(const tank_t *t, int b, float *bx0, int *n);
void court_site(const tank_t *t, float *cx, float *cy, float *rx);
void veg_sync(tank_t *t);
float veg_cap_mean(const tank_t *t, int b);
bool veg_inside(const tank_t *t, int b, float x, float y);
void tank_startle(tank_t *t, float x, float y);
void hold_approach(tank_t *t, int i);
void body_follow(const tank_t *t, fish_t *f, float env, float dt);
float stuck_floor(const fish_t *f);
int zone_of(float x, float y);

#endif
