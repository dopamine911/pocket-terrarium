/* terrarium_priv.h - what the terrarium's files share (issue #4 phase 3). Private to common/worlds/terrarium/: nothing of the
 * engine (tank.c) includes it - the engine reaches these files through their ops tables (locomotion.h loco_walk, mechanics.h
 * food_crickets and mech_terrarium) and the public functions of tank.h. */
#ifndef TERRARIUM_PRIV_H
#define TERRARIUM_PRIV_H
#include "tank.h"
#include "tank_priv.h"

#define LZ_BITE_PX       8.0f       /* a cricket this near its snout (+ 3 x its size) is in its jaws - its snout the drawn body's half length
                                     * ahead of its middle (tank_lizard_half_len) */
#define LZ_DISH_REACH_PX 8.0f       /* at the dish: its snout this near the dish's middle */

/* the walk, the hunt, the look and the crickets between them */
void walk_span(const tank_t *t, const fish_t *f, const terrain_path_t *p, float lane, float *x0, float *x1);
float ck_ground(const terrain_path_t *p, float x);
bool prey_spot(const tank_t *t, int i, int *surf, float *x, float *y);
void ck_flee(tank_t *t, cricket_t *c, float from_x);
int lizard_prey(const tank_t *t, fish_t *f);
bool strike_spot(const tank_t *t, const fish_t *f, int s, float px, float sdx, int dir, bool dish, float *x);
float lizard_miss_odds(const fish_t *f, const cricket_t *c);
void lizard_catch(tank_t *t, int idx, int i);
void lizard_worm(tank_t *t, int idx);
void lizard_missed(tank_t *t, cricket_t *c, float from_x);
void lizard_look(tank_t *t, fish_t *f, float dt);

/* the shed and the decorations: the members of mech_terrarium (terrarium.c) */
bool terr_in_damp_hide(const tank_t *t, int i);
bool terr_shed_growth(tank_t *t, int i);
void terr_shed_clock(tank_t *t, float dt);
void terr_mist_pass(tank_t *t, int k);
bool terr_mist_soak(tank_t *t, int i);
const deco_info_t *terr_deco(int item);
bool terr_deco_clear(const tank_t *t, int item, float x);
float terr_deco_slide(const tank_t *t, int item, float x);
float terr_deco_home(const tank_t *t, int item);
bool terr_deco_hit(const tank_t *t, float x, float y, int *item);

#endif
