/* sheet_lizard.c - `fishsim --lizard-sheet <prefix>` (pocket-terrarium P3a, P2c): the lizard and the desert, standalone.
 *
 * Writes PPM contact sheets at 1x (the screen's own pixels; each lizard on a strip of the desert's wall and sand):
 *   <prefix>_morphs.ppm   every morph (rows) x stage (columns), standing
 *   <prefix>_poses.ppm    every pose (tank.h LZ_POSE_*), an adult NORMAL; then facing left; then a turn on the spot
 *                         from right to left in nine steps (the tail a beat behind)
 *   <prefix>_motion.ppm   the walk's cycle in eight frames (an adult, a hatchling); the blink; the tongue; the fat
 *                         reserve; the dig's scrabble; the lunge; the stalking tail tip; the standoff's tail; the breath
 *   <prefix>_shed.ppm     the shed: normal, soon, shedding (three steps), stuck, clean - an adult and a juvenile
 *   <prefix>_scene_<glass>.ppm and _night: the desert on each glass (rect, round, watch, square) with its terraces, its
 *                         shelf and its ramps (terrain.h), geckos on every level - basking on the high rock under the
 *                         lamp, flicking on the shelf, walking the low ledge, peeking out of the humid box, walking the
 *                         floor, a hatchling by the warm cave; dark, asleep up on the rocks and on the floor, one hidden
 *   <prefix>_moist.ppm    the humid box at moisture 0, 0.25, 0.5, 0.75, 1
 * checks the ground as drawn against the engine's (every point of every rock's and ramp's walking line stands on that
 * piece's own top, on all four glasses: the exit status), and measures the cost (<prefix>_cost.txt, and stdout): a
 * body's build per pose, a cached draw, three lizards living a minute, the scene's bake per glass - in microseconds on
 * this machine, and scaled to the board by the --bench convention (the stage ratios carry over): the urchin's per-pixel
 * spines as render.c drew them before 2026-10-03 cost 1.8 ms a frame on the 1.8 (PLAN.md R5), and are timed here beside
 * the lizard. */
#include "render_lizard.h"
#include "render_scene_desert.h"
#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t sl_now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void sl_ppm(const char *path, const uint16_t *fb, int w, int h, int stride) {
    FILE *f = fopen(path, "wb"); if (!f) { printf("lizard-sheet: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t p = FB_SWAP16(fb[(size_t)y * stride + x]);
            unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    printf("lizard-sheet: %s (%dx%d)\n", path, w, h);
}

/* ---- a sheet: a buffer with room under it for the font's window (render_text clips to a frame from its origin) ---- */
#define SH_W 1240
#define SH_H 760
static uint16_t g_sheet[(SH_H + TANK_H) * SH_W];
static int g_sw, g_sh;
static void sh_begin(int w, int h) {
    g_sw = w; g_sh = h;
    uint16_t bg = FB_SWAP16((uint16_t)(((0x16 >> 3) << 11) | ((0x14 >> 2) << 5) | (0x18 >> 3)));
    for (int i = 0; i < SH_W * (SH_H + TANK_H); i++) g_sheet[i] = bg;
}
static void sh_label(int x, int y, uint32_t rgb, const char *s) {   /* at sheet (x, y): a page origin there */
    render_text(g_sheet + (size_t)y * SH_W + x, SH_W, -PAGE_X, -PAGE_Y, 1, rgb, s);
}
static lz_canvas_t sh_canvas(void) { lz_canvas_t c = { .fb = g_sheet, .stride = SH_W, .w = g_sw, .h = g_sh, .dim = 1.0f }; return c; }

/* the cells' backdrop: the desert's wall and sand without its hides or its ground's rocks (the lamp off-glass), lit */
static uint16_t g_strip[TANK_W * TANK_H];
static float g_strip_sand;
static void strip_make(void) {
    static uint16_t fb[480 * 480];
    habitat_t h = *habitat_get(HABITAT_DESERT); h.n_hides = 0; h.n_rocks = 0; h.n_ramps = 0; h.bask_surf = 0; h.bask_u = 3.0f;
    glass_t b = glass_of(0);
    scene_desert_draw(fb, b.w, &b, &h, true, NULL, 1.0f, NULL);
    for (int y = 0; y < TANK_H && y < b.h; y++) for (int x = 0; x < TANK_W && x < b.w; x++) g_strip[y * TANK_W + x] = fb[y * b.w + x];
    g_strip_sand = glass_place_y(&b) + TERRAIN_FEET_DY;
}
/* a cell at (x, y) w x h whose sand line (the feet in the middle lane) is `foot` px from its top */
static void sh_cell(int x, int y, int w, int h, int foot) {
    const int sy0 = (int)g_strip_sand - foot;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = 24 + i, sy = sy0 + j;
            if (sy < 0) sy = 0;
            if (sy >= TANK_H) sy = TANK_H - 1;
            if (x + i < g_sw && y + j < g_sh) g_sheet[(size_t)(y + j) * SH_W + x + i] = g_strip[sy * TANK_W + (sx < TANK_W ? sx : TANK_W - 1)];
        }
}

