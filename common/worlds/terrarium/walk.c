/* walk.c - the terrarium's walk, a movement module (locomotion.h loco_walk): the ground, the plan, the lizard's frame (update_lizard)
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

/* ---- THE WALK's footing (P2b, P2c; tank.h; the rest is with update_lizard): its middle on its path's walking line -
 * the floor's in the floor's band - its body inside the glass at its height (LZ_GLASS_M, and the body as drawn: its
 * tank_lizard_half_len), facing one way or the other ---- */
float tank_lizard_scale(const fish_t *f) { return lizard_stage_scale(f->stage) * (1 + 0.5f * (clampf(f->base_size, 0.8f, 1.1f) - 1)); }   /* (at most 1.08 x 1.05:
                                                                               a half length of 47.6 px, under TERRAIN_LINK_IN_PX) */
float tank_lizard_half_len(const tank_t *t, const fish_t *f) { return lizard_half_len(&tank_species(t)->geom, tank_lizard_scale(f)); }
static float walk_margin(const tank_t *t, const fish_t *f) { return fmaxf(LZ_GLASS_M, tank_lizard_half_len(t, f)); }
/* the x its middle may take on path p (on the floor, `lane` px off its line): the path's own ends, and the glass at the
   ends' height (a ramp's end, where it lands, is TERRAIN_LINK_IN_PX in: never cut) */
void walk_span(const tank_t *t, const fish_t *f, const terrain_path_t *p, float lane, float *x0, float *x1) {
    const float m = walk_margin(t, f), y0 = p->y[0] + (p->kind == PATH_FLOOR ? lane : 0), y1 = p->y[p->n - 1] + (p->kind == PATH_FLOOR ? lane : 0);
    *x0 = fmaxf(p->x[0], tank_glass_x0(y0) + m); *x1 = fminf(p->x[p->n - 1], tank_glass_x1(y1) - m);
    if (*x0 > *x1) *x0 = *x1 = (*x0 + *x1) * 0.5f;          /* (no ground that narrow on any board) */
}
static void walk_face(fish_t *f, int facing) { f->facing = (int8_t)(facing < 0 ? -1 : 1); f->heading = f->facing > 0 ? 0.0f : 3.14159265f; }
/* it stands on path s: over its span, on its line (the floor's: in its band) */
static bool walk_on(const terrain_t *tr, const fish_t *f, int s) {
    const terrain_path_t *p = &tr->path[s];
    if (!(f->x >= p->x[0] - 0.5f && f->x <= p->x[p->n - 1] + 0.5f)) return false;
    const float d = f->y - terrain_y(p, f->x);
    return s == 0 ? d >= LZ_LANE_BACK - 0.01f && d <= LZ_LANE_FRONT + 0.01f : fabsf(d) <= 0.5f;
}
static void walk_ground(const tank_t *t, fish_t *f) {         /* onto its path where it is: a newcomer, a staging, a wake */
    const terrain_t *tr = tank_terrain(t);
    if (f->x != f->x || f->y != f->y) { f->surf = 0; f->x = tank_floor_x(0.5f); f->y = PLACE_Y; }   /* (a NaN: the floor's middle) */
    if (f->surf < 0 || f->surf >= tr->n_paths || !walk_on(tr, f, f->surf))   /* staged off its path: the floor if it is down by it (the */
        f->surf = (int8_t)(f->y >= PLACE_Y - TERRAIN_LIFT_PX * 0.5f ? 0 : terrain_at(tr, f->x, f->y));   /* rocks stand higher), else the nearest ground over it */
    f->lane = clampf(f->lane == f->lane ? f->lane : 0, LZ_LANE_BACK, LZ_LANE_FRONT);
    const terrain_path_t *p = &tr->path[f->surf];
    if (f->surf == 0 && !(f->y >= PLACE_Y + LZ_LANE_BACK && f->y <= PLACE_Y + LZ_LANE_FRONT)) f->y = PLACE_Y + f->lane;
    float x0, x1; walk_span(t, f, p, f->surf == 0 ? f->y - PLACE_Y : 0, &x0, &x1);
    f->x = clampf(f->x, x0, x1);
    if (f->surf != 0) f->y = terrain_y(p, f->x);
    walk_face(f, f->facing ? f->facing : cosf(f->heading) < 0 ? -1 : 1);
    if (f->turn_s <= 0) f->yaw = f->facing;
}
static void walk_floor(const tank_t *t, fish_t *f) {          /* onto the floor where it is across: a lizard made, bought, hatched */
    f->surf = 0; f->slope = 0;
    if (!(f->y >= PLACE_Y + LZ_LANE_BACK && f->y <= PLACE_Y + LZ_LANE_FRONT)) f->y = PLACE_Y;
    walk_ground(t, f);
}

/* THE HAND's place (tank.h tank_hand_point): the keeper's hand rests on the ground - the point of the ground's walking
   lines nearest the finger (a rock beside it, the sand under it), its path returned */
static int hand_ground(const tank_t *t, float *x, float *y) {
    *x = t->hold_x; *y = t->hold_y;
    const terrain_t *tr = tank_terrain(t);
    int gs = 0; float bd = 1e30f;
    for (int s = 0; s < tr->n_paths; s++) {
        const terrain_path_t *p = &tr->path[s];
        const float gx = clampf(t->hold_x, p->x[0], p->x[p->n - 1]), gy = terrain_y(p, gx), d = tank_dist(gx, gy, t->hold_x, t->hold_y);
        if (d < bd) { bd = d; gs = s; *x = gx; *y = gy; }
    }
    return gs;
}
static void walk_hand_point(const tank_t *t, float *x, float *y) { hand_ground(t, x, y); }

