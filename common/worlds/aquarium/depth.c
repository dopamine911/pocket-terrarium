/* depth.c - SWIM DEPTH: in front of, behind or through the grass and the pieces (docs/swim-depth.md), for the swim, the seahorse,
 * the shrimp and the floor critters (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
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

uint8_t critter_depth_roll(const tank_t *t, float x, uint32_t salt) {   /* a floor critter's want against the grass: a fifth behind, a third through */
    float r = depth_hash(t, x, 0, salt);
    return r < 0.22f ? FISH_DEPTH_BEHIND : r < 0.55f ? FISH_DEPTH_THROUGH : FISH_DEPTH_FRONT;
}

/* SWIM DEPTH (0.3.9.4): is a box clear of every bed of grass / every piece? (an animal's depth changes only there: nothing pops
   through a frond or a piece). The pieces by their footprint and height (render.c's sprites) */
bool box_clear_beds(const tank_t *t, float x0, float x1, float y1) {
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (t->veg_growth[b] < VEG_BARE) continue;
        float bx0, bx1, top; int n; tank_veg_bed(t, b, &bx0, &bx1, &top, &n);
        if (x1 > bx0 - 8 && x0 < bx1 + 8 && y1 > top) return false;
    }
    return true;
}
bool box_clear_pieces(const tank_t *t, float x0, float x1, float y1) {
    static const int P[5] = { 2, 3, 4, 8, 9 }, H[5] = { 164, 92, 120, 82, 208 };   /* the castle, coral, reef cluster, pumpkin, volcano */
    for (int k = 0; k < 5; k++) {
        if (!tank_decor_in(t, P[k])) continue;
        float cx = tank_decor_x(t, P[k]), hw = tank_decor_half_w(P[k]) + 4;
        if (x1 > cx - hw && x0 < cx + hw && y1 > TANK_BOT - 16 - H[k]) return false;
    }
    return true;
}
/* ... and a change of an animal's depths (g0, p0 -> g1, p1) over the pieces: taken only where every piece the box meets stays
   before it or behind it as it was (0.3.9.7, a #help thread: "they choose to be on a layer behind the volcano while they're
   in front of the volcano. So they just poof disappear" - the grass depth waited for the beds but not for the pieces, and
   behind or through the grass is behind a piece IN FRONT, behind it is behind a piece BEHIND) */
static bool g_dbg_depth_pop_old;                     /* (tests: the 0.3.9.6 rule - the grass depth blind to the pieces) */
void tank_debug_depth_pop_old(bool on) { g_dbg_depth_pop_old = on; }
bool pieces_keep(const tank_t *t, float x0, float x1, float y1, int g0, int p0, int g1, int p1) {
    if (g_dbg_depth_pop_old) return g0 != g1 || box_clear_pieces(t, x0, x1, y1);
    static const int P[5] = { 2, 3, 4, 8, 9 }, H[5] = { 164, 92, 120, 82, 208 };   /* (as box_clear_pieces) */
    for (int k = 0; k < 5; k++) {
        if (!tank_decor_in(t, P[k])) continue;
        float cx = tank_decor_x(t, P[k]), hw = tank_decor_half_w(P[k]) + 4;
        if (!(x1 > cx - hw && x0 < cx + hw && y1 > TANK_BOT - 16 - H[k])) continue;
        const int z = tank_decor_z(t, P[k]);
        if (depth_piece_cover(z, P[k] == 2, g0, p0) != depth_piece_cover(z, P[k] == 2, g1, p1)) return false;
    }
    return true;
}
/* THE WAY OUT (a held finger's call, Strato 11-01: "the fish would not be able to see my finger through the volcano"): an
   animal hidden by the pieces its box meets swims out round them before it comes to the front - never a pop through a piece */
bool g_dbg_hold_emerge_off;                  /* (tests: 0.4's finger before the call - the depth its goal's, no way out) */
void tank_debug_hold_emerge_off(bool on) { g_dbg_hold_emerge_off = on; }
const int EXIT_P[5] = { 2, 3, 4, 8, 9 }, EXIT_H[5] = { 164, 92, 120, 82, 208 };   /* (as box_clear_pieces) */
/* ... the way round: the pieces that would hide a box (x - hw .. x + hw, down to y + hh) and meet it, as one block - out to the
   nearest of its left side, its right side and over its top, the shortest way on to wx, wy */
