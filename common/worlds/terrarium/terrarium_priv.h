/* terrarium_priv.h - what the terrarium's files share (issue #4 phase 3). Private to common/worlds/terrarium/ and tank.c. */
#ifndef TERRARIUM_PRIV_H
#define TERRARIUM_PRIV_H
#include "tank.h"
#include "tank_priv.h"

#define LZ_BITE_PX       8.0f       /* a cricket this near its snout (+ 3 x its size) is in its jaws - its snout the drawn body's half length
                                     * ahead of its middle (tank_lizard_half_len) */
#define LZ_DISH_REACH_PX 8.0f       /* at the dish: its snout this near the dish's middle */

extern const float DECO_HW[TERR_DECO_N];
extern const float DECO_H[TERR_DECO_N];
extern const float DECO_U[TERR_DECO_N];
extern const uint32_t DECO_BIT[TERR_DECO_N];

void walk_span(const tank_t *t, const fish_t *f, const terrain_path_t *p, float lane, float *x0, float *x1);
void walk_floor(const tank_t *t, fish_t *f);
float ck_ground(const terrain_path_t *p, float x);
bool prey_spot(const tank_t *t, int i, int *surf, float *x, float *y);
int ck_free(const tank_t *t);
float ck_drop_x(const tank_t *t, float x);
void ck_drop_at(tank_t *t, int i, float x, float y, bool from_player);
void ck_flee(tank_t *t, cricket_t *c, float from_x);
void ck_scare(tank_t *t, float x, float y, float r);
int ck_tick(tank_t *t, float dt);
int lizard_prey(const tank_t *t, fish_t *f);
bool strike_spot(const tank_t *t, const fish_t *f, int s, float px, float sdx, int dir, bool dish, float *x);
float lizard_miss_odds(const fish_t *f, const cricket_t *c);
void lizard_catch(tank_t *t, int idx, int i);
void lizard_worm(tank_t *t, int idx);
void lizard_missed(tank_t *t, cricket_t *c, float from_x);
void lizard_spook(tank_t *t, int i);
bool hand_draws(const tank_t *t, const fish_t *f);
int lizard_hand_target(const tank_t *t, int idx, float *target_x, float *hand_x);
void lizard_look(tank_t *t, fish_t *f, float dt);
void update_lizard(tank_t *t, int idx, float dt);

#endif
