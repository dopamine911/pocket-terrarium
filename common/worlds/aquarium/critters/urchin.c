/* critters/urchin.c - the aquarium's urchin (SD_ITEM_URCHIN), a creature of the aquarium module (aquarium.c)
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

/* ---- the urchin (2026-10-02, SD_ITEM_URCHIN; tank.h has the rules) ----
 * The grass's snail. Awake its appetite fills slowly (URCHIN_AWAKE_PER_H of
 * frond height an hour, for the whole tank: about what a trimmed bed grows
 * awake, so it holds the grass where the keeper left it - the scissors
 * still bring it down; 0.30 only slowed the climb, and keepers saw an
 * urchin "on vacation", 0.3.6); with a bite's worth it crawls to the frond standing
 * tallest over URCHIN_KEEP (a near one wins a close call), chews at its foot
 * for URCHIN_CHEW_S while the frond comes down a bite and bits of leaf drift
 * up from the tip, then rests and ambles the sand. Asleep it grazes the
 * grass's height over the keep line in proportion to how much there is
 * (URCHIN_SLEEP_FRAC_H of it an hour), every frond its share, so the canopy
 * keeps its shape, only lower; a tall tank feeds it faster, so a long
 * absence settles below the ceilings instead of on them. */
#define URCHIN_PX_S          1.6f     /* tube feet: slower than the snail */
#define URCHIN_AWAKE_PER_H   0.50f    /* (0.30 until 0.3.6: 6 h awake from 0.57 climbed to 0.63; now ~flat) */
#define URCHIN_SLEEP_FRAC_H  0.35f    /* of the height over the keep line, an hour asleep (tuned in
                                       * selftest-sleep: a 7 h night from a trimmed 0.35 lands ~0.52
                                       * where it lands 0.69 alone; days away settle ~0.54, not 0.83.
                                       * 0.20 until 0.3.6: 0.57 a night, 0.64 a week) */
#define URCHIN_BITE          0.05f    /* one sitting: ~17 px off a frond on the 1.8 */
#define URCHIN_CHEW_S        14.0f
#define URCHIN_TAP_RADIUS    40.0f    /* a fingertip round a ~32 px urchin */
#define URCHIN_HALF_W        16.0f
static bool urchin_frond_ok(const tank_t *t, int b, int i) {
    if (b < 0 || b >= tank_veg_beds(t) || tank_veg_kind(t, b) != VEG_KIND_GRASS) return false;
    float bx0; int n; veg_bed_base(t, b, &bx0, &n);
    return i >= 0 && i < n;
}
static float urchin_lo(void) { return TANK_FX0 + DECOR_MARGIN + URCHIN_HALF_W; }
static float urchin_hi(void) { return TANK_FX1 - DECOR_MARGIN - URCHIN_HALF_W; }
static float urchin_frond_x(const tank_t *t, int b, int i) {
    float x; tank_veg_frond(t, b, i, &x);
    return clampf(x, urchin_lo(), urchin_hi());
}
/* the frond standing tallest over the keep line, a near one winning a close
 * call; -1 when no frond is worth a sitting */
