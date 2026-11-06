/* crickets.c - THE CRICKETS and the dish, a food module (mechanics.h food_crickets): the prey every lizard sees
 * (issue #4 phase 3: moved out of tank.c verbatim; the feed, trickle, night and sight are tank.c's former terrarium branches) */
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

/* ================================================================ THE CRICKETS (pocket-terrarium P2c; tank.h has the rules):
 * the terrarium's live prey - from the keeper's hand (tank_feed) or the tank's own trickle (tank_scatter_food) - falling to
 * the first top under them, sitting, hopping along their own path, flying from a lizard that comes near, crawling into
 * cover and out again, gone after CK_LIFE_S; and the mealworm dish. THE WALK's hunt eats them. A drop draws from the tank's
 * random stream what a pellet's did (its x, its height under the glass), so a seeded tank starts as it always has. */
const char *const CK_STATE_NAMES[CK_STATES] = { "none", "fall", "sit", "hop", "crawl", "hide" };
#define CK_HIDE_EDGE (TERRAIN_HIDE_HW - 6.0f)   /* cover by a hide: tucked against its side */
#define CK_POP_VY    (-110.0f)                  /* a drop off a rock or a ramp: up a little first (px/s) */
float ck_ground(const terrain_path_t *p, float x) { return terrain_y(p, x) + TERRAIN_FEET_DY; }
/* the x its feet may take on path p: the path's own ends, and the glass CK_GLASS_M in at the ends' height (its feet's) */
static void ck_span(const terrain_path_t *p, float *x0, float *x1) {
    *x0 = fmaxf(p->x[0], tank_glass_x0(p->y[0] + TERRAIN_FEET_DY) + CK_GLASS_M);
    *x1 = fminf(p->x[p->n - 1], tank_glass_x1(p->y[p->n - 1] + TERRAIN_FEET_DY) - CK_GLASS_M);
    if (*x0 > *x1) *x0 = *x1 = 0.5f * (*x0 + *x1);
}
bool tank_cricket_about(const cricket_t *c) { return c->state == CK_SIT || c->state == CK_HOP || c->state == CK_CRAWL; }
int tank_crickets_about(const tank_t *t) { int n = 0; for (int i = 0; i < MAX_CRICKETS; i++) n += tank_cricket_about(&t->cricket[i]); return n; }
bool tank_dish_here(const tank_t *t) { return tank_habitat(t)->dish; }
void tank_dish_point(const tank_t *t, float *x, float *y) { *x = tank_floor_x(tank_habitat(t)->dish_u); *y = PLACE_Y + TERRAIN_FEET_DY; }
static void crickets_clear(tank_t *t) {
    for (int i = 0; i < MAX_CRICKETS; i++) t->cricket[i].state = CK_NONE;
    for (int i = 0; i < N_FISH_SLOTS; i++) { t->fish[i].prey = -1; t->fish[i].seen_surf = -1; }   /* (no prey, nor a memory of one) */
}
/* paths a and b of the ground are joined: a walker gets from one to the other through the links (terrain_check: every path
   of a habitat is reachable from the floor - so on a habitat that passed its rules, every pair) */
static bool ground_joined(const terrain_t *tr, int a, int b) {
    if (a == b) return true;
    if (a < 0 || b < 0 || a >= tr->n_paths || b >= tr->n_paths) return false;
    int lab[TERRAIN_PATHS_MAX];
    for (int k = 0; k < tr->n_paths; k++) lab[k] = k;
    for (int pass = 0; pass < tr->n_paths; pass++)                /* (the labels flow along the links: a dozen of them) */
        for (int l = 0; l < tr->n_links; l++) {
            const int pa = tr->link[l].a, pb = tr->link[l].b, m = lab[pa] < lab[pb] ? lab[pa] : lab[pb];
            lab[pa] = lab[pb] = m;
        }
    return lab[a] == lab[b];
}
/* a lizard's PREY to hunt or see (tank.h THE CRICKETS): slot i's cricket about, or (PREY_DISH) the dish while it holds
   worms - its path and x; false: no such prey now */
