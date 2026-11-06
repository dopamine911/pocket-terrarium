/* aq_seahorse.c - the seahorse (SD_ITEM_SEAHORSE)
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

/* ---- the seahorse (0.3.6, SD_ITEM_SEAHORSE; tank.h has the story) ----
 * Three things it does. PERCH: its tail round an ANCHOR (tank.h: a blade, a
 * post on a decoration, or the sand) for a while, then it lets go and DRIFTs
 * to another at SH_DRIFT_PX_S - upright, slow, its dorsal fin doing the work.
 * A finger resting on the glass (the settled hold the fish answer,
 * HOLD_ATTRACT_S) makes it let go at once and swim to the finger's edge at
 * SH_SWIM_PX_S (the one hurry it has); within SH_AT_PX of the spot it is
 * HITCHED - a hitch counts once per hold, and every fish earns trust at
 * SEAHORSE_TRUST_X the base rate until the finger lifts. It lingers
 * SH_LINGER_S where the finger was, then drifts home. An anchor that goes
 * (a blade cut under SH_MIN_H, a piece moved or sold) drops it; it looks
 * again after a moment. In the dark it stays where it is. */
#define SH_DRIFT_PX_S   18.0f
#define SH_SWIM_PX_S    42.0f
#define SH_AT_PX        2.5f
#define SH_STAY_MIN     45.0f     /* on a blade (Strato 10-15: "move from spot to spot, blade to blade") */
#define SH_STAY_MAX     140.0f
#define SH_POST_STAY_MIN 60.0f    /* on a decoration */
#define SH_POST_STAY_MAX 180.0f
#define SH_SAND_STAY_MIN 50.0f    /* resting on the bottom, before it lifts off to look again */
#define SH_SAND_STAY_MAX 110.0f
#define SH_LINGER_S     3.0f
/* the water (Strato 10-15: "it moves to a new location and stops abruptly
 * with no easing ... it should continue to slightly drift the direction it
 * was moving due to momentum"): it swims by velocity. The fin's thrust pulls
 * the velocity toward what it wants (a cruise toward the spot, easing off
 * inside SH_EASE_PX of it) with a lag of ~SH_LAG_S, so it builds speed,
 * coasts past a stop and settles. */
#define SH_LAG_S        0.55f
#define SH_EASE_PX      34.0f
#define SH_TAP_RADIUS   26.0f
#define SH_SAND_Y       (TANK_BOT - 21.0f)   /* the coil's centre resting on the floor */
#define SH_GRIP_SWORD   0.72f                /* a sword leaf is gripped higher, where it is narrow */
typedef struct { uint8_t kind, front; int8_t bed, frond, item; float x, y, hw; } sh_cand_t;
/* a frond it may hold: any frond tall enough. Its LANE comes from where the
 * frond is drawn: a grass bed is woven (even fronds behind the fish, odd in
 * front), the sword plant by its depth (BEHIND: all behind; AMONG: woven;
 * IN FRONT: all in front) */
static bool sh_frond_ok(const tank_t *t, int b, int i) {
    if (b < 0 || b >= tank_veg_beds(t)) return false;
    float bx0; int n; veg_bed_base(t, b, &bx0, &n);
    return i >= 0 && i < n && t->veg_h[b][i] >= SH_MIN_H;
}

static bool sh_frond_front(const tank_t *t, int b, int i) { (void)i; return tank_bed_z(t, b) == DECOR_Z_FRONT; }
/* the coil's spot on a blade: the spine's x (no sway: the renderer adds it)
 * and part of the way up the blade from the root line */
static void sh_grip(const tank_t *t, int b, int i, float *x, float *y) {
    int segs = tank_veg_frond(t, b, i, x);
    float frac = tank_veg_kind(t, b) == VEG_KIND_SWORD ? SH_GRIP_SWORD : SH_GRIP_FRAC;
    *y = TANK_BOT - 16 - segs * frac * VEG_SEG_PX;
}
/* the POSTS: solid parts of the decorations, at any depth - a piece IN FRONT
 * puts it in its front lane. Offsets from each piece's centre on its floor
 * line, hand-placed on the geometry in render.c. */
