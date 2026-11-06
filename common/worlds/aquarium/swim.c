/* swim.c - the aquarium's swim, a movement module (locomotion.h loco_swim): a fish's goal -> target, its steering, its depth,
 * the doors, its meals (update_fish). Moved out of tank.c (issue #4 phase 3) with its bodies unchanged; see tank.h for the rules */
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
#define HOLD_GATHERED_PX  100.0f  /* a fish this close to a drawing finger has come to it (social warmth, 2026-10-29;
                                     0.3.9.3: the ring's outer reach, 1.2 x (R + wobble) to the side; was 45 under the fingertip) */
#define HOLD_APPROACH_FROM 140.0f /* a hold-approach must START at least this far out - past the ring:
                                 * a fish already at the finger earns nothing (0.3.9.3: was 60 / 30, under the fingertip) */
#define HOLD_APPROACH_AT   HOLD_GATHERED_PX   /* ... and come in this close: onto the ring */
#define VEG_SLOW            0.60f     /* cruise speed factor inside a canopy */
/* a random point of open water: y in [y0, y1], x at least mx off the side glass at that height */
static void rand_water(tank_t *t, float mx, float y0, float y1, float *x, float *y) {
#if defined(TANK_ROUND) || defined(TANK_CORNER_R)
    *y = tank_randf(t, y0, y1);
    float lo = tank_glass_x0(*y) + mx, hi = tank_glass_x1(*y) - mx;
    *x = lo < hi ? tank_randf(t, lo, hi) : TANK_W * 0.5f;
#else
    *x = tank_randf(t, mx, TANK_W - mx); *y = tank_randf(t, y0, y1);
#endif
}

/* the breather (2026-10-21, Strato's 1.8 after a reset: "sluggish", no "first
 * rest in the seagrass"; a Discord keeper's tank the same for days): the v4
 * brain rests at NIGHT - its teacher rested a lit fish only at energy <= 1,
 * one roll in twenty - while the energy economy below (09-04) was tuned for a
 * brain that rested by day. So a lit tank ran its fish down to 0 in ten
 * minutes and kept them there, crawling at 45%, and a keeper who never turns
 * the light off never saw the seagrass badge. Physiology, like the famine's
 * begging: a spent fish ducks into the nearest canopy and hovers until it has
 * its wind back (the rest rate, ~10 s). The advisor still owns the goal. */
#define BREATHER_AT   1.0f    /* energy under this by day: a breather */
#define BREATHER_TO   4.0f    /* ... until this */
#define BREATHER_NEAR 22.0f   /* settled in the fronds within this: the wind comes back */
#define HUNGER_BOLD_PER_S  0.008f   /* bold fish burn hotter: ~9 min */
#define HUNGER_DART_PER_S  0.012f   /* play costs extra */

#define CURIOSITY_SPEND_PER_S 0.25f /* spent at the bubbles / the reef: 9 -> 3 in ~24 s */
#define CURIOSITY_SPEND_RADIUS 60.0f

/* the courtship circle, shared by the tell's episodes and the spawning: the
 * pair weaves a low loop through the nursery on opposite sides - performed
 * rather than printed (reflex presentation; the advisor still owns both
 * goals). Down in the fronds, not mid-water: Strato (2026-09-04) - by the
 * reef it read as two friends at the bubbles. */
/* the breather's spot: low in the nearest seagrass bed with real cover, a
 * body apart per slot; with every bed shaved, the rest spot in the corner */
static void breather_spot(const tank_t *t, const fish_t *f, int idx, float *x, float *y) {
    int best = -1; float bd = 1e9f, bx = 0;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (tank_veg_kind(t, b) != VEG_KIND_GRASS || t->veg_growth[b] < VEG_BARE) continue;
        float x0, x1; tank_veg_bed(t, b, &x0, &x1, NULL, NULL);
        float cx = (x0 + x1) * 0.5f, d = fabsf(cx - f->x);
        if (d < bd) { bd = d; best = b; bx = cx; }
    }
    if (best < 0) { *x = t->reef_x + 13 + f->rest_dx; *y = TANK_BOT - 50 + f->rest_dy; return; }
    *x = bx + (idx % 2 ? 14 : -14) + (idx / 2) * 10;
    *y = TANK_BOT - 16 - 14 - (idx % 3) * 6;
    tank_glass_clamp(x, y, 20);
}
static void court_steer(const tank_t *t, const fish_t *f, int idx, float *desired, float *speed) {
    float ph = anim(t) * 1.7f + (idx == t->court_b ? 3.14159f : 0);
    float cx, cy, rx; court_site(t, &cx, &cy, &rx);
    float wx = cx + cosf(ph) * rx, wy = cy + sinf(ph) * 6;
    float to = atan2f(wy - f->y, wx - f->x);
    *desired = norm_ang(*desired + norm_ang(to - *desired) * 0.85f);
    *speed = tank_dist(f->x, f->y, cx, cy) > 90 ? 46 : 30;
}

/* ---- goal → target point + cruise speed (prototype targetForGoal, x0.55) ---- */
typedef struct { float x, y, speed; bool valid; } target_t;

/* `goal` is normally f->goal.id; the hesitation glance asks for the runner-up's
 * target, in which case nothing is mutated (no dart burst is started). */
/* ---- the tank link's doors (0.4, docs/tank-link-0.4.md) ----
 * A visit is EXPLORE resolving its destination, never a goal of its own: the
 * model still decides. A home fish exploring counts a linked neighbor as a
 * zone, stale until it has been there (door_seen), so an explorer heads for an
 * open door now and then; a visitor's explorations lean more and more toward
 * its home door the longer it is away (DOOR_HOME_PULL_*). The tank link
 * (tank_link.c) does the rest at the glass. */
#define DOOR_STALE_S        45.0f   /* a neighbor visited this recently is not stale */
#define DOOR_HOME_PULL_S    45.0f   /* a visitor's pull home starts after this long ... */
#define DOOR_HOME_FULL_S    300.0f  /* ... and is certain this long after that (5 min) */
#define DOOR_REACH_PX       30.0f   /* at the glass: asks (home) / passes (a visitor going home) */
#define DOOR_BOLT_SPEED     85.0f   /* px/s for its door, spooked: a flight (a startled fish bolts 55-90) */
#define DOOR_SWIM_SPEED     38.0f   /* px/s through the doorway: ~44 px from asking to gone in ~1.2 s */
static float door_glass_x(int e) { return e == 0 ? tank_glass_x0(DOOR_Y) : tank_glass_x1(DOOR_Y); }
static bool door_choose(tank_t *t, fish_t *f, int idx) {
    float p = 0; int e = -1;
    if (f->door != DOOR_NONE) return false;                     /* in a doorway already: that is decided (2026-10-05: explore picked again under a PASSING fish and turned it round, 9 of 25 accepted crossings on the boards) */
    if (tank_is_visitor(idx)) {                                 /* home: the door it came in by */
        int k = idx - N_FISH_MAX, de = f->door_edge;
        if (!t->door_open[de]) return false;
        if (t->courting && (idx == t->court_a || idx == t->court_b) && !f->homeward) return false;   /* courting here: home can wait */
        float away = (float)(t->clock - t->vis_since[k]);
        p = clampf((away - DOOR_HOME_PULL_S) / DOOR_HOME_FULL_S, 0, 1); e = de;
    } else {
        for (int k = 0; k < 2; k++) {
            if (!t->door_open[k] || t->clock < f->door_refused || t->clock - f->door_seen < DOOR_STALE_S) continue;
            float pk = 0.45f + f->curiosity * 0.03f;            /* the stalest place there is: a good chance, more for the curious */
            if (e < 0 || (xr(t) & 1)) { e = k; p = pk; }
        }
    }
    if (e < 0 || tank_randf(t, 0, 1) >= p) return false;
    f->door = DOOR_HEADING; f->door_edge = (int8_t)e;
    return true;
}
static target_t door_target(const tank_t *t, const fish_t *f) {
    float gx = door_glass_x(f->door_edge), in = f->door_edge == 0 ? 22 : -22;
    target_t tg = { gx + in, DOOR_Y + sinf(f->wander) * DOOR_BAND_H * 0.2f, 30, true };
    (void)t; return tg;
}