/* ================================================================ THE WALK (pocket-terrarium P2b, P2c; tank.h has the
 * rules): the terrarium's lizards, on the ground (tank_terrain). Each frame a lizard's goal becomes a PLAN - a point of
 * the ground to walk to (a path and an x on it; or none: it stays where it stands), what its body does there (its
 * pose), the way it faces there, a hide to go into - and the walk carries it out: THE ROUTE's next leg (terrain_route:
 * along its own path to the point, or to the link where the way steps onto another path), a turn about on the spot when
 * the way lies behind it (a switchback's corner too), the stop-and-go along the paths, the hunt's freeze and lunge, a
 * hide's hidden spells and looks out. The crab's walk (crab_tick) was the template: planted feet, a pace that is the
 * pack's, stops where it pleases. */
#define LZ_LANE_PX_S     6.0f       /* a lane change: px/s across the floor's depth (it drifts over walking, never hops) */
#define LZ_FRONT_ODDS    0.6f       /* a walk takes the front lane this often (the critters' lanes' odds) */
#define LZ_FREEZE_LO_S   0.5f       /* the hunt's freeze before the lunge */
#define LZ_FREEZE_HI_S   1.1f
#define LZ_LUNGE_MAX_S   0.6f       /* a lunge lasts at most this */
#define LZ_MISS_REST_S   1.2f       /* after a miss: a beat before it stalks again */

#define LZ_HIDDEN_LO_S   8.0f       /* in a hide: hidden this long (x 1.4 - 0.8 bold: a bold one looks out sooner) ... */
#define LZ_HIDDEN_HI_S   25.0f
#define LZ_LOOK_LO_S     2.0f       /* ... then a look out this long */
#define LZ_LOOK_HI_S     5.0f
#define LZ_STRESS_SETTLE 0.18f      /* stress settles this a second (the fish's), twice that hidden or lying still */
#define LZ_NEW_WALK_PX   24.0f      /* a destination this far from the last (or on another path) is a new walk (its lane is rolled again) */
#define LZ_EXPLORE_STEP  6.0f       /* explore: the ground sampled every this many px along each path for a zone's points ... */
#define LZ_ZONE_IN_PX    16.0f      /* ... each this far inside its zone's edges (the glass is no edge) */
#define LZ_PLACE_NEAR_PX 40.0f      /* investigate alone: a place this near is the one it is at */
typedef struct {
    bool    go;                     /* walk to (surf, x) (false: it stays where it stands) */
    bool    hand;                   /* this plan answers the hand, not a goal or a dash through the same place */
    int     surf;                   /* ... on this path of the ground */
    float   x;
    float   lane;                   /* the floor depth it settles at once there (a walk keeps its own) */
    uint8_t pose;                   /* what it does once there, or standing */
    int     face;                   /* the way it faces once there: +1 right, -1 left, 0 as it came */
    float   pace, stops;            /* x its walking pace; x its stops' length (0: it walks straight there) */
    bool    flick;                  /* its stops are tongue flicks (explore, investigate) */
    int     enter;                  /* the hide it goes into once there (-1 none) */
    bool    stay_in;                /* in a hide, it stays in (the hide goal's own hide; rest) */
    int     prey;                   /* the hunt's prey (a cricket's slot, PREY_DISH the dish; -1 none) ... */
    bool    in_reach;               /* ... in its snout's striking reach: a freeze and a lunge (the dish: a look in and a dab) */
} walk_plan_t;

int tank_lizard_hide_for(const tank_t *t, int i) {
    const fish_t *f = &t->fish[i];
    const habitat_t *h = tank_habitat(t);
    if (!h->n_hides) return -1;
    const bool shed = f->shed == SHED_SOON || f->shed == SHED_SHEDDING || f->shed == SHED_STUCK;   /* `shed soon|yes` */
    const bool in_cool = f->hide >= 0 && f->hide < h->n_hides && h->hides[f->hide].kind == HIDE_COOL;
    const hide_kind_t want = shed ? HIDE_HUMID
                           : f->heat >= LZ_HIDE_COOL_AT || (in_cool && f->heat >= LZ_HIDE_COOL_TO) ? HIDE_COOL : HIDE_WARM;
    int best = -1, any = -1; float bd = 1e9f, ad = 1e9f;
    for (int k = 0; k < h->n_hides; k++) {                    /* the nearest of the kind; none of it, the nearest of any */
        float hx, hy; tank_hide_point(t, k, &hx, &hy);
        float d = fabsf(hx - f->x);
        if (h->hides[k].kind == want && d < bd) { bd = d; best = k; }
        if (d < ad) { ad = d; any = k; }
    }
    return best >= 0 ? best : any;
}
static void lizard_into(fish_t *f, int k) { f->hide = (int8_t)k; f->pose = LZ_POSE_HIDDEN; f->pose_s = 0; f->peek_s = 1.5f; f->speed = f->target_speed = 0; f->strike_s = 0; }
static void lizard_out(fish_t *f) { f->hide = -1; f->pose = LZ_POSE_STAND; f->pose_s = 0; f->peek_s = 0; }
bool tank_lizard_enter(tank_t *t, int i, int k) {
    if (i < 0 || i >= N_FISH_SLOTS || k < 0 || k >= tank_habitat(t)->n_hides) return false;
    fish_t *f = &t->fish[i];
    float hx, hy; tank_hide_point(t, k, &hx, &hy);
    f->x = hx; f->y = hy; f->lane = 0; f->turn_s = 0; f->surf = 0; f->slope = 0;   /* (a hide is on the floor) */
    lizard_into(f, k);
    return true;
}
void tank_lizard_leave(tank_t *t, int i) { if (i >= 0 && i < N_FISH_SLOTS && t->fish[i].hide >= 0) lizard_out(&t->fish[i]); }

/* THE RAP (tank.h THE WALK): lizard i dashes for the hide nearest by its route - it stays in the one it is in - and stays
   LZ_SPOOK_S; a strike under way is off */
