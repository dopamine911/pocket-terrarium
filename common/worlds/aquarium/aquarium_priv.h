/* aquarium_priv.h - what the aquarium's files share (issue #4 phase 3). Private to common/worlds/aquarium/ and tank.c. */
#ifndef AQUARIUM_PRIV_H
#define AQUARIUM_PRIV_H
#include "tank.h"
#include "tank_priv.h"

/* THE LANES (tank.h): no piece placed IN FRONT stands over [x - half, x + half] - a critter here may change its lane
   without popping through one. The pieces with a depth: the castle, the coral, the cluster, the pumpkin, the volcano. */
/* a critter in its FRONT lane inside a piece IN FRONT's footprint stands on that piece's base line, DECOR_FY_FRONT
   (the depth scheme puts a front piece 7-8 px lower than the sand's middle line the critters walk) - else it hangs
   over the base (Strato 10-24: "it looks like the crab is floating ... it's because of the castle"). The drop, eased */
#define UR_BASE_T       7.0f    /* render.c UR_BASE: the urchin's base below URCHIN_FLOOR_Y */
#define SNAIL_SOLE_DY_T 6.0f    /* render.c SNAIL_SOLE_DY: the sole below snail_y */

extern bool g_dbg_hold_emerge_off;
extern const int EXIT_P[5];

uint8_t critter_depth_roll(const tank_t *t, float x, uint32_t salt);
float lane_drop(const tank_t *t, bool front, float x, float half, float floor_y, float now, float dt);
void snail_tick(tank_t *t, float dt);
void snail_sleep(tank_t *t, float seconds);
void urchin_tick(tank_t *t, float dt);
void urchin_sleep(tank_t *t, float seconds);
bool crab_floor_x(const tank_t *t, float *x);
void crab_tick(tank_t *t, float dt);
bool box_clear_beds(const tank_t *t, float x0, float x1, float y1);
bool box_clear_pieces(const tank_t *t, float x0, float x1, float y1);
bool pieces_keep(const tank_t *t, float x0, float x1, float y1, int g0, int p0, int g1, int p1);
bool exit_round(const tank_t *t, float x, float y, float hw, float hh, const bool hides[5], float wx, float wy, float *ex, float *ey);
bool piece_exit(const tank_t *t, const fish_t *f, float wx, float wy, float *ex, float *ey);
float depth_hash(const tank_t *t, float x, float y, uint32_t salt);
void seahorse_tick(tank_t *t, float dt);
void shrimp_tick(tank_t *t, float dt);
void shrimp_sleep(tank_t *t, float seconds);
void shrimp_scatter(tank_t *t, float x, float y, float radius);
uint8_t depth_pick(tank_t *t, const fish_t *f, goal_id_t g);
uint8_t pdepth_pick(tank_t *t, const fish_t *f, goal_id_t g);
bool fish_pieces_keep(const tank_t *t, const fish_t *f, int g, int p);
bool grass_clear(const tank_t *t, const fish_t *f);
void update_fish(tank_t *t, int idx, float dt);

#endif
