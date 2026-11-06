/* tr_decor.c - the terrarium's decorations
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

/* THE TERRARIUM's DECORATIONS (tank.h, P5b): their art's size (render.c draws them from these), the floor fraction each
   stands at until placed (slid clear), their bits */
const float    DECO_HW[TERR_DECO_N]  = { 11, 16, 28, 12 };            /* the saguaro, the skull, the driftwood, the pot */
const float    DECO_H[TERR_DECO_N]   = { 48, 18, 16, 28 };
const float    DECO_U[TERR_DECO_N]   = { 0.86f, 0.60f, 0.38f, 0.14f };
const uint32_t DECO_BIT[TERR_DECO_N] = { SD_ITEM_SAGUARO, SD_ITEM_SKULL, SD_ITEM_DRIFTWOOD, SD_ITEM_POT };
_Static_assert(TERR_DECO_N == sizeof ((tank_t *)0)->deco_x, "a saved x for every decoration");

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
bool tank_deco_clear(const tank_t *t, int item, float x) {
    const int d = tank_deco_index(item);
    if (d < 0) return true;
    float lo, hi, a[24], b[24]; deco_span(DECO_HW[d], &lo, &hi);
    if (x < lo - 0.5f || x > hi + 0.5f) return false;
    const int n = deco_bars(t, DECO_HW[d], a, b);
    for (int k = 0; k < n; k++) if (x > a[k] && x < b[k]) return false;
    return true;
}
float tank_deco_slide(const tank_t *t, int item, float x) {
    const int d = tank_deco_index(item);
    if (d < 0) return x;
    float lo, hi, a[24], b[24]; deco_span(DECO_HW[d], &lo, &hi);
    x = clampf(x, lo, hi);
    if (tank_deco_clear(t, item, x)) return x;
    const int n = deco_bars(t, DECO_HW[d], a, b);
    float best = x, bd = 1e9f;
    for (int k = 0; k <= n; k++)
        for (int e = 0; e < 2; e++) {                              /* a bar's edge, 2 px past it (the save keeps x to 2 px); the sand's ends */
            const float c = k == n ? (e ? hi : lo) : e ? b[k] + 2 : a[k] - 2;
            if (fabsf(c - x) < bd && tank_deco_clear(t, item, c)) { bd = fabsf(c - x); best = c; }
        }
    return best;
}
float tank_deco_home(const tank_t *t, int item) {
    const int d = tank_deco_index(item);
    if (d < 0) return 0;
    float lo, hi; deco_span(DECO_HW[d], &lo, &hi);
    const float want = tank_floor_x(DECO_U[d]);
    float best = tank_deco_slide(t, item, want), bs = 1e9f;
    for (float x = lo; x <= hi; x += 2) {                          /* every clear x: the overlap with the others first, then the way off its own */
        if (!tank_deco_clear(t, item, x)) continue;
        float over = 0;
        for (int e = 0; e < TERR_DECO_N; e++) {
            if (e == d || !tank_decor_in(t, TERR_DECO_ITEM0 + e)) continue;
            const float gap = fabsf(x - tank_decor_x(t, TERR_DECO_ITEM0 + e)) - DECO_HW[d] - DECO_HW[e] - 4;
            if (gap < 0) over -= gap;
        }
        const float s = over * 8 + fabsf(x - want);
        if (s < bs) { bs = s; best = x; }
    }
    return best;
}