static void lizard_spook(tank_t *t, int i) {
    fish_t *f = &t->fish[i];
    const terrain_t *tr = tank_terrain(t);
    const habitat_t *h = tank_habitat(t);
    int best = f->hide; float bc = 1e30f;
    for (int k = 0; f->hide < 0 && k < h->n_hides; k++) {      /* (out of every hide: the nearest by its route) */
        float hx, hy; terrain_hop_t hop; tank_hide_point(t, k, &hx, &hy);
        if (terrain_route(tr, f->surf, f->x, 0, hx, &hop) && hop.cost < bc) { bc = hop.cost; best = k; }
    }
    f->spook_s = LZ_SPOOK_S; f->spook_hide = (int8_t)best; f->hesitate = 0; f->pause_s = 0;
    if (f->pose == LZ_POSE_STALK || f->pose == LZ_POSE_LUNGE) { f->strike_s = 0; f->pose = LZ_POSE_STAND; }
}
/* THE HAND (tank.h THE WALK): a settled hold draws a lizard - trusting, not too hungry, awake, not overheating, not frightened */
static bool hand_draws(const tank_t *t, const fish_t *f) {
    return t->hold_time >= HOLD_ATTRACT_S && f->trust >= 4.0f && f->hunger < HOLD_HUNGER_VETO
        && tank_activity_phase(t) == PHASE_ACTIVE && f->heat < HEAT_HOT && f->spook_s <= 0;
}
/* ... to the hand's place on the ground (tank_hand_point: up a rock if that is nearest the finger), beside it: the first one
   LZ_HAND_GAP to its left, the next to its right, then further out - a side its ground has no room for skipped - facing it,
   flicking its tongue */
static int lizard_hand_target(const tank_t *t, int idx, float *target_x, float *hand_x) {
    const fish_t *f = &t->fish[idx];
    float gx, gy; const int gs = hand_ground(t, &gx, &gy);
    float x0, x1; walk_span(t, f, &tank_terrain(t)->path[gs], gs == 0 && f->surf == 0 ? f->y - PLACE_Y : 0, &x0, &x1);
    int rank = 0;
    for (int i = 0; i < idx; i++) rank += tank_swims(t, i) && hand_draws(t, &t->fish[i]);
    float x = clampf(gx - LZ_HAND_GAP, x0, x1);              /* (no slot with room: as near as its ground lets it) */
    for (int k = 0, n = 0; k < 2 * N_FISH_SLOTS; k++) {
        const float s = gx + (k / 2 + 1) * LZ_HAND_GAP * (k & 1 ? 1.0f : -1.0f);
        if (s < x0 || s > x1) continue;
        if (n++ == rank) { x = s; break; }
    }
    *target_x = x; if (hand_x) *hand_x = gx;
    return gs;
}
static void lizard_hand(tank_t *t, int idx, walk_plan_t *w) {
    float gx; w->surf = lizard_hand_target(t, idx, &w->x, &gx);
    const float x = w->x;
    w->go = w->hand = true; w->lane = 0;
    w->face = gx >= x ? 1 : -1; w->pace = 0.9f; w->stops = 0.8f; w->flick = true; w->pose = LZ_POSE_FLICK;
}
/* where its way to (to, tx) comes onto path `to` - its own x when it is on it already (the side it approaches from) */
static float lizard_entry(const terrain_t *tr, const fish_t *f, int to, float tx) {
    terrain_hop_t hop;
    return f->surf == to ? f->x : terrain_route(tr, f->surf, f->x, to, tx, &hop) ? hop.entry : tx;
}
/* investigate alone: the nearest place by the way there that it is not at - a hide's mouth or the basking spot */
static void lizard_place_near(const tank_t *t, fish_t *f) {
    const terrain_t *tr = tank_terrain(t);
    const habitat_t *h = tank_habitat(t);
    float bd = 1e30f; int bs = 0; float bx = tank_floor_x(0.5f);
    for (int k = -1; k < h->n_hides; k++) {
        int s = 0; float px, py;
        if (k < 0) { s = tr->bask_path; px = tr->bask_x; py = tr->bask_y; } else tank_hide_point(t, k, &px, &py);
        terrain_hop_t hop;
        if (tank_dist(px, py, f->x, f->y) <= LZ_PLACE_NEAR_PX || !terrain_route(tr, f->surf, f->x, s, px, &hop)) continue;
        if (hop.cost < bd) { bd = hop.cost; bs = s; bx = px; }
    }
    f->explore_surf = (int8_t)bs; f->explore_x = bx;
}
/* the zone (the schema's 3 x 2, zone_of) a point of the ground is well inside - LZ_ZONE_IN_PX off its edges - or -1 */
static int zone_in(float x, float y) {
    const float cw = TANK_W / 3.0f, half = TANK_BOT / 2.0f;
    const int z = zone_of(x, y), col = z % 3, row = z / 3;
    if ((col > 0 && x < col * cw + LZ_ZONE_IN_PX) || (col < 2 && x > (col + 1) * cw - LZ_ZONE_IN_PX)) return -1;
    if ((row == 0 && y > half - LZ_ZONE_IN_PX) || (row == 1 && y < half + LZ_ZONE_IN_PX)) return -1;
    return z;
}
/* explore's point: one of the ground's in the zone it has seen least lately (one of the two stalest, so two explorers
   don't take one line) - any of the schema's six that the ground reaches: the top row is up on the rocks */
