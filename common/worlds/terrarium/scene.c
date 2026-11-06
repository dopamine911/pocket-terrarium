/* scene.c - THE TERRARIUM's scene (issue #4 phase 3: moved verbatim out of render.c, where it was THE TERRARIUM block).
 *
 * render_tank's frame in the terrarium: the desert (scene_desert.h) on the habitat's ground, the dish, the crickets, the
 * decorations and the lizards on it, the mister's spray, the film on the glass. render.c reaches it through world_view()
 * (view.c); it draws through the renderer's pixel layer (render_core.h) and the lizard body (bodies/lizard). */
#include "render_core.h"
#include "worlds/terrarium/scene.h"
#include "worlds/terrarium/scene_desert.h"
#include "bodies/lizard/lizard.h"
#include "pt_util.h"
#include "terrain.h"
#include "habitat.h"
#include "icons.h"
#include "progression.h"
#include <stdlib.h>

/* ==== THE TERRARIUM (pocket-terrarium P3b): render_tank in the terrarium's bin ====
 * The scene is the desert (render_scene_desert.h) on the habitat's ground (terrain.h), baked into the scene cache; over
 * it each frame the mealworm dish, the crickets and the lizards (draw_lizard, render_lizard.h, from the engine's state),
 * back to front by where their feet stand on the glass - lower is nearer - then the mister's spray, the vignette
 * re-applied on what was drawn, the film on the glass and the snail on it. The cards, the pages, the tool chip are the
 * platform's, drawn after render_tank as in the aquarium.
 *
 * THE BAKE: one scene per light - the shown one in g_scene, the other in the second buffer (render_set_scene_spare; the
 * sim's calloc'd at the first need) - so the light's switch is a swap, no bake. The light not shown is baked in the
 * background, TS_BAKE_US of a frame (TS_BAKE_ROWS rows without a clock); a switch before it is done keeps the old light
 * on the glass while the bake hurries (TS_HURRY_US a frame), then swaps. A humid hide's moisture step repaints only its
 * box (scene_desert_moist_box) in the scene shown, at once - the other one when it is next shown. Only the first scene of
 * a habitat is baked whole in one frame (a boot, a new tank).
 *
 * THE STACK: a lizard up on a rock is behind one on the floor below it and over the rock it stands on (the scene); one
 * going into a hide, coming out of it or looking out of its door is drawn BEHIND the hide's front (lz_canvas_t.cover:
 * the hide's front from the door's open edge back, scene_desert_hide_part) - it shows through the door's dark and in
 * front of the hide's flank on the open side; a HIDDEN one is not drawn. In the lamp's pool a lizard is lit as the rock
 * under it (scene_desert_lamp_at: the pool's gain at its feet, x TS_GLOW_K - it is round, the rock's top flat), with the
 * lamp out moonlit as the scene. Its look: lizard_morph_of (the morph by its body colour's gene), its stage's pattern
 * and size (tank_lizard_scale), its shed (SHEDDING's loose skin by how far through the window it is), the engine's
 * draw-only phase, blink, tongue and fat (tank.c THE LOOK), its pitch the ground's slope under it (fish_t.slope). */
#define TS_BAKE_US    5000              /* the background bake's share of a frame on the board (~4 rows of the 1.8's) ... */
#define TS_HURRY_US   16000             /* ... and a switch's, while the old light still shows */
#define TS_BAKE_ROWS  12                /* (without a clock - the sim's tests: rows a frame) */
#define TS_HURRY_ROWS 48
#define TS_GLOW_K     0.6f              /* the lamp's pool on a lizard: this share of the rock's gain */
#define TS_COVER_W    96                /* a hide's box (scene_desert_hide_box: the warm cave's 82 x 42 the widest) */
#define TS_COVER_H    48
#define TS_COVER_WPR  ((TS_COVER_W + 31) / 32)
#define TS_PEEK       0.30f             /* a look out: the head and the neck (lizard_pose_t.peek), in over ~0.45 s, back over 0.35 */
#define TS_PATCH_FRAMES 3               /* a moisture step's box, copied into this many frames primed with the scene as it was (the
                                           platform's two buffers and one in flight) */
