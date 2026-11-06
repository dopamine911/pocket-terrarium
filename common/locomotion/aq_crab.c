/* aq_crab.c - the strawberry crab (SD_ITEM_CRAB)
 * (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
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

/* ---- the strawberry crab (0.3.9, SD_ITEM_CRAB; tank.h has the rules) ----
 * A slow crab's walk: CR_WALK_PX_S, the gait advancing with the ground
 * covered so its planted feet never slide (render: a stance sweeps 3.2 px
 * through 0.6 of a cycle). Where it stops it digs and nibbles at the sand
 * (CR_DIG) or just sits (CR_REST), for minutes. After CR_OUTING_S out in the
 * open it looks for a shelter - the castle's arch or the volcano's cave,
 * whichever is in the water (the nearer when both) - walks in and ROOSTS
 * there facing the glass (render: the front view) for CR_ROOST_S, longer
 * with the light off. A pellet resting on the floor brings it out from
 * anywhere but the flank: it walks to it and eats it over CR_EAT_S (the
 * shrimp may get there first; the crab takes what is left). A tap raises
 * its claws for CR_POKE_S (a walk stops there; a roost stays a roost). The
 * climb: once CR_CLIMB_EVERY_S have passed it takes the next walk to the
 * volcano's foot and goes up the near flank to the crater rim and down again
 * (CR_CLIMB_S each way, a pause at the top), its feet on tank_volcano_half's
 * rock. Asleep (tank_sleep) it does nothing: a boot finds it where it stopped. */
#define CR_WALK_PX_S    5.0f     /* a slow crab (Strato: "doesn't move around too much or too fast") */
#define CR_FEED_PX_S    12.0f    /* ... but with a pellet on the sand it has some pep (Strato 2026-10-24: "quicker ... less urgent than fleeing") */
#define CR_GAIT_PER_PX  1.178f   /* radians of gait per px walked: 2 pi / (3.2 px / 0.6) - the stance's sweep */
#define CR_TAP_RADIUS   44.0f    /* round its body's middle, 10 px over its feet: as generous as the snail's (48) and the urchin's (40). 28 until
                                    2026-10-24 - Strato on the 1.8: "I can't seem to tap the crab ... never when in the castle". The 1.8's panel
                                    runs out ~35 px above the picture's foot (touch_port: a finger on the floor reads y 333 at best), so a tap
                                    on the crab in a front castle (its middle at TANK_BOT - 18) read 31 px above it - outside 28, inside 44;
                                    on open sand it stands 7 px higher and the same finger landed inside. The fish are tested first (38 px). */
#define CR_POKE_S       2.2f
#define CR_TAP_GAP_S    1.2f     /* taps closer than this count together: the third sends it running */
#define CR_SCURRY_PX_S  36.0f    /* scared: to the nearer shelter at a run - the crab's fastest motion (Strato 2026-10-24: "fleeing should be the fastest"; 24 before) */
#define CR_REST_S_LO    12.0f    /* shorter than the hermit's (Strato: motionless too long), and alive meanwhile: crab_fidget */
#define CR_REST_S_HI    40.0f
#define CR_DIG_S_LO     15.0f
#define CR_DIG_S_HI     40.0f
#define CR_FIDGET_LO    2.5f     /* s between its small doings */
#define CR_FIDGET_HI    6.0f
#define CR_SNAIL_GAP    40.0f    /* the snail on the sand this near: a standoff, then it steps aside (the snail turns back at SNAIL_CRAB_GAP 34,
                                  * so the crab must notice it FIRST - 30 was never reached, 2026-10-24) */
#define CR_WALK_LO      25.0f    /* px, a short walk */
#define CR_WALK_HI      90.0f
#define CR_OUTING_S     300.0f   /* ~5 min out, then a shelter (Strato: out of the castle too long) */
#define CR_ROOST_S_LO   600.0f   /* 10..30 min roosting; halved with the light off (CR_NIGHT_OUT) */
#define CR_ROOST_S_HI   1800.0f
#define CR_EAT_S        7.0f

#define CR_CLIMB_EVERY_S (3 * 3600.0f)
#define CR_CLIMB_S      75.0f    /* each way */
#define CR_TOP_S        25.0f
#define CR_SLOPE_W      4        /* the climb's footing and slope: the silhouette averaged this many cells either side (0.3.9.1) */
#define CR_CLIMB_EASE   12.0f    /* ... and eased toward, per second: ~95% of a change in a quarter second */
#define CR_OVER_DX      36.0f    /* a fish this near it sideways ... (Strato 10-24; 24 until 0.3.9.1) */
#define CR_OVER_DY      60.0f    /* ... and from CR_OVER_LOW under its middle (a fish down on the sand beside it, eating) to this far over it:
                                    claws up for CR_OVER_S, once per CR_OVER_COOL_S. 0.3.9 asked for a fish 0..34 px OVER its middle alone, a band
                                    fish almost never cross (2 times an hour in a lived-in sim tank; Strato never saw it on the glass) */