static void lizard_explore_pick(tank_t *t, fish_t *f) {
    const terrain_t *tr = tank_terrain(t);
    int count[6] = { 0 };
    for (int s = 0; s < tr->n_paths; s++) {
        float x0, x1; walk_span(t, f, &tr->path[s], 0, &x0, &x1);
        for (float x = x0; x <= x1; x += LZ_EXPLORE_STEP) { const int z = zone_in(x, terrain_y(&tr->path[s], x)); if (z >= 0) count[z]++; }
    }
    const int cur = zone_of(f->x, f->y);
    int best = -1, second = -1;
    for (int z = 0; z < 6; z++) {
        if (z == cur || !count[z]) continue;
        if (best < 0 || f->zone_seen[z] < f->zone_seen[best]) { second = best; best = z; }
        else if (second < 0 || f->zone_seen[z] < f->zone_seen[second]) second = z;
    }
    f->explore_set = true; f->explore_surf = f->surf; f->explore_x = f->x; f->explore_y = f->y;   /* (no other zone: where it is) */
    if (best < 0) return;
    const int z = (second >= 0 && (xr(t) % 3) == 0) ? second : best;
    int k = (int)(xr(t) % (uint32_t)count[z]);
    for (int s = 0; s < tr->n_paths; s++) {
        float x0, x1; walk_span(t, f, &tr->path[s], 0, &x0, &x1);
        for (float x = x0; x <= x1; x += LZ_EXPLORE_STEP) {
            const float y = terrain_y(&tr->path[s], x);
            if (zone_in(x, y) == z && k-- == 0) { f->explore_surf = (int8_t)s; f->explore_x = x; f->explore_y = y; return; }
        }
    }
}

static void lizard_plan(tank_t *t, int idx, walk_plan_t *w) {
    fish_t *f = &t->fish[idx];
    const terrain_t *tr = tank_terrain(t);
    w->go = w->hand = false; w->surf = f->surf; w->x = f->x; w->lane = 0; w->pose = LZ_POSE_STAND; w->face = 0;
    w->pace = 1; w->stops = 1; w->flick = false; w->enter = -1; w->stay_in = false; w->prey = -1; w->in_reach = false;
    goal_id_t g = f->goal.id;
    const bool starving = f->hunger >= 8.0f && tank_food_sight(t, idx, NULL, NULL, NULL);
    /* the night: the sleep phase is for rest, or a hide (a starving lizard still hunts); and with the lamp out a bask's
       warmth is the warm hide's mat (the hide its need picks) - never a climb to a cold rock it would have to come down from */
    if (tank_activity_phase(t) == PHASE_SLEEP && g != GOAL_LZ_HIDE && !(g == GOAL_LZ_HUNT && starving)) g = GOAL_LZ_REST;
    if (t->night && g == GOAL_LZ_BASK) g = GOAL_LZ_HIDE;
    /* what outranks the goal, as with the fish: the spawning's pair to the nursery, the setup page's lizard to its
       spot, THE RAP's dash for cover, THE HAND - the advisor keeps the goal */
    if (t->spawning && (idx == t->court_a || idx == t->court_b) && !starving) {
        float cx, cy, rx; court_site(t, &cx, &cy, &rx);
        const float side = idx == t->court_a ? -1.0f : 1.0f;
        w->go = true; w->surf = 0; w->x = cx + side * 14; w->face = side < 0 ? 1 : -1;   /* side by side on the floor, facing each other */
        return;
    }
    if (idx == t->stage_fish) {
        w->go = true; w->surf = 0; w->x = t->stage_x; w->lane = LZ_LANE_FRONT;
        w->face = sinf(anim(t) * 0.7f + f->wander) > 0 ? 1 : -1;     /* it turns about now and then: both flanks show */
        return;
    }
    if (f->spook_s > 0) {                                     /* THE RAP: for cover at a dash, and in it while the fright lasts */
        const int k = f->spook_hide;
        if (f->hide >= 0 && (k < 0 || f->hide == k)) { w->stay_in = true; return; }
        if (k >= 0) { float hx, hy; tank_hide_point(t, k, &hx, &hy); w->go = true; w->surf = 0; w->x = hx; w->enter = k; w->pace = LZ_DASH_X; w->stops = 0; return; }
        w->go = true; w->x = f->x + (f->x >= t->startle_x ? 1 : -1) * 120; w->pace = LZ_DASH_X; w->stops = 0;   /* (no hide: away along its path) */
        return;
    }
    if (hand_draws(t, f)) { lizard_hand(t, idx, w); return; }
    switch (g) {
    case GOAL_LZ_HUNT: {
        const int pi = lizard_prey(t, f);
        if (pi < 0 && f->seen_surf >= 0 && f->seen_surf < tr->n_paths) {   /* its prey gone from sight (into cover): to where it was
                                                                               last seen, its snout over the spot (a cricket hidden there
                                                                               is flushed: CK_FLUSH_PX), tongue-flicking - once there, forgotten */
            const int ss = f->seen_surf;
            const float sdx = tank_lizard_half_len(t, f) * cosf(terrain_tilt(&tr->path[ss], f->seen_x, 1));
            int dir = f->seen_x >= lizard_entry(tr, f, ss, f->seen_x) ? 1 : -1;
            float sx;
            if (!strike_spot(t, f, ss, f->seen_x, sdx, dir, true, &sx)) { if (strike_spot(t, f, ss, f->seen_x, sdx, -dir, true, &sx)) dir = -dir; else sx = f->seen_x - dir * sdx; }
            if (f->surf == ss && fabsf(f->x - sx) <= 2 * LZ_ARRIVE_PX) f->seen_surf = -1;   /* (there: nothing - it looks elsewhere next) */
            w->go = true; w->surf = ss; w->x = sx; w->face = dir; w->pace = 0.8f; w->stops = 0.8f; w->flick = true; w->pose = LZ_POSE_FLICK;
            break;
        }
        if (pi < 0) {                                         /* nothing to hunt, nothing seen: a slow walk to where food came last */
            float sx = t->feed_spot_x >= 0 ? t->feed_spot_x : TANK_W * 0.5f;
            w->go = f->surf != 0 || fabsf(sx - f->x) > 40; w->surf = 0; w->x = sx; w->pace = 0.6f; w->stops = 1.5f; w->flick = true; w->pose = LZ_POSE_FLICK;
            break;
        }
        int ps; float px, py; prey_spot(t, pi, &ps, &px, &py);
        const bool dish = pi == PREY_DISH;
        const float half = tank_lizard_half_len(t, f);
        w->prey = pi; w->pose = dish ? LZ_POSE_STAND : LZ_POSE_STALK; w->stops = 0.6f;
        if (f->surf == ps) {                                  /* on its ground: in reach ahead of its snout - or the last stretch, a creep */
            const float ahead = (px - (f->x + f->facing * half * cosf(f->slope))) * f->facing;
            if (dish ? fabsf(ahead) <= LZ_DISH_REACH_PX : ahead >= -LZ_BITE_PX && ahead <= LZ_STRIKE_PX) { w->face = f->facing; w->in_reach = true; break; }
            if (!dish && fabsf(ahead) <= LZ_CREEP_FROM) w->pace = LZ_CREEP_X;
        }
        const float sdx = half * cosf(terrain_tilt(&tr->path[ps], px, 1));   /* (its snout's reach along that ground) */
        int dir = px >= lizard_entry(tr, f, ps, px) ? 1 : -1;  /* (from the side its way onto that ground reaches it; else the other) */
        float sx;
        if (!strike_spot(t, f, ps, px, sdx, dir, dish, &sx)) { if (strike_spot(t, f, ps, px, sdx, -dir, dish, &sx)) dir = -dir; else sx = px - dir * sdx; }
        w->go = true; w->surf = ps; w->x = sx; w->face = dir;
        break;
    }
    case GOAL_LZ_BASK:
        w->go = true; w->surf = tr->bask_path; w->x = tr->bask_x; w->pose = LZ_POSE_BASK; w->stops = 0.5f;
        break;
    case GOAL_LZ_HIDE: {
        const int k = tank_lizard_hide_for(t, idx);
        if (k < 0) { w->pose = LZ_POSE_REST; break; }         /* (a habitat without a hide: it lies low) */
        if (f->hide == k) { w->stay_in = true; break; }
        float hx, hy; tank_hide_point(t, k, &hx, &hy);
        w->go = true; w->surf = 0; w->x = hx; w->enter = k; w->stops = 0.5f;   /* (a hide is entered from the floor) */
        break;
    }
    case GOAL_LZ_EXPLORE:
        if (!f->explore_set || (f->surf == f->explore_surf && fabsf(f->x - f->explore_x) < LZ_ARRIVE_PX)) lizard_explore_pick(t, f);
        w->go = true; w->surf = f->explore_surf; w->x = f->explore_x; w->lane = f->lane; w->pace = 0.8f; w->stops = 1.5f; w->flick = true; w->pose = LZ_POSE_FLICK;
        break;
    case GOAL_LZ_INVESTIGATE: {
        float od; const int o = tank_nearest_friend(t, idx, &od);
        w->pace = 0.8f; w->stops = 1.2f; w->flick = true; w->pose = LZ_POSE_FLICK;
        if (o >= 0) {                                         /* the other lizard, on whatever ground: up to it, or - territorial - a stand-off */
            const fish_t *q = &t->fish[o];
            const int qs = q->surf >= 0 && q->surf < tr->n_paths ? q->surf : 0;
            const int dir = q->x >= lizard_entry(tr, f, qs, q->x) ? 1 : -1;
            const bool low = f->sociable < LZ_SOCIAL_LOW;
            const float want = low ? LZ_STANDOFF_PX : LZ_CONTACT_PX;
            w->face = dir; w->lane = f->lane;
            if (low) w->pose = LZ_POSE_STANDOFF;
            if (f->surf != qs || fabsf(q->x - f->x) > want + 10) { w->go = true; w->surf = qs; w->x = q->x - dir * want; }   /* (no nearer: it holds its ground) */
        } else {                                              /* nobody: a place it is not at - picked once, then it stays at it */
            if (!f->explore_set) { lizard_place_near(t, f); f->explore_set = true; }
            w->go = true; w->surf = f->explore_surf; w->x = f->explore_x;
        }
        break;
    }
    case GOAL_LZ_DIG:
        w->pose = LZ_POSE_DIG;                                /* at the sand where it stands (out of a hide first) - up on the */
        if (f->surf != 0) { w->go = true; w->surf = 0; w->x = lizard_entry(tr, f, 0, f->x); }   /* rocks, where its way down meets it */
        break;
    default:                                                  /* rest (and a fish goal, which no lizard holds): lie still, on any ground */
        w->pose = LZ_POSE_REST; w->stay_in = true;
        break;
    }
}