static lizard_look_t sl_look(int morph, int stage, uint32_t seed) {
    const species_t *sp = species_get(SPECIES_GECKO);
    lizard_look_t lk = { &sp->geom, &sp->morphs[morph % sp->n_morphs], &sp->pattern[stage & 3], (uint8_t)stage, seed };
    return lk;
}
static const char *const STAGE_NAME[4] = { "HATCHLING", "JUVENILE", "ADULT", "ELDER" };
static const char *const POSE_NAME[LZ_POSES] = { "STAND", "WALK", "TURN", "FLICK", "STALK", "LUNGE", "BASK", "DIG", "REST", "HIDDEN", "PEEK", "STANDOFF" };

/* one lizard in a cell, its middle at the cell's middle (+ dx), its feet on the cell's sand line */
static void sh_lizard(int cx, int cy, int cw, int ch, int foot, const lizard_look_t *lk, const lizard_pose_t *p, float scale, float dx) {
    sh_cell(cx, cy, cw, ch, foot);
    lz_canvas_t cv = sh_canvas();                                               /* the cell's window: clipped to it */
    cv.fb = g_sheet + (size_t)cy * SH_W + cx; cv.ox = cx; cv.oy = cy; cv.w = cw; cv.h = ch;
    draw_lizard(&cv, lk, p, cx + cw * 0.5f + dx, (float)(cy + foot), scale);
}

/* ---- the sheets ---- */
static void sheet_morphs(const char *prefix) {
    const int CW = 136, CH = 62, FOOT = 46, LX = 78, TY = 16;
    const species_t *sp = species_get(SPECIES_GECKO);
    sh_begin(LX + 4 * CW + 4, TY + sp->n_morphs * CH + 4);
    for (int s = 0; s < 4; s++) sh_label(LX + s * CW + 4, 4, 0xe8e0d0, STAGE_NAME[s]);
    for (int m = 0; m < sp->n_morphs; m++) {
        sh_label(4, TY + m * CH + CH / 2 - 4, 0xe8e0d0, sp->morphs[m].name);
        for (int s = 0; s < 4; s++) {
            lizard_look_t lk = sl_look(m, s, 0x1234u + (uint32_t)m * 77u);
            lizard_pose_t p = lizard_pose(LZ_POSE_STAND, 1);
            p.fat = 0.6f;
            sh_lizard(LX + s * CW, TY + m * CH, CW - 2, CH - 2, FOOT, &lk, &p, lizard_stage_scale(s), 0);
        }
    }
    char path[512]; snprintf(path, sizeof path, "%s_morphs.ppm", prefix);
    sl_ppm(path, g_sheet, g_sw, g_sh, SH_W);
}

static void sheet_poses(const char *prefix) {
    const int CW = 150, CH = 66, FOOT = 50, TY = 14, COLS = 6;
    sh_begin(COLS * CW + 4, TY + 3 * (CH + 12) + CH + 26);
    lizard_look_t lk = sl_look(0, STAGE_ADULT, 0x5eedu);
    int n = 0;
    for (int k = 0; k < LZ_POSES; k++, n++) {
        const int cx = (n % COLS) * CW + 2, cy = TY + (n / COLS) * (CH + 12);
        lizard_pose_t p = lizard_pose(k, 1); p.fat = 0.6f;
        if (k == LZ_POSE_WALK) p.phase = 0.25f;
        if (k == LZ_POSE_TURN) { p.yaw = 0.30f; p.yaw_tail = 0.80f; p.phase = 0.3f; }
        if (k == LZ_POSE_FLICK) p.tongue = 1;
        if (k == LZ_POSE_LUNGE) p.phase = 1;
        if (k == LZ_POSE_DIG) p.phase = 0.3f;
        if (k == LZ_POSE_STALK) p.clock = 0.1f;
        if (k == LZ_POSE_PEEK) {                                                /* out of a door's dark */
            sh_cell(cx, cy, CW - 2, CH - 2, FOOT);
            for (int y = cy + FOOT - 20; y < cy + FOOT; y++) for (int x = cx + 30; x < cx + 58; x++) g_sheet[(size_t)y * SH_W + x] = FB_SWAP16(0x0841);
            lz_canvas_t cv = sh_canvas(); cv.fb = g_sheet + (size_t)cy * SH_W + cx; cv.ox = cx; cv.oy = cy; cv.w = CW - 2; cv.h = CH - 2;
            float hl = lizard_half_len(lk.geom, 1.0f);
            draw_lizard(&cv, &lk, &p, cx + 48 - hl * (1 - 2 * p.peek), (float)(cy + FOOT), 1.0f);
        } else sh_lizard(cx, cy, CW - 2, CH - 2, FOOT, &lk, &p, 1.0f, 0);
        sh_label(cx + 2, cy + CH - 1, 0xe8e0d0, POSE_NAME[k]);
    }
    /* facing left */
    static const int LEFT[6] = { LZ_POSE_STAND, LZ_POSE_WALK, LZ_POSE_STALK, LZ_POSE_LUNGE, LZ_POSE_BASK, LZ_POSE_DIG };
    n = 12;
    for (int i = 0; i < 6; i++, n++) {
        const int cx = (n % COLS) * CW + 2, cy = TY + (n / COLS) * (CH + 12);
        lizard_pose_t p = lizard_pose(LEFT[i], -1); p.fat = 0.6f; p.phase = LEFT[i] == LZ_POSE_LUNGE ? 0.66f : 0.6f;
        sh_lizard(cx, cy, CW - 2, CH - 2, FOOT, &lk, &p, 1.0f, 0);
        char lab[32]; snprintf(lab, sizeof lab, "%s LEFT", POSE_NAME[LEFT[i]]); sh_label(cx + 2, cy + CH - 1, 0xe8e0d0, lab);
    }
    /* a turn on the spot, right to left: the head leads, the tail a beat behind */
    const int TW = 100, ty = TY + 3 * (CH + 12);
    for (int i = 0; i < 9; i++) {
        float yh = 1 - 2 * (float)i / 8, yt = yh + 0.45f > 1 ? 1 : yh + 0.45f;
        if (i == 8) yt = -1;
        lizard_pose_t p = lizard_pose(LZ_POSE_TURN, yh); p.yaw_tail = yt; p.fat = 0.6f; p.phase = i * 0.125f;
        sh_lizard(2 + i * TW, ty, TW - 2, CH + 10, FOOT + 6, &lk, &p, 1.0f, 0);
    }
    sh_label(4, ty + CH + 12, 0xe8e0d0, "TURN ON THE SPOT: YAW 1 TO -1, THE TAIL A BEAT BEHIND");
    char path[512]; snprintf(path, sizeof path, "%s_poses.ppm", prefix);
    sl_ppm(path, g_sheet, g_sw, g_sh, SH_W);
}