#define CR_OVER_LOW     14.0f
#define CR_OVER_S       1.0f
#define CR_OVER_COOL_S  20.0f
#define CR_NIGHT_OUT    3.0f     /* a night crab: outings this many times longer with the light off, roosts half as long */
#define CR_NEAR_PX      16.0f    /* this near its shelter's x: its feet ease onto the piece's own floor line */
#define VOLCANO_BASE_CELL 4.0f   /* the cone's foot (render.c VO_BASE), cell rows up from the sprite's bottom */
#define VOLCANO_TOP_CELL  100.0f /* the far rim's top (VO_TOP) */
float tank_volcano_half(float y, int s) {                        /* render.c vo_half, the same rock (sinf for its fast_sin) */
    float u = (y - VOLCANO_BASE_CELL) / (VOLCANO_TOP_CELL - VOLCANO_BASE_CELL);
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    float h = 11.0f + 35.0f * powf(1 - u, 0.85f);
    float g = s < 0 ? (u - 0.32f) / 0.16f : (u - 0.58f) / 0.12f;
    h += (s < 0 ? 5.0f : 2.5f) * expf(-g * g);
    h += 1.2f * sinf(y * 0.47f + (s < 0 ? 0.3f : 2.1f)) + 0.8f * sinf(y * 0.19f + (s < 0 ? 1.7f : 0.2f));
    h += (float)((int)(chash((int)y >> 2, s + 5) % 3) - 1) * 0.6f;
    return h;
}
static float crab_lo(void) { return TANK_FX0 + DECOR_MARGIN + CR_HALF_W; }
static float crab_hi(void) { return TANK_FX1 - DECOR_MARGIN - CR_HALF_W; }
static void crab_mode(tank_t *t, int mode) {
    if (t->cr_mode != mode) { t->cr_mode_s = 0; t->cr_fidget_kind = CR_FID_NONE; t->cr_fidget = 0; t->cr_next_fidget = 1.5f; t->cr_scurry = false; }
    t->cr_mode = (uint8_t)mode;
}
bool tank_crab_walking(const tank_t *t) { return t->cr_mode == CR_WALK || t->cr_mode == CR_SHELTER || t->cr_mode == CR_FEED || t->cr_mode == CR_CLIMB || t->cr_fidget_kind == CR_FID_SHUFFLE; }
/* the snail, upright on the sand (its floor walk): where; false = on the glass, or none */
static bool crab_snail_x(const tank_t *t, float *x) {
    if (!(t->sd_unlocks & SD_ITEM_SNAIL) || t->snail_x < 0 || t->snail_y < SNAIL_FLOOR_Y - 1) return false;
    *x = t->snail_x; return true;
}
/* the crab on the sand (not on the volcano's flank): where; false = none / climbing (the snail's turn-back, snail_tick) */
bool crab_floor_x(const tank_t *t, float *x) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0 || t->cr_mode == CR_CLIMB) return false;
    *x = t->cr_x; return true;
}
bool tank_crab_hit(const tank_t *t, float x, float y) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0) return false;
    return tank_dist(t->cr_x, CR_FLOOR_Y - t->cr_y - 10, x, y) <= CR_TAP_RADIUS;
}
/* a sand dollar held up (0.3.9.4, Dragonshadow on Discord: "The snail is in the way. I can only tap the snail ... then
   have the shrimp come over and block me. Before I can tap the coin it disappears"): while the crab holds one its tap
   comes FIRST - before the fish, the snail, the shrimp and the rest - on the crab or round the coin over its head
   (28 px over its feet; the circle reaches the 1.8's high reading of a floor tap) */