typedef struct { short x0, y0, x1, y1; } ts_rect_t;
typedef struct { bool ok; uint8_t lit, hab, bulb, moist[HABITAT_HIDES_MAX]; } ts_key_t;   /* (bulb: the shop's BASKING BULB, its wider pool) */
static uint16_t *g_ts_scene0, *g_ts_spare;      /* the platform's scene cache, the second buffer (render_set_scene_spare) */
static bool      g_ts_spare_own, g_ts_swapped;  /* (the sim's own spare); the spare is the one shown (g_scene) */
static ts_key_t  g_ts_show, g_ts_other;         /* what the scene shown holds, and the other buffer (whole) */
static ts_key_t  g_ts_job; static int g_ts_job_row;   /* the bake under way into the other buffer: its key, its next row */
static ts_key_t  g_ts_ctx; static unsigned g_ts_ctx_n;   /* scene_desert_begin's context: the key it was begun with (and its count) */
static scene_desert_t g_ts_d; static int g_ts_d_hab = -1;   /* the layout on this glass: the hides, the peek, the lamp */
static uint32_t  g_ts_cover[HABITAT_HIDES_MAX][TS_COVER_H * TS_COVER_WPR];   /* each hide's front, from its door's open edge back */
static int       g_ts_cov[HABITAT_HIDES_MAX][4];                              /* ... its box: x0, y0, w, h */
static uint32_t  g_ts_in_uid[N_FISH_SLOTS]; static int8_t g_ts_in_k[N_FISH_SLOTS];   /* a lizard last seen in hide k (out: behind its front until clear) */
static int       g_ts_rows, g_ts_slices; static int64_t g_ts_us_max;            /* (render_terrarium_bake_stats) */
static ts_rect_t g_ts_patch; static int g_ts_patch_n;                           /* the moisture step's box, and the frames still to take it */

void terrarium_scene_reset(uint16_t *scene) {
    g_ts_scene0 = scene; g_ts_swapped = false;
    g_ts_show.ok = g_ts_other.ok = g_ts_job.ok = g_ts_ctx.ok = false; g_ts_patch_n = 0;
}
void render_set_scene_spare(uint16_t *buf) {
    if (g_ts_spare_own) free(g_ts_spare);
    g_ts_spare = buf; g_ts_spare_own = false;
    if (g_ts_swapped && g_ts_scene0) g_scene = g_ts_scene0;          /* (the spare was shown: the platform's own back on the glass) */
    terrarium_scene_reset(g_ts_scene0);
}
void render_terrarium_bake_stats(int *rows, int *slices, int64_t *us_max) {
    if (rows) *rows = g_ts_rows;
    if (slices) *slices = g_ts_slices;
    if (us_max) *us_max = g_ts_us_max;
}
void render_terrarium_bake_stats_reset(void) { g_ts_rows = g_ts_slices = 0; g_ts_us_max = 0; }
static uint16_t *ts_other(void) {                /* the buffer not shown */
#ifndef ESP_PLATFORM
    if (!g_ts_spare) { g_ts_spare = (uint16_t *)calloc((size_t)TANK_W * TANK_H, sizeof(uint16_t)); g_ts_spare_own = g_ts_spare != NULL; }
#endif
    return g_ts_swapped ? g_ts_scene0 : g_ts_spare;
}
static void ts_swap(void) {
    uint16_t *o = ts_other();
    const ts_key_t k = g_ts_show;
    g_scene = o; g_ts_swapped = !g_ts_swapped;
    g_ts_show = g_ts_other; g_ts_other = k;
    g_scene_epoch++;
}
static inline bool ts_eq(const ts_key_t *a, const ts_key_t *b) { return a->ok && b->ok && !memcmp(a, b, sizeof *a); }
static inline bool ts_light_eq(const ts_key_t *a, const ts_key_t *b) { return a->ok && b->ok && a->lit == b->lit && a->hab == b->hab && a->bulb == b->bulb; }
/* the habitat as the scene draws it: the tank's, its lamp's pool SD_BULB_R_X as wide with the shop's BASKING BULB - the
   engine's own number (tank.c lamp_x: the warmth the lizards feel) - so the pool drawn is the pool felt; the key's bulb
   bakes the scene again once one is bought */
