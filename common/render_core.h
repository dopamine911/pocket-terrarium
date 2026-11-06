/* render_core.h - the renderer's shared pixel layer and page drawing primitives (issue #4 phase 3).
 *
 * render.c is the core: the draw target (ctx_t), the dirty / touched / box masks every drawn pixel marks, the blends and spans, the
 * scene cache and its vignette, the pixel font and the pages. The world scenes (common/worlds/<world>/scene.c) and the body types'
 * art (common/bodies/<body>/) draw through these and nothing else of render.c - the hot ones are static inline here, so they cost a
 * module what they cost render.c itself. Not for any file outside the renderer: the public API is render.h.
 *
 * (The text below is render.c's own, moved verbatim: its statics became externs defined in render.c, its small functions
 * static inline.) */
#ifndef RENDER_CORE_H
#define RENDER_CORE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "render.h"
#include "tank.h"

#define TAU 6.2831853f

/* a draw target: fb + stride + night dim, plus the window it covers in tank
 * coordinates (ox,oy,w,h) - the frame is (0,0,TANK_W,TANK_H); the stats card
 * cache is a small sprite that still gets drawn in tank coordinates */
typedef struct { uint16_t *fb; int stride; float dim; int ox, oy, w, h; } ctx_t;
/* every draw target made outside the keeper page counts: the page keeps a framebuffer only while nothing else drew
   (THE PAGE KEPT, render_milestones) */
extern unsigned g_ctx_other; extern bool g_ms_in;
#define CTX_MADE() do { if (!g_ms_in) g_ctx_other++; } while (0)
static inline ctx_t ctx_full(uint16_t *fb, int stride, float dim) {
    CTX_MADE();
    ctx_t c = { fb, stride, dim, 0, 0, TANK_W, TANK_H }; return c;
}
/* PAGE space (render.h): the full-screen pages, the prompts and the exported
 * drawing helpers put their 448 x 368 layout in the middle of the frame - a
 * ctx whose origin is the page's corner. In the rectangle that IS the frame. */
extern bool g_dirty_hold;                          /* (a page's coordinates must never reach the dirty mask) */
static inline ctx_t ctx_page(uint16_t *fb, int stride) {
    CTX_MADE();
    ctx_t c = { fb, stride, 1.0f, -PAGE_X, -PAGE_Y, TANK_W, TANK_H };
    g_dirty_hold = true;                           /* a page's coordinates must never reach the mask */
    return c;
}
#define CTX_IN(c, x, y) ((unsigned)((x) - (c)->ox) < (unsigned)(c)->w && (unsigned)((y) - (c)->oy) < (unsigned)(c)->h)
#define CTX_PX(c, x, y) ((c)->fb[((y) - (c)->oy) * (c)->stride + ((x) - (c)->ox)])

/* Dirty mask (2026-09-01): one bit per pixel, set by everything drawn over
 * the baked scene (px, span, blends), cleared every frame. The porthole
 * vignette re-apply walks the mask instead of comparing each pixel with the
 * scene cache (32 pixels per word, no scene reads), and a pixel that already
 * had its vignette applied inline (frond spans, span_final) is simply not
 * marked, so no later rect darkens it twice. (A first version tagged the
 * green LSB instead and cleared it across the scene - that halved the color
 * steps of the dark vignette falloff into visible contour rings. Colors are
 * untouched now.) */
#define DIRTY_WORDS_PER_ROW ((TANK_W + 31) / 32)   /* 14 (15 on the 466 px bowl) */
extern uint32_t *g_dirty;
/* A PAGE draws in its own coordinates (ctx_page: on the bowl its origin is
 * not the frame's, and its y runs negative above the page) - marked as they
 * are, those wrote in front of the mask (2026-10-01: the settings page's
 * fill took the round board down). Nothing reads the mask after render_tank's
 * sweep, so from the first page ctx of a frame until the next render_tank
 * nothing is marked. */