static void sheet_motion(const char *prefix) {
    const int CW = 112, CH = 60, FOOT = 46, LX = 70, ROWS = 10;
    sh_begin(LX + 8 * CW + 4, ROWS * CH + 6);
    lizard_look_t ad = sl_look(0, STAGE_ADULT, 0xabcu), ha = sl_look(0, STAGE_FRY, 0xabcu);
    for (int r = 0; r < ROWS; r++) {
        static const char *const ROW[10] = { "WALK", "WALK HATCH", "BLINK", "TONGUE", "FAT", "DIG", "LUNGE", "STALK TIP", "STANDOFF", "REST BREATH" };
        sh_label(4, r * CH + CH / 2 - 4, 0xe8e0d0, ROW[r]);
        int n = r == 0 || r == 1 || r == 5 ? 8 : r == 7 || r == 8 ? 6 : r == 9 ? 3 : 4;
        for (int i = 0; i < n; i++) {
            lizard_pose_t p = lizard_pose(LZ_POSE_STAND, 1); p.fat = 0.6f;
            const lizard_look_t *lk = &ad; float sc = 1.0f;
            switch (r) {
            case 0: p.kind = LZ_POSE_WALK; p.phase = i / 8.0f; break;
            case 1: p.kind = LZ_POSE_WALK; p.phase = i / 8.0f; lk = &ha; sc = lizard_stage_scale(STAGE_FRY); break;
            case 2: p.blink = i / 3.0f; break;
            case 3: p.kind = LZ_POSE_FLICK; p.tongue = i / 3.0f; break;
            case 4: p.fat = i / 3.0f; break;
            case 5: p.kind = LZ_POSE_DIG; p.phase = i / 8.0f; break;
            case 6: p.kind = LZ_POSE_LUNGE; p.phase = i / 3.0f; break;
            case 7: p.kind = LZ_POSE_STALK; p.clock = i / (6.0f * 2.2f) + 0.01f; break;
            case 8: p.kind = LZ_POSE_STANDOFF; p.clock = i / (6.0f * 0.7f) + 0.01f; break;
            case 9: p.kind = LZ_POSE_REST; p.clock = i == 0 ? 3.75f : i == 1 ? 0.0f : 1.25f; break;
            }
            sh_lizard(LX + i * CW, r * CH + 2, CW - 2, CH - 2, FOOT, lk, &p, sc, 0);
        }
    }
    char path[512]; snprintf(path, sizeof path, "%s_motion.ppm", prefix);
    sl_ppm(path, g_sheet, g_sw, g_sh, SH_W);
}