bool tank_crab_find_hit(const tank_t *t, float x, float y) {
    if (t->cr_find <= 0) return false;
    return tank_crab_hit(t, x, y) || tank_dist(t->cr_x, CR_FLOOR_Y - t->cr_y - 28, x, y) <= CR_TAP_RADIUS;
}
/* a pellet resting on the floor, the nearest; -1 = none */
static int crab_pellet(const tank_t *t) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < MAX_FOOD; i++) {
        const food_t *p = &t->food[i];
        if (!p->alive || p->floor_s <= 0) continue;
        float d = fabsf(p->x - t->cr_x);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
/* the shelter it would take: the FRONTMOST of the pieces it roosts in that are in the water, by the depth scheme
   (tank_decor_above: the greater depth, then the higher rank), wherever they stand; -1 = none. Strato 2026-11-02, the
   watch: the crab "roosting in the volcano" with the castle standing in front of it - the nearer one, as it was, put it
   in a piece another covered (a volcano BEHIND: the castle IN FRONT over the crab; both IN FRONT with the castle ranked
   above: the crab in the front slot over the castle). "The crab should always choose the uppermost roosting spot."
   A new shelter joins CR_SHELTERS. */
static const int CR_SHELTERS[] = { 2, 9 };           /* the castle's arch, the volcano's cave */
static bool g_dbg_roost_nearer;                      /* (tests: the 0.3.9 pick - the nearer shelter, blind to the depth) */
void tank_debug_roost_nearer(bool on) { g_dbg_roost_nearer = on; }
static int crab_cave(const tank_t *t) {
    int best = -1;
    for (int k = 0; k < (int)(sizeof CR_SHELTERS / sizeof CR_SHELTERS[0]); k++) {
        const int c = CR_SHELTERS[k];
        if (!tank_decor_in(t, c)) continue;
        if (best < 0 || (g_dbg_roost_nearer ? fabsf(tank_decor_x(t, c) - t->cr_x) < fabsf(tank_decor_x(t, best) - t->cr_x)
                                            : tank_decor_above(t, c, best))) best = c;
    }
    return best;
}
/* where it sits in shelter c: the piece's middle, inside the walls it walks between */
static float crab_cave_x(const tank_t *t, int c) { return clampf(tank_decor_x(t, c), crab_lo(), crab_hi()); }
/* a find is due (0.3.9.1): the wait is over, none held up, the light on and the keeper here (tank.h CR_FIND_PRESENT_S) */
static bool crab_find_due(const tank_t *t) {
    return t->cr_find_cool <= 0 && t->cr_find <= 0 && !t->night && t->idle_s < CR_FIND_PRESENT_S;
}
static void crab_set_walk(tank_t *t, float to, int mode) {
    float sx;
    if (mode == CR_WALK && t->cr_cave < 0 && crab_snail_x(t, &sx) && fabsf(to - sx) < CR_SNAIL_GAP) to = t->cr_x - (to - t->cr_x);   /* not onto the snail: the other way */
    t->cr_to = clampf(to, crab_lo(), crab_hi());
    if (fabsf(t->cr_to - t->cr_x) > 0.5f) t->cr_facing = t->cr_to > t->cr_x ? 1 : -1;
    /* its lane for the walk and the stop (tank.h THE LANES): rolled in the clear; a walk that will end in a dig
       takes the front lane (a dig is always in front); inside a piece's footprint the lane holds, and a dig
       that would need the other lane waits for the next stop */
    const bool dig = tank_randf(t, 0, 1) < 0.6f || crab_find_due(t); const float r = tank_randf(t, 0, 1);   /* both rolls always drawn: the RNG's run is the same with or without a piece;
                                                                                                                 a find due: a dig, always */
    t->cr_next_dig = mode == CR_WALK && t->cr_cave < 0 && dig;
    if (tank_floor_clear(t, t->cr_x, CR_HALF_W)) t->cr_lane_front = t->cr_next_dig || r < LANE_FRONT_ODDS;
    else if (!t->cr_lane_front) t->cr_next_dig = false;
    t->cr_gdepth_want = t->cr_next_dig ? FISH_DEPTH_FRONT : critter_depth_roll(t, t->cr_x, 0x3c3u);   /* SWIM DEPTH: a dig in front, as its lane */
    crab_mode(t, mode);
}
/* going for pellet f: a shrimp of the school after it, on the sand within CR_SHOO_PX of its body's edge - claws up, the
   shrimp near it flee, the pellet is the crab's (tank.h, THE CRAB'S PELLET above). True when it shooed them. */
static bool crab_shoo(tank_t *t, int f) {
    if (!(t->sd_unlocks & SD_ITEM_SHRIMP) || t->shrimp_fp != f) return false;
    for (int i = 0; i < t->shrimp_n; i++) {
        const shrimp_t *q = &t->shrimp[i];
        if (FOOD_FLOOR_Y - q->y > CR_SHOO_BAND || fabsf(q->x - t->cr_x) - CR_HALF_W > CR_SHOO_PX) continue;
        t->cr_claim = (int8_t)f; t->cr_shoo = CR_SHOO_S; t->cr_poke = CR_SHOO_POKE;
        shrimp_scatter(t, t->cr_x, FOOD_FLOOR_Y - 4, CR_HALF_W + 30);   /* the ones at it dart off tail first; the school leaves the pellet */
        t->shrimp_fp = -1;
        /* ... and keeps off: its next goal well past the crab on their side, held while the crab walks on and eats (a half-second
           dart alone sent them straight back through its shell - Strato on the square: "it did not look that way") */
        float sx = 0; for (int j = 0; j < t->shrimp_n; j++) sx += t->shrimp[j].x;
        float away = sx / t->shrimp_n >= t->cr_x ? 1.0f : -1.0f, go = tank_randf(t, CR_SHOO_AWAY_LO, CR_SHOO_AWAY_HI);
        const float zx = t->food[f].x, reach = fmaxf(CR_SHOO_AWAY_LO, CR_SHOO_ZONE_PX + 20);
        if ((away > 0 ? TANK_FX1 - 40 - fmaxf(t->cr_x, zx) : fminf(t->cr_x, zx) - (TANK_FX0 + 40)) < reach) away = -away;   /* no room that side: the other */
        t->shrimp_tx = clampf((away > 0 ? fmaxf(t->cr_x, zx) : fminf(t->cr_x, zx)) + away * fmaxf(go, CR_SHOO_ZONE_PX + 20), TANK_FX0 + 40, TANK_FX1 - 40);
        t->shrimp_avoid_side = (int8_t)away;
        t->shrimp_ty = FOOD_FLOOR_Y - tank_randf(t, 14, 30);
        t->shrimp_tt = tank_randf(t, CR_SHOO_KEEP_LO_S, CR_SHOO_KEEP_HI_S);
        t->shrimp_flee = CR_SHOO_FLEE_S;
        t->shrimp_avoid_x = t->food[f].x; t->shrimp_avoid_s = CR_SHOO_ZONE_S;
        return true;
    }
    return false;
}
/* a step toward x at a crab's pace, facing that way; true once there */
static bool crab_step(tank_t *t, float to, float dt) {
    float d = to - t->cr_x;
    if (fabsf(d) > 0.5f) t->cr_facing = d > 0 ? 1 : -1;
    float step = fminf(fabsf(d), (t->cr_scurry ? CR_SCURRY_PX_S : t->cr_mode == CR_FEED ? CR_FEED_PX_S : CR_WALK_PX_S) * dt);
    t->cr_x += (d > 0 ? 1 : -1) * step;
    t->cr_gait += step * CR_GAIT_PER_PX;
    if (t->cr_gait > 1000) t->cr_gait -= 1000;
    return fabsf(d) <= 0.5f;
}
static bool crab_advance(tank_t *t, float dt) { return crab_step(t, t->cr_to, dt); }
/* scared (three quick taps on it, or the tank's triple-tap startle anywhere: tank_startle): it runs for its shelter
   (the frontmost of the castle and the volcano, crab_cave; the nearer until 0.4), claws up, and ducks into the dark there; already in it, it backs into the dark;
   with neither in the water, away from the finger (fx). Strato 10-23 */
void tank_crab_scare(tank_t *t, float fx) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0 || t->cr_mode == CR_CLIMB) return;
    t->cr_poke = 0; t->cr_food = -1;
    int c = crab_cave(t);
    if (c >= 0 && fabsf(t->cr_x - tank_decor_x(t, c)) < 3) {      /* already in it: it backs into the dark (10-23: a scare in the roost showed nothing) */
        t->cr_cave = (int8_t)c; crab_mode(t, CR_ROOST);
        t->cr_hide = tank_randf(t, 20, 40); if (t->cr_timer < t->cr_hide + 60) t->cr_timer = t->cr_hide + 60;
        return;
    }
    if (c >= 0) { t->cr_cave = (int8_t)c; crab_set_walk(t, tank_decor_x(t, c), CR_SHELTER); }
    else {                                                        /* nowhere to hide: away from the finger */
        float away = fx > t->cr_x ? -1.0f : 1.0f, to = t->cr_x + away * 70;
        if (to < crab_lo() || to > crab_hi()) to = t->cr_x - away * 70;
        t->cr_cave = -1; t->cr_to = clampf(to, crab_lo(), crab_hi()); crab_mode(t, CR_WALK);
    }
    t->cr_scurry = true;
}
int tank_crab_tap(tank_t *t, float fx) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0) return 0;
    if (t->cr_find > 0) {                                             /* it holds a sand dollar up: the tap TAKES it (the ledger pays SD_CRAB_FIND) */
        t->cr_find = 0; if (t->cr_found < 65535) t->cr_found++;
        t->cr_find_cool = tank_randf(t, CR_FIND_NEXT_LO_S, CR_FIND_NEXT_HI_S);
        t->cr_poke = CR_POKE_S; t->cr_taps = 0; t->cr_tap_t = t->clock;
        tank_emit(TEV_CRAB_COIN, -1);                                 /* (the coin, 0.3.9.3) */
        return 0;
    }
    t->cr_taps = (t->clock - t->cr_tap_t <= CR_TAP_GAP_S && t->cr_tap_t <= t->clock) ? (uint8_t)(t->cr_taps + 1) : 1;
    t->cr_tap_t = t->clock;
    if (t->cr_mode == CR_CLIMB) return t->cr_taps;                   /* on the flank it holds on */
    if (t->cr_taps >= 3) { tank_crab_scare(t, fx); return t->cr_taps; }   /* three quick taps on it */
    t->cr_poke = CR_POKE_S;
    if (t->cr_mode == CR_WALK || t->cr_mode == CR_SHELTER) { t->cr_cave = -1; crab_mode(t, CR_REST); t->cr_timer = tank_randf(t, 4, 8); }   /* it stops and stands its ground */
    return t->cr_taps;
}
static void crab_stop(tank_t *t) {                             /* arrived: dig here (decided at the walk's start, with its lane), or just sit */
    if (t->cr_next_dig && t->cr_lane_front) { crab_mode(t, CR_DIG); t->cr_timer = tank_randf(t, CR_DIG_S_LO, CR_DIG_S_HI); }
    else { crab_mode(t, CR_REST); t->cr_timer = tank_randf(t, CR_REST_S_LO, CR_REST_S_HI); }
    t->cr_next_dig = false;
}
/* its small life while it rests, digs or roosts (tank.h CR_FID_*), and a step aside from the snail */
static void crab_fidget(tank_t *t, float dt) {
    float sx;
    const bool snail_near = crab_snail_x(t, &sx) && fabsf(sx - t->cr_x) < CR_SNAIL_GAP;
    if (t->cr_stood && (!snail_near && !(crab_snail_x(t, &sx) && fabsf(sx - t->cr_x) < CR_SNAIL_GAP + 10))) t->cr_stood = false;   /* the snail went: the next approach is new */
    if (t->cr_mode != CR_ROOST && !t->cr_stood && t->cr_fidget_kind != CR_FID_SHUFFLE && t->cr_fidget_kind != CR_FID_STANDOFF && snail_near) {
        t->cr_fidget_kind = CR_FID_STANDOFF; t->cr_fidget = CR_STANDOFF_S;   /* the snail is here: it notices - claws up at it first (Strato 10-24) ... */
        t->cr_poke = CR_STANDOFF_S; t->cr_facing = sx > t->cr_x ? 1 : -1;
    }
    if (t->cr_fidget_kind == CR_FID_STANDOFF) {
        t->cr_fidget -= dt;
        if (t->cr_fidget > 0) return;
        t->cr_stood = true; t->cr_fidget_kind = CR_FID_NONE;          /* once per approach: after this it holds its ground, and the snail turns back (snail_tick) */
        if (!crab_snail_x(t, &sx)) return;                            /* (it crawled onto the glass meanwhile) */
        float away = sx > t->cr_x ? -1.0f : 1.0f;                     /* ... and then it steps aside, never across it */
        float to = t->cr_x + away * (CR_SNAIL_GAP + 8 - fabsf(sx - t->cr_x));
        if (to < crab_lo() || to > crab_hi()) return;                 /* a wall behind it: it stands its ground, claws up */
        t->cr_shuffle_to = to; t->cr_fidget_kind = CR_FID_SHUFFLE; t->cr_fidget = 0;
    }
    if (t->cr_fidget_kind == CR_FID_SHUFFLE) {
        if (crab_snail_x(t, &sx) && fabsf(sx - t->cr_shuffle_to) < CR_SNAIL_GAP - 8 && fabsf(sx - t->cr_x) < fabsf(sx - t->cr_shuffle_to) + 1)
            { t->cr_fidget_kind = CR_FID_NONE; return; }                /* the snail got in the way of a shuffle: it stops short */
        if (crab_step(t, t->cr_shuffle_to, dt)) { t->cr_fidget_kind = CR_FID_NONE; t->cr_next_fidget = tank_randf(t, CR_FIDGET_LO, CR_FIDGET_HI); }
        return;
    }
    if (t->cr_fidget_kind != CR_FID_NONE) {
        t->cr_fidget -= dt;
        if (t->cr_fidget <= 0) { t->cr_fidget_kind = CR_FID_NONE; t->cr_fidget = 0; }
        return;
    }
    t->cr_next_fidget -= dt;
    if (t->cr_next_fidget > 0) return;
    t->cr_next_fidget = tank_randf(t, CR_FIDGET_LO, CR_FIDGET_HI);
    if (t->cr_mode == CR_ROOST) { t->cr_fidget_kind = CR_FID_STRETCH; t->cr_fidget = CR_STRETCH_S; t->cr_next_fidget *= 1.6f; return; }
    float r = tank_randf(t, 0, 1);
    if (t->cr_mode == CR_DIG) r = 0.4f + 0.6f * r;                    /* digging, its claw is busy: a shuffle or a turn */
    if (r < 0.40f) { t->cr_fidget_kind = CR_FID_GROOM; t->cr_fidget = CR_GROOM_S; }
    else if (r < 0.65f) {                                              /* a few steps */
        float to = t->cr_x + tank_randf(t, 3, 9) * (tank_randf(t, 0, 1) < 0.5f ? -1 : 1);
        if (crab_snail_x(t, &sx) && fabsf(to - sx) < CR_SNAIL_GAP) to = t->cr_x - (to - t->cr_x);
        t->cr_shuffle_to = clampf(to, crab_lo(), crab_hi()); t->cr_fidget_kind = CR_FID_SHUFFLE;
    } else if (r < 0.82f) t->cr_facing = (int8_t)-t->cr_facing;        /* it turns about */
    else { t->cr_fidget_kind = CR_FID_SETTLE; t->cr_fidget = CR_SETTLE_S; }
}
void crab_tick(tank_t *t, float dt) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB)) return;
    if (t->cr_x < 0) tank_crab_place(t);
    t->cr_since_climb += dt; t->cr_mode_s += dt;
    if (t->cr_poke > 0) t->cr_poke -= dt;
    if (t->cr_full > 0) t->cr_full -= dt;
    if (t->cr_claim >= 0 && (!t->food[t->cr_claim].alive || t->cr_food != t->cr_claim || (t->cr_mode != CR_FEED && t->cr_mode != CR_EAT)))
        t->cr_claim = -1;                                             /* eaten, gone, or it went off: the shrimp may have it */
    if (t->cr_hide > 0) t->cr_hide = t->cr_mode == CR_ROOST ? t->cr_hide - dt : 0;
    {   /* backing into the dark and coming out again: a motion, never a switch (Strato 10-24, the square: "it looked more
           like a light turned off"); the hide holds it back, its end lets it forward */
        const float to = (t->cr_mode == CR_ROOST && t->cr_hide > 0) ? 1.0f : 0.0f;
        const float step = dt / (to > t->cr_back ? CR_BACK_IN_S : CR_BACK_OUT_S);
        if (t->cr_back < to) { t->cr_back += step; if (t->cr_back > to) t->cr_back = to; }
        else if (t->cr_back > to) { t->cr_back -= step; if (t->cr_back < to) t->cr_back = to; }
    }
    if (t->cr_startle_cool > 0) t->cr_startle_cool -= dt;
    if (t->cr_find <= 0 && t->cr_find_cool > 0) t->cr_find_cool = fmaxf(0, t->cr_find_cool - dt);   /* the wait for the next find (tank_tick_sleep runs it asleep) */
    if (t->cr_mode != CR_ROOST && t->cr_mode != CR_SHELTER && t->cr_mode != CR_CLIMB) t->cr_outing += dt;
    /* a night crab (Strato 10-24): the light going off brings it out of its roost soon; the light coming on
       sends it in soon (both at a crab's pace: a minute or two, never on the switch) */
    if (t->night != t->cr_was_night) {
        if (t->night && t->cr_mode == CR_ROOST && t->cr_hide <= 0) { float soon = tank_randf(t, 20, 90); if (t->cr_timer > soon) t->cr_timer = soon; }
        if (!t->night && (t->cr_mode == CR_REST || t->cr_mode == CR_DIG || t->cr_mode == CR_WALK)) {
            float left = tank_randf(t, 30, 120); if (t->cr_outing < CR_OUTING_S - left) t->cr_outing = CR_OUTING_S - left;
        }
        t->cr_was_night = t->night;
    }
    /* a fish passing over it: claws up (its own small drama, dozens of times a day) */
    if (t->cr_mode != CR_CLIMB && t->cr_startle_cool <= 0 && t->cr_poke <= 0 && t->cr_find <= 0) {
        const float my = CR_FLOOR_Y - t->cr_y - 10;
        for (int i = 0; i < t->n_fish; i++) {
            const fish_t *f = &t->fish[i];
            float dy = my - f->y;
            if (fabsf(f->x - t->cr_x) < CR_OVER_DX && dy > -CR_OVER_LOW && dy < CR_OVER_DY) { t->cr_poke = CR_OVER_S; t->cr_startle_cool = CR_OVER_COOL_S; break; }
        }
    }
    /* its feet on its shelter's own floor line (a piece IN FRONT stands 7 px lower than the open sand): eased, never a jump */
    if (t->cr_mode != CR_CLIMB) {
        float to = 0;
        if (t->cr_cave >= 0 && (t->cr_mode == CR_ROOST || t->cr_mode == CR_SHELTER) && tank_decor_in(t, t->cr_cave) && fabsf(t->cr_x - tank_decor_x(t, t->cr_cave)) < CR_NEAR_PX)
            to = CR_FLOOR_Y - tank_decor_fy(t, t->cr_cave);
        else if ((t->cr_lane_front || t->cr_mode == CR_DIG) && t->cr_mode != CR_ROOST && t->cr_mode != CR_SHELTER && !tank_floor_clear(t, t->cr_x, CR_HALF_W - 8))
            to = CR_FLOOR_Y - (float)DECOR_FY_FRONT;                /* its front lane inside a piece IN FRONT's footprint: on that piece's base line (Strato 10-24) */
        t->cr_y += (to - t->cr_y) * fminf(1.0f, 6.0f * dt);
        if (fabsf(t->cr_y - to) < 0.05f) t->cr_y = to;
    }
    /* food on the floor comes first (Strato: "it should come out for food when food reaches the ground") - but not
       from the dark it just ran into, and not with a sand dollar in its claw */
    if (t->cr_mode != CR_CLIMB && t->cr_mode != CR_EAT && t->cr_mode != CR_FEED && !t->cr_scurry && t->cr_hide <= 0 && t->cr_find <= 0 && t->cr_full <= 0) {   /* (full: CR_MEALS_FULL) */
        int f = crab_pellet(t);
        if (f >= 0) { t->cr_food = (int8_t)f; t->cr_cave = -1; crab_set_walk(t, t->food[f].x - CR_REACH * (t->food[f].x > t->cr_x ? 1 : -1), CR_FEED); }
    }
    switch (t->cr_mode) {
    case CR_FEED: {
        int f = t->cr_food;
        if (f < 0 || !t->food[f].alive) { t->cr_food = -1; crab_stop(t); return; }
        if (t->cr_shoo > 0) { t->cr_shoo -= dt; return; }              /* claws up at the shrimp: it stands its ground a beat */
        if (t->cr_claim != f && crab_shoo(t, f)) return;
        t->cr_to = clampf(t->food[f].x - CR_REACH * (t->food[f].x > t->cr_x ? 1 : -1), crab_lo(), crab_hi());
        if (crab_advance(t, dt) || fabsf(t->food[f].x - t->cr_x) <= CR_REACH + 2) {
            t->cr_facing = t->food[f].x > t->cr_x ? 1 : -1; crab_mode(t, CR_EAT); t->cr_timer = CR_EAT_S;
            t->cr_claim = (int8_t)f;                                   /* in its claw: not the shrimp's */
        }
        return; }
    case CR_EAT: {
        int f = t->cr_food;
        if (f < 0 || !t->food[f].alive) { t->cr_food = -1; crab_stop(t); return; }
        t->food[f].nibbled += dt; t->cr_timer -= dt;
        if (t->cr_timer <= 0) {
            t->food[f].alive = false; t->cr_food = -1; if (t->cr_eaten < 65535) t->cr_eaten++;
            if (++t->cr_meals >= CR_MEALS_FULL) { t->cr_meals = 0; t->cr_full = tank_randf(t, CR_FULL_LO_S, CR_FULL_HI_S); }   /* full a while */
            crab_stop(t);
        }
        return; }
    case CR_ROOST:
        crab_fidget(t, dt);
        t->cr_timer -= dt;
        if (crab_find_due(t) && t->cr_hide <= 0 && t->cr_timer > CR_FIND_COME_S) t->cr_timer = CR_FIND_COME_S;   /* a find due, the keeper here: it comes out to dig */
        if (t->cr_cave >= 0 && !tank_decor_in(t, t->cr_cave)) { t->cr_cave = -1; t->cr_timer = 0; }   /* the keeper took the piece away: it is on the open sand */
        else if (t->cr_cave >= 0 && t->cr_hide <= 0 && t->cr_timer > 0 && !g_dbg_roost_nearer) {        /* the frontmost changed under it (a piece bought or back from
                                                                                                           the shelf, a depth or a rank changed - a SWAP IN, too) or its
                                                                                                           piece moved: it goes over, once out of the dark */
            const int c = crab_cave(t);
            if (c != t->cr_cave || fabsf(t->cr_x - crab_cave_x(t, c)) > CR_NEAR_PX) { t->cr_cave = (int8_t)c; crab_set_walk(t, tank_decor_x(t, c), CR_SHELTER); return; }
        }
        if (t->cr_timer <= 0) { t->cr_cave = -1; t->cr_outing = 0; crab_set_walk(t, t->cr_x + tank_randf(t, CR_WALK_LO, CR_WALK_HI) * (tank_randf(t, 0, 1) < 0.5f ? -1 : 1), CR_WALK); }
        return;
    case CR_SHELTER:
        if (t->cr_cave < 0 || !tank_decor_in(t, t->cr_cave)) { t->cr_cave = -1; crab_stop(t); return; }
        if (!g_dbg_roost_nearer) t->cr_cave = (int8_t)crab_cave(t);                                        /* on the way: always for the frontmost (the order may change) */
        t->cr_to = crab_cave_x(t, t->cr_cave);
        if (crab_advance(t, dt)) {
            bool ran = t->cr_scurry;
            crab_mode(t, CR_ROOST); t->cr_timer = tank_randf(t, CR_ROOST_S_LO, CR_ROOST_S_HI) * (t->night ? 0.5f : 1);   /* a night crab: half as long with the light off */
            if (ran) t->cr_hide = tank_randf(t, 12, 20);              /* in at a run: it ducks into the dark a while */
        }
        return;
    case CR_CLIMB: {
        /* THE CLIMB, smoothed (0.3.9.1; Strato 2026-10-24 after a staged one on his 1.8: "still jerky ... smooth that journey
           out. Have it stop along the way and nibble the volcanic rocks"). Until then its footing, height and tilt were read off
           tank_volcano_half ROW BY ROW - every jagged boulder edge (the silhouette's 4-cell hash step, its sines) turned the whole
           crab at once. Now: the rock under it is a silhouette averaged over CR_SLOPE_W cells either side, the slope the difference
           of the two halves of that window, and footing, height and tilt all EASE toward their targets (CR_CLIMB_EASE a second).
           Three stops on the way (CR_NIBBLE_AT: two up, one down): the climb waits CR_NIBBLE_S while it turns to the rock and
           nibbles (render: the dig's pick against the rock, moss on the claw). */
        const float vx = tank_decor_x(t, 9), fy = tank_decor_fy(t, 9);
        if (!tank_decor_in(t, 9)) { t->cr_y = 0; t->cr_tilt = 0; t->cr_climb = 0; t->cr_nibble = 0; crab_stop(t); return; }
        const float top = CR_TOP_S / CR_CLIMB_S;
        static const float CR_NIBBLE_AT[3] = { 0.30f, 0.64f, 1.0f + 25.0f / 75.0f + 0.48f };                  /* (the last: half way down; CR_TOP_S / CR_CLIMB_S inline) */
        if (t->cr_nibble > 0) {                                                                               /* a stop: nibbling */
            t->cr_nibble -= dt;
            if (t->cr_nibble < 0) t->cr_nibble = 0;
        } else {
            t->cr_climb += dt / CR_CLIMB_S;
            if (t->cr_nibble_i < 3 && t->cr_climb >= CR_NIBBLE_AT[t->cr_nibble_i]) { t->cr_nibble = CR_NIBBLE_S; t->cr_nibble_i++; }
            if (t->cr_climb < 1 || t->cr_climb >= 1 + top) t->cr_gait += 3.0f * dt;                          /* striding up and down; still at the top and at a stop */
        }
        float up = t->cr_climb < 1 ? t->cr_climb : t->cr_climb < 1 + top ? 1 : 2 + top - t->cr_climb;
        if (up <= 0 && t->cr_climb > 1) { t->cr_y = 0; t->cr_tilt = 0; t->cr_climb = 0; t->cr_nibble = 0; t->cr_since_climb = 0; crab_stop(t); return; }
        const float py = VOLCANO_BASE_CELL + up * (VOLCANO_TOP_CELL - 10 - VOLCANO_BASE_CELL);                /* its height in cells: the near rim, under the far rim's top */
        float lo = 0, hi = 0, all = 0;                                                                        /* the rock, smoothed: CR_SLOPE_W cells below and above */
        for (int k = 1; k <= CR_SLOPE_W; k++) { lo += tank_volcano_half(py - k, t->cr_side); hi += tank_volcano_half(py + k, t->cr_side); }
        all = (lo + hi + tank_volcano_half(py, t->cr_side)) / (2 * CR_SLOPE_W + 1); lo /= CR_SLOPE_W; hi /= CR_SLOPE_W;
        float run = (lo - hi) * 2.0f / (CR_SLOPE_W + 1); if (run < 0) run = 0;                                /* the flank's run per 2 px of rise, over the window */
        const float tx = vx + t->cr_side * all * 2.0f, ty = (CR_FLOOR_Y) - (fy - py * 2.0f), tt = atan2f(t->cr_side * 2.0f, run);
        const float a = 1 - expf(-CR_CLIMB_EASE * dt);                                                        /* the ease: most of the way in a quarter second */
        t->cr_x += (tx - t->cr_x) * a; t->cr_y += (ty - t->cr_y) * a; t->cr_tilt += (tt - t->cr_tilt) * a;
        t->cr_facing = t->cr_nibble > 0 ? -t->cr_side : (t->cr_climb < 1) ? -t->cr_side : t->cr_side;         /* up and nibbling: toward the rock; down: away */
        return; }
    case CR_DIG:
    case CR_REST:
        if (t->cr_find > 0) {                                                                                 /* holding its find up for the keeper: it stands there, glinting */
            t->cr_find -= dt;
            if (t->cr_find <= 0) { t->cr_find = 0; t->cr_find_cool = tank_randf(t, CR_FIND_MISSED_LO_S, CR_FIND_MISSED_HI_S); }   /* untaken: buried again, another chance sooner */
            return;
        }
        if (t->cr_mode == CR_DIG && crab_find_due(t)) {                                                       /* the pick comes up with a SAND DOLLAR (render: held up over the fronds) */
            float u = fmodf(t->cr_mode_s, CR_PICK_S) / CR_PICK_S;
            if (u >= 0.42f && u < 0.5f) { t->cr_find = CR_FIND_HOLD_S; t->cr_fidget_kind = CR_FID_NONE; t->cr_fidget = 0; t->cr_poke = 0; tank_emit(TEV_CRAB_FIND, -1); return; }   /* (its cue, 0.3.9.1) */
        }
        crab_fidget(t, dt);
        t->cr_timer -= dt;
        if (t->cr_mode == CR_REST && crab_find_due(t) && t->cr_timer > 4) t->cr_timer = 4;                     /* a find due: up and off to dig */
        if (t->cr_timer > 0 || t->cr_fidget_kind == CR_FID_SHUFFLE || t->cr_fidget_kind == CR_FID_STANDOFF) return;
        if (crab_find_due(t)) {                                                                               /* ... no shelter, no climb: the next walk ends in a dig */
            if (t->cr_mode == CR_DIG) { t->cr_timer = tank_randf(t, CR_DIG_S_LO, CR_DIG_S_HI); return; }     /* (a dig that missed the pick: another round) */
            crab_set_walk(t, t->cr_x + tank_randf(t, CR_WALK_LO, CR_WALK_HI) * (tank_randf(t, 0, 1) < 0.5f ? -1 : 1), CR_WALK);
            return;
        }
        if (t->cr_outing >= CR_OUTING_S * (t->night ? CR_NIGHT_OUT : 1)) {                                    /* time for a shelter (a night crab stays out longer in the dark) */
            int c = crab_cave(t);
            if (c >= 0) { t->cr_cave = (int8_t)c; crab_set_walk(t, tank_decor_x(t, c), CR_SHELTER); return; }
            t->cr_outing = CR_OUTING_S * 0.5f;                                                                /* nowhere to go: look again later */
        }
        if (t->cr_since_climb >= CR_CLIMB_EVERY_S && tank_decor_in(t, 9) && tank_randf(t, 0, 1) < 0.35f) {   /* the rare climb: to the near foot first */
            float vx = tank_decor_x(t, 9);
            t->cr_side = t->cr_x < vx ? -1 : 1;
            crab_set_walk(t, vx + t->cr_side * tank_volcano_half(VOLCANO_BASE_CELL, t->cr_side) * 2.0f, CR_WALK);
            t->cr_cave = 9;                                                                                   /* (the climb's flag: the walk ends at the foot) */
            return;
        }
        if (t->cr_mode == CR_DIG && tank_randf(t, 0, 1) < 0.5f) { crab_mode(t, CR_REST); t->cr_timer = tank_randf(t, CR_REST_S_LO, CR_REST_S_HI); return; }
        crab_set_walk(t, t->cr_x + tank_randf(t, CR_WALK_LO, CR_WALK_HI) * (tank_randf(t, 0, 1) < 0.5f ? -1 : 1), CR_WALK);
        return;
    default: {                                                                                                /* CR_WALK */
        float sx;
        if (t->cr_cave < 0 && crab_snail_x(t, &sx) && (sx - t->cr_x) * t->cr_facing > 0 && fabsf(sx - t->cr_x) < CR_SNAIL_GAP) { crab_stop(t); return; }   /* the snail ahead: it stops short */
        if (crab_advance(t, dt)) {
            if (t->cr_cave == 9 && tank_decor_in(t, 9) && t->cr_since_climb >= CR_CLIMB_EVERY_S) {            /* at the foot: up it goes */
                t->cr_cave = -1; crab_mode(t, CR_CLIMB); t->cr_climb = 0; t->cr_nibble = 0; t->cr_nibble_i = 0; t->cr_facing = -t->cr_side; return;
            }
            t->cr_cave = -1; crab_stop(t);
        }
        return; }
    }
}
/* the rare climb, now (the director, the sim; Strato 2026-10-24: "let me see how it looks") */
bool tank_crab_climb(tank_t *t) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0 || t->cr_mode == CR_CLIMB || !tank_decor_in(t, 9)) return false;
    float vx = tank_decor_x(t, 9);
    t->cr_since_climb = CR_CLIMB_EVERY_S; t->cr_find = 0; t->cr_poke = 0; t->cr_scurry = false; t->cr_hide = 0; t->cr_nibble = 0; t->cr_nibble_i = 0;
    t->cr_side = t->cr_x < vx ? -1 : 1;
    crab_set_walk(t, vx + t->cr_side * tank_volcano_half(VOLCANO_BASE_CELL, t->cr_side) * 2.0f, CR_WALK);
    t->cr_cave = 9;
    return true;
}
/* send it to a shelter now (the director, the sim): 2 the castle's arch, 9 the volcano's cave, -1 the frontmost
   of those in the water (crab_cave); it walks there and roosts - a named one that is not the frontmost only starts
   the walk: on the way it heads for the frontmost. false = no crab, it is climbing, or no such shelter */