static target_t target_for_goal(tank_t *t, int idx, goal_id_t goal, bool glance) {
    fish_t *f = &t->fish[idx];
    const float tm = anim(t);
    target_t tg = {
        f->x + cosf(f->heading + sinf(f->wander) * 0.8f) * 50,
        f->y + sinf(f->heading + cosf(f->wander * 0.7f) * 0.45f) * 39,
        lerpf(12, 23, f->bold) * (1 - f->lazy * 0.35f), true,
    };
    if (!glance && (f->door == DOOR_HEADING || f->door == DOOR_ASKED)) return door_target(t, f);   /* on its way to a door */
    switch (goal) {
    case GOAL_SEEK_FOOD: {
        float d; int i = tank_nearest_food(t, f, &d);
        if (i >= 0) {
            /* hunger governs pursuit aggression regardless of which brain set
             * the goal: a peckish fish saunters (~0.75x), a starving one
             * charges (~1.3x) and barely brakes on approach */
            float h = clampf(f->hunger / 10.0f, 0, 1);
            float slow = clampf(d / 60, 0.4f + h * 0.35f, 1);
            tg.x = t->food[i].x; tg.y = t->food[i].y;
            tg.speed = lerpf(29, 62, f->bold) * slow * (0.75f + h * 0.55f);
            return tg;   /* skip the margin clamp: pellets rest at the floor */
        }
        tg.valid = false;
        break;
    }
    case GOAL_FLEE_SHADOW: {
        /* the roaming shadow was removed 2026-09-13; the token stays in the
         * frozen schema (the state line always reads `shadow none`, which the
         * model was trained on). If it still says flee, the fish bolts away
         * from the surface centre - the old no-shadow fallback. */
        float sx = TANK_W * 0.5f, sy = -60;
        float away = atan2f(f->y - sy, f->x - sx);
        tg.x = f->x + cosf(away) * 105; tg.y = f->y + sinf(away) * 83;
        tg.speed = lerpf(51, 83, f->bold);
        break;
    }
    case GOAL_VISIT_BUBBLES:
        /* each visitor keeps its own lane: a per-fish phase and radius, so
         * four fish orbit the column instead of stacking on one point */
        tg.x = t->bubble_x + sinf(tm * 1.2f + f->wander + idx * 1.9f) * (26 + idx * 6);
        tg.y = t->bubble_y - 74 + cosf(tm * 0.8f + f->wander + idx * 1.3f) * (34 + idx * 6);
        tg.speed = 23;
        break;
    case GOAL_FOLLOW_FRIEND: {
        float d; int i = tank_nearest_friend(t, idx, &d);
        if (i >= 0) {
            const fish_t *fr = &t->fish[i];
            float ang = atan2f(f->y - fr->y, f->x - fr->x) + sinf(tm + f->wander) * 0.35f;
            tg.x = fr->x + cosf(ang) * 54; tg.y = fr->y + sinf(ang) * 34;   /* a body length off, not on top */
            tg.speed = clampf(d - 40, 10, 42);
        } else tg.valid = false;
        break;
    }
    case GOAL_REST:
        tg.x = t->reef_x + 13 + f->rest_dx;
        tg.y = TANK_BOT - 50 + f->rest_dy;
        tg.speed = 6 + f->bold * 4;
        break;
    case GOAL_DART_PLAY:
        if (glance) { tg.valid = false; break; }
        if (f->dart_timer <= 0) {
            rand_water(t, 38, 40, TANK_BOT - 64, &f->dart_x, &f->dart_y);
            f->dart_timer = tank_randf(t, 0.8f, 1.5f);
        }
        tg.x = f->dart_x; tg.y = f->dart_y;
        tg.speed = lerpf(65, 95, f->bold);
        break;
    case GOAL_INSPECT_REEF: {
        float rx, ry; tank_reef_spot(t, &rx, &ry);   /* the reef cluster, once there is one */
        tg.x = rx + sinf(tm * 0.65f + f->wander + idx * 1.7f) * (39 + idx * 4);
        tg.y = ry - REEF_ORBIT_UP + cosf(tm * 0.8f + f->wander + idx * 1.1f) * (15 + idx * 4);
        tg.speed = 15 + f->curiosity * 1.7f;
        break;
    }
    case GOAL_EXPLORE: {
        /* a destination, not a drift (2026-09-14). Before this, explore was
         * the wander target above - 50 px ahead of the nose with a wobble -
         * a random walk that never left the neighbourhood, so an explore
         * decision looked like idling (Strato: "they don't explore the tank
         * very much"). Now the fish picks the zone it has seen least recently
         * (one of the two stalest, so two explorers don't take the same line),
         * cruises to a point inside it, and on arrival picks the next. The
         * model still owns the goal; this is the reflex layer resolving the
         * concrete target, as it does for every other goal. */
        if (glance) { if (!f->explore_set) tg.valid = false; else { tg.x = f->explore_x; tg.y = f->explore_y; } break; }
        if (!f->explore_set || tank_dist(f->x, f->y, f->explore_x, f->explore_y) < 26) {
            if (door_choose(t, f, idx)) { f->explore_set = false; return door_target(t, f); }   /* next door, this time */
            int cur = zone_of(f->x, f->y), best = -1, second = -1;
            for (int z = 0; z < 6; z++) {
                if (z == cur) continue;
                if (best < 0 || f->zone_seen[z] < f->zone_seen[best]) { second = best; best = z; }
                else if (second < 0 || f->zone_seen[z] < f->zone_seen[second]) second = z;
            }
            int z = (second >= 0 && (xr(t) % 3) == 0) ? second : best;
            int col = z % 3, row = z / 3;
            f->explore_x = tank_randf(t, col * (TANK_W / 3.0f) + 40, (col + 1) * (TANK_W / 3.0f) - 40);
            f->explore_y = tank_randf(t, row * (TANK_BOT / 2.0f) + 42, (row + 1) * (TANK_BOT / 2.0f) - 40);
#if defined(TANK_ROUND) || defined(TANK_CORNER_R)
            tank_glass_clamp(&f->explore_x, &f->explore_y, 44);   /* a zone's corner is outside the bowl */
#endif
            f->explore_set = true;
        }
        /* the wander wobble bends the line so it reads as a swim, not a bee-line */
        tg.x = f->explore_x + sinf(f->wander) * 18;
        tg.y = f->explore_y + cosf(f->wander * 0.7f) * 12;
        break;
    }
    default: if (glance) tg.valid = false; break;
    }
    float m = 23;
#ifdef TANK_ROUND
    tank_glass_clamp(&tg.x, &tg.y, m + 4);
#else
    tg.x = clampf(tg.x, m, TANK_W - m);
    tg.y = clampf(tg.y, m + 9, TANK_H - m);
    corner_clamp(&tg.x, &tg.y, m + 4);
#endif
    return tg;
}