static habitat_t g_ts_hab;
static const habitat_t *ts_habitat(const tank_t *t) {
    g_ts_hab = *tank_habitat(t);
    if (t->sd_unlocks & SD_ITEM_BULB) g_ts_hab.bask_r_px *= SD_BULB_R_X;
    return &g_ts_hab;
}
static ts_key_t ts_want(const tank_t *t, const habitat_t *h) {
    ts_key_t k; memset(&k, 0, sizeof k);
    k.ok = true; k.lit = !t->night; k.hab = h->id; k.bulb = (t->sd_unlocks & SD_ITEM_BULB) != 0;
    for (int i = 0; i < h->n_hides && i < HABITAT_HIDES_MAX; i++)   /* (only a humid hide shows its moisture) */
        k.moist[i] = h->hides[i].kind == HIDE_HUMID ? (uint8_t)scene_desert_moist_step(t->moist[i]) : 0;
    return k;
}
/* the layout on this glass and each hide's cover, once a habitat */
static void ts_layout(const habitat_t *h) {
    if (g_ts_d_hab == h->id) return;
    const glass_t g = glass_here();
    scene_desert_layout(&g, h, &g_ts_d); g_ts_d_hab = h->id;
    memset(g_ts_cover, 0, sizeof g_ts_cover);
    for (int k = 0; k < g_ts_d.n_hides && k < HABITAT_HIDES_MAX; k++) {
        int x0, y0, x1, y1; scene_desert_hide_box(&g_ts_d, k, &x0, &y0, &x1, &y1);
        const int w = x1 - x0 + 1 < TS_COVER_W ? x1 - x0 + 1 : TS_COVER_W, hh = y1 - y0 + 1 < TS_COVER_H ? y1 - y0 + 1 : TS_COVER_H;
        g_ts_cov[k][0] = x0; g_ts_cov[k][1] = y0; g_ts_cov[k][2] = w; g_ts_cov[k][3] = hh;
        for (int y = 0; y < hh; y++)                  /* (scene_desert_hide_covers: the front on the closed side of the door's edge at each row) */
            for (int x = 0; x < w; x++)
                if (scene_desert_hide_covers(&g_ts_d, k, x0 + x + 0.5f, y0 + y + 0.5f)) g_ts_cover[k][y * TS_COVER_WPR + (x >> 5)] |= 1u << (x & 31);
    }
}
static void ts_vig(void) {                       /* the vignette's LUT, as bake_scene fills it */
    if (!g_vig || g_vig_filled) return;
    for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) g_vig[y * TANK_W + x] = (uint8_t)vig_alpha(x, y);
    g_vig_filled = true;
}
static void ts_begin(const tank_t *t, const ts_key_t *k) {
    if (ts_eq(&g_ts_ctx, k) && g_ts_ctx_n == scene_desert_begun()) return;
    float m[HABITAT_HIDES_MAX];
    for (int i = 0; i < HABITAT_HIDES_MAX; i++) m[i] = k->moist[i] / 8.0f;
    const glass_t g = glass_here();
    static habitat_t hb; hb = *tank_habitat(t);                       /* (the key's pool: a bake in flight keeps the light it began with) */
    if (k->bulb) hb.bask_r_px *= SD_BULB_R_X;
    scene_desert_begin(&g, &hb, k->lit, m, k->lit ? 1.0f : 0.45f, g_vig_filled ? g_vig : NULL);
    g_ts_ctx = *k; g_ts_ctx_n = scene_desert_begun();
}
/* rows of scene k into buf from y0 - until the budget (the clock's, or the rows') is spent, at least one - the next row */
static int ts_rows(const tank_t *t, uint16_t *buf, const ts_key_t *k, int y0, int64_t budget_us, int rows) {
    const int64_t t0 = PROF_MARK();
    ts_begin(t, k);
    int y = y0;
    while (y < TANK_H) {
        scene_desert_rows(buf, TANK_W, y, y, 0, TANK_W - 1); y++;
        if (render_clock_us ? render_clock_us() - t0 >= budget_us : y - y0 >= rows) break;
    }
    g_ts_rows += y - y0; g_ts_slices++;
    if (render_clock_us && render_clock_us() - t0 > g_ts_us_max) g_ts_us_max = render_clock_us() - t0;
    return y;
}
/* the humid hides' boxes whose step differs, repainted in buf at key `to`: false none; else their union in *box */
static bool ts_moist(const tank_t *t, uint16_t *buf, const ts_key_t *from, const ts_key_t *to, ts_rect_t *box) {
    bool any = false;
    for (int k = 0; k < HABITAT_HIDES_MAX; k++) any |= from->moist[k] != to->moist[k];
    if (!any) return false;
    ts_begin(t, to);
    any = false;
    for (int k = 0; k < g_ts_d.n_hides && k < HABITAT_HIDES_MAX; k++) {
        int x0, y0, x1, y1;
        if (from->moist[k] == to->moist[k] || !scene_desert_moist_box(&g_ts_d, k, &x0, &y0, &x1, &y1)) continue;
        scene_desert_rows(buf, TANK_W, y0, y1, x0, x1);
        x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > TANK_W - 1 ? TANK_W - 1 : x1; y1 = y1 > TANK_H - 1 ? TANK_H - 1 : y1;
        if (!any) { box->x0 = (short)x0; box->y0 = (short)y0; box->x1 = (short)x1; box->y1 = (short)y1; any = true; }
        else { if (x0 < box->x0) box->x0 = (short)x0; if (y0 < box->y0) box->y0 = (short)y0; if (x1 > box->x1) box->x1 = (short)x1; if (y1 > box->y1) box->y1 = (short)y1; }
    }
    return any;
}
/* the scene the frame shows: baked, swapped, its boxes repainted - and the other light's baked a slice */
/* (tick: the tank's clock moved since the last frame drawn - the slices below are a TICK's: a frame drawn twice, as the
   restore test's fresh reference is, bakes no more and swaps in the same draw as the first) */
