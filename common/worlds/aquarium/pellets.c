/* pellets.c - the aquarium's food, a food module (mechanics.h food_pellets): the pellets (tank_nearest_food moved out of tank.c
 * verbatim; the feed, trickle, tick, night and sight are tank.c's former aquarium branches; issue #4 phase 3) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/aquarium/aquarium_priv.h"
#include "pt_util.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tank_nearest_food(const tank_t *t, const fish_t *f, float *dist_out) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < MAX_FOOD; i++) {
        if (!t->food[i].alive) continue;
        float d = tank_dist(f->x, f->y, t->food[i].x, t->food[i].y);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist_out) *dist_out = bd;
    return best;
}

/* ---- THE PELLETS as a food module (mechanics.h): what tank.c's feed, trickle, tick, night and sight were for the aquarium ---- */
/* the keeper's gesture: n pellets from the hand at x, the plink, and a meal opened (tank.h tank_feed) */
static void pellets_feed(tank_t *t, float x, int n) {
    x = clampf(x, TANK_FX0 + 25, TANK_FX1 - 25);
    int dropped = 0;
    for (int i = 0; i < MAX_FOOD && n > 0; i++) {
        if (t->food[i].alive) continue;
        t->food[i].alive = true; t->food[i].from_player = true;
        t->food[i].x = clampf(x + tank_randf(t, -14, 14), TANK_FX0 + 20, TANK_FX1 - 20);
        t->food[i].y = tank_glass_top(t->food[i].x) + tank_randf(t, 6, 18);
        t->food[i].age = 0; t->food[i].floor_s = 0; t->food[i].nibbled = 0;
        n--; dropped++;
    }
    if (dropped) tank_emit(TEV_FEED, -1);        /* the plink is for pellets, not for the tap (a full tank drops none) */
    t->feed_spot_x = t->feed_spot_x < 0 ? x : t->feed_spot_x + (x - t->feed_spot_x) * 0.3f;
    t->feed_open = true;                         /* a meal once somebody eats from it */
}
/* the tank's own trickle: n pellets at a random x */
static void pellets_scatter(tank_t *t, int n) {
    for (int i = 0; i < MAX_FOOD && n > 0; i++) {
        if (t->food[i].alive) continue;
        t->food[i].alive = true; t->food[i].from_player = false;
        t->food[i].x = tank_randf(t, TANK_FX0 + 25, TANK_FX1 - 25);
        t->food[i].y = tank_glass_top(t->food[i].x) + tank_randf(t, 6, 20);
        t->food[i].age = 0; t->food[i].floor_s = 0; t->food[i].nibbled = 0;
        n--;
    }
}
/* food sinks, settles, decays */
static int pellets_tick(tank_t *t, float dt) {
    int live_food = 0;
    for (int i = 0; i < MAX_FOOD; i++) {
        food_t *p = &t->food[i];
        if (!p->alive) continue;
        live_food++;
        p->age += dt;
        if (p->y < FOOD_FLOOR_Y) {
            p->y += FOOD_SINK_PX_S * dt;
            p->x += sinf(p->age * 1.5f) * dt * 2;
        } else p->floor_s += dt;                        /* resting on the floor (FOOD_FLOOR_S) */
        if (p->floor_s > 0 ? p->floor_s > FOOD_FLOOR_S : p->age > FOOD_LIFE_S) p->alive = false;
    }
    return live_food;
}
/* overnight pellets go stale */
static void pellets_sleep(tank_t *t, float seconds) {
    for (int i = 0; i < MAX_FOOD; i++)
        if (t->food[i].alive && (t->food[i].age += seconds) > 45) t->food[i].alive = false;
}
/* the aquarium: the nearest pellet, as ever */
static bool pellets_sight(const tank_t *t, int idx, float *x, float *y, float *dist) {
    const fish_t *f = &t->fish[idx];
    float d; const int i = tank_nearest_food(t, f, &d);
    if (i < 0) return false;
    if (x) *x = t->food[i].x;
    if (y) *y = t->food[i].y;
    if (dist) *dist = d;
    return true;
}
static int pellets_count(const tank_t *t) { int n = 0; for (int i = 0; i < MAX_FOOD; i++) n += t->food[i].alive; return n; }
static void pellets_clear(tank_t *t) { for (int i = 0; i < MAX_FOOD; i++) t->food[i].alive = false; }
const food_ops_t food_pellets = {
    .name = "pellets", .diet = DIET_PELLET,
    .feed = pellets_feed, .scatter = pellets_scatter, .tick = pellets_tick, .sleep = pellets_sleep, .sight = pellets_sight,
    .count = pellets_count, .clear = pellets_clear,
};