static int sh_posts(const tank_t *t, sh_cand_t *out, int max) {
    int n = 0;
    #define POST(it, fr, px, py, phw) do { if (n < max) { out[n].kind = SH_ANCHOR_POST; out[n].front = (fr); out[n].bed = out[n].frond = -1; out[n].item = (it); out[n].x = (px); out[n].y = (py); out[n].hw = (phw); n++; } } while (0)
    if (tank_decor_in(t, 2)) {                                                           /* the towers (castle_front_row / draw_castle): the coil round
                                                                                          * each CONE just under its peak (Strato 10-15: on the tower's
                                                                                          * body it blended with the stone - "curled around the peak"),
                                                                                          * so the body stands over the roofline in open water; the
                                                                                          * crenellated tower at its merlons */
        float cx = tank_decor_x(t, 2), fy = tank_decor_fy(t, 2); bool fr = tank_decor_z(t, 2) == DECOR_Z_FRONT;   /* fy: its depth's floor line (0.3.9) */
        POST(2, fr, cx + 39, fy - 128, 5);                                               /* the rear tower's cone (peak fy - 142): behind the wall at
                                                                                          * EITHER depth - in the castle's lane, its front row put back over
                                                                                          * it (THE PIECE'S STACK, sh_orow; 0.3.9.6 kept it in the back
                                                                                          * lane, under every piece IN FRONT) */
        POST(2, fr, cx - 48, fy - 96, 5);                                                /* the left tower's cone (peak fy - 108) */
        POST(2, fr, cx - 73, fy - 60, 5);                                                /* the far-left cone (peak fy - 72) */
        POST(2, fr, cx + 71, fy - 64, 10);                                               /* the far-right tower: its merlons (top fy - 70) */
    }
    if (tank_decor_in(t, 3)) {                                                           /* the trunk (CORAL_SEGS, 2 px cells, y scale 0.88) */
        float cx = tank_decor_x(t, 3), fy = tank_decor_fy(t, 3) + 6, g = tank_coral_growth(t); bool fr = tank_decor_z(t, 3) != DECOR_Z_BACK;   /* (+6: the sprite's bottom sits 6 px under the line) */
        POST(3, fr, cx + 1, fy - 18, 6);
        if (g >= 0.6f) POST(3, fr, cx + 3, fy - 47, 5);
    }
    if (tank_decor_in(t, 4)) {                                                           /* the tube sponges (CL_TUBES, 2 px cells about x 36) */
        float cx = tank_decor_x(t, 4), fy = tank_decor_fy(t, 4) + 6; bool fr = tank_decor_z(t, 4) != DECOR_Z_BACK;
        float g = t->cluster_growth <= 0 ? 1 : t->cluster_growth > 1 ? 1 : t->cluster_growth;
        float sz = CLUSTER_SIZE_MIN + (1.0f - CLUSTER_SIZE_MIN) * g;
        POST(4, fr, cx + 33 * sz, fy - 40 * sz, 11 * sz); POST(4, fr, cx + 52 * sz, fy - 32 * sz, 10 * sz); POST(4, fr, cx + 17 * sz, fy - 27 * sz, 9 * sz);
    }
    if (tank_decor_in(t, 9)) {                                                           /* the volcano (0.3.9; render.c vo_build, 2 px cells from its
                                                                                          * floor line): the far rim of the crater - it perches over the
                                                                                          * lava, the bubbles climbing past it -, the left shoulder's ledge
                                                                                          * (the body in open water beside the flank), and the crown of the
                                                                                          * cave's arch (the body against the dark mouth) */
        float cx = tank_decor_x(t, 9), fy = tank_decor_fy(t, 9); bool fr = tank_decor_z(t, 9) == DECOR_Z_FRONT;
        POST(9, fr, cx + 8, fy - 196, 6);                                                /* the far rim (the cone's top row fy - 200) */
        POST(9, fr, cx - 78, fy - 66, 6);                                                /* the left shoulder */
        POST(9, fr, cx, fy - 58, 8);                                                     /* the cave's crown (the vault's top fy - 58) */
    }
    #undef POST
    return n;
}
/* is what it holds still there? (a piece or plant the keeper moved to another
 * depth under it keeps it: it goes with it to that lane) */