/* ---- (P3b DRAW-ONLY: THE LOOK ends) ---- */

static void update_lizard(tank_t *t, int idx, float dt) {
    fish_t *f = &t->fish[idx];
    const species_t *p = tank_species(t);
    const terrain_t *tr = tank_terrain(t);
    const int phase = tank_activity_phase(t);
    walk_ground(t, f);                                        /* (a newcomer, a staging, a wake: onto the ground first) */
    f->gdepth = f->gdepth_want = f->pdepth = f->pdepth_want = FISH_DEPTH_FRONT;   /* (render: in front of the grass and the pieces) */
    f->breather = 0; f->at_bubbles = false; f->door = DOOR_NONE;
    /* 1. its heat: what it feels - its hide's air, or the air where it stands, on the floor or up on a rock - at its
          pack's pace for its stage */
    body_follow(t, f, tank_animal_env_c(t, f, !t->night), dt);
    /* 2. its drives, its pack's rates (last frame's pose says what it was doing) */
    const bool still = f->hide >= 0 || f->pose == LZ_POSE_REST || f->pose == LZ_POSE_BASK;   /* lying still */
    const bool busy = f->speed > 0.5f || f->pose == LZ_POSE_DIG;
    const float cold = f->heat < HEAT_SLOW ? HEAT_SLOW_MIN + (1 - HEAT_SLOW_MIN) * f->heat / HEAT_SLOW : 1.0f;
    f->hunger = clampf(f->hunger + dt * p->hunger_per_s * (f->shed == SHED_SOON || f->shed == SHED_SHEDDING ? SHED_HUNGER_X : 1.0f), 0, 10);
    f->energy = clampf(f->energy + dt * (still ? p->energy_rest_per_s * cold                     /* cold slows its recovery */
                                       : busy ? -p->energy_per_s * (f->pose == LZ_POSE_DIG ? 1.5f : 1.0f) : 0), 0, 10);
    const bool looking = f->goal.id == GOAL_LZ_EXPLORE || f->goal.id == GOAL_LZ_INVESTIGATE;
    f->curiosity = clampf(f->curiosity + dt * p->curiosity_per_s * (looking ? -LZ_CURIOSITY_SPEND_X : 1.0f), 0, 10);
    f->stress = clampf(f->stress - dt * LZ_STRESS_SETTLE * (still ? 2.0f : 1.0f), 0, 10);
    if (f->heat >= HEAT_HOT) {                                /* overheating pulls stress up (tank.h HEAT_HOT) */
        float to = 4 + 2 * (f->heat - HEAT_HOT);
        if (f->stress < to) f->stress = clampf(f->stress + HEAT_HOT_PULL * (to - f->stress) * dt, 0, 10);
    }
    f->stress = fmaxf(f->stress, stuck_floor(f));             /* a stuck shed's floor (docs/shed.md) */
    /* 3. boredom: the fish's numbers (BORED_*), its own pastimes - a need met (food, the hide a shed or the heat
       wants, warming up) never bores, nor does the night */
    {
        const goal_id_t g = f->goal.id;
        const bool need_hide = f->shed != SHED_NORMAL || f->heat >= LZ_HIDE_COOL_AT;
        const bool leisure = phase == PHASE_ACTIVE && (g == GOAL_LZ_EXPLORE || g == GOAL_LZ_INVESTIGATE || g == GOAL_LZ_DIG || g == GOAL_LZ_REST
                             || (g == GOAL_LZ_BASK && f->heat >= 5) || (g == GOAL_LZ_HIDE && !need_hide));
        f->bored = clampf(f->bored + dt * (leisure ? BORED_PER_S : -BORED_RELIEF_PER_S), 0, 10);
        int z = zone_of(f->x, f->y);
        if (z != f->zone_last) {
            if (f->zone_last >= 0 && t->clock - f->zone_seen[z] > BORED_ZONE_STALE_S) f->bored = clampf(f->bored - BORED_NEW_ZONE, 0, 10);
            f->zone_last = (int8_t)z;
        }
        f->zone_seen[z] = t->clock;
    }
    f->wander += dt * (0.65f + f->curiosity * 0.04f);
    if (f->wander >= 62.831853f) f->wander = fmodf(f->wander, 62.831853f);
    f->goal_age += dt; f->ask_age += dt;

    /* 4. the plan, and a moment's hesitation over a torn decision (it stands; in a hide it stays in) - never in the rap's dash */
    const int from_surf = f->surf;
    walk_plan_t w; lizard_plan(t, idx, &w);
    if (f->hesitate > 0) {
        f->hesitate -= dt;
        if (f->spook_s <= 0) { w.go = false; w.pose = LZ_POSE_STAND; w.face = 0; w.enter = -1; w.stay_in = true; w.prey = -1; w.in_reach = false; }
    }
    if (f->spook_s > 0) f->spook_s = fmaxf(0, f->spook_s - dt * (f->hide >= 0 ? 1.0f : 0.25f));   /* THE RAP: the fright runs out in its cover */
    f->to_hide = (int8_t)w.enter;                             /* (render's stack: the hide this walk goes into) */
    /* 5. a hide: in it while the plan keeps it there, else out at its mouth */
    if (f->hide >= 0 && !(w.stay_in || w.enter == f->hide)) lizard_out(f);
    /* 6. THE ROUTE: the point onto its path (as near as the glass lets it), and the way there - the next leg along its own
          path: to the point, or to the link where the way steps onto another path. At a link it steps over, the same
          point, and walks the next leg (off the floor only from the middle lane: its lane eases to 0 on the way there, and
          it waits for it) */
    float wx = f->x; int via = -1;
    if (w.go) {
        if (w.surf < 0 || w.surf >= tr->n_paths) w.surf = 0;
        float x0, x1; walk_span(t, f, &tr->path[w.surf], w.surf != 0 ? 0 : f->surf == 0 ? f->y - PLACE_Y : w.lane, &x0, &x1);
        w.x = clampf(w.x, x0, x1);
        for (int k = 0; k < 4; k++) {
            terrain_hop_t hop;
            if (!terrain_route(tr, f->surf, f->x, w.surf, w.x, &hop)) { w.go = false; break; }   /* (no way there: it stays) */
            wx = hop.x; via = hop.via;
            if (via < 0 || fabsf(wx - f->x) > 0.01f || (f->surf == 0 && fabsf(f->y - PLACE_Y) > 0.01f)) break;
            f->surf = (int8_t)via; f->x = wx; f->y = terrain_y(&tr->path[via], wx);   /* over onto the next path, the same point (down on the
                                                                                         floor its lane drifts back in, walking) */
        }
    }
    const terrain_path_t *path = &tr->path[f->surf];

    /* 7. the walk */
    const float climb = terrain_tilt(path, f->x, f->facing) * f->facing;   /* + uphill the way it faces */
    float pace = p->walk_px_s[f->stage & 3] * (0.8f + f->goal.urgency * 0.05f) * w.pace;   /* the urgency's 0.8..1.25 */
    pace *= cold;                                             /* cold: sluggish (x0.5 at heat 0) */
    if (f->energy < 1.2f) pace *= 0.45f;                      /* spent */
    pace *= terrain_pace(climb);                              /* uphill slower, downhill a little careful */
    float speed = 0, dx = 0;
    uint8_t pose = w.pose;
    const float sdx = tank_lizard_half_len(t, f) * cosf(f->slope);   /* its snout: the drawn body's half length ahead of its middle */
    int ps = -1; float px = 0, py = 0;
    const bool prey = w.prey >= 0 && prey_spot(t, w.prey, &ps, &px, &py) && ps == f->surf;   /* its prey, on its ground */
    const bool dish = w.prey == PREY_DISH;
    if (f->hide >= 0) {                                       /* in a hide: hidden, looking out now and then */
        pose = f->pose == LZ_POSE_PEEK ? LZ_POSE_PEEK : LZ_POSE_HIDDEN;
        if ((f->peek_s -= dt) <= 0) {
            if (pose == LZ_POSE_HIDDEN) { pose = LZ_POSE_PEEK; f->peek_s = tank_randf(t, LZ_LOOK_LO_S, LZ_LOOK_HI_S); }
            else { pose = LZ_POSE_HIDDEN; f->peek_s = tank_randf(t, LZ_HIDDEN_LO_S, LZ_HIDDEN_HI_S) * (1.4f - 0.8f * clampf(f->bold, 0, 1)) * (phase == PHASE_SLEEP ? 3 : 1); }
        }
    } else if (f->turn_s > 0) {                               /* turning about on the spot: it stands while its yaw swings */
        f->turn_s -= dt; pose = LZ_POSE_TURN;
    } else if (f->pose == LZ_POSE_LUNGE && f->strike_s > 0 && prey) {   /* the lunge: at the prey, flat out (the dish: a dab) */
        f->strike_s -= dt; pose = LZ_POSE_LUNGE;
        if (dish) {
            if (f->strike_s <= 0) { lizard_worm(t, idx); pose = LZ_POSE_STAND; f->pause_s = DISH_CHEW_S; }   /* a worm, every time; then it chews */
        } else {
            cricket_t *c = &t->cricket[w.prey];
            const float lift = ck_ground(path, c->x) - c->y;  /* (mid-hop: off the ground) */
            dx = c->x - (f->x + f->facing * sdx);
            if (fabsf(dx) <= LZ_BITE_PX + 3 * f->size && lift <= CK_REACH_DY) {   /* the strike: caught, or missed */
                if (tank_randf(t, 0, 1) >= lizard_miss_odds(f, c)) lizard_catch(t, idx, w.prey);
                else lizard_missed(t, c, f->x + f->facing * sdx);
                pose = LZ_POSE_STAND; f->strike_s = 0; f->pause_s = LZ_MISS_REST_S;   /* (caught or missed: a beat) */
            } else if (dx * f->facing > 0 && f->strike_s > 0) speed = pace * LZ_LUNGE_X;
            else { lizard_missed(t, c, f->x + f->facing * sdx); pose = LZ_POSE_STAND; f->strike_s = 0; f->pause_s = LZ_MISS_REST_S; }   /* past it, or out of time: a miss */
        }
    } else {
        dx = w.go ? wx - f->x : 0;
        const bool there = !w.go || (via < 0 && fabsf(dx) <= LZ_ARRIVE_PX);
        const bool waiting = w.go && via >= 0 && fabsf(dx) <= 0.01f;   /* at a ramp's foot, its lane still easing to the middle */
        const int face = there ? (w.face ? w.face : f->facing) : waiting ? f->facing : dx > 0 ? 1 : -1;
        if (face != f->facing) {                              /* the way lies behind it: it turns about where it stands */
            walk_face(f, face); f->turn_s = p->turn_s; pose = LZ_POSE_TURN; f->walk_s = 0; f->strike_s = 0;
        } else if (waiting) {
            pose = LZ_POSE_STAND; f->strike_s = 0;
        } else if (!there) {                                  /* walking: a bout, a stop, a bout ... */
            if (fabsf(w.x - f->to_x) > LZ_NEW_WALK_PX || w.surf != f->to_surf) {   /* a new walk: its lane */
                f->to_x = w.x; f->to_surf = (int8_t)w.surf;
                f->lane = tank_randf(t, 0, 1) < LZ_FRONT_ODDS ? LZ_LANE_FRONT : LZ_LANE_BACK;
            }
            if (f->pause_s > 0) {
                f->pause_s -= dt; pose = w.flick ? LZ_POSE_FLICK : LZ_POSE_STAND;
                if (f->pause_s <= 0) f->walk_s = tank_randf(t, p->bout_s[0], p->bout_s[1]);
            } else {
                if (f->walk_s <= 0) f->walk_s = tank_randf(t, p->bout_s[0], p->bout_s[1]);
                speed = pace; pose = LZ_POSE_WALK;
                if ((f->walk_s -= dt) <= 0 && w.stops > 0) f->pause_s = tank_randf(t, p->pause_s[0], p->pause_s[1]) * w.stops;
            }
            f->strike_s = 0;
        } else {                                              /* there: what the goal does here */
            f->walk_s = 0;
            if (f->pause_s > 0) { f->pause_s -= dt; pose = LZ_POSE_STAND; }   /* (a beat after a strike; a worm's chewing) */
            else if (w.enter >= 0 && f->surf == 0) { lizard_into(f, w.enter); pose = LZ_POSE_HIDDEN; }
            else if (w.in_reach && prey) {                    /* the strike: a freeze, then the lunge (the dish: a look in, then a dab) */
                if (f->pose != LZ_POSE_STALK || f->strike_s <= 0) {
                    if (f->pose == LZ_POSE_STALK) { pose = LZ_POSE_LUNGE; f->strike_s = dish ? DISH_DAB_S : LZ_LUNGE_MAX_S; }   /* the freeze is over */
                    else { pose = LZ_POSE_STALK; f->strike_s = dish ? tank_randf(t, DISH_LOOK_LO_S, DISH_LOOK_HI_S) : tank_randf(t, LZ_FREEZE_LO_S, LZ_FREEZE_HI_S); }
                } else { f->strike_s -= dt; pose = LZ_POSE_STALK; if (f->strike_s <= 0) { pose = LZ_POSE_LUNGE; f->strike_s = dish ? DISH_DAB_S : LZ_LUNGE_MAX_S; } }
            }
        }
    }
    /* 8. the step: along its path in its facing - its pace along the slope, so a ramp's length is walked - never past the
       leg's point (the lunge runs on to the prey); its middle on the path's line, the floor's lane drifting - to the
       middle on its way to a ramp's foot - but not while it turns about: it stands, its y too (on a narrowing floor - the
       bowl's, the watch's corners - a drift across the depth moves the glass's edge, and the footing would push it along) */
    const float step_x = f->x;
    if (speed > 0) {
        float step = speed * dt * cosf(terrain_tilt(path, f->x, f->facing));
        if (pose != LZ_POSE_LUNGE && fabsf(dx) < step) step = fabsf(dx);
        f->x += f->facing * step;
    }
    {
        float x0, x1; walk_span(t, f, path, f->surf == 0 ? f->y - PLACE_Y : 0, &x0, &x1);
        f->x = clampf(f->x, x0, x1);
        const float line = terrain_y(path, f->x);
        if (f->surf != 0) f->y = line;
        else if (pose != LZ_POSE_TURN) {
            const float lane = f->hide >= 0 || (w.go && via >= 0) ? 0 : speed > 0 ? f->lane : w.lane;
            f->y += clampf(line + lane - f->y, -LZ_LANE_PX_S * dt, LZ_LANE_PX_S * dt);
        }
    }
    walk_ground(t, f);
    /* The aquarium's 140 -> 100 px ring test missed ordinary lizard approaches from nearby. Here the walk itself is
       the evidence: it must really step while answering the hand, then reach its own reachable slot on the right path.
       Already standing there, a cursor moved over it, a dash, a hunt or a point below the hand's rock earns nothing. */
    if (w.hand && w.go) {
        const bool stepped = speed > 0 && f->x != step_x;
        if (stepped) f->hold_far = true;
        if (f->hold_far && (stepped || f->surf != from_surf) && f->hide < 0 &&
            f->surf == w.surf && fabsf(f->x - w.x) <= LZ_ARRIVE_PX) hold_approach(t, idx);
    } else f->hold_far = false;
    f->slope += clampf(terrain_tilt(path, f->x, f->facing) - f->slope, -LZ_SLOPE_RATE * dt, LZ_SLOPE_RATE * dt);   /* its body takes the ground's slope */
    f->speed = speed; f->target_speed = speed;
    f->yaw = f->turn_s > 0 ? f->facing * (1 - 2 * clampf(f->turn_s / p->turn_s, 0, 1)) : f->facing;   /* a turn swings it through head-on */
    if (pose != f->pose) { f->pose = pose; f->pose_s = 0; } else f->pose_s += dt;
    tank_fish_trail(f, dt);                                   /* (render's body and tail follow, as a fish's) */
    fish_beat(f, dt);
    lizard_look(t, f, dt);                                    /* P3b DRAW-ONLY: the phase, the blink, the tongue, the fat (THE LOOK) */
}

