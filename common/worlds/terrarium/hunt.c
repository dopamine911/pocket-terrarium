/* hunt.c - THE HUNT's prey and its meal, the mealworm farm, the hatchling (the walk's, walk.c)
 * (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/terrarium/terrarium_priv.h"
#include "pt_util.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- THE HUNT's prey and its meal (tank.h THE CRICKETS; the crickets themselves: THE CRICKETS above) ----
 * the prey: the cricket about, or the dish while it holds worms, nearest by the route - the dish at LZ_DISH_X its cost - the
 * one it is after (fish_t.prey) kept unless another is LZ_PREY_SWITCH cheaper; -1 none. A cricket it is after is where it
 * was last seen (fish_t.seen_surf, seen_x: a cricket crawling into cover is seen to the spot) */
int lizard_prey(const tank_t *t, fish_t *f) {
    const terrain_t *tr = tank_terrain(t);
    int best = -1; float bc = 1e30f, cur = 1e30f;
    for (int i = 0; i <= PREY_DISH; i++) {
        int s; float px, py; terrain_hop_t hop;
        if (!prey_spot(t, i, &s, &px, &py) || !terrain_route(tr, f->surf, f->x, s, px, &hop)) continue;
        const float cost = hop.cost * (i == PREY_DISH ? LZ_DISH_X : 1.0f);
        if (i == f->prey) cur = cost;
        if (cost < bc) { bc = cost; best = i; }
    }
    if (cur < 1e30f && !(bc < LZ_PREY_SWITCH * cur)) best = f->prey;
    f->prey = (int8_t)best;
    if (best >= 0 && best < MAX_CRICKETS) { f->seen_surf = t->cricket[best].surf; f->seen_x = t->cricket[best].x; }
    return best;
}
/* where on path s it stands to strike prey at px facing dir (the dish: its snout over it): its snout - sdx ahead of its
   middle - in the strike's reach of px (LZ_BITE_PX behind .. LZ_STRIKE_PX ahead; the dish's LZ_DISH_REACH_PX either way),
   LZ_ARRIVE_PX inside it (a walk stops that short of its point), as near the stalk's mark (LZ_STRIKE_PX x 0.6 short) as its
   span lets it; false: no such spot facing dir (a narrow ledge, the prey at its end) */
bool strike_spot(const tank_t *t, const fish_t *f, int s, float px, float sdx, int dir, bool dish, float *x) {
    float x0, x1; walk_span(t, f, &tank_terrain(t)->path[s], 0, &x0, &x1);
    const float behind = dish ? LZ_DISH_REACH_PX : LZ_BITE_PX, ahead = dish ? LZ_DISH_REACH_PX : LZ_STRIKE_PX;
    float lo = dir > 0 ? px - ahead - sdx : px - behind + sdx, hi = dir > 0 ? px + behind - sdx : px + ahead + sdx;
    if (hi - lo > 2 * LZ_ARRIVE_PX + 1) { lo += LZ_ARRIVE_PX + 0.5f; hi -= LZ_ARRIVE_PX + 0.5f; }
    lo = fmaxf(lo, x0); hi = fminf(hi, x1);
    if (lo > hi) return false;
    *x = clampf(px - dir * (sdx + (dish ? 0 : LZ_STRIKE_PX * 0.6f)), lo, hi);
    return true;
}
/* a strike's odds of missing (tank.h LZ_MISS*): the cold, its stage, the cricket in the air or frightened */
float lizard_miss_odds(const fish_t *f, const cricket_t *c) {
    float m = LZ_MISS;
    if (f->heat < HEAT_SLOW) m += LZ_MISS_COLD * (1 - clampf(f->heat, 0, HEAT_SLOW) / HEAT_SLOW);
    m += f->stage == STAGE_FRY ? LZ_MISS_HATCH : f->stage == STAGE_JUV ? LZ_MISS_JUV : f->stage == STAGE_ELDER ? LZ_MISS_ELDER : 0;
    if (c->state == CK_HOP) m += LZ_MISS_HOP;
    if (c->alert > 0) m += LZ_MISS_ALERT;
    return clampf(m, 0, 0.95f);
}
/* a meal: hunger off by its pack's hunger_per_cricket x worth - and from the keeper's hand a MEAL, as a fish's pellet is
   (eat_nearby_food): eaten_player, the first-meal badge, the meal's pay; the feed gesture counted once */
static void lizard_meal(tank_t *t, int idx, float worth, bool from_player) {
    fish_t *f = &t->fish[idx];
    f->hunger = clampf(f->hunger - tank_species(t)->hunger_per_cricket * worth, 0, 10);
    f->energy = clampf(f->energy + worth, 0, 10);
    f->eaten++;
    tank_emit(TEV_EAT, idx);
    if (from_player) {
        if (!tank_is_visitor(idx)) { f->eaten_player++; f->ms_bits |= MS_FIRST_MEAL_FROM_YOU; f->meal_due = true; }
        if (t->feed_open) { t->player_feedings++; t->feed_open = false; }   /* the gesture became a meal */
    }
}
void lizard_catch(tank_t *t, int idx, int i) {
    cricket_t *c = &t->cricket[i];
    const bool mine = c->from_player;
    c->state = CK_NONE; t->ck_caught++; t->fish[idx].prey = -1; t->fish[idx].seen_surf = -1;   /* (eaten: nothing to look for) */
    if (!tank_is_visitor(idx)) t->fish[idx].ms_bits |= MS_FIRST_CATCH;   /* its first cricket, anyone's (P5's badge) */
    lizard_meal(t, idx, 1.0f, mine);
}
void lizard_worm(tank_t *t, int idx) {                 /* a worm from the dish: the keeper's - or the farm's, the tank's own (P5) */
    if (!t->dish_worms) return;
    const bool mine = !t->dish_farm;
    t->dish_worms--; t->dish_eaten++;
    if (!t->dish_worms) t->dish_farm = false;
    lizard_meal(t, idx, DISH_WORM_X, mine);
}
/* the MEALWORM FARM (P5, tank.h): owned, a dish here and not full - filled, and the dish the farm's: its worms the tank's
   own (whoever filled it last owns it, as a feed gesture's fill makes it the keeper's) */
void tank_dish_farm(tank_t *t) {
    if (!(t->sd_unlocks & SD_ITEM_FARM) || !tank_dish_here(t) || t->dish_worms >= DISH_WORMS) return;
    t->dish_worms = DISH_WORMS; t->dish_farm = true;
}
/* a hatchling (P5): inside the humid hide - or the first hide there is - as a lizard staged into one (tank_lizard_enter) */
void tank_hatch_place(tank_t *t, int slot) {
    int k = tank_humid_hide(t);
    if (k < 0 && tank_habitat(t)->n_hides > 0) k = 0;
    if (k >= 0) tank_lizard_enter(t, slot, k);
}
void lizard_missed(tank_t *t, cricket_t *c, float from_x) {
    t->ck_missed++;
    if (c->state == CK_HOP) c->alert = CK_ALERT_S; else ck_flee(t, c, from_x);   /* (mid-hop: it lands first, wide awake) */
}
