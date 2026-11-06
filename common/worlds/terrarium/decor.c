/* decor.c - the terrarium's decorations: their size, and where each may stand (the terrarium module's, terrarium.c)
 * (issue #4 phase 3: moved out of tank.c; see tank.h for the rules it plays by) */
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

/* THE TERRARIUM's DECORATIONS (tank.h, P5b): their art's size (render.c draws them from these through tank_decor_half_w /
   tank_decor_height), the floor fraction each stands at until placed (slid clear), their bits - by index, the order of
   tank_t.deco_x. The shared decor machinery (tank.c) asks for them through the mech hook `deco`; where one may stand is below */
static const deco_info_t DECO[TERR_DECO_N] = {
    /* index half_w height u      fy             bit */
    { 0,    11,    48,    0.86f,  TERR_DECO_FY,  SD_ITEM_SAGUARO },       /* the saguaro */
    { 1,    16,    18,    0.60f,  TERR_DECO_FY,  SD_ITEM_SKULL },         /* the skull */
    { 2,    28,    16,    0.38f,  TERR_DECO_FY,  SD_ITEM_DRIFTWOOD },     /* the driftwood */
    { 3,    12,    28,    0.14f,  TERR_DECO_FY,  SD_ITEM_POT },           /* the pot */
};
_Static_assert(TERR_DECO_N == sizeof ((tank_t *)0)->deco_x, "a saved x for every decoration");
const deco_info_t *terr_deco(int item) { const int d = item - TERR_DECO_ITEM0; return d >= 0 && d < TERR_DECO_N ? &DECO[d] : NULL; }

#define DECO_GAP 6.0f      /* the sand kept clear past a hide's or the dish's edge */
/* the spans a piece of half-width `half` may not be centred in: over a hide or the dish, on a ramp's foot (its way on and
   off the floor: the ramp's link with the floor, terrain.h) or under the basking spot - all where this glass has them */
static int deco_bars(const tank_t *t, float half, float a[], float b[]) {
    int n = 0;
    const habitat_t *h = tank_habitat(t);
    for (int k = 0; k < h->n_hides && n < 8; k++) { float hx, hy; tank_hide_point(t, k, &hx, &hy); a[n] = hx - TERRAIN_HIDE_HW - half - DECO_GAP; b[n] = hx + TERRAIN_HIDE_HW + half + DECO_GAP; n++; }
    if (tank_dish_here(t)) { float dx, dy; tank_dish_point(t, &dx, &dy); a[n] = dx - HABITAT_DISH_HW - half - DECO_GAP; b[n] = dx + HABITAT_DISH_HW + half + DECO_GAP; n++; }
    const terrain_t *tr = tank_terrain(t);
    for (int k = 0; k < tr->n_links && n < 20; k++)
        if (tr->link[k].a == 0 || tr->link[k].b == 0) { a[n] = tr->link[k].x - half - 14; b[n] = tr->link[k].x + half + 14; n++; }
    if (tr->bask_path >= 0) { a[n] = tr->bask_x - half - 16; b[n] = tr->bask_x + half + 16; n++; }
    return n;
}
static void deco_span(float half, float *lo, float *hi) {      /* the sand it may stand on, inside the glass */
    *lo = fmaxf(TANK_FX0 + DECOR_MARGIN, tank_glass_x0(TERR_DECO_FY) + 6) + half;
    *hi = fminf(TANK_FX1 - DECOR_MARGIN, tank_glass_x1(TERR_DECO_FY) - 6) - half;
}
bool terr_deco_clear(const tank_t *t, int item, float x) {
    const deco_info_t *d = terr_deco(item);
    if (!d) return true;
    float lo, hi, a[24], b[24]; deco_span(d->half_w, &lo, &hi);
    if (x < lo - 0.5f || x > hi + 0.5f) return false;
    const int n = deco_bars(t, d->half_w, a, b);
    for (int k = 0; k < n; k++) if (x > a[k] && x < b[k]) return false;
    return true;
}
float terr_deco_slide(const tank_t *t, int item, float x) {
    const deco_info_t *d = terr_deco(item);
    if (!d) return x;
    float lo, hi, a[24], b[24]; deco_span(d->half_w, &lo, &hi);
    x = clampf(x, lo, hi);
    if (terr_deco_clear(t, item, x)) return x;
    const int n = deco_bars(t, d->half_w, a, b);
    float best = x, bd = 1e9f;
    for (int k = 0; k <= n; k++)
        for (int e = 0; e < 2; e++) {                              /* a bar's edge, 2 px past it (the save keeps x to 2 px); the sand's ends */
            const float c = k == n ? (e ? hi : lo) : e ? b[k] + 2 : a[k] - 2;
            if (fabsf(c - x) < bd && terr_deco_clear(t, item, c)) { bd = fabsf(c - x); best = c; }
        }
    return best;
}
float terr_deco_home(const tank_t *t, int item) {
    const deco_info_t *d = terr_deco(item);
    if (!d) return 0;
    float lo, hi; deco_span(d->half_w, &lo, &hi);
    const float want = tank_floor_x(d->u);
    float best = terr_deco_slide(t, item, want), bs = 1e9f;
    for (float x = lo; x <= hi; x += 2) {                          /* every clear x: the overlap with the others first, then the way off its own */
        if (!terr_deco_clear(t, item, x)) continue;
        float over = 0;
        for (int e = 0; e < TERR_DECO_N; e++) {
            if (e == d->index || !tank_decor_in(t, TERR_DECO_ITEM0 + e)) continue;
            const float gap = fabsf(x - tank_decor_x(t, TERR_DECO_ITEM0 + e)) - d->half_w - DECO[e].half_w - 4;
            if (gap < 0) over -= gap;
        }
        const float s = over * 8 + fabsf(x - want);
        if (s < bs) { bs = s; best = x; }
    }
    return best;
}
/* a finger on the glass over a decoration (tank.h tank_decor_hit): this world owns the answer - the piece, or none: the hand, never
   an invisible aquarium piece from old bits / default spots. Each decoration's own box, above its feet, the last drawn first - the
   hand on the sand under it stays the hand, and so does a hold on a lizard in front of it (the hand hold is the trust's, P2c) */
bool terr_deco_hit(const tank_t *t, float x, float y, int *item) {
    static const int8_t DRAW[TERR_DECO_N] = TERR_DECO_DRAW;
    *item = -1;
    for (int k = TERR_DECO_N - 1; k >= 0; k--) {
        const int d = DRAW[k], it = TERR_DECO_ITEM0 + d;
        if (!tank_decor_in(t, it) || tank_decor_locked(t, it)) continue;
        const float cx = tank_decor_x(t, it), half = DECO[d].half_w + 6;
        if (x >= cx - half && x <= cx + half && y >= TERR_DECO_FY - DECO[d].height - 6 && y <= TERR_DECO_FY - 2) {
            for (int i = 0; i < t->n_fish; i++)
                if (tank_swims(t, i) && tank_tap_d2(t, i, t->fish[i].x, t->fish[i].y, x, y) < 16.0f * 16.0f) return true;
            *item = it;
            return true;
        }
    }
    return true;
}