static void ts_scene(const tank_t *t, const habitat_t *h, bool tick) {
    const ts_key_t want = ts_want(t, h);
    ts_vig();
    if (!g_ts_show.ok || g_ts_show.hab != want.hab || g_ts_show.bulb != want.bulb) {   /* the first scene - or the BASKING BULB just bought, its
                                                                          wider pool on both lights: whole, now (once; the other light again behind it) */
        ts_rows(t, g_scene, &want, 0, INT64_MAX, TANK_H);
        g_ts_show = want; g_ts_other.ok = g_ts_job.ok = false; g_scene_epoch++;
        return;
    }
    uint16_t *other = ts_other();
    if (g_ts_show.lit == want.lit) {                                   /* the light as shown */
        if (!ts_eq(&g_ts_show, &want)) {                               /* a moisture step: its box repainted - the frames primed with the scene as it
                                                                          was take the box from it (TS_PATCH_FRAMES: the platform's buffers), the rest
                                                                          of them stays as primed (no epoch: no whole copy) */
            if (ts_moist(t, g_scene, &g_ts_show, &want, &g_ts_patch)) g_ts_patch_n = TS_PATCH_FRAMES;
            g_ts_show = want;
        }
        if (!other || !tick) return;
        ts_key_t opp = want; opp.lit = !want.lit;                      /* the background: the other light, if the other buffer has it not */
        if (g_ts_other.ok && ts_light_eq(&g_ts_other, &opp)) return;  /* (its moisture is fixed when it is next shown) */
        if (!ts_light_eq(&g_ts_job, &opp)) { g_ts_job = opp; g_ts_job_row = 0; g_ts_other.ok = false; }
        g_ts_job_row = ts_rows(t, other, &g_ts_job, g_ts_job_row, TS_BAKE_US, TS_BAKE_ROWS);
        if (g_ts_job_row >= TANK_H) { g_ts_other = g_ts_job; g_ts_job.ok = false; }
        return;
    }
    if (!other) {                                                      /* the light switched, one buffer: baked whole, now */
        ts_rows(t, g_scene, &want, 0, INT64_MAX, TANK_H);
        g_ts_show = want; g_scene_epoch++;
        return;
    }
    if (!(g_ts_other.ok && ts_light_eq(&g_ts_other, &want))) {         /* the other light is not whole yet: hurried, the old light shown meanwhile */
        if (!tick) return;
        if (!ts_light_eq(&g_ts_job, &want)) { g_ts_job = want; g_ts_job_row = 0; g_ts_other.ok = false; }
        g_ts_job_row = ts_rows(t, other, &g_ts_job, g_ts_job_row, TS_HURRY_US, TS_HURRY_ROWS);
        if (g_ts_job_row < TANK_H) return;
        g_ts_other = g_ts_job; g_ts_job.ok = false;
    }
    ts_swap();                                                         /* the switch: the other light on the glass */
    if (!ts_eq(&g_ts_show, &want)) { ts_rect_t box; ts_moist(t, g_scene, &g_ts_show, &want, &box); g_ts_show = want; }   /* (the swap copies the
                                                                                                         whole scene: no patch) */
}

/* ---- what moves ---- */
#define TS_RECTS (N_FISH_SLOTS + MAX_CRICKETS + 4 + TERR_DECO_N)
static void ts_mark(void *user, int x0, int x1, int y) {             /* draw_lizard's runs: the dirty and touched masks */
    (void)user;
    if ((unsigned)y >= (unsigned)TANK_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > TANK_W - 1) x1 = TANK_W - 1;
    if (x0 > x1) return;
    dirty_span(x0, x1, y); touch_span_f(x0, x1, y);
}
/* the hide a lizard out on the floor is going into (fish_t.to_hide: its walk's, a rap's dash too) - or coming out of (the
   one it was last seen in, until it is clear of it): its front stands before it; -1 none */