static float wall_avoidance(const fish_t *f, bool *hit) {
    const float m = 34;
    float vx = 0, vy = 0;
#ifdef TANK_ROUND                                          /* the bowl: away from the glass along its radius, and up off the floor */
    float dx = f->x - TANK_RAD, dy = f->y - TANK_RAD, d = sqrtf(dx * dx + dy * dy), gap = TANK_RAD - d;
    if (gap < m && d > 1) { float p = 1 - (gap < 0 ? 0 : gap) / m; vx -= dx / d * p; vy -= dy / d * p; }
    if (f->y > TANK_BOT - m) vy -= 1 - (TANK_BOT - f->y) / m;
#else
    if (f->x < m)          vx += 1 - f->x / m;
    if (f->x > TANK_W - m) vx -= 1 - (TANK_W - f->x) / m;
    if (f->y < m + 6)      vy += 1 - (f->y - 6) / m;
    if (f->y > TANK_H - m) vy -= 1 - (TANK_H - f->y) / m;
#ifdef TANK_CORNER_R                                       /* the watch: in a corner the arc is nearer than either straight wall */
    { float cx, cy, d = corner_in(f->x, f->y, &cx, &cy), gap = TANK_CORNER_R - d;
      if (d > 1 && gap < m) { float p = 1 - (gap < 0 ? 0 : gap) / m; vx -= (f->x - cx) / d * p; vy -= (f->y - cy) / d * p; } }
#endif
#endif
    *hit = (vx != 0 || vy != 0);
    return *hit ? atan2f(vy, vx) : 0;
}

static void eat_nearby_food(tank_t *t, fish_t *f) {
    for (int i = 0; i < MAX_FOOD; i++) {
        if (!t->food[i].alive) continue;
        if (tank_dist(f->x, f->y, t->food[i].x, t->food[i].y) < 12 * f->size + 4) {
            t->food[i].alive = false;
            f->hunger = clampf(f->hunger - FISH_HUNGER_PER_PELLET, 0, 10);
            f->energy = clampf(f->energy + 1.0f, 0, 10);
            f->curiosity = clampf(f->curiosity + 0.5f, 0, 10);   /* was 0.8: a trickle burst
                                                                     re-synced the school's curiosity */
            f->eaten++;
            tank_emit(TEV_EAT, (int)(f - t->fish));
            if (t->food[i].from_player) {
                if (!tank_is_visitor((int)(f - t->fish))) { f->eaten_player++; f->ms_bits |= MS_FIRST_MEAL_FROM_YOU; f->meal_due = true; }   /* (a visitor's keeper is elsewhere: no badge, no pay) */
                if (t->feed_open) { t->player_feedings++; t->feed_open = false; }   /* the gesture became a meal */
            }
            /* post-meal reflex from the prototype */
            if (f->hunger < 2.2f && f->goal.id == GOAL_SEEK_FOOD)
                f->goal.id = (xr(t) & 1) ? GOAL_VISIT_BUBBLES : GOAL_EXPLORE;
            return;   /* one bite per frame: a fish in a cloud of pellets takes
                         them one at a time, so a tank-mate gets a look in */
        }
    }
}

/* a point on the loose ring round a held finger (HOLD_RING_*): wider than tall (a fish swims mostly level),
 * and by a wall or the sand the ring folds onto the open side - smoothly, so a place never jumps across */
static void hold_ring_point(const tank_t *t, float ph, float r, float *x, float *y) {
    float room[4];                                   /* how far the ring may reach: right, down, left, up */
    for (int s = 0; s < 4; s++) {
        float px = t->hold_x + (s == 0 ? r * 1.2f : s == 2 ? -r * 1.2f : 0), py = t->hold_y + (s == 1 ? r * 0.85f : s == 3 ? -r * 0.85f : 0);
        float cx = px, cy = py; tank_glass_clamp(&cx, &cy, 22);
        room[s] = (s & 1) ? fabsf(cy - t->hold_y) : fabsf(cx - t->hold_x);
    }
    float c = cosf(ph), s = sinf(ph);
    if (room[2] < 45 && room[0] >= 45) c = fabsf(c);
    else if (room[0] < 45 && room[2] >= 45) c = -fabsf(c);
    if (room[3] < 40 && room[1] >= 40) s = fabsf(s);
    else if (room[1] < 40 && room[3] >= 40) s = -fabsf(s);
    *x = t->hold_x + c * (c >= 0 ? room[0] : room[2]);
    *y = t->hold_y + s * (s >= 0 ? room[1] : room[3]);
    tank_glass_clamp(x, y, 22);
}