static bool sh_hold_ok(tank_t *t) {
    if (t->sh_akind == SH_ANCHOR_BLADE) {
        if (!sh_frond_ok(t, t->sh_bed, t->sh_frond)) return false;
        t->sh_front_want = sh_frond_front(t, t->sh_bed, t->sh_frond);   /* (the keeper moved its plant: coiled on it, it goes with it at once;
                                                                       on its way, as THE LANE allows - seahorse_depth) */
        if (t->sh_mode == SH_PERCH) t->sh_front = t->sh_front_want;
        return true;
    }
    if (t->sh_akind == SH_ANCHOR_POST) {
        sh_cand_t c[12]; int n = sh_posts(t, c, 12);
        for (int i = 0; i < n; i++)
            if (c[i].item == t->sh_aitem && fabsf(c[i].x - t->sh_ax) < 0.01f && fabsf(c[i].y - t->sh_ay) < 0.01f) { t->sh_front_want = c[i].front != 0; if (t->sh_mode == SH_PERCH) t->sh_front = t->sh_front_want; return true; }
        return false;
    }
    return t->sh_akind == SH_ANCHOR_SAND;
}
static void sh_take(tank_t *t, const sh_cand_t *c) {
    t->sh_akind = c->kind; t->sh_bed = c->bed; t->sh_frond = c->frond; t->sh_aitem = c->item;
    t->sh_ax = c->x; t->sh_ay = c->y; t->sh_ahw = c->hw;
    t->sh_aref = c->kind == SH_ANCHOR_POST ? tank_decor_x(t, c->item) : 0;
    t->sh_front_want = c->front != 0;                                      /* its lane for this hold: taken up clear of the pieces, or as it
                                                                              settles (seahorse_depth) - not mid-hold, never through a piece */
    t->sh_tx = c->x; t->sh_ty = c->y;
    t->sh_gdepth_want = c->kind == SH_ANCHOR_BLADE ? FISH_DEPTH_THROUGH : FISH_DEPTH_FRONT;   /* SWIM DEPTH: coiled on a blade, through the grass */
}
/* an anchor to hold, from here: the NEAREST wins, a blade before a post (and
 * a taller blade a little sooner), with a little chance in it, so it hops
 * blade to blade through a bed and crosses to the next now and then (Strato
 * 10-15: after a cut "it shouldn't be out in open water for very long").
 * The sand only when there is nothing else. Never the one it is on. */
static bool sh_pick(tank_t *t, float from_x, float from_y) {
    sh_cand_t best = { 0, 0, -1, -1, -1, 0, 0, 0 }; bool have = false; float bs = 0;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        float bx0; int n; veg_bed_base(t, b, &bx0, &n);
        for (int i = 0; i < n; i++) {
            if (!sh_frond_ok(t, b, i) || (t->sh_akind == SH_ANCHOR_BLADE && b == t->sh_bed && i == t->sh_frond)) continue;
            sh_cand_t c = { SH_ANCHOR_BLADE, sh_frond_front(t, b, i), (int8_t)b, (int8_t)i, -1, 0, 0, 1.5f };
            sh_grip(t, b, i, &c.x, &c.y);
            float score = -tank_dist(c.x, c.y, from_x, from_y) + 60 + (t->veg_h[b][i] - SH_MIN_H) * 90 + tank_randf(t, 0, 30) - (c.front != t->sh_front ? 15 : 0);
            if (!have || score > bs) { bs = score; best = c; have = true; }
        }
    }
    sh_cand_t posts[12]; int np = sh_posts(t, posts, 12);
    for (int i = 0; i < np; i++) {
        if (t->sh_akind == SH_ANCHOR_POST && fabsf(posts[i].x - t->sh_ax) < 0.01f && fabsf(posts[i].y - t->sh_ay) < 0.01f) continue;
        float score = -tank_dist(posts[i].x, posts[i].y, from_x, from_y) + tank_randf(t, 0, 30) - (posts[i].front != t->sh_front ? 15 : 0);   /* a lane change a little later */
        if (!have || score > bs) { bs = score; best = posts[i]; have = true; }
    }
    if (!have) {                                                           /* nothing: a spot on the sand near here */
        best.kind = SH_ANCHOR_SAND; best.front = t->sh_front; best.bed = best.frond = -1; best.item = -1;   /* the sand in the lane it is in */
        best.x = clampf(from_x + tank_randf(t, -60, 60), TANK_FX0 + 40, TANK_FX1 - 40); best.y = SH_SAND_Y; best.hw = 0;
    }
    sh_take(t, &best);
    return true;
}
/* anything to hold at all (a blade tall enough, a post on a piece behind)? */
static bool sh_any_hold(const tank_t *t) {
    sh_cand_t c[12];
    if (sh_posts(t, c, 12) > 0) return true;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        float bx0; int n; veg_bed_base(t, b, &bx0, &n);
        for (int i = 0; i < n; i++) if (sh_frond_ok(t, b, i)) return true;
    }
    return false;
}
/* a hold other than the one it has? (its stay over, with nowhere else to go
 * it keeps the one it has rather than dropping to the sand and climbing back) */