static inline void dirty_px(int x, int y) {
    if (g_dirty && !g_dirty_hold) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] |= 1u << (x & 31);
}
static inline void dirty_span(int x0, int x1, int y) {
    if (!g_dirty || g_dirty_hold) return;
    uint32_t *row = g_dirty + y * DIRTY_WORDS_PER_ROW;
    int w0 = x0 >> 5, w1 = x1 >> 5;
    if (w0 == w1) { row[w0] |= (0xFFFFFFFFu >> (31 - (x1 & 31))) & (0xFFFFFFFFu << (x0 & 31)); return; }
    row[w0] |= 0xFFFFFFFFu << (x0 & 31);
    for (int w = w0 + 1; w < w1; w++) row[w] = 0xFFFFFFFFu;
    row[w1] |= 0xFFFFFFFFu >> (31 - (x1 & 31));
}

static inline void dirty_clear_span(int x0, int x1, int y) {   /* the span's marks off, a word at a time */
    if (!g_dirty || g_dirty_hold) return;
    uint32_t *row = g_dirty + y * DIRTY_WORDS_PER_ROW;
    int w0 = x0 >> 5, w1 = x1 >> 5;
    if (w0 == w1) { row[w0] &= ~((0xFFFFFFFFu >> (31 - (x1 & 31))) & (0xFFFFFFFFu << (x0 & 31))); return; }
    row[w0] &= ~(0xFFFFFFFFu << (x0 & 31));
    for (int w = w0 + 1; w < w1; w++) row[w] = 0;
    row[w1] &= ~(0xFFFFFFFFu >> (31 - (x1 & 31)));
}

/* bounding box of everything drawn while g_bb_on (render_tank wraps each
 * fish in it, so its vignette rect is exactly the pixels it touched instead
 * of a fixed 88 px box - fewer PSRAM reads in the re-apply sweep) */
extern bool g_bb_on; extern int g_bb_x0, g_bb_y0, g_bb_x1, g_bb_y1;
extern uint32_t *g_keep_rec, *g_keep_rec2;       /* SWIM DEPTH phase 2: while set, the fish being drawn marks its every span here (g_keep; and
                                                     g_fbg_front / _thru against a bed IN FRONT) */
extern uint32_t *g_fbg_clr1, *g_fbg_clr2;         /* (the bed's marks, for the front slot's clearing too) */
extern uint32_t *g_keep_clr_m;                   /* (g_keep, for the clearing) */
extern bool g_keep_clr;                           /* ... while set (a critter of the front slot or the top lane drawn after the pieces), its spans
                                                     clear the marks: what it paints over a fish is the critter's, shaded and covered as the critter */
static inline void bb_add(int x0, int x1, int y) {
    if (!g_bb_on) return;
    if ((g_keep_rec || g_keep_rec2 || g_keep_clr) && (unsigned)y < (unsigned)TANK_H) {
        int a = x0 < 0 ? 0 : x0, b = x1 > TANK_W - 1 ? TANK_W - 1 : x1;
        const int wpr = (TANK_W + 31) / 32;
        uint32_t *row = g_keep_rec ? g_keep_rec + y * wpr : NULL, *row2 = g_keep_rec2 ? g_keep_rec2 + y * wpr : NULL;
        uint32_t *c0 = g_keep_clr && g_keep_clr_m ? g_keep_clr_m + y * wpr : NULL, *c1 = g_keep_clr && g_fbg_clr1 ? g_fbg_clr1 + y * wpr : NULL,
                 *c2 = g_keep_clr && g_fbg_clr2 ? g_fbg_clr2 + y * wpr : NULL;
        for (int w = a >> 5; a <= b && w <= b >> 5; w++) {
            uint32_t m = 0xFFFFFFFFu;
            if (w == a >> 5) m &= 0xFFFFFFFFu << (a & 31);
            if (w == b >> 5) m &= 0xFFFFFFFFu >> (31 - (b & 31));
            if (c0) c0[w] &= ~m;                     /* (the clearing first: an animal of the front slot marking its own pixels keeps them) */
            if (c1) c1[w] &= ~m;
            if (c2) c2[w] &= ~m;
            if (row) row[w] |= m;
            if (row2) row2[w] |= m;
        }
    }
    if (x0 < g_bb_x0) g_bb_x0 = x0;
    if (x1 > g_bb_x1) g_bb_x1 = x1;
    if (y < g_bb_y0) g_bb_y0 = y;
    if (y > g_bb_y1) g_bb_y1 = y;
}
/* The TOUCHED mask (2026-10-23, the frame rate on every board): one bit per
 * pixel the frame WROTE over the baked scene - every writer marks it, in the
 * FRAME's coordinates (a page's ctx has its own origin), never held, final
 * stores included. It is what puts the scene back under the frame once it
 * has left the wire (render_restore_touched: the touched words copied from
 * the scene), in place of the whole-frame prefetch that moved 920 KB over
 * the PSRAM bus every frame on the square - the bus, not the CPU, is the
 * wall on these boards. The firmware keeps one per framebuffer and hands
 * the frame's over before render_tank (render_set_touched_mask); a frame
 * that is a whole page says so (render_touch_all). The sim's
 * --selftest-restore draws through this path against a whole re-prime and
 * fails on any ghost. */