static void sheet_shed(const char *prefix) {
    const int CW = 130, CH = 62, FOOT = 46, LX = 70, TY = 16;
    static const char *const COL[7] = { "NORMAL", "SOON", "SHEDDING .15", "SHEDDING .45", "SHEDDING .8", "STUCK", "CLEAN" };
    sh_begin(LX + 7 * CW + 4, TY + 3 * CH + 4);
    for (int c = 0; c < 7; c++) sh_label(LX + c * CW + 4, 4, 0xe8e0d0, COL[c]);
    static const struct { int morph, stage; const char *name; } ROW[3] = { { 0, STAGE_ADULT, "ADULT" }, { 1, STAGE_JUV, "JUVENILE" }, { 4, STAGE_ADULT, "MACK SNOW" } };
    for (int r = 0; r < 3; r++) {
        sh_label(4, TY + r * CH + CH / 2 - 4, 0xe8e0d0, ROW[r].name);
        lizard_look_t lk = sl_look(ROW[r].morph, ROW[r].stage, 0x77u + (uint32_t)r);
        for (int c = 0; c < 7; c++) {
            lizard_pose_t p = lizard_pose(LZ_POSE_STAND, 1); p.fat = 0.6f;
            static const uint8_t SHED[7] = { SHED_NORMAL, SHED_SOON, SHED_SHEDDING, SHED_SHEDDING, SHED_SHEDDING, SHED_STUCK, SHED_CLEAN };
            static const float K[7] = { 0, 0, 0.15f, 0.45f, 0.8f, 0, 0 };
            p.shed = SHED[c]; p.shed_k = K[c];
            sh_lizard(LX + c * CW, TY + r * CH, CW - 2, CH - 2, FOOT, &lk, &p, lizard_stage_scale(ROW[r].stage), 0);
        }
    }
    char path[512]; snprintf(path, sizeof path, "%s_shed.ppm", prefix);
    sl_ppm(path, g_sheet, g_sw, g_sh, SH_W);
}

/* the scene on glass b with geckos on every level (lit: basking on the high rock, flicking on the shelf, walking along the
   low ledge, peeking out of the humid box, walking the floor, a hatchling by the warm cave; dark: asleep up on the high
   rock and on the shelf, one hidden, one resting on the floor) - each one's feet on its path's line + TERRAIN_FEET_DY */