static bool sh_another_hold(const tank_t *t) {
    sh_cand_t c[12]; int n = sh_posts(t, c, 12);
    for (int i = 0; i < n; i++)
        if (!(t->sh_akind == SH_ANCHOR_POST && fabsf(c[i].x - t->sh_ax) < 0.01f && fabsf(c[i].y - t->sh_ay) < 0.01f)) return true;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        float bx0; int nn; veg_bed_base(t, b, &bx0, &nn);
        for (int i = 0; i < nn; i++)
            if (sh_frond_ok(t, b, i) && !(t->sh_akind == SH_ANCHOR_BLADE && b == t->sh_bed && i == t->sh_frond)) return true;
    }
    return false;
}
static float sh_stay_for(tank_t *t) {
    return t->sh_akind == SH_ANCHOR_BLADE ? tank_randf(t, SH_STAY_MIN, SH_STAY_MAX)
         : t->sh_akind == SH_ANCHOR_POST  ? tank_randf(t, SH_POST_STAY_MIN, SH_POST_STAY_MAX)
         : tank_randf(t, SH_SAND_STAY_MIN, SH_SAND_STAY_MAX);
}
void tank_seahorse_place(tank_t *t) {
    float x0, x1; tank_veg_bed(t, 0, &x0, &x1, NULL, NULL);
    t->sh_x = clampf(x1 + 30, TANK_FX0 + 30, TANK_FX1 - 30); t->sh_y = TANK_BOT - 110;
    t->sh_akind = SH_ANCHOR_NONE; t->sh_bed = t->sh_frond = -1; t->sh_aitem = -1; t->sh_mode = SH_DRIFT; t->sh_facing = 1;
    t->sh_hitched = t->sh_credited = false; t->sh_stay = 0; t->sh_vx = t->sh_vy = 0;
    t->sh_pose = SH_POSE_BLADE; t->sh_land = 0;
    sh_pick(t, t->sh_x, t->sh_y);
    t->sh_x = t->sh_tx; t->sh_y = t->sh_ty; t->sh_mode = SH_PERCH; t->sh_stay = sh_stay_for(t); t->sh_land = SH_LAND_S;
    t->sh_front = t->sh_front_want;                                /* (set down on its hold: its lane at once) */
    t->sh_obed = t->sh_akind == SH_ANCHOR_BLADE ? t->sh_bed : -1;   /* (placed on it: in its stack from the start) */
}
bool tank_seahorse_hold(tank_t *t, int b, int i) {
    if (!sh_frond_ok(t, b, i)) return false;
    sh_cand_t c = { SH_ANCHOR_BLADE, sh_frond_front(t, b, i), (int8_t)b, (int8_t)i, -1, 0, 0, 1.5f };
    sh_grip(t, b, i, &c.x, &c.y);
    sh_take(t, &c);
    t->sh_x = c.x; t->sh_y = c.y; t->sh_vx = t->sh_vy = 0; t->sh_hitched = false;
    t->sh_mode = SH_PERCH; t->sh_stay = sh_stay_for(t); t->sh_pose = SH_POSE_BLADE; t->sh_land = SH_LAND_S;
    t->sh_gdepth = t->sh_gdepth_want; t->sh_obed = (int8_t)b; t->sh_front = t->sh_front_want;
    return true;
}
bool tank_seahorse_post(tank_t *t, int item, int n) {
    sh_cand_t c[12]; int m = sh_posts(t, c, 12), k = -1;
    for (int i = 0; i < m; i++) if (c[i].item == item && n-- == 0) { k = i; break; }
    if (k < 0) return false;
    sh_take(t, &c[k]);
    t->sh_x = c[k].x; t->sh_y = c[k].y; t->sh_vx = t->sh_vy = 0; t->sh_hitched = false;
    t->sh_mode = SH_PERCH; t->sh_stay = sh_stay_for(t); t->sh_pose = SH_POSE_BLADE; t->sh_land = SH_LAND_S;
    t->sh_gdepth = t->sh_gdepth_want; t->sh_front = t->sh_front_want; t->sh_obed = -1;
    t->sh_opost = (int8_t)item; t->sh_orow = item == 2 && fabsf(c[k].x - (tank_decor_x(t, 2) + 39)) < 1.0f;
    return true;
}
const char *tank_seahorse_holding(const tank_t *t) {
    if (t->sh_akind == SH_ANCHOR_BLADE) return t->sh_bed >= 0 && tank_veg_kind(t, t->sh_bed) == VEG_KIND_SWORD ? "A LEAF" : "A BLADE";
    if (t->sh_akind == SH_ANCHOR_POST) return t->sh_aitem == 2 ? "THE CASTLE" : t->sh_aitem == 3 ? "THE CORAL" : t->sh_aitem == 9 ? "THE VOLCANO" : "THE REEF";
    return "THE SAND";
}
bool tank_seahorse_hit(const tank_t *t, float x, float y) {
    if (!(t->sd_unlocks & SD_ITEM_SEAHORSE) || t->sh_x < 0) return false;
    return tank_dist(t->sh_x + 4 * t->sh_facing, t->sh_y - SH_BODY_UP, x, y) <= SH_TAP_RADIUS;
}
float tank_seahorse_days(const tank_t *t) { return (float)(t->sh_s / 86400.0); }
/* the finger's edge: SH_HITCH_OFF px from the hold toward the middle of the
 * glass, kept inside the water */