static void update_fish(tank_t *t, int idx, float dt) {
    fish_t *f = &t->fish[idx];
    /* the lizard's body heat follows where it is (tank.h THE HEAT FIELD), at its pack's pace for its stage */
    body_follow(t, f, tank_env_c(t, f->x, f->y), dt);
    /* drives (prototype rates, speed rescaled by the same 0.55); before a shed it goes off its food (docs/shed.md) */
    float shed_hunger = f->shed == SHED_SOON || f->shed == SHED_SHEDDING ? SHED_HUNGER_X : 1.0f;
    f->hunger    = clampf(f->hunger + dt * shed_hunger * (HUNGER_PER_S + f->bold * HUNGER_BOLD_PER_S +
                          (f->goal.id == GOAL_DART_PLAY ? HUNGER_DART_PER_S : 0)), 0, 10);
    /* curiosity: exploring feeds it (prototype) and - new 2026-09-01 -
     * satisfying it SPENDS it: a fish that has reached the bubbles or the
     * reef uses curiosity up there, the way rest restores energy and a meal
     * drops hunger. Before this a fed fish had no sink at all (+0.8 per
     * pellet, nothing ever took it back), so after days the whole school sat
     * at curiosity 9 and the advisor - reading "fed, calm, curious" exactly
     * as trained - parked all four at the bubble column for good. The model
     * still owns the goal; it just sees an honest drive, and the curiosity
     * band is in the re-ask signature so it gets to react. */
    {
        float dc = f->goal.id == GOAL_EXPLORE ? FISH_CURIOSITY_PER_S : -0.025f;
        if (f->goal.id == GOAL_VISIT_BUBBLES || f->goal.id == GOAL_INSPECT_REEF) {
            bool bub = f->goal.id == GOAL_VISIT_BUBBLES;
            float sx = t->bubble_x, sy = t->bubble_y - 74;
            if (!bub) { tank_reef_spot(t, &sx, &sy); sy -= REEF_ORBIT_UP; }
            bool at = tank_dist(f->x, f->y, sx, sy) < CURIOSITY_SPEND_RADIUS;
            if (at) dc = -CURIOSITY_SPEND_PER_S;
            if (bub && at && !f->at_bubbles) tank_emit(TEV_BUBBLES, idx);   /* arrival at the column: the play cue */
            f->at_bubbles = bub && at;
        } else f->at_bubbles = false;
        f->curiosity = clampf(f->curiosity + dt * dc, 0, 10);
    }
    f->stress    = clampf(f->stress - dt * (f->goal.id == GOAL_REST ? 0.48f : 0.18f), 0, 10);
    /* energy: the prototype's per-second drain (full to empty in ~3 min of
     * cruising) kept the school at energy ~3 - permanently tired, so the
     * advisor never had a fish fit enough to dart and read every content
     * fish as "relaxed": bubbles, reef, follow (2026-09-01 census). Now a
     * fish cruises ~10 min on a full tank and a rest refills it in ~35 s. */
    bool settled = false;                        /* a breather, settled in the fronds */
    if (f->breather > 0) { float bx, by; breather_spot(t, f, idx, &bx, &by); settled = tank_dist(f->x, f->y, bx, by) < BREATHER_NEAR; }
    f->energy    = clampf(f->energy + dt * ((f->goal.id == GOAL_REST || settled) ? FISH_ENERGY_REST_PER_S
                          : -FISH_ENERGY_PER_S - f->speed / 4000.0f), 0, 10);
    /* the breather (see BREATHER_AT): in when spent by day, out with the wind
       back, never over a flight, the famine's begging or the night's own rest */
    bool on_door_duty = f->door != DOOR_NONE || f->homeward || f->bolt_home || f->deported;   /* the tank link (0.4): a doorway is decided; a breather waits */
    if (f->breather > 0) {
        if (f->energy >= BREATHER_TO || t->night || f->goal.id == GOAL_FLEE_SHADOW || (t->ravenous && f->hunger > 6.5f) || on_door_duty) f->breather = 0;
        else f->breather += dt;
    } else if (!t->night && f->energy < BREATHER_AT && f->goal.id != GOAL_FLEE_SHADOW && !(t->ravenous && f->hunger > 6.5f) && !on_door_duty)
        { f->breather = dt; f->gdepth_rest = depth_pick(t, f, GOAL_REST) == FISH_DEPTH_THROUGH ? FISH_DEPTH_THROUGH : FISH_DEPTH_BEHIND; }
    /* SWIM DEPTH: its depth taken up in the clear of every bed (a breather hides at its own); the first one at once
       (a start: the boot show brings the fish in) - and either one only where no piece it is on would pop (0.3.9.7) */
    if (f->gdepth_want == 0xff) { f->gdepth_want = depth_pick(t, f, f->goal.id); if (fish_pieces_keep(t, f, f->gdepth_want, f->pdepth)) f->gdepth = f->gdepth_want; }
    { uint8_t want = f->hold_call ? FISH_DEPTH_FRONT : f->breather > 0 ? f->gdepth_rest : f->gdepth_want;   /* (a finger's call: in sight - through the
                                                                         seagrass if it must, never through a piece) */
      if (f->gdepth != want && (f->hold_call || grass_clear(t, f)) && fish_pieces_keep(t, f, want, f->pdepth)) f->gdepth = want; }
    if (f->pdepth_want == 0xff) { f->pdepth_want = pdepth_pick(t, f, f->goal.id); if (fish_pieces_keep(t, f, f->gdepth, f->pdepth_want)) f->pdepth = f->pdepth_want; }
    { uint8_t want = f->hold_call || f->breather > 0 || f->goal.id == GOAL_REST ? FISH_DEPTH_FRONT : f->pdepth_want;
      if (f->pdepth != want && fish_pieces_keep(t, f, f->gdepth, want)) f->pdepth = want; }
    /* a canopy is a place to hide: a fish inside one calms faster (below) */
    bool hidden = false;
    for (int b = 0; b < tank_veg_beds(t) && !hidden; b++)
        hidden = t->veg_growth[b] >= VEG_BARE && veg_inside(t, b, f->x, f->y);
    /* vegetation comfort (2026-09-04 rework: fish LIKE cover). Three regimes,
     * each seeking its own equilibrium against the natural decay above:
     *  - SMOTHERED: the second-tallest bed past VEG_SMOTHER (85% of the way to
     *    its own ceiling, tank_veg_cap - since 2026-09-23 a bed stops short of
     *    the surface) - at least two beds crowding their ceilings - is the tank
     *    being overrun: real stress, ramping from nothing at 85% to the full
     *    press at 100% (settles ~6-7 with every bed at its ceiling). One bed
     *    at the ceiling is just a good hiding place, never a stressor.
     *  - BARE: no bed past VEG_BARE - nowhere to hide - is a mild unease
     *    (settles ~1.3) that lifts the moment one tuft regrows.
     *  - otherwise COVER CALMS: stress decays faster the more canopy there is
     *    (up to +0.18/s, doubling the base rate, at a tank full of grass), and
     *    faster again for a fish tucked inside a canopy. */
    {
        float g1 = 0, g2 = 0, gmean = 0;               /* tallest, second tallest */
        int nb = tank_veg_beds(t);
        for (int b = 0; b < nb; b++) {
            float g = t->veg_growth[b];
            gmean += g / nb;                          /* cover calms by real height */
            g /= veg_cap_mean(t, b);                  /* the smother band by fullness */
            if (g > g1) { g2 = g1; g1 = g; } else if (g > g2) g2 = g;
        }
        float over = (g2 - VEG_SMOTHER) / (1.0f - VEG_SMOTHER);
        if (over > 0)
            f->stress = clampf(f->stress + fminf(1, over) * 0.15f * (8.0f - f->stress) * dt, 0, 10);
        else if (g1 < VEG_BARE)
            f->stress = clampf(f->stress + 0.15f * (2.5f - f->stress) * dt, 0, 10);
        else
            f->stress = clampf(f->stress - dt * (0.18f * gmean + (hidden ? 0.18f : 0)), 0, 10);
    }
    /* overheating pulls stress up (tank.h HEAT_HOT), and a stuck shed holds a floor under it (docs/shed.md) */
    if (f->heat >= HEAT_HOT) {
        float to = 4 + 2 * (f->heat - HEAT_HOT);
        if (f->stress < to) f->stress = clampf(f->stress + HEAT_HOT_PULL * (to - f->stress) * dt, 0, 10);
    }
    f->stress = fmaxf(f->stress, stuck_floor(f));

    f->wander += dt * (0.65f + f->curiosity * 0.04f) + sinf(anim(t) + f->x * 0.01f) * dt * 0.12f;
    if (f->wander >= 62.831853f) f->wander = fmodf(f->wander, 62.831853f);   /* (20 pi: its every multiple is tenths - the lap is seamless;
                                                                                summed for days, a float stopped adding at ~8-12 days and the fish circled) */
    if (f->dart_timer > 0) f->dart_timer -= dt;
    f->goal_age += dt; f->ask_age += dt;
    /* boredom: the same pastime goes stale (a minute to the top); a meal or a
     * night's rest is never boring. A zone the fish has not seen for a while
     * is a small relief - so a real explore across the tank roughly pays for
     * itself, while an orbit at the bubble column or a tail-chase behind a
     * friend only accrues. (A zone boundary under the orbit gives nothing:
     * the zone has to be BORED_ZONE_STALE_S stale.) The new-goal relief is in
     * tank_tick where goals change. The band is in state_signature, so a fish
     * crossing into "bored" is re-asked - and the v4 model reads the value. */
    {
        goal_id_t g = f->goal.id;
        bool leisure = g == GOAL_VISIT_BUBBLES || g == GOAL_FOLLOW_FRIEND || g == GOAL_INSPECT_REEF ||
                       g == GOAL_DART_PLAY || g == GOAL_EXPLORE || (g == GOAL_REST && !t->night);
        f->bored = clampf(f->bored + dt * (leisure ? BORED_PER_S : -BORED_RELIEF_PER_S), 0, 10);
        int z = zone_of(f->x, f->y);
        if (z != f->zone_last) {
            if (f->zone_last >= 0 && t->clock - f->zone_seen[z] > BORED_ZONE_STALE_S)
                f->bored = clampf(f->bored - BORED_NEW_ZONE, 0, 10);
            f->zone_last = (int8_t)z;
        }
        f->zone_seen[z] = t->clock;
    }

    /* the doors: a visitor told home heads for its door; at the glass a home
       fish asks (tank_link offers it next door), a visitor going home passes */
    /* a parent following its fry home (phase 5): behind it while it is still
       here, then the door - never ahead of it */
    int follow = -1;
    if (f->follow_uid) {
        for (int k = 0; k < N_VISIT_MAX && follow < 0; k++)
            if (t->vis_used[k] && t->fish[N_FISH_MAX + k].uid == f->follow_uid) follow = N_FISH_MAX + k;
        if (follow < 0) f->follow_uid = 0;               /* through: now its own turn */
        else if (f->door == DOOR_HEADING) f->door = DOOR_NONE;
    }
    if (f->homeward && follow < 0 && f->door == DOOR_NONE && t->door_open[f->door_edge]) f->door = DOOR_HEADING;
    if ((f->door == DOOR_HEADING || f->door == DOOR_ASKED) && !t->door_open[f->door_edge]) f->door = DOOR_NONE;   /* the door closed */
    if (f->door == DOOR_HEADING && fabsf(f->x - door_glass_x(f->door_edge)) < DOOR_REACH_PX && fabsf(f->y - DOOR_Y) < DOOR_BAND_H / 2)
        f->door = tank_is_visitor(idx) ? DOOR_PASSING : DOOR_ASKED;
    if (f->door == DOOR_ENTERING && fabsf(f->x - door_glass_x(f->door_edge)) > 46) f->door = DOOR_NONE;   /* in: a fish of this tank's water again */
    if (f->door == DOOR_PASSING && f->speed < 20) f->speed = 20;   /* accepted: off it goes (it may be hovering, facing in) */
    bool through = f->door == DOOR_PASSING || f->door == DOOR_ENTERING;

    target_t tg = target_for_goal(t, idx, f->goal.id, false);
    /* fish prefer shallow climb/dive angles while cruising; full vertical
     * agility stays available for urgent goals */
    bool agile = f->goal.id == GOAL_FLEE_SHADOW || f->goal.id == GOAL_DART_PLAY;
    float desired = atan2f((tg.y - f->y) * (agile ? 1.0f : 0.72f), tg.x - f->x);
    if (through) {                                          /* straight through the doorway: out, or in */
        bool out = f->door == DOOR_PASSING;
        bool left = f->door_edge == 0;
        desired = (out == left) ? 3.14159265f : 0.0f;
        desired += (DOOR_Y - f->y) * 0.004f;                /* (a little toward the band's middle) */
        /* a fish swims the way it FACES, and its facing changes only by a
           committed turn: one hovering at the door facing in would swim on
           in, whatever the heading says (2026-10-05: accepted fish on the
           boards turned back). Through a doorway the turn is committed at
           once - the U-turn shows, then the swim follows. */
        int8_t want = (out == left) ? -1 : 1;
        if (f->facing != want) { f->facing = want; f->behind = -1.3f; }   /* (update_fish's TURN_SETTLE) */
    }
    /* final food approach: let the fish dip to the floor for the pellet
     * instead of hovering above it on wall-avoidance (the "staring" bug) */
    bool final_approach = f->goal.id == GOAL_SEEK_FOOD &&
                          tank_dist(f->x, f->y, tg.x, tg.y) < 40;
    bool hit; float push = wall_avoidance(f, &hit);
    if (through) hit = false;                               /* the open door is no wall */
    /* nor on the way to it: in the doorway band the glass's push (34 px) held a fish
       off past the door's reach (30 px) - slow ones crept in, a swimming one never did
       (2026-10-05, a spooked visitor circled its door on every board) */
    if (f->door == DOOR_HEADING && fabsf(f->x - door_glass_x(f->door_edge)) < 60 && fabsf(f->y - DOOR_Y) < DOOR_BAND_H / 2) hit = false;
    if (hit && !final_approach)
        desired = norm_ang(desired + norm_ang(push - desired) * 0.62f);

    /* hesitation: a low-confidence decision is shown, not hidden - the fish
     * hovers and glances between its new target and the runner-up's for a
     * moment before committing. Never for flight or high urgency. */
    float hes_speed = -1;
    if (f->hesitate > 0) {
        f->hesitate -= dt;
        target_t alt = f->goal.runner_up < GOAL_COUNT
                     ? target_for_goal(t, idx, f->goal.runner_up, true) : (target_t){0, 0, 0, false};
        float phase = sinf(anim(t) * 5.0f + f->wander);
        if (alt.valid && phase < 0) desired = atan2f((alt.y - f->y) * 0.72f, alt.x - f->x);
        hes_speed = 5;
    }

    /* touch: spooked fish bolt from the tap site; trusting fish drift to a
     * resting finger (reflex-layer, independent of the advisor's goal) */
    float touch_speed = -1;
    bool ringing = false;                        /* on the ring round a held finger (HOLD_RING_*): turns at its ends come quickly */
    bool paddling = false; float pad_x = 0, pad_y = 0;   /* ... and near its place there: nose on the finger, fins to the place */
    f->hold_call = false;                                   /* (set again below while it answers a held finger) */
    if (follow >= 0) {                                      /* a body behind its fry, as fast as it goes */
        const fish_t *g = &t->fish[follow];
        float bx = g->x - cosf(g->heading) * 26, by = g->y - sinf(g->heading) * 12;
        desired = atan2f(by - f->y, bx - f->x);
        float d = tank_dist(f->x, f->y, bx, by);
        touch_speed = clampf(g->speed * (d > 30 ? 1.15f : 0.95f), 20, 95);
    } else if (f->deported && f->door == DOOR_HEADING) {
        touch_speed = fabsf(f->x - door_glass_x(f->door_edge)) > 70 ? 55 : DOOR_SWIM_SPEED;   /* sent home: briskly, not in fright */
    } else if (f->bolt_home && f->door == DOOR_HEADING) {
        /* spooked: for its door, not away from the finger (the door's line is the target); flat out
           across the tank, braking to the doorway's swim for the last stretch (at 85 px/s it circled
           the gap on the watch's narrow glass, 2026-10-05) */
        touch_speed = fabsf(f->x - door_glass_x(f->door_edge)) > 70 ? DOOR_BOLT_SPEED : DOOR_SWIM_SPEED;
    } else if (t->startled && f->goal.id != GOAL_FLEE_SHADOW) {
        float d = tank_dist(f->x, f->y, t->startle_x, t->startle_y);
        if (d < STARTLE_RADIUS * 1.6f) {
            float away = atan2f(f->y - t->startle_y, f->x - t->startle_x);
            desired = norm_ang(desired + norm_ang(away - desired) * 0.85f);
            touch_speed = lerpf(55, 90, f->bold);
        }
    } else if (idx == t->stage_fish) {
        /* on stage (setup): a lazy figure-of-eight round the page's clear
         * spot, turning at each end so both flanks show; a fish far from it
         * cruises over first. Above every other presentation: the keeper is
         * looking at THIS fish. */
        float ph = anim(t) * 1.1f + f->wander;
        float wx = t->stage_x + cosf(ph) * 24, wy = t->stage_y + sinf(ph * 2) * 6;
        float to = atan2f(wy - f->y, wx - f->x);
        desired = norm_ang(desired + norm_ang(to - desired) * 0.85f);
        touch_speed = tank_dist(f->x, f->y, t->stage_x, t->stage_y) > 80 ? 44 : 20;
    } else if (t->spawning && (idx == t->court_a || idx == t->court_b) &&
               f->goal.id != GOAL_FLEE_SHADOW && !(t->ravenous && f->hunger > 6.5f)) {
        /* the spawning (2026-09-24): the courtship that ends in a fry. It
         * outranks a resting finger and the greeting - this is the moment the
         * keeper is here for - and hunger short of the famine doesn't veto
         * it; a starving parent still begs first (feed it, it comes back). */
        court_steer(t, f, idx, &desired, &touch_speed);
    } else if (t->hold_active && t->hold_time >= HOLD_ATTRACT_S &&
               f->goal.id != GOAL_FLEE_SHADOW && f->trust >= 4.0f &&
               f->hunger < HOLD_HUNGER_VETO) {
        /* only a settled hold (~3 s of contact) draws fish in - quick taps and
         * card-taps never twitch the school - and a strongly hungry fish has
         * better things to do than visit a finger */
        float d = tank_dist(f->x, f->y, t->hold_x, t->hold_y);
        if (d < HOLD_RADIUS) {
            f->hold_call = !g_dbg_hold_emerge_off;
            float w = (f->trust - 4.0f) / 6.0f;          /* 0..1 with trust */
            /* its place on the loose ring (HOLD_RING_*): a little behind the fish before it, its own drift
             * along and in and out, and now and then the far side (the sine gate: ~1 minute in 8) */
            /* in pairs (four fish or more: two pairs either side of the finger, so it is surrounded and every
             * fish has its company); each its own slow sway along the ring and in and out */
            int rank = 0, m = 0;
            for (int i = 0; i < t->n_fish; i++) if (t->fish[i].trust >= 4.0f && t->fish[i].hunger < HOLD_HUNGER_VETO) { m++; if (i < idx) rank++; }
            int groups = m >= 4 ? m / 2 : 1, g = rank / 2 < groups - 1 ? rank / 2 : groups - 1, k = rank - g * 2;
            if (groups == 1) k = rank;
            float ph = t->hold_ph + g * 6.2832f / groups - t->hold_dir * k * HOLD_RING_GAP + 0.15f * sinf(anim(t) * 0.17f + f->wander * 4.1f);
            bool crossing = sinf(anim(t) * 0.11f + f->wander * 6.3f + idx * 1.7f) > 0.93f;
            if (crossing) ph += 3.14159f;
            float r = HOLD_RING_R + HOLD_RING_WOBBLE * sinf(anim(t) * 0.23f + f->wander * 5.3f + idx);
            float wx, wy; hold_ring_point(t, ph, r, &wx, &wy);   /* its place */
            /* the way there: round the ring, never under the fingertip - a place more than ~50 degrees on is
             * reached by the ring itself (the crossing excepted: that one swims straight over) */
            float fa = atan2f((f->y - t->hold_y) / 0.85f, (f->x - t->hold_x) / 1.2f);
            float dph = norm_ang(atan2f((wy - t->hold_y) / 0.85f, (wx - t->hold_x) / 1.2f) - fa);
            if (!crossing && fabsf(dph) >= 0.9f) hold_ring_point(t, fa + (dph > 0 ? 0.9f : -0.9f), r, &wx, &wy);
            float ds = tank_dist(f->x, f->y, wx, wy), to = atan2f(wy - f->y, wx - f->x);
            float ex, ey;
            if (piece_exit(t, f, wx, wy, &ex, &ey)) {    /* hidden by a piece (behind the volcano, the castle): out round its nearest side
                                                            first - it cannot see the finger through it - and in front there (Strato 11-01) */
                desired = norm_ang(desired + norm_ang(atan2f(ey - f->y, ex - f->x) - desired) * 0.85f);
                touch_speed = lerpf(18, 30, w);
            } else if (d > HOLD_RING_R + 50) {           /* far fish cross the tank at a cruise, not a drift, to their side of it */
                desired = norm_ang(desired + norm_ang(to - desired) * (0.5f + 0.45f * w));
                float far = d > 170 ? 1.0f : (d - HOLD_RING_R) / 112.0f;
                touch_speed = lerpf(14, 30, w) + far * lerpf(10, 20, w);
            } else {                                     /* at the finger: its attention stays on it (Strato, 0.3.9.3 on the glass:
                                                            "if it swam past it would quickly turn around back to where it was
                                                            focusing its attention") - a short swim to its place when it has
                                                            slipped, then it faces the finger again and hangs there */
                ringing = true;
                if (ds > 45) {                           /* far from its place (the far side, a crossing): a quick swim */
                    desired = norm_ang(desired + norm_ang(to - desired) * 0.8f);
                    touch_speed = 22;
                } else {                                 /* near it: the nose stays on the finger, the fins do the moving */
                    float tf = atan2f(t->hold_y - f->y, t->hold_x - f->x);
                    desired = norm_ang(desired + norm_ang(tf - desired) * 0.9f);
                    touch_speed = clampf((d - r) * 0.3f, 0, 3);
                    paddling = true; pad_x = wx; pad_y = wy;
                }
            }
        }
    } else if (t->greet_timer > 0 && f->trust >= 7.0f && f->goal.id != GOAL_FLEE_SHADOW &&
               f->goal.id != GOAL_SEEK_FOOD) {
        /* light-on greeting: trusting fish come up front to see who's there */
        float gx = TANK_W * 0.5f + (idx - t->n_fish * 0.5f) * 34, gy = 70;
        float d = tank_dist(f->x, f->y, gx, gy);
        if (d > 24) {
            float to = atan2f(gy - f->y, gx - f->x);
            desired = norm_ang(desired + norm_ang(to - desired) * 0.7f);
            touch_speed = 26;
        } else touch_speed = 5;
    } else if (t->ravenous && f->goal.id != GOAL_FLEE_SHADOW && f->hunger > 6.5f) {
        /* starving tank: reflex presentation of the famine, like the greet/
         * hold overrides above - the advisor still owns the goal. Two phases:
         * empty water = beg where meals come from (quick darts back and forth
         * under the feed spot, unmistakably "feed me"); live pellets = the
         * DASH - a real starving fish bolts the moment food hits the water,
         * it doesn't wait to think it over (the advisor's seek_food arrives
         * seconds later and takes back a fish that is already eating). A fish
         * that has eaten (hunger <= 6.5) drops out of the frenzy at once. */
        float fd; int fi = tank_nearest_food(t, f, &fd);
        if (fi >= 0) {
            float to = atan2f(t->food[fi].y - f->y, t->food[fi].x - f->x);
            desired = norm_ang(desired + norm_ang(to - desired) * 0.92f);
            touch_speed = fd > 14 ? lerpf(85, 115, clampf(f->hunger / 10.0f, 0, 1)) : 30;
        } else {
            float spot = t->feed_spot_x >= 0 ? t->feed_spot_x : TANK_W * 0.5f;
            if ((f->beg += dt * (1.6f + f->bold * 0.9f)) >= 6.2831853f) f->beg = fmodf(f->beg, 6.2831853f);   /* (its own phase: a bold that drifts never jumps it) */
            float wx = spot + sinf(f->beg + idx * 2.1f) * (34 + idx * 6);
            float wy = 30 + idx * 4 + sinf(anim(t) * 3.1f + f->wander) * 6;
            float to = atan2f(wy - f->y, wx - f->x);
            desired = norm_ang(desired + norm_ang(to - desired) * 0.85f);
            touch_speed = tank_dist(f->x, f->y, wx, wy) > 18
                        ? lerpf(40, 68, clampf(f->hunger / 10.0f, 0, 1)) : 22;
        }
    } else if (f->breather > 0) {
        /* the breather: to the fronds at a tired cruise, then a hover (the
         * canopy's own slowing below keeps it gentle); the goal stays */
        float bx, by; breather_spot(t, f, idx, &bx, &by);
        float d = tank_dist(f->x, f->y, bx, by);
        if (d > 10) { float to = atan2f(by - f->y, bx - f->x); desired = norm_ang(desired + norm_ang(to - desired) * 0.85f); }
        touch_speed = d > BREATHER_NEAR ? 18 : 3;
    } else if (t->court_active > 0 && !t->ravenous &&
               (idx == t->court_a || idx == t->court_b) &&
               f->goal.id != GOAL_FLEE_SHADOW && f->hunger < HOLD_HUNGER_VETO) {
        /* the tell: an episode of the courtship circle, the arrival close */
        court_steer(t, f, idx, &desired, &touch_speed);
    }

    /* separation - personal space. Fish are ~40 px long; the old 16 px
     * radius let four content fish stack on one landmark like a pile of
     * cards (2026-09-01). Now a body length, weighted by how close they are;
     * a fleeing fish keeps its line. */
    if (f->goal.id != GOAL_FLEE_SHADOW && !paddling)   /* (a fish paddling at the finger keeps its nose on it: its place is a body apart already) */
        for (int i = 0; i < N_FISH_SLOTS; i++) {
            if (i == idx || !tank_swims(t, i)) continue;
            float d = tank_dist(f->x, f->y, t->fish[i].x, t->fish[i].y), r = 36 * f->size;
            if (d < r) {
                float away = atan2f(f->y - t->fish[i].y, f->x - t->fish[i].x);
                desired = norm_ang(desired + norm_ang(away - desired) * (0.35f + 0.5f * (1 - d / r)));
            }
        }

    /* urgency scales cruise speed a touch (0..9 → 0.8..1.25) */
    float ugain = 0.8f + f->goal.urgency * 0.05f;
    /* a U-turn is a YAW (2026-09-29): a fish whose way lies behind it no longer
     * loops over through vertical and rolls flat - it turns round where it is,
     * toward the glass. (A YouTube viewer on the old roll: "that looks bad".)
     * It swims on one SIDE (facing) and changes side only by a committed turn:
     * the way must stay behind it for a moment - ~2 s hovering (3 at its
     * spot), a third of a second cruising, at once when it matters (a dash,
     * a startle, darting play) - and a turn is followed by ~1.3 s of settling. Until it commits
     * it glances back (a part turn) and eases off, steering on its own side:
     * the way mirrored across vertical, its climb or dive kept. Strato, the
     * same night, on the first cut (a turn the moment the way fell behind):
     * "too much time in a vertical position or flip flopping back and forth" -
     * 21 flips a fish-minute, 34 at the bubble column, where the lane swings
     * side to side every ~5 s. */
    const float BEHIND_BAND  = 0.15f;    /* |cos| past vertical before the way is "behind" */
    const float TURN_SETTLE  = 1.3f;     /* s after a turn before the next can begin */
    if (!f->facing) f->facing = cosf(f->heading) < 0 ? -1 : 1;
    /* through a doorway nothing else steers (2026-10-05: on the boards an
       accepted fish was pulled back by a reflex laid over its heading - a
       hesitation's glance, the courtship, a finger - and missed the window):
       the door's line and side again, last, before the side is decided */
    if (through) {
        bool out = f->door == DOOR_PASSING, left = f->door_edge == 0;
        desired = ((out == left) ? 3.14159265f : 0.0f) + (DOOR_Y - f->y) * 0.004f;
        int8_t want = (out == left) ? -1 : 1;
        if (f->facing != want) { f->facing = want; f->behind = -1.3f; }
        touch_speed = -1; hes_speed = -1;
    }
    bool urgent = agile || (t->ravenous && f->hunger > 6.5f) || (t->startled && touch_speed >= 55) || f->bolt_home || follow >= 0;
    /* a breather settled in its fronds has nowhere to go: it hovers on the side it came in on (0.3.9.3, the
     * square's "fish spinning in the corner" - within 10 px of its spot the steering let go, the old way lay
     * behind it, and it turned, drifted off, steered back and turned again: ~30 turns in 100 s) */
    bool behind = !(settled && f->breather > 0) && cosf(desired) * f->facing < -BEHIND_BAND;
    if (behind) f->behind += dt;
    else f->behind = f->behind > 0 ? 0 : fminf(0, f->behind + dt);
    /* hanging about its spot (the goal's point within ~30 px) a fish is in
     * no hurry: the lane swinging past it is a glance, not a turn */
    bool at_spot = tg.valid && tank_dist(f->x, f->y, tg.x, tg.y) < 30;
    float commit = urgent ? 0 : ringing ? 0.2f    /* at the finger: a fish that slipped past turns back to it at once */
                 : lerpf(2.0f, 0.3f, clampf((f->speed - 10) * (1 / 25.0f), 0, 1)) + (at_spot ? 1.2f : 0);
    if (behind && f->behind >= commit) {
        f->facing = (int8_t)-f->facing;
        f->heading = norm_ang(3.14159f - f->heading);   /* the climb or dive kept */
        f->behind = urgent ? 0 : -TURN_SETTLE;
    }
    bool glancing = f->behind > 0;       /* the way is behind, the turn not yet committed */
    if (cosf(desired) * f->facing < 0)   /* on its own side until it turns */
        desired = atan2f(sinf(desired), f->facing * fabsf(cosf(desired)));
    float turn_rate = f->turn_rate * tank_form_turn(f);  /* its form's feel (0.4); exactly f->turn_rate for a classic */
    float turn = clampf(norm_ang(desired - f->heading), -turn_rate * dt, turn_rate * dt);
    f->heading = norm_ang(f->heading + turn);
    /* the yaw follows the side: a full turn in ~0.5 s at mira's turn rate
     * (bolt 0.4, nori 0.7), quicker with urgency; a glance is 40% of one.
     * The tail follows the head a beat behind (tank_fish_trail) */
    float hc = cosf(f->heading);
    float face = f->facing * (glancing ? 0.6f : 1.0f);
    float yrate = turn_rate * 1.2f * (1 + f->goal.urgency * 0.06f) * dt;
    f->yaw += clampf(face - f->yaw, -yrate, yrate);
    float want = touch_speed >= 0 ? touch_speed : hes_speed >= 0 ? hes_speed : tg.speed * ugain;
    f->target_speed = want * (f->energy < 1.2f && f->breather <= 0 ? 0.45f : 1) * (glancing ? 0.6f : 1);
    if (f->heat < HEAT_SLOW) f->target_speed *= HEAT_SLOW_MIN + (1 - HEAT_SLOW_MIN) * f->heat / HEAT_SLOW;   /* cold: sluggish (x0.5 at heat 0) */
    if (through) f->target_speed = f->bolt_home ? DOOR_BOLT_SPEED : DOOR_SWIM_SPEED;        /* through a doorway at a swim, never a dawdle (the hand-off waits LINK_CROSS_MS) */
    /* swimming through a canopy is slow going (a fleeing fish crashes through) */
    if (f->goal.id != GOAL_FLEE_SHADOW)
        for (int b = 0; b < tank_veg_beds(t); b++)
            if (veg_inside(t, b, f->x, f->y)) { f->target_speed *= VEG_SLOW; break; }
    f->speed = lerpf(f->speed, f->target_speed, clampf(dt * 2.6f, 0, 1));
    /* the swim follows the facing: side-on it is the heading's own, and as
     * the fish turns head-on its level travel crosses zero and it brakes to
     * 40%. Only a LEVEL turn: steep, the heading steers as it always did (a
     * diver's small corrections at the pellet don't wait on a flip) */
    float ahc = fabsf(hc), w = clampf((ahc - 0.2f) * (1 / 0.4f), 0, 1);
    float brake = 1 - 0.6f * (1 - fabsf(f->yaw)) * ahc * w;
    f->x += (hc + (f->yaw * ahc - hc) * w) * f->speed * brake * dt;
    f->y += sinf(f->heading) * f->speed * brake * dt + sinf(anim(t) * 1.4f + f->wander) * dt * 1.7f;
    if (paddling) {                              /* to its place at the finger without turning: a fin's few px a second */
        float px = pad_x - f->x, py = pad_y - f->y, pd = sqrtf(px * px + py * py);
        if (pd > 1) { float v = fminf(pd * 0.9f, 9.0f) * dt / pd; f->x += px * v; f->y += py * v; }
    }

    /* hard bounds. The side glass mirrors a heading only while it points
     * INTO the glass: the facing lags the mirror, and a second bounce on the
     * next frame would send the fish back into the glass */
    float m = 15 * f->size;
    if (through) {                                         /* in an open doorway: no glass, only the band's height */
        f->y = clampf(f->y, DOOR_Y - DOOR_BAND_H / 2 + m, DOOR_Y + DOOR_BAND_H / 2 - m);
    } else {
#ifdef TANK_ROUND                                          /* the bowl's glass: back onto the circle m inside it, the heading mirrored
                                                              in the glass while it points out, the body turned to swim back in */
    { float dx = f->x - TANK_RAD, dy = f->y - TANK_RAD, d = sqrtf(dx * dx + dy * dy), lim = TANK_RAD - m;
      if (d > lim && d > 1) {
          float nx = dx / d, ny = dy / d;
          f->x = TANK_RAD + nx * lim; f->y = TANK_RAD + ny * lim;
          float hx = cosf(f->heading), hy = sinf(f->heading), out = hx * nx + hy * ny;
          if (out > 0) {
              hx -= 2 * out * nx; hy -= 2 * out * ny; f->heading = atan2f(hy, hx);
              if (nx < -0.5f && f->facing < 0) { f->facing = 1; f->behind = -TURN_SETTLE; }
              if (nx > 0.5f && f->facing > 0)  { f->facing = -1; f->behind = -TURN_SETTLE; }
          }
      }
      if (f->y > TANK_BOT - m) { f->y = TANK_BOT - m; f->heading = -f->heading; } }
#else
    if (f->x < m)          { f->x = m;          if (cosf(f->heading) < 0) f->heading = norm_ang(3.14159f - f->heading);
                                                if (f->facing < 0) { f->facing = 1; f->behind = -TURN_SETTLE; } }
    if (f->x > TANK_W - m) { f->x = TANK_W - m; if (cosf(f->heading) > 0) f->heading = norm_ang(3.14159f - f->heading);
                                                if (f->facing > 0) { f->facing = -1; f->behind = -TURN_SETTLE; } }
    if (f->y < m)          { f->y = m;          f->heading = -f->heading; }
    if (f->y > TANK_H - m) { f->y = TANK_H - m; f->heading = -f->heading; }
#ifdef TANK_CORNER_R                                       /* the watch's corners: back onto the arc m inside it, the heading mirrored
                                                              in the glass while it points out (the bowl's rule, a corner at a time) */
    { float cx, cy, d = corner_in(f->x, f->y, &cx, &cy), lim = TANK_CORNER_R - m;
      if (d > lim && d > 1) {
          float nx = (f->x - cx) / d, ny = (f->y - cy) / d;
          f->x = cx + nx * lim; f->y = cy + ny * lim;
          float hx = cosf(f->heading), hy = sinf(f->heading), out = hx * nx + hy * ny;
          if (out > 0) {
              hx -= 2 * out * nx; hy -= 2 * out * ny; f->heading = atan2f(hy, hx);
              if (nx < -0.5f && f->facing < 0) { f->facing = 1; f->behind = -TURN_SETTLE; }
              if (nx > 0.5f && f->facing > 0)  { f->facing = -1; f->behind = -TURN_SETTLE; }
          }
      } }
#endif
#endif
    }

    tank_fish_trail(f, dt);                      /* last: the body and its tail follow where the swim went */
    fish_beat(f, dt);
    eat_nearby_food(t, f);
}

/* ---- THE SWIM as a movement module (locomotion.h): what tank.c asks of an animal that swims ---- */
/* the finger itself: a swimmer goes to it (the ring round it, hold_ring_point) */
static void swim_hand_point(const tank_t *t, float *x, float *y) { *x = t->hold_x; *y = t->hold_y; }
/* it has come to a settled hold: within the ring's outer reach of the finger (hold_time, not hold_active: the tick consumes the
   flag, and the time drops to 0 the frame the finger lifts) */
static bool swim_at_finger(const tank_t *t, int i) {
    float hx, hy; tank_hand_point(t, &hx, &hy);
    return t->hold_time >= HOLD_ATTRACT_S
        && tank_dist(t->fish[i].x, t->fish[i].y, hx, hy) < HOLD_GATHERED_PX;
}
/* a hold-approach = a fish that was out in the tank when the settled hold began its draw and then came all the way in (tank.c touch_tick) */
static void swim_hold_credit(tank_t *t, int i, float d, bool draw_begins) {
    fish_t *f = &t->fish[i];
    if (draw_begins) f->hold_far = d >= HOLD_APPROACH_FROM;
    if (f->hold_far && d < HOLD_APPROACH_AT) hold_approach(t, i);
}
/* a fish: its middle, as ever */
static float swim_tap_d2(const tank_t *t, int i, float x, float y, float px, float py) { return (px - x) * (px - x) + (py - y) * (py - y); }
/* SWIM DEPTH: how it goes about its new goal (a lizard walks the floor's lanes) */
static void swim_goal_set(tank_t *t, fish_t *f, goal_id_t g) {
    f->gdepth_want = depth_pick(t, f, g);
    f->pdepth_want = pdepth_pick(t, f, g);
}
const loco_ops_t loco_swim = {
    .name = "swim", .mode = CAP_SWIM,
    .update = update_fish, .hand_point = swim_hand_point, .at_finger = swim_at_finger, .hold_credit = swim_hold_credit,
    .tap_d2 = swim_tap_d2, .goal_set = swim_goal_set,
};