bool tank_crab_roost(tank_t *t, int cave) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0 || t->cr_mode == CR_CLIMB) return false;
    if (cave < 0) cave = crab_cave(t);
    if (cave < 0 || !tank_decor_in(t, cave)) return false;
    t->cr_cave = (int8_t)cave; t->cr_food = -1; t->cr_poke = 0;
    crab_set_walk(t, tank_decor_x(t, cave), CR_SHELTER);
    return true;
}
bool tank_crab_pellet_race(tank_t *t, float gap, int n) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0 || t->cr_mode == CR_CLIMB) return false;
    const float dir = t->cr_x < (TANK_FX0 + TANK_FX1) * 0.5f ? 1.0f : -1.0f, px = clampf(t->cr_x + dir * gap, TANK_FX0 + 30, TANK_FX1 - 30);
    static const float DX[4] = { 0, 18, -15, 9 };                  /* a cluster, as a few feeds on one spot leave them */
    int k = -1;
    for (int j = 0, i = 0; j < (n < 1 ? 1 : n > 4 ? 4 : n); j++) {
        while (i < MAX_FOOD && t->food[i].alive) i++;
        if (i >= MAX_FOOD) break;
        food_t *p = &t->food[i];
        p->alive = true; p->from_player = false; p->x = clampf(px + DX[j] * dir, TANK_FX0 + 20, TANK_FX1 - 20); p->y = FOOD_FLOOR_Y; p->age = 0; p->floor_s = 1; p->nibbled = 0;
        if (k < 0) k = i;
    }
    if (k < 0) return false;
    if (t->sd_unlocks & SD_ITEM_SHRIMP)
        for (int i = 0; i < t->shrimp_n; i++) {                       /* the school at it, pecking */
            shrimp_t *q = &t->shrimp[i];
            q->x = px + (i - t->shrimp_n * 0.5f) * 5; q->y = FOOD_FLOOR_Y - 4; q->vx = q->vy = 0; q->dart = 0; q->facing = (int8_t)(q->x < px ? 1 : -1);
        }
    t->cr_y = 0; t->cr_tilt = 0; t->cr_cave = -1; t->cr_scurry = false; t->cr_hide = 0; t->cr_find = 0; t->cr_poke = 0; t->cr_shoo = 0;
    t->cr_claim = -1; t->cr_full = 0; t->cr_food = (int8_t)k; t->cr_facing = (int8_t)dir;
    t->cr_to = px - CR_REACH * dir; crab_mode(t, CR_FEED);
    return true;
}
void tank_crab_place(tank_t *t) {
    float x0, x1; tank_veg_bed(t, 0, &x0, &x1, NULL, NULL);       /* out on the open sand right of the reef bed (past the urchin's spot) */
    t->cr_x = clampf(x1 + 70, crab_lo(), crab_hi());
    t->cr_y = 0; t->cr_tilt = 0; t->cr_facing = 1; t->cr_mode = CR_REST; t->cr_mode_s = 0; t->cr_timer = 5; t->cr_to = -1; t->cr_gait = 0;
    t->cr_cave = -1; t->cr_food = -1; t->cr_side = 1; t->cr_climb = 0; t->cr_outing = 0; t->cr_poke = 0;
    t->cr_fidget_kind = CR_FID_NONE; t->cr_fidget = 0; t->cr_next_fidget = 2; t->cr_shuffle_to = -1;
    t->cr_taps = 0; t->cr_tap_t = -100; t->cr_scurry = false; t->cr_hide = 0; t->cr_back = 0;
    t->cr_startle_cool = 0; t->cr_find = 0; t->cr_find_cool = tank_randf(t, CR_FIND_FIRST_LO_S, CR_FIND_FIRST_HI_S); t->cr_was_night = t->night; t->cr_stood = false;
    t->cr_lane_front = true; t->cr_next_dig = false;
    t->cr_claim = -1; t->cr_shoo = 0; t->cr_meals = 0; t->cr_full = 0;
}