static void sh_finger_spot(const tank_t *t, float *x, float *y) {
    float mid = (TANK_FX0 + TANK_FX1) * 0.5f;
    float dir = t->hold_x < mid ? 1.0f : -1.0f;
    *x = clampf(t->hold_x + dir * SH_HITCH_OFF, TANK_FX0 + 24, TANK_FX1 - 24);
    *y = clampf(t->hold_y, 70, TANK_BOT - 60);
}
/* a swim step: thrust toward the spot with the water's lag, coast, settle.
 * Inside SH_AT_PX it is drawn onto the spot (no snap: a pull that closes
 * the last pixels in ~0.4 s); arrived = on it, still. */
#define SH_ON_PX 0.3f
static bool sh_at(const tank_t *t) { return tank_dist(t->sh_x, t->sh_y, t->sh_tx, t->sh_ty) <= SH_ON_PX && t->sh_vx == 0 && t->sh_vy == 0; }
static void sh_move(tank_t *t, float dt, float px_s) {
    float dx = t->sh_tx - t->sh_x, dy = t->sh_ty - t->sh_y, d = sqrtf(dx * dx + dy * dy);
    if (d <= SH_AT_PX) {                                                 /* the last pixels: a pull, the thrust off */
        float k2 = 1 - expf(-dt * 9.0f);
        t->sh_x += dx * k2; t->sh_y += dy * k2; t->sh_vx *= 1 - k2; t->sh_vy *= 1 - k2;
        if (d < SH_ON_PX) { t->sh_x = t->sh_tx; t->sh_y = t->sh_ty; t->sh_vx = t->sh_vy = 0; }
        return;
    }
    float want = d < SH_EASE_PX ? px_s * d / SH_EASE_PX : px_s;          /* ease off into the spot */
    float wx = d > 0.01f ? dx / d * want : 0, wy = d > 0.01f ? dy / d * want : 0;
    float k = 1 - expf(-dt / SH_LAG_S);                                  /* the lag: speed builds and bleeds off */
    t->sh_vx += (wx - t->sh_vx) * k; t->sh_vy += (wy - t->sh_vy) * k;
    t->sh_x += t->sh_vx * dt; t->sh_y += t->sh_vy * dt;
    if (fabsf(t->sh_vx) > 4) t->sh_facing = t->sh_vx > 0 ? 1 : -1;      /* it faces the way it swims */
}
/* the hold's morph clock: up while settled on something, down once it lets go */
static void sh_land_tick(tank_t *t, float dt, bool settled) {
    t->sh_land = settled ? fminf(SH_LAND_S, t->sh_land + dt) : fmaxf(0, t->sh_land - dt);
}
/* it lets go of what it holds and heads for the next anchor */
static void sh_let_go(tank_t *t, float wait) {
    t->sh_mode = SH_DRIFT; t->sh_stay = wait;
    sh_pick(t, t->sh_x, t->sh_y);
}
/* a snack: a pellet within SH_SENSE of its head starts a SNATCH - first a
 * turn if the pellet is on its other side (a beat), then the head and neck
 * jut forward over SH_SNAP_S and the pellet is taken at the furthest point
 * if it is still within reach of the jutted snout (the snout sits ~16 px
 * ahead of the anchor and ~21 up), with the fish's eat cue. It never swims
 * for one. */