static uint16_t g_frame[502 * 480];
static uint8_t  g_vig[502 * 480];
static void vig_fill(const glass_t *b) {                                        /* render.c vig_alpha on glass b */
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            float dx = (x - b->w * 0.5f) * (2.0f / b->w), dy = (y - b->h * 0.5f) * (2.0f / b->h), d2 = dx * dx + dy * dy;
            int a = d2 <= 0.72f ? 0 : (int)((d2 - 0.72f) * 220);
            g_vig[y * b->w + x] = (uint8_t)(a > 255 ? 255 : a);
        }
}
/* a gecko standing on path s at share k of its walking line (0 its left end .. 1 its right) */
static void gecko_on(const lz_canvas_t *cv, const scene_desert_t *d, int s, float k, const lizard_look_t *lk, const lizard_pose_t *p, float scale) {
    const terrain_path_t *q = &d->tr.path[s];
    const float x = q->x[0] + (q->x[q->n - 1] - q->x[0]) * k;
    draw_lizard(cv, lk, p, x, terrain_y(q, x) + TERRAIN_FEET_DY, scale);
}
static void scene_with_geckos(const glass_t *b, bool lit, const float *moist, float clock) {
    const habitat_t *h = habitat_get(HABITAT_DESERT);
    vig_fill(b);
    scene_desert_draw(g_frame, b->w, b, h, lit, moist, lit ? 1.0f : 0.45f, g_vig);
    scene_desert_t d; scene_desert_layout(b, h, &d);
    lz_canvas_t cv = { .fb = g_frame, .stride = b->w, .w = b->w, .h = b->h, .dim = lit ? 1.0f : 0.45f, .moon = !lit };
    int other = 0, low = 0;                                                     /* a shelf (else a second rock), and another rock: off the basking one */
    for (int k = 0; k < d.n_rocks; k++) if (d.rock[k].shelf && d.rock[k].path != d.tr.bask_path) other = d.rock[k].path;
    for (int s = 1; s <= d.tr.n_rocks; s++) if (s != d.tr.bask_path && s != other) { if (!low) low = s; else if (!other) other = s; }
    /* one on the high rock at the basking spot, basking (or asleep there) */
    lizard_look_t a = sl_look(0, STAGE_ADULT, 0x1001u);
    lizard_pose_t pa = lizard_pose(lit ? LZ_POSE_BASK : LZ_POSE_REST, 1); pa.fat = 0.7f; pa.clock = clock;
    draw_lizard(&cv, &a, &pa, d.bask_x - 6, d.bask_y + TERRAIN_FEET_DY, 1.0f);
    /* one on the shelf, flicking its tongue (dark: asleep there) */
    if (other) {
        lizard_look_t o = sl_look(1, STAGE_ADULT, 0x5005u);
        lizard_pose_t po = lizard_pose(lit ? LZ_POSE_FLICK : LZ_POSE_REST, -1); po.tongue = 0.7f; po.fat = 0.55f; po.clock = clock;
        gecko_on(&cv, &d, other, 0.62f, &o, &po, 0.95f);
    }
    /* a juvenile walking along the low ledge (lit only) */
    if (low && lit) {
        lizard_look_t l = sl_look(3, STAGE_JUV, 0x6006u);
        lizard_pose_t pl = lizard_pose(LZ_POSE_WALK, -1); pl.phase = 0.6f; pl.clock = clock;
        gecko_on(&cv, &d, low, 0.45f, &l, &pl, lizard_stage_scale(STAGE_JUV));
    }
    /* one peeking out of the humid box (dark: in it, hidden) */
    lizard_look_t j = sl_look(1, STAGE_JUV, 0x2002u);
    lizard_pose_t pj = lizard_pose(lit ? LZ_POSE_PEEK : LZ_POSE_HIDDEN, 1); pj.peek = 0.30f;
    int hk = -1; for (int k = 0; k < d.n_hides; k++) if (d.hide[k].kind == HIDE_HUMID) hk = k;
    if (hk >= 0) {
        float px, py, yaw; const float sc = lizard_stage_scale(STAGE_JUV);
        scene_desert_peek_at(&d, hk, lizard_half_len(j.geom, sc), pj.peek, &px, &py, &yaw);
        pj.yaw = pj.yaw_tail = yaw;
        draw_lizard(&cv, &j, &pj, px, py, sc);
    }
    /* one walking the floor's front lane toward the warm end (dark: resting by the cool cave) */
    lizard_look_t m = sl_look(4, STAGE_ADULT, 0x3003u);
    lizard_pose_t pm = lizard_pose(lit ? LZ_POSE_WALK : LZ_POSE_REST, -1); pm.phase = 0.3f; pm.fat = 0.45f; pm.clock = clock;
    float mx = 0.5f * (d.hide[1].cx + d.hide[2].cx) + 8;
    draw_lizard(&cv, &m, &pm, mx, d.sand_y + LZ_LANE_FRONT, 1.0f);
    /* a hatchling in the back lane by the warm cave, flicking its tongue */
    if (lit) {
        lizard_look_t hl = sl_look(2, STAGE_FRY, 0x4004u);
        lizard_pose_t ph = lizard_pose(LZ_POSE_FLICK, 1); ph.tongue = 0.8f;
        draw_lizard(&cv, &hl, &ph, d.hide[0].cx + d.hide[0].half_w + 18, d.sand_y + LZ_LANE_BACK, lizard_stage_scale(STAGE_FRY));
    }
}
static void sheet_scenes(const char *prefix) {
    char path[512];
    for (int k = 0; k < GLASS_BOARDS; k++) {
        glass_t b = glass_of(k);
        for (int lit = 1; lit >= 0; lit--) {
            scene_with_geckos(&b, lit != 0, NULL, 2.0f);
            snprintf(path, sizeof path, "%s_scene_%s%s.ppm", prefix, b.name, lit ? "" : "_night");
            sl_ppm(path, g_frame, b.w, b.h, b.w);
        }
    }
    /* the humid box, moisture 0 .. 1 */
    glass_t b = glass_of(0);
    const habitat_t *h = habitat_get(HABITAT_DESERT);
    scene_desert_t d; scene_desert_layout(&b, h, &d);
    int hk = 1; for (int k = 0; k < d.n_hides; k++) if (d.hide[k].kind == HIDE_HUMID) hk = k;
    const int cw = 96, ch = 60;
    sh_begin(5 * cw + 4, ch + 18);
    for (int i = 0; i < 5; i++) {
        float mo[HABITAT_HIDES_MAX] = { 0.6f, 0.6f, 0.6f, 0.6f }; mo[hk] = i / 4.0f;
        scene_desert_draw(g_frame, b.w, &b, h, true, mo, 1.0f, NULL);
        const int x0 = (int)d.hide[hk].cx - cw / 2, y0 = (int)d.hide[hk].base_y - ch + 14;
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) g_sheet[(size_t)y * SH_W + 2 + i * cw + x] = g_frame[(y0 + y) * b.w + x0 + x];
        char lab[16]; snprintf(lab, sizeof lab, "MOIST %.2f", i / 4.0f); sh_label(4 + i * cw, ch + 6, 0xe8e0d0, lab);
    }
    snprintf(path, sizeof path, "%s_moist.ppm", prefix);
    sl_ppm(path, g_sheet, g_sw, g_sh, SH_W);
}
/* the ground drawn where the engine walks: on every glass, under every point of every rock's and ramp's walking line, the
   feet stand on that path's own top as the scene draws it (or, by a ramp's end, on the piece it lands on); the misses */
static int ground_misses(void) {
    int bad = 0, pts = 0;
    for (int k = 0; k < GLASS_BOARDS; k++) {
        glass_t b = glass_of(k);
        static scene_desert_t d; scene_desert_layout(&b, habitat_get(HABITAT_DESERT), &d);
        for (int s = 1; s < d.tr.n_paths; s++) {
            const terrain_path_t *q = &d.tr.path[s];
            for (float x = q->x[0]; x <= q->x[q->n - 1]; x += 1) {
                const int at = scene_desert_ground_at(&d, x, terrain_y(q, x) + TERRAIN_FEET_DY + 1);
                bool ok = at == s;
                for (int l = 0; l < d.tr.n_links && !ok; l++) {
                    const terrain_link_t *n = &d.tr.link[l];
                    ok = fabsf(n->x - x) <= TERRAIN_JUNCTION_PX / 3 && ((n->a == s && n->b == at) || (n->b == s && n->a == at));
                }
                if (!ok && bad++ < 8) printf("lizard-sheet: on the %s, path %d at x %.0f: the feet stand on piece %d as drawn\n", b.name, s, x, at);
                pts++;
            }
        }
    }
    printf("lizard-sheet: the ground as drawn under the engine's walking lines, all four glasses: %d of %d points on their own top%s\n", pts - bad, pts, bad ? " - MISSES" : "");
    return bad;
}