static int ts_behind(const tank_t *t, int i) {
    const fish_t *f = &t->fish[i];
    if (g_ts_in_uid[i] != f->uid) { g_ts_in_uid[i] = f->uid; g_ts_in_k[i] = -1; }
    if (f->hide >= 0) { g_ts_in_k[i] = f->hide; return f->hide; }
    if (f->surf != 0) { g_ts_in_k[i] = -1; return -1; }
    const float half = tank_lizard_half_len(t, f);
    const int out = g_ts_in_k[i];
    if (out >= 0 && out < g_ts_d.n_hides && fabsf(f->x - g_ts_d.hide[out].cx) < g_ts_d.hide[out].half_w + half) return out;
    g_ts_in_k[i] = -1;
    const int k = f->to_hide;
    if (k < 0 || k >= g_ts_d.n_hides || fabsf(f->x - g_ts_d.hide[k].cx) >= g_ts_d.hide[k].half_w + half) return -1;
    return k;
}
static bool ts_lizard(ctx_t *c, const tank_t *t, int i, const habitat_t *h, bool lit, float dim, ts_rect_t *box) {
    const fish_t *f = &t->fish[i];
    const species_t *sp = tank_species(t);
    if (sp->plan != PLAN_LIZARD) return false;
    const int cover = ts_behind(t, i);
    if (f->hide >= 0 && f->pose != LZ_POSE_PEEK) return false;         /* in a hide: out of sight */
    lizard_look_t lk = { &sp->geom, lizard_morph_of(sp, f), &sp->pattern[f->stage & 3], (uint8_t)f->stage, f->uid };
    lizard_pose_t p = lizard_pose(f->pose, f->yaw);
    p.yaw_tail = f->yaw_tail; p.pitch = f->slope;
    p.phase = f->lz_phase; p.blink = f->lz_blink; p.tongue = f->lz_tongue; p.fat = f->lz_fat;
    p.shed = f->shed == SHED_CLEAN ? SHED_NORMAL : f->shed;
    if (p.shed == SHED_SHEDDING) p.shed_k = clampf(1 - f->shed_s / (sp->shed_window_s > 0 ? sp->shed_window_s : 1), 0, 1);
    p.clock = tank_anim(t->clock);
    const float scale = tank_lizard_scale(f);
    float x = f->x, fy = f->y + TERRAIN_FEET_DY;
    if (f->hide >= 0) {                                                /* a look out of its door, the hide's front before it */
        float yaw;
        p.peek = TS_PEEK * clampf(fminf(f->pose_s / 0.45f, f->peek_s / 0.35f), 0, 1);
        if (p.peek < 0.04f) return false;
        scene_desert_peek_at(&g_ts_d, f->hide, lizard_half_len(&sp->geom, scale), p.peek, &x, &fy, &yaw);
        p.yaw = p.yaw_tail = yaw; p.pitch = 0;
    }
    lz_canvas_t cv; memset(&cv, 0, sizeof cv);
    cv.fb = c->fb; cv.stride = c->stride; cv.ox = c->ox; cv.oy = c->oy; cv.w = c->w; cv.h = c->h; cv.dim = dim;
    cv.mark = ts_mark; cv.moon = !lit;
    cv.glow = lit ? TS_GLOW_K * scene_desert_lamp_at(&g_ts_d, h, x, fy) : 0;
    if (cover >= 0 && cover < HABITAT_HIDES_MAX) {
        cv.cover = g_ts_cover[cover]; cv.cover_x0 = g_ts_cov[cover][0]; cv.cover_y0 = g_ts_cov[cover][1];
        cv.cover_w = g_ts_cov[cover][2]; cv.cover_h = g_ts_cov[cover][3]; cv.cover_wpr = TS_COVER_WPR;
    }
    draw_lizard(&cv, &lk, &p, x, fy, scale);
    int x0, y0, x1, y1;
    if (!lizard_last_box(&x0, &y0, &x1, &y1)) return false;
    box->x0 = (short)x0; box->y0 = (short)y0; box->x1 = (short)x1; box->y1 = (short)y1;
    return true;
}
/* the light a thing stands in, as the scene's (render_scene_desert.h scene_light_rgb): the lamp's pool at its feet x
   TS_GLOW_K by day, the moon at night */
static inline uint32_t ts_lit(uint32_t rgb, float glow, bool moon) { return glow > 0 || moon ? scene_light_rgb(rgb, glow, moon) : rgb; }
static float ts_glow_at(const habitat_t *h, bool lit, float x, float y) { return lit ? TS_GLOW_K * scene_desert_lamp_at(&g_ts_d, h, x, y) : 0; }
/* a cricket, side on, facing its way: SIT (legs folded) and the rest (a hop, a fall: the hind legs out) */
static const char *const TS_CK_SIT[6] = { ".aaa......", "a...aa..k.", "...cCCc.kk", "cCCCCCCCkk", ".CCkkCC.l.", ".l.l..l.l." };
static const char *const TS_CK_HOP[6] = { "..aaa.....", ".a...aa.k.", "...cCCc.kk", ".cCCCCCCkk", "l.kkCC.l..", "l.......l." };
static void ts_cricket(ctx_t *c, const cricket_t *k, const habitat_t *h, bool lit, ts_rect_t *box) {
    static const char KEYS[] = "aCckl";
    static const uint32_t RGB[5] = { 0x5a3a1c, 0xa86f32, 0xd9a35c, 0x6e4420, 0x4a2e16 };
    const float glow = ts_glow_at(h, lit, k->x, k->y);
    src_t pal[5]; for (int q = 0; q < 5; q++) pal[q] = src_color(ts_lit(RGB[q], glow, !lit), c->dim);
    const char *const *fr = k->state == CK_SIT || k->state == CK_CRAWL ? TS_CK_SIT : TS_CK_HOP;
    const int f = k->facing < 0 ? -1 : 1, ox = (int)floorf(k->x + 0.5f), oy = (int)floorf(k->y + 0.5f) - 6;
    if (k->state == CK_SIT || k->state == CK_CRAWL) {                  /* its shadow on the ground */
        src_t sh = src_color(0x000000, 1.0f);
        span(c, ox - 4, ox + 4, oy + 6, &sh, 70);
    }
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 10; x++) {
            const char ch = fr[y][x];
            if (ch == '.') continue;
            const char *q = strchr(KEYS, ch);
            if (!q) continue;
            px_blend_s(c, ox + f * (x - 5), oy + y, &pal[q - KEYS], 255);
        }
    box->x0 = (short)(ox - 6); box->y0 = (short)(oy - 1); box->x1 = (short)(ox + 6); box->y1 = (short)(oy + 7);
}
/* THE TERRARIUM's DECORATIONS (tank.h, P5b): each its sprite (assets/icons/deco_*_terr.png, tools/gen_terr_icons.py) on
   the sand at its x, its feet on TERR_DECO_FY just behind the lizards' walking band, in the light the dish stands in -
   the lamp's pool by day, the moon at night, the night's dim - over a soft contact shadow */