float tank_seahorse_jut(const tank_t *t) {
    if (t->sh_snap < 0) return 0;
    return SH_JUT * sinf(3.14159265f * fminf(1, t->sh_snap / SH_SNAP_S));
}
static void sh_snack(tank_t *t, float dt) {
    if (t->sh_snack_cool > 0) t->sh_snack_cool -= dt;
    if (t->sh_snap >= 0) {                                             /* a snatch under way */
        t->sh_snap += dt;
        if (t->sh_snap_food >= 0 && t->sh_snap >= SH_SNAP_AT) {        /* the head at its furthest: the bite */
            food_t *p = &t->food[t->sh_snap_food];
            float mx = t->sh_x + (16.0f + SH_JUT) * t->sh_facing, my = t->sh_y - 21.0f;
            if (p->alive && tank_dist(mx, my, p->x, p->y) <= SH_REACH + 3) {
                p->alive = false;                                      /* snapped: no fish meal, no shrimp pellet */
                t->sh_eaten++;
                if (t->sh_snack_cool <= 0) { t->sh_snack_pay++; t->sh_snack_cool = SH_SNACK_COOL_S; }
                for (int k = 0; k < MAX_BUBBLE; k++) {                 /* a bubble from the snout */
                    if (t->bubble[k].column) continue;
                    t->bubble[k].x = mx; t->bubble[k].y = my - 2;
                    break;
                }
                tank_emit(TEV_EAT, -1);                                /* the fish's eat cue, at the plain pitch */
            }
            t->sh_snap_food = -1;
        }
        if (t->sh_snap >= SH_SNAP_S) { t->sh_snap = -1; t->sh_turn = SH_SNAP_REST; }   /* back; a rest before the next */
        return;
    }
    if (t->sh_turn > 0) { t->sh_turn -= dt; if (t->sh_turn > 0) return; }
    float hx = t->sh_x + 6.0f * t->sh_facing, hy = t->sh_y - 24.0f;    /* the head's centre */
    for (int i = 0; i < MAX_FOOD; i++) {
        if (!t->food[i].alive || tank_dist(hx, hy, t->food[i].x, t->food[i].y) > SH_SENSE) continue;
        int side = t->food[i].x > t->sh_x + 2 ? 1 : t->food[i].x < t->sh_x - 2 ? -1 : t->sh_facing;
        if (side != t->sh_facing) { t->sh_facing = (int8_t)side; t->sh_turn = SH_TURN_S; return; }   /* it turns to it first */
        /* a snatch only for a pellet the jutted snout will reach: one neck
         * movement per pellet taken, never a jut at a cluster drifting past */
        float mx = t->sh_x + (16.0f + SH_JUT) * t->sh_facing, my = t->sh_y - 21.0f;
        if (tank_dist(mx, my, t->food[i].x, t->food[i].y) > SH_REACH + 1) continue;
        t->sh_snap = 0; t->sh_snap_food = (int8_t)i;
        return;
    }
}
static void seahorse_depth(tank_t *t) {             /* SWIM DEPTH: drifting, its own roll (once a drift); its depth taken up clear of every bed */
    if (t->sh_mode == SH_DRIFT && !t->sh_drift_rolled) {
        float r = depth_hash(t, t->sh_x, t->sh_y, 0x5e4u);
        t->sh_gdepth_want = r < 0.2f ? FISH_DEPTH_BEHIND : r < 0.5f ? FISH_DEPTH_THROUGH : FISH_DEPTH_FRONT;
    }
    t->sh_drift_rolled = t->sh_mode == SH_DRIFT;
    const bool call = t->sh_mode == SH_HITCH && !g_dbg_hold_emerge_off;   /* a held finger's call: in sight - the front lane, in front of
                                                                      the grass (through the seagrass if it must), never through a piece */
    const uint8_t gw = call ? FISH_DEPTH_FRONT : t->sh_gdepth_want;
    if (t->sh_gdepth != gw && (call || box_clear_beds(t, t->sh_x - 20, t->sh_x + 20, t->sh_y + 30))) t->sh_gdepth = gw;
    const bool fw = call || t->sh_front_want;      /* THE LANE (0.4): clear of every piece, or as it settles on its hold */
    if (t->sh_front != fw && (t->sh_mode == SH_PERCH || box_clear_pieces(t, t->sh_x - 20, t->sh_x + 20, t->sh_y + 30))) t->sh_front = fw;
    /* THE BED'S STACK (0.3.9.7): heading for a frond, it goes behind what stands before that frond's bed - once clear of every
       piece, so it never pops through one; settled on the frond, at once (it is coiled on it: whatever hides the frond hides it) */
    const int obed = t->sh_akind == SH_ANCHOR_BLADE && t->sh_mode != SH_HITCH ? t->sh_bed : -1;
    if (t->sh_obed != obed && ((t->sh_mode == SH_PERCH && obed >= 0) || box_clear_pieces(t, t->sh_x - 20, t->sh_x + 20, t->sh_y + 30))) t->sh_obed = (int8_t)obed;
    /* THE PIECE'S STACK (0.4): heading for a post, it goes right before that piece - under what stands before it - the same way */
    const int opost = t->sh_akind == SH_ANCHOR_POST && t->sh_mode != SH_HITCH ? t->sh_aitem : -1;
    const bool orow = opost == 2 && fabsf(t->sh_ax - (tank_decor_x(t, 2) + 39)) < 1.0f;   /* (the rear tower: behind the castle's front row) */
    if ((t->sh_opost != opost || t->sh_orow != orow) && ((t->sh_mode == SH_PERCH && opost >= 0) || box_clear_pieces(t, t->sh_x - 20, t->sh_x + 20, t->sh_y + 30)))
        { t->sh_opost = (int8_t)opost; t->sh_orow = orow; }
}
void seahorse_tick(tank_t *t, float dt) {
    if (!(t->sd_unlocks & SD_ITEM_SEAHORSE)) return;
    if (t->sh_x < 0) tank_seahorse_place(t);
    t->sh_s += dt;
    seahorse_depth(t);
    sh_snack(t, dt);                                               /* in the dark too: the fish eat then (0.3.7) */
    bool finger = t->hold_active && t->hold_time >= HOLD_ATTRACT_S;
    if (finger) {                                                  /* the one thing that moves it at once */
        if (t->sh_mode != SH_HITCH) { t->sh_mode = SH_HITCH; t->sh_akind = SH_ANCHOR_NONE; t->sh_bed = t->sh_frond = -1; t->sh_hitched = false; t->sh_credited = false; }
        sh_finger_spot(t, &t->sh_tx, &t->sh_ty);
        bool hides[5]; float ex, ey;                                   /* THE WAY OUT: behind a piece (its back lane, or the bed's stack it
                                                                          coiled in), out round it first - it cannot see the finger through it */
        for (int k = 0; k < 5; k++) hides[k] = t->sh_obed >= 0 || (!t->sh_front && tank_decor_z(t, EXIT_P[k]) == DECOR_Z_FRONT);
        const bool out = !g_dbg_hold_emerge_off && exit_round(t, t->sh_x, t->sh_y, 20, 30, hides, t->sh_tx, t->sh_ty, &ex, &ey);
        if (out) { t->sh_tx = ex; t->sh_ty = ey; }
        sh_move(t, dt, SH_SWIM_PX_S);
        bool at = !out && sh_at(t);                                    /* settled on the spot, not passing through it (nor the way out) */
        if (at && !t->sh_credited) { t->sh_credited = true; t->sh_hitches++; }
        t->sh_hitched = at;
        if (at && t->sh_pose != SH_POSE_GRIP) { t->sh_pose = SH_POSE_GRIP; t->sh_land = 0; }   /* the tail closes on the finger */
        sh_land_tick(t, dt, at);
        t->sh_facing = t->hold_x >= t->sh_x ? 1 : -1;              /* it faces the finger */
        t->sh_stay = SH_LINGER_S;
        return;
    }
    if (t->sh_mode == SH_HITCH) {                                  /* the finger lifted: a moment, then home */
        t->sh_hitched = false; t->sh_credited = false;
        sh_land_tick(t, dt, false);                                    /* it lets go of the finger */
        t->sh_stay -= dt;
        if (t->sh_stay > 0) return;
        sh_let_go(t, 0);
    }
    if (t->sh_mode == SH_PERCH) {
        if (!sh_hold_ok(t)) {                                      /* the scissors took its blade, the piece moved */
            /* a SHORT wait before it looks again: the blade's stay timer used
             * to carry over here and it hung in the water for minutes
             * (Strato 10-15: "it moved a little bit but now it looks frozen") */
            sh_let_go(t, tank_randf(t, 0.4f, 1.2f));
            return;
        }
        if (t->sh_akind == SH_ANCHOR_BLADE) {
            float gx, gy; sh_grip(t, t->sh_bed, t->sh_frond, &gx, &gy);
            if (fabsf(gy - t->sh_ay) > 3.0f || fabsf(gx - t->sh_ax) > 3.0f) {   /* the scissors took the blade down under it: not a teleport to
                                                                                  * the new grip (Strato 10-15, the watch) - it is knocked off, sinks
                                                                                  * a little and swims back to the blade's new grip */
                t->sh_ax = gx; t->sh_ay = gy; t->sh_tx = gx; t->sh_ty = gy;
                t->sh_mode = SH_DRIFT; t->sh_stay = 0; t->sh_vy = 14; t->sh_vx = 0;
                return;
            }
            t->sh_ax = gx; t->sh_ay = gy;                                   /* growth: it rides the blade */
        }
        t->sh_x = t->sh_ax; t->sh_y = t->sh_ay;
        t->sh_vx = t->sh_vy = 0;
        sh_land_tick(t, dt, true);                                     /* the tail closes round the anchor */
        if (t->sh_akind == SH_ANCHOR_SAND) {                           /* resting: a new plant or piece takes it off the bottom at once
                                                                        * (Strato 10-15: "i added a sword plant, i would have expected
                                                                        * the seahorse to move to it right away") */
            t->sh_look -= dt;
            if (t->sh_look <= 0) { t->sh_look = 2.0f; if (sh_any_hold(t)) { sh_let_go(t, 0); return; } }
        }
        /* the dark: it stays on its hold till the light (it rests like the
         * fish). Only this - the night used to stop every move, so a seahorse
         * let off the finger in the dark hung in open water where the finger
         * was until the light came on (Strato 2026-10-18); a trip home, a
         * knock-off and a new plant all still run now. */
        if (t->night) return;
        t->sh_stay -= dt;
        if (t->sh_stay <= 0) {                                     /* off to another (from the sand: a look around) */
            if (t->sh_akind != SH_ANCHOR_SAND && !sh_another_hold(t)) { t->sh_stay = sh_stay_for(t); return; }   /* the only hold there is: it stays */
            sh_let_go(t, 0);
        }
        return;
    }
    /* adrift: on its way to an anchor (a blade, a post, or a spot on the sand) */
    if (t->sh_akind != SH_ANCHOR_NONE && t->sh_akind != SH_ANCHOR_SAND && !sh_hold_ok(t)) sh_pick(t, t->sh_x, t->sh_y);
    if (t->sh_akind == SH_ANCHOR_BLADE) { sh_grip(t, t->sh_bed, t->sh_frond, &t->sh_ax, &t->sh_ay); t->sh_tx = t->sh_ax; t->sh_ty = t->sh_ay; }
    if (t->sh_stay > 0) { t->sh_stay -= dt; sh_land_tick(t, dt, false); return; }   /* the moment after a loss: it hangs, then goes */
    if (t->sh_akind == SH_ANCHOR_SAND && sh_any_hold(t)) sh_pick(t, t->sh_x, t->sh_y);   /* on its way to rest: something better turned up */
    sh_move(t, dt, SH_DRIFT_PX_S);
    sh_land_tick(t, dt, false);                                        /* whatever it held, it has let go */
    if (sh_at(t)) {                                                    /* the tail finds the anchor */
        t->sh_mode = SH_PERCH; t->sh_stay = sh_stay_for(t); t->sh_pose = SH_POSE_BLADE; t->sh_land = 0;
    }
}