/* ---- THE WALK as a movement module (locomotion.h): what tank.c asks of an animal that walks ---- */
/* it has come to the keeper's resting hand: awake, trusting, out of its hide, standing at its own slot beside it on that ground
   (not merely near it through a rock) */
static bool walk_at_finger(const tank_t *t, int i) {
    const fish_t *f = &t->fish[i];
    if (!tank_swims(t, i) || !hand_draws(t, f) || f->hide >= 0) return false;
    float x; const int surf = lizard_hand_target(t, i, &x, NULL);
    return f->surf == surf && fabsf(f->x - x) <= LZ_ARRIVE_PX;
}
/* a tap at (px, py) on a lizard as if its middle were at (x, y): its body's line nose to tail tip, as drawn */
static float walk_tap_d2(const tank_t *t, int i, float x, float y, float px, float py) {
    const fish_t *f = &t->fish[i];
    const float half = tank_lizard_half_len(t, f), ux = cosf(f->slope), uy = -(f->facing < 0 ? -1.0f : 1.0f) * sinf(f->slope);
    const float u = clampf((px - x) * ux + (py - y) * uy, -half, half), qx = x + ux * u - px, qy = y + uy * u - py;
    return qx * qx + qy * qy;
}
/* a lizard is asked again as its heat crosses a band (cold, cool, comfortable, warm, hot), a shed comes or goes, the light's phase
   turns (P2b: the L1 line's heat, shed and phase) */
static uint32_t walk_signature(const tank_t *t, int idx) {
    const fish_t *f = &t->fish[idx];
    const int hb = f->heat < 3 ? 0 : f->heat < 5 ? 1 : f->heat < 7 ? 2 : f->heat < HEAT_HOT ? 3 : 4;
    const int sh = f->shed == SHED_SOON ? 1 : f->shed == SHED_SHEDDING || f->shed == SHED_STUCK ? 2 : 0;
    return (uint32_t)hb << 16 | (uint32_t)sh << 19 | (uint32_t)tank_activity_phase(t) << 21;
}
/* (no hold_credit: a lizard earns the hold-approach badge by really walking under the hand's draw - update_lizard - and no goal
   set: it walks the floor's lanes, no depths) */
const loco_ops_t loco_walk = {
    .name = "walk", .mode = CAP_WALK,
    .update = update_lizard, .seat = walk_floor, .hand_point = walk_hand_point, .at_finger = walk_at_finger,
    .tap_d2 = walk_tap_d2, .spook = lizard_spook, .signature = walk_signature,
};