static const icon_t *ts_deco_art(int d) {
    static const icon_t *const ART[TERR_DECO_N] = { &icon_deco_saguaro_terr, &icon_deco_skull_terr, &icon_deco_driftwood_terr, &icon_deco_pot_terr };
    return ART[d];
}
static void ts_deco(ctx_t *c, const tank_t *t, int d, const habitat_t *h, bool lit, ts_rect_t *box) {
    const icon_t *ic = ts_deco_art(d);
    const float x = tank_decor_x(t, TERR_DECO_ITEM0 + d);
    const int fy = (int)lroundf(TERR_DECO_FY), x0 = (int)lroundf(x - ic->w * 0.5f), y0 = fy - ic->h;
    const src_t sh = src_color(0x000000, 1.0f);
    for (int r = -1; r <= 1; r++) span(c, x0 + 1 + 3 * abs(r), x0 + ic->w - 2 - 3 * abs(r), fy + r, &sh, 55);   /* its shadow on the sand */
    const float glow = ts_glow_at(h, lit, x, fy - ic->h * 0.5f);
    struct { uint16_t v; bool ok; src_t s; } memo[16] = { 0 };          /* the sprite's few colours lit once a frame, not a pixel at a time */
    for (int j = 0; j < ic->h; j++)
        for (int i = 0; i < ic->w; i++) {
            const int a = ic->a[j * ic->w + i];
            if (!a) continue;
            const uint16_t v = ic->rgb[j * ic->w + i];
            const int k = (v ^ v >> 5 ^ v >> 11) & 15;
            if (!memo[k].ok || memo[k].v != v) {
                const uint32_t rgb = (uint32_t)((v >> 11) << 3) << 16 | (uint32_t)(((v >> 5) & 63) << 2) << 8 | (uint32_t)((v & 31) << 3);
                memo[k].v = v; memo[k].ok = true; memo[k].s = src_color(ts_lit(rgb, glow, !lit), c->dim);
            }
            px_blend_s(c, x0 + i, y0 + j, &memo[k].s, a);
        }
    box->x0 = (short)(x0 - 1); box->y0 = (short)(y0 - 1); box->x1 = (short)(x0 + ic->w + 1); box->y1 = (short)(fy + 2);
}
/* the mealworm dish (tank.h THE CRICKETS): a shallow clay dish on the sand, HABITAT_DISH_HW either side of its middle,
   its worms wriggling in it - as many as it holds */
static void ts_dish(ctx_t *c, const tank_t *t, const habitat_t *h, bool lit, ts_rect_t *box) {
    float x, y; tank_dish_point(t, &x, &y);
    const float hw = HABITAT_DISH_HW, a = tank_anim(t->clock);
    src_t sh = src_color(0x000000, 1.0f);
    for (int r = -1; r <= 1; r++) span(c, (int)(x - hw - 2 + 2 * fabsf((float)r)), (int)(x + hw + 2 - 2 * fabsf((float)r)), (int)y + 1 + r, &sh, 60);   /* its shadow */
    const float glow = ts_glow_at(h, lit, x, y);
    fill_ellipse(c, x, y - 2.5f, hw, 4.5f, ts_lit(0x7a4026, glow, !lit), 255);   /* the body, the bowl in it, its lit rim */
    fill_ellipse(c, x, y - 4.0f, hw - 1.5f, 3.0f, ts_lit(0xb06a42, glow, !lit), 255);
    fill_ellipse(c, x, y - 4.0f, hw - 3.0f, 2.0f, ts_lit(0x4a2618, glow, !lit), 255);
    const uint32_t worm[2] = { ts_lit(0xf0cc78, glow, !lit), ts_lit(0xd8a850, glow, !lit) };
    for (int w = 0; w < t->dish_worms && w < DISH_WORMS; w++) {       /* the worms: short, segmented, each its own wriggle */
        const uint32_t hh = (uint32_t)(w + 1) * 2654435761u;
        const float wx = x - hw + 5 + (float)(hh % (uint32_t)(2 * hw - 9)), wy = y - 4.4f + 0.8f * (float)((hh >> 8) % 3) - 0.8f;
        const float ph = a * (2.0f + (float)((hh >> 12) & 3) * 0.4f) + w;
        for (int s = 0; s < 4; s++) px_blend(c, (int)(wx + (s - 1.5f) * 1.2f), (int)(wy + 0.7f * sinf(ph + s * 1.3f)), worm[s & 1], 255);
    }
    box->x0 = (short)(x - hw - 3); box->y0 = (short)(y - 9); box->x1 = (short)(x + hw + 3); box->y1 = (short)(y + 3);
}
/* the mister in hand on the glass: a fine spray at the finger, drifting down - while the finger is down (tank_touch_drag
   each frame it is: a stroke's drag_has_prev outlives the tick, hold_active is never the mister's) */