/* ---- the cost ---- */
/* the urchin's spines as render.c drew them before 2026-10-03 (draw_urchin's tone per pixel, each crown spine's rim,
   end and taper - a sqrtf and two divides - per pixel): the 1.8 ms a frame on the 1.8 of PLAN.md R5. The --bench
   convention: the stage ratios carry over from this machine to the board, so lizard_us / this_us x 1.8 ms */
static volatile int g_sink;
static int ur_tone_old(float lx, float ly, const float *dx, const float *dy, const float *len) {
    const float RX = 10, RY = 8, BASE = 7;
    if (ly > BASE) return 6;
    float lit = (-lx * 0.55f - ly * 0.83f) / RX, ex = lx / RX, ey = ly / RY, er = ex * ex + ey * ey;
    static const float FX[5] = { -4.5f, 0.5f, -5.0f, 2.0f, 5.5f }, FY[5] = { -2.5f, -4.5f, 3.0f, 2.5f, 0.0f };
    for (int k = 11; k < 16; k++) {
        float vx = lx - FX[k - 11], vy = ly - FY[k - 11], u = vx * dx[k] + vy * dy[k], w = vx * dy[k] - vy * dx[k];
        if (u < 0 || u > len[k]) continue;
        float half = 1.6f * (1 - u / len[k]) + 0.35f;
        if (w * w > half * half) continue;
        return u > len[k] - 1.6f ? 5 : lit > 0 ? 4 : 3;
    }
    if (er <= 1.0f) {
        int tone = 2;
        if (lit > 0.15f) tone = 3;
        if (lit > 0.55f && er > 0.20f) tone = 4;
        if (lit < -0.30f) tone = 1;
        if (lit < -0.75f || ly > BASE - 1.2f) tone = 0;
        uint32_t h = (uint32_t)((int)floorf(lx) + 40) * 0x9E3779B1u ^ (uint32_t)((int)floorf(ly) + 40) * 0x85EBCA77u; h ^= h >> 15;
        if ((h & 7) == 0) tone = tone > 0 ? tone - 1 : 0; else if ((h & 15) == 1 && tone < 4) tone++;
        return tone;
    }
    for (int k = 0; k < 11; k++) {
        float u = lx * dx[k] + ly * dy[k], w = lx * dy[k] - ly * dx[k];
        float rim = 1.0f / sqrtf(dx[k] * dx[k] / (RX * RX) + dy[k] * dy[k] / (RY * RY)), end = rim + len[k];
        if (u < rim - 2 || u > end) continue;
        float half = 2.6f * (end - u) / (end - rim + 2) + 0.3f;
        if (w * w > half * half) continue;
        float slit = -dx[k] * 0.55f - dy[k] * 0.83f;
        if (u > end - 1.8f) return slit > -0.5f ? 5 : 4;
        if (u > end - 3.8f) return slit > -0.3f ? 4 : 3;
        return slit > 0.2f ? 3 : slit > -0.5f ? 2 : 1;
    }
    return 6;
}
static double urchin_old_us(void) {
    static uint8_t tn[24 * 37];
    const int N = 3000;
    int64_t t0 = sl_now_us();
    for (int f = 0; f < N; f++) {
        float dx[16], dy[16], len[16], clock = f * 0.04f;
        for (int k = 0; k < 11; k++) { float a = -3.14159f - 0.32f + k * (3.14159f + 0.64f) / 10 + 0.07f * sinf(clock * 0.8f + k * 1.7f); dx[k] = cosf(a); dy[k] = sinf(a); len[k] = (k & 1) ? 4.0f : 6.0f; }
        static const float FA[5] = { -2.4f, -1.9f, 2.2f, 1.2f, 0.5f };
        for (int k = 0; k < 5; k++) { float a = FA[k] + 0.04f * sinf(clock * 0.8f + k * 2.3f); dx[11 + k] = cosf(a); dy[11 + k] = sinf(a); len[11 + k] = 4.5f; }
        for (int ly = -16; ly <= 7; ly++) for (int lx = -18; lx <= 18; lx++) tn[(ly + 16) * 37 + lx + 18] = (uint8_t)ur_tone_old(lx + 0.5f, ly + 0.5f, dx, dy, len);
        g_sink += tn[(f * 7) % (24 * 37)];
    }
    return (double)(sl_now_us() - t0) / N;
}