static bool urchin_pick(tank_t *t) {
    float best = 0; int bb = -1, bi = -1;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (tank_veg_kind(t, b) != VEG_KIND_GRASS) continue;
        float bx0; int n; veg_bed_base(t, b, &bx0, &n);
        for (int i = 0; i < n; i++) {
            float over = t->veg_h[b][i] - URCHIN_KEEP;
            if (over < URCHIN_BITE * 0.5f) continue;
            float score = over - fabsf(urchin_frond_x(t, b, i) - t->urchin_x) * 0.0004f;
            if (bb < 0 || score > best) { best = score; bb = b; bi = i; }
        }
    }
    t->urchin_bed = (int8_t)bb; t->urchin_frond = (int8_t)bi; t->urchin_chew = 0;
    return bb >= 0;
}
bool tank_urchin_chewing(const tank_t *t) { return t->urchin_frond >= 0 && t->urchin_chew > 0; }
bool tank_urchin_hit(const tank_t *t, float x, float y) {
    if (!(t->sd_unlocks & SD_ITEM_URCHIN) || t->urchin_x < 0) return false;
    return tank_dist(t->urchin_x, URCHIN_FLOOR_Y + t->urchin_dy, x, y) <= URCHIN_TAP_RADIUS;
}
void urchin_tick(tank_t *t, float dt) {
    if (!(t->sd_unlocks & SD_ITEM_URCHIN)) return;
    if (t->urchin_x < 0) tank_urchin_place(t);
    t->urchin_appetite = fminf(t->urchin_appetite + URCHIN_AWAKE_PER_H / 3600.0f * dt, URCHIN_BITE * 2);
    if (t->urchin_frond >= 0 && (!urchin_frond_ok(t, t->urchin_bed, t->urchin_frond)
                                 || t->veg_h[t->urchin_bed][t->urchin_frond] <= URCHIN_KEEP + 0.005f)) {
        t->urchin_bed = t->urchin_frond = -1; t->urchin_chew = 0;      /* the keeper's scissors got there first */
    }
    if (t->urchin_frond < 0 && t->urchin_appetite >= URCHIN_BITE) urchin_pick(t);
    if (t->urchin_frond >= 0) {
        int b = t->urchin_bed, i = t->urchin_frond;
        float fx = urchin_frond_x(t, b, i), d = fx - t->urchin_x;
        if (fabsf(d) > 1.5f) {                                         /* on its way */
            t->urchin_x += (d > 0 ? 1 : -1) * fminf(fabsf(d), URCHIN_PX_S * dt);
            t->urchin_chew = 0;
            return;
        }
        float before = t->veg_h[b][i];                                 /* chewing at its foot */
        float h = fmaxf(URCHIN_KEEP, before - URCHIN_BITE / URCHIN_CHEW_S * dt);
        float eat = fminf(before - h, fmaxf(t->urchin_appetite, 0));
        t->veg_h[b][i] = before - eat;
        t->urchin_appetite -= eat;
        t->urchin_grazed_px += eat * (VEG_SEGS_FULL - 1) * VEG_PAY_PX;
        float c0 = t->urchin_chew; t->urchin_chew += dt;
        if (fmodf(c0, 1.8f) > fmodf(t->urchin_chew, 1.8f)) {           /* a bit of leaf drifts up from the tip */
            float x; int segs = tank_veg_frond(t, b, i, &x);
            for (int k = 0; k < MAX_BUBBLE; k++) {
                if (t->bubble[k].column) continue;
                t->bubble[k].x = x + tank_randf(t, -3, 3);
                t->bubble[k].y = TANK_BOT - 16 - segs * VEG_SEG_PX;
                break;
            }
        }
        veg_sync(t);
        if (t->urchin_appetite <= 0.0005f || t->veg_h[b][i] <= URCHIN_KEEP + 0.0005f) {   /* full, or down to the keep line */
            t->urchin_bed = t->urchin_frond = -1; t->urchin_chew = 0;
            t->urchin_to = -1; t->urchin_rest = tank_randf(t, 30, 70);
        }
        return;
    }
    t->urchin_dy = lane_drop(t, t->urchin_front, t->urchin_x, 13, URCHIN_FLOOR_Y + UR_BASE_T, t->urchin_dy, dt);
    if (t->urchin_rest > 0) { t->urchin_rest -= dt; return; }      /* resting, its spines waving */
    if (t->urchin_to < 0) {
        t->urchin_to = tank_randf(t, urchin_lo(), urchin_hi());
        float r = tank_randf(t, 0, 1);                                                           /* the roll is always drawn: the RNG's run is the same with or without a piece */
        if (tank_floor_clear(t, t->urchin_x, 19)) t->urchin_front = r < LANE_FRONT_ODDS;       /* its lane for this move (a keeper's ask, 10-24), only in the clear */
        t->urchin_gdepth_want = critter_depth_roll(t, t->urchin_to, 0x3c2u);                    /* SWIM DEPTH: its depth against the grass for it */
    }
    float d = t->urchin_to - t->urchin_x;
    t->urchin_x += (d > 0 ? 1 : -1) * fminf(fabsf(d), URCHIN_PX_S * 0.7f * dt);
    if (fabsf(d) < 0.5f) { t->urchin_to = -1; t->urchin_rest = tank_randf(t, 40, 120); }
}
/* asleep: a share of the grass over the keep line, every frond in proportion
 * (see above); it wakes where the grass stands tallest */
void urchin_sleep(tank_t *t, float seconds) {
    if (!(t->sd_unlocks & SD_ITEM_URCHIN)) return;
    if (t->urchin_x < 0) tank_urchin_place(t);
    float k = 1.0f - expf(-URCHIN_SLEEP_FRAC_H * seconds / 3600.0f);
    float eaten = 0, most = 0; int mb = -1, mi = -1;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (tank_veg_kind(t, b) != VEG_KIND_GRASS) continue;
        float bx0; int n; veg_bed_base(t, b, &bx0, &n);
        for (int i = 0; i < n; i++) {
            float over = t->veg_h[b][i] - URCHIN_KEEP;
            if (over <= 0) continue;
            t->veg_h[b][i] -= over * k; eaten += over * k;
            if (over > most) { most = over; mb = b; mi = i; }
        }
    }
    if (eaten <= 0) return;
    t->urchin_grazed_px += eaten * (VEG_SEGS_FULL - 1) * VEG_PAY_PX;
    if (mb >= 0) t->urchin_x = urchin_frond_x(t, mb, mi);
    t->urchin_bed = t->urchin_frond = -1; t->urchin_chew = 0; t->urchin_to = -1; t->urchin_rest = 0;
    veg_sync(t);
}

void tank_urchin_place(tank_t *t) {
    float x0, x1; tank_veg_bed(t, 0, &x0, &x1, NULL, NULL);       /* on the sand just right of the reef bed */
    t->urchin_x = clampf(x1 + 18, urchin_lo(), urchin_hi());
    t->urchin_front = false;
    t->urchin_bed = t->urchin_frond = -1; t->urchin_chew = 0; t->urchin_appetite = 0;
    t->urchin_to = -1; t->urchin_rest = 20;
}