bool prey_spot(const tank_t *t, int i, int *surf, float *x, float *y) {
    if (i >= 0 && i < MAX_CRICKETS) {
        const cricket_t *c = &t->cricket[i];
        if (!tank_cricket_about(c) || c->surf < 0) return false;
        *surf = c->surf; *x = c->x; *y = c->y;
        return true;
    }
    if (i == PREY_DISH && t->dish_worms > 0 && tank_dish_here(t)) { *surf = 0; tank_dish_point(t, x, y); return true; }
    return false;
}
static bool crickets_sight(const tank_t *t, int idx, float *x, float *y, float *dist) {
    const fish_t *f = &t->fish[idx];
    const terrain_t *tr = tank_terrain(t);
    const int from = f->surf >= 0 && f->surf < tr->n_paths ? f->surf : 0;
    float bd = 1e9f, bx = 0, by = 0; bool got = false;
    for (int i = 0; i <= PREY_DISH; i++) {                         /* the crickets about on ground its own joins, and the dish */
        int s; float px, py;
        if (!prey_spot(t, i, &s, &px, &py) || !ground_joined(tr, from, s)) continue;
        const float d = tank_dist(f->x, f->y, px, py);
        if (d < bd) { bd = d; bx = px; by = py; got = true; }
    }
    if (!got) return false;
    if (x) *x = bx;
    if (y) *y = by;
    if (dist) *dist = bd;
    return true;
}
static int crickets_count(const tank_t *t) {
    int n = 0;
    for (int i = 0; i < MAX_CRICKETS; i++) n += t->cricket[i].state != CK_NONE;
    return n + (t->dish_worms > 0 && tank_dish_here(t));
}
/* the first top under (x, y) but path `skip`'s: the ground at x nearest at or below y (a walking line + TERRAIN_FEET_DY); -1 none */
static int ck_top_under(const terrain_t *tr, float x, float y, int skip, float *gy) {
    int best = -1; float by = 1e9f;
    for (int s = 0; s < tr->n_paths; s++) {
        const terrain_path_t *p = &tr->path[s];
        if (s == skip || x < p->x[0] || x > p->x[p->n - 1]) continue;
        const float g = ck_ground(p, x);
        if (g >= y - 0.01f && g < by) { by = g; best = s; }
    }
    if (best >= 0) *gy = by;
    return best;
}
static int ck_free(const tank_t *t) { for (int i = 0; i < MAX_CRICKETS; i++) if (t->cricket[i].state == CK_NONE) return i; return -1; }
static float ck_drop_x(const tank_t *t, float x) { float x0, x1; ck_span(&tank_terrain(t)->path[0], &x0, &x1); return clampf(x, x0, x1); }   /* (over the floor's span) */
static void ck_drop_at(tank_t *t, int i, float x, float y, bool from_player) {
    t->cricket[i] = (cricket_t){ .x = x, .y = y, .surf = -1, .state = CK_FALL, .facing = (int8_t)(i & 1 ? 1 : -1), .from_player = from_player };
    t->ck_dropped++;
}
int tank_cricket_drop(tank_t *t, float x, bool from_player) {
    const int i = ck_free(t);
    if (i < 0) return -1;
    x = ck_drop_x(t, x);
    ck_drop_at(t, i, x, tank_glass_top(x) + tank_randf(t, 6, 18), from_player);
    return i;
}
static void ck_sit(tank_t *t, cricket_t *c) {
    c->state = CK_SIT; c->vy = 0;
    c->timer = tank_randf(t, CK_SIT_LO_S, CK_SIT_HI_S) * (c->alert > 0 ? 0.5f : 1.0f);   /* (frightened: its next move sooner) */
    c->y = ck_ground(&tank_terrain(t)->path[c->surf], c->x);
}
/* a hop along its path to `to` (its span's bounds) */
static void ck_hop(tank_t *t, cricket_t *c, float to) {
    float x0, x1; ck_span(&tank_terrain(t)->path[c->surf], &x0, &x1);
    to = clampf(to, x0, x1);
    const float len = fabsf(to - c->x);
    c->state = CK_HOP; c->timer = 0; c->x0 = c->x; c->x1 = to;
    c->hop_s = CK_HOP_T0_S + len / CK_HOP_PX_S; c->hop_h = CK_HOP_H0 + CK_HOP_HK * len;
    if (len > 0.5f) c->facing = (int8_t)(to > c->x ? 1 : -1);
}
/* cover on path s within CK_COVER_REACH_PX of x: the floor's - a hide's sides, the floor under a rock's end; a rock's - its
   ends; a ramp has none */