static uint16_t g_bench[TANK_W * TANK_H];
static double bench_draw(const lizard_look_t *lk, lizard_pose_t p, float scale, int n, bool advance, int *builds) {
    lz_canvas_t cv = { .fb = g_bench, .stride = TANK_W, .w = TANK_W, .h = TANK_H, .dim = 1.0f };
    int b0 = lizard_cache_builds();
    int64_t t0 = sl_now_us();
    for (int i = 0; i < n; i++) {
        if (advance) { p.phase = (float)i / 12.0f; p.phase -= floorf(p.phase); p.clock = i / 25.0f; }
        draw_lizard(&cv, lk, &p, 200.0f + (i & 7), 300.0f, scale);
    }
    double us = (double)(sl_now_us() - t0) / n;
    if (builds) *builds = lizard_cache_builds() - b0;
    return us;
}
/* three geckos living a minute at 25 fps: A walks, turning about every 4 s, blinking, flicking its tongue; B basks
   (breathing) then rests; C stalks, lunges, walks, digs. Their shed: B soon, C stuck. budget: lizard_frame's (-1 none).
   The mean a frame (all three) and the 99th percentile frame */
static int cmp_i64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static double bench_life(int budget, int *builds, int *frames, double *p99) {
    static int64_t ft[1500];
    lz_canvas_t cv = { .fb = g_bench, .stride = TANK_W, .w = TANK_W, .h = TANK_H, .dim = 1.0f };
    lizard_look_t A = sl_look(0, STAGE_ADULT, 11), B = sl_look(1, STAGE_JUV, 22), C = sl_look(4, STAGE_ELDER, 33);
    const int N = 1500; int b0 = lizard_cache_builds();
    int64_t total = 0;
    for (int f = 0; f < N; f++) {
        const float t = f / 25.0f;
        lizard_pose_t a = lizard_pose(LZ_POSE_WALK, 1); a.fat = 0.6f; a.clock = t; a.phase = t * 1.8f - floorf(t * 1.8f);
        float ct = fmodf(t, 4.0f);
        if (ct < 0.7f) { float q = ct / 0.7f, s = (((int)(t / 4.0f)) & 1) ? -1.0f : 1.0f; a.kind = LZ_POSE_TURN; a.yaw = s * cosf(3.14159f * q); a.yaw_tail = s * cosf(3.14159f * (q > 0.25f ? q - 0.25f : 0) / 0.75f * 0.9f); }
        else a.yaw = a.yaw_tail = (((int)(t / 4.0f)) & 1) ? 1.0f : -1.0f;
        float bt = fmodf(t, 3.0f); a.blink = bt < 0.12f ? 1 - fabsf(bt - 0.06f) / 0.06f : 0;
        float tt = fmodf(t, 5.0f); a.tongue = tt < 0.25f ? sinf(3.14159f * tt / 0.25f) : 0;
        lizard_pose_t b = lizard_pose(t < 30 ? LZ_POSE_BASK : LZ_POSE_REST, 1); b.fat = 0.5f; b.clock = t; b.shed = SHED_SOON;
        lizard_pose_t c = lizard_pose(LZ_POSE_STALK, -1); c.clock = t; c.shed = SHED_STUCK; c.fat = 0.3f;
        float pc = fmodf(t, 12.0f);
        if (pc < 3) c.kind = LZ_POSE_STALK;
        else if (pc < 3.4f) { c.kind = LZ_POSE_LUNGE; c.phase = (pc - 3) / 0.4f; }
        else if (pc < 8) { c.kind = LZ_POSE_WALK; c.phase = pc * 1.6f - floorf(pc * 1.6f); }
        else { c.kind = LZ_POSE_DIG; c.phase = pc * 3 - floorf(pc * 3); }
        const int64_t t0 = sl_now_us();
        lizard_frame(budget);
        draw_lizard(&cv, &A, &a, 120, 300, 1.0f);
        draw_lizard(&cv, &B, &b, 220, 300, lizard_stage_scale(STAGE_JUV));
        draw_lizard(&cv, &C, &c, 330, 300, lizard_stage_scale(STAGE_ELDER));
        ft[f] = sl_now_us() - t0; total += ft[f];
    }
    lizard_frame(-1);
    qsort(ft, N, sizeof ft[0], cmp_i64);
    *p99 = (double)ft[N * 99 / 100];
    *builds = lizard_cache_builds() - b0; *frames = N;
    return (double)total / N;
}
/* the board's factor (the --bench convention above): its microseconds per this machine's, from the old urchin's spines
   (shot_terrarium.c's --terr-bench reads it too) */
double lizard_board_k(void) {
    urchin_old_us();                                                            /* (a warm-up) */
    return 1800.0 / urchin_old_us();
}