bool exit_round(const tank_t *t, float x, float y, float hw, float hh, const bool hides[5], float wx, float wy, float *ex, float *ey) {
    float x0 = 1e9f, x1 = -1e9f, top = 1e9f;
    for (int k = 0; k < 5; k++) {
        if (!hides[k] || !tank_decor_in(t, EXIT_P[k])) continue;
        const float cx = tank_decor_x(t, EXIT_P[k]), phw = tank_decor_half_w(EXIT_P[k]) + 4, ptop = TANK_BOT - 16 - EXIT_H[k];
        if (!(x + hw > cx - phw && x - hw < cx + phw && y + hh > ptop)) continue;
        if (cx - phw < x0) x0 = cx - phw;
        if (cx + phw > x1) x1 = cx + phw;
        if (ptop < top) top = ptop;
    }
    if (x1 < x0) return false;
    const float M = 8;                               /* (a little past the edge: the box then clears at once) */
    float cand[3][2] = { { x0 - hw - M, y }, { x1 + hw + M, y }, { x, top - hh - M } };
    bool ok[3] = { cand[0][0] >= TANK_FX0 + hw, cand[1][0] <= TANK_FX1 - hw, cand[2][1] >= 24 };
    int best = -1; float bd = 1e9f;
    for (int c = 0; c < 3; c++) {
        if (!ok[c]) continue;
        const float d = tank_dist(x, y, cand[c][0], cand[c][1]) + tank_dist(cand[c][0], cand[c][1], wx, wy);
        if (d < bd) { bd = d; best = c; }
    }
    if (best < 0) return false;
    *ex = cand[best][0]; *ey = cand[best][1];
    return true;
}
bool piece_exit(const tank_t *t, const fish_t *f, float wx, float wy, float *ex, float *ey) {   /* (a fish: what its depths put in front of it) */
    if (g_dbg_hold_emerge_off) return false;
    bool hides[5];
    for (int k = 0; k < 5; k++) hides[k] = depth_piece_cover(tank_decor_z(t, EXIT_P[k]), EXIT_P[k] == 2, f->gdepth, f->pdepth) > 0;
    return exit_round(t, f->x, f->y, 34 * f->size + 6, 26 * f->size + 6, hides, wx, wy, ex, ey);
}
/* ... and a roll of its own (a hash, as the lanes': the tank's random stream untouched) */
float depth_hash(const tank_t *t, float x, float y, uint32_t salt) {
    uint32_t h = ((uint32_t)(uint64_t)(t->clock * 997.0) ^ (uint32_t)(x * 31.0f) * 2654435761u ^ (uint32_t)(y * 17.0f) * 40503u ^ salt) * 2654435761u;
    return ((h >> 12) & 0xffff) / 65536.0f;
}

/* SWIM DEPTH (0.3.9.4, docs/swim-depth.md; Strato: the goal and the temperament choose it, no new model): how a
   fish carries out its goal against the back grass. Resting it tucks into the grass (mysiak: "Maybe they could hide
   at least while resting"), inspecting it weaves through, fleeing it hides; it feeds and plays at the bubbles in
   front, where the keeper sees it; exploring mostly in front. A shy fish hides more */
static float depth_roll(const tank_t *t, const fish_t *f, uint32_t salt) {   /* (its own hash, as the lanes': the tank's random stream untouched) */
    uint32_t h = ((uint32_t)(uint64_t)(t->clock * 997.0) ^ (uint32_t)(f->x * 31.0f) * 2654435761u ^ (uint32_t)(f->y * 17.0f) * 40503u ^ salt) * 2654435761u;
    return ((h >> 12) & 0xffff) / 65536.0f;
}
uint8_t depth_pick(tank_t *t, const fish_t *f, goal_id_t g) {
    (void)t;
    float shy = 1.0f - clampf(f->bold, 0, 1), r = depth_roll(t, f, 0x9e37u), pb, pt;   /* behind, through: the rest IN FRONT */
    switch (g) {
    case GOAL_REST:          pb = 0.30f + 0.25f * shy; pt = 0.45f; break;
    case GOAL_INSPECT_REEF:  pb = 0.15f + 0.15f * shy; pt = 0.55f; break;
    case GOAL_FLEE_SHADOW:   pb = 0.85f; pt = 0.15f; break;
    case GOAL_DART_PLAY:     pb = 0.05f; pt = 0.45f; break;
    case GOAL_EXPLORE: case GOAL_FOLLOW_FRIEND: pb = 0.05f + 0.15f * shy; pt = 0.15f + 0.10f * shy; break;
    default:                 return FISH_DEPTH_FRONT;
    }
    return r < pb ? FISH_DEPTH_BEHIND : r < pb + pt ? FISH_DEPTH_THROUGH : FISH_DEPTH_FRONT;
}
/* ... and against the pieces (phase 2): behind them only in passing - fleeing, poking round the reef, now and then
   exploring; never at rest, never feeding (Strato's D2: a fish behind the castle vanishes, the kids must find it) */
uint8_t pdepth_pick(tank_t *t, const fish_t *f, goal_id_t g) {
    float shy = 1.0f - clampf(f->bold, 0, 1), r = depth_roll(t, f, 0x7f4au), pb;
    switch (g) {
    case GOAL_FLEE_SHADOW:   pb = 0.85f; break;
    case GOAL_INSPECT_REEF:  pb = 0.40f + 0.20f * shy; break;
    case GOAL_EXPLORE: case GOAL_FOLLOW_FRIEND: pb = 0.15f + 0.25f * shy; break;
    case GOAL_DART_PLAY:     pb = 0.20f; break;
    default:                 return FISH_DEPTH_FRONT;   /* REST, SEEK_FOOD, VISIT_BUBBLES */
    }
    return r < pb ? FISH_DEPTH_BEHIND : FISH_DEPTH_FRONT;
}
/* ... its depths changed to g, p, every piece it is on stays before or behind it as it was (pieces_keep) */
bool fish_pieces_keep(const tank_t *t, const fish_t *f, int g, int p) {
    float hw = 34 * f->size + 6, hh = 26 * f->size + 6;
    return pieces_keep(t, f->x - hw, f->x + hw, f->y + hh, f->gdepth, f->pdepth, g, p);
}
/* ... clear of every bed of back grass (a frond would pop over or off it if the depth changed here) */
bool grass_clear(const tank_t *t, const fish_t *f) {
    float hw = 34 * f->size + 6, hh = 26 * f->size + 6;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (t->veg_growth[b] < VEG_BARE) continue;   /* (every bed: the sword plant IN FRONT too, 0.3.9.4) */
        float x0, x1, top; int n; tank_veg_bed(t, b, &x0, &x1, &top, &n);
        if (f->x + hw > x0 - 8 && f->x - hw < x1 + 8 && f->y + hh > top) return false;
    }
    return true;
}