static bool ck_cover(const tank_t *t, int s, float x, float *cx) {
    const terrain_t *tr = tank_terrain(t);
    const terrain_path_t *p = &tr->path[s];
    float x0, x1; ck_span(p, &x0, &x1);
    float best = CK_COVER_REACH_PX; bool got = false;
#define CK_CAND(v) do { const float v_ = clampf((v), x0, x1), d_ = fabsf(v_ - x); if (d_ < best) { best = d_; *cx = v_; got = true; } } while (0)
    if (p->kind == PATH_FLOOR) {
        const habitat_t *h = tank_habitat(t);
        for (int k = 0; k < h->n_hides; k++) { float hx, hy; tank_hide_point(t, k, &hx, &hy); CK_CAND(hx - CK_HIDE_EDGE); CK_CAND(hx + CK_HIDE_EDGE); }
        for (int r = 1; r <= tr->n_rocks; r++) { CK_CAND(tr->path[r].x[0] + CK_EDGE_PX); CK_CAND(tr->path[r].x[tr->path[r].n - 1] - CK_EDGE_PX); }
    } else if (p->kind == PATH_ROCK) { CK_CAND(p->x[0] + CK_EDGE_PX); CK_CAND(p->x[p->n - 1] - CK_EDGE_PX); }
#undef CK_CAND
    return got;
}
/* a fright, away from from_x: a longer hop along its path - or, up on a rock or a ramp, cornered or CK_DROP_ODDS of the
   time, off the front down to the ground below (it falls past the path it left; never where no ground is under it - a
   rock high in the round bowl reaches past the floor's ends); on the floor, cornered, the other way (over the lizard) */
void ck_flee(tank_t *t, cricket_t *c, float from_x) {
    c->alert = CK_ALERT_S;
    if (c->state == CK_FALL || c->surf < 0) return;
    const terrain_t *tr = tank_terrain(t);
    const terrain_path_t *p = &tr->path[c->surf];
    float x0, x1; ck_span(p, &x0, &x1);
    int dir = c->x >= from_x ? 1 : -1;
    float room = dir > 0 ? x1 - c->x : c->x - x0, gy;
    if (p->kind != PATH_FLOOR && ck_top_under(tr, c->x, c->y, c->surf, &gy) >= 0 && (room < CK_FLEE_LO_PX || tank_randf(t, 0, 1) < CK_DROP_ODDS)) {
        c->state = CK_FALL; c->vy = CK_POP_VY; c->facing = (int8_t)dir;   /* (surf kept: the path it falls past) */
        return;
    }
    if (room < CK_FLEE_LO_PX) { dir = -dir; room = dir > 0 ? x1 - c->x : c->x - x0; }
    ck_hop(t, c, c->x + dir * fminf(tank_randf(t, CK_FLEE_LO_PX, CK_FLEE_HI_PX), room));
}
/* a lizard snout near enough to frighten the cricket - walking (faster than CK_CALM_PX_S) within CK_ALERT_PX, creeping or
   turning within CK_CREEP_PX, still within CK_STILL_PX; hidden in cover, any within CK_FLUSH_PX - out of a hide, not
   mid-lunge (a strike is its own roll): the nearest's snout x and its odds' share; -1 none */