static void cost(const char *prefix) {
    char path[512]; snprintf(path, sizeof path, "%s_cost.txt", prefix);
    FILE *out = fopen(path, "w");
    #define SAY(...) do { printf(__VA_ARGS__); if (out) fprintf(out, __VA_ARGS__); } while (0)
    /* warm up the clocks */
    { lizard_look_t lk = sl_look(0, STAGE_ADULT, 1); bench_draw(&lk, lizard_pose(LZ_POSE_WALK, 1), 1.0f, 2000, true, NULL); urchin_old_us(); }
    const double ur = urchin_old_us(), k = 1800.0 / ur;
    SAY("lizard cost (this machine; the board = x %.0f: the old urchin's per-pixel spines take %.1f us here and 1.8 ms on the 1.8)\n", k, ur);
    lizard_look_t lk = sl_look(0, STAGE_ADULT, 0x99u);
    int builds = 0;
    double hit = bench_draw(&lk, lizard_pose(LZ_POSE_WALK, 1), 1.0f, 20000, true, &builds);
    SAY("  cached draw, an adult walking (legs, eyes live):  %6.2f us  ~%5.0f us on the board   (%d builds in 20000 draws)\n", hit, hit * k, builds);
    lizard_look_t el = sl_look(0, STAGE_ELDER, 0x99u);
    double hit_el = bench_draw(&el, lizard_pose(LZ_POSE_WALK, 1), 1.3f, 20000, true, &builds);
    SAY("  cached draw, an elder at 1.3 walking:             %6.2f us  ~%5.0f us on the board\n", hit_el, hit_el * k);
    lizard_cache_on(false);
    double worst = 0; const char *wname = "";
    for (int p = 0; p < LZ_POSES; p++) {
        if (p == LZ_POSE_HIDDEN) continue;
        lizard_pose_t q = lizard_pose(p, 1); q.fat = 0.6f; q.phase = 0.5f; q.tongue = p == LZ_POSE_FLICK ? 1 : 0;
        double us = bench_draw(&lk, q, 1.0f, 2000, false, NULL);
        if (us > worst) { worst = us; wname = POSE_NAME[p]; }
        SAY("  build + draw, %-9s (cache off):               %6.2f us  ~%5.0f us on the board\n", POSE_NAME[p], us, us * k);
    }
    { lizard_pose_t q = lizard_pose(LZ_POSE_TURN, 0.1f); q.yaw_tail = 0.6f;
      double us = bench_draw(&lk, q, 1.0f, 2000, false, NULL);
      SAY("  build + draw, a turn head-on (cache off):         %6.2f us  ~%5.0f us on the board\n", us, us * k); }
    lizard_cache_on(true);
    int frames = 0; double p99 = 0;
    for (int budget = -1; budget <= 1; budget += 2) {
        double life = bench_life(budget, &builds, &frames, &p99);
        SAY("  three geckos living a minute (walk+turns+blinks+flicks / bask+rest / stalk+lunge+walk+dig), all three a frame, %s:\n",
            budget < 0 ? "no build budget" : "lizard_frame(1)");
        SAY("                                    mean %6.2f us  ~%5.0f us on the board; 99th pct frame %5.0f us ~%5.0f us   (%d builds in %d frames)\n",
            life, life * k, p99, p99 * k, builds, frames);
    }
    {                                                                           /* the grid fits every body: each pose at 1.3 (an elder well fed), side-on and head-on,
                                                                                   level and pitched on the steepest ground a lizard walks, either way */
        lz_canvas_t cv = { .fb = g_bench, .stride = TANK_W, .w = TANK_W, .h = TANK_H, .dim = 1.0f };
        lizard_look_t big = sl_look(0, STAGE_ELDER, 0x77u);
        for (int p = 0; p < LZ_POSES; p++)
            for (int y = 0; y < 3; y++)
                for (int s = -1; s <= 1; s++) {
                    lizard_pose_t q = lizard_pose(p, y == 0 ? 1.0f : y == 1 ? 0.05f : 0.5f); q.fat = 1; q.phase = 1; q.yaw_tail = y == 2 ? -0.6f : q.yaw;
                    q.pitch = s * TERRAIN_SLOPE_MAX * 3.14159265f / 180.0f;
                    draw_lizard(&cv, &big, &q, 220, 300, 1.3f);
                }
    }
    SAY("  the worst build (%s) on a frame that builds:     ~%5.0f us on the board\n", wname, worst * k);
    SAY("  scratch (cached bodies + the build's): %u bytes (PSRAM on the board: lizard_set_scratch); bodies cut to the grid: %d\n",
        (unsigned)lizard_scratch_bytes(), lizard_cache_clipped());
    for (int b = 0; b < GLASS_BOARDS; b++) {
        glass_t sb = glass_of(b);
        int64_t t0 = sl_now_us();
        for (int i = 0; i < 10; i++) scene_desert_draw(g_frame, sb.w, &sb, habitat_get(HABITAT_DESERT), true, NULL, 1.0f, NULL);
        double us = (double)(sl_now_us() - t0) / 10;
        SAY("  the desert's bake, %-6s %3dx%3d:               %6.0f us  ~%5.1f ms on the board (once per light switch / moisture step)\n", sb.name, sb.w, sb.h, us, us * k / 1000);
    }
    #undef SAY
    if (out) fclose(out);
}

int lizard_sheet(const char *prefix) {
    strip_make();
    sheet_morphs(prefix);
    sheet_poses(prefix);
    sheet_motion(prefix);
    sheet_shed(prefix);
    sheet_scenes(prefix);
    const int misses = ground_misses();                                        /* the ground as drawn against the engine's paths */
    cost(prefix);
    return misses ? 1 : 0;
}