static bool ts_misting(const tank_t *t) { return t->tool == TOOL_MISTER && t->drag_has_prev; }
static void ts_mist(ctx_t *c, const tank_t *t, ts_rect_t *box) {
    const float mx = t->drag_px, my = t->drag_py, a = tank_anim(t->clock);
    const src_t s = src_color(0xe6f6ff, c->dim);
    fill_ellipse(c, mx, my + 7, 9, 6, 0xdcf0ff, 46);                  /* the puff at the nozzle, a soft haze under it */
    fill_ellipse(c, mx, my + 16, 15, 9, 0xd0ecff, 24);
    for (int k = 0; k < 44; k++) {                                     /* the droplets: a cone down from the finger, each on its own beat */
        const uint32_t hh = (uint32_t)(k + 1) * 2654435761u;
        const float ph = fmodf(a * (1.1f + (float)(hh >> 28) * 0.06f) + (float)(hh & 1023) / 1023.0f, 1.0f);
        const float ang = ((float)((hh >> 10) & 255) / 255.0f - 0.5f) * 1.5f, r = 3 + 30 * ph;
        const int x = (int)floorf(mx + sinf(ang) * r + 0.5f), y = (int)floorf(my + 2 + cosf(ang) * r * 0.85f + 7 * ph * ph + 0.5f);
        const int al = (int)(235 * (1 - ph));
        px_blend_s(c, x, y, &s, al);
        if ((hh >> 20 & 3) == 0 && ph < 0.6f) { px_blend_s(c, x + 1, y, &s, al * 3 / 4); px_blend_s(c, x, y + 1, &s, al * 3 / 4); }   /* (a few bigger drops) */
    }
    box->x0 = (short)(mx - 34); box->y0 = (short)(my - 2); box->x1 = (short)(mx + 34); box->y1 = (short)(my + 42);
}
/* the vignette again on what moved (THE SCENE CACHE's rule: the scene carries it; a drawn pixel takes it once - its mark
   cleared as it does, so two boxes over one pixel darken it once) */