extern uint32_t *g_touched;
static inline void touch_span_f(int x0, int x1, int y) {          /* frame coordinates */
    if (!g_touched || (unsigned)y >= (unsigned)TANK_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > TANK_W - 1) x1 = TANK_W - 1;
    if (x0 > x1) return;
    uint32_t *row = g_touched + y * DIRTY_WORDS_PER_ROW;
    int w0 = x0 >> 5, w1 = x1 >> 5;
    if (w0 == w1) { row[w0] |= (0xFFFFFFFFu >> (31 - (x1 & 31))) & (0xFFFFFFFFu << (x0 & 31)); return; }
    row[w0] |= 0xFFFFFFFFu << (x0 & 31);
    for (int w = w0 + 1; w < w1; w++) row[w] = 0xFFFFFFFFu;
    row[w1] |= 0xFFFFFFFFu >> (31 - (x1 & 31));
}
static inline void touch_px_f(int x, int y) {
    if (g_touched && (unsigned)x < (unsigned)TANK_W && (unsigned)y < (unsigned)TANK_H) g_touched[y * DIRTY_WORDS_PER_ROW + (x >> 5)] |= 1u << (x & 31);
}
#define TOUCH_SPAN(c, x0, x1, y) touch_span_f((x0) - (c)->ox, (x1) - (c)->ox, (y) - (c)->oy)
#define TOUCH_PX(c, x, y)        touch_px_f((x) - (c)->ox, (y) - (c)->oy)

/* the pixels a layer under the grass must leave alone (the grass cache and the loop: the aquarium's scene sets them) */
extern const uint32_t *g_under;                /* while the overlays behind are drawn: pixels to leave alone */
extern int g_under_y0;                          /* the first row g_under holds (the grass loop's masks are bands) */
static inline bool under_bg(int x, int y) {
    return g_under && (unsigned)x < (unsigned)TANK_W && y >= g_under_y0 && y < TANK_H && ((g_under[y * DIRTY_WORDS_PER_ROW + (x >> 5)] >> (x & 31)) & 1);
}

static inline void px(ctx_t *c, int x, int y, uint16_t col) {
    if (CTX_IN(c, x, y)) {
        CTX_PX(c, x, y) = FB_SWAP16(col);
        bb_add(x, x, y);
        dirty_px(x, y); TOUCH_PX(c, x, y);
    }
}

/* A source color dimmed ONCE per shape (night palette), never per pixel:
 * the float multiply + conversions were the bulk of every blended pixel's
 * cost (vegetation, algae film, light shafts, the stats card backdrop). */