static int ck_threat(const tank_t *t, const cricket_t *c, float *from_x, float *share) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < N_FISH_SLOTS; i++) {
        if (!tank_swims(t, i)) continue;
        const fish_t *f = &t->fish[i];
        if (f->hide >= 0 || f->pose == LZ_POSE_LUNGE) continue;
        const float half = tank_lizard_half_len(t, f);
        const float sx = f->x + f->facing * half * cosf(f->slope), sy = f->y + TERRAIN_FEET_DY - f->facing * half * sinf(f->slope);
        const float d = tank_dist(c->x, c->y, sx, sy);
        const bool walking = f->speed > CK_CALM_PX_S, moving = f->speed > 0 || f->turn_s > 0;
        const float range = c->state == CK_HIDE ? CK_FLUSH_PX : walking ? CK_ALERT_PX : moving ? CK_CREEP_PX : CK_STILL_PX;
        if (d < range && d < bd) { bd = d; best = i; *from_x = sx; *share = c->state == CK_HIDE ? 1e6f : walking ? 1.0f : moving ? 0.5f : 0.33f; }
    }
    return best;
}
/* the rap: the crickets about within r of (x, y) fly from it; one in cover stays put, wide awake */
static void ck_scare(tank_t *t, float x, float y, float r) {
    for (int i = 0; i < MAX_CRICKETS; i++) {
        cricket_t *c = &t->cricket[i];
        if (c->state == CK_NONE || c->state == CK_FALL || tank_dist(c->x, c->y, x, y) > r) continue;
        if (c->state == CK_SIT || c->state == CK_CRAWL) ck_flee(t, c, x); else c->alert = CK_ALERT_S;
    }
}
/* a frame of the crickets; returns the live ones (any state) */
static int ck_tick(tank_t *t, float dt) {
    const terrain_t *tr = tank_terrain(t);
    int live = 0;
    for (int i = 0; i < MAX_CRICKETS; i++) {
        cricket_t *c = &t->cricket[i];
        if (c->state == CK_NONE) continue;
        if ((c->age += dt) > CK_LIFE_S) { c->state = CK_NONE; t->ck_lost++; continue; }   /* uneaten: gone */
        live++;
        c->alert = fmaxf(0, c->alert - dt);
        if (c->state == CK_FALL) {                              /* down to the first top under it - never through rock */
            const float vy = fminf(c->vy + CK_GRAVITY * dt, CK_FALL_MAX), ny = c->y + vy * dt;
            float gy = 0; const int s = vy > 0 ? ck_top_under(tr, c->x, c->y, c->surf, &gy) : -1;
            if (s >= 0 && ny >= gy) {
                float x0, x1; ck_span(&tr->path[s], &x0, &x1);
                c->surf = (int8_t)s; c->x = clampf(c->x, x0, x1);
                ck_sit(t, c); c->timer *= 0.5f;                 /* (landed: a first move soon) */
            } else { c->y = ny; c->vy = vy; }
            continue;
        }
        const terrain_path_t *p = &tr->path[c->surf];
        float fx = 0, share = 0;
        if (c->state != CK_HOP && ck_threat(t, c, &fx, &share) >= 0 && tank_randf(t, 0, 1) < CK_NOTICE_PER_S * share * dt) { ck_flee(t, c, fx); continue; }
        switch (c->state) {
        case CK_SIT:
            c->y = ck_ground(p, c->x);
            if ((c->timer -= dt) <= 0) {                        /* its next move: into cover now and then, else a hop */
                float cx = c->x;                                 /* (ck_cover sets it when it finds cover) */
                if (tank_randf(t, 0, 1) < (c->alert > 0 ? CK_COVER_SCARED : CK_COVER_ODDS) && ck_cover(t, c->surf, c->x, &cx)) {
                    c->state = CK_CRAWL; c->x1 = cx;
                    if (fabsf(cx - c->x) > 0.5f) c->facing = (int8_t)(cx > c->x ? 1 : -1);
                } else {
                    float x0, x1; ck_span(p, &x0, &x1);
                    int dir = tank_randf(t, 0, 1) < 0.7f ? c->facing : -c->facing;
                    if ((dir > 0 ? x1 - c->x : c->x - x0) < CK_HOP_LO_PX) dir = -dir;
                    ck_hop(t, c, c->x + dir * tank_randf(t, CK_HOP_LO_PX, CK_HOP_HI_PX));
                }
            }
            break;
        case CK_HOP: {
            c->timer += dt;
            const float ph = clampf(c->timer / c->hop_s, 0, 1);
            c->x = c->x0 + (c->x1 - c->x0) * ph;
            c->y = ck_ground(p, c->x) - c->hop_h * 4 * ph * (1 - ph);
            if (ph >= 1) ck_sit(t, c);
            break; }
        case CK_CRAWL: {
            const float d = c->x1 - c->x, step = CK_CRAWL_PX_S * dt;
            if (fabsf(d) <= step) { c->x = c->x1; c->state = CK_HIDE; c->timer = tank_randf(t, CK_HIDE_LO_S, CK_HIDE_HI_S); }
            else c->x += d > 0 ? step : -step;
            c->y = ck_ground(p, c->x);
            break; }
        case CK_HIDE:
            if ((c->timer -= dt) <= 0) {                        /* out again: a hop back the way it crawled in (no room: the other way) */
                float x0, x1; ck_span(p, &x0, &x1);
                int dir = -c->facing;
                if ((dir > 0 ? x1 - c->x : c->x - x0) < CK_HOP_LO_PX) dir = -dir;
                ck_hop(t, c, c->x + dir * tank_randf(t, CK_HOP_LO_PX, CK_HOP_HI_PX));
            }
            break;
        default: break;
        }
    }
    return live;
}