static void ts_vignette(ctx_t *c, const ts_rect_t *r, int n) {
    if (!g_dirty) return;
    for (int i = 0; i < n; i++) {
        const int x0 = r[i].x0 < 0 ? 0 : r[i].x0, x1 = r[i].x1 > TANK_W - 1 ? TANK_W - 1 : r[i].x1;
        const int y0 = r[i].y0 < 0 ? 0 : r[i].y0, y1 = r[i].y1 > TANK_H - 1 ? TANK_H - 1 : r[i].y1;
        for (int y = y0; y <= y1; y++) {
            uint32_t *drow = g_dirty + y * DIRTY_WORDS_PER_ROW;
            for (int x = x0; x <= x1; x++) {
                const uint32_t w = drow[x >> 5];
                if (!w) { x |= 31; continue; }
                if (!((w >> (x & 31)) & 1u)) continue;
                drow[x >> 5] &= ~(1u << (x & 31));
                const int a = g_vig && g_vig_filled ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                if (a) px_darken(&c->fb[y * c->stride + x], a);
            }
        }
    }
}
static double g_ts_clock = -1; static int g_ts_left = 1;   /* the tank clock last drawn, and its builds left (render_terrarium) */
void terrarium_render(const tank_t *t, uint16_t *fb, int stride) {
    const habitat_t *h = ts_habitat(t);                                /* (the bulb's wider pool, if bought) */
    int64_t p0 = PROF_MARK();
    const bool tick = t->clock != g_ts_clock;                         /* (a new tank tick: the bake's slice and a body build are its) */
    g_ts_clock = t->clock;
    const bool cached = g_scene && g_dirty && stride == TANK_W;
    g_dirty_hold = false;
    if (cached) memset(g_dirty, 0, TANK_H * DIRTY_WORDS_PER_ROW * sizeof(uint32_t));
    if (g_touched) memset(g_touched, 0, TANK_H * DIRTY_WORDS_PER_ROW * sizeof(uint32_t));
    ts_layout(h);
    bool lit = !t->night;
    if (cached) {
        ts_scene(t, h, tick);
        lit = g_ts_show.lit;                                           /* (the light the glass shows: a switch waits for its bake) */
        if (!(g_primed_fb == fb && g_primed_epoch == g_scene_epoch)) memcpy(fb, g_scene, (size_t)TANK_W * TANK_H * sizeof(uint16_t));
        else if (g_ts_patch_n > 0)                                     /* (primed with the scene as it was: the moisture step's box from it) */
            for (int y = g_ts_patch.y0; y <= g_ts_patch.y1; y++)
                memcpy(fb + (size_t)y * TANK_W + g_ts_patch.x0, g_scene + (size_t)y * TANK_W + g_ts_patch.x0, (size_t)(g_ts_patch.x1 - g_ts_patch.x0 + 1) * sizeof(uint16_t));
        if (g_ts_patch_n > 0) g_ts_patch_n--;
    } else {                                                           /* no cache (the tests' bare path): the scene drawn whole */
        ts_vig();
        float m[HABITAT_HIDES_MAX];
        for (int i = 0; i < HABITAT_HIDES_MAX; i++) m[i] = i < h->n_hides && h->hides[i].kind == HIDE_HUMID ? (float)scene_desert_moist_step(t->moist[i]) / 8.0f : 0;
        const glass_t g = glass_here();
        scene_desert_draw(fb, stride, &g, h, lit, m, lit ? 1.0f : 0.45f, g_vig && g_vig_filled ? g_vig : NULL);
        g_ts_ctx.ok = false;
    }
    g_primed_fb = NULL;
    const float dim = lit ? 1.0f : 0.45f;
    ctx_t c = ctx_full(fb, stride, dim);
    PROF_ADD(0, p0);
    ts_rect_t rects[TS_RECTS]; int nr = 0;
    /* back to front by the feet's y: the dish, the crickets about, the lizards (a lizard's feet: its middle + TERRAIN_FEET_DY) */
    uint16_t who[N_FISH_SLOTS + MAX_CRICKETS + 1 + TERR_DECO_N]; float at[N_FISH_SLOTS + MAX_CRICKETS + 1 + TERR_DECO_N]; int n = 0;
    for (int i = 0; i < N_FISH_SLOTS; i++) if (tank_swims(t, i)) { who[n] = (uint16_t)i; at[n] = t->fish[i].y + TERRAIN_FEET_DY; n++; }
    for (int k = 0; k < MAX_CRICKETS; k++) {
        const cricket_t *q = &t->cricket[k];
        if (q->state == CK_NONE || q->state == CK_HIDE) continue;     /* (in cover: out of sight) */
        who[n] = (uint16_t)(0x100 | k); at[n] = q->y; n++;
    }
    if (tank_dish_here(t)) { float dx; who[n] = 0x200; tank_dish_point(t, &dx, &at[n]); n++; }
    { static const int8_t DRAW[TERR_DECO_N] = TERR_DECO_DRAW;          /* the decorations: behind the walking band's feet (TERR_DECO_FY), the
                                                                          taller behind the lower (the sort keeps their order) */
      for (int k = 0; k < TERR_DECO_N; k++)
          if (tank_decor_in(t, TERR_DECO_ITEM0 + DRAW[k])) { who[n] = (uint16_t)(0x400 | DRAW[k]); at[n] = TERR_DECO_FY; n++; } }
    for (int a = 1; a < n; a++) {                                      /* (an insertion: a dozen at most) */
        const uint16_t w = who[a]; const float y = at[a]; int b = a;
        while (b > 0 && at[b - 1] > y) { who[b] = who[b - 1]; at[b] = at[b - 1]; b--; }
        who[b] = w; at[b] = y;
    }
    lizard_frame(tick ? 1 : g_ts_left);                                /* a body built a tank tick at most (render_lizard.h); the same tick drawn
                                                                          again builds no more, so its lizards fall back alike */
    for (int q = 0; q < n && nr < TS_RECTS; q++) {
        if (who[q] & 0x400) { ts_deco(&c, t, who[q] & 0xff, h, lit, &rects[nr]); nr++; PROF_ADD(3, p0); }
        else if (who[q] == 0x200) { ts_dish(&c, t, h, lit, &rects[nr]); nr++; PROF_ADD(3, p0); }
        else if (who[q] & 0x100) { ts_cricket(&c, &t->cricket[who[q] & 0xff], h, lit, &rects[nr]); nr++; PROF_ADD(3, p0); }
        else { if (ts_lizard(&c, t, who[q], h, lit, dim, &rects[nr])) nr++; PROF_ADD(7, p0); }
    }
    g_ts_left = lizard_frame(-1);
    if (ts_misting(t) && nr < TS_RECTS) { ts_mist(&c, t, &rects[nr]); nr++; }
    PROF_ADD(4, p0);
    if (cached) ts_vignette(&c, rects, nr);
    PROF_ADD(5, p0);
    draw_algae(&c, t);                                                 /* the film on the glass (the sponge's) */
    PROF_ADD(6, p0);
}