typedef struct { int r, g, b; uint16_t v; } src_t;
static inline src_t src_color(uint32_t rgb, float dim) {
    src_t s;
    s.r = (int)(((rgb >> 16) & 255) * dim);
    s.g = (int)(((rgb >> 8) & 255) * dim);
    s.b = (int)((rgb & 255) * dim);
    s.v = (uint16_t)(((s.r >> 3) << 11) | ((s.g >> 2) << 5) | (s.b >> 3));
    return s;
}
/* alpha 0..255 blend of a pre-dimmed source onto one framebuffer pixel */
static inline void blend565(uint16_t *p, const src_t *s, int a) {
    uint16_t cur = FB_SWAP16(*p);
    int dr = (cur >> 11) << 3, dg = ((cur >> 5) & 63) << 2, db = (cur & 31) << 3;
    int r = dr + ((s->r - dr) * a >> 8), g = dg + ((s->g - dg) * a >> 8), b = db + ((s->b - db) * a >> 8);
    *p = FB_SWAP16((uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
}
static inline void px_blend_s(ctx_t *c, int x, int y, const src_t *s, int a) {
    if (CTX_IN(c, x, y) && !under_bg(x, y)) { blend565(&CTX_PX(c, x, y), s, a); bb_add(x, x, y); dirty_px(x, y); TOUCH_PX(c, x, y); }   /* (bb_add: the fish's box and SWIM DEPTH's marks see blends too) */
}
/* alpha 0..255 blend onto existing pixel (one-off pixels; shapes hoist) */
static inline void px_blend(ctx_t *c, int x, int y, uint32_t rgb, int a) {
    src_t s = src_color(rgb, c->dim);
    px_blend_s(c, x, y, &s, a);
}
/* horizontal span [x0,x1] on row y, clipped to the frame, pre-dimmed source */
static inline void span(ctx_t *c, int x0, int x1, int y, const src_t *s, int alpha) {
    if ((unsigned)(y - c->oy) >= (unsigned)c->h) return;
    if (x0 < c->ox) x0 = c->ox;
    if (x1 > c->ox + c->w - 1) x1 = c->ox + c->w - 1;
    if (x0 > x1) return;
    if (g_under) {                                   /* under the cached grass: pixel by pixel, only what shows (marking the rest would vignette the grass twice) */
        uint16_t *q = &CTX_PX(c, x0, y); uint16_t sv = FB_SWAP16(s->v);
        for (int x = x0; x <= x1; x++, q++) { if (under_bg(x, y)) continue; if (alpha >= 255) *q = sv; else blend565(q, s, alpha); dirty_px(x, y); TOUCH_PX(c, x, y); }
        bb_add(x0, x1, y);
        return;
    }
    bb_add(x0, x1, y);
    dirty_span(x0, x1, y); TOUCH_SPAN(c, x0, x1, y);
    uint16_t *p = &CTX_PX(c, x0, y);
    if (alpha >= 255) { uint16_t sv = FB_SWAP16(s->v); for (int x = x0; x <= x1; x++) *p++ = sv; }
    else              for (int x = x0; x <= x1; x++) blend565(p++, s, alpha);
}

#define PROF_MARK() (render_clock_us ? render_clock_us() : 0)
#define PROF_ADD(i, t0) do { if (render_clock_us) { int64_t _n = render_clock_us(); render_prof_us[i] += _n - (t0); (t0) = _n; } } while (0)

/* ---- static scene: gradient, pebbles, backdrop decor (cacheable) ---- */
extern uint16_t *g_scene;
extern float     g_scene_dim;
extern unsigned  g_scene_epoch;
extern const uint16_t *g_primed_fb;
extern unsigned  g_primed_epoch;
extern uint8_t  *g_vig;                  /* per-pixel vignette alpha (static) */
extern bool      g_vig_filled;

/* vignette alpha at (x,y); matches the classic per-pixel loop */
static inline int vig_alpha(int x, int y) {
    float dx = (x - TANK_W * 0.5f) * (2.0f / TANK_W);   /* (multiplies: a frond span calls this - 2026-10-23) */
    float dy = (y - TANK_H * 0.5f) * (2.0f / TANK_H);
    float d2 = dx * dx + dy * dy;
    if (d2 <= 0.72f) return 0;
    int a = (int)((d2 - 0.72f) * 220);
    return a > 255 ? 255 : a;
}
/* pure darkening (blend toward black), independent of ctx dim */
static inline void px_darken(uint16_t *p, int a) {
    int inv = 256 - a; uint16_t cur = FB_SWAP16(*p);
    *p = FB_SWAP16((uint16_t)(((((cur >> 11) * inv) >> 8) << 11) |
                              (((((cur >> 5) & 63) * inv) >> 8) << 5) |
                              (((cur & 31) * inv) >> 8)));
}

/* shared functions that stay in render.c */
float ell_half(float t);                                      /* row half-width of a unit circle at |t| (tabled) */
void  fill_ellipse(ctx_t *c, float cx, float cy, float rx, float ry, uint32_t rgb, int alpha);
float fast_sin(float x);                                      /* sine from a 256-entry table (phases non-negative) */
void  fill_poly_a(ctx_t *c, const float *xs, const float *ys, int n, uint32_t rgb, int alpha);   /* filled convex polygon, points in world space */
void  fill_poly(ctx_t *c, const float *xs, const float *ys, int n, uint32_t rgb);
void  draw_algae(ctx_t *c, const tank_t *t);                  /* the film on the glass (both worlds' sponge wipes it) */
#endif