/* ---- THE CRICKETS as a food module (mechanics.h): what tank.c's feed, trickle, tick, night and sight were for the terrarium ---- */
/* the keeper's gesture: n crickets from the hand at x - or, over the dish, the dish filled (THE CRICKETS, tank.h) */
static void crickets_feed(tank_t *t, float x, int n) {
    float dx = 0, dy;
    if (tank_dish_here(t)) tank_dish_point(t, &dx, &dy);
    if (tank_dish_here(t) && fabsf(x - dx) <= DISH_FEED_PX) {
        if (t->dish_worms < DISH_WORMS) { t->dish_worms = DISH_WORMS; t->dish_farm = false; tank_emit(TEV_FEED, -1); }   /* (the keeper's dish now: P5) */
    } else {
        int dropped = 0;
        for (int i = ck_free(t); i >= 0 && n > 0; i = ck_free(t), n--) {   /* (the draws a pellet's were: its jitter, its height) */
            const float cx = ck_drop_x(t, x + tank_randf(t, -14, 14));
            ck_drop_at(t, i, cx, tank_glass_top(cx) + tank_randf(t, 6, 18), true);
            dropped++;
        }
        if (dropped) tank_emit(TEV_FEED, -1);
    }
    t->feed_spot_x = t->feed_spot_x < 0 ? x : t->feed_spot_x + (x - t->feed_spot_x) * 0.3f;
    t->feed_open = true;                     /* a meal once a lizard eats from it */
}
/* AUTO CRICKETS (tank.h THE CRICKETS): the tank's own, at a random x (a pellet's draws) */
static void crickets_scatter(tank_t *t, int n) {
    for (int i = ck_free(t); i >= 0 && n > 0; i = ck_free(t), n--) {
        const float x = ck_drop_x(t, tank_randf(t, TANK_FX0 + 25, TANK_FX1 - 25));
        ck_drop_at(t, i, x, tank_glass_top(x) + tank_randf(t, 6, 20), false);
    }
}
/* a night: the crickets live out their time (a night: gone); the dish's worms stay */
static void crickets_sleep(tank_t *t, float seconds) {
    for (int i = 0; i < MAX_CRICKETS; i++) {
        cricket_t *c = &t->cricket[i];
        if (c->state != CK_NONE && (c->age += seconds) > CK_LIFE_S) { c->state = CK_NONE; t->ck_lost++; }
    }
}
/* (P2b: nobody feeds lizards after lights-out - settle or sleep; P2c: AUTO CRICKETS, in the active phase only) */
static bool crickets_trickle_hold(const tank_t *t) { return tank_activity_phase(t) != PHASE_ACTIVE; }
const food_ops_t food_crickets = {
    .name = "crickets", .diet = DIET_CRICKET | DIET_MEALWORM,
    .feed = crickets_feed, .scatter = crickets_scatter, .tick = ck_tick, .sleep = crickets_sleep, .sight = crickets_sight,
    .count = crickets_count, .clear = crickets_clear, .trickle_hold = crickets_trickle_hold, .scare = ck_scare,
};
