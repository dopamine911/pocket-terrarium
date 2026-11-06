/* render.c — porthole look: deep gradient water, AMOLED-black floor, procedural
 * fish (body polygon + animated tail + earned markings), bubbles, seagrass
 * the keeper trims. Everything is drawn
 * into a bare RGB565 buffer; night dims the palette. */
#include "render.h"
#include "pt_util.h"
#include "advanced.h"
#include "card.h"
#include "icons.h"
#include "progression.h"
#include "collection.h"
#include "sounds.h"
#include "notice.h"
#include "gift.h"
#include "tank_events.h"
#include "setup.h"
#include "version.h"
#include "world_view.h"
#include "body.h"
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "render_core.h"

/* THE SHARED PIXEL LAYER's state (render_core.h): every file of the renderer draws through it */
unsigned g_ctx_other; bool g_ms_in;
bool g_dirty_hold;
uint32_t *g_dirty = NULL;
bool g_bb_on; int g_bb_x0, g_bb_y0, g_bb_x1, g_bb_y1;
uint32_t *g_keep_rec, *g_keep_rec2;
uint32_t *g_fbg_clr1, *g_fbg_clr2;
uint32_t *g_keep_clr_m;
bool g_keep_clr;
uint32_t *g_touched;
const uint32_t *g_under;
int g_under_y0;
uint16_t *g_scene = NULL;
float     g_scene_dim = -1;
unsigned  g_scene_epoch = 0;
const uint16_t *g_primed_fb = NULL;
unsigned  g_primed_epoch = 0;
uint8_t  *g_vig = NULL;
bool      g_vig_filled = false;



void render_set_dirty_mask(uint32_t *buf) { g_dirty = buf; }

void render_set_touched_mask(uint32_t *buf) { g_touched = buf; }
void render_touch_all(void) { if (g_touched) memset(g_touched, 0xff, (size_t)RENDER_DIRTY_WORDS * 4); }
/* row half-width of a unit circle at |t| = k/64: sqrt(1 - t^2), tabled -
 * the ESP32-S3 computes sqrtf in software and a fouled glass alone asks for
 * ~7000 of them per frame (algae blobs); the quantisation is sub-pixel */
float ell_half(float t) {
    static float lut[66]; static bool filled;
    if (!filled) { for (int k = 0; k <= 65; k++) { float u = k / 64.0f; lut[k] = u < 1 ? sqrtf(1 - u * u) : 0; } filled = true; }
    if (t < 0) t = -t;
    return t >= 1 ? 0 : lut[(int)(t * 64 + 0.5f)];
}
void fill_ellipse(ctx_t *c, float cx, float cy, float rx, float ry,
                  uint32_t rgb, int alpha) {
    src_t s = src_color(rgb, c->dim);
    int y0 = (int)(cy - ry), y1 = (int)(cy + ry);
    for (int y = y0; y <= y1; y++) {
        float w = ell_half((y - cy) / ry);
        if (w <= 0) continue;
        float half = rx * w;
        span(c, (int)(cx - half), (int)(cx + half), y, &s, alpha);
    }
}

/* sine from a 256-entry table: the frond sway only has to look continuous,
 * and the ESP32-S3 computes sinf in software (~1000 per frame at full
 * canopy). Phases here are non-negative; & 255 keeps any sign honest. */
float fast_sin(float x) {
    static float lut[256]; static bool filled;
    if (!filled) { for (int i = 0; i < 256; i++) lut[i] = sinf(i * (TAU / 256)); filled = true; }
    return lut[(int)(x * (256.0f / TAU)) & 255];
}

/* filled convex polygon, points in world space */
void fill_poly_a(ctx_t *c, const float *xs, const float *ys, int n, uint32_t rgb, int alpha) {
    float miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) { if (ys[i] < miny) miny = ys[i]; if (ys[i] > maxy) maxy = ys[i]; }
    src_t s = src_color(rgb, c->dim);
    for (int y = (int)miny; y <= (int)maxy; y++) {
        float x0 = 1e9f, x1 = -1e9f;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float ay = ys[i], by = ys[j];
            if ((ay <= y && by > y) || (by <= y && ay > y)) {
                float x = xs[i] + (xs[j] - xs[i]) * (y - ay) / (by - ay);
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
            }
        }
        span(c, (int)x0, (int)x1, y, &s, alpha);
    }
}
void fill_poly(ctx_t *c, const float *xs, const float *ys, int n, uint32_t rgb) { fill_poly_a(c, xs, ys, n, rgb, 255); }

bool g_no_animals;                        /* THE EDITOR (0.3.9.4, Strato): a placement page up - every creature hidden, the frame rate to the drag */

/* algae film on the glass, drawn over everything: dappled blobs per covered
 * grid cell, thicker film = bigger and greener. The keeper wipes it off. */
void draw_algae(ctx_t *c, const tank_t *t) {
    const uint32_t *film = world_view()->algae_rgb;
    for (int cy = 0; cy < ALGAE_ROWS; cy++)
        for (int cx = 0; cx < ALGAE_COLS; cx++) {
            int cov = t->algae[cy * ALGAE_COLS + cx];
            if (!cov) continue;
            uint32_t h = (uint32_t)((cx * 73856093u) ^ (cy * 19349663u));
            float bx = cx * ALGAE_CELL, by = cy * ALGAE_CELL;
            for (int b = 0; b < 3; b++) {
                uint32_t hb = h ^ (b * 2654435761u);
                float ox = (float)(hb % ALGAE_CELL);
                float oy = (float)((hb >> 5) % ALGAE_CELL);
                float r = (1.5f + (float)((hb >> 10) % 3)) * (0.55f + 0.45f * cov / 255.0f)
                        + 2.2f * cov / 255.0f;
                fill_ellipse(c, bx + ox, by + oy, r, r * 0.85f,   /* the world's film: the aquarium's algae greens, the terrarium's dusty smudge (the sponge's all the same) */
                             (hb & 4) ? film[0] : film[1], 34 + cov * 96 / 255);
            }
        }
}

/* optional per-stage frame profiling (render.h) */
int64_t (*render_clock_us)(void) = NULL;
int64_t render_prof_us[14];

/* ---- static scene: gradient, pebbles, backdrop decor (cacheable) ---- */
void render_set_scene_cache(uint16_t *buf) {
    g_scene = buf; g_scene_dim = -1;
    if (world_view()->scene_reset) world_view()->scene_reset(buf);
}
void render_set_vignette_cache(uint8_t *buf) { g_vig = buf; g_vig_filled = false; }

const uint16_t *render_scene_buf(unsigned *epoch) {
    const uint16_t *alt = world_view()->scene_alt ? world_view()->scene_alt(epoch) : NULL;   /* the world's own cache of the scene (the aquarium's grass cache), when it is current */
    if (alt) return alt;
    if (epoch) *epoch = g_scene_epoch;
    return g_scene_epoch ? g_scene : NULL;
}
void render_fb_primed(const uint16_t *fb, unsigned epoch) { g_primed_fb = fb; g_primed_epoch = epoch; }
/* the scene back under what a frame drew (its touched mask): runs of touched
 * 32 px words copied from the scene. Returns the scene epoch it restored
 * against: the buffer is primed for that epoch (a bake in between shows as a
 * mismatch at the next render, which copies the whole scene then). */
unsigned render_restore_touched(uint16_t *fb, const uint32_t *touched) {
    unsigned ep = g_scene_epoch;
    g_ctx_other++;                                   /* (written with no draw target: a kept keeper page is gone) */
    if (!g_scene || !touched) return ep;
    for (int y = 0; y < TANK_H; y++) {
        const uint32_t *row = touched + y * DIRTY_WORDS_PER_ROW;
        uint16_t *d = fb + y * TANK_W; const uint16_t *s = g_scene + y * TANK_W;
        for (int w = 0; w < DIRTY_WORDS_PER_ROW; w++) {
            if (!row[w]) continue;
            int w1 = w;
            while (w1 + 1 < DIRTY_WORDS_PER_ROW && row[w1 + 1]) w1++;
            int x0 = w * 32, x1 = (w1 + 1) * 32;
            if (x1 > TANK_W) x1 = TANK_W;
            memcpy(d + x0, s + x0, (size_t)(x1 - x0) * 2);
            w = w1;
        }
    }
    return ep;
}
unsigned render_scene_epoch(void) { return g_scene_epoch; }

void render_tank(const tank_t *t, uint16_t *fb, int stride) { world_view()->render(t, fb, stride); }



/* ---- the battery (2026-09-24 redraw): Strato is color blind and could not
 * tell the charging pill (a teal fill) from the full one (green). Now the
 * cable shows as a SHAPE - a lightning bolt left of the pill - and flowing
 * charge as MOTION - a bright band sweeping the fill; the hue only repeats
 * it. On battery the fill wears the level colors; on the cable it is the
 * calm green whatever the level (a red sliver on the charger is no alarm).
 * The info page draws the same battery large. ---- */

/* the bolt, a polygon in a 10 x 16 box: the flat top, the stroke down to
   the left, the jog across, the tail to the point */
static const float BOLT_X[7] = { 6.0f, 0.5f, 4.0f, 2.5f, 10.0f, 6.5f, 10.0f };
static const float BOLT_Y[7] = { 0.0f, 9.5f, 9.5f, 16.0f, 5.5f, 5.5f, 0.0f };
/* filled with 4 sub-rows of exact span coverage per pixel row, so it stays a
   bolt at the pill's 15 px (no stair-stepped mush) and the page's 38 */
static void bolt_fill(ctx_t *c, float ox, float oy, float s, uint32_t rgb, int alpha) {
    src_t col = src_color(rgb, 1.0f);
    int x0 = (int)floorf(ox), w = (int)ceilf(10.0f * s) + 2;
    float cov[64];
    if (w > 64) w = 64;
    for (int y = (int)floorf(oy); y <= (int)ceilf(oy + 16.0f * s); y++) {
        memset(cov, 0, sizeof cov);
        for (int sub = 0; sub < 4; sub++) {
            float yy = (y + (sub + 0.5f) * 0.25f - oy) / s, xs[8]; int n = 0;
            for (int i = 0, j = 6; i < 7; j = i++)
                if ((BOLT_Y[i] > yy) != (BOLT_Y[j] > yy))
                    xs[n++] = ox + s * (BOLT_X[j] + (yy - BOLT_Y[j]) * (BOLT_X[i] - BOLT_X[j]) / (BOLT_Y[i] - BOLT_Y[j]));
            for (int i = 1; i < n; i++) for (int k = i; k > 0 && xs[k] < xs[k - 1]; k--) { float t = xs[k]; xs[k] = xs[k - 1]; xs[k - 1] = t; }
            for (int k = 0; k + 1 < n; k += 2)                   /* each inside interval, spread over the pixels it crosses */
                for (int px_ = (int)floorf(xs[k]); px_ <= (int)floorf(xs[k + 1]); px_++) {
                    float a = fmaxf(xs[k], (float)px_), b = fminf(xs[k + 1], px_ + 1.0f);
                    if (b > a && px_ - x0 >= 0 && px_ - x0 < w) cov[px_ - x0] += (b - a) * 0.25f;
                }
        }
        for (int i = 0; i < w; i++)
            if (cov[i] > 0.02f) px_blend_s(c, x0 + i, y, &col, (int)(fminf(cov[i], 1.0f) * alpha));
    }
}
/* the bolt h px tall at (x, y), with a dark rim so it reads over bright water */
static void draw_bolt(ctx_t *c, float x, float y, float h, uint32_t rgb) {
    float s = h / 16.0f;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) bolt_fill(c, x + dx, y + dy, s, 0x04141a, 200);
    bolt_fill(c, x, y, s, rgb, 255);
}
static float bolt_w(float h) { return 10.0f * h / 16.0f; }
static int bolt_room(int H) { return (int)bolt_w(H * 1.25f) + H / 4 + 2; }   /* the bolt beside a battery H tall, and its gap */

/* a battery at (X, Y), body W x H, `line` px of outline, the nub on the right;
   on the cable the bolt stands left of it (the caller leaves the room) */
static void draw_battery(ctx_t *c, int X, int Y, int W, int H, int line, float frac, int state, double clock) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    bool pw = BAT_ON_POWER(state);
    int pct = (int)(frac * 100 + 0.5f);                       /* the percent the battery page shows: the pill and the page agree at 20 (0.3.9.3, C1704) */
    uint32_t col = pw ? 0x78d67d : pct < 20 ? 0xf25b65 : pct < 45 ? 0xffbd59 : 0x78d67d;
    uint32_t edge = pw ? 0xdfeef0 : 0x9fb4b8;                  /* on the cable the rim brightens too */
    src_t bg = src_color(0x04141a, 1.0f), e = src_color(edge, 1.0f), f = src_color(col, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 215);
    for (int k = 0; k < line; k++) {
        span(c, X, X + W - 1, Y + k, &e, 255); span(c, X, X + W - 1, Y + H - 1 - k, &e, 255);
        for (int y = Y; y < Y + H; y++) { px(c, X + k, y, e.v); px(c, X + W - 1 - k, y, e.v); }
    }
    int nh = H * 2 / 5, nw = line + 2;                         /* the nub */
    for (int y = Y + (H - nh) / 2; y < Y + (H + nh) / 2; y++) span(c, X + W, X + W + nw - 1, y, &e, 255);
    int in = line + 1, iw = W - 2 * in, fw = (int)(iw * frac + 0.5f);
    if (pw && fw < 2) fw = 2;
    for (int y = Y + in; y < Y + H - in; y++) if (fw > 0) span(c, X + in, X + in + fw - 1, y, &f, 255);
    if (state == BAT_CHARGING && fw > 0) {                     /* the sweep: charge flowing in, left to right */
        float ph = (float)fmod(clock, 1.8) / 1.3f;
        if (ph <= 1.0f) {
            float bw = fmaxf(4.0f, iw / 5.0f), cx = X + in - bw + ph * (fw + 2 * bw);
            src_t wh = src_color(0xffffff, 1.0f);
            for (int x = X + in; x < X + in + fw; x++) {
                float d = fabsf(x + 0.5f - cx) / bw;
                if (d < 1) for (int y = Y + in; y < Y + H - in; y++) px_blend_s(c, x, y, &wh, (int)(170 * (1 - d)));
            }
        }
    }
    if (pw) draw_bolt(c, X - bolt_room(H), Y - H / 8.0f, H * 1.25f, state == BAT_PLUGGED ? 0x9fb4b8 : 0xffffff);   /* a touch taller than the battery */
}

/* the pill, top right (render.h) */
void render_battery(uint16_t *fb, int stride, float frac, int state, double clock) {
    ctx_t c = ctx_full(fb, stride, 1.0f);
    draw_battery(&c, RENDER_BAT_X, RENDER_BAT_Y, RENDER_BAT_W, RENDER_BAT_H, 1, frac, state, clock);
}

/* ---- stats overlay (selection ring + visual card) ---- */

void ring(ctx_t *c, float cx, float cy, float r, uint32_t rgb) {
    for (int i = 0; i < 64; i++) {
        float a = i * (TAU / 64);
        px_blend(c, (int)(cx + cosf(a) * r), (int)(cy + sinf(a) * r), rgb, 180);
    }
}

/* blend one native RGB565 pixel (icon art is pre-colored; no dim - the card
 * ignores night, matching px_blend's use with c->dim = 1) */
static void px565_blend(ctx_t *c, int x, int y, uint16_t v, int a) {
    if (!CTX_IN(c, x, y)) return;
    uint16_t *p = &CTX_PX(c, x, y), cur = FB_SWAP16(*p);
    int r = (cur >> 11)       + (((v >> 11)       - (cur >> 11))       * a >> 8);
    int g = ((cur >> 5) & 63) + ((((v >> 5) & 63) - ((cur >> 5) & 63)) * a >> 8);
    int b = (cur & 31)        + (((v & 31)        - (cur & 31))        * a >> 8);
    *p = FB_SWAP16((uint16_t)((r << 11) | (g << 5) | b)); bb_add(x, x, y); TOUCH_PX(c, x, y);
}

/* draw a baked icon (icons.h, generated from Strato's pixel art) at x,y.
 * alpha scales the icon's own alpha plane: 255 = as drawn, lower = dimmed
 * (unrevealed traits, torn thought bubbles). */
void blit_icon(ctx_t *c, int x, int y, const icon_t *ic, int alpha) {
    for (int j = 0; j < ic->h; j++)
        for (int i = 0; i < ic->w; i++) {
            int a = ic->a[j * ic->w + i];
            if (!a) continue;
            px565_blend(c, x + i, y + j, ic->rgb[j * ic->w + i], a * alpha >> 8);
        }
}

/* a segmented meter: value 0..1 over `segs` segments of `sw` x 10 px. The
 * last partial segment fills proportionally, so it still reads analog up
 * close but "3 of 5" at a glance. */
static void meter(ctx_t *c, int x, int y, int segs, int sw, float frac, uint32_t rgb) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float per = 1.0f / segs;
    src_t on = src_color(rgb, c->dim), off = src_color(0x2a3f45, c->dim);
    for (int s = 0; s < segs; s++) {
        int sx = x + s * (sw + 2);
        float have = (frac - s * per) / per;                   /* 0..1 of this segment */
        int fw = have >= 1 ? sw : have <= 0 ? 0 : (int)(have * sw + 0.5f);
        for (int yy = 0; yy < 10; yy++) {
            if (fw > 0)  span(c, sx, sx + fw - 1, y + yy, &on, 235);
            if (fw < sw) span(c, sx + fw, sx + sw - 1, y + yy, &off, 150);
        }
    }
}

/* a trait spectrum: pole icons at both ends, the fish sits at `frac` between
 * them. Unrevealed = dimmed poles, dashed line, a "?" instead of the dot -
 * you learn who a fish is by watching it, not by reading it. */
static void slider(ctx_t *c, int x, int y, int w, float frac, uint32_t rgb,
                   const icon_t *lo, const icon_t *hi, bool revealed) {
    blit_icon(c, x, y, lo, revealed ? 255 : 70);
    blit_icon(c, x + w - 16, y, hi, revealed ? 255 : 70);
    int lx = x + 20, lw = w - 40, ly = y + 8;                  /* the line between poles */
    for (int xx = 0; xx < lw; xx++)
        if (revealed || (xx & 4))
            for (int yy = -1; yy <= 0; yy++) px_blend(c, lx + xx, ly + yy, 0x2a3f45, revealed ? 200 : 110);
    if (revealed) {
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        fill_ellipse(c, lx + lw * frac, ly - 0.5f, 3.4f, 3.4f, rgb, 255);
        fill_ellipse(c, lx + lw * frac - 1, ly - 1.5f, 1.0f, 1.0f, 0xffffff, 200);
    } else
        blit_icon(c, lx + lw / 2 - 8, y, &icon_unknown_16, 220);
}

#define CARD_MORE_H 22
static void card_draw(ctx_t c, const tank_t *t, int fish_idx) {
    const fish_t *f = &t->fish[fish_idx];
    /* card: top-left, bordered in the fish's own color (that's its "name").
     * Two zones: NEEDS (things you can act on now - icon + segmented meter)
     * above the divider, WHO THEY ARE (slow traits - pole-to-pole spectrum
     * sliders) below it. Iconified 2026-08-30 with Strato's pixel art. */
    const int X = RENDER_CARD_X, Y = RENDER_CARD_Y, W = RENDER_CARD_W, H = RENDER_CARD_H; /* x clear of the curved bezel */
    /* backdrop: 28k blended pixels - hoisted (this alone was most of the
     * card's ~12 ms/frame on the device through per-pixel px_blend) */
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(&c, X, X + W - 1, y, &bg, 215);
    for (int x = X; x < X + W; x++) { px(&c, x, Y, rgb565(f->color, 1)); px(&c, x, Y + H - 1, rgb565(f->color, 1)); }
    for (int y = Y; y < Y + H; y++) { px(&c, X, y, rgb565(f->color, 1)); px(&c, X + W - 1, y, rgb565(f->color, 1)); }

    /* identity row: the fish's NAME and the certainty dot - bright = the
     * model was sure of its last decision, dim = torn. (2026-09-29, Strato:
     * the name at the top; the stage pips and growth bar are gone - "a bit
     * ambiguous", the keeper sees the fish's size, and the milestones page a
     * tap deeper names the stage. The colour swatch went with them: the
     * border is the fish's colour, and a 7-letter name needs the row.) */
    draw_text(&c, X + 8, Y + 8, 2, 0xffffff, f->name);
    fill_ellipse(&c, X + W - 14, Y + 14, 3.2f, 3.2f, 0xffffff, (int)(40 + 200 * f->goal.confidence));

    /* needs: 5 segments each. Hunger is shown as FULLNESS - a full belly is
     * a full meter, and it drains as the fish gets hungry (a food icon next
     * to a growing bar read backwards) */
    struct { const icon_t *ic; float v; uint32_t rgb; } needs[4] = {
        { world_view()->art.hunger, 10.0f - f->hunger, 0xffbd59 },   /* (the terrarium's: a cricket) */
        { &icon_energy, f->energy,         0x78d67d },
        { &icon_stress, f->stress,         0xf25b65 },
        { &icon_trust,  f->trust,          0xffd166 },
    };
    for (int i = 0; i < 4; i++) {
        int ry = Y + 30 + i * 27;
        blit_icon(&c, X + 8, ry, needs[i].ic, 255);
        meter(&c, X + 38, ry + 7, 5, 14, needs[i].v / 10.0f, needs[i].rgb);
    }

    /* divider between the zones */
    for (int x = X + 8; x < X + W - 8; x++) px_blend(&c, x, Y + 142, 0x2a3f45, 200);

    /* who they are: spectrum sliders, revealed by behaviour you have seen
       this fish do (docs/progression.md habits) */
    bool saw_bold = f->ms_bits & MS_FIRST_DART;
    bool saw_social = f->ms_bits & MS_FIRST_FOLLOW;
    bool saw_curious = f->ms_bits & (MS_INSPECTED | MS_FIRST_BUBBLES);
    slider(&c, X + 8, Y + 152, W - 16, f->bold,             0xffffff, world_view()->art.shy,      world_view()->art.bold,    saw_bold);
    slider(&c, X + 8, Y + 178, W - 16, f->sociable,         0x38dcc7, world_view()->art.solo,     world_view()->art.social,  saw_social);
    slider(&c, X + 8, Y + 204, W - 16, f->curiosity / 10.0f, 0x6db9ff, world_view()->art.cautious, world_view()->art.curious, saw_curious);

    /* the way onward (2026-09-16): a MORE button in the pages' dress at the
       foot of the card. The whole card was already the tap that opens the
       milestones page (and from there SETTINGS / UPGRADES), but nothing said
       so - a keeper asked Strato how to get there. The button is the sign;
       the hit box is still the card (touch ports, RENDER_CARD_H). */
    button(&c, X + 8, Y + H - 8 - CARD_MORE_H, W - 16, CARD_MORE_H, 0x1c2f36, 0x9fd8e2, "MORE", 2);
}

/* ---- stats card cache (2026-09-01) ----
 * The card cost ~7 ms of every frame it was up (28k blended backdrop pixels
 * + icons + meters) - fps sagged whenever the keeper inspected a fish. With a
 * cache it is redrawn at most 4x a second over the STATIC water under it
 * (from the scene cache, so the translucent backdrop looks as before; only
 * things swimming behind the card stop showing through at 16%) and copied
 * into the frame otherwise. The selection ring follows the fish per frame. */
static uint16_t *g_card = NULL;
static int g_card_fish = -1; static double g_card_t = -1; static unsigned g_card_epoch;
void render_set_card_cache(uint16_t *buf) { g_card = buf; g_card_fish = -1; }

#if PT_NET   /* the networking UI (version.h PT_NET): the passport, the bakes of the GIVE modal and the gift card, the gift card */
/* a visitor's card (2026-10-05, Strato: "it should be a bit different since
   it's a visiting fish, it should maybe look a little more like a passport"):
   the Pocket Tank Passport - a cover band, its photo, its fields, this tank's
   stamp, the machine line at the foot. Its home is named by its badge's SHAPE
   in words as well as drawn (never colour alone). */
static const char *const BADGE_WORDS[LINK_BADGES] = { "CIRCLE", "TRIANGLE", "SQUARE", "DIAMOND" };
/* HOME on a passport (2026-11-01, Strato): its HOME WATERS (passport_home_waters, from the tank's ID - the badge's shape is only
   who is next door now), or a renamed tank's own name with its waters small in the label - two SAM'S REEFs still differ */
static void home_fields(uint32_t home, const char *name, const char *lbl0, char *lbl, size_t ln, char *val, size_t vn) {
    char w[PASSPORT_WATERS_MAX]; passport_home_waters(home, w, sizeof w);
    if (name && name[0]) { snprintf(lbl, ln, "%s - %s", lbl0, w); snprintf(val, vn, "%s", name); }
    else { snprintf(lbl, ln, "%s", lbl0); snprintf(val, vn, "%s", w); }
}
static void passport_card_draw(ctx_t *c, uint16_t *fb, int stride, const tank_t *t, int k) {
    const fish_t *f = &t->fish[N_FISH_MAX + k];
    const uint32_t GOLD = 0xe8c872, INK = 0x13324f, LBL = 0x7fa3b5;
    ring(c, f->x, f->y, 17 * f->size, GOLD);
    const int W = PASSPORT_CARD_W, H = PASSPORT_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x0e2233, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 240);
    for (int y = Y + 34; y < Y + H - 30; y += 7)              /* the page's fine wavy lines */
        for (int x = X + 3; x < X + W - 3; x++) px_blend(c, x, y + (int)(2.0f * sinf(x * 0.11f + y * 0.37f)), 0x2b4a63, 80);
    src_t band = src_color(INK, 1.0f);
    for (int y = Y; y < Y + 26; y++) span(c, X, X + W - 1, y, &band, 255);
    const char *title = "POCKET TANK PASSPORT";
    draw_text(c, X + (W - text_w(title, 2)) / 2, Y + 7, 2, GOLD, title);
    rect_edge(c, X, Y, W, H, GOLD); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, INK);
    /* the photo */
    const int PX = X + 12, PY = Y + 36, PW = 100, PH = 84;
    rect_fill(c, PX, PY, PW, PH, 0x0a1824); rect_edge(c, PX, PY, PW, PH, 0x9fd8e2);
    render_fish_portrait(fb, stride, PX + PW / 2.0f - PAGE_X, PY + PH / 2.0f - PAGE_Y, 1.3f, f, PW - 20, t->clock);   /* (page coordinates) */
    /* the fields */
    const int FX = PX + PW + 12;
    int vb = t->vis_badge[k];
    char home[24], hlbl[40], form[24], here[24];
    home_fields(t->vis_home[k], t->vis_home_name[k], "HOME", hlbl, sizeof hlbl, home, sizeof home);
    snprintf(form, sizeof form, "%s %s", SHAPE_NAMES[f->shape % SHAPE_N], FINS_NAMES[f->fins % FINS_N]);
    float s = (float)(t->clock - t->vis_since[k]);
    if (f->homeward) snprintf(here, sizeof here, "HEADING HOME");
    else if (s < 60) snprintf(here, sizeof here, "JUST ARRIVED");
    else snprintf(here, sizeof here, "HERE %d MIN", (int)(s / 60));
    static const char *const STAGE_W[4] = { "FRY", "JUVENILE", "ADULT", "ELDER" };
    struct { const char *lbl, *val; } FL[4] = { { "NAME", f->name }, { hlbl, home }, { "FORM", form }, { "STAGE", STAGE_W[f->stage & 3] } };
    for (int i = 0; i < 4; i++) {
        int fy = Y + 33 + i * 26;
        draw_text(c, FX, fy, 1, LBL, FL[i].lbl);
        int vx = FX;
        if (i == 1 && vb >= 0) { link_badge_draw(c, FX + 6, fy + 17, 6, vb, 255); vx += 16; }
        draw_text(c, vx, fy + 10, 2, 0xffffff, FL[i].val);
    }
    draw_text(c, PX, PY + PH + 12, 1, LBL, "VISITING");   /* (a gap under STAGE's value: HERE NN MIN reaches under it) */
    draw_text(c, PX, PY + PH + 21, 2, 0xffffff, here);
    /* time together (2026-10-06): CLOSE TO <one of ours> and five pips that
       fill - filled vs hollow, never colour - no minutes (the goal is random) */
    { uint32_t mate = 0; float b = progression_bond(t, f->uid, &mate);
      const fish_t *m = NULL; for (int i = 0; i < t->n_fish && !m; i++) if (mate && t->fish[i].uid == mate) m = &t->fish[i];
      if (m && b > 0.02f) {
          char cl[32]; snprintf(cl, sizeof cl, "INTERESTED IN %s", m->name);   /* (Strato 11-01: was CLOSE TO) */
          int cy = PY + PH + 39;
          draw_text(c, PX, cy, 1, LBL, cl);
          int lit = (int)(b * 5 + 0.0001f);
          static const char *const HEART[10] = { ".XXX...XXX.", "XXXXX.XXXXX", "XXXXXXXXXXX", "XXXXXXXXXXX", "XXXXXXXXXXX",
                                                 ".XXXXXXXXX.", "..XXXXXXX..", "...XXXXX...", "....XXX....", ".....X....." };
          for (int i = 0; i < 5; i++) {                 /* five hearts (Strato 11-01): filled, or hollow - the shape, never the color */
              int bx = PX + i * 15, by = cy + 10;
              for (int yy = 0; yy < 10; yy++)
                  for (int xx = 0; xx < 11; xx++) {
                      if (HEART[yy][xx] != 'X') continue;
                      bool edge = xx == 0 || xx == 10 || yy == 0 || yy == 9 || HEART[yy][xx - 1] != 'X' || HEART[yy][xx + 1] != 'X'
                                  || HEART[yy - 1][xx] != 'X' || HEART[yy + 1][xx] != 'X';
                      if (i < lit) px_blend(c, bx + xx, by + yy, 0xffffff, 255);
                      else if (edge) px_blend(c, bx + xx, by + yy, 0x9fd8e2, 255);
                  }
          }
      } }
    /* this tank's stamp: two rings, our badge, VISITED */
    if (t->link_badge >= 0) {
        const float sx = X + W - 52, sy = Y + 142;
        for (int a = 0; a < 360; a += 2) {
            float r = a * 0.0174533f;
            px_blend(c, (int)(sx + cosf(r) * 30), (int)(sy + sinf(r) * 30), GOLD, 170);
            px_blend(c, (int)(sx + cosf(r) * 27), (int)(sy + sinf(r) * 27), GOLD, 120);
        }
        link_badge_draw(c, (int)sx, (int)sy - 5, 9, t->link_badge, 200);
        draw_text(c, (int)sx - text_w("VISITED", 1) / 2, (int)sy + 9, 1, GOLD, "VISITED");
    }
    /* the machine line */
    char m1[64], m2[64], nm[FISH_NAME_MAX + 1];
    int i = 0; for (; f->name[i] && i < FISH_NAME_MAX; i++) nm[i] = f->name[i]; nm[i] = 0;
    int cols = (W - 20) / 6; if (cols > 60) cols = 60;
    snprintf(m1, sizeof m1, "P<FISH<<%s", nm);
    snprintf(m2, sizeof m2, "%08X<<%08X<<%d", (unsigned)f->uid, (unsigned)t->vis_home[k], f->stage & 3);
    for (int j = (int)strlen(m1); j < cols; j++) m1[j] = '<';
    for (int j = (int)strlen(m2); j < cols; j++) m2[j] = '<';
    m1[cols] = m2[cols] = 0;
    draw_text(c, X + 10, Y + 186, 1, 0xc8d6de, m1);
    draw_text(c, X + 10, Y + 197, 1, 0xc8d6de, m2);
    /* collected: our COLLECTED stamp beside the VISITED one (Strato: keepers
       tell at a glance), and the buttons - DEPORT | COLLECT, the second dim
       once it is in the collection */
    bool have = collection_find(f->uid) >= 0, full = !have && collection_full();   /* full: every page (0.4 pages) - FULL, dim */
    if (have) {
        const char *st = "COLLECTED";
        int sw = text_w(st, 2) + 12, sx0 = FX + 10, sy0 = Y + 152;   /* (clear of HERE N MIN and of the VISITED stamp) */
        rect_edge(c, sx0, sy0, sw, 24, GOLD); rect_edge(c, sx0 + 2, sy0 + 2, sw - 4, 20, GOLD);
        draw_text(c, sx0 + 6, sy0 + 5, 2, GOLD, st);
    }
    const int BY = Y + PASSPORT_BTN_Y, BW = PASSPORT_BTN_W, gap = W - 2 * BW - 24;
    button(c, X + 12, BY, BW, PASSPORT_BTN_H, 0x1c2f36, 0x9fd8e2, "DEPORT", 2);
    if (have || full) {
        const char *w = have ? "COLLECTED" : "FULL";
        rect_edge(c, X + 12 + BW + gap, BY, BW, PASSPORT_BTN_H, 0x3f6a72);
        draw_text(c, X + 12 + BW + gap + (BW - text_w(w, 2)) / 2, BY + 8, 2, 0x3f6a72, w);
    } else button(c, X + 12 + BW + gap, BY, BW, PASSPORT_BTN_H, 0x1c2f36, GOLD, "COLLECT", 2);
}
/* ---- THE CARDS BAKED (0.4, 11-02; Strato's rule: always look for frame-rate wins, never drop frames) ----
 * The GIVE modal and the gift card offered to us were drawn whole over the live tank every frame (+11-12 ms of render
 * on the 1.8 and the square: 24.9 -> 21.2 fps, 20.3 -> 16.7 with the pick). Both are opaque, so each is painted into
 * its own buffer (PSRAM, main.c) only when what it shows changes - its key: the step, the fish, the tanks, the rows
 * open, the words - and copied into the frame otherwise, as the stats card's cache is. Only what moves is drawn live
 * over the copy: the fish's portrait and the time bar's fill (neither overlaps anything else on the card, so the frame
 * is the card drawn whole, pixel for pixel - selftest-doors 7k checks). No buffer (a board short of RAM): drawn whole. */
typedef struct { uint16_t *buf; uint32_t key; int h; bool ok; } bake_t;
static bake_t g_bake_gm, g_bake_gift;
static uint16_t *g_bake_bufs[2];
void render_set_modal_cache(uint16_t *give, uint16_t *gift) {
    g_bake_bufs[0] = give; g_bake_bufs[1] = gift;
    g_bake_gm.buf = give; g_bake_gift.buf = gift; g_bake_gm.ok = g_bake_gift.ok = false;
}
void render_modal_cache_on(bool on) {   /* (the director's `link bake on|off`: the same build measured both ways) */
    g_bake_gm.buf = on ? g_bake_bufs[0] : NULL; g_bake_gift.buf = on ? g_bake_bufs[1] : NULL; g_bake_gm.ok = g_bake_gift.ok = false;
}
#endif
static uint32_t bake_hash(uint32_t h, uint32_t v) { for (int b = 0; b < 4; b++) { h ^= (v >> (b * 8)) & 255; h *= 16777619u; } return h; }
#if PT_NET
static uint32_t bake_hash_s(uint32_t h, const char *s) { while (s && *s) h = bake_hash(h, (uint8_t)*s++); return bake_hash(h, 0); }
/* the bake's ctx (the card's own frame coordinates), nothing marked while it is painted (the copy marks the frame) */
static bool bake_stale(bake_t *b, uint32_t key, int h) { return !b->ok || b->key != key || b->h != h; }
static uint32_t *bake_begin(bake_t *b, ctx_t *bc, int X, int Y, int W, int H, bool *hold) {
    uint32_t *tm = g_touched; g_touched = NULL; *hold = g_dirty_hold; g_dirty_hold = true;
    ctx_t c = { b->buf, W, 1.0f, X, Y, W, H }; *bc = c;
    return tm;
}
static void bake_end(bake_t *b, uint32_t key, int h, uint32_t *tm, bool hold) { g_touched = tm; g_dirty_hold = hold; b->key = key; b->h = h; b->ok = true; }
static void bake_blit(const bake_t *b, int X, int Y, int W, int H, uint16_t *fb, int stride) {
    for (int y = 0; y < H; y++) {
        int fy = Y + y;
        if ((unsigned)fy >= (unsigned)TANK_H) continue;
        memcpy(fb + fy * stride + X, b->buf + y * W, (size_t)W * 2);
        touch_span_f(X, X + W - 1, fy);
    }
}
/* a gift offered to us: its card (render.h) - baked (THE CARDS BAKED, above), its portrait and the time bar's fill live */
static void gift_paint(ctx_t *c, const tank_t *t) {
    const gift_book_t *g = gift_of(t);
    const passport_t *pp = &g->ask.pp;
    fish_t f; memset(&f, 0, sizeof f); passport_to_fish(pp, &f);
    const uint32_t GOLD = 0xe8c872, INK = 0x13324f, LBL = 0x7fa3b5;   /* (GIVE_GOLD, the passport's gold) */
    const int W = GIFT_CARD_W, H = GIFT_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    rect_fill(c, X, Y, W, H, 0x0e2233);
    rect_fill(c, X, Y, W, 26, INK);
    const char *title = "A GIFT FOR YOUR TANK";
    draw_text(c, X + (W - text_w(title, 2)) / 2, Y + 7, 2, GOLD, title);
    rect_edge(c, X, Y, W, H, GOLD); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, INK);
    const int PX = X + 12, PY = Y + 36, PW = 100, PH = 84;
    rect_fill(c, PX, PY, PW, PH, 0x0a1824); rect_edge(c, PX, PY, PW, PH, 0x9fd8e2);
    const int FX = PX + PW + 12;
    char from[24], flbl[40], form[24];
    int b = g->ask.badge;
    home_fields(pp->home_tank, pp->home_name, "FROM", flbl, sizeof flbl, from, sizeof from);
    snprintf(form, sizeof form, "%s %s", SHAPE_NAMES[f.shape % SHAPE_N], FINS_NAMES[f.fins % FINS_N]);
    static const char *const STAGE_W[4] = { "FRY", "JUVENILE", "ADULT", "ELDER" };
    struct { const char *lbl, *val; } FL[4] = { { "NAME", f.name }, { flbl, from }, { "FORM", form }, { "STAGE", STAGE_W[pp->stage & 3] } };
    for (int i = 0; i < 4; i++) {
        int fy = Y + 33 + i * 26;
        draw_text(c, FX, fy, 1, LBL, FL[i].lbl);
        int vx = FX;
        if (i == 1 && b >= 0 && b < LINK_BADGES) { link_badge_draw(c, FX + 6, fy + 17, 6, b, 255); vx += 16; }   /* the shape AND the word */
        draw_text(c, vx, fy + 10, 2, 0xffffff, FL[i].val);
    }
    const char *l1 = "IT WOULD LIVE HERE", *l2 = "FOR GOOD";
    draw_text(c, X + (W - text_w(l1, 2)) / 2, Y + 140, 2, 0xffffff, l1);
    draw_text(c, X + (W - text_w(l2, 2)) / 2, Y + 160, 2, 0xffffff, l2);
    rect_edge(c, X + 24, Y + 186, W - 48, 6, 0x3f6a72);              /* the time left to answer (its fill: live) */
    const int BY = Y + GIFT_BTN_Y, gap = W - 2 * GIFT_BTN_W - 24;
    button(c, X + 12, BY, GIFT_BTN_W, GIFT_BTN_H, 0x1c2f36, GOLD, "ACCEPT", 2);
    button(c, X + 12 + GIFT_BTN_W + gap, BY, GIFT_BTN_W, GIFT_BTN_H, 0x1c2f36, 0x9fd8e2, "NO THANKS", 2);
}
/* where it may come up (the 11-02 sweep: it hid under every page - no card, no sound, NO ANSWER CAME after 60 s): OVER any
   page - the keeper page, settings, the shop, VISITORS, MY TANKS, the battery page, the LINK page - and it waits under the
   tank's own setup (naming, placement), the reset prompt and ADVANCED (main: render_gift_allow). The keeper page draws it as
   part of itself (its kept frames stay kept); its hold counts from the first frame it is drawn, and that frame brings its cue */
static unsigned g_rframe;                            /* (THE PAGE KEPT's frame count, below) */
static bool g_gift_ok = true, g_gift_cue;
static unsigned g_gift_frame;
void render_gift_allow(bool ok) { g_gift_ok = ok; }
bool render_gift_up(const tank_t *t) { const gift_book_t *g = gift_of(t); return g && g->ask.on && g_gift_ok; }
bool render_card_over_tank(const tank_t *t) { return render_give_modal_step(t) != RENDER_GM_OFF || render_gift_up(t); }
int  render_gift_take_cue(void) { if (!g_gift_cue) return -1; g_gift_cue = false; return SND_ARRIVAL; }
void render_gift(const tank_t *t, uint16_t *fb, int stride) {
    gift_book_t *g = gift_of(t);
    if (!g || !g->ask.on || !g_gift_ok) return;
    if (!g_ms_in && g_rframe && g_gift_frame == g_rframe) return;   /* (the keeper page drew it this frame) */
    g_gift_frame = g_rframe;
    if (!g->ask.shown) { g->ask.shown = true; g->ask.seen = t->clock; g_gift_cue = true; }   /* first seen: its hold and its cue from now */
    ctx_t cc = ctx_full(fb, stride, 1.0f); ctx_t *c = &cc;
    const passport_t *pp = &g->ask.pp;
    const int W = GIFT_CARD_W, H = GIFT_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    if (g_bake_gift.buf) {
        uint32_t key = bake_hash(bake_hash(bake_hash(2166136261u, g->ask.uid), g->ask.nonce), (uint32_t)(g->ask.badge + 1));
        key = bake_hash_s(bake_hash(key, pp->home_tank), pp->home_name); key = bake_hash_s(key, pp->name);
        key = bake_hash(key, (uint32_t)pp->stage); key = bake_hash(key, (uint32_t)pp->shape | (uint32_t)pp->fins << 8);
        if (bake_stale(&g_bake_gift, key, H)) {
            ctx_t bc; bool hold; uint32_t *tm = bake_begin(&g_bake_gift, &bc, X, Y, W, H, &hold);
            gift_paint(&bc, t);
            bake_end(&g_bake_gift, key, H, tm, hold);
        }
        bake_blit(&g_bake_gift, X, Y, W, H, fb, stride);
    } else gift_paint(c, t);
    fish_t f; memset(&f, 0, sizeof f); passport_to_fish(pp, &f);      /* live: the fish in its box, the bar running down */
    const int PX = X + 12, PY = Y + 36, PW = 100, PH = 84;
    render_fish_portrait(fb, stride, PX + PW / 2.0f - PAGE_X, PY + PH / 2.0f - PAGE_Y, 1.3f, &f, PW - 20, t->clock);   /* (page coordinates) */
    float left = 1.0f - (float)(t->clock - g->ask.since) / (LINK_GIVE_ASK_MS / 1000.0f);   /* the time left to answer */
    left = left < 0 ? 0 : left > 1 ? 1 : left;
    if (left > 0) rect_fill(c, X + 25, Y + 187, (int)((W - 50) * left), 4, 0x9fd8e2);
}
int render_gift_hit(float x, float y) {
    const int W = GIFT_CARD_W, H = GIFT_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    const int BY = Y + GIFT_BTN_Y, gap = W - 2 * GIFT_BTN_W - 24;
    if (y >= BY - 10 && y < BY + GIFT_BTN_H + 14 && x >= X && x < X + W) return x < X + 12 + GIFT_BTN_W + gap / 2 ? GIFT_HIT_ACCEPT : GIFT_HIT_NO;
    return x >= X && x < X + W && y >= Y && y < Y + H ? GIFT_HIT_CARD : GIFT_HIT_NONE;
}
int render_passport_hit(float x, float y) {
    const int W = PASSPORT_CARD_W, H = PASSPORT_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    const int BY = Y + PASSPORT_BTN_Y, BW = PASSPORT_BTN_W, gap = W - 2 * BW - 24;
    if (y >= BY - 8 && y < BY + PASSPORT_BTN_H + 10) {
        if (x >= X + 4 && x < X + 12 + BW + gap / 2) return PASSPORT_HIT_DEPORT;
        if (x >= X + 12 + BW + gap / 2 && x < X + W - 4) return PASSPORT_HIT_COLLECT;
    }
    return x >= X && x < X + W && y >= Y && y < Y + H ? PASSPORT_HIT_CARD : PASSPORT_HIT_NONE;
}
#endif

/* the critters' cards (the snail's, the shrimp school's, the urchin's, the seahorse's, the crab's), their arrows and the crab's COLLECT:
   the world's, where it has critters (world_view.h) */
bool render_critter_here(const tank_t *t, int card) { const world_view_t *w = world_view(); return w->critter_here && w->critter_here(t, card); }
int  render_critter_step(const tank_t *t, int card, int dir) { const world_view_t *w = world_view(); return w->critter_step ? w->critter_step(t, card, dir) : card; }
int  render_critter_nav_hit(int card, float x, float y) { const world_view_t *w = world_view(); return w->critter_nav_hit ? w->critter_nav_hit(card, x, y) : 0; }
bool render_crab_collect_hit(const tank_t *t, float x, float y) { const world_view_t *w = world_view(); return w->crab_collect_hit && w->crab_collect_hit(t, x, y); }
static void tools_draw(ctx_t *c, const tank_t *t);   /* the toolbox, below */
static void next_draw(ctx_t *c, const tank_t *t);    /* the NEXT FISH arrow, below */
#if PT_NET
static void give_box_draw(ctx_t *c, const tank_t *t, int fish);   /* GIVE under the toolbox while linked (0.4), below */
#endif
void render_stats_card(const tank_t *t, int fish_idx, uint16_t *fb, int stride) {
    if (render_critter_here(t, fish_idx)) {
        ctx_t sc = ctx_full(fb, stride, 1.0f);
        world_view()->critter_card(&sc, t, fish_idx);
        return;
    }
#if PT_NET
    if (tank_is_visitor(fish_idx)) {                  /* a visitor: its passport */
        if (!t->vis_used[fish_idx - N_FISH_MAX]) return;
        ctx_t sc = ctx_full(fb, stride, 1.0f);
        passport_card_draw(&sc, fb, stride, t, fish_idx - N_FISH_MAX);
        return;
    }
#endif
    if (fish_idx == RENDER_CARD_SNAIL || fish_idx == RENDER_CARD_SHRIMP || fish_idx == RENDER_CARD_URCHIN
        || fish_idx == RENDER_CARD_SEAHORSE || fish_idx == RENDER_CARD_CRAB) return;   /* its critter is gone */
    if (fish_idx < 0 || fish_idx >= t->n_fish) return;
    ctx_t c = ctx_full(fb, stride, 1.0f);            /* card ignores night dimming */
    const fish_t *f = &t->fish[fish_idx];
    ring(&c, f->x, f->y, 17 * f->size, f->color);
    unsigned ep; const uint16_t *scene = render_scene_buf(&ep);
    if (g_card && scene) {
        if (fish_idx != g_card_fish || ep != g_card_epoch ||
            t->clock - g_card_t > 0.25f || t->clock < g_card_t) {
            for (int y = 0; y < RENDER_CARD_H; y++)
                memcpy(g_card + y * RENDER_CARD_W,
                       scene + (RENDER_CARD_Y + y) * TANK_W + RENDER_CARD_X, RENDER_CARD_W * 2);
            ctx_t cc = { g_card, RENDER_CARD_W, 1.0f, RENDER_CARD_X, RENDER_CARD_Y, RENDER_CARD_W, RENDER_CARD_H };
            card_draw(cc, t, fish_idx);
            g_card_fish = fish_idx; g_card_epoch = ep; g_card_t = t->clock;
        }
        for (int y = 0; y < RENDER_CARD_H; y++) {
            memcpy(fb + (RENDER_CARD_Y + y) * stride + RENDER_CARD_X,
                   g_card + y * RENDER_CARD_W, RENDER_CARD_W * 2);
            touch_span_f(RENDER_CARD_X, RENDER_CARD_X + RENDER_CARD_W - 1, RENDER_CARD_Y + y);
        }
    } else card_draw(c, t, fish_idx);
    tools_draw(&c, t);
    next_draw(&c, t);
#if PT_NET
    give_box_draw(&c, t, fish_idx);
#endif
}

/* ---- the toolbox (2026-10-01) ----
 * Drawn every frame, uncached - so the backdrop is the card's look by bit
 * shifts, not blends: an eighth of the water through, the rest the card's
 * dark (~8.7k pixels for the price of a copy; blended, the card's 28k cost
 * ~7 ms on the device). */
void shade_box(ctx_t *c, int x, int y, int w, int h) {
    const uint16_t base = rgb565(0x04141a, 0.875f);
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)(yy - c->oy) >= (unsigned)c->h) continue;
        int x0 = x < c->ox ? c->ox : x, x1 = x + w - 1 > c->ox + c->w - 1 ? c->ox + c->w - 1 : x + w - 1;
        uint16_t *p = &CTX_PX(c, x0, yy);
        for (int xx = x0; xx <= x1; xx++, p++) { uint16_t n = FB_SWAP16(*p); *p = FB_SWAP16((uint16_t)(((n >> 3) & 0x18E3) + base)); }
        TOUCH_SPAN(c, x0, x1, yy);
    }
}
/* the toolbox's tools, by world (P2b): the aquarium's two - the sponge and the scissors, as pocket-tank's, their icons
 * at 2x in halves - and the terrarium's three, the mister third (P2a: the icons at 1x to fit). The fish do not shed:
 * no mister in their box (tank_set_tool refuses it too) */
static void tools_draw(ctx_t *c, const tank_t *t) {
    const int X = RENDER_TOOLS_X, Y = RENDER_TOOLS_Y, W = RENDER_TOOLS_W, H = RENDER_TOOLS_H;
    shade_box(c, X, Y, W, H);
    rect_edge(c, X, Y, W, H, 0x9fd8e2); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    const world_view_t *w = world_view();
    const int *TOOL = w->tools, TOOLS_N = w->n_tools;          /* the world's tools, left to right */
    const int TOOL_BTN_W = (RENDER_TOOLS_W - 16 - 6 * (TOOLS_N - 1)) / TOOLS_N, TOOL_BTN_H = RENDER_TOOLS_H - 16;
    const int s = TOOLS_N == 2 ? 2 : 1;                       /* the icons' scale */
    for (int k = 0; k < TOOLS_N; k++) {
        int bx = X + 8 + k * (TOOL_BTN_W + 6), by = Y + 8;
        bool held = t->tool == TOOL[k];
        rect_fill(c, bx, by, TOOL_BTN_W, TOOL_BTN_H, held ? 0x356670 : 0x14252b);
        uint32_t edge = held ? 0xffffff : 0x4d6a72;
        rect_edge(c, bx, by, TOOL_BTN_W, TOOL_BTN_H, edge);
        if (held) rect_edge(c, bx + 1, by + 1, TOOL_BTN_W - 2, TOOL_BTN_H - 2, edge);
        const icon_t *ic = TOOL[k] == TOOL_SPONGE ? &icon_tool_sponge : TOOL[k] == TOOL_SCISSORS ? &icon_tool_scissors : &icon_tool_mister;   /* (the mister's art, P3) */
        blit_icon_scaled(c, bx + (TOOL_BTN_W - ic->w * s) / 2, by + (TOOL_BTN_H - ic->h * s) / 2, ic, s, true);
    }
    /* the LOCK beside it (0.3.5) */
    const int LX = RENDER_LOCK_X, LY = RENDER_LOCK_Y, LW = RENDER_LOCK_W, LH = RENDER_LOCK_H;
    if (t->tools_locked) rect_fill(c, LX, LY, LW, LH, 0x356670); else shade_box(c, LX, LY, LW, LH);
    rect_edge(c, LX, LY, LW, LH, t->tools_locked ? 0xffffff : 0x9fd8e2);
    rect_edge(c, LX + 1, LY + 1, LW - 2, LH - 2, t->tools_locked ? 0xffffff : 0x1c2f36);
    const icon_t *lk = t->tools_locked ? &icon_tool_lock_closed : &icon_tool_lock_open;
    blit_icon(c, LX + (LW - lk->w) / 2, LY + (LH - lk->h) / 2, lk, 255);
}
/* an arrow box in the NEXT FISH arrow's dress (the critter cards', the visitor collection's pages': dir -1 points left, +1 right) */
void crit_nav_box(ctx_t *c, int bx, int by, int dir, uint32_t rgb) {
    shade_box(c, bx, by, RENDER_CRIT_NAV_W, RENDER_CRIT_NAV_W);
    rect_edge(c, bx, by, RENDER_CRIT_NAV_W, RENDER_CRIT_NAV_W, rgb); rect_edge(c, bx + 1, by + 1, RENDER_CRIT_NAV_W - 2, RENDER_CRIT_NAV_W - 2, 0x1c2f36);
    int cx = bx + RENDER_CRIT_NAV_W / 2, cy = by + RENDER_CRIT_NAV_W / 2;
    for (int i = 0; i < 9; i++) {                            /* a solid triangle, 9 wide, 17 tall, pointing its way */
        int hh = 8 - i;
        rect_fill(c, dir > 0 ? cx - 4 + i : cx + 4 - i, cy - hh, 1, 2 * hh + 1, rgb);
    }
}
/* the NEXT FISH arrow (0.3.9) beside the card's top right: drawn every
   frame like the lock (outside the card's cache) */
static void next_draw(ctx_t *c, const tank_t *t) {
    const int X = RENDER_NEXT_X, Y = RENDER_NEXT_Y, W = RENDER_NEXT_W, H = RENDER_NEXT_H;
    const uint32_t rgb = t->n_fish > 1 ? 0x9fd8e2 : 0x2c4a52;
    shade_box(c, X, Y, W, H);
    rect_edge(c, X, Y, W, H, rgb); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    int cx = X + W / 2, cy = Y + H / 2;
    for (int i = 0; i < 9; i++) {                           /* a solid triangle, 9 wide, 17 tall, pointing on */
        int hh = 8 - i;
        rect_fill(c, cx - 4 + i, cy - hh, 1, 2 * hh + 1, rgb);
    }
}
bool render_next_hit(float x, float y) {
    return x >= RENDER_NEXT_X - RENDER_SIDE_GAP / 2 && x < RENDER_NEXT_X + RENDER_NEXT_W + 16 &&
           y >= RENDER_NEXT_Y - 8 && y < RENDER_NEXT_Y + RENDER_NEXT_H + 12;
}
int render_next_fish(const tank_t *t, int fish_idx) {
    if (fish_idx < 0 || fish_idx >= t->n_fish) return fish_idx;
    return (fish_idx + 1) % t->n_fish;
}
#if PT_NET
#define GIVE_GOLD 0xe8c872
enum { GV_HIDDEN, GV_GIVE, GV_DIM, GV_ASKING, GV_LEAVING };   /* (the GIVE section below) */
/* GIVE on the stat card (0.4): the keeper page card's button in a box of its own, its states the same
   (give_look / give_button, the GIVE section below) - while the tank is linked, for a fish of ours */
static int give_look(const tank_t *t, int fish);
static void give_button(ctx_t *c, int x, int y, int w, int h, int look, int scale);
static void give_box_draw(ctx_t *c, const tank_t *t, int fish) {
    int look = give_look(t, fish);
    if (look == GV_HIDDEN) return;
    const int X = RENDER_GIVE_X, Y = RENDER_GIVE_Y, W = RENDER_GIVE_W, H = RENDER_GIVE_H;
    shade_box(c, X, Y, W, H);
    if (look == GV_LEAVING) {                        /* the stamp, as on the keeper page */
        rect_edge(c, X, Y, W, H, GIVE_GOLD); rect_edge(c, X + 3, Y + 3, W - 6, H - 6, GIVE_GOLD);
        draw_text(c, X + (W - text_w("GIVEN", 2)) / 2, Y + (H - 14) / 2, 2, GIVE_GOLD, "GIVEN");
        return;
    }
    give_button(c, X + 4, Y + 4, W - 8, H - 8, look, 2);
}
bool render_give_hit(const tank_t *t, int fish_idx, float x, float y) {
    if (give_look(t, fish_idx) == GV_HIDDEN) return false;
    return x >= RENDER_GIVE_X - 4 && x < RENDER_GIVE_X + RENDER_GIVE_W + 8 && y >= RENDER_GIVE_Y - 4 && y < RENDER_GIVE_Y + RENDER_GIVE_H + 10;
}
#endif
bool render_lock_hit(float x, float y) {
    return y >= RENDER_TOOLS_Y && x >= RENDER_LOCK_X - RENDER_SIDE_GAP / 2 && x < RENDER_LOCK_X + RENDER_LOCK_W + 16;
}
int render_tools_hit(float x, float y) {
    const int X = RENDER_TOOLS_X, W = RENDER_TOOLS_W;
    if (y < RENDER_TOOLS_Y || x < X - RENDER_CARD_HIT_SIDE || x >= X + W + RENDER_SIDE_GAP / 2) return -1;   /* (the right: half the gap, the lock has the rest) */
    const world_view_t *w = world_view();                                                                   /* the box in equal parts, one a tool (the aquarium's halves, the terrarium's thirds) */
    for (int k = 0; k < w->n_tools - 1; k++) if (x < X + (k + 1) * W / w->n_tools) return w->tools[k];
    return w->tools[w->n_tools - 1];
}

/* the chip: the tool at 1x and DONE, top left where the card would be */
#ifdef TANK_ROUND                                    /* the bowl: up in the left shoulder, inside the glass */
#define TOOL_CHIP_X 84
#define TOOL_CHIP_Y 72
#else
#define TOOL_CHIP_X RENDER_CARD_X
#define TOOL_CHIP_Y RENDER_CARD_Y
#endif
#define TOOL_CHIP_H 40
static int tool_chip_w(void) { return 8 + 24 + 8 + text_w("DONE", 2) + 10; }
void render_tool_chip(const tank_t *t, int fish_idx, uint16_t *fb, int stride) {
    if (t->tool == TOOL_HAND || (fish_idx >= 0 && fish_idx < t->n_fish)) return;
    /* a stroke on the glass: the chip steps aside until the finger lifts, so
     * the film it covers shows as the sponge takes it (0.3.5, Strato: "the
     * sponge should be able to clean algae behind the DONE button") */
    if (tank_stroking(t)) return;                    /* (not a press: the tap on DONE itself keeps it in view) */
    ctx_t c = ctx_full(fb, stride, 1.0f);
    const int X = TOOL_CHIP_X, Y = TOOL_CHIP_Y, W = tool_chip_w(), H = TOOL_CHIP_H;
    shade_box(&c, X, Y, W, H);
    rect_edge(&c, X, Y, W, H, 0xffffff); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x9fd8e2);
    const icon_t *ic = t->tool == TOOL_SCISSORS ? &icon_tool_scissors : t->tool == TOOL_MISTER ? &icon_tool_mister : &icon_tool_sponge;
    blit_icon(&c, X + 8, Y + (H - ic->h) / 2, ic, 255);
    draw_text(&c, X + 8 + 24 + 8, Y + (H - 14) / 2, 2, 0xffffff, "DONE");   /* (every tool's icon is 24 px) */
}
bool render_tool_chip_hit(const tank_t *t, float x, float y) {
    return t->tool != TOOL_HAND && x < TOOL_CHIP_X + tool_chip_w() + 16 && y < TOOL_CHIP_Y + TOOL_CHIP_H + 24;
}


/* ---- a small pixel font (2026-09-11) ----
 * 5x7 glyphs drawn as scale x scale blocks, so one table reads at 2x for a
 * caption and 3x for a button. One table, every printable ASCII character
 * (render.h): the tank's text draws its upper-case set from it (glyph), the
 * update page's case-sensitive text all of it - a network's name or a
 * passphrase has to show exactly as typed. Rows are 5 bits, high bit left. */
const uint8_t render_font5x7[95][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /*   */
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 }, /* ! */
    { 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00 }, /* " */
    { 0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a }, /* # */
    { 0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04 }, /* $ */
    { 0x19, 0x1a, 0x02, 0x04, 0x08, 0x0b, 0x13 }, /* % */
    { 0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d }, /* & */
    { 0x0c, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 }, /* ' */
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 }, /* ( */
    { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 }, /* ) */
    { 0x00, 0x04, 0x15, 0x0e, 0x15, 0x04, 0x00 }, /* * */
    { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 }, /* + */
    { 0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08 }, /* , */
    { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 }, /* - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c }, /* . */
    { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 }, /* / */
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }, /* 0 */
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* 1 */
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }, /* 2 */
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e }, /* 3 */
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, /* 4 */
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, /* 6 */
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }, /* 8 */
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }, /* 9 */
    { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 }, /* : */
    { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x04, 0x08 }, /* ; */
    { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 }, /* < */
    { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00 }, /* = */
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, /* > */
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 }, /* ? */
    { 0x0e, 0x11, 0x01, 0x0d, 0x15, 0x15, 0x0e }, /* @ */
    { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, /* A */
    { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e }, /* B */
    { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e }, /* C */
    { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e }, /* D */
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f }, /* E */
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 }, /* F */
    { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f }, /* G */
    { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, /* H */
    { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* I */
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c }, /* J */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f }, /* L */
    { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, /* N */
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* O */
    { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 }, /* P */
    { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d }, /* Q */
    { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 }, /* R */
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e }, /* S */
    { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* U */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 }, /* V */
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a }, /* W */
    { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 }, /* X */
    { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 }, /* Y */
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f }, /* Z */
    { 0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e }, /* [ */
    { 0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01 }, /* \ */
    { 0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e }, /* ] */
    { 0x04, 0x0a, 0x11, 0x00, 0x00, 0x00, 0x00 }, /* ^ */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f }, /* _ */
    { 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00 }, /* ` */
    { 0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f }, /* a */
    { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1e }, /* b */
    { 0x00, 0x00, 0x0e, 0x10, 0x10, 0x11, 0x0e }, /* c */
    { 0x01, 0x01, 0x0d, 0x13, 0x11, 0x11, 0x0f }, /* d */
    { 0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e }, /* e */
    { 0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08 }, /* f */
    { 0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e }, /* g */
    { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11 }, /* h */
    { 0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e }, /* i */
    { 0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c }, /* j */
    { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 }, /* k */
    { 0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* l */
    { 0x00, 0x00, 0x1a, 0x15, 0x15, 0x11, 0x11 }, /* m */
    { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 }, /* n */
    { 0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e }, /* o */
    { 0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10 }, /* p */
    { 0x00, 0x00, 0x0d, 0x13, 0x0f, 0x01, 0x01 }, /* q */
    { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 }, /* r */
    { 0x00, 0x00, 0x0e, 0x10, 0x0e, 0x01, 0x1e }, /* s */
    { 0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06 }, /* t */
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d }, /* u */
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04 }, /* v */
    { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a }, /* w */
    { 0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11 }, /* x */
    { 0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e }, /* y */
    { 0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f }, /* z */
    { 0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02 }, /* { */
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* | */
    { 0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08 }, /* } */
    { 0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00 }, /* ~ */
};
/* the tank's own set: upper case (lower case maps to it), digits, a little punctuation - anything else advances
 * a cell. Its '*' is a sparkle (the family page's hidden copy, 0.4), not the ASCII asterisk. */
static const char FONT_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789?!.,-:'/+%~*<";
static const uint8_t FONT_STAR[7] = { 0x00, 0x15, 0x0e, 0x1f, 0x0e, 0x15, 0x00 };
static const uint8_t *glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (!ch || !strchr(FONT_CHARS, ch)) return NULL;
    return ch == '*' ? FONT_STAR : render_font5x7[ch - 32];
}
int text_w(const char *s, int scale) { int n = (int)strlen(s); return n ? n * 6 * scale - scale : 0; }
void draw_text(ctx_t *c, int x, int y, int scale, uint32_t rgb, const char *s) {
    src_t col = src_color(rgb, c->dim);
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (g[r] & (0x10 >> k))
                    for (int yy = 0; yy < scale; yy++)
                        span(c, x + k * scale, x + k * scale + scale - 1, y + r * scale + yy, &col, 255);
    }
}

/* 8 px text (2026-09-16, the settings page's version line): the 5x7 font at
 * scale 1 with its second row drawn twice. Row 1 is a vertical-stroke row in
 * every glyph the line can hold (letters, digits, '-', '.'), so the doubling
 * only lengthens strokes and never thickens a bar - Strato wanted the line
 * smaller than the page's text (14 px) but not the font's 7 px. */
static void draw_text_8px(ctx_t *c, int x, int y, uint32_t rgb, const char *s) {
    src_t col = src_color(rgb, c->dim);
    for (; *s; s++, x += 6) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        for (int r = 0, yy = y; r < 7; r++, yy++) {
            for (int rep = r == 1 ? 2 : 1; rep; rep--, yy += rep ? 1 : 0)
                for (int k = 0; k < 5; k++)
                    if (g[r] & (0x10 >> k)) span(c, x + k, x + k, yy, &col, 255);
        }
    }
}

/* ---- reset confirm (2026-09-11) ----
 * The keeper's "start over" (device: hold BOOT 5 s, then tap the glass - 0.3.9.2; sim: X, or the 0 key held 5 s + a click). A
 * modal panel over the live tank - the fish keep swimming behind it - that
 * asks before the save is wiped. Opaque stores only, so a frame with it up
 * costs about what the milestones page does. */
void rect_fill(ctx_t *c, int x, int y, int w, int h, uint32_t rgb) {
    src_t f = src_color(rgb, 1.0f);
    for (int yy = y; yy < y + h; yy++) span(c, x, x + w - 1, yy, &f, 255);
}
void rect_edge(ctx_t *c, int x, int y, int w, int h, uint32_t rgb) {
    src_t e = src_color(rgb, 1.0f);
    span(c, x, x + w - 1, y, &e, 255); span(c, x, x + w - 1, y + h - 1, &e, 255);
    for (int yy = y + 1; yy < y + h - 1; yy++) { px(c, x, yy, e.v); px(c, x + w - 1, yy, e.v); }
}
void button(ctx_t *c, int x, int y, int W, int H, uint32_t fill, uint32_t edge, const char *label, int scale) {
    rect_fill(c, x, y, W, H, fill);
    rect_edge(c, x, y, W, H, edge); rect_edge(c, x + 1, y + 1, W - 2, H - 2, edge);
    draw_text(c, x + (W - text_w(label, scale)) / 2, y + (H - 7 * scale) / 2, scale, 0xffffff, label);
}
/* the same primitives for panels built elsewhere (render.h) */
int  render_text_w(const char *s, int scale) { return text_w(s, scale); }
void render_text(uint16_t *fb, int stride, int x, int y, int scale, uint32_t rgb, const char *s) {
    ctx_t c = ctx_page(fb, stride); draw_text(&c, x, y, scale, rgb, s);
}
void render_rect(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t rgb) {
    ctx_t c = ctx_page(fb, stride); rect_fill(&c, x, y, w, h, rgb);
}
void render_rect_edge(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t rgb) {
    ctx_t c = ctx_page(fb, stride); rect_edge(&c, x, y, w, h, rgb);
}
void render_rect_blend(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t rgb, int alpha) {
    ctx_t c = ctx_page(fb, stride); src_t s = src_color(rgb, 1.0f);
    for (int yy = y; yy < y + h; yy++) span(&c, x, x + w - 1, yy, &s, alpha);
}
void render_ring(uint16_t *fb, int stride, float cx, float cy, float r, uint32_t rgb) {
    ctx_t c = ctx_page(fb, stride); ring(&c, cx, cy, r, rgb);
}
void render_button(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t fill, uint32_t edge, const char *label, int scale) {
    ctx_t c = ctx_page(fb, stride); button(&c, x, y, w, h, fill, edge, label, scale);
}
void render_glyph(uint16_t *fb, int stride, int x, int y, int scale, uint32_t rgb, const uint8_t *rows) {
    ctx_t c = ctx_page(fb, stride); src_t col = src_color(rgb, 1.0f);
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
            if (rows[r] & (0x10 >> k))
                for (int yy = 0; yy < scale; yy++)
                    span(&c, x + k * scale, x + k * scale + scale - 1, y + r * scale + yy, &col, 255);
}
/* the species the pages draw: the world's default pack (the keeper page, the shop's offers, the setup's portraits) - and its body type's
   art (body.h): a page asks the body, never which world it is */
static const species_t *page_species(void) { return species_get(SPECIES_DEFAULT); }
static const body_ops_t *page_body(void) { return body_of(page_species()); }
const body_ops_t *body_of(const species_t *sp) {
    const world_view_t *w = world_view();
    for (int i = 0; i < w->n_bodies; i++) if (sp && w->bodies[i]->plan == sp->plan) return w->bodies[i];
    return w->bodies[0];
}
const body_ops_t *tank_body(const tank_t *t, int i) { (void)i; return body_of(tank_species(t)); }
/* the box a CLASSIC fish of a stage takes (bodies/fish fish_box(SHAPE_CLASSIC, FINS_CLASSIC, stage)): b = x0, x1, y0, y1 in its local px - the
   unit the pages' portrait zones are laid out in, so any body fits the room a fish has. Its outline and its classic fins as fish_fins draws
   them at rest (the tail, and the grown fish's dorsal crest, the elder's both longer) */
void ref_box(stage_t stage, float b[4]) {
    static const float BX[10] = { 16, 10, 2, -8, -15, -18, -15, -8, 2, 10 }, BY[10] = { 0, -7, -10, -9, -6, 0, 6, 9, 10, 7 };
    const float R = -11, H = 10, FH = 10, K = 1.0f;
    const bool elder = stage == STAGE_ELDER, grown = stage >= STAGE_ADULT;
    const float tail_len = elder ? 1.22f : 1.0f, tail = 0;
    float fx[2][3], fy[2][3]; int nf = 0;
    fx[0][0] = R; fx[0][1] = (R - 15) * tail_len; fx[0][2] = (R - 15) * tail_len;
    fy[0][0] = 0; fy[0][1] = -10 - tail * 8; fy[0][2] = 10 + tail * 8; nf++;
    if (grown) { float db = -H * 8 / 10, dd = db - FH * (elder ? 9 : 5) / 10;
                 fx[1][0] = 6 * K; fx[1][1] = -2 * K; fx[1][2] = -9 * K; fy[1][0] = db; fy[1][1] = dd; fy[1][2] = db; nf++; }
    b[0] = b[2] = 1e9f; b[1] = b[3] = -1e9f;
    for (int k = -1; k < nf; k++) {
        const float *xs = k < 0 ? BX : fx[k], *ys = k < 0 ? BY : fy[k];
        int n = k < 0 ? 10 : 3;
        for (int i = 0; i < n; i++) {
            b[0] = fminf(b[0], xs[i]); b[1] = fmaxf(b[1], xs[i]);
            b[2] = fminf(b[2], ys[i]); b[3] = fmaxf(b[3], ys[i]);
        }
    }
}
static void draw_still(uint16_t *fb, int stride, float x, float y, float size, fish_t *f, const fish_t *form, double clock, float room) {
    ctx_t c = ctx_page(fb, stride);
    const species_t *sp = page_species();
    body_of(sp)->still(&c, sp, x, y, size, f, form, clock, room);
}
void render_fish_preview(uint16_t *fb, int stride, float x, float y, float size,
                         uint32_t body, uint32_t fin, uint32_t accent, const fish_t *form, float room, double clock) {
    fish_t f; memset(&f, 0, sizeof f);
    f.stage = STAGE_ADULT;                       /* grown: the crest shows the fin colour */
    f.color = body; f.fin = fin; f.accent = accent;
    draw_still(fb, stride, x, y, size, &f, form, clock, room);
}
void render_fish_portrait(uint16_t *fb, int stride, float x, float y, float size, const fish_t *who, float room, double clock) {
    fish_t f; memset(&f, 0, sizeof f);
    f.stage = who->stage;
    f.color = who->color; f.fin = who->fin; f.accent = who->accent;
    draw_still(fb, stride, x, y, size, &f, who, clock, room);
}
/* a fish AS IT IS NOW (its own stage) in the given colours - the milestones
   rows and its card: a fry shows a fry's plain tail, its fins come in at
   ADULT as they do in the tank (0.4; the rows drew every fish grown before).
   `cap_adult`: the rows draw an elder as an adult, as they always have - an
   elder's long tail would reach past the row's left edge (the watch's page
   starts 19 px off the glass); the card has the room for it */
static void fish_as_is(uint16_t *fb, int stride, float x, float y, float size, const fish_t *who,
                       uint32_t body, uint32_t fin, uint32_t accent, float room, bool cap_adult, double clock) {
    fish_t f; memset(&f, 0, sizeof f);
    f.stage = cap_adult && who->stage > STAGE_ADULT ? STAGE_ADULT : who->stage;
    f.color = body; f.fin = fin; f.accent = accent;
    draw_still(fb, stride, x, y, size, &f, who, clock, room);
}
void render_fish_sample(uint16_t *fb, int stride, float x, float y, float size, const fish_t *who, double clock) {
    fish_t f; memset(&f, 0, sizeof f);
    f.stage = who->stage;
    f.color = who->color; f.fin = who->fin; f.accent = who->accent;
    draw_still(fb, stride, x, y, size, &f, who, clock, 0);
}
void render_confirm_reset(uint16_t *fb, int stride, float frac) {
    ctx_t c = ctx_page(fb, stride);              /* ignores night dimming, like the card */
    const int X = RENDER_CONFIRM_X, Y = RENDER_CONFIRM_Y, W = RENDER_CONFIRM_W, H = RENDER_CONFIRM_H;
    rect_fill(&c, X, Y, W, H, 0x04141a);
    rect_edge(&c, X, Y, W, H, 0x9fd8e2); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    const char *title = world_view()->text.reset_title;
    draw_text(&c, X + (W - text_w(title, 3)) / 2, Y + 20, 3, 0xffffff, title);
    const char *l1 = world_view()->text.reset_line, *l2 = "EVERYTHING ELSE IS LOST";
    draw_text(&c, X + (W - text_w(l1, 2)) / 2, Y + 58, 2, 0x9fd8e2, l1);
    draw_text(&c, X + (W - text_w(l2, 2)) / 2, Y + 78, 2, 0x9fd8e2, l2);
    /* NO is the calm one; YES wears the stress red */
    button(&c, RENDER_CONFIRM_NO_X,  RENDER_CONFIRM_BTN_Y, RENDER_CONFIRM_BTN_W, RENDER_CONFIRM_BTN_H, 0x1c2f36, 0x9fd8e2, "NO", 3);
    button(&c, RENDER_CONFIRM_YES_X, RENDER_CONFIRM_BTN_Y, RENDER_CONFIRM_BTN_W, RENDER_CONFIRM_BTN_H, 0x7a2028, 0xf25b65, "YES", 3);
    /* the prompt lets itself go: a bar draining toward the timeout */
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    const int bw = W - 48, bx = X + 24, by = Y + H - 16;
    rect_fill(&c, bx, by, bw, 3, 0x2a3f45);
    int fw = (int)(bw * frac + 0.5f);
    if (fw > 0) rect_fill(&c, bx, by, fw, 3, 0x9fd8e2);
}
int render_confirm_hit(float x, float y) {
    x -= PAGE_X; y -= PAGE_Y;                    /* the page's own coordinates */
    const int m = 10;                                   /* a fingertip's slop around each button */
    if (y < RENDER_CONFIRM_BTN_Y - m || y >= RENDER_CONFIRM_BTN_Y + RENDER_CONFIRM_BTN_H + m) return 0;
    if (x >= RENDER_CONFIRM_NO_X - m  && x < RENDER_CONFIRM_NO_X  + RENDER_CONFIRM_BTN_W + m) return -1;
    if (x >= RENDER_CONFIRM_YES_X - m && x < RENDER_CONFIRM_YES_X + RENDER_CONFIRM_BTN_W + m) return 1;
    return 0;
}

/* ---- the battery page (2026-09-24): a tap on the pill. The snail card's
 * dress, centered over the live tank: the battery large with its percent,
 * the state in words (the one thing no color has to carry), the estimate,
 * then two or three rows - since the cable moved, the screen-on time on
 * this charge, what a full charge lasts - and the cell voltage, dim, for
 * the curious. Rows the clock cannot answer are left out. ---- */
void render_battery_info(uint16_t *fb, int stride, const bat_info_t *bi, double clock) {
    ctx_t c = ctx_full(fb, stride, 1.0f);
    bool pw = BAT_ON_POWER(bi->state);
    char lab[3][16], val[3][32], d[16]; int nr = 0;
    if (bi->since_min >= 0) {
        snprintf(lab[nr], sizeof lab[nr], pw ? "PLUGGED IN" : "UNPLUGGED");
        if (bi->since_min < 1) snprintf(val[nr], sizeof val[nr], "JUST NOW");
        else { battery_fmt_dur(d, sizeof d, bi->since_min); snprintf(val[nr], sizeof val[nr], "%s AGO", d); }
        nr++;
    }
    if (!pw && bi->awake_min >= 0) {
        snprintf(lab[nr], sizeof lab[nr], "SCREEN ON");
        battery_fmt_dur(val[nr], sizeof val[nr], bi->awake_min); nr++;
    }
    if (bi->life_min > 0) {
        snprintf(lab[nr], sizeof lab[nr], "BATTERY LIFE");
        battery_fmt_dur(d, sizeof d, bi->life_min); snprintf(val[nr], sizeof val[nr], "~%s", d); nr++;
    }
    const int W = 336, H = (bi->mv > 0 ? 170 : 154) + nr * 26, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(&c, X, X + W - 1, y, &bg, 240);
    rect_edge(&c, X, Y, W, H, 0x9fd8e2); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    /* the battery, its percent beside it */
    char pct[16]; snprintf(pct, sizeof pct, "%d%%", bi->pct < 0 ? 0 : bi->pct > 100 ? 100 : bi->pct);
    const int BW = 84, BH = 36, TS = 5;
    int bolt = pw ? bolt_room(BH) : 0;
    int gw = bolt + BW + 6 + 20 + text_w(pct, TS), gx = X + (W - gw) / 2, gy = Y + 22;
    draw_battery(&c, gx + bolt, gy, BW, BH, 2, bi->pct / 100.0f, bi->state, clock);
    draw_text(&c, gx + bolt + BW + 6 + 20, gy + (BH - 7 * TS) / 2, TS, 0xffffff, pct);
    /* the state, then what it means in time */
    const char *st = bi->state == BAT_CHARGING ? "CHARGING" : bi->state == BAT_FULL ? "FULLY CHARGED"
                   : bi->state == BAT_PLUGGED ? "PLUGGED IN" : "ON BATTERY";
    draw_text(&c, X + (W - text_w(st, 3)) / 2, Y + 78, 3, 0xffffff, st);
    char est[48];
    if (bi->state == BAT_CHARGING) { battery_fmt_dur(d, sizeof d, bi->left_min); snprintf(est, sizeof est, "FULL IN ABOUT %s", d); }
    else if (bi->state == BAT_FULL) snprintf(est, sizeof est, "READY TO UNPLUG");
    else if (bi->state == BAT_PLUGGED) snprintf(est, sizeof est, "NOT CHARGING RIGHT NOW");
    else if (bi->left_min < 5) snprintf(est, sizeof est, "ALMOST EMPTY");
    else { battery_fmt_dur(d, sizeof d, bi->left_min); snprintf(est, sizeof est, "ABOUT %s LEFT", d); }
    draw_text(&c, X + (W - text_w(est, 2)) / 2, Y + 110, 2, 0x9fd8e2, est);
    rect_fill(&c, X + 20, Y + 138, W - 40, 1, 0x1c2f36);
    for (int i = 0; i < nr; i++) {
        int ry = Y + 152 + i * 26;
        draw_text(&c, X + 20, ry, 2, 0x9fd8e2, lab[i]);
        draw_text(&c, X + W - 20 - text_w(val[i], 2), ry, 2, 0xffffff, val[i]);
    }
    if (bi->mv > 0) {
        char v[32]; snprintf(v, sizeof v, "%d.%02d V", bi->mv / 1000, bi->mv % 1000 / 10);
        draw_text_8px(&c, X + (W - (int)strlen(v) * 6 + 1) / 2, Y + H - 20, 0x5f7f86, v);
    }
}

/* ---- milestones page (2026-09-13 redesign: an achievement wall) ----
 * Rows of 40 px: the fish (render_fish_preview at its real size, so growth
 * shows), its name, a growth strip, then six 32 px badges at a 40 px pitch.
 * The tank row repeats the shape with the population strip. A locked badge
 * is the art as a flat grey silhouette (blit_icon_locked); a badge earned
 * since the keeper last closed the page wears a two-tone ring. Tapping a
 * badge / a name / a strip opens a small detail modal in the reset prompt's
 * dress (the art at 2x, a title, the words); the next tap anywhere closes
 * it back to the page. Render-local state, cleared by render_milestones_leave. */
/* The BOWL (TANK_ROUND; Strato on the glass, 2026-10-01: the top fish's
 * portrait and its last badge were cut by the circle - "nudge this down",
 * SETTINGS "to the top, dead center as it would fit in that opening",
 * UPGRADES and CLOSE "closer together centered"): the rows start where the
 * circle is wide enough for them and stand 34 px apart, SETTINGS sits alone
 * in the cap above them, the other two side by side at the foot. */
/* (the page's numbers, MSP_*: render.h) */

/* the tank row's pages (2026-09-30, the seventh badge): six badges a page;
   with more, a small arrow at the row's right end (or a sideways swipe along
   the row) flips to the next page, pointing back from the last. */
static int g_ms_tpage;                  /* the tank row's page (kept while a modal opens and closes; 0 on leaving the page) */
/* which tank badges show: the world's rule (a bought thing's badge only once the thing is bought, ...); all of them when it has none */
static bool tank_badge_shown(const tank_t *t, int i) { const world_view_t *w = world_view(); return !w->tank_badge_shown || w->tank_badge_shown(t, i); }
static int tank_badge_n(const tank_t *t) {
    int n = 0;
    for (int i = 0; i < world_view()->n_tank_badges; i++) n += tank_badge_shown(t, i);
    return n;
}
/* the k-th badge that shows (k < tank_badge_n) */
static const badge_t *tank_badge(const tank_t *t, int k) {
    const world_view_t *w = world_view();
    for (int i = 0; i < w->n_tank_badges; i++)
        if (tank_badge_shown(t, i) && k-- == 0) return &w->tank_badges[i];
    return &w->tank_badges[0];
}
static int tank_pages(const tank_t *t) { return (tank_badge_n(t) + MSP_PER_ROW - 1) / MSP_PER_ROW; }
/* the detail modal (a tap on a badge / name / strip): what to show until
 * the next tap. caption[0] == 0 means no modal. */
static char g_ms_caption[72], g_ms_title[16];
static char g_ms_caption2[32];       /* the caption's second line (the fry checklist's sentences), or empty */
static char g_ms_sub[32];            /* a line under the caption (the fry checklist's progress), or empty */
static bool g_ms_lit;
static const icon_t *g_ms_icon;      /* the badge's art, or NULL */
static int  g_ms_fish = -1;          /* a fish's own sprite instead, or -1 */
static bool g_ms_fry;                /* the fry-to-be (a silhouette) instead */
static int  g_ms_kind = -1;          /* a gate's modal: its kind (the HOW? button shows), or -1 */
static bool g_ms_tip;                /* the gate's tip page is up instead of its modal */
/* where the modal came from (2026-09-16, Strato: arrows at the top corners
   to cycle without dropping back to the page): the row (a fish's index, or
   the TANK / NEW FRY row) and the column (-1 = the name / strip, else the
   badge or gate). ms_step moves along the group the modal belongs to: a
   fish's six badges, the tank's six, the fry's gates, or - from a fish's
   name - the fish themselves. A group of one (TANK's tally, NEW FRY's)
   shows no arrows. */
static int  g_ms_row = -1, g_ms_k = -1;
static bool g_ms_tankrow, g_ms_fryrow;
/* a fish's CARD (the modal behind its name) carries two buttons at its foot
   (2026-10-01, Strato): RENAME - the letter wheel over the live tank - and
   SELL. SELL is two taps on the placement page's idiom: the first arms it
   (the label turns to the price and OK?, the line above says TAP AGAIN), the
   second sells - but not within MSP_SELL_WAIT_S of the tap before it, so a
   doubled tap, or a mash, cannot sell a fish (each spent tap restarts the
   wait, 0.4 rc round three). A fish the tank will not part with (the last pair,
   or a fry still owed its welcome) shows SELL dim, and the line above stays
   empty until the dim button is tapped - then it gives the reason (Strato,
   2026-10-01: the reason "only if the gray sell button is tapped"). The card stands where a gate's modal does (the taller one), inside
   the box every board's glass shows whole: no per-board layout. */
#define MSP_CARD_BTN_W   116
#define MSP_CARD_BTN_GAP 24
#define MSP_SELL_WAIT_S  0.6f
static bool  g_ms_sell_armed;
static bool  g_ms_sell_asked;        /* the dim SELL was tapped: the line says why it stays */
static double g_ms_sell_clock;       /* the tank clock when SELL was armed */
/* GIVE (tank link phase 6; Strato 2026-10-20, from the mockups docs/media/give-sheet-*.png): on the
   fish card between RENAME and SELL, ONLY while a door is open. A tap opens the GIVE modal (below; it
   replaced SURE? on 11-02), the card's line says ASKING / GIVEN TO and the answer a while. */
static uint32_t g_ms_given_uid;      /* the fish whose card says GIVEN: the card closes once it has left (rows shift then) */
#if PT_NET   /* the GIVE modal and the keeper card's GIVE (version.h PT_NET) */
static const char *const BADGE_WORDS[LINK_BADGES];   /* (the passport's, below) */
static void gm_open(const tank_t *t, int fish);      /* the GIVE modal, below */
static int give_look(const tank_t *t, int fish) {
    const gift_book_t *g = gift_of(t);
    if (!g || fish < 0 || fish >= t->n_fish) return GV_HIDDEN;
    uint32_t uid = t->fish[fish].uid;
    if (g->leaving_uid == uid) return GV_LEAVING;
    if (g->asking_uid == uid || g->wait_uid == uid || g->want_uid == uid) return GV_ASKING;   /* (waiting for a fish in a doorway: asking already) */
    if (!t->door_open[0] && !t->door_open[1]) return GV_HIDDEN;   /* unlinked: the card is RENAME | SELL as ever */
    if (!progression_fish_givable(t, fish)) return GV_DIM;
    return GV_GIVE;
}
/* the card's line while GIVE has something to say (false: SELL's line, as before) */
static bool give_line(const tank_t *t, int fish, char *out, size_t n) {
    const gift_book_t *g = gift_of(t);
    if (!g || fish < 0 || fish >= t->n_fish) return false;
    int look = give_look(t, fish);
    char who[20];                                   /* "THE TRIANGLE TANK" / "THE TANK NEXT DOOR": 18 at most (GIVEN TO + it fits the line's 32) */
    if (g->peer_badge >= 0 && g->peer_badge < LINK_BADGES) snprintf(who, sizeof who, "THE %s TANK", BADGE_WORDS[g->peer_badge]);
    else snprintf(who, sizeof who, "THE TANK NEXT DOOR");
    if (look == GV_LEAVING) { snprintf(out, n, "GIVEN TO %s", who); return true; }
    if (look == GV_ASKING)  { snprintf(out, n, "ASKING %s", who); return true; }
    if (g->said_uid == t->fish[fish].uid && t->clock < g->said_until) {
        static const char *const SAID[] = { "THEY SAID NO THANKS", "THEIR TANK IS FULL", "NO ANSWER CAME", "", "CANNOT BE GIVEN RIGHT NOW" };
        snprintf(out, n, "%s", SAID[g->said_reason <= LINK_GIVE_NO_OTHER ? g->said_reason : LINK_GIVE_NO_OTHER]);
        return out[0] != 0;
    }
    return false;
}
static void give_button(ctx_t *c, int x, int y, int w, int h, int look, int scale) {
    if (look == GV_GIVE)   { button(c, x, y, w, h, 0x1c2f36, GIVE_GOLD, "GIVE", scale); return; }
    if (look == GV_ASKING) { button(c, x, y, w, h, 0x155e58, GIVE_GOLD, "ASKING", scale); return; }
    const char *s = look == GV_LEAVING ? "GIVEN" : "GIVE";                /* dim: not now (or on its way) */
    rect_fill(c, x, y, w, h, 0x04141a); rect_edge(c, x, y, w, h, MSP_DIM);
    draw_text(c, x + (w - text_w(s, scale)) / 2, y + (h - 7 * scale) / 2, scale, MSP_DIM, s);
}
/* a tap on GIVE - the keeper card's or the stat card's box: the GIVE modal (GIVE, ASKING and the dim GIVE alike; GIVEN is no button) */
static void give_tap(const tank_t *t, int fish) {
    int look = give_look(t, fish);
    g_ms_sell_armed = false;
    if (!gift_of(t) || look == GV_HIDDEN || look == GV_LEAVING) return;
    gm_open(t, fish);
}
void render_give_tap(const tank_t *t, int fish_idx) { give_tap(t, fish_idx); }   /* the stat card's GIVE box: the keeper card's taps */

/* ---- THE GIVE MODAL (0.4; Strato 2026-11-02 "Build option B", working-assets/link-edge/mock/sheet_B_*.png) ----
 * Both ways to give - the stat card's quick GIVE box and the keeper card's GIVE - open this one card, over the live tank
 * or the keeper page: the fish, the tank it would go to (the passport's HOME words, its badge's shape, an arrow to its
 * side), IT WOULD LIVE THERE FOR GOOD, GIVE | BACK (it replaced SURE?). Both doors open (the middle of three): the pick
 * comes first - GIVE <FISH> TO, a row a tank, BACK - and picking is the yes. Then ASKING THEIR KEEPER with the other
 * keeper's time as a bar, CALL OFF. A NO, a door or the tank picked gone, the fish gone: the words stay up until OK. A
 * dim GIVE opens it with why. Every button but BACK sleeps GIFT_HOLD_S once its step is up (the gift card's hold: a
 * kid's next tap must not GIVE, CALL OFF or OK by accident); BACK is always the way out. A tap off the card: nothing. */
#define GM_W   336
#define GM_H   252                  /* the fish's card; the pick is GM_PICK_H */
#define GM_PICK_H 232
#define GM_BTN_W 140
#define GM_BTN_H 34
#define GM_ROW_Y0 38                /* the pick's rows (card coordinates) */
#define GM_ROW_PITCH 68
#define GM_ROW_H 58
_Static_assert(GM_W * GM_H <= GIFT_CARD_W * GIFT_CARD_H && GM_PICK_H <= GM_H, "the GIVE modal's bake is a gift card's size");
static struct {
    bool on; uint32_t uid; double opened, step_t, asked; int step;
    bool pick; int8_t edge; uint32_t peer[LINK_EDGES]; int8_t badge[LINK_EDGES];   /* latched as it opens: the door and tank, or the two */
    char line[32];                                                                  /* GM_SAID / GM_WHY: the words, until OK */
    unsigned drawn_frame;                                                           /* the frame it was last drawn in (0: not yet) */
} g_gm;
static int gm_fish(const tank_t *t) {
    for (int i = 0; i < t->n_fish; i++) if (t->fish[i].uid == g_gm.uid) return i;
    return -1;
}
static void gm_say(const tank_t *t, const char *why) { snprintf(g_gm.line, sizeof g_gm.line, "%s", why); g_gm.step = RENDER_GM_SAID; g_gm.step_t = t->clock; }
static const char *gm_why(const tank_t *t, int fish) {   /* keep-two as the GIVE counts it: fish of ours on their way out are gone (803f1d2) */
    return !progression_keeps_two(t, fish) ? "KEEP AT LEAST TWO FISH" : "NAME THE NEW FRY FIRST"; }
static bool gm_row_open(const tank_t *t, const gift_book_t *g, int e) { return t->door_open[e] && g->door_peer[e] && g->door_peer[e] == g_gm.peer[e]; }
static void gm_open(const tank_t *t, int fish) {
    const gift_book_t *g = gift_of(t);
    memset(&g_gm, 0, sizeof g_gm);
    g_gm.on = true; g_gm.uid = t->fish[fish].uid; g_gm.opened = g_gm.step_t = t->clock; g_gm.edge = -1;
    g_gm.asked = t->clock + 1e-3f;                    /* (an answer from before it opened is not this modal's) */
    for (int e = 0; e < LINK_EDGES; e++) { g_gm.peer[e] = t->door_open[e] ? g->door_peer[e] : 0; g_gm.badge[e] = g->door_badge[e]; }
    const fish_t *f = &t->fish[fish];
    if (g->asking_uid == g_gm.uid || g->wait_uid == g_gm.uid || g->want_uid == g_gm.uid) {   /* (a tap on ASKING: its modal, CALL OFF) */
        g_gm.step = RENDER_GM_ASKING; g_gm.asked = g->asking_uid == g_gm.uid ? g->asking_since : g->wait_since;
        for (int e = 0; e < LINK_EDGES; e++) if (g->peer_badge >= 0 && g->peer_badge == g->door_badge[e]) g_gm.edge = (int8_t)e;
        return;
    }
    int e = f->away ? f->door_edge : t->door_open[LINK_LEFT] && t->door_open[LINK_RIGHT] ? -2 : t->door_open[LINK_LEFT] ? LINK_LEFT : t->door_open[LINK_RIGHT] ? LINK_RIGHT : -1;
    if (!progression_fish_givable(t, fish)) {          /* a dim GIVE: why (TO the one tank next door, as the mockup) */
        snprintf(g_gm.line, sizeof g_gm.line, "%s", gm_why(t, fish)); g_gm.step = RENDER_GM_WHY;
        if (e >= 0 && e < LINK_EDGES && g_gm.peer[e]) g_gm.edge = (int8_t)e;
        return;
    }
    if (e == -2) { g_gm.pick = true; g_gm.step = RENDER_GM_PICK; return; }
    if (e < 0 || e >= LINK_EDGES || !g_gm.peer[e]) { gm_say(t, "CANNOT BE GIVEN RIGHT NOW"); return; }
    g_gm.edge = (int8_t)e; g_gm.step = RENDER_GM_CONFIRM;
}
/* where the modal is now - the book and the doors move it on (and may close it) */
static int gm_eval(const tank_t *t) {
    if (!g_gm.on) return RENDER_GM_OFF;
    const gift_book_t *g = gift_of(t);
    int step = g_gm.step;
    if (step == RENDER_GM_SAID || step == RENDER_GM_WHY) return step;          /* (until OK) */
    int i = gm_fish(t);
    if (g && g->leaving_uid == g_gm.uid) { g_gm.on = false; return RENDER_GM_OFF; }   /* accepted: the card's GIVEN stamp takes it from here */
    if (!g || i < 0) { gm_say(t, "CANNOT BE GIVEN RIGHT NOW"); return g_gm.step; }
    if (g->asking_uid == g_gm.uid || g->wait_uid == g_gm.uid || g->want_uid == g_gm.uid) step = RENDER_GM_ASKING;
    else if (g->said_uid == g_gm.uid && g->said_until - GIFT_SAID_S >= g_gm.asked) {   /* this gift's NO */
        static const char *const SAID[] = { "THEY SAID NO THANKS", "THEIR TANK IS FULL", "NO ANSWER CAME", "", "CANNOT BE GIVEN RIGHT NOW" };
        gm_say(t, SAID[g->said_reason <= LINK_GIVE_NO_OTHER ? g->said_reason : LINK_GIVE_NO_OTHER]);
        if (!g_gm.line[0]) g_gm.on = false;                                     /* (called off: nothing to say) */
        return g_gm.on ? g_gm.step : RENDER_GM_OFF;
    }
    else if (step == RENDER_GM_ASKING) { g_gm.on = false; return RENDER_GM_OFF; }   /* (taken back without a word) */
    else if (!progression_fish_givable(t, i)) { snprintf(g_gm.line, sizeof g_gm.line, "%s", gm_why(t, i)); step = RENDER_GM_WHY; }
    else if (g_gm.pick) {
        if (!gm_row_open(t, g, LINK_LEFT) && !gm_row_open(t, g, LINK_RIGHT)) { gm_say(t, "CANNOT BE GIVEN RIGHT NOW"); return g_gm.step; }
    } else if (g_gm.edge < 0 || !gm_row_open(t, g, g_gm.edge)) { gm_say(t, "CANNOT BE GIVEN RIGHT NOW"); return g_gm.step; }
    if (step != g_gm.step) { g_gm.step = step; g_gm.step_t = t->clock; }
    return step;
}
/* a neighbor's keeper-given name, if one of its fish is here or in the visitor collection ("" = its HOME WATERS alone) */
static const char *gm_tank_name(const tank_t *t, uint32_t tank) {
    for (int k = 0; k < N_VISIT_MAX; k++) if (t->vis_used[k] && t->vis_home[k] == tank && t->vis_home_name[k][0]) return t->vis_home_name[k];
    for (int k = collection_count() - 1; k >= 0; k--) { const coll_entry_t *e = collection_get(k); if (e && e->home == tank && e->home_name[0]) return e->home_name; }
    return "";
}
static void gm_arrow(ctx_t *c, int cx, int cy, int half, int dir, uint32_t rgb) {   /* a solid triangle pointing dir: half+1 wide, 2*half+1 tall */
    for (int i = 0; i <= half; i++) { int hh = half - i; rect_fill(c, cx + (dir > 0 ? -half / 2 + i : half / 2 - i), cy - hh, 1, 2 * hh + 1, rgb); }
}
static void gm_split(const char *s, char *a, char *b, int n) {   /* two lines at the space nearest the middle */
    int len = (int)strlen(s), best = -1;
    for (int i = 0; i < len; i++) if (s[i] == ' ' && (best < 0 || abs(i - len / 2) < abs(best - len / 2))) best = i;
    if (best < 0) { snprintf(a, n, "%s", s); b[0] = 0; return; }
    snprintf(a, n, "%.*s", best, s); snprintf(b, n, "%s", s + best + 1);
}
static int gm_card_h(void) { return g_gm.step == RENDER_GM_PICK ? GM_PICK_H : GM_H; }
static void gm_frame(int *X, int *Y) { *X = (TANK_W - GM_W) / 2; *Y = (TANK_H - gm_card_h()) / 2; }
/* the card but its portrait and its bar's fill (gm_draw draws those live) */
static void gm_paint(ctx_t *c, const tank_t *t, int step) {
    const gift_book_t *g = gift_of(t);
    const uint32_t GOLD = GIVE_GOLD, INK = 0x13324f, LBL = 0x7fa3b5, DIM = 0x3f6a72;
    int i = gm_fish(t);
    const char *nm = i >= 0 ? t->fish[i].name : "";
    const int W = GM_W, H = gm_card_h(); int X, Y; gm_frame(&X, &Y);
    rect_fill(c, X, Y, W, H, 0x0e2233);
    rect_fill(c, X, Y, W, 26, INK);
    char title[40]; snprintf(title, sizeof title, step == RENDER_GM_PICK ? "GIVE %s TO" : "GIVE %s?", nm);
    draw_text(c, X + (W - text_w(title, 2)) / 2, Y + 7, 2, GOLD, title);
    rect_edge(c, X, Y, W, H, GOLD); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, INK);
    if (step == RENDER_GM_PICK) {
        for (int s = 0; s < LINK_EDGES; s++) {   /* LINK_LEFT on top, its arrow at the left end; LINK_RIGHT under it, arrow right */
            const int RX = X + 12, RY = Y + GM_ROW_Y0 + s * GM_ROW_PITCH, RW = W - 24, RH = GM_ROW_H;
            bool off = !gm_row_open(t, g, s);
            uint32_t edge = off ? DIM : GOLD;
            rect_fill(c, RX, RY, RW, RH, 0x1c2f36);
            rect_edge(c, RX, RY, RW, RH, edge); rect_edge(c, RX + 1, RY + 1, RW - 2, RH - 2, edge);
            gm_arrow(c, s ? RX + RW - 18 : RX + 18, RY + RH / 2, 11, s ? 1 : -1, off ? DIM : 0xffffff);
            char lbl[40], val[24];
            home_fields(g_gm.peer[s], gm_tank_name(t, g_gm.peer[s]), "HOME", lbl, sizeof lbl, val, sizeof val);
            int tx = s ? RX + 16 : RX + 40, vx = tx, b = g_gm.badge[s];
            draw_text(c, tx, RY + 10, 1, off ? 0xc8d6de : LBL, off ? "NOT LINKED NOW" : lbl);   /* (closed while up: which one, dim, no tap) */
            if (b >= 0 && b < LINK_BADGES) { link_badge_draw(c, tx + 7, RY + 33, 7, b, off ? 90 : 255); vx += 24; }   /* the shape AND the name */
            draw_text(c, vx, RY + 26, 2, off ? DIM : 0xffffff, val);
        }
        button(c, X + (W - GM_BTN_W) / 2, Y + H - GM_BTN_H - 12, GM_BTN_W, GM_BTN_H, 0x1c2f36, 0x9fd8e2, "BACK", 2);
        return;
    }
    const int PX = X + 12, PY = Y + 36, PW = 100, PH = 84;
    rect_fill(c, PX, PY, PW, PH, 0x0a1824); rect_edge(c, PX, PY, PW, PH, 0x9fd8e2);
    const int FX = PX + PW + 12;
    draw_text(c, FX, Y + 36, 1, LBL, "NAME");
    draw_text(c, FX, Y + 46, 2, 0xffffff, nm);
    if (g_gm.edge >= 0 && g_gm.peer[g_gm.edge]) {     /* TO: the tank and its door */
        int e = g_gm.edge; char lbl[40], val[24];
        home_fields(g_gm.peer[e], gm_tank_name(t, g_gm.peer[e]), "TO", lbl, sizeof lbl, val, sizeof val);
        draw_text(c, FX, Y + 72, 1, LBL, lbl);
        int vx = FX, b = g_gm.badge[e];
        if (b >= 0 && b < LINK_BADGES) { link_badge_draw(c, FX + 6, Y + 89, 6, b, 255); vx += 18; }
        draw_text(c, vx, Y + 82, 2, 0xffffff, val);
        gm_arrow(c, vx + text_w(val, 2) + 14, Y + 89, 7, e == LINK_RIGHT ? 1 : -1, 0xffffff);
    }
    char a[32], b[32];
    if (step == RENDER_GM_CONFIRM) { snprintf(a, sizeof a, "IT WOULD LIVE THERE"); snprintf(b, sizeof b, "FOR GOOD"); }
    else if (step == RENDER_GM_ASKING) { snprintf(a, sizeof a, "ASKING THEIR KEEPER"); b[0] = 0; }
    else if (text_w(g_gm.line, 2) <= W - 24) { snprintf(a, sizeof a, "%s", g_gm.line); b[0] = 0; }
    else gm_split(g_gm.line, a, b, sizeof a);
    draw_text(c, X + (W - text_w(a, 2)) / 2, Y + 136, 2, 0xffffff, a);
    if (b[0]) draw_text(c, X + (W - text_w(b, 2)) / 2, Y + 156, 2, 0xffffff, b);
    if (step == RENDER_GM_ASKING) rect_edge(c, X + 24, Y + 162, W - 48, 6, DIM);   /* the other keeper's time (its fill: live) */
    const int BY = Y + H - GM_BTN_H - 12, gap = W - 2 * GM_BTN_W - 24;
    if (step == RENDER_GM_CONFIRM) {
        button(c, X + 12, BY, GM_BTN_W, GM_BTN_H, 0x1c2f36, GOLD, "GIVE", 2);
        button(c, X + 12 + GM_BTN_W + gap, BY, GM_BTN_W, GM_BTN_H, 0x1c2f36, 0x9fd8e2, "BACK", 2);
    } else button(c, X + (W - GM_BTN_W) / 2, BY, GM_BTN_W, GM_BTN_H, 0x1c2f36, 0x9fd8e2, step == RENDER_GM_ASKING ? "CALL OFF" : "OK", 2);
}
/* what the still card shows (a new key: painted again) */
static uint32_t gm_key(const tank_t *t, int step) {
    const gift_book_t *g = gift_of(t);
    int i = gm_fish(t);
    uint32_t h = bake_hash(2166136261u, (uint32_t)step);
    h = bake_hash(h, g_gm.uid); h = bake_hash_s(h, i >= 0 ? t->fish[i].name : ""); h = bake_hash(h, (uint32_t)(g_gm.edge + 1));
    h = bake_hash_s(h, g_gm.line);
    for (int e = 0; e < LINK_EDGES; e++) {
        h = bake_hash(h, g_gm.peer[e]); h = bake_hash(h, (uint32_t)(g_gm.badge[e] + 1));
        h = bake_hash(h, g && gm_row_open(t, g, e)); h = bake_hash_s(h, gm_tank_name(t, g_gm.peer[e]));
    }
    return h;
}
static void gm_draw(const tank_t *t, uint16_t *fb, int stride) {
    int step = gm_eval(t);
    if (step == RENDER_GM_OFF) return;
    if (g_gm.drawn_frame && g_rframe - g_gm.drawn_frame > 1) g_gm.step_t = t->clock;   /* back from under a page (a birth flow): a
                                                                   fresh hold - its buttons were never seen (the 11-02 sweep) */
    g_gm.drawn_frame = g_rframe;
    const gift_book_t *g = gift_of(t);
    ctx_t cc = ctx_full(fb, stride, 1.0f); ctx_t *c = &cc;
    const int W = GM_W, H = gm_card_h(); int X, Y; gm_frame(&X, &Y);
    if (g_bake_gm.buf) {
        uint32_t key = gm_key(t, step);
        if (bake_stale(&g_bake_gm, key, H)) {
            ctx_t bc; bool hold; uint32_t *tm = bake_begin(&g_bake_gm, &bc, X, Y, W, H, &hold);
            gm_paint(&bc, t, step);
            bake_end(&g_bake_gm, key, H, tm, hold);
        }
        bake_blit(&g_bake_gm, X, Y, W, H, fb, stride);
    } else gm_paint(c, t, step);
    if (step == RENDER_GM_PICK) return;
    int i = gm_fish(t);                                /* live: the fish swims in its box, the bar runs down */
    const int PX = X + 12, PY = Y + 36, PW = 100, PH = 84;
    if (i >= 0) render_fish_portrait(fb, stride, PX + PW / 2.0f - PAGE_X, PY + PH / 2.0f - PAGE_Y, 1.3f, &t->fish[i], PW - 20, t->clock);   /* (page coordinates) */
    if (step == RENDER_GM_ASKING) {
        float left = g && g->asking_uid == g_gm.uid ? 1.0f - (float)(t->clock - g->asking_since) / (LINK_GIVE_ASK_MS / 1000.0f) : 1.0f;
        left = left < 0 ? 0 : left > 1 ? 1 : left;
        if (left > 0) rect_fill(c, X + 25, Y + 163, (int)((W - 50) * left), 4, 0x9fd8e2);
    }
}
int  render_give_modal_step(const tank_t *t) { return gm_eval(t); }
void render_give_modal_close(void) { g_gm.on = false; }   /* (the director's `link modal off`: as BACK) */
const char *render_give_modal_line(const tank_t *t) {
    int step = gm_eval(t);
    return step == RENDER_GM_CONFIRM ? "IT WOULD LIVE THERE" : step == RENDER_GM_ASKING ? "ASKING THEIR KEEPER" : step == RENDER_GM_PICK ? "" : g_gm.line;
}
void render_give_modal(const tank_t *t, uint16_t *fb, int stride) { if (!g_ms_in) gm_draw(t, fb, stride); }   /* (the keeper page draws its own) */
bool render_give_modal_button(const tank_t *t, int which, int *x, int *y) {
    int step = gm_eval(t);
    if (step == RENDER_GM_OFF) return false;
    int X, Y; gm_frame(&X, &Y); const int H = gm_card_h(), W = GM_W, BY = Y + H - GM_BTN_H - 12 + GM_BTN_H / 2;
    if (step == RENDER_GM_PICK) {
        if (which == RENDER_GM_TAP_LEFT || which == RENDER_GM_TAP_RIGHT) { *x = X + W / 2; *y = Y + GM_ROW_Y0 + (which == RENDER_GM_TAP_RIGHT) * GM_ROW_PITCH + GM_ROW_H / 2; return true; }
        if (which == RENDER_GM_TAP_BACK) { *x = X + W / 2; *y = BY; return true; }
        return false;
    }
    if (step == RENDER_GM_CONFIRM) {
        if (which == RENDER_GM_TAP_GIVE) { *x = X + 12 + GM_BTN_W / 2; *y = BY; return true; }
        if (which == RENDER_GM_TAP_BACK) { *x = X + W - 12 - GM_BTN_W / 2; *y = BY; return true; }
        return false;
    }
    if ((which == RENDER_GM_TAP_CALLOFF && step == RENDER_GM_ASKING) || (which == RENDER_GM_TAP_OK && step != RENDER_GM_ASKING)) { *x = X + W / 2; *y = BY; return true; }
    return false;
}
int render_give_modal_tap(const tank_t *t, float x, float y) {
    int step = gm_eval(t);
    if (step == RENDER_GM_OFF) return RENDER_GM_TAP_NONE;
    gift_book_t *g = gift_of(t);
    int X, Y; gm_frame(&X, &Y); const int H = gm_card_h(), W = GM_W, BY = Y + H - GM_BTN_H - 12;
    if (x < X || x >= X + W || y < Y || y >= Y + H + 10) return RENDER_GM_TAP_NONE;   /* (off the card: nothing - a kid's stray tap) */
    const bool held = t->clock - g_gm.step_t < GIFT_HOLD_S;
    int hit = RENDER_GM_TAP_CARD;
    if (step == RENDER_GM_PICK) {
        for (int s = 0; s < LINK_EDGES; s++) {
            int RY = Y + GM_ROW_Y0 + s * GM_ROW_PITCH;
            if (y >= RY - 4 && y < RY + GM_ROW_H + 4) hit = gm_row_open(t, g, s) ? (s ? RENDER_GM_TAP_RIGHT : RENDER_GM_TAP_LEFT) : RENDER_GM_TAP_CARD;
        }
        if (y >= BY - 8 && x >= X + (W - GM_BTN_W) / 2 - 20 && x < X + (W + GM_BTN_W) / 2 + 20) hit = RENDER_GM_TAP_BACK;
    } else if (y >= BY - 10) {
        if (step == RENDER_GM_CONFIRM) hit = x < X + W / 2 ? RENDER_GM_TAP_GIVE : RENDER_GM_TAP_BACK;
        else if (x >= X + (W - GM_BTN_W) / 2 - 20 && x < X + (W + GM_BTN_W) / 2 + 20) hit = step == RENDER_GM_ASKING ? RENDER_GM_TAP_CALLOFF : RENDER_GM_TAP_OK;
    }
    if (hit == RENDER_GM_TAP_CARD) return hit;
    if (hit == RENDER_GM_TAP_BACK) { g_gm.on = false; return hit; }
    if (held) return RENDER_GM_TAP_HELD;
    if (hit == RENDER_GM_TAP_OK) { g_gm.on = false; return hit; }
    if (hit == RENDER_GM_TAP_CALLOFF) {
        if (g) { g->cancel_uid = g_gm.uid; if (g->want_uid == g_gm.uid) g->want_uid = 0; }
        g_gm.on = false; return hit;
    }
    int e = hit == RENDER_GM_TAP_GIVE ? g_gm.edge : hit == RENDER_GM_TAP_RIGHT ? LINK_RIGHT : LINK_LEFT;   /* GIVE, or a tank picked: the yes */
    if (!g || e < 0) return RENDER_GM_TAP_CARD;
    g->want_uid = g_gm.uid; g->want_door = (uint8_t)(GIFT_DOOR_ANY + 1 + e); g->want_peer = g_gm.peer[e]; g->said_uid = 0;
    g_gm.edge = (int8_t)e; g_gm.asked = t->clock; g_gm.step = RENDER_GM_ASKING; g_gm.step_t = t->clock;
    return hit;
}
#endif

/* the TANK tally's picture (2026-10-02, Strato: the modal "feels bare"): the
 * school itself across the top of the box - every fish in its own colours,
 * at its own size and stage, in the order they arrived - then a dim
 * silhouette for each place still open. The count is in the words under it;
 * here it is seen (lit against dim, never by hue alone). */
#define MSP_SCHOOL_PITCH 52
#define MSP_SCHOOL_SCALE 0.74f
#define MSP_SCHOOL_MAX   1.02f            /* the size that still fits its place (a big elder) */
static void ms_school(const tank_t *t, uint16_t *fb, int stride, int X, int Y, int W) {
    const int cap = progression_pop_cap(t);                   /* (the pack's cap in the terrarium) */
    for (int i = 0; i < cap; i++) {
        float cx = X + (W - cap * MSP_SCHOOL_PITCH) / 2 + MSP_SCHOOL_PITCH / 2 + 4 + i * MSP_SCHOOL_PITCH;   /* (+4: a fish's centre sits ahead of its middle) */
        if (i < t->n_fish) {
            float sz = t->fish[i].size * MSP_SCHOOL_SCALE;
            render_fish_portrait(fb, stride, cx, Y + 50, sz > MSP_SCHOOL_MAX ? MSP_SCHOOL_MAX : sz, &t->fish[i], FISH_ROOM_ROW, t->clock + i * 0.9f);
        } else render_fish_preview(fb, stride, cx, Y + 50, 0.9f * MSP_SCHOOL_SCALE, MSP_DIM, MSP_DIM, MSP_DIM, NULL, FISH_ROOM_ROW, t->clock + i * 0.9f);
    }
}

/* a locked badge: the same art as a flat grey silhouette - luminance keeps
 * the shapes readable, the low alpha keeps it quiet on the ink */
static uint32_t locked_rgb(uint16_t v) {
    int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    int l = (r * 77 + g * 151 + b * 28) >> 8;                /* 0..255 luminance */
    int k = 140 + l * 115 / 255;                              /* 0.55 .. 1.0, in 1/255 */
    return (uint32_t)(20 + (0x2a * k >> 8)) << 16 | (uint32_t)(20 + (0x3f * k >> 8)) << 8
         | (uint32_t)(20 + (0x45 * k >> 8));
}
static void blit_icon_locked(ctx_t *c, int x, int y, const icon_t *ic) {
    for (int j = 0; j < ic->h; j++)
        for (int i = 0; i < ic->w; i++) {
            int a = ic->a[j * ic->w + i];
            if (a) px_blend(c, x + i, y + j, locked_rgb(ic->rgb[j * ic->w + i]), a * 180 >> 8);
        }
}
/* the same art at an integer scale (the detail modal), lit or as the silhouette */
void blit_icon_scaled(ctx_t *c, int x, int y, const icon_t *ic, int s, bool lit) {
    for (int j = 0; j < ic->h; j++)
        for (int i = 0; i < ic->w; i++) {
            int a = ic->a[j * ic->w + i];
            if (!a) continue;
            uint16_t v = ic->rgb[j * ic->w + i];
            for (int yy = 0; yy < s; yy++)
                for (int xx = 0; xx < s; xx++) {
                    if (lit) px565_blend(c, x + i * s + xx, y + j * s + yy, v, a);
                    else     px_blend(c, x + i * s + xx, y + j * s + yy, locked_rgb(v), a * 180 >> 8);
                }
        }
}
static void badge(ctx_t *c, int x, int y, const icon_t *ic, bool on, bool fresh) {
    if (on) blit_icon(c, x, y, ic, 255); else blit_icon_locked(c, x, y, ic);
    if (on && fresh) {
        rect_edge(c, x - 3, y - 3, MSP_ICON + 6, MSP_ICON + 6, MSP_TEAL);
        rect_edge(c, x - 4, y - 4, MSP_ICON + 8, MSP_ICON + 8, 0x3f6a72);
    }
}
static int bit_index(uint32_t bit) { int i = 0; while (bit > 1u) { bit >>= 1; i++; } return i; }
/* the art for a gate of the fry checklist: the card's trust icon, the tank's
   own badges for meals / a hold / grass / a change / a wiped glass / clean sheds (the terrarium's), or NULL = the
   youngest fish itself (the GROW gate) */
static const icon_t *fry_req_icon(int kind) {
    switch (kind) {
    case FRY_REQ_TRUST:  return &icon_trust;
    case FRY_REQ_FEED:   return world_view()->art.req_feed;
    case FRY_REQ_HOLD:   return world_view()->art.req_hold;
    case FRY_REQ_CHANGE: return world_view()->art.req_change;
    case FRY_REQ_GRASS:  return &icon_ms_first_trimming;
    case FRY_REQ_GLASS:  return world_view()->art.req_glass;
    default:             return world_view()->req_icon ? world_view()->req_icon(kind) : NULL;   /* (the terrarium's clean-shed badge's own art) */
    }
}
/* a gate as a badge in a 32 px cell: lit when met, the silhouette until then */
static void fry_badge(ctx_t *c, uint16_t *fb, int stride, const tank_t *t, int x, int y, const fry_req_t *r) {
    const icon_t *ic = fry_req_icon(r->kind);
    if (ic) {
        int off = (MSP_ICON - ic->w) / 2;        /* the 24 px card icon sits centred */
        if (r->met) blit_icon(c, x + off, y + off, ic, 255); else blit_icon_locked(c, x + off, y + off, ic);
    } else {                                     /* GROW: the youngest fish, its own colours once grown */
        const fish_t *f = &t->fish[t->n_fish - 1];
        float sz = f->size > 0.7f ? 0.7f : f->size;
        if (r->met) render_fish_preview(fb, stride, x + MSP_ICON / 2, y + MSP_ICON / 2, sz, f->color, f->fin, f->accent, f, FISH_ROOM_ROW, t->clock);
        else        render_fish_preview(fb, stride, x + MSP_ICON / 2, y + MSP_ICON / 2, sz, MSP_DIM, MSP_DIM, MSP_DIM, f, FISH_ROOM_ROW, t->clock);
    }
}

/* a chevron pointing left or right, its tip at (tx, cy): 3 px steps, 24 px tall */
static void ms_chevron(ctx_t *c, int tx, int cy, bool left, uint32_t rgb) {
    for (int i = 0; i < 4; i++) {
        int xx = left ? tx + i * 3 : tx - 3 - i * 3;
        rect_fill(c, xx, cy - 3 - i * 3, 3, 3, rgb);
        rect_fill(c, xx, cy + i * 3, 3, 3, rgb);
    }
}
/* the modal's group: how many things the arrows cycle through and where
   this one sits. nreq = the fry checklist's gate count (the caller has it). */
static int ms_group(const tank_t *t, int nreq, int *idx) {
    if (g_ms_fryrow)  { *idx = g_ms_k; return g_ms_k < 0 ? 1 : nreq; }
    if (g_ms_tankrow) { *idx = g_ms_k; return g_ms_k < 0 ? 1 : tank_badge_n(t); }
    if (g_ms_k < 0)   { *idx = g_ms_row; return t->n_fish; }    /* a fish's name: the fish */
    *idx = g_ms_k; return 6;
}
static int ms_open(const tank_t *t, int row, bool tank_row, bool fry_row, int k,
                   const fry_req_t *req, int nreq, bool staged);
static void ms_close(void);
/* a fish card's second line (0.4): its form - what it is, or will grow into (its body type's words: body.h) */
static void ms_genes_line(const fish_t *f) {
    page_body()->genes_line(page_species(), f, g_ms_caption2, sizeof g_ms_caption2);
}
/* the modal's box: a gate's and a fish's card are taller (a button row at the
   foot) and stand higher, clear of the CLOSE button */
static bool ms_card(const tank_t *t) { return g_ms_caption[0] && !g_ms_tip && !g_ms_fryrow && !g_ms_tankrow && g_ms_k < 0 && g_ms_row >= 0 && g_ms_row < t->n_fish; }
static int  ms_modal_y(const tank_t *t) { return g_ms_kind >= 0 || ms_card(t) ? MSP_FRY_MODAL_Y : MSP_MODAL_Y; }
static int  ms_modal_h(const tank_t *t) {
    return MSP_MODAL_H + (g_ms_caption2[0] || ms_card(t) ? 20 : 0)   /* a card always has its genes line (0.4): its buttons
                                                                        never move, even before the first draw fills it */
           + (g_ms_sub[0] || ms_card(t) ? 24 : 0) + (g_ms_kind >= 0 || ms_card(t) ? MSP_HOW_H + 14 : 0);
}
static int  ms_card_btn_y(const tank_t *t) { return ms_modal_y(t) + ms_modal_h(t) - 10 - MSP_HOW_H; }
static int  ms_card_btn_x(int which) {            /* 0 = RENAME, 1 = SELL: the pair centred */
    return MSP_MODAL_X + (MSP_MODAL_W - 2 * MSP_CARD_BTN_W - MSP_CARD_BTN_GAP) / 2 + which * (MSP_CARD_BTN_W + MSP_CARD_BTN_GAP);
}
#define MSP_CARD_BTN3_W   100                     /* linked: RENAME | GIVE | SELL (0.4) */
#define MSP_CARD_BTN3_GAP 8
static int  ms_card_btn3_x(int which) {
    return MSP_MODAL_X + (MSP_MODAL_W - 3 * MSP_CARD_BTN3_W - 2 * MSP_CARD_BTN3_GAP) / 2 + which * (MSP_CARD_BTN3_W + MSP_CARD_BTN3_GAP);
}
/* the arrows: the previous / next thing in the modal's group, wrapping */
static void ms_step(const tank_t *t, int dir) {
    fry_req_t req[FRY_REQ_MAX]; bool staged;
    int nreq = progression_next_fry(t, req, &staged);
    int idx, n = ms_group(t, nreq, &idx);
    if (n < 2) return;
    g_ms_sell_armed = false; g_ms_sell_asked = false;
    idx = (idx + dir + n) % n;
    if (!g_ms_fryrow && !g_ms_tankrow && g_ms_k < 0) ms_open(t, idx, false, false, -1, req, nreq, staged);
    else ms_open(t, g_ms_row, g_ms_tankrow, g_ms_fryrow, idx, req, nreq, staged);
}

/* ---- THE ICON FOOT (0.4; Strato 2026-11-01, approved from the sim's sketches) ----
 * The keeper page's foot as square icon buttons, a little taller than the text buttons and easier to hit: UPGRADES
 * (the sand dollar - its balance in the shop since footer icons v4), SETTINGS (a cog; the bowl's in the cap), MY TANKS
 * (two tanks on a bench - the switcher's way in, off the settings page), VISITORS (a passport: the visitor collection),
 * CLOSE (an X) - sprites since 11-01 (assets/icons/foot_*.png from working-assets/footer-icons-v4), spread evenly between the old foot's outer corners, so on curved glass the corners stand where the text
 * foot's stood (render.h MSP_FOOT_*). Tapping goes to the nearest square along the strip (render_milestones_tap). */
#define FS_SQ MSP_FOOT_SQ
static void fs_button(ctx_t *c, int x, int y) { button(c, x, y, FS_SQ, FS_SQ, 0x1c2f36, MSP_TEAL, "", 2); }
void render_ms_foot_box(int which, int *x, int *y) {
    if (PAGE_BOWL && which == MSF_SET) { *x = (PAGE_W - FS_SQ) / 2; *y = MSP_SET_Y - 6; return; }   /* (the bowl's cog: in the cap) */
    const int n = MSF_N - (PAGE_BOWL ? 1 : 0) - (PT_NET ? 0 : 1);   /* (no VISITORS square without the network, PT_NET) */
    const int i = which - (PAGE_BOWL && which > MSF_SET ? 1 : 0) - (!PT_NET && which > MSF_VIS ? 1 : 0);
    const int x0 = MSP_FOOT_UPG_X, x1 = MSP_FOOT_CLOSE_X + MSP_CLOSE_W - FS_SQ;
    *x = x0 + (x1 - x0) * i / (n - 1); *y = MSP_FOOT_TOP;
}
static void ms_foot(const tank_t *t, ctx_t *c) {
    for (int b = 0; b < MSF_N; b++) {
        if (!MSF_SHOWN(b)) continue;
        int x, y; render_ms_foot_box(b, &x, &y);
        fs_button(c, x, y);
        /* THE FOOT'S ICONS (0.4, 11-01: Strato's footer-icons-v4, working-assets/): 36 px sprites from 4 px in - the cog, two tanks on a
           bench, the PASSPORT (the visitor collection), a thick X - and OUR sand dollar (the shop's, 34 px) centered; no balance on the
           keeper page any more (the shop shows it) */
        const icon_t *ic = b == MSF_UPG ? world_view()->art.foot_coins : b == MSF_SET ? &icon_foot_settings : b == MSF_TANKS ? world_view()->art.foot_my_tanks
                         : b == MSF_VIS ? &icon_foot_passport : &icon_foot_close;
        blit_icon(c, x + (FS_SQ - ic->w) / 2, y + (FS_SQ - ic->h) / 2, ic, 255);
    }
}
/* ---- THE PAGE KEPT (0.4; Strato 11-01: "with four or five fish with animated tails the frame rate is pretty slow") ----
 * On the glass the keeper page drew at 8-10 fps on watch B, the square and the pendant (render 80-107 ms a frame, the
 * flush 13): every frame the whole page again - the ink, the names, ~1000 blended pixels a badge, the foot's icons (drawn
 * then with trig, 16 samples a pixel; sprites since) - while only the fish move. A framebuffer that holds this page from the last
 * time it was drawn into, with nothing drawn into it since and nothing still on the page changed (ms_key), now keeps it:
 * each moving thing's box (a row's portrait, the NEW FRY row's fry, the GROW gate's fish) goes back to the ink and the
 * moving things are drawn again, in the page's own order - nothing else. Pixel for pixel the page drawn whole:
 *  - a moving thing must stay inside its ZONE, where nothing still is drawn (the portraits' column left of the names and
 *    above the TANK row's line; the GROW gate's cell between its bar and the row above's rings) - a box outside it and
 *    the page is drawn whole that frame and the buffer is not kept;
 *  - "nothing drawn since": the page drawn in every frame since (render_frame_begin, once a frame - a caller that never
 *    calls it always gets the whole page), and no other draw target made since (CTX_MADE: an overlay over the page, the
 *    tank, any other page breaks it);
 *  - a modal up: the boxes grow to hold every place the thing has been (their pixels outside the box are the halved ink),
 *    are halved after the drawing, and the modal is drawn whole over them as before.
 * --selftest-keeper draws it against the whole page every frame. */
#define MS_ANIM_MAX (N_FISH_MAX + 2)
typedef struct { short x0, y0, x1, y1; } ms_box_t;           /* page coordinates, inclusive; x0 > x1: nothing drawn */
typedef struct { const uint16_t *fb; int stride; uint32_t key; unsigned frame, other; bool ok; int n; ms_box_t b[MS_ANIM_MAX]; } ms_kept_t;
static ms_kept_t g_ms_kept[2];
static unsigned g_rframe, g_ms_frame, g_ms_run0;            /* the frame count, the last frame the page was drawn in, the first of the run */
static int g_ms_kept_n, g_ms_whole_n, g_ms_why[MS_WHY_N];    /* frames kept, drawn whole, and why whole (render.h MS_WHY_*) */
static bool g_ms_whole_dbg;                                   /* the selftest's reference: drawn whole, nothing kept or counted */
void render_frame_begin(void) { g_rframe++; }
void render_milestones_counts(int *kept, int *whole, int why[MS_WHY_N]) { *kept = g_ms_kept_n; *whole = g_ms_whole_n; if (why) memcpy(why, g_ms_why, sizeof g_ms_why); }
void render_debug_page_whole(bool on) { g_ms_whole_dbg = on; }
/* everything the still page is drawn from (a new one: the page drawn whole) */
static uint32_t ms_key(const tank_t *t, const fry_req_t *req, int nreq, bool staged) {
    uint32_t h = bake_hash(2166136261u, (uint32_t)t->n_fish);
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i];
        for (int k = 0; k < (int)sizeof f->name && f->name[k]; k++) h = bake_hash(h, (uint8_t)f->name[k]);
        h = bake_hash(h, f->stage); h = bake_hash(h, f->accent); h = bake_hash(h, f->ms_bits); h = bake_hash(h, f->ms_seen);
    }
    h = bake_hash(h, (uint32_t)nreq); h = bake_hash(h, staged);
    for (int k = 0; k < nreq; k++) { h = bake_hash(h, (uint32_t)req[k].kind); h = bake_hash(h, req[k].met); h = bake_hash(h, (uint32_t)(int)(MSP_ICON * req[k].frac + 0.5f)); }
    h = bake_hash(h, t->sd_unlocks); h = bake_hash(h, t->tank_ms_bits); h = bake_hash(h, t->tank_ms_seen); h = bake_hash(h, (uint32_t)g_ms_tpage);
    h = bake_hash(h, g_ms_caption[0] != 0);
#if PT_NET
    h = bake_hash(h, g_gm.on); h = bake_hash(h, render_gift_up(t));
#endif
    if (g_ms_caption[0]) { h = bake_hash(h, g_ms_tip); h = bake_hash(h, (uint32_t)ms_modal_y(t)); h = bake_hash(h, (uint32_t)ms_modal_h(t)); }
    return h;
}
/* the NEW FRY row's label: the GROW gate's fish can reach over its last letter, so the kept page puts it back first
   inside that fish's box (`clip`; NULL: whole) */
static void ms_fry_label(ctx_t *c, int top, const ms_box_t *clip) {
    ctx_t cw = *c;
    if (clip) {
        int x0 = clip->x0, y0 = clip->y0, x1 = clip->x1, y1 = clip->y1;
        if (x0 < c->ox) x0 = c->ox;
        if (y0 < c->oy) y0 = c->oy;
        if (x1 > c->ox + c->w - 1) x1 = c->ox + c->w - 1;
        if (y1 > c->oy + c->h - 1) y1 = c->oy + c->h - 1;
        if (x0 > x1 || y0 > y1) return;
        cw.fb = &CTX_PX(c, x0, y0); cw.ox = x0; cw.oy = y0; cw.w = x1 - x0 + 1; cw.h = y1 - y0 + 1;
    }
    draw_text(&cw, 92, top + 2, 2, MSP_TEAL, world_view()->text.new_arrival);   /* (the terrarium's next arrival hatches from an egg) */
}
/* the moving things, in the page's order: each fish's portrait, the NEW FRY row's fry, the GROW gate's fish. Draws
   thing k (false: there is no thing k) and gives its zone (where nothing still is drawn) */
static bool ms_anim(const tank_t *t, uint16_t *fb, int stride, int k, const fry_req_t *req, int nreq, bool staged, ms_box_t *zone) {
    zone->x0 = (short)-PAGE_X; zone->y0 = (short)-PAGE_Y; zone->x1 = 88; zone->y1 = (short)(MSP_TANK_Y - 6);   /* left of the names (x 92) and
                                                                                                  the strips (~91), above the TANK row's line */
    if (k < t->n_fish) {
        const fish_t *f = &t->fish[k];
        fish_as_is(fb, stride, MSP_FISH_X, MSP_ROW_Y0 + k * MSP_ROW_H + MSP_ROW_MID, f->size, f, f->color, f->fin, f->accent, FISH_ROOM_ROW, true, t->clock);
        return true;
    }
    if (nreq <= 0) return false;
    int top = MSP_ROW_Y0 + t->n_fish * MSP_ROW_H;
    if (k == t->n_fish) {
        uint32_t fry_rgb = staged ? MSP_TEAL : MSP_DIM;
        page_body()->unborn(fb, stride, MSP_FISH_X, top + MSP_ROW_MID, fry_rgb, t->clock);
        return true;
    }
    for (int g = 0; g < nreq; g++)
        if (k == t->n_fish + 1 && !fry_req_icon(req[g].kind)) {
            int x = MSP_BADGE_X0 + g * MSP_BADGE_DX;
            zone->x0 = (short)(x - 8); zone->x1 = (short)(x + MSP_ICON);              /* the cell and its tail's reach left (the ticks end by x 162;
                                                                                         the NEW FRY label can reach in: ms_fry_label) */
            zone->y0 = (short)(top + MSP_ROW_BADGE + 3); zone->y1 = (short)(top + MSP_ROW_BAR - 1);   /* below the row above's rings, above the bar */
            const fish_t *f = &t->fish[t->n_fish - 1];              /* (fry_badge's GROW) */
            float sz = f->size > 0.7f ? 0.7f : f->size;
            if (req[g].met) render_fish_preview(fb, stride, x + MSP_ICON / 2, top + MSP_ROW_BADGE + MSP_ICON / 2, sz, f->color, f->fin, f->accent, f, FISH_ROOM_ROW, t->clock);
            else            render_fish_preview(fb, stride, x + MSP_ICON / 2, top + MSP_ROW_BADGE + MSP_ICON / 2, sz, MSP_DIM, MSP_DIM, MSP_DIM, f, FISH_ROOM_ROW, t->clock);
            return true;
        }
    return false;
}
static void ms_bb_begin(void) { g_bb_on = true; g_bb_x0 = g_bb_y0 = 1 << 20; g_bb_x1 = g_bb_y1 = -1; }
static ms_box_t ms_bb_end(void) { g_bb_on = false; ms_box_t b = { (short)g_bb_x0, (short)g_bb_y0, (short)g_bb_x1, (short)g_bb_y1 }; if (g_bb_x1 < 0) b.x0 = 1, b.x1 = 0; return b; }
static bool ms_box_in(ms_box_t b, ms_box_t z) { return b.x0 > b.x1 || (b.x0 >= z.x0 && b.x1 <= z.x1 && b.y0 >= z.y0 && b.y1 <= z.y1); }
static ms_box_t ms_box_or(ms_box_t a, ms_box_t b) {
    if (a.x0 > a.x1) return b;
    if (b.x0 > b.x1) return a;
    ms_box_t r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}
/* the boxes' pixels (each once where they overlap): back to the ink, or halved */
static void ms_boxes_do(uint16_t *fb, int stride, const ms_box_t *b, int n, bool halve) {
    int y0 = 1 << 20, y1 = -1;
    for (int i = 0; i < n; i++) if (b[i].x0 <= b[i].x1) { if (b[i].y0 < y0) y0 = b[i].y0; if (b[i].y1 > y1) y1 = b[i].y1; }
    const uint16_t ink = FB_SWAP16(rgb565(MSP_INK, 1));
    for (int y = y0; y <= y1; y++) {
        int fy = y + PAGE_Y;
        if ((unsigned)fy >= (unsigned)TANK_H) continue;
        int xs[MS_ANIM_MAX][2], m = 0;                  /* this row's runs, sorted and merged */
        for (int i = 0; i < n; i++) {
            if (b[i].x0 > b[i].x1 || y < b[i].y0 || y > b[i].y1) continue;
            int a = b[i].x0 + PAGE_X, e = b[i].x1 + PAGE_X, j = m++;
            if (a < 0) a = 0;
            if (e > TANK_W - 1) e = TANK_W - 1;
            while (j > 0 && xs[j - 1][0] > a) { xs[j][0] = xs[j - 1][0]; xs[j][1] = xs[j - 1][1]; j--; }
            xs[j][0] = a; xs[j][1] = e;
        }
        for (int r = 0; r < m; r++) {
            int a = xs[r][0], e = xs[r][1];
            while (r + 1 < m && xs[r + 1][0] <= e + 1) { if (xs[r + 1][1] > e) e = xs[r + 1][1]; r++; }
            uint16_t *p = fb + fy * stride;
            if (halve) for (int x = a; x <= e; x++) { uint16_t v = FB_SWAP16(p[x]); p[x] = FB_SWAP16((uint16_t)((v >> 1) & 0x7bef)); }
            else for (int x = a; x <= e; x++) p[x] = ink;
        }
    }
}
/* the kept page: false = not this buffer, or a box left its zone (the page is drawn whole) */
static bool ms_kept_draw(const tank_t *t, uint16_t *fb, int stride, ms_kept_t *k, const fry_req_t *req, int nreq, bool staged) {
    const bool modal = g_ms_caption[0] != 0;
    ms_boxes_do(fb, stride, k->b, k->n, false);
    if (k->n == t->n_fish + 2) { ctx_t c = ctx_page(fb, stride); ms_fry_label(&c, MSP_ROW_Y0 + t->n_fish * MSP_ROW_H, &k->b[t->n_fish + 1]); }   /* (GROW's fish is in) */
    ms_box_t nb[MS_ANIM_MAX], z;
    for (int i = 0; i < k->n; i++) {
        ms_bb_begin();
        ms_anim(t, fb, stride, i, req, nreq, staged, &z);
        nb[i] = ms_bb_end();
        if (!ms_box_in(nb[i], z) || (modal && !ms_box_in(nb[i], k->b[i]))) return false;   /* (under a modal the pixels round a box are halved ink) */
    }
    if (modal) ms_boxes_do(fb, stride, k->b, k->n, true);
    else memcpy(k->b, nb, sizeof nb[0] * (size_t)k->n);
    return true;
}
void render_milestones(const tank_t *t, uint16_t *fb, int stride) {
    if (g_ms_given_uid && !(g_ms_row >= 0 && g_ms_row < t->n_fish && t->fish[g_ms_row].uid == g_ms_given_uid)) ms_close();   /* a GIVEN fish has left:
                                                                                  its card goes with it (the rows have shifted) */
    fry_req_t req[FRY_REQ_MAX]; bool staged;
    int nreq = progression_next_fry(t, req, &staged);
    if (g_ms_tpage >= tank_pages(t)) g_ms_tpage = 0;
    const uint32_t key = ms_key(t, req, nreq, staged);
    ms_kept_t dbg = { 0 };
    if (!g_ms_whole_dbg) {
        if (g_ms_frame + 1 != g_rframe) g_ms_run0 = g_rframe;   /* a frame without the page: a new run */
        g_ms_frame = g_rframe;
    }
    ms_kept_t *kp = g_ms_whole_dbg ? &dbg : g_ms_kept[0].fb == fb ? &g_ms_kept[0] : g_ms_kept[1].fb == fb ? &g_ms_kept[1]
                  : g_ms_kept[0].frame <= g_ms_kept[1].frame ? &g_ms_kept[0] : &g_ms_kept[1];
    const bool keep = !g_ms_whole_dbg && kp->fb == fb && kp->ok && kp->stride == stride && kp->key == key && kp->other == g_ctx_other
                      && kp->frame < g_rframe && kp->frame >= g_ms_run0;
    g_ms_in = true;
    ctx_t c = ctx_page(fb, stride);
    if (keep && ms_kept_draw(t, fb, stride, kp, req, nreq, staged)) g_ms_kept_n++;
    else {
        if (!g_ms_whole_dbg) {
            g_ms_whole_n++;
            g_ms_why[keep ? MS_WHY_OUT : kp->fb != fb || kp->stride != stride ? MS_WHY_NEW : !kp->ok ? MS_WHY_ZONE : kp->key != key ? MS_WHY_CHANGED
                     : kp->other != g_ctx_other ? MS_WHY_OVER : MS_WHY_GAP]++;
        }
        ms_box_t ab[MS_ANIM_MAX]; int an = 0; bool in = true;
        const bool grow = kp->fb == fb && kp->ok && kp->key == key && g_ms_caption[0];   /* (under a modal: the boxes only grow while nothing changes) */
        for (int y = 0; y < TANK_H; y++)
            for (int x = 0; x < TANK_W; x++) fb[y * stride + x] = FB_SWAP16(rgb565(MSP_INK, 1));
#define MS_ANIM(i) do { ms_box_t _z; ms_bb_begin(); ms_anim(t, fb, stride, (i), req, nreq, staged, &_z); ab[i] = ms_bb_end(); \
                        in = in && ms_box_in(ab[i], _z); if (grow && (i) < kp->n) ab[i] = ms_box_or(ab[i], kp->b[i]); an = (i) + 1; } while (0)
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i];
        int top = MSP_ROW_Y0 + i * MSP_ROW_H;
        MS_ANIM(i);                                  /* the portrait */
        draw_text(&c, 92, top + 2, 2, 0xffffff, f->name);
        static const int GX[4] = { 96, 111, 129, 151 };   /* each glyph's own pitch: a 4 px gap as they grow */
        const uint32_t grown = page_body()->strip_rgb(page_species(), f);
        for (int s = 0; s < 4; s++)          /* growth strip: fry -> elder, lit up to the stage reached */
            page_body()->glyph(&c, GX[s], top + MSP_ROW_STRIP, 2.2f + s * 0.9f, s <= (int)f->stage ? grown : MSP_DIM);
        for (int k = 0; k < 6; k++) {
            uint32_t bit = world_view()->fish_badges[k].bit;
            badge(&c, MSP_BADGE_X0 + k * MSP_BADGE_DX, top + MSP_ROW_BADGE, world_view()->fish_badges[k].icon,
                  (f->ms_bits & bit) != 0, (f->ms_seen & bit) == 0);
        }
    }
    /* the NEW FRY row (2026-09-14): under the last fish while the tank can
       still grow - what the next arrival needs, as badges that light up when
       met (the fry-to-be a silhouette at the left, a tick per gate under
       its name, a filling bar under each badge still owed) */
    if (nreq > 0) {
        int top = MSP_ROW_Y0 + t->n_fish * MSP_ROW_H;
        MS_ANIM(t->n_fish);                          /* the fry-to-be */
        ms_fry_label(&c, top, NULL);
        for (int k = 0; k < nreq; k++)           /* one tick per gate, lit when met */
            rect_fill(&c, 96 + k * 14, top + MSP_ROW_STRIP - 3, 10, 6, req[k].met ? MSP_TEAL : MSP_DIM);
        for (int k = 0; k < nreq; k++) {
            int x = MSP_BADGE_X0 + k * MSP_BADGE_DX;
            if (fry_req_icon(req[k].kind)) fry_badge(&c, fb, stride, t, x, top + MSP_ROW_BADGE, &req[k]);
            else MS_ANIM(t->n_fish + 1);         /* GROW: the youngest fish (fry_badge's, as a moving thing) */
            if (!req[k].met) {                   /* the bar: how far along */
                int w = (int)(MSP_ICON * req[k].frac + 0.5f);
                rect_fill(&c, x, top + MSP_ROW_BAR, MSP_ICON, 2, MSP_DIM);
                if (w > 0) rect_fill(&c, x, top + MSP_ROW_BAR, w, 2, MSP_TEAL);
            }
        }
    }
    /* the tank's row (its sand dollar and balance went to the foot's UPGRADES, 0.4) */
    for (int x = 24; x < PAGE_W - 24; x++) px_blend(&c, x, MSP_TANK_Y - 4, MSP_DIM, 200);
    draw_text(&c, 92, MSP_TANK_Y + 2, 2, MSP_TEAL, world_view()->tank_word(t));   /* (the terrarium's row: its habitat, DESERT) */
    for (int k = 0; k < progression_pop_cap(t); k++)     /* population strip: who is here, who could still arrive */
        page_body()->glyph(&c, 96 + k * 14, MSP_TANK_Y + 30, 2.8f, k < t->n_fish ? MSP_TEAL : MSP_DIM);
    int nb = tank_badge_n(t), np = tank_pages(t);
    if (g_ms_tpage >= np) g_ms_tpage = 0;
    for (int j = 0; j < MSP_PER_ROW; j++) {
        int k = g_ms_tpage * MSP_PER_ROW + j;
        if (k >= nb) break;
        const badge_t *tb = tank_badge(t, k);
        badge(&c, MSP_BADGE_X0 + j * MSP_BADGE_DX, MSP_TANK_Y + 4, tb->icon,
              (t->tank_ms_bits & tb->bit) != 0, (t->tank_ms_seen & tb->bit) == 0);
    }
    if (np > 1) {   /* the page arrow: toward the next page, back from the last; the badges' two-tone
                       ring round it while another page holds a badge not seen yet */
        bool last = g_ms_tpage == np - 1, fresh = false;
        for (int k = 0; k < nb; k++)
            if (k / MSP_PER_ROW != g_ms_tpage && (t->tank_ms_bits & ~t->tank_ms_seen & tank_badge(t, k)->bit)) fresh = true;
        int ax = MSP_TPG_X + 4, ay = MSP_TANK_Y + 4, aw = 16;
        ms_chevron(&c, last ? ax + 2 : ax + aw - 2, ay + MSP_ICON / 2, last, 0xffffff);
        if (fresh) { rect_edge(&c, ax - 3, ay - 3, aw + 6, MSP_ICON + 6, MSP_TEAL);
                     rect_edge(&c, ax - 4, ay - 4, aw + 8, MSP_ICON + 8, 0x3f6a72); }
        for (int p = 0; p < np; p++)        /* a pip per page under it, the current one lit */
            rect_fill(&c, ax + aw / 2 - np * 3 + p * 6 + 1, MSP_TANK_Y + 40, 4, 2, p == g_ms_tpage ? MSP_TEAL : MSP_DIM);
    }
    /* the way out: a CLOSE button in the prompt's calm dress (a tap anywhere
       else never drops the page - too much to tap for that) */
    ms_foot(t, &c);                                  /* the icon foot (0.4) */
    /* a modal up: the page under it is out of reach (any tap only closes the
       modal), so it LOOKS out of reach - every pixel at half (Strato: with
       CLOSE lit it looked like you could still tap it). One shift per
       pixel, so cheap enough for every frame. */
    if (g_ms_caption[0])
        for (int y = 0; y < TANK_H; y++)
            for (int x = 0; x < TANK_W; x++) { uint16_t n = FB_SWAP16(fb[y * stride + x]); fb[y * stride + x] = FB_SWAP16((uint16_t)((n >> 1) & 0x7bef)); }
#undef MS_ANIM
        kp->fb = fb; kp->stride = stride; kp->key = key; kp->ok = in; kp->n = an;     /* (the frame and the count: below, after the modal's own drawing) */
        memcpy(kp->b, ab, sizeof ab[0] * (size_t)an);
    }
    /* the detail modal, in the reset prompt's dress: the art at 2x, a title
       (the fish's name / TANK, or NOT YET), the milestone's words */
    if (g_ms_caption[0] && g_ms_tip) {
        /* the tip page: HOW, then the gate's lines; any tap closes it */
        const int X = MSP_MODAL_X, Y = MSP_FRY_MODAL_Y, W = MSP_MODAL_W;
        const char *const *tip = progression_fry_tip(g_ms_kind);
        int n = 0; while (tip[n]) n++;
        const int H = 56 + n * 20 + 16;
        rect_fill(&c, X, Y, W, H, 0x04141a);
        rect_edge(&c, X, Y, W, H, MSP_TEAL); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
        char title[24]; snprintf(title, sizeof title, "%s: HOW", g_ms_title);
        draw_text(&c, X + (W - text_w(title, 3)) / 2, Y + 16, 3, 0xffffff, title);
        for (int i = 0; i < n; i++) draw_text(&c, X + (W - text_w(tip[i], 2)) / 2, Y + 56 + i * 20, 2, MSP_TEAL, tip[i]);
    } else if (g_ms_caption[0]) {
        const int X = MSP_MODAL_X, W = MSP_MODAL_W;
        const int Y = ms_modal_y(t), H = ms_modal_h(t);
        const bool card = ms_card(t);
        if (card) {                                /* a fish's card is live: its name and stage as they are now, what it
                                                      would fetch (or why it stays), and the two buttons */
            const fish_t *f = &t->fish[g_ms_row];
            bool can = progression_fish_sellable(t, g_ms_row);
            int worth = progression_fish_value(t, g_ms_row);
            if (!can) g_ms_sell_armed = false; else g_ms_sell_asked = false;
            snprintf(g_ms_title, sizeof g_ms_title, "%s", f->name);
            snprintf(g_ms_caption, sizeof g_ms_caption, "%s", page_body()->stage_words[f->stage & 3]);
            ms_genes_line(f);                    /* its form under its stage (0.4) */
#if PT_NET
            char gl[sizeof g_ms_sub];
            if (give_line(t, g_ms_row, gl, sizeof gl)) memcpy(g_ms_sub, gl, sizeof g_ms_sub);   /* GIVE has the line (0.4) */
            else
#endif
            if (g_ms_sell_armed) snprintf(g_ms_sub, sizeof g_ms_sub, "TAP AGAIN TO SELL");
            else if (can) snprintf(g_ms_sub, sizeof g_ms_sub, world_view()->text.worth_fmt, worth);
            else if (g_ms_sell_asked) snprintf(g_ms_sub, sizeof g_ms_sub, f->away ? "VISITING NEXT DOOR" : !progression_keeps_two(t, g_ms_row)
                                               ? world_view()->text.keep_two : world_view()->text.name_first);
            else g_ms_sub[0] = 0;                  /* (the line's room stays: the buttons never move) */
        }
        rect_fill(&c, X, Y, W, H, 0x04141a);
        rect_edge(&c, X, Y, W, H, MSP_TEAL); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
        if (g_ms_kind >= 0)                        /* the way further in: HOW?, centred at the foot (Strato: bottom
                                                      right sat too close to CLOSE for comfort) */
            button(&c, X + (W - MSP_HOW_W) / 2, Y + H - 10 - MSP_HOW_H, MSP_HOW_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, "HOW?", 2);
#if PT_NET
        if (card && give_look(t, g_ms_row) == GV_LEAVING) {   /* accepted next door (Strato 11-01: RENAME still live "felt weird"): the
                                                                buttons give way to a GIVEN stamp in the passport's gold - nothing to tap
                                                                now - and the card closes by itself once the fish has gone */
            const int by = ms_card_btn_y(t), sw = text_w("GIVEN", 3) + 28, sh = MSP_HOW_H + 6, sx0 = X + (W - sw) / 2, sy0 = by - 3;
            rect_edge(&c, sx0, sy0, sw, sh, GIVE_GOLD); rect_edge(&c, sx0 + 3, sy0 + 3, sw - 6, sh - 6, GIVE_GOLD);
            draw_text(&c, sx0 + 14, sy0 + (sh - 21) / 2, 3, GIVE_GOLD, "GIVEN");
            g_ms_given_uid = t->fish[g_ms_row].uid;
        } else if (card && give_look(t, g_ms_row) != GV_HIDDEN) {   /* linked: RENAME | GIVE | SELL, narrower */
            const int by = ms_card_btn_y(t);
            if (give_look(t, g_ms_row) == GV_ASKING) {   /* asking next door: no new name while the other keeper decides (Strato 11-01) */
                rect_fill(&c, ms_card_btn3_x(0), by, MSP_CARD_BTN3_W, MSP_HOW_H, 0x04141a); rect_edge(&c, ms_card_btn3_x(0), by, MSP_CARD_BTN3_W, MSP_HOW_H, MSP_DIM);
                draw_text(&c, ms_card_btn3_x(0) + (MSP_CARD_BTN3_W - text_w("RENAME", 2)) / 2, by + (MSP_HOW_H - 14) / 2, 2, MSP_DIM, "RENAME");
            } else button(&c, ms_card_btn3_x(0), by, MSP_CARD_BTN3_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, "RENAME", 2);
            give_button(&c, ms_card_btn3_x(1), by, MSP_CARD_BTN3_W, MSP_HOW_H, give_look(t, g_ms_row), 2);
            const int sx = ms_card_btn3_x(2);
            if (!progression_fish_sellable(t, g_ms_row)) {
                rect_fill(&c, sx, by, MSP_CARD_BTN3_W, MSP_HOW_H, 0x04141a); rect_edge(&c, sx, by, MSP_CARD_BTN3_W, MSP_HOW_H, MSP_DIM);
                draw_text(&c, sx + (MSP_CARD_BTN3_W - text_w("SELL", 2)) / 2, by + (MSP_HOW_H - 14) / 2, 2, MSP_DIM, "SELL");
            } else if (g_ms_sell_armed) {
                char ok[16]; snprintf(ok, sizeof ok, "+%d OK?", progression_fish_value(t, g_ms_row));
                button(&c, sx, by, MSP_CARD_BTN3_W, MSP_HOW_H, 0x155e58, 0x38dcc7, ok, 2);
            } else button(&c, sx, by, MSP_CARD_BTN3_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, "SELL", 2);
        } else
#endif
        if (card) {
            const int by = ms_card_btn_y(t);
            button(&c, ms_card_btn_x(0), by, MSP_CARD_BTN_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, "RENAME", 2);
            if (!progression_fish_sellable(t, g_ms_row)) {           /* not for sale: the button in the page's dim ink, the words too */
                rect_fill(&c, ms_card_btn_x(1), by, MSP_CARD_BTN_W, MSP_HOW_H, 0x04141a);
                rect_edge(&c, ms_card_btn_x(1), by, MSP_CARD_BTN_W, MSP_HOW_H, MSP_DIM);
                draw_text(&c, ms_card_btn_x(1) + (MSP_CARD_BTN_W - text_w("SELL", 2)) / 2, by + (MSP_HOW_H - 14) / 2, 2, MSP_QUIET, "SELL");
            } else if (g_ms_sell_armed) {
                char ok[16]; snprintf(ok, sizeof ok, "+%d OK?", progression_fish_value(t, g_ms_row));
                button(&c, ms_card_btn_x(1), by, MSP_CARD_BTN_W, MSP_HOW_H, 0x155e58, 0x38dcc7, ok, 2);
            } else button(&c, ms_card_btn_x(1), by, MSP_CARD_BTN_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, "SELL", 2);
        }
        if (g_ms_icon) blit_icon_scaled(&c, X + (W - g_ms_icon->w * 2) / 2, Y + 16 + (32 - g_ms_icon->w), g_ms_icon, 2, g_ms_lit);
        else if (g_ms_fish >= 0 && g_ms_fish < t->n_fish) {
            const fish_t *f = &t->fish[g_ms_fish];
            if (g_ms_lit) fish_as_is(fb, stride, X + W / 2, Y + 48, f->size * 1.6f, f, f->color, f->fin, f->accent, FISH_ROOM_CARD, false, t->clock);
            else          fish_as_is(fb, stride, X + W / 2, Y + 48, f->size * 1.6f, f, MSP_DIM, MSP_DIM, MSP_DIM, FISH_ROOM_CARD, false, t->clock);
        } else if (g_ms_fry)
            render_fish_preview(fb, stride, X + W / 2, Y + 48, 0.9f, g_ms_lit ? MSP_TEAL : MSP_DIM, g_ms_lit ? MSP_TEAL : MSP_DIM, g_ms_lit ? MSP_TEAL : MSP_DIM, NULL, FISH_ROOM_CARD, t->clock);
        else if (g_ms_tankrow && g_ms_k < 0) ms_school(t, fb, stride, X, Y, W);   /* TANK's tally: the school */
        draw_text(&c, X + (W - text_w(g_ms_title, 3)) / 2, Y + 92, 3, g_ms_lit ? 0xffffff : MSP_TEAL, g_ms_title);
        draw_text(&c, X + (W - text_w(g_ms_caption, 2)) / 2, Y + 124, 2, g_ms_lit ? MSP_TEAL : 0x5f8a92, g_ms_caption);
        int ly = Y + 144;
        if (g_ms_caption2[0]) { draw_text(&c, X + (W - text_w(g_ms_caption2, 2)) / 2, ly, 2, g_ms_lit ? MSP_TEAL : 0x5f8a92, g_ms_caption2); ly += 20; }
        if (g_ms_sub[0]) draw_text(&c, X + (W - text_w(g_ms_sub, 2)) / 2, ly + 4, 2, g_ms_lit ? 0xffffff : MSP_TEAL, g_ms_sub);
        {   /* the arrows (2026-09-16): the previous / next of the group at the
               top corners, in the buttons' dress, only when there is a group */
            int idx, n = ms_group(t, nreq, &idx);
            if (n > 1) {
                int ax = X + MSP_ARROW_IN, ay = Y + MSP_ARROW_IN, bx = X + W - MSP_ARROW_IN - MSP_ARROW_W;
                rect_fill(&c, ax, ay, MSP_ARROW_W, MSP_ARROW_H, 0x1c2f36);
                rect_edge(&c, ax, ay, MSP_ARROW_W, MSP_ARROW_H, MSP_TEAL); rect_edge(&c, ax + 1, ay + 1, MSP_ARROW_W - 2, MSP_ARROW_H - 2, MSP_TEAL);
                ms_chevron(&c, ax + 14, ay + MSP_ARROW_H / 2, true, 0xffffff);
                rect_fill(&c, bx, ay, MSP_ARROW_W, MSP_ARROW_H, 0x1c2f36);
                rect_edge(&c, bx, ay, MSP_ARROW_W, MSP_ARROW_H, MSP_TEAL); rect_edge(&c, bx + 1, ay + 1, MSP_ARROW_W - 2, MSP_ARROW_H - 2, MSP_TEAL);
                ms_chevron(&c, bx + MSP_ARROW_W - 14, ay + MSP_ARROW_H / 2, false, 0xffffff);
            }
        }
    }
#if PT_NET
    if (g_gm.on) gm_draw(t, fb, stride);             /* the GIVE modal over the page, whole every frame (ms_key: up or not) */
    if (render_gift_up(t)) render_gift(t, fb, stride);   /* a gift offered to us over both (the 11-02 sweep; ms_key: up or not) */
#endif
    g_ms_in = false;
    kp->frame = g_rframe; kp->other = g_ctx_other;   /* what this buffer holds now (anything drawn after: CTX_MADE) */
}

int render_milestones_tap(const tank_t *t, float x, float y) {
#if PT_NET
    if (gm_eval(t) != RENDER_GM_OFF) { render_give_modal_tap(t, x, y); return MS_TAP_KEPT; }   /* the GIVE modal over the page has every tap */
#endif
    x -= PAGE_X; y -= PAGE_Y;                    /* the page's own coordinates */
    if (g_ms_caption[0]) {                       /* a modal is up */
        if (!g_ms_tip) {                         /* the arrows at its top corners: the previous / next of the group */
            fry_req_t req[FRY_REQ_MAX]; bool staged; int idx;
            int n = ms_group(t, progression_next_fry(t, req, &staged), &idx);
            const int Y = ms_modal_y(t);
            if (n > 1 && y >= Y - 12 && y < Y + MSP_ARROW_IN + MSP_ARROW_H + 24) {
                if (x < MSP_MODAL_X + MSP_ARROW_HIT)               { ms_step(t, -1); return MS_TAP_KEPT; }
                if (x >= MSP_MODAL_X + MSP_MODAL_W - MSP_ARROW_HIT) { ms_step(t, +1); return MS_TAP_KEPT; }
            }
        }
        if (ms_card(t)) {                        /* a fish's card: RENAME / SELL across its foot - each button's half of
                                                    the panel, the HOW? button's depth (a miss costs the modal) */
            const int by = ms_card_btn_y(t), fish = g_ms_row;
            if (x >= MSP_MODAL_X && x < MSP_MODAL_X + MSP_MODAL_W && y >= by - MSP_HOW_SLOP_UP && y < by + MSP_HOW_H + MSP_HOW_SLOP_DN) {
#if PT_NET
                if (give_look(t, fish) == GV_LEAVING) return MS_TAP_KEPT;   /* GIVEN: the stamp is no button */
                if (give_look(t, fish) != GV_HIDDEN) {   /* linked: three buttons, each its third of the panel */
                    if (x < ms_card_btn3_x(1) - MSP_CARD_BTN3_GAP / 2) {
                        if (give_look(t, fish) == GV_ASKING) return MS_TAP_KEPT;   /* (RENAME rests while the gift is asked) */
                        ms_close(); return MS_TAP_RENAME + fish; }
                    if (x < ms_card_btn3_x(2) - MSP_CARD_BTN3_GAP / 2) { give_tap(t, fish); return MS_TAP_KEPT; }
                } else
#endif
                if (x < MSP_MODAL_X + MSP_MODAL_W / 2) { ms_close(); return MS_TAP_RENAME + fish; }   /* the platform opens the wheel */
                if (!progression_fish_sellable(t, fish)) { g_ms_sell_asked = true; return MS_TAP_KEPT; }   /* the line says why, from now on */
                if (!g_ms_sell_armed) { g_ms_sell_armed = true; g_ms_sell_clock = t->clock; return MS_TAP_KEPT; }
                if (t->clock - g_ms_sell_clock < MSP_SELL_WAIT_S) { g_ms_sell_clock = t->clock; return MS_TAP_KEPT; }   /* a doubled tap (or a mash:
                                                                   arm, then tap on at 5 Hz sold at the 0.6 s mark): still armed, the wait again from it */
                ms_close(); return MS_TAP_SELL + fish;                                               /* the platform sells it */
            }
        }
        if (g_ms_kind >= 0 && !g_ms_tip) {       /* a gate's: the HOW? button opens its tip page */
            const int H = ms_modal_h(t);
            const int bx = MSP_MODAL_X + (MSP_MODAL_W - MSP_HOW_W) / 2, by = MSP_FRY_MODAL_Y + H - 10 - MSP_HOW_H;
            if (x >= bx - MSP_HOW_SLOP_X && x < bx + MSP_HOW_W + MSP_HOW_SLOP_X && y >= by - MSP_HOW_SLOP_UP && y < by + MSP_HOW_H + MSP_HOW_SLOP_DN) {
                g_ms_tip = true; return MS_TAP_KEPT; }
        }
        ms_close(); return MS_TAP_KEPT;                  /* any other tap: back to the page */
    }
    if (PAGE_BOWL) {                                 /* the bowl: the cog in the cap above the rows */
        int sx, sy; render_ms_foot_box(MSF_SET, &sx, &sy);
        if (y < sy + FS_SQ + 8) return x >= sx - 24 && x < sx + FS_SQ + 24 ? MS_TAP_SETTINGS : MS_TAP_NONE;
    }
    if (y >= MSP_FOOT_HIT_Y) {                       /* the foot's strip down to the glass: the nearest square along it */
        static const int TAP[MSF_N] = { MS_TAP_SHOP, MS_TAP_SETTINGS, MS_TAP_TANKS, MS_TAP_VISITORS, MS_TAP_CLOSE };
        int best = -1; float bd = 1e9f;
        for (int b = 0; b < MSF_N; b++) {
            if ((PAGE_BOWL && b == MSF_SET) || !MSF_SHOWN(b)) continue;
            int bx, by; render_ms_foot_box(b, &bx, &by);
            float d = fabsf(x - (bx + FS_SQ / 2.0f));
            if (d < bd) { bd = d; best = b; }
        }
        return TAP[best];
    }
    int row = -1; bool tank_row = false, fry_row = false;
    fry_req_t req[FRY_REQ_MAX]; bool staged;
    int nreq = progression_next_fry(t, req, &staged);
    if (y >= MSP_ROW_Y0 - 2 && y < MSP_ROW_Y0 + N_FISH_MAX * MSP_ROW_H) {
        row = (int)((y - MSP_ROW_Y0) / MSP_ROW_H);
        if (row < 0) row = 0;
        if (row == t->n_fish && nreq > 0) fry_row = true;   /* the NEW FRY row */
        else if (row >= t->n_fish) return MS_TAP_NONE;      /* an empty row */
    } else if (y >= MSP_TANK_Y - 6 && y < MSP_FOOT_HIT_Y) tank_row = true;    /* down to the button strip: fingers near the
                                                                              bottom bezel report LOW */
    else return MS_TAP_NONE;
    if (tank_row && x >= MSP_TPG_X && tank_pages(t) > 1) {   /* the page arrow: the next page, wrapping */
        g_ms_tpage = (g_ms_tpage + 1) % tank_pages(t); return MS_TAP_KEPT; }
    int k;                                           /* badge column, or -1 for the name / strip cluster */
    if (x >= MSP_BADGE_X0 - 4 && x < MSP_BADGE_X0 + 6 * MSP_BADGE_DX) {
        k = (int)((x - MSP_BADGE_X0 + 4) / MSP_BADGE_DX);
        if (k > 5) k = 5;
    } else if (x >= 20 && x < MSP_BADGE_X0 - 4) k = -1;
    else return MS_TAP_NONE;
    if (tank_row && k >= 0) {                        /* this page's badge; an empty cell on the last page is nothing */
        k += g_ms_tpage * MSP_PER_ROW;
        if (k >= tank_badge_n(t)) return MS_TAP_NONE;
    }
    return ms_open(t, row, tank_row, fry_row, k, req, nreq, staged);
}
/* open the detail modal for a row's name / strip (k < 0) or its k-th badge
   or gate: the words, the art, and where it came from (the arrows' group) */
static int ms_open(const tank_t *t, int row, bool tank_row, bool fry_row, int k,
                   const fry_req_t *req, int nreq, bool staged) {
    g_ms_caption2[0] = 0; g_ms_sub[0] = 0; g_ms_fry = false; g_ms_kind = -1; g_ms_tip = false; g_ms_sell_armed = false; g_ms_sell_asked = false;
    g_ms_row = row; g_ms_k = k; g_ms_tankrow = tank_row; g_ms_fryrow = fry_row;
    if (fry_row) {
        if (k < 0) {                                 /* the name: the tally, and when it comes */
            int met = 0; for (int i = 0; i < nreq; i++) met += req[i].met;
            snprintf(g_ms_title, sizeof g_ms_title, "%s", world_view()->text.new_arrival);   /* (the terrarium's arrival: an egg hatches in the humid hide) */
            if (staged) { snprintf(g_ms_caption, sizeof g_ms_caption, "%s", world_view()->text.steps_done);   /* the spawning, 2026-09-24 */
                          snprintf(g_ms_caption2, sizeof g_ms_caption2, "%s", world_view()->text.born_where);
                          snprintf(g_ms_sub, sizeof g_ms_sub, "ON ITS WAY"); }
            else { snprintf(g_ms_caption, sizeof g_ms_caption, "%s", world_view()->text.needs_met);     /* Strato's words, 2026-09-14 */
                   snprintf(g_ms_caption2, sizeof g_ms_caption2, "%s", world_view()->text.ready_to_be_born);
                   snprintf(g_ms_sub, sizeof g_ms_sub, "%d OF %d DONE", met, nreq); }
            g_ms_lit = staged; g_ms_icon = NULL; g_ms_fish = -1; g_ms_fry = true;
        } else if (k < nreq) {                       /* a gate: the words, and where it stands */
            const fry_req_t *r = &req[k];
            snprintf(g_ms_title, sizeof g_ms_title, "%s", r->title);
            snprintf(g_ms_caption, sizeof g_ms_caption, "%s", r->words);
            snprintf(g_ms_caption2, sizeof g_ms_caption2, "%s", r->words2);
            snprintf(g_ms_sub, sizeof g_ms_sub, "%s", r->progress);
            g_ms_lit = r->met; g_ms_icon = fry_req_icon(r->kind); g_ms_kind = r->kind;
            g_ms_fish = g_ms_icon ? -1 : t->n_fish - 1;
        } else return MS_TAP_NONE;
    } else if (tank_row) {
        if (k < 0) {
            snprintf(g_ms_title, sizeof g_ms_title, "%.15s", world_view()->tank_word(t));   /* (a pack's name is up to 23: the title holds 15) */
            snprintf(g_ms_caption, sizeof g_ms_caption, world_view()->text.so_far_fmt, t->n_fish, progression_pop_cap(t));
            g_ms_lit = true; g_ms_icon = NULL; g_ms_fish = -1;
        } else {
            uint32_t bit = tank_badge(t, k)->bit; bool on = (t->tank_ms_bits & bit) != 0;
            g_ms_tpage = k / MSP_PER_ROW;            /* the arrows can cross pages: the row follows */
            snprintf(g_ms_title, sizeof g_ms_title, "%.15s", on ? (world_view()->tank_word(t)) : "NOT YET");
            snprintf(g_ms_caption, sizeof g_ms_caption, "%s", TMS_NAMES[bit_index(bit)]);
            g_ms_lit = on; g_ms_icon = tank_badge(t, k)->icon; g_ms_fish = -1;
        }
    } else {
        const fish_t *f = &t->fish[row];
        if (k < 0) {
            snprintf(g_ms_title, sizeof g_ms_title, "%s", f->name);
            snprintf(g_ms_caption, sizeof g_ms_caption, "%s", page_body()->stage_words[f->stage & 3]);
            ms_genes_line(f);                    /* its form under its stage (0.4) */
            g_ms_lit = true; g_ms_icon = NULL; g_ms_fish = row;
        } else {
            uint32_t bit = world_view()->fish_badges[k].bit; bool on = (f->ms_bits & bit) != 0;
            snprintf(g_ms_title, sizeof g_ms_title, "%s", on ? f->name : "NOT YET");
            snprintf(g_ms_caption, sizeof g_ms_caption, "%s", MS_NAMES[bit_index(bit)]);
            g_ms_lit = on; g_ms_icon = world_view()->fish_badges[k].icon; g_ms_fish = -1;
        }
    }
    return MS_TAP_KEPT;
}
/* a sideways swipe along the TANK row: the next page (leftward) or the
   previous (rightward), no wrap. False = not a page swipe (the platforms then
   treat the release as they did before). */
bool render_milestones_swipe(const tank_t *t, float x, float y, float dx) {
    (void)x; y -= PAGE_Y;
    if (g_ms_caption[0] || tank_pages(t) < 2 || y < MSP_TANK_Y - 6 || y >= MSP_FOOT_HIT_Y) return false;
    int p = g_ms_tpage + (dx < 0 ? 1 : -1);
    if (p >= 0 && p < tank_pages(t)) g_ms_tpage = p;
    return true;
}
/* the modal down, the page (and the tank row's page) kept */
static void ms_close(void) { g_ms_given_uid = 0; g_ms_sell_armed = false; g_ms_sell_asked = false; g_ms_kind = -1; g_ms_tip = false; g_ms_caption[0] = 0; g_ms_caption2[0] = 0; g_ms_title[0] = 0; g_ms_sub[0] = 0; g_ms_icon = NULL; g_ms_fish = -1; g_ms_fry = false;
                                     g_ms_row = -1; g_ms_k = -1; g_ms_tankrow = false; g_ms_fryrow = false; }
void render_milestones_leave(void) { ms_close(); g_ms_tpage = 0; }

#if PT_NET   /* the VISITOR COLLECTION page (version.h PT_NET) */
/* ---- the VISITOR COLLECTION (2026-10-06, Strato: "a grid of friends made and
   if you tap one it shows their passport and whatever additional info you
   think would be nice") ---- */
#define COLL_COLS 4
#define COLL_CW   92
#define COLL_CH   80
#define COLL_X0   ((PAGE_W - COLL_COLS * COLL_CW) / 2)
#define COLL_Y0   60
static int g_coll_sel = -1;
/* PAGES (0.4, Strato 11-03, approved from the mockup): 12 a page, up to 10. The arrows in the critter cards' dress at
   the title's ends (only with more than one page: a small collection's page is as it was), wrapping like NEXT FISH;
   the page in words and a pip per page in the foot - the page's pip a filled block, the others hollow (shape and
   words, never hue); it opens on page 1 */
static int g_coll_page;
#ifdef TANK_ROUND
#define COLL_NAV_X 76                  /* the bowl: the arrows' top corners 6 px inside the circle */
#define COLL_NAV_Y 14
#else
#define COLL_NAV_X 72
#define COLL_NAV_Y 12
#endif
#define COLL_PIP 10                    /* a pip's pitch: 6 px hollow, the page's 8 px filled */
void render_collection_leave(void) { g_coll_sel = -1; g_coll_page = 0; }
int  render_collection_page(void) { return g_coll_page; }
void render_collection_set_page(int page) { int np = collection_pages(); g_coll_page = page < 0 ? 0 : page >= np ? np - 1 : page; }
bool render_collection_arrow(bool right, int *x, int *y) {
    if (collection_pages() < 2) return false;
    if (x) *x = PAGE_X + (right ? PAGE_W - COLL_NAV_X - RENDER_CRIT_NAV_W / 2 : COLL_NAV_X + RENDER_CRIT_NAV_W / 2);
    if (y) *y = PAGE_Y + COLL_NAV_Y + RENDER_CRIT_NAV_W / 2;
    return true;
}
static void fmt_dur(char *out, size_t n, uint32_t s) {
    if (s < 60) snprintf(out, n, "%u S", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%u MIN", (unsigned)(s / 60));
    else snprintf(out, n, "%u H %u MIN", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
}
#endif
/* a fish left and the slots shifted (progression: a sale, a gift): the keeper page's card and its armed SELL follow their
   fish; the card of the one that left closes (the 11-02 sweep: SELL armed by row sold the fish that slid into it) */
void render_fish_removed(int idx) {
    if (!g_ms_tankrow && g_ms_row >= 0) {
        if (g_ms_row == idx && !g_ms_fryrow) { ms_close(); g_ms_row = -1; }
        else if (g_ms_row > idx) g_ms_row--;
    }
    if (g_ms_fish == idx) g_ms_fish = -1; else if (g_ms_fish > idx) g_ms_fish--;
}
#if PT_NET
static void fmt_met(char *out, size_t n, uint32_t met) {
    int64_t now = clock_port_now_unix();
    if (!met || now <= 0 || now < met) { snprintf(out, n, "ON THIS GLASS"); return; }   /* (a board without a clock) */
    uint32_t d = (uint32_t)((now - met) / 86400);
    if (d == 0) snprintf(out, n, "TODAY"); else if (d == 1) snprintf(out, n, "YESTERDAY"); else snprintf(out, n, "%u DAYS AGO", (unsigned)d);
}
static void entry_fish(fish_t *f, const coll_entry_t *e) {
    memset(f, 0, sizeof *f);
    f->color = e->color; f->fin = e->fin; f->accent = e->accent; f->stage = (stage_t)e->stage;
    f->shape = e->shape; f->fins = e->fins; f->pattern = e->pattern; f->uid = e->uid;
    memcpy(f->name, e->name, FISH_NAME_MAX); f->name[FISH_NAME_MAX] = 0;
}
/* a collected fish's passport, with its history here */
static void entry_card_draw(ctx_t *c, uint16_t *fb, int stride, const coll_entry_t *e, double clock) {
    const uint32_t GOLD = 0xe8c872, INK = 0x13324f, LBL = 0x7fa3b5;
    const int W = PASSPORT_CARD_W, H = PASSPORT_BTN_Y, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x0e2233, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 248);
    for (int y = Y + 34; y < Y + H - 30; y += 7)
        for (int x = X + 3; x < X + W - 3; x++) px_blend(c, x, y + (int)(2.0f * sinf(x * 0.11f + y * 0.37f)), 0x2b4a63, 80);
    src_t band = src_color(INK, 1.0f);
    for (int y = Y; y < Y + 26; y++) span(c, X, X + W - 1, y, &band, 255);
    draw_text(c, X + (W - text_w("POCKET TANK PASSPORT", 2)) / 2, Y + 7, 2, GOLD, "POCKET TANK PASSPORT");
    rect_edge(c, X, Y, W, H, GOLD); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, INK);
    const int PX = X + 12, PY = Y + 36, PW = 100, PH = 84;
    rect_fill(c, PX, PY, PW, PH, 0x0a1824); rect_edge(c, PX, PY, PW, PH, 0x9fd8e2);
    fish_t f; entry_fish(&f, e);
    render_fish_portrait(fb, stride, PX + PW / 2.0f - PAGE_X, PY + PH / 2.0f - PAGE_Y, 1.3f, &f, PW - 20, clock);
    const int FX = PX + PW + 12;
    static const char *const STAGE_W[4] = { "FRY", "JUVENILE", "ADULT", "ELDER" };
    char home[24], form[24], met[24], here[24], visits[24];
    char hlbl[40]; home_fields(e->home, e->home_name, "HOME", hlbl, sizeof hlbl, home, sizeof home);
    snprintf(form, sizeof form, "%s %s", SHAPE_NAMES[e->shape % SHAPE_N], FINS_NAMES[e->fins % FINS_N]);
    fmt_met(met, sizeof met, e->met_unix); fmt_dur(here, sizeof here, e->here_s);
    snprintf(visits, sizeof visits, "%u VISIT%s", (unsigned)e->visits, e->visits == 1 ? "" : "S");
    struct { const char *lbl, *val; } FL[4] = { { "NAME", f.name }, { hlbl, home }, { "FORM", form }, { "STAGE", STAGE_W[e->stage & 3] } };
    for (int i = 0; i < 4; i++) {
        int fy = Y + 33 + i * 26;
        draw_text(c, FX, fy, 1, LBL, FL[i].lbl);
        int vx = FX;                                  /* (no shape: a collected fish's home is its waters, for good) */
        draw_text(c, vx, fy + 10, 2, 0xffffff, FL[i].val);
    }
    /* its history here: first met, visits, time in this water, fry */
    draw_text(c, PX, PY + PH + 6, 1, LBL, "FIRST MET");
    draw_text(c, PX, PY + PH + 15, 2, 0xffffff, met);
    draw_text(c, FX, Y + 139, 1, LBL, "IN YOUR TANK");
    draw_text(c, FX, Y + 148, 2, 0xffffff, here);
    draw_text(c, PX, PY + PH + 34, 1, LBL, "VISITS");
    draw_text(c, PX, PY + PH + 43, 2, 0xffffff, visits);
    if (e->fry) {
        char fr[24]; snprintf(fr, sizeof fr, "%u FRY HERE", (unsigned)e->fry);
        draw_text(c, FX, Y + 167, 1, LBL, "FAMILY");
        draw_text(c, FX, Y + 176, 2, 0xffffff, fr);
    }
    const char *st = "COLLECTED";                      /* the stamp, as on its card */
    int sw = text_w(st, 2) + 12, sx0 = X + W - sw - 10, sy0 = Y + 139;
    rect_edge(c, sx0, sy0, sw, 24, GOLD); rect_edge(c, sx0 + 2, sy0 + 2, sw - 4, 20, GOLD);
    draw_text(c, sx0 + 6, sy0 + 5, 2, GOLD, st);
    char m1[64]; int cols = (W - 20) / 6; if (cols > 60) cols = 60;
    snprintf(m1, sizeof m1, "P<FISH<<%s<<%08X", f.name, (unsigned)e->uid);
    for (int j = (int)strlen(m1); j < cols; j++) m1[j] = '<';
    m1[cols] = 0;
    draw_text(c, X + 10, Y + H - 13, 1, 0xc8d6de, m1);
}
/* the page's words and pips in the foot, left of CLOSE (the bowl: two lines in UPGRADES' place, the pips under the grid) */
static void coll_pager(ctx_t *c, int pg, int np) {
    char line[32];
#if defined(TANK_ROUND)
    char l2[24]; snprintf(line, sizeof line, "PAGE %d", pg + 1); snprintf(l2, sizeof l2, "OF %d", np);
    draw_text(c, 114 + (116 - text_w(line, 2)) / 2, MSP_CLOSE_Y, 2, 0xffffff, line);
    draw_text(c, 114 + (116 - text_w(l2, 2)) / 2, MSP_CLOSE_Y + 16, 2, 0xffffff, l2);
    int bx0 = (PAGE_W - (np * COLL_PIP - 4)) / 2, by = COLL_Y0 + 3 * COLL_CH + 3;
#else
    snprintf(line, sizeof line, "PAGE %d OF %d", pg + 1, np);
    int ty = MSP_CLOSE_Y + (MSP_CLOSE_H - 14) / 2, tx = 32;   /* (as far in as CLOSE stands from the right) */
    draw_text(c, tx, ty, 2, 0xffffff, line);
    char widest[32]; snprintf(widest, sizeof widest, "PAGE %d OF %d", np, np);   /* (the pips stand still page to page) */
    int bx0 = tx + text_w(widest, 2) + 12, by = ty + 4;
#endif
    for (int p = 0; p < np; p++) {
        int bx = bx0 + p * COLL_PIP;
        if (p == pg) rect_fill(c, bx - 1, by - 1, 8, 8, 0xffffff); else rect_edge(c, bx, by, 6, 6, MSP_TEAL);
    }
}
void render_collection(const tank_t *t, uint16_t *fb, int stride) {
    ctx_t c = ctx_page(fb, stride);
    for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) fb[y * stride + x] = rgb565(MSP_INK, 1);   /* (the milestones page's backdrop) */
    char line[32]; int n = collection_count(), np = collection_pages();
    if (g_coll_page >= np) g_coll_page = np - 1;
    draw_text(&c, (PAGE_W - text_w("VISITOR COLLECTION", 2)) / 2, 22, 2, 0xe8c872, "VISITOR COLLECTION");
    snprintf(line, sizeof line, "%d OF %d COLLECTED", n, COLL_MAX);
    draw_text(&c, (PAGE_W - text_w(line, 1)) / 2, 42, 1, MSP_TEAL, line);
    if (np > 1) {
        crit_nav_box(&c, COLL_NAV_X, COLL_NAV_Y, -1, 0x9fd8e2);
        crit_nav_box(&c, PAGE_W - COLL_NAV_X - RENDER_CRIT_NAV_W, COLL_NAV_Y, 1, 0x9fd8e2);
        coll_pager(&c, g_coll_page, np);
    }
    for (int j = 0; j < COLL_PAGE; j++) {
        int i = g_coll_page * COLL_PAGE + j;
        int cx = COLL_X0 + (j % COLL_COLS) * COLL_CW, cy = COLL_Y0 + (j / COLL_COLS) * COLL_CH;
        if (i >= n) { rect_edge(&c, cx + 6, cy + 4, COLL_CW - 12, COLL_CH - 12, 0x1c2f36); continue; }   /* an empty place to fill */
        const coll_entry_t *e = collection_get(i);
        rect_edge(&c, cx + 6, cy + 4, COLL_CW - 12, COLL_CH - 12, 0x3f6a72);
        fish_t f; entry_fish(&f, e);
        render_fish_portrait(fb, stride, cx + COLL_CW / 2.0f, cy + 30, 0.9f, &f, COLL_CW - 26, t->clock + j * 0.7f);
        draw_text(&c, cx + (COLL_CW - text_w(f.name, 2)) / 2, cy + 52, 2, 0xffffff, f.name);
    }
    button(&c, MSP_CLOSE_X, MSP_CLOSE_Y, MSP_CLOSE_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "CLOSE", 2);
    if (g_coll_sel >= 0 && g_coll_sel < n) {           /* its passport, the page dimmed under it */
        for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) fb[y * stride + x] = (uint16_t)((fb[y * stride + x] >> 1) & 0x7bef);
        ctx_t fc = ctx_full(fb, stride, 1.0f);
        entry_card_draw(&fc, fb, stride, collection_get(g_coll_sel), t->clock);
    }
}
int render_collection_tap(const tank_t *t, float x, float y) {
    (void)t;
    if (g_coll_sel >= 0) { g_coll_sel = -1; return COLL_TAP_KEPT; }   /* any tap: back to the grid */
    float px = x - PAGE_X, py = y - PAGE_Y;
    if (px >= MSP_CLOSE_X - 8 && py >= MSP_CLOSE_Y - 6) return COLL_TAP_CLOSE;
    int np = collection_pages();
    if (np > 1 && py < COLL_NAV_Y + RENDER_CRIT_NAV_W + 12) {   /* the arrows: their box and 12 px round it (the grid starts below) */
        if (px >= COLL_NAV_X - 12 && px < COLL_NAV_X + RENDER_CRIT_NAV_W + 12) { g_coll_page = (g_coll_page + np - 1) % np; return COLL_TAP_KEPT; }
        if (px >= PAGE_W - COLL_NAV_X - RENDER_CRIT_NAV_W - 12 && px < PAGE_W - COLL_NAV_X + 12) { g_coll_page = (g_coll_page + 1) % np; return COLL_TAP_KEPT; }
    }
    if (px >= COLL_X0 && px < COLL_X0 + COLL_COLS * COLL_CW && py >= COLL_Y0 && py < COLL_Y0 + 3 * COLL_CH) {
        int i = g_coll_page * COLL_PAGE + (int)((py - COLL_Y0) / COLL_CH) * COLL_COLS + (int)((px - COLL_X0) / COLL_CW);
        if (i < collection_count()) { g_coll_sel = i; return COLL_TAP_KEPT; }
    }
    return COLL_TAP_NONE;
}
#endif
void render_milestones_show_fish(const tank_t *t, int fish) {
    if (fish >= 0 && fish < t->n_fish) ms_open(t, fish, false, false, -1, NULL, 0, false);
}
bool render_milestones_card(const tank_t *t, int *fish, int *rename_x, int *sell_x, int *btn_y) {
    if (!ms_card(t)) return false;
    if (fish) *fish = g_ms_row;
#if PT_NET
    bool three = give_look(t, g_ms_row) != GV_HIDDEN;   /* linked: RENAME | GIVE | SELL */
#else
    const bool three = false;
#endif
    if (rename_x) *rename_x = PAGE_X + (three ? ms_card_btn3_x(0) + MSP_CARD_BTN3_W / 2 : ms_card_btn_x(0) + MSP_CARD_BTN_W / 2);
    if (sell_x)   *sell_x   = PAGE_X + (three ? ms_card_btn3_x(2) + MSP_CARD_BTN3_W / 2 : ms_card_btn_x(1) + MSP_CARD_BTN_W / 2);
    if (btn_y)    *btn_y    = PAGE_Y + ms_card_btn_y(t) + MSP_HOW_H / 2;
    return true;
}
const char *render_milestones_line(void) { return g_ms_sub; }
#if PT_NET
bool render_milestones_give(const tank_t *t, int *x, int *y) {
    if (!ms_card(t) || give_look(t, g_ms_row) == GV_HIDDEN) return false;
    if (x) *x = PAGE_X + ms_card_btn3_x(1) + MSP_CARD_BTN3_W / 2;
    if (y) *y = PAGE_Y + ms_card_btn_y(t) + MSP_HOW_H / 2;
    return true;
}
#endif
bool render_milestones_arrow(const tank_t *t, bool right, int *x, int *y) {
    if (!g_ms_caption[0] || g_ms_tip) return false;
    if (x) *x = PAGE_X + (right ? MSP_MODAL_X + MSP_MODAL_W - MSP_ARROW_IN - MSP_ARROW_W / 2 : MSP_MODAL_X + MSP_ARROW_IN + MSP_ARROW_W / 2);
    if (y) *y = PAGE_Y + ms_modal_y(t) + MSP_ARROW_IN + MSP_ARROW_H / 2;
    return true;
}
void render_milestones_row(int row, int *name_x, int *y) {
    if (name_x) *name_x = PAGE_X + 100;
    if (y)      *y      = PAGE_Y + MSP_ROW_Y0 + row * MSP_ROW_H + MSP_ROW_MID;
}

/* ---- announcement modal (notice.h, 2026-09-15) ----
 * The milestones page's detail modal, over the live tank: the badge art at
 * 2x (a fish's own sprite for a stage), the name, the caption, and a thin
 * bar along the foot that runs out with the notice's time. */
void render_notice(const tank_t *t, uint16_t *fb, int stride, int kind, int fish, uint32_t bit, float frac_left) {
    ctx_t c = ctx_page(fb, stride);
    const int X = MSP_MODAL_X, W = MSP_MODAL_W, Y = MSP_MODAL_Y, H = MSP_MODAL_H;
    rect_fill(&c, X, Y, W, H, 0x04141a);
    rect_edge(&c, X, Y, W, H, MSP_TEAL); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    char title[FISH_NAME_MAX + 16], caption[40] = "";
    snprintf(title, sizeof title, "%s", world_view()->text.the_tank);
    const char *caption2 = NULL;                              /* a second line: the title and both move up */
    const icon_t *ic = NULL;
    const fish_t *f = fish >= 0 && fish < t->n_fish ? &t->fish[fish] : NULL;
    if (kind == 6 || kind == 7 || kind == 8) {                /* NOTICE_MODEL_DAMAGED (2026-10-21): the AI model is damaged; */
        if (kind == 8) {                                      /* NOTICE_PACK_MISSING (issue #4): the tank's save names a pack this build does not have (phase 1) - or both are here and the species needs what the habitat, the firmware or the model lacks (phase 2) */
            const char *w = progression_packs_missing_name(t), *need = progression_packs_need(t);
            if (need[0]) {                                    /* PACK NEEDS BASK: the title when it fits the modal; a longer need (a goal's word) goes to the caption under a bare PACK NEEDS */
                snprintf(title, sizeof title, "PACK NEEDS %s", need);
                if (text_w(title, 3) > W - 12) { snprintf(title, sizeof title, "PACK NEEDS"); snprintf(caption, sizeof caption, "%s", need); }
                else snprintf(caption, sizeof caption, "%s", w[0] ? w : "A PACK THIS TANK USES");
            } else {
                snprintf(title, sizeof title, "PACK MISSING");
                snprintf(caption, sizeof caption, "%s", w[0] ? w : "A PACK THIS TANK USES");
            }
            caption2 = world_view()->text.the_tank_waits;
        } else if (kind == 7) {                               /* NOTICE_WRONG_BOARD (0.3.9.3): this version is another board's */
            const char *w = pt_board_words(board_port_hw());
            snprintf(title, sizeof title, "WRONG VERSION");
            snprintf(caption, sizeof caption, "THIS IS THE %s", w ? w : "BOARD");
            caption2 = "CHECK FOR UPDATES FIXES IT";
        } else {
            snprintf(title, sizeof title, "AI MODEL DAMAGED");
            snprintf(caption, sizeof caption, "REINSTALL AT POCKETANK.COM");
            caption2 = world_view()->text.animals_safe;
        }
        /* a warning triangle with an exclamation mark: the shape says it, not the hue */
        const int TH = 34, PX = X + W / 2, PY = Y + 14;      /* apex at the top, base TH below */
        for (int r = 0; r <= TH; r++) {
            int half = r * 20 / TH;
            rect_fill(&c, PX - half, PY + r, 2, 1, MSP_TEAL); rect_fill(&c, PX + half - 1, PY + r, 2, 1, MSP_TEAL);
        }
        rect_fill(&c, PX - 20, PY + TH - 1, 41, 2, MSP_TEAL);
        rect_fill(&c, PX - 1, PY + 11, 3, 13, MSP_TEAL);      /* ! */
        rect_fill(&c, PX - 1, PY + 27, 3, 3, MSP_TEAL);
    } else if (kind == 5) {                                   /* NOTICE_LIGHTS_OUT (2026-10-03): the first double-tap that turned the light off */
        snprintf(title, sizeof title, "LIGHTS OUT");
        snprintf(caption, sizeof caption, "YOU DOUBLE-TAPPED THE GLASS");
        caption2 = "DOUBLE-TAP TO TURN IT ON";
        /* a crescent moon: a disc with a bite out of it */
        const int R = 20, PX = X + W / 2 - 3, PY = Y + 36;
        for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
            int bx = dx - 10, by = dy + 7;
            if (dx * dx + dy * dy <= R * R && bx * bx + by * by > 17 * 17) rect_fill(&c, PX + dx, PY + dy, 1, 1, MSP_TEAL);
        }
    } else if (kind == 4) {                                   /* NOTICE_UPDATED (2026-09-30): the first boot of a new release */
        snprintf(title, sizeof title, "UPDATED");
        snprintf(caption, sizeof caption, world_view()->text.now_version_fmt, PT_RELEASE);
        /* a tick in a ring, teal: the shape says it, not the hue */
        const int R = 22, PX = X + W / 2, PY = Y + 48;
        ring(&c, PX, PY, R, MSP_TEAL); ring(&c, PX, PY, R - 1, MSP_TEAL);
        for (int i = 0; i < 6; i++)  rect_fill(&c, PX - 11 + i, PY + i - 1, 3, 3, MSP_TEAL);
        for (int i = 0; i < 12; i++) rect_fill(&c, PX - 6 + i, PY + 4 - i, 3, 3, MSP_TEAL);
    } else if (kind == 3) {                                   /* NOTICE_LOW_BATTERY */
        snprintf(title, sizeof title, "LOW BATTERY");
        snprintf(caption, sizeof caption, "%s", world_view()->text.please_charge);
        /* the pill, large: outline + nub, the last sliver lit red */
        const int PW = 60, PH = 28, PX = X + (W - PW) / 2, PY = Y + 34;
        rect_edge(&c, PX, PY, PW, PH, 0x9fb4b8); rect_edge(&c, PX + 1, PY + 1, PW - 2, PH - 2, 0x9fb4b8);
        rect_fill(&c, PX + PW, PY + 8, 5, PH - 16, 0x9fb4b8);
        rect_fill(&c, PX + 4, PY + 4, 7, PH - 8, 0xf25b65);
    } else if (kind == 2) {                                   /* NOTICE_STAGE */
        if (f) { snprintf(title, sizeof title, "%s", f->name);
                 snprintf(caption, sizeof caption, "IS NOW %s %s", f->stage == STAGE_ADULT || f->stage == STAGE_ELDER ? "AN" : "A", page_body()->stage_words[f->stage & 3]);
                 render_fish_preview(fb, stride, X + W / 2, Y + 48, f->size * 1.6f, f->color, f->fin, f->accent, f, FISH_ROOM_CARD, t->clock); }
    } else if (kind == 1) {                                   /* NOTICE_TANK_MILESTONE */
        for (int k = 0; k < world_view()->n_tank_badges; k++) if (world_view()->tank_badges[k].bit == bit) ic = world_view()->tank_badges[k].icon;
        int bi = 0; while (bi < 31 && !(bit & (1u << bi))) bi++;
        snprintf(caption, sizeof caption, "%s", bi < TMS_COUNT ? TMS_NAMES[bi] : "");
        if (bit == TMS_FIRST_SALE) caption2 = world_view()->text.newborns_in_shop;   /* what the sale opened (0.3.5) */
        if (!ic && t->n_fish) {                               /* a population milestone: the newest fish */
            const fish_t *n = &t->fish[t->n_fish - 1];
            render_fish_preview(fb, stride, X + W / 2, Y + 48, n->size * 1.6f, n->color, n->fin, n->accent, n, FISH_ROOM_CARD, t->clock);
        }
    } else {                                                  /* NOTICE_MILESTONE */
        for (int k = 0; k < 6; k++) if (world_view()->fish_badges[k].bit == bit) ic = world_view()->fish_badges[k].icon;
        int bi = 0; while (bi < 31 && !(bit & (1u << bi))) bi++;
        if (f) snprintf(title, sizeof title, "%s", f->name);
        snprintf(caption, sizeof caption, "%s", bi < MS_FISH_COUNT ? MS_NAMES[bi] : "");
        if (!ic && f) render_fish_preview(fb, stride, X + W / 2, Y + 48, f->size * 1.6f, f->color, f->fin, f->accent, f, FISH_ROOM_CARD, t->clock);
    }
    if (ic) blit_icon_scaled(&c, X + (W - ic->w * 2) / 2, Y + 16 + (32 - ic->w), ic, 2, true);
    draw_text(&c, X + (W - text_w(title, 3)) / 2, Y + (caption2 ? 70 : 92), 3, 0xffffff, title);
    draw_text(&c, X + (W - text_w(caption, 2)) / 2, Y + (caption2 ? 104 : 124), 2, MSP_TEAL, caption);
    if (caption2) draw_text(&c, X + (W - text_w(caption2, 2)) / 2, Y + 126, 2, MSP_TEAL, caption2);
    if (frac_left < 0) frac_left = 0;
    if (frac_left > 1) frac_left = 1;
    rect_fill(&c, X + 2, Y + H - 4, (int)((W - 4) * frac_left), 2, 0x1c2f36);
}

/* ---- the shop (2026-09-15): sand dollars, and what they buy ----
 * The milestones page's dress. A 64 px coin and the balance at the top, a
 * row per item (SD_ITEMS), HOW TO EARN bottom left, CLOSE bottom right. A
 * row opens the item's modal (the art at 2x, the words, the price, UNLOCK -
 * dim when the balance is short, IN THE TANK once owned); HOW TO EARN a
 * modal of the sources. The page dims under a modal like the milestones
 * page. Fingers land low here too: the row bands run 8 px above and to the
 * next row, the buttons' bands to the glass edge. */
/* (the page's numbers, SHP_*: render.h) */
static int  g_shp_modal = -1;        /* the item whose modal is up, or -1 */
static bool g_shp_earn;              /* the HOW TO EARN modal is up */
static bool g_shp_sell_armed;        /* SELL tapped once: the next tap on it sells */
static double g_shp_sell_clock;      /* ... when (0.4: a doubled tap within MSP_SELL_WAIT_S is not the yes) */
static double g_shp_open_clock;      /* the tank clock when a row opened its modal: a tap within MSP_SELL_WAIT_S of it is the
                                        same double tap (0.4 rc: a kid's double tap on rows 3-4 opened the modal, then its
                                        second tap landed on BUY, which stands where those rows do) */
static bool shp_fresh(const tank_t *t) { double d = t->clock - g_shp_open_clock; return d >= 0 && d < MSP_SELL_WAIT_S; }
static const icon_t *shop_icon(int item) { return world_view()->shop_icon(item); }
/* pages (2026-09-23, the fourth item): SHP_PER_PAGE rows fit between the
 * coin and the foot buttons once the filler caption went (HOW TO EARN says
 * the same). With more items than a page holds, arrows at the header's
 * right flip through the pages; with one page nothing shows. */
static int g_shp_page;
static void shop_arrow(ctx_t *c, int x, int y, bool right, uint32_t rgb) {   /* a chevron in a button */
    button(c, x, y, SHP_ARROW_W, SHP_ARROW_H, 0x1c2f36, rgb, "", 2);
    int cx = x + SHP_ARROW_W / 2, cy = y + SHP_ARROW_H / 2;
    for (int i = 0; i < 7; i++) {                            /* a chevron: two strokes meeting at the tip */
        int x = right ? cx - 3 + i : cx + 3 - i;
        rect_fill(c, x, cy - 6 + i, 2, 1, rgb);
        rect_fill(c, x, cy + 6 - i, 2, 1, rgb);
    }
}
static void price_tag(ctx_t *c, int x, int y, int price, uint32_t rgb) {   /* the small coin + the number */
    blit_icon(c, x, y - 1, world_view()->art.coin16, 255);   /* (the terrarium's: a desert rose) */
    char n[16]; snprintf(n, sizeof n, "%d", price);
    draw_text(c, x + 20, y, 2, rgb, n);
}
/* the FRY row (0.3.5): the first row once a fish has been sold. Rows are
 * numbered across the pages; row 0 is FRY when it shows, the items after */
#define SHP_MODAL_FRY 99             /* g_shp_modal: the fry's modal */
static int g_shp_fry_pick;           /* the fry picked in its modal */
static bool shp_fry(const tank_t *t) { return progression_fry_for_sale(t); }
/* the world's shop lists its own items (the aquarium's, the terrarium's SD_TERR_FIRST ..), after the FRY row once a fish was sold */
static int  shp_rows(const tank_t *t) { return world_view()->shop_count(t) + shp_fry(t); }
static int  shp_pages(const tank_t *t) { return (shp_rows(t) + SHP_PER_PAGE - 1) / SHP_PER_PAGE; }
static int  shp_item(const tank_t *t, int row) {                                       /* -1 = the FRY row; the world's items after it */
    int i = shp_fry(t) ? row - 1 : row;
    return i < 0 ? -1 : world_view()->shop_item(t, i);
}
static void fry_hours(char *out, size_t n) {   /* "NEW FRY IN 14 H" / "IN 40 MIN" */
    float s = progression_fry_new_in_s();
    if (s >= 3600) snprintf(out, n, world_view()->text.more_in_h_fmt, (int)((s + 1799) / 3600));
    else snprintf(out, n, world_view()->text.more_in_min_fmt, (int)(s / 60) + 1);
}
static void fry_draw(uint16_t *fb, int stride, float x, float y, float size, const fry_offer_t *o, double clock) {
    fish_t f; memset(&f, 0, sizeof f);
    f.stage = STAGE_ADULT; f.color = o->body; f.fin = tank_fin_for(o->body); f.accent = o->accent;   /* (0.4, 11-01, Strato: the adult it grows into -
                                                                                                         its fins and markings show; a big form scaled down to fit) */
    f.shape = o->shape; f.fins = o->fins; f.pattern = o->pattern;
    render_fish_portrait(fb, stride, x, y, size, &f, FISH_ROOM_ROW, clock);
}
static int fry_first_left(void) {
    for (int k = 0; k < FRY_OFFER_N; k++) { fry_offer_t o; progression_fry_offer(k, &o); if (!o.sold) return k; }
    return -1;
}
static void shop_fry_row(ctx_t *c, const tank_t *t, uint16_t *fb, int stride, int top) {
    int k = fry_first_left();
    bool room = progression_fry_room_to_buy(t), can = t->sd_balance >= SD_PRICE_FRY;
    if (k >= 0) { fry_offer_t o; progression_fry_offer(k, &o); fry_draw(fb, stride, 48, top + 16, 0.9f, &o, t->clock); }
    draw_text(c, 76, top + 2, 2, 0xffffff, world_view()->text.newborn);
    if (k < 0) { char l[24]; fry_hours(l, sizeof l); draw_text(c, 76, top + 20, 2, MSP_QUIET, l); }
    else price_tag(c, 76, top + 20, SD_PRICE_FRY, can && room ? MSP_TEAL : MSP_QUIET);
    const char *b = k < 0 ? "SOLD OUT" : !room ? "FULL" : "CHOOSE";
    if (k >= 0 && room && can) { button(c, SHP_BTN_X, top, SHP_BTN_W, SHP_BTN_H, MSP_TEAL, MSP_TEAL, b, 2);
                                 draw_text(c, SHP_BTN_X + (SHP_BTN_W - text_w(b, 2)) / 2, top + (SHP_BTN_H - 14) / 2, 2, MSP_INK, b); }
    else button(c, SHP_BTN_X, top, SHP_BTN_W, SHP_BTN_H, 0x1c2f36, MSP_DIM, b, 2);
}
static void shop_fry_modal(ctx_t *c, const tank_t *t, uint16_t *fb, int stride) {
    const int X = SHP_MODAL_X, Y = SHP_MODAL_Y, W = SHP_MODAL_W, H = SHP_MODAL_H;
    rect_fill(c, X, Y, W, H, 0x04141a);
    rect_edge(c, X, Y, W, H, MSP_TEAL); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    { const char *ti = world_view()->text.newborns_for_sale; draw_text(c, X + (W - text_w(ti, 3)) / 2, Y + 14, 3, 0xffffff, ti); }
    for (int k = 0; k < FRY_OFFER_N; k++) {
        fry_offer_t o; progression_fry_offer(k, &o);
        int bx = SHP_FRY_BOX_X(k), by = SHP_FRY_BOX_Y;
        bool lit = !o.sold && k == g_shp_fry_pick;
        rect_fill(c, bx, by, SHP_FRY_BOX_W, SHP_FRY_BOX_H, lit ? 0x14303a : 0x0b1d23);
        uint32_t e = lit ? 0xffffff : o.sold ? 0x1c2f36 : 0x4d6a72;
        rect_edge(c, bx, by, SHP_FRY_BOX_W, SHP_FRY_BOX_H, e);
        if (lit) rect_edge(c, bx + 1, by + 1, SHP_FRY_BOX_W - 2, SHP_FRY_BOX_H - 2, e);
        if (o.sold) continue;                                    /* bought: a blank until the fresh three */
        fry_draw(fb, stride, bx + SHP_FRY_BOX_W / 2, by + 34, 1.5f, &o, t->clock + k * 0.7f);
        /* its body in words, where the color squares were (the adult shows its colors) - the body alone, Strato 11-01: the fins and
           the pattern were too tiny to read, "and we can leave a bit of mystery there" */
        draw_text(c, bx + (SHP_FRY_BOX_W - text_w(page_body()->offer_word(page_species(), &o), 2)) / 2, by + SHP_FRY_BOX_H - 20, 2, lit ? 0xffffff : 0x9fd8e2, page_body()->offer_word(page_species(), &o));
    }
    bool room = progression_fry_room_to_buy(t), can = t->sd_balance >= SD_PRICE_FRY;
    fry_offer_t pk; progression_fry_offer(g_shp_fry_pick, &pk);

    bool go = room && can && !pk.sold && progression_fry_left() > 0;
    char line[32]; snprintf(line, sizeof line, "%d", SD_PRICE_FRY);
    price_tag(c, X + (W - 20 - text_w(line, 2)) / 2, Y + 138, SD_PRICE_FRY, go ? 0xffffff : MSP_QUIET);
    if (!room) snprintf(line, sizeof line, "%s", progression_fish_held(t) >= progression_pop_cap(t) ? world_view()->text.tank_full
                                                 : world_view()->text.welcome_newborn);   /* (a place held for a gift or a fry next door: full) */
    else if (!can) snprintf(line, sizeof line, "YOU HAVE %d", (int)t->sd_balance);
    else line[0] = 0;
    if (line[0]) draw_text(c, X + (W - text_w(line, 2)) / 2, Y + 160, 2, MSP_QUIET, line);
    fry_hours(line, sizeof line);
    draw_text(c, X + (W - text_w(line, 2)) / 2, Y + 180, 2, MSP_QUIET, line);
    const int bx = X + (W - MSP_HOW_W) / 2, by = Y + H - 12 - MSP_HOW_H;
    if (go) { button(c, bx, by, MSP_HOW_W, MSP_HOW_H, MSP_TEAL, MSP_TEAL, "BUY", 2);
              draw_text(c, bx + (MSP_HOW_W - text_w("BUY", 2)) / 2, by + (MSP_HOW_H - 14) / 2, 2, MSP_INK, "BUY"); }
    else button(c, bx, by, MSP_HOW_W, MSP_HOW_H, 0x1c2f36, MSP_DIM, "BUY", 2);
}
/* the shelf's words (0.3.9): the line under IN THE TANK / ON THE SHELF in a placeable piece's modal - the world's note on it (the
 * aquarium's pumpkin and living pieces), else why a piece cannot be moved. NULL = nothing to say. */
static const char *shelf_line(const tank_t *t, int item, char *out, size_t n) {
    bool shelved = (t->sd_shelved & progression_item(item)->bit) != 0;
    const world_view_t *w = world_view();
    const char *note = w->shelf_note ? w->shelf_note(t, item, shelved, out, n) : NULL;
    if (note) return note;
    if (!shelved) return tank_decor_locked(t, item) ? "LOCKED IN PLACE" : NULL;   /* (0.3.9: why a hold on it opens nothing; MOVE is the way to its page) */
    return NULL;
}
static void shelf_slot(ctx_t *c, int x, int y, const tank_t *t, int item, int s) {   /* the icon slot: s = 1 on the row (32 px), 2 in the modal (64, as the icons) */
    const int W = SHELF_THUMB * s, H = SHELF_THUMB * s;
    const world_view_t *w = world_view();
    if (w->shelf_thumb && w->shelf_thumb(c, x, y, t, item, s)) return;   /* a living piece: the little tank, the thumb, the glass, a glint */
    rect_fill(c, x, y + H - 3 * s, W, 3 * s, 0x5a3c22);                /* the shelf board under a piece that does not live */
    rect_fill(c, x, y + H - 3 * s, W, 1, 0x8a6a42);
    if (s == 1) blit_icon(c, x, y, shop_icon(item), 255); else blit_icon_scaled(c, x, y, shop_icon(item), s, true);
}
void render_shop(const tank_t *t, uint16_t *fb, int stride) {
    if (world_view()->shelf_prepare) world_view()->shelf_prepare(t, fb, stride);   /* (the shelf's thumbs first: the frame is their scratch) */
    ctx_t c = ctx_page(fb, stride);
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    blit_icon(&c, SHP_COIN_X, SHP_COIN_Y, world_view()->art.coin64, 255);
    draw_text(&c, SHP_HEAD_X, SHP_COIN_Y + 6, 2, MSP_TEAL, world_view()->text.coins);
    char bal[16]; snprintf(bal, sizeof bal, "%d", (int)t->sd_balance);
    draw_text(&c, SHP_HEAD_X, SHP_COIN_Y + 28, 4, 0xffffff, bal);
    for (int x = 24; x < PAGE_W - 24; x++) px_blend(&c, x, SHP_ROW_Y0 - 10, MSP_DIM, 200);
    if (shp_pages(t) > 1) {
        shop_arrow(&c, SHP_ARROW_X0, SHP_ARROW_Y, false, g_shp_page > 0 ? MSP_TEAL : MSP_DIM);
        shop_arrow(&c, SHP_ARROW_X1, SHP_ARROW_Y, true, g_shp_page < shp_pages(t) - 1 ? MSP_TEAL : MSP_DIM);
    }
    for (int r = g_shp_page * SHP_PER_PAGE; r < shp_rows(t) && r < (g_shp_page + 1) * SHP_PER_PAGE; r++) {
        int top = SHP_ROW_Y0 + (r - g_shp_page * SHP_PER_PAGE) * SHP_ROW_DY, i = shp_item(t, r);
        if (i < 0) { shop_fry_row(&c, t, fb, stride, top); continue; }
        const sd_item_t *it = progression_item(i);
        bool owned = (t->sd_unlocks & it->bit) != 0, can = t->sd_balance >= it->price, shelved = owned && (t->sd_shelved & it->bit);
        if (shelved) shelf_slot(&c, 32, top, t, i, 1);                                                             /* the shelf (0.3.9): the piece as it is, in its little tank */
        else if (owned) blit_icon(&c, 32, top, shop_icon(i), 255); else blit_icon_locked(&c, 32, top, shop_icon(i));   /* (the rows are where the bowl is wide: their own column) */
        draw_text(&c, 76, top + 2, 2, 0xffffff, it->name);
        if (shelved) draw_text(&c, 76, top + 20, 2, MSP_TEAL, "ON THE SHELF");
        else if (owned) draw_text(&c, 76, top + 20, 2, MSP_TEAL, world_view()->text.in_the_tank);
        else price_tag(&c, 76, top + 20, it->price, can ? MSP_TEAL : MSP_QUIET);
        if (shelved) { button(&c, SHP_BTN_X, top, SHP_BTN_W, SHP_BTN_H, MSP_TEAL, MSP_TEAL, "PUT BACK", 2);          /* live: one tap returns it */
                       draw_text(&c, SHP_BTN_X + (SHP_BTN_W - text_w("PUT BACK", 2)) / 2, top + (SHP_BTN_H - 14) / 2, 2, MSP_INK, "PUT BACK"); }
        else if (owned) button(&c, SHP_BTN_X, top, SHP_BTN_W, SHP_BTN_H, MSP_INK, MSP_DIM, world_view()->text.in_tank_short, 2);
        else if (can) { button(&c, SHP_BTN_X, top, SHP_BTN_W, SHP_BTN_H, MSP_TEAL, MSP_TEAL, "UNLOCK", 2);
                        draw_text(&c, SHP_BTN_X + (SHP_BTN_W - text_w("UNLOCK", 2)) / 2, top + (SHP_BTN_H - 14) / 2, 2, MSP_INK, "UNLOCK"); }
        else           button(&c, SHP_BTN_X, top, SHP_BTN_W, SHP_BTN_H, 0x1c2f36, MSP_DIM, "UNLOCK", 2);
    }
    button(&c, SHP_EARN_X, MSP_CLOSE_Y, SHP_EARN_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "HOW TO EARN", 2);
    button(&c, SHP_CLOSE_X, MSP_CLOSE_Y, MSP_CLOSE_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "CLOSE", 2);
    if (g_shp_modal < 0 && !g_shp_earn) return;
    for (int y = 0; y < TANK_H; y++)                          /* the page out of reach under a modal */
        for (int x = 0; x < TANK_W; x++) { uint16_t n = FB_SWAP16(fb[y * stride + x]); fb[y * stride + x] = FB_SWAP16((uint16_t)((n >> 1) & 0x7bef)); }
    const int X = SHP_MODAL_X, W = SHP_MODAL_W;
    if (g_shp_earn) {
        const int Y = SHP_EARN_MODAL_Y, H = SHP_EARN_MODAL_H;
        rect_fill(&c, X, Y, W, H, 0x04141a);
        rect_edge(&c, X, Y, W, H, MSP_TEAL); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
        draw_text(&c, X + (W - text_w("HOW TO EARN", 3)) / 2, Y + 14, 3, 0xffffff, "HOW TO EARN");
        const char *const *lines = progression_sd_earn_lines();
        for (int i = 0; lines[i]; i++) draw_text(&c, X + 14, Y + 50 + i * 24, 2, MSP_TEAL, lines[i]);
        draw_text(&c, X + (W - text_w("TAP TO CLOSE", 2)) / 2, Y + H - 24, 2, MSP_QUIET, "TAP TO CLOSE");
        return;
    }
    if (g_shp_modal == SHP_MODAL_FRY) { shop_fry_modal(&c, t, fb, stride); return; }
    const sd_item_t *it = progression_item(g_shp_modal);
    const int Y = SHP_MODAL_Y, H = SHP_MODAL_H;
    bool owned = (t->sd_unlocks & it->bit) != 0, can = t->sd_balance >= it->price;
    rect_fill(&c, X, Y, W, H, 0x04141a);
    rect_edge(&c, X, Y, W, H, MSP_TEAL); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    blit_icon_scaled(&c, X + (W - 64) / 2, Y + 14, shop_icon(g_shp_modal), 2, true);
    draw_text(&c, X + (W - text_w(it->name, 3)) / 2, Y + 88, 3, 0xffffff, it->name);
    draw_text(&c, X + (W - text_w(it->words, 2)) / 2, Y + 118, 2, MSP_TEAL, it->words);
    draw_text(&c, X + (W - text_w(it->words2, 2)) / 2, Y + 138, 2, MSP_TEAL, it->words2);
    const int bx = X + (W - MSP_HOW_W) / 2, by = Y + H - 12 - MSP_HOW_H;
    if (owned && tank_decor_placeable(g_shp_modal)) {         /* a placeable piece (0.3.9): in the tank MOVE | SHELVE; on the shelf PLACE | SELL (twice) */
        bool shelved = (t->sd_shelved & it->bit) != 0; char sub[28];
        if (shelved) shelf_slot(&c, X + (W - 64) / 2, Y + 14, t, g_shp_modal, 2);   /* over the icon: the piece as it is on the shelf */
        const char *state = shelved ? "ON THE SHELF" : world_view()->text.in_the_tank;
        draw_text(&c, X + (W - text_w(state, 2)) / 2, Y + 164, 2, MSP_TEAL, state);
        if (shelf_line(t, g_shp_modal, sub, sizeof sub)) draw_text(&c, X + (W - text_w(sub, 2)) / 2, Y + 184, 2, MSP_QUIET, sub);
        button(&c, SHP_TWO_X0, by, MSP_HOW_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, shelved ? "PLACE" : "MOVE", 2);
        if (!shelved) button(&c, SHP_TWO_X1, by, MSP_HOW_W, MSP_HOW_H, 0x1c2f36, MSP_TEAL, "SHELVE", 2);
        else if (g_shp_sell_armed) { snprintf(sub, sizeof sub, "+%d OK?", progression_sell_value(g_shp_modal));
                                     button(&c, SHP_TWO_X1, by, MSP_HOW_W, MSP_HOW_H, MSP_TEAL, MSP_TEAL, sub, 2);
                                     draw_text(&c, SHP_TWO_X1 + (MSP_HOW_W - text_w(sub, 2)) / 2, by + (MSP_HOW_H - 14) / 2, 2, MSP_INK, sub); }
        else button(&c, SHP_TWO_X1, by, MSP_HOW_W, MSP_HOW_H, 0x1c2f36, MSP_DIM, "SELL", 2);
    } else if (owned) { const char *in = world_view()->text.in_the_tank; draw_text(&c, X + (W - text_w(in, 2)) / 2, by + 8, 2, MSP_TEAL, in); }   /* the snail: a permanent resident */
    else {
        char line[32]; snprintf(line, sizeof line, "%d", it->price);
        int pw = 20 + text_w(line, 2);
        price_tag(&c, X + (W - pw) / 2, Y + 164, it->price, can ? 0xffffff : MSP_QUIET);
        if (!can) { snprintf(line, sizeof line, "YOU HAVE %d", (int)t->sd_balance);
                    draw_text(&c, X + (W - text_w(line, 2)) / 2, Y + 184, 2, MSP_QUIET, line); }
        if (can) { button(&c, bx, by, MSP_HOW_W, MSP_HOW_H, MSP_TEAL, MSP_TEAL, "UNLOCK", 2);
                   draw_text(&c, bx + (MSP_HOW_W - text_w("UNLOCK", 2)) / 2, by + (MSP_HOW_H - 14) / 2, 2, MSP_INK, "UNLOCK"); }
        else button(&c, bx, by, MSP_HOW_W, MSP_HOW_H, 0x1c2f36, MSP_DIM, "UNLOCK", 2);
    }
}
int render_shop_tap(const tank_t *t, float x, float y) {
    x -= PAGE_X; y -= PAGE_Y;                    /* the page's own coordinates */
    if (g_shp_earn) { g_shp_earn = false; return SHOP_TAP_KEPT; }
    if (g_shp_modal >= 0 && shp_fresh(t)) { g_shp_open_clock = t->clock; return SHOP_TAP_KEPT; }   /* the row's tap doubled: spent, the modal stays (BUY,
                                                                   MOVE, SHELVE, a close) - and the wait starts again: a mash never reaches BUY (0.4 rc round three) */
    if (g_shp_modal == SHP_MODAL_FRY) {                       /* the fry: a box picks, BUY buys the pick, elsewhere closes */
        if (y >= SHP_FRY_BOX_Y - 8 && y < SHP_FRY_BOX_Y + SHP_FRY_BOX_H + 6)
            for (int k = 0; k < FRY_OFFER_N; k++)
                if (x >= SHP_FRY_BOX_X(k) - SHP_FRY_BOX_GAP / 2 && x < SHP_FRY_BOX_X(k) + SHP_FRY_BOX_W + SHP_FRY_BOX_GAP / 2) {
                    fry_offer_t o; progression_fry_offer(k, &o);
                    if (!o.sold) g_shp_fry_pick = k;
                    return SHOP_TAP_KEPT;
                }
        const int bx = SHP_MODAL_X + (SHP_MODAL_W - MSP_HOW_W) / 2, by = SHP_MODAL_Y + SHP_MODAL_H - 12 - MSP_HOW_H;
        bool on_btn = y >= by - MSP_HOW_SLOP_UP && y < by + MSP_HOW_H + MSP_HOW_SLOP_DN && x >= bx - MSP_HOW_SLOP_X && x < bx + MSP_HOW_W + MSP_HOW_SLOP_X;
        fry_offer_t pk; progression_fry_offer(g_shp_fry_pick, &pk);
        g_shp_modal = -1;
        if (on_btn && !pk.sold && progression_fry_room_to_buy(t) && t->sd_balance >= SD_PRICE_FRY) return SHOP_TAP_FRY + g_shp_fry_pick;
        return SHOP_TAP_KEPT;
    }
    if (g_shp_modal >= 0) {
        int item = g_shp_modal; const sd_item_t *it = progression_item(item);
        bool owned = (t->sd_unlocks & it->bit) != 0, can = t->sd_balance >= it->price;
        const int bx = SHP_MODAL_X + (SHP_MODAL_W - MSP_HOW_W) / 2, by = SHP_MODAL_Y + SHP_MODAL_H - 12 - MSP_HOW_H;
        bool row = y >= by - MSP_HOW_SLOP_UP && y < by + MSP_HOW_H + MSP_HOW_SLOP_DN;
        bool on_btn = row && x >= bx - MSP_HOW_SLOP_X && x < bx + MSP_HOW_W + MSP_HOW_SLOP_X;
        if (owned && tank_decor_placeable(item)) {                /* two buttons: in the tank MOVE | SHELVE; on the shelf PLACE | SELL (armed, then confirmed) */
            bool shelved = (t->sd_shelved & it->bit) != 0;
            bool on_left  = row && x >= SHP_TWO_X0 - MSP_HOW_SLOP_X && x < SHP_TWO_X0 + MSP_HOW_W + SHP_TWO_GAP / 2;
            bool on_right = row && x >= SHP_TWO_X1 - SHP_TWO_GAP / 2 && x < SHP_TWO_X1 + MSP_HOW_W + MSP_HOW_SLOP_X;
            if (shelved && on_right && !g_shp_sell_armed) { g_shp_sell_armed = true; g_shp_sell_clock = t->clock; return SHOP_TAP_KEPT; }
            if (shelved && on_right && t->clock - g_shp_sell_clock < MSP_SELL_WAIT_S) { g_shp_sell_clock = t->clock; return SHOP_TAP_KEPT; }   /* a doubled tap
                                                                   (or a mash): still armed, the wait again from it */
            g_shp_sell_armed = false; g_shp_modal = -1;
            if (on_right) return (shelved ? SHOP_TAP_SELL : SHOP_TAP_SHELVE) + item;
            if (on_left)  return (shelved ? SHOP_TAP_PLACE : SHOP_TAP_MOVE) + item;
            return SHOP_TAP_KEPT;
        }
        g_shp_modal = -1; g_shp_sell_armed = false;
        if (on_btn && !owned && can) return SHOP_TAP_BUY + item;
        return SHOP_TAP_KEPT;
    }
    if (x >= SHP_CLOSE_X - 6 && y >= MSP_CLOSE_Y - 4) return SHOP_TAP_CLOSE;
    if (x < SHP_EARN_X + SHP_EARN_W + 8 && y >= MSP_CLOSE_Y - 4) { g_shp_earn = true; return SHOP_TAP_KEPT; }
    if (shp_pages(t) > 1 && y < SHP_ROW_Y0 - 10 && x >= SHP_ARROW_X0 - 8) {   /* the page arrows, in the header band */
        if (x < SHP_ARROW_X1 - 4) { if (g_shp_page > 0) g_shp_page--; }
        else if (g_shp_page < shp_pages(t) - 1) g_shp_page++;
        return SHOP_TAP_KEPT;
    }
    for (int r = g_shp_page * SHP_PER_PAGE; r < shp_rows(t) && r < (g_shp_page + 1) * SHP_PER_PAGE; r++) {
        int top = SHP_ROW_Y0 + (r - g_shp_page * SHP_PER_PAGE) * SHP_ROW_DY, i = shp_item(t, r);
        if (x >= 20 && y >= top - 8 && y < top + SHP_ROW_DY - 8) {
            if (i >= 0 && (t->sd_shelved & progression_item(i)->bit) && x >= SHP_BTN_X - 8) return SHOP_TAP_PLACE + i;   /* the row's PUT BACK (0.3.9) */
            if (i < 0) { int k = fry_first_left(); g_shp_fry_pick = k < 0 ? 0 : k; g_shp_modal = SHP_MODAL_FRY; }   /* the fry's modal, the first one left picked */
            else g_shp_modal = i;
            g_shp_open_clock = t->clock;
            return SHOP_TAP_KEPT;
        }
    }
    return SHOP_TAP_NONE;
}
void render_shop_leave(void) { g_shp_modal = -1; g_shp_earn = false; g_shp_page = 0; g_shp_sell_armed = false; if (world_view()->shelf_leave) world_view()->shelf_leave(); }

/* the toast: "+N" by a coin, top centre, for TOAST_S on the tank clock */
#define TOAST_S 2.5f
static int   g_toast_n;
static double g_toast_until = -1;
void render_sd_toast(const tank_t *t, uint16_t *fb, int stride) {
    int n = progression_sd_take_award();
    if (n > 0) {
        if (t->clock < g_toast_until) g_toast_n += n; else g_toast_n = n;
        g_toast_until = t->clock + TOAST_S;
    }
    if (t->clock >= g_toast_until || t->clock < g_toast_until - TOAST_S - 1) return;   /* (a reset sends the clock back) */
    ctx_t c = ctx_full(fb, stride, 1.0f);
    char txt[16]; snprintf(txt, sizeof txt, "+%d", g_toast_n);
    const int W = 30 + text_w(txt, 2), H = 22, X = (TANK_W - W) / 2, Y = PAGE_BOWL ? 44 : 8;   /* (the bowl's top is a narrow cap: lower) */
    for (int y = Y; y < Y + H; y++)
        for (int x = X; x < X + W; x++) px_blend(&c, x, y, 0x04141a, 215);
    rect_edge(&c, X, Y, W, H, MSP_TEAL);
    blit_icon(&c, X + 5, Y + 3, world_view()->art.coin16, 255);
    draw_text(&c, X + 25, Y + 4, 2, 0xffffff, txt);
}

/* ---- settings page (2026-09-15) ----
 * The milestones page's foot used to carry the brightness row; Strato:
 * "a new UI for settings and leave milestones alone - we may need more
 * room there anyway". Rows of segment buttons, generous hit bands (fingers
 * land low near the bezel, as on the milestones page). 0.3.2: LIGHTS OUT is
 * one row (a value between two arrows, where MANUAL / AUTO and a big seconds
 * selector stood), and AUTO FEED and ROTATION have the room it gave back.
 * 0.3.9: BRIGHTNESS is a slider (30 / 60 / 100 % was too coarse at the dim
 * end - Bananna on Discord, light sensitive: "even the 30% brightness can be
 * a bit bright"). */
/* (the page's numbers, SET_*: render.h) */
static const char *const SET_LIGHT[LIGHT_IDLE_N + 1] = { "DOUBLE-TAP",   /* MANUAL, the default: the row says how the light is worked */
    "5 SEC", "15 SEC", "30 SEC", "1 MIN", "3 MIN", "5 MIN", "10 MIN", "30 MIN" };
static const char *const SET_FEED[2]   = { "ON", "OFF" };          /* the default first */
#if TANK_WORN
static const char *const SET_SCREEN[2] = { "NORMAL", "TURNED" };   /* the default first */
#endif

#if PT_NET
static void link_glyph(ctx_t *c, int x, int y, uint32_t rgb);   /* the link mode section, at the end */
#endif
static void set_row(ctx_t *c, int row_y, const char *label, const char *const names[], int n, int chosen) {
    draw_text(c, SET_LABEL_X, row_y, 2, MSP_TEAL, label);
    for (int i = 0; i < n; i++) {
        int x = SET_SEG_X + i * SET_SEG_DX, y = SET_SEG_Y(row_y);
        if (i == chosen) {                       /* lit: teal, ink lettering */
            button(c, x, y, SET_SEG_W, SET_SEG_H, MSP_TEAL, MSP_TEAL, names[i], 2);
            draw_text(c, x + (SET_SEG_W - text_w(names[i], 2)) / 2, y + (SET_SEG_H - 14) / 2, 2, MSP_INK, names[i]);
        } else button(c, x, y, SET_SEG_W, SET_SEG_H, 0x1c2f36, MSP_DIM, names[i], 2);
    }
}
/* the percent a finger at page x asks of a slider running lo .. 100: the nearest step (the rail is 0 .. 100 for all) */
static int set_slider_at(float x, int lo, int step) {
    float f = (x - SET_BRT_X0) / (float)(SET_BRT_X1 - SET_BRT_X0);
    int pct = (int)lroundf(f * 100 / step) * step;
    return pct < lo ? lo : pct > 100 ? 100 : pct;
}
static int set_bright_at(float x) { return set_slider_at(x, SET_BRT_MIN, SET_BRT_STEP); }
static int set_volume_at(float x) { return set_slider_at(x, 0, SET_VOL_STEP); }
/* the slider: the rail lit (teal) up to the knob and dim past it - the lit
 * part is the brighter one, so it reads without the hue - and the knob, a
 * lit button with the percent on it in ink. VOLUME's knob at 0 says OFF and
 * goes out (an outline, teal letters): off by fill and word, not by hue. */
static void set_slider(ctx_t *c, int row_y, const char *label, int lo, int pct) {
    int y = SET_SEG_Y(row_y), cy = y + SET_SEG_H / 2;
    pct = pct < lo ? lo : pct > 100 ? 100 : pct;
    int kx = SET_BRT_X(pct);                                /* (one scale: equal percents line up) */
    draw_text(c, SET_LABEL_X, row_y, 2, MSP_TEAL, label);
    rect_fill(c, SET_SEG_X + 4, cy - 3, kx - SET_SEG_X - 4, 6, MSP_TEAL);
    rect_fill(c, kx, cy - 3, SET_SEG_X + SET_SPAN_W - 4 - kx, 6, 0x1c2f36);
    rect_edge(c, kx, cy - 3, SET_SEG_X + SET_SPAN_W - 4 - kx, 6, MSP_DIM);
    char txt[8]; snprintf(txt, sizeof txt, pct ? "%d%%" : "OFF", pct);
    bool off = pct == 0;
    button(c, kx - SET_KNOB_W / 2, y, SET_KNOB_W, SET_SEG_H, off ? 0x1c2f36 : MSP_TEAL, MSP_TEAL, "", 2);
    draw_text(c, kx - text_w(txt, 2) / 2, y + (SET_SEG_H - 14) / 2, 2, off ? MSP_TEAL : MSP_INK, txt);
}
/* an arrow button of the LIGHTS OUT row: dim at the end of the list */
static void set_arrow(ctx_t *c, int x, int y, bool right, bool live) {
    uint32_t rgb = live ? MSP_TEAL : 0x2c4a52;
    button(c, x, y, SET_ARW_W, SET_SEG_H, 0x1c2f36, rgb, "", 2);
    int cx = x + SET_ARW_W / 2, cy = y + SET_SEG_H / 2;
    for (int i = 0; i < 7; i++) {                           /* a solid triangle, 7 wide, 13 tall */
        int px = right ? cx - 3 + i : cx + 3 - i, hh = 6 - i;
        rect_fill(c, px, cy - hh, 1, 2 * hh + 1, rgb);
    }
}
#if !TANK_WORN                                              /* (the watch has SCREEN where ROTATION is) */
/* ROTATION's picture: a padlock inside a turning arrow - shut when the way
 * up is locked, its shackle swung open while the picture follows the tank
 * (the two differ in shape, not in color alone) */
static void set_lock_icon(ctx_t *c, int cx, int cy, bool locked, uint32_t rgb) {
    for (int dy = -15; dy <= 15; dy++)                      /* the ring, open at the top right */
        for (int dx = -15; dx <= 15; dx++) {
            float r2 = (float)(dx * dx + dy * dy);
            if (r2 < 11.5f * 11.5f || r2 > 14.0f * 14.0f) continue;
            if (dx > 1 && dy < 0 && dy < -dx * 0.45f) continue;
            px_blend(c, cx + dx, cy + dy, rgb, 255);
        }
    for (int i = 0; i < 6; i++)                             /* its arrowhead, at the top, pointing clockwise */
        rect_fill(c, cx + 1 + i, cy - 13 - (5 - i), 1, 2 * (5 - i) + 1, rgb);
    rect_fill(c, cx - 6, cy - 1, 12, 8, rgb);              /* the padlock's body */
    int top = locked ? cy - 7 : cy - 10;                    /* the shackle: down in the body, or lifted with one leg free */
    rect_fill(c, cx - 4, top, 8, 2, rgb);
    rect_fill(c, cx - 4, top, 2, cy - 1 - top, rgb);
    rect_fill(c, cx + 2, top, 2, locked ? cy - 1 - top : 4, rgb);
}
#endif
void render_settings(const tank_t *t, uint16_t *fb, int stride, int bright_pct, int volume) {
    ctx_t c = ctx_page(fb, stride);
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    draw_text(&c, (PAGE_W - text_w("SETTINGS", 3)) / 2, SET_TITLE_Y, 3, 0xffffff, "SETTINGS");
    set_slider(&c, SET_ROW1_Y, "BRIGHTNESS", SET_BRT_MIN, bright_pct);
    set_slider(&c, SET_ROW2_Y, "VOLUME", 0, volume);
    /* LIGHTS OUT: < the choice > */
    {
        int ch = tank_light_choice(t), y = SET_SEG_Y(SET_ROW3_Y);
        int bx = SET_SEG_X + SET_ARW_W + 4, bw = SET_SPAN_W - 2 * (SET_ARW_W + 4);
        draw_text(&c, SET_LABEL_X, SET_ROW3_Y, 2, MSP_TEAL, "LIGHTS OUT");
        set_arrow(&c, SET_SEG_X, y, false, ch > 0);
        button(&c, bx, y, bw, SET_SEG_H, MSP_TEAL, MSP_TEAL, "", 2);
        draw_text(&c, bx + (bw - text_w(SET_LIGHT[ch], 2)) / 2, y + (SET_SEG_H - 14) / 2, 2, MSP_INK, SET_LIGHT[ch]);
        set_arrow(&c, SET_SEG_X + SET_SPAN_W - SET_ARW_W, y, true, ch < LIGHT_IDLE_N);
    }
    set_row(&c, SET_ROW4_Y, "AUTO FEED", SET_FEED, 2, t->autofeed_off ? 1 : 0);
#if TANK_WORN
    /* SCREEN (2026-10-02): the way up of a watch worn either way round - TURNED
       for buttons toward the elbow. The picture turns as the finger lifts. */
    set_row(&c, SET_ROW5_Y, "SCREEN", SET_SCREEN, 2, t->screen_turned ? 1 : 0);
    draw_text(&c, SET_LABEL_X, SET_NOTE5_Y, 2, MSP_QUIET, "WORN THE OTHER WAY AROUND?");
#else
    /* ROTATION (0.3.2): the picture turns over with the tank, unless locked */
    {
        int x = SET_SEG_X, y = SET_SEG_Y(SET_ROW5_Y);
        draw_text(&c, SET_LABEL_X, SET_ROW5_Y, 2, MSP_TEAL, "ROTATION");
        if (t->orient_lock) button(&c, x, y, SET_SEG_W, SET_SEG_H, MSP_TEAL, MSP_TEAL, "", 2);
        else                button(&c, x, y, SET_SEG_W, SET_SEG_H, 0x1c2f36, MSP_DIM, "", 2);
        set_lock_icon(&c, x + SET_SEG_W / 2, y + SET_SEG_H / 2 + 1, t->orient_lock, t->orient_lock ? MSP_INK : MSP_TEAL);
        draw_text(&c, SET_ROT_WORD_X, SET_ROW5_Y, 2, t->orient_lock ? 0xffffff : MSP_QUIET, t->orient_lock ? "LOCKED" : "UNLOCKED");
    }
#endif
    /* the firmware version, hugging the bottom left of the frame (6 px up,
       on the labels' x; the bezel's curve is clear there), small (8 px) and
       dim: it is for the keeper who asks "how do I update?", not for
       reading - Strato: "FISH ARE QUIET AT NIGHT is meant to be read; the
       fw version is something most people should not care about. minimize
       it", "make it 8px tall", "hug the bottom of the frame" (2026-09-16).
       The installer page shows the version it would write in the same words. */
    /* 2026-09-29: the release number first ("V0.2.0 ALPHA"), the build id after it */
    char ver[64]; snprintf(ver, sizeof ver, "V%s %s  BUILD %s", PT_RELEASE, PT_RELEASE_STAGE, version_port_string());
#if TANK_WORN                                    /* the watch: centred under the foot, between the lower corners */
    draw_text_8px(&c, (PAGE_W - ((int)strlen(ver) * 6 - 1)) / 2, SET_FOOT_Y + MSP_CLOSE_H + 2, MSP_DIM, ver);
#else
    draw_text_8px(&c, SET_LABEL_X + (PAGE_BOWL ? 96 : 0), PAGE_H - 8 - 6, MSP_DIM, ver);
#endif
    button(&c, SET_CLOSE_X, SET_FOOT_Y, MSP_CLOSE_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "CLOSE", 2);
#if PT_NET
    /* UPDATES (2026-09-30, docs/OTA.md): bottom left, the same size as CLOSE */
    button(&c, SET_UPD_X, SET_FOOT_Y, SET_UPD_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "UPDATES", 2);
    /* LINK (0.4, the tank link): the glyph - two tanks, a door between - and the word */
    button(&c, SET_LINK_X, SET_LINK_Y, SET_LINK_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "", 2);
    { int gw = 20 + 8 + text_w("LINK", 2), gx = SET_LINK_X + (SET_LINK_W - gw) / 2;
      link_glyph(&c, gx, SET_LINK_Y + (MSP_CLOSE_H - 12) / 2, 0xffffff);
      draw_text(&c, gx + 28, SET_LINK_Y + (MSP_CLOSE_H - 14) / 2, 2, 0xffffff, "LINK"); }
#endif
}
static int set_segment(float x, int n) {
    if (x < SET_SEG_X - 10) return -1;
    int i = (int)((x - SET_SEG_X + 3) / SET_SEG_DX);
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}
/* the hit test: what a TAP at (x,y) means. *value: BRIGHT the percent the
 * slider's step nearest x,
 * VOLUME the percent its step nearest x (0 = OFF), FEED 1 = ON, SCREEN 1 = TURNED; ROTATE carries none (a
 * toggle); the LIGHTS OUT row's own hits are LIGHT_PREV / LIGHT_NEXT (its
 * left and right halves). */
enum { SET_HIT_LIGHT_PREV = 100, SET_HIT_LIGHT_NEXT };
int render_settings_tap(float x, float y, int *value) {
    x -= PAGE_X; y -= PAGE_Y;                    /* the page's own coordinates */
    if (x >= SET_CLOSE_X - 8 && y >= SET_FOOT_Y - 4) return SET_TAP_CLOSE;
    /* without the network (PT_NET) UPDATES and LINK are not drawn: their boxes stay dead, never a row's band */
    if (x < SET_UPD_X + SET_UPD_W + 8 && y >= SET_FOOT_Y - 4) return PT_NET ? SET_TAP_UPDATES : SET_TAP_NONE;
    if (x >= SET_LINK_X - 8 && x < SET_LINK_X + SET_LINK_W + 8 && y >= SET_LINK_Y - 6 && y < SET_LINK_Y + MSP_CLOSE_H + 10) return PT_NET ? SET_TAP_LINK : SET_TAP_NONE;
    /* the row bands: from a little above each segment down to the next row
       (fingers report low); the last one stops at the foot's */
    int seg = set_segment(x, 3), two = set_segment(x, 2);
    if (y >= SET_SEG_Y(SET_ROW1_Y) - 12 && y < SET_SEG_Y(SET_ROW2_Y) - 12) { if (seg < 0) return SET_TAP_NONE; *value = set_bright_at(x); return SET_TAP_BRIGHT; }
    if (y >= SET_SEG_Y(SET_ROW2_Y) - 12 && y < SET_SEG_Y(SET_ROW3_Y) - 12) { if (seg < 0) return SET_TAP_NONE; *value = set_volume_at(x); return SET_TAP_VOLUME; }
    if (y >= SET_SEG_Y(SET_ROW3_Y) - 12 && y < SET_SEG_Y(SET_ROW4_Y) - 12) { if (seg < 0) return SET_TAP_NONE; *value = 0; return x < SET_SPAN_MID ? SET_HIT_LIGHT_PREV : SET_HIT_LIGHT_NEXT; }
    if (y >= SET_SEG_Y(SET_ROW4_Y) - 12 && y < SET_SEG_Y(SET_ROW5_Y) - 12) { if (two < 0) return SET_TAP_NONE; *value = two == 0; return SET_TAP_FEED; }
    if (y >= SET_SEG_Y(SET_ROW5_Y) - 12 && y < SET_FOOT_Y - 4) {
        if (two < 0) return SET_TAP_NONE;
#if TANK_WORN
        *value = two == 1; return SET_TAP_SCREEN;
#else
        *value = 0; return SET_TAP_ROTATE;       /* the button or its word: one toggle */
#endif
    }
    return SET_TAP_NONE;
}
static bool s_set_down; static float s_set_px, s_set_py; static int s_set_hit, s_set_hv;   /* render_settings_touch's press in hand */
void render_settings_cancel(void) { s_set_down = false; s_set_hit = SET_TAP_NONE; }
int render_settings_touch(tank_t *t, float x, float y, bool down, int *value) {
    int r = SET_TAP_NONE; *value = 0;
    if (down && !s_set_down) {                                  /* press */
        s_set_px = x; s_set_py = y;
        s_set_hit = render_settings_tap(x, y, &s_set_hv);
        if (s_set_hit == SET_TAP_BRIGHT || s_set_hit == SET_TAP_VOLUME) {   /* a slider: the knob jumps to the finger */
            r = s_set_hit == SET_TAP_BRIGHT ? SET_TAP_BRIGHT_LIVE : SET_TAP_VOLUME_LIVE; *value = s_set_hv;
        }
    } else if (down && (s_set_hit == SET_TAP_BRIGHT || s_set_hit == SET_TAP_VOLUME)) {   /* a slider under a finger: the knob follows it along the row, */
        bool brt = s_set_hit == SET_TAP_BRIGHT;
        int v = brt ? set_bright_at(x - PAGE_X) : set_volume_at(x - PAGE_X);   /* however far up or down the finger wanders */
        if (v != s_set_hv) { s_set_hv = v; r = brt ? SET_TAP_BRIGHT_LIVE : SET_TAP_VOLUME_LIVE; *value = v; }
    } else if (!down && s_set_down && (s_set_hit == SET_TAP_BRIGHT || s_set_hit == SET_TAP_VOLUME)) {   /* let go: the last step it showed is the setting */
        r = s_set_hit; *value = s_set_hv;
    } else if (!down && s_set_down) {                           /* release: a tap, if it stayed on what it pressed */
        int v = 0, h = render_settings_tap(x, y, &v);
        float dx = x - s_set_px, dy = y - s_set_py;
        if (h == s_set_hit && dx * dx + dy * dy < 24 * 24) {
            if (h == SET_HIT_LIGHT_PREV || h == SET_HIT_LIGHT_NEXT) {   /* one choice along; either way the light comes on */
                int was = tank_light_choice(t), now = was + (h == SET_HIT_LIGHT_NEXT ? 1 : -1);
                if (now >= 0 && now <= LIGHT_IDLE_N) {
                    tank_light_choice_set(t, now); tank_emit(TEV_WHEEL_TICK, -1); progression_settings_changed();
                    if ((was == 0) != (now == 0)) { r = SET_TAP_LIGHT; *value = now != 0; }
                    else { r = SET_TAP_IDLE; *value = t->light_idle_s; }
                }
            } else if (h == SET_TAP_FEED) {                    /* ON (1, the default) / OFF */
                if ((v == 0) != t->autofeed_off) { t->autofeed_off = v == 0; progression_settings_changed(); }
                r = SET_TAP_FEED; *value = v;
            } else if (h == SET_TAP_ROTATE) {                  /* lock the way up it has now, or let it turn again */
                tank_orient_lock(t, !t->orient_lock); progression_settings_changed();
                r = SET_TAP_ROTATE; *value = t->orient_lock;
            } else if (h == SET_TAP_SCREEN) {                  /* the way up: NORMAL (0, the default) / TURNED (1) */
                if ((v != 0) != t->screen_turned) { tank_screen_set(t, v != 0); progression_settings_changed(); }
                r = SET_TAP_SCREEN; *value = v;
            } else if (h == SET_TAP_CLOSE || h == SET_TAP_UPDATES || h == SET_TAP_LINK) { r = h; *value = v; }
        }
    }
    s_set_down = down;
    return r;
}

#if PT_NET   /* link mode, its badges and the DRIFTED notice (version.h PT_NET) */
/* ---- link mode (0.4, the tank link: render.h, docs/tank-link-0.4.md) ---- */
/* the badges: a shape AND a color (Strato is color blind: the shape is the
   identity, the color a second cue) - circle, triangle, square, diamond */
static const uint32_t LINK_BADGE_RGB[LINK_BADGES] = { 0xff725c, 0x78d67d, 0x4da3ff, 0xffd166 };
void link_badge_draw(ctx_t *c, int cx, int cy, int r, int badge, int a) {
    badge = badge < 0 ? 0 : badge % LINK_BADGES;
    uint32_t rgb = LINK_BADGE_RGB[badge];
    for (int y = -r - 1; y <= r + 1; y++)
        for (int x = -r - 1; x <= r + 1; x++) {
            float fx = x + 0.5f, fy = y + 0.5f, d;            /* d: inside < 0 < outside, in px */
            switch (badge) {
            case 0: d = sqrtf(fx * fx + fy * fy) - r; break;                                  /* circle */
            case 1: { float t = (fy + r) / (2.0f * r); d = fmaxf(fabsf(fx) - t * r * 0.98f, fabsf(fy) - r); break; }   /* triangle, point up */
            case 2: d = fmaxf(fabsf(fx), fabsf(fy)) - r * 0.86f; break;                        /* square */
            default: d = fabsf(fx) + fabsf(fy) - r * 1.12f; break;                             /* diamond */
            }
            if (d < -1.5f) px_blend(c, cx + x, cy + y, rgb, a);
            else if (d < 0.5f) px_blend(c, cx + x, cy + y, 0xffffff, a);                      /* a light rim: it reads on any water */
        }
}
void render_link_badge(uint16_t *fb, int stride, int cx, int cy, int r, int badge) {
    ctx_t c = ctx_full(fb, stride, 1.0f); link_badge_draw(&c, cx, cy, r, badge, 255);
}
/* the link glyph: two little tanks side by side, an open door between them */
static void link_glyph(ctx_t *c, int x, int y, uint32_t rgb) {
    rect_edge(c, x, y, 8, 12, rgb); rect_edge(c, x + 12, y, 8, 12, rgb);
    rect_fill(c, x + 8, y + 5, 4, 2, rgb);                              /* the way through */
}

/* where things are (frame coordinates) */
#define LINK_TAB_W    46
#define LINK_TAB_H    104
#define LINK_TAB_GAP  6                                     /* from the glass */
/* the hint and NEARBY, down by the floor (Strato, 2026-10-05: out of the fish's
   way) - below the PAGE safe box, so checked per glass: page y 284..348 is on
   the 1.8's sand, inside a 330+ px chord of the bowl's circle (the widest line
   is 312 px) and above the watch's lower corners (page 335 = frame 402) */
#define LINK_HINT_Y   284
/* the DRIFTED notice's panel (page y) and its DISCONNECT button; link mode's
   DISCONNECT chip mirrors DONE's at the top right while a door is open */
#define LINK_DRIFT_Y  70
#define LINK_DISC_W   156
#define LINK_DISC_X   ((PAGE_W - LINK_DISC_W) / 2)
#define LINK_DISC_Y   (LINK_DRIFT_Y + 122)
static int link_dchip_w(void) { return 10 + text_w("DISCONNECT", 2) + 10; }
#ifdef TANK_ROUND          /* the bowl's top is a narrow cap: DISCONNECT gets its own row, centred under DONE and the badge */
static int link_dchip_x(void) { return (TANK_W - link_dchip_w()) / 2; }
static int link_dchip_y(void) { return TOOL_CHIP_Y + TOOL_CHIP_H + 10; }
#else
static int link_dchip_x(void) { return TANK_W - TOOL_CHIP_X - link_dchip_w(); }
static int link_dchip_y(void) { return TOOL_CHIP_Y; }
#endif
static float link_mid_y(void) { return TANK_BOT * 0.5f; }
static int link_tab_x(int e) {
    float y = link_mid_y();
    return e == LINK_LEFT ? (int)tank_glass_x0(y) + LINK_TAB_GAP : (int)tank_glass_x1(y) - LINK_TAB_GAP - LINK_TAB_W;
}
static int link_tab_y(void) { return (int)link_mid_y() - LINK_TAB_H / 2; }
static int link_chip_w(void) { return 8 + 20 + 8 + text_w("DONE", 2) + 10; }

static float g_link_hold[LINK_EDGES];
static int   g_link_down = -1;                              /* what the finger went down on: an edge, 2 = DONE, -1 = nothing */
static bool  g_link_fired;
void render_link_where(int what, float *x, float *y) {
    if (what == 2) { *x = TOOL_CHIP_X + link_chip_w() / 2.0f; *y = TOOL_CHIP_Y + TOOL_CHIP_H / 2.0f; return; }
    if (what == 3) { *x = link_dchip_x() + link_dchip_w() / 2.0f; *y = link_dchip_y() + TOOL_CHIP_H / 2.0f; return; }   /* DISCONNECT in link mode */
    if (what == 4) { *x = PAGE_X + LINK_DISC_X + LINK_DISC_W / 2.0f; *y = PAGE_Y + LINK_DISC_Y + MSP_CLOSE_H / 2.0f; return; }   /* ... on the DRIFTED notice */
    *x = link_tab_x(what) + LINK_TAB_W / 2.0f; *y = link_mid_y();
}
void render_link_leave(void) { g_link_hold[0] = g_link_hold[1] = 0; g_link_down = -1; g_link_fired = false; }
void render_link_set_holds(float left, float right) { g_link_hold[LINK_LEFT] = left; g_link_hold[LINK_RIGHT] = right; }

static void link_chevron(ctx_t *c, int cx, int cy, int dir, uint32_t rgb, int a) {   /* dir -1 points left, +1 right */
    for (int i = 0; i < 9; i++) {
        int x = cx - dir * 6 + dir * i, dy = 9 - i;
        for (int k = 0; k < 4; k++) { px_blend(c, x + dir * k, cy - dy, rgb, a); px_blend(c, x + dir * k, cy + dy, rgb, a); }
    }
}
static void link_tab(ctx_t *c, const link_view_t *v, int e, float clock) {   /* clock: tank_anim's */
    const int X = link_tab_x(e), Y = link_tab_y(), W = LINK_TAB_W, H = LINK_TAB_H, dir = e == LINK_LEFT ? -1 : 1;
    const int st = v->edge[e].state;
    float pulse = 0.5f + 0.5f * sinf(clock * 5.0f);
    shade_box(c, X, Y, W, H);
    uint32_t edge_rgb = st == LV_OPEN ? LINK_BADGE_RGB[v->edge[e].badge % LINK_BADGES] : st == LV_FREE ? 0x3f6a72 : 0xffffff;
    rect_edge(c, X, Y, W, H, edge_rgb);
    if (st != LV_FREE) rect_edge(c, X + 1, Y + 1, W - 2, H - 2, edge_rgb);
    /* the hold: the tab fills from the bottom */
    float h = g_link_hold[e] < 0 ? 0 : g_link_hold[e] > 1 ? 1 : g_link_hold[e];
    if (h > 0) { int fh = (int)((H - 4) * h); for (int y = Y + H - 2 - fh; y < Y + H - 2; y++) for (int x = X + 2; x < X + W - 2; x++) px_blend(c, x, y, 0xffffff, 70); }
    const int cx = X + W / 2, cy = Y + H / 2;
    switch (st) {
    case LV_FREE:  link_chevron(c, cx, cy, dir, 0x8fb4ba, 255); break;
    case LV_ARMED: link_chevron(c, cx, cy, dir, 0xffffff, 140 + (int)(115 * pulse)); break;          /* ours, asking */
    case LV_ASKED: link_badge_draw(c, cx, cy - 22, 12, v->edge[e].badge, 255);                       /* the tank asking here */
                   link_chevron(c, cx, cy + 18, dir, LINK_BADGE_RGB[v->edge[e].badge % LINK_BADGES], 120 + (int)(135 * pulse)); break;
    case LV_TWO:   link_badge_draw(c, cx, cy - 26, 10, v->edge[e].badge, 255);                       /* two asking: both shown, nothing joins */
                   link_badge_draw(c, cx, cy, 10, v->edge[e].badge2, 255);
                   draw_text(c, cx - text_w("2", 2) / 2, cy + 20, 2, 0xffffff, "2"); break;
    case LV_OPEN:  link_badge_draw(c, cx, cy - 18, 13, v->edge[e].badge, 255);                       /* the door is open: the neighbor */
                   link_chevron(c, cx - dir * 4, cy + 22, dir, 0xffffff, 200); link_chevron(c, cx + dir * 6, cy + 22, dir, 0xffffff, 200); break;
    }
}
static void link_line(ctx_t *c, int py, const char *s, uint32_t rgb) {   /* a centred hint line at PAGE y */
    int w = text_w(s, 2), x = PAGE_X + (PAGE_W - w) / 2, y = PAGE_Y + py;
    for (int yy = y - 4; yy < y + 18; yy++) for (int xx = x - 8; xx < x + w + 8; xx++) px_blend(c, xx, yy, 0x04141a, 170);
    draw_text(c, x, y, 2, rgb, s);
}
void render_link_mode(const link_view_t *v, uint16_t *fb, int stride, double tclock) {
    const float clock = tank_anim(tclock);
    ctx_t c = ctx_full(fb, stride, 1.0f);
    /* the chip: the glyph and DONE (a tool in hand's chip) */
    const int CX = TOOL_CHIP_X, CY = TOOL_CHIP_Y, CW = link_chip_w(), CH = TOOL_CHIP_H;
    shade_box(&c, CX, CY, CW, CH);
    rect_edge(&c, CX, CY, CW, CH, 0xffffff); rect_edge(&c, CX + 1, CY + 1, CW - 2, CH - 2, 0x9fd8e2);
    link_glyph(&c, CX + 8, CY + (CH - 12) / 2, 0xffffff);
    draw_text(&c, CX + 8 + 20 + 8, CY + (CH - 14) / 2, 2, 0xffffff, "DONE");
    /* linked and nothing pending: DONE is the next step - say it with the chip
       itself (Strato 2026-10-05: "not at all intuitive that the user is supposed
       to hit DONE"): a pulsing second outline and an arrow under it, bobbing up
       at it - motion and shape, not a color */
    bool done_next = (v->edge[0].state == LV_OPEN || v->edge[1].state == LV_OPEN) &&
                     v->edge[0].state != LV_ARMED && v->edge[1].state != LV_ARMED &&
                     v->edge[0].state != LV_ASKED && v->edge[1].state != LV_ASKED &&
                     v->edge[0].state != LV_TWO && v->edge[1].state != LV_TWO;
    if (done_next) {
        /* the chip fills in - bright with dark letters, breathing between two brightnesses - and an
           arrow under it bobs up at it */
        float pulse = 0.5f + 0.5f * sinf(clock * 4.0f);
        int a = 200 + (int)(55 * pulse);
        for (int y = CY + 2; y < CY + CH - 2; y++) for (int x = CX + 2; x < CX + CW - 2; x++) px_blend(&c, x, y, 0xe8fbff, a);
        link_glyph(&c, CX + 8, CY + (CH - 12) / 2, 0x04141a);
        draw_text(&c, CX + 8 + 20 + 8, CY + (CH - 14) / 2, 2, 0x04141a, "DONE");
        int ax = CX + CW / 2, ay = CY + CH + 10 + (int)(5 * (0.5f + 0.5f * sinf(clock * 4.0f)));
        for (int i = 0; i < 13; i++) for (int k = 0; k < 5; k++) {
            px_blend(&c, ax - 13 + i, ay + 13 - i + k, 0xffffff, 235);
            px_blend(&c, ax + 13 - i, ay + 13 - i + k, 0xffffff, 235);
        }
    }
    /* DISCONNECT: top right, mirroring DONE, while a door is open */
    if (v->edge[0].state == LV_OPEN || v->edge[1].state == LV_OPEN) {
        int dx = link_dchip_x(), dy = link_dchip_y(), dw = link_dchip_w();
        shade_box(&c, dx, dy, dw, CH);
        rect_edge(&c, dx, dy, dw, CH, 0xffffff); rect_edge(&c, dx + 1, dy + 1, dw - 2, CH - 2, 0x9fd8e2);
        draw_text(&c, dx + 10, dy + (CH - 14) / 2, 2, 0xffffff, "DISCONNECT");
    }
    /* ours: the badge, top centre */
    { int bx = TANK_W / 2, by = CY + CH / 2;
      for (int y = by - 22; y < by + 22; y++) for (int x = bx - 22; x < bx + 22; x++) px_blend(&c, x, y, 0x04141a, 150);
      link_badge_draw(&c, bx, by, 16, v->badge, 255); }
    for (int e = 0; e < LINK_EDGES; e++) link_tab(&c, v, e, clock);
    /* the hint, in plain words, for what the tank needs next */
    bool open = v->edge[0].state == LV_OPEN || v->edge[1].state == LV_OPEN;
    bool asked = v->edge[0].state == LV_ASKED || v->edge[1].state == LV_ASKED;
    bool two = v->edge[0].state == LV_TWO || v->edge[1].state == LV_TWO;
    bool ours = v->edge[0].state == LV_ARMED || v->edge[1].state == LV_ARMED;
    const char *l1, *l2;
    if (two)               { l1 = "TWO TANKS ARE ASKING";         l2 = "ONLY ONE AT A TIME"; }
    else if (asked)        { l1 = "A TANK IS ASKING";             l2 = "HOLD THE GLOWING ARROW"; }
    else if (ours)         { l1 = "ASKING...";                    l2 = "HOLD THE OTHER TANK'S ARROW"; }
    else if (open)         { l1 = "LINKED! THE DOOR IS OPEN";     l2 = "TAP DONE TO WATCH THEM SWIM"; }
    else if (!v->n_near)   { l1 = "OPEN LINK ON THE OTHER TANK";  l2 = "AND PUT THEM SIDE BY SIDE"; }
    else                   { l1 = "PUT THE TANKS SIDE BY SIDE";   l2 = "HOLD THE ARROWS THAT TOUCH"; }
    link_line(&c, LINK_HINT_Y, l1, 0xffffff);
    link_line(&c, LINK_HINT_Y + 22, l2, 0x9fd8e2);
    /* NEARBY: every tank in link mode that is heard */
    { const int py = LINK_HINT_Y + 46, r = 7;
      int w = text_w("NEARBY", 2) + 10 + (v->n_near ? v->n_near * (2 * r + 8) : text_w("NONE YET", 2));
      int x = PAGE_X + (PAGE_W - w) / 2, y = PAGE_Y + py;
      for (int yy = y - 4; yy < y + 18; yy++) for (int xx = x - 8; xx < x + w + 8; xx++) px_blend(&c, xx, yy, 0x04141a, 170);
      draw_text(&c, x, y, 2, 0x9fd8e2, "NEARBY");
      x += text_w("NEARBY", 2) + 10;
      if (!v->n_near) draw_text(&c, x, y, 2, 0x8fb4ba, "NONE YET");
      for (int i = 0; i < v->n_near; i++) link_badge_draw(&c, x + r + i * (2 * r + 8), y + 7, r, v->near_badge[i], 255); }
}
int render_link_touch(const link_view_t *v, float x, float y, bool down, float dt) {
    int r = LINK_UI_NONE;
    if (down && g_link_down == -1 && !g_link_fired) {                      /* the press: what is under it */
        float my = link_mid_y();
        bool open = v->edge[0].state == LV_OPEN || v->edge[1].state == LV_OPEN;
        if (x < TOOL_CHIP_X + link_chip_w() + 16 && y < TOOL_CHIP_Y + TOOL_CHIP_H + 24) g_link_down = 2;
        else if (open && x >= link_dchip_x() - 16 && x < link_dchip_x() + link_dchip_w() + 16 &&
                 y >= link_dchip_y() - 16 && y < link_dchip_y() + TOOL_CHIP_H + 16) g_link_down = 4;   /* DISCONNECT */
        else if (fabsf(y - my) < LINK_TAB_H / 2 + 30 && x < link_tab_x(LINK_LEFT) + LINK_TAB_W + 40) g_link_down = LINK_LEFT;
        else if (fabsf(y - my) < LINK_TAB_H / 2 + 30 && x > link_tab_x(LINK_RIGHT) - 40) g_link_down = LINK_RIGHT;
        else g_link_down = 3;                                              /* the water: nothing (a tool owns the glass) */
    }
    if (down && (g_link_down == LINK_LEFT || g_link_down == LINK_RIGHT) && !g_link_fired) {
        int e = g_link_down;
        bool open = v->edge[e].state == LV_OPEN;
        g_link_hold[e] += dt / (open ? LINK_CLOSE_HOLD_S : LINK_ARM_HOLD_S);
        if (g_link_hold[e] >= 1) {
            g_link_fired = true; g_link_hold[e] = 0;
            r = open ? (e == LINK_LEFT ? LINK_UI_CLOSE_LEFT : LINK_UI_CLOSE_RIGHT) : (e == LINK_LEFT ? LINK_UI_ARM_LEFT : LINK_UI_ARM_RIGHT);
        }
    }
    if (!down) {
        if (g_link_down == 2) r = LINK_UI_DONE;                             /* DONE: on the release */
        if (g_link_down == 4) r = LINK_UI_DISCONNECT;
        g_link_hold[0] = g_link_hold[1] = 0; g_link_down = -1; g_link_fired = false;
    }
    return r;
}
/* an open door outside link mode: a soft glow down the side glass in the
   neighbor's color and its badge, small (2026-10-05, Strato: a watch was
   linked but showed nothing - only the DRIFTED notice when it moved). The
   doorway proper comes with the doors, phase 4. */
#define LINK_DOOR_H   (TANK_BOT * 2 / 5)                    /* the door band: the middle of the water */
void render_link_doors(const link_view_t *v, uint16_t *fb, int stride, double tclock) {
    const float clock = tank_anim(tclock);
    ctx_t c = ctx_full(fb, stride, 1.0f);
    float my = link_mid_y(), pulse = 0.75f + 0.25f * sinf(clock * 1.6f);
    for (int e = 0; e < LINK_EDGES; e++) {
        if (v->edge[e].state != LV_OPEN) continue;
        uint32_t rgb = LINK_BADGE_RGB[v->edge[e].badge % LINK_BADGES];
        for (int y = (int)my - LINK_DOOR_H / 2; y < (int)my + LINK_DOOR_H / 2; y++) {
            float gx = e == LINK_LEFT ? tank_glass_x0((float)y) : tank_glass_x1((float)y);
            float ty = fabsf(y - my) / (LINK_DOOR_H / 2.0f);          /* fades toward the band's ends */
            int a0 = (int)(190 * pulse * (1 - ty * ty));
            for (int k = 0; k < 12; k++) {
                int x = e == LINK_LEFT ? (int)gx + k : (int)gx - 1 - k;
                px_blend(&c, x, y, rgb, a0 * (12 - k) / 12);
            }
        }
        int bx = e == LINK_LEFT ? (int)tank_glass_x0(my) + 24 : (int)tank_glass_x1(my) - 24;
        link_badge_draw(&c, bx, (int)my, 9, v->edge[e].badge, 230);
    }
}

/* the DRIFTED notice (link.h LEV_DRIFT): centred, plain words, the grace as a
   bar that empties; over the tank whether link mode is up or not */
void render_link_drift(const link_view_t *v, uint16_t *fb, int stride, double clock) {
    (void)clock;
    if (!v->drifting) return;
    ctx_t c = ctx_full(fb, stride, 1.0f);
    const int W = 336, H = 164, X = PAGE_X + (PAGE_W - W) / 2, Y = PAGE_Y + LINK_DRIFT_Y;
    shade_box(&c, X, Y, W, H);
    rect_edge(&c, X, Y, W, H, 0xffffff); rect_edge(&c, X + 1, Y + 1, W - 2, H - 2, 0x9fd8e2);
    static const char *const GRACE[4] = { "THE TANKS DRIFTED", "PUT THEM BACK SIDE BY SIDE", "AND KEEP THEM STILL", "OR THE FISH WILL SWIM HOME" };
    static const char *const HOME[4]  = { "THE TANKS DRIFTED", "THE FISH ARE SWIMMING HOME", "THEN THE DOOR CLOSES", "" };   /* past the grace */
    const char *const *L = v->drift_homeward ? HOME : GRACE;
    for (int i = 0; i < 4; i++) draw_text(&c, X + (W - text_w(L[i], 2)) / 2, Y + 12 + i * 22, 2, i ? 0x9fd8e2 : 0xffffff, L[i]);
    float left = v->drift_ms_left / (float)(v->drift_homeward ? LINK_HOMEWARD_MS : LINK_DRIFT_GRACE_MS); left = left < 0 ? 0 : left > 1 ? 1 : left;
    rect_edge(&c, X + 24, Y + 104, W - 48, 8, 0x3f6a72);
    rect_fill(&c, X + 26, Y + 106, (int)((W - 52) * left), 4, 0xffffff);
    /* DISCONNECT (Strato, 2026-10-05: "an easy way to unlink"): every door of this tank shuts now */
    button(&c, PAGE_X + LINK_DISC_X, PAGE_Y + LINK_DISC_Y, LINK_DISC_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "DISCONNECT", 2);
}
/* the DRIFTED notice's DISCONNECT answers a tap only where the notice is seen (the 11-02 sweep: its hit ran under every
   page and card - a neighbor lifting their tank and a tap mid-page closed every door and ate the tap): no page up, and no
   milestone notice, GIVE modal or gift card drawn over it */
bool render_drift_live(const tank_t *t, bool page_up) {
    const gift_book_t *g = gift_of(t);
    return !page_up && !notice_current() && gm_eval(t) == RENDER_GM_OFF && !(g && g->ask.on);
}
bool render_link_drift_hit(const link_view_t *v, float x, float y) {
    if (!v->drifting) return false;
    x -= PAGE_X; y -= PAGE_Y;
    return x >= LINK_DISC_X - 10 && x < LINK_DISC_X + LINK_DISC_W + 10 && y >= LINK_DISC_Y - 8 && y < LINK_DISC_Y + MSP_CLOSE_H + 12;
}
#endif
/* ---- THE ADVANCED PAGE (0.3.9.2) ---------------------------------------
 * Hidden behind a BOOT chord (common/advanced.c). Two rows on the settings
 * page's grid: TOUCH (one wide CALIBRATE button) and BATTERY (the size
 * between two arrows, as LIGHTS OUT steps its choice), a quiet note under
 * each, BACK where CLOSE stands; FOR GROWN-UPS small under the title. */
#define ADV_WARN_RGB  0xffc94a          /* the warning's triangle (its shape says it; the hue only helps) */
#define ADV_BAT_ROW   (SET_ROW1_Y + 108)                          /* the BATTERY row's label line */
#define ADV_SET_Y     (SET_SEG_Y(ADV_BAT_ROW) + SET_SEG_H + 34)   /* SET <size>, under the row's note */
#define ADV_SET_H     36
#define ADV_WARN_BOX_Y (SET_TITLE_Y + 202)                        /* the warning's box to tick: 20 px under READ THE SIZE PRINTED ON IT. */
#define ADV_WARN_BOX   30
#define ADV_CONT_W     128
#define ADV_CONT_X     (SET_CLOSE_X + MSP_CLOSE_W - ADV_CONT_W > SET_UPD_X + SET_UPD_W + 10 ? SET_CLOSE_X + MSP_CLOSE_W - ADV_CONT_W : SET_UPD_X + SET_UPD_W + 10)
#define ADV_START_Y    (SET_TITLE_Y + 164)                        /* the touch page's START */
#define ADV_START_W    140
#define ADV_RESET_Y    (ADV_START_Y + SET_SEG_H + 16)             /* ... RESET TOUCH under it */
#define ADV_RESET_W    160
/* the warning sign: a solid triangle, point up, w wide, its ! cut out in ink */
static void adv_warn_sign(ctx_t *c, int cx, int top, int w, uint32_t rgb) {
    int h = w * 7 / 8;
    for (int dy = 0; dy < h; dy++) {
        int half = (w / 2) * dy / h;
        rect_fill(c, cx - half, top + dy, 2 * half + 1, 1, rgb);
    }
    int bw = w / 9 < 2 ? 2 : w / 9;
    rect_fill(c, cx - bw / 2, top + h * 3 / 8, bw, h * 3 / 8, MSP_INK);
    rect_fill(c, cx - bw / 2, top + h * 13 / 16, bw, bw, MSP_INK);
}
static void text_centre(ctx_t *c, int y, int scale, uint32_t rgb, const char *s) {
    draw_text(c, (PAGE_W - text_w(s, scale)) / 2, y, scale, rgb, s);
}
/* SWAP IN, tapped (0.3.9.4, Strato: "some feedback on screen immediately when 'swap in' is tapped"): the button
   pulses toward white - its light, not its hue - and says SWAPPING while the platform fades the glass out; the save,
   the move and the restart happen in the dark (render_shelf_card_swapping: seconds since the tap, < 0 off) */
static float g_shc_swap = -1;
static uint32_t sh_mix(uint32_t a, uint32_t b, int t);   /* below */
void render_shelf_card_swapping(float t) { g_shc_swap = t; }
static void swap_button(ctx_t *c, int x, int y, int w, int h, float t) {
    float a = 0.5f - 0.5f * cosf(t * 6.2831853f / SWAP_PULSE_S);   /* 0 -> 1 -> 0 each pulse */
    uint32_t fill = sh_mix(MSP_TEAL, 0xf2fbfc, (int)(a * 200));
    button(c, x, y, w, h, fill, fill, "", 2);
    rect_edge(c, x - 3, y - 3, w + 6, h + 6, sh_mix(0x0a2a36, 0xf2fbfc, (int)(a * 220)));   /* (a halo that breathes with it) */
    draw_text(c, x + (w - text_w("SWAPPING", 2)) / 2, y + (h - 14) / 2, 2, MSP_INK, "SWAPPING");
}
/* the whole frame at light/256 of its light (the swap's fade to dark) */
void render_fade(uint16_t *fb, int stride, int light) {
    if (light >= 256) return;
    if (light < 0) light = 0;
    for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) {
        uint16_t q = FB_SWAP16(fb[y * stride + x]);
        int r = (((q >> 11) & 31) * light) >> 8, g = (((q >> 5) & 63) * light) >> 8, b = ((q & 31) * light) >> 8;
        fb[y * stride + x] = FB_SWAP16((uint16_t)((r << 11) | (g << 5) | b));
    }
}
static void lit_button(ctx_t *c, int x, int y, int w, int h, const char *label) {   /* teal, ink lettering */
    button(c, x, y, w, h, MSP_TEAL, MSP_TEAL, "", 2);
    draw_text(c, x + (w - text_w(label, 2)) / 2, y + (h - 14) / 2, 2, MSP_INK, label);
}
void render_advanced(uint16_t *fb, int stride, int pick_mah, int pick_ma, int set_mah, bool stock, bool fast) {
    ctx_t c = ctx_page(fb, stride);
    char txt[40];
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    text_centre(&c, SET_TITLE_Y, 3, 0xffffff, "ADVANCED");
    draw_text_8px(&c, (PAGE_W - (13 * 6 - 1)) / 2, SET_TITLE_Y + 25, MSP_QUIET, "FOR GROWN-UPS");
    /* TOUCH */
    draw_text(&c, SET_LABEL_X, SET_ROW1_Y, 2, MSP_TEAL, "TOUCH");
    button(&c, SET_SEG_X, SET_SEG_Y(SET_ROW1_Y), SET_SPAN_W, SET_SEG_H, 0x1c2f36, MSP_TEAL, "CALIBRATE", 2);
    draw_text(&c, SET_LABEL_X, SET_SEG_Y(SET_ROW1_Y) + SET_SEG_H + 10, 2, MSP_QUIET, "TAPS MISSING THEIR MARK?");
    /* BATTERY: < 800 MAH > */
    int y = SET_SEG_Y(ADV_BAT_ROW);
    int bx = SET_SEG_X + SET_ARW_W + 4, bw = SET_SPAN_W - 2 * (SET_ARW_W + 4);
    const int *sz; int n = adv_cell_sizes(&sz);
    draw_text(&c, SET_LABEL_X, ADV_BAT_ROW, 2, MSP_TEAL, "BATTERY");
    set_arrow(&c, SET_SEG_X, y, false, pick_mah > sz[0]);
    button(&c, bx, y, bw, SET_SEG_H, MSP_TEAL, MSP_TEAL, "", 2);
    snprintf(txt, sizeof txt, "%d MAH", pick_mah);
    int tw = text_w(txt, 2), sx = bx + (bw - tw - (fast ? 22 : 0)) / 2;
    if (fast) adv_warn_sign(&c, sx + 8, y + (SET_SEG_H - 15) / 2, 17, MSP_INK);   /* faster than the board's own: the sign rides along */
    draw_text(&c, sx + (fast ? 22 : 0), y + (SET_SEG_H - 14) / 2, 2, MSP_INK, txt);
    set_arrow(&c, SET_SEG_X + SET_SPAN_W - SET_ARW_W, y, true, pick_mah < sz[n - 1]);
    if (stock) snprintf(txt, sizeof txt, "STANDARD - CHARGES AT %d MA", pick_ma);
    else       snprintf(txt, sizeof txt, "CHARGES AT %d MA", pick_ma);
    draw_text(&c, SET_LABEL_X, y + SET_SEG_H + 10, 2, MSP_QUIET, txt);
    if (pick_mah != set_mah) {                    /* a size picked, not in use yet: SET applies it (the warning first, when it is faster) */
        snprintf(txt, sizeof txt, "SET %d MAH", pick_mah);
        lit_button(&c, SET_SEG_X, ADV_SET_Y, SET_SPAN_W, ADV_SET_H, txt);
        snprintf(txt, sizeof txt, "NOW %d MAH", set_mah);
        draw_text(&c, SET_LABEL_X, ADV_SET_Y + (ADV_SET_H - 14) / 2, 2, MSP_QUIET, txt);
    } else draw_text(&c, SET_LABEL_X, y + SET_SEG_H + 32, 2, MSP_QUIET, "MATCH THE SIZE ON YOUR BATTERY");
#if PT_CARD
    if (card_slot()) button(&c, SET_UPD_X, SET_FOOT_Y, SET_UPD_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "PACKS", 2);   /* only a board with a card slot (card.h) */
#endif
    button(&c, SET_CLOSE_X, SET_FOOT_Y, MSP_CLOSE_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "BACK", 2);
}
int render_advanced_tap(float x, float y) {
    x -= PAGE_X; y -= PAGE_Y;
    if (x >= SET_CLOSE_X - 8 && y >= SET_FOOT_Y - 4) return ADV_HIT_BACK;
#if PT_CARD
    if (card_slot() && x >= SET_UPD_X - 8 && x < SET_UPD_X + SET_UPD_W + 8 && y >= SET_FOOT_Y - 4) return ADV_HIT_PACKS;   /* the foot's left slot: the card library (packs_page.h) */
#endif
    if (x < SET_SEG_X - 10) return ADV_HIT_NONE;
    if (y >= SET_SEG_Y(SET_ROW1_Y) - 12 && y < SET_SEG_Y(SET_ROW1_Y) + SET_SEG_H + 12) return ADV_HIT_CALIBRATE;
    if (y >= SET_SEG_Y(ADV_BAT_ROW) - 12 && y < SET_SEG_Y(ADV_BAT_ROW) + SET_SEG_H + 12)
        return x < SET_SPAN_MID ? ADV_HIT_PREV : ADV_HIT_NEXT;                         /* the row's halves, as LIGHTS OUT's */
    if (y >= ADV_SET_Y - 8 && y < ADV_SET_Y + ADV_SET_H + 12) return ADV_HIT_SET;       /* (only while it is drawn: advanced.c asks) */
    return ADV_HIT_NONE;
}
/* the battery warning: a whole page - the sign and WARNING, what the size
 * does in plain words, the box to tick (the size again, in the keeper's own
 * hand), CANCEL and CONTINUE. CONTINUE is an outline with dim words until
 * the box is ticked, then a lit button (brightness and fill, not hue). */
void render_battery_warning(uint16_t *fb, int stride, int cell_mah, int charge_ma, bool checked) {
    ctx_t c = ctx_page(fb, stride);
    char txt[48];
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    {   /* the sign and the word, one group, centred */
        int sw = 36, gap = 12, tw = text_w("WARNING", 3), x0 = (PAGE_W - sw - gap - tw) / 2;
        adv_warn_sign(&c, x0 + sw / 2, SET_TITLE_Y - 6, sw, ADV_WARN_RGB);
        draw_text(&c, x0 + sw + gap, SET_TITLE_Y, 3, 0xffffff, "WARNING");
    }
    int y = SET_TITLE_Y + 42;
    text_centre(&c, y, 2, 0xffffff, "ONLY FOR A BIGGER BATTERY."); y += 30;
    snprintf(txt, sizeof txt, world_view()->text.charge_fmt, charge_ma);
    text_centre(&c, y, 2, MSP_TEAL, txt); y += 22;
    text_centre(&c, y, 2, MSP_TEAL, "A SMALLER BATTERY CHARGED THIS"); y += 22;
    text_centre(&c, y, 2, MSP_TEAL, "FAST CAN GET HOT, SWELL UP"); y += 22;
    text_centre(&c, y, 2, MSP_TEAL, "OR CATCH FIRE."); y += 30;
    text_centre(&c, y, 2, 0xffffff, "READ THE SIZE PRINTED ON IT.");
    {   /* the box: [x]  MY BATTERY IS 800 MAH / OR BIGGER */
        snprintf(txt, sizeof txt, "MY BATTERY IS %d MAH", cell_mah);
        int bs = ADV_WARN_BOX, gap = 14, tw = text_w(txt, 2), x0 = (PAGE_W - bs - gap - tw) / 2, by = ADV_WARN_BOX_Y;
        rect_fill(&c, x0, by, bs, bs, checked ? MSP_TEAL : 0x1c2f36);
        rect_edge(&c, x0, by, bs, bs, MSP_TEAL); rect_edge(&c, x0 + 1, by + 1, bs - 2, bs - 2, MSP_TEAL);
        if (checked)                                         /* the tick: a short stroke down, a long one up */
            for (int k = 0; k < 18; k++) {
                int px = x0 + 6 + k, py = k < 6 ? by + 14 + k : by + 20 - (k - 6) * 13 / 12;
                rect_fill(&c, px, py - 1, 2, 4, MSP_INK);
            }
        draw_text(&c, x0 + bs + gap, by, 2, 0xffffff, txt);
        draw_text(&c, x0 + bs + gap, by + 20, 2, 0xffffff, "OR BIGGER");
    }
    button(&c, SET_UPD_X, SET_FOOT_Y, SET_UPD_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "CANCEL", 2);
    if (checked) lit_button(&c, ADV_CONT_X, SET_FOOT_Y, ADV_CONT_W, MSP_CLOSE_H, "CONTINUE");
    else {
        rect_edge(&c, ADV_CONT_X, SET_FOOT_Y, ADV_CONT_W, MSP_CLOSE_H, MSP_DIM);
        draw_text(&c, ADV_CONT_X + (ADV_CONT_W - text_w("CONTINUE", 2)) / 2, SET_FOOT_Y + (MSP_CLOSE_H - 14) / 2, 2, MSP_DIM, "CONTINUE");
    }
}
int render_battery_warning_tap(float x, float y) {
    x -= PAGE_X; y -= PAGE_Y;
    if (y >= SET_FOOT_Y - 8) {
        if (x >= SET_UPD_X - 8 && x < SET_UPD_X + SET_UPD_W + 4) return ADV_HIT_CANCEL;
        if (x >= ADV_CONT_X - 4 && x < ADV_CONT_X + ADV_CONT_W + 8) return ADV_HIT_CONTINUE;
        return ADV_HIT_NONE;
    }
    if (y >= ADV_WARN_BOX_Y - 12 && y < ADV_WARN_BOX_Y + 46) return ADV_HIT_BOX;    /* the box or its words: the whole line */
    return ADV_HIT_NONE;
}
/* touch calibration's pages */
void render_touch_intro(uint16_t *fb, int stride, bool own_in_use, const char *msg) {
    ctx_t c = ctx_page(fb, stride);
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    text_centre(&c, SET_TITLE_Y, 3, 0xffffff, "TOUCH");
    int y = SET_TITLE_Y + 46;
    text_centre(&c, y, 2, MSP_TEAL, world_view()->text.hold_the_tank); y += 22;
    text_centre(&c, y, 2, MSP_TEAL, "YOU USUALLY DO."); y += 34;
    text_centre(&c, y, 2, MSP_TEAL, "NINE CROSSES COME UP."); y += 22;
    text_centre(&c, y, 2, MSP_TEAL, "TAP THE MIDDLE OF EACH ONE.");
    lit_button(&c, (PAGE_W - ADV_START_W) / 2, ADV_START_Y, ADV_START_W, SET_SEG_H, "START");
    if (own_in_use) button(&c, (PAGE_W - ADV_RESET_W) / 2, ADV_RESET_Y, ADV_RESET_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "RESET TOUCH", 2);
    if (msg) text_centre(&c, ADV_RESET_Y + MSP_CLOSE_H + 10, 2, 0xffffff, msg);
    button(&c, SET_CLOSE_X, SET_FOOT_Y, MSP_CLOSE_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "BACK", 2);
}
int render_touch_intro_tap(float x, float y, bool own_in_use) {
    x -= PAGE_X; y -= PAGE_Y;
    if (x >= SET_CLOSE_X - 8 && y >= SET_FOOT_Y - 4) return ADV_HIT_BACK;
    if (fabsf(x - PAGE_W / 2) < ADV_START_W / 2 + 12 && y >= ADV_START_Y - 12 && y < ADV_START_Y + SET_SEG_H + 6) return ADV_HIT_START;
    if (own_in_use && fabsf(x - PAGE_W / 2) < ADV_RESET_W / 2 + 12 && y >= ADV_RESET_Y - 6 && y < ADV_RESET_Y + MSP_CLOSE_H + 12) return ADV_HIT_RESET;
    return ADV_HIT_NONE;
}
static void touch_cross(ctx_t *c, int fx, int fy, uint32_t rgb) {      /* a cross at FRAME px */
    int x = fx - PAGE_X, y = fy - PAGE_Y;
    rect_fill(c, x - 24, y - 1, 49, 3, rgb); rect_fill(c, x - 1, y - 24, 3, 49, rgb);
    rect_fill(c, x - 4, y - 4, 9, 9, 0xffffff);
}
void render_touch_cross(uint16_t *fb, int stride, int i, int n, int tx, int ty) {
    ctx_t c = ctx_page(fb, stride);
    char txt[32];
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    touch_cross(&c, tx, ty, MSP_TEAL);
    snprintf(txt, sizeof txt, "TAP THE CROSS  %d OF %d", i + 1, n);
    text_centre(&c, ty - PAGE_Y < PAGE_H / 2 ? PAGE_H - 60 : 46, 2, 0xffffff, txt);
}
void render_touch_check(uint16_t *fb, int stride, int tx, int ty, float secs_left, float secs_total) {
    ctx_t c = ctx_page(fb, stride);
    char txt[40];
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    touch_cross(&c, tx, ty, MSP_TEAL);
    int y = ty - PAGE_Y < PAGE_H / 2 ? PAGE_H - 96 : 30;
    text_centre(&c, y, 2, 0xffffff, "TAP THIS CROSS TO KEEP"); y += 22;
    text_centre(&c, y, 2, 0xffffff, "THE NEW TOUCH"); y += 28;
    int s = (int)ceilf(secs_left < 0 ? 0 : secs_left);
    snprintf(txt, sizeof txt, "THE OLD ONE COMES BACK IN %d", s);
    text_centre(&c, y, 2, MSP_QUIET, txt); y += 24;
    int bw = 240, bx = (PAGE_W - bw) / 2;                    /* the time left, a bar that shrinks */
    float f = secs_total > 0 ? secs_left / secs_total : 0; if (f < 0) f = 0;
    rect_edge(&c, bx, y, bw, 8, MSP_DIM);
    rect_fill(&c, bx + 2, y + 2, (int)((bw - 4) * f), 4, MSP_TEAL);
}

/* ---- MY TANKS + THE LIBRARY: the tank switcher (docs/tank-switcher.md) ----
 * ART FOR APPROVAL, 2026-10-29. Two wooden shelves of four little tanks on
 * a dark wall, each tank holding the snapshot taken when it was shelved.
 * The tank on the glass is lit, with a NOW tag over its lid; the others are
 * dimmed and wear a sign of how they rest - zZ dormant, two bars paused:
 * shapes, never hue alone. An empty slot is a dashed outline and a +. The
 * frame's own coordinates (not the 448 x 368 page): the shelves are as wide
 * as each glass lets them be (BOARDS.md rule 5). */
#if PAGE_BOWL                    /* the bowl: a square picture from the circle's middle; the footer on two lines */
#define SH_TW      80
#define SH_IH      80
#define SH_GAP     10
#define SH_TITLE_Y 32
#define SH_ROW1_Y  80
#define SH_PAGER_Y 362
#define SH_FOOT_Y  404
#define SH_LIB_X0  64            /* THE LIBRARY's rows, in from the circle */
#elif defined(TANK_WATCH)        /* portrait little tanks; the footer on two lines, clear of the 100 px corners */
#define SH_TW      84
#define SH_IH      103
#define SH_GAP     10
#define SH_TITLE_Y 30
#define SH_ROW1_Y  76
#define SH_PAGER_Y 414
#define SH_FOOT_Y  452
#define SH_LIB_X0  24
#elif defined(TANK_SQUARE)
#define SH_TW      96
#define SH_IH      96
#define SH_GAP     14
#define SH_TITLE_Y 20
#define SH_ROW1_Y  80
#define SH_FOOT_Y  416
#define SH_LIB_X0  28
#else                            /* the 1.8 */
#define SH_TW      88
#define SH_IH      72
#define SH_GAP     14
#define SH_TITLE_Y 10
#define SH_ROW1_Y  54
#define SH_FOOT_Y  326
#define SH_LIB_X0  24
#endif
#define SH_LID      5
#define SH_BASE     4
#define SH_PIC_W    (SH_TW - 4)                          /* the picture inside the glass's two sides */
#define SH_BOX_H    (SH_LID + SH_IH + SH_BASE)
#define SH_TOP      7                                    /* the plank's top, seen from a little above */
#define SH_FACE     36                                   /* its front: the name plates (two lines of scale 2 - Strato: the 6 px names "barely visible") */
#define SH_SHELF_H  (SH_BOX_H + SH_TOP + SH_FACE)
#if defined(TANK_SQUARE) || defined(TANK_WATCH)
#define SH_ROW_GAP  26                                   /* a plank's foot to the next row's lid: room for a NOW tag */
#else
#define SH_ROW_GAP  22                                   /* (the 1.8 and the bowl: as tight as the tag allows) */
#endif
#define SH_ROW_W    (4 * SH_TW + 3 * SH_GAP)
#define SH_X0       ((TANK_W - SH_ROW_W) / 2)
#define SH_OVER     12                                   /* the plank runs this far past the end tanks */
#define SH_BTN_W    116
#define SH_ARW_W    36
#define SH_WALL_TOP 0x241b16
#define SH_WALL_BOT 0x0d0a09
#define SH_BRASS    0xd2b878
static ctx_t ctx_frame(uint16_t *fb, int stride) {
    CTX_MADE();
    ctx_t c = { fb, stride, 1.0f, 0, 0, TANK_W, TANK_H };
    g_dirty_hold = true;
    return c;
}
static uint32_t sh_mix(uint32_t a, uint32_t b, int t) {   /* t 0..256 from a to b */
    int r = ((a >> 16) & 255) + ((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t >> 8);
    int g = ((a >> 8) & 255) + ((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t >> 8);
    int l = (a & 255) + ((((int)b & 255) - (int)(a & 255)) * t >> 8);
    return (uint32_t)(r << 16 | g << 8 | l);
}
static uint32_t sh_shade(uint32_t rgb, int pct) {          /* 100 = as is */
    int r = ((rgb >> 16) & 255) * pct / 100, g = ((rgb >> 8) & 255) * pct / 100, b = (rgb & 255) * pct / 100;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}
static void sh_wall(ctx_t *c) {
    for (int y = 0; y < TANK_H; y++) rect_fill(c, 0, y, TANK_W, 1, sh_mix(SH_WALL_TOP, SH_WALL_BOT, y * 256 / TANK_H));
}
/* the wood: long streaks that wander, a little noise, darker end grain */
static inline float sh_sin(float a) {                     /* sinf to ~1e-4, without libm's range reduction (the wood's first draw, 0.3.9.3) */
    a -= 6.2831853f * floorf(a * 0.15915494f + 0.5f);       /* to -pi .. pi */
    if (a > 1.5707963f) a = 3.1415927f - a; else if (a < -1.5707963f) a = -3.1415927f - a;   /* to -pi/2 .. pi/2 */
    float a2 = a * a;
    return a * (1.0f + a2 * (-0.16666667f + a2 * (0.0083333310f + a2 * -0.00019840874f)));
}
static int sh_grain(int x, int y) {
    float g = sh_sin(x * 0.035f + sh_sin(x * 0.009f + y * 0.55f) * 2.4f + y * 1.15f);
    return (int)(g * 9) + (int)(chash(x >> 2, y) % 5) - 2;
}
static uint16_t *g_sh_strip[2]; static int g_sh_sx[2], g_sh_sy[2], g_sh_sw[2];   /* the planks' wood (sh_plank), while MY TANKS is open */
void render_shelf_free(void) {                            /* MY TANKS closed: the wood's memory back (0.4, the 11-02 evening sweep) */
    for (int k = 0; k < 2; k++) { free(g_sh_strip[k]); g_sh_strip[k] = NULL; }
}
static void sh_plank(ctx_t *c, int x, int y, int w) {     /* y = the top's back edge */
    for (int k = 1; k <= 10; k++)                          /* its shadow on the wall */
        rect_fill(c, x + 4, y + SH_TOP + SH_FACE + k - 1, w - 8, 1, sh_shade(sh_mix(SH_WALL_TOP, SH_WALL_BOT, (y + SH_TOP + SH_FACE + k) * 256 / TANK_H), 40 + k * 6));
    for (int b = 0; b < 2; b++) {                          /* two iron brackets under it */
        int bx = b ? x + w - 34 : x + 22;
        for (int dy = 0; dy < 14; dy++) rect_fill(c, bx, y + SH_TOP + SH_FACE + dy, 14 - dy, 1, dy == 0 ? 0x4a3c32 : 0x2e2620);
    }
    /* the wood (0.3.9.3, Strato: a tap on the shelves lagged "long enough to make it feel like it was a miss-tap"):
       its grain is two sinf a pixel - 340-470 ms a redraw on the 1.8, 730-960 on the square, every tap - so each plank's
       wood is worked out ONCE into a strip (heap: PSRAM on the board) and copied in after that */
    uint16_t **strip = g_sh_strip; int *sx = g_sh_sx, *sy = g_sh_sy, *sw = g_sh_sw;
    const int H = SH_TOP + SH_FACE;
    int slot = -1;
    for (int k = 0; k < 2 && slot < 0; k++) if (strip[k] && sx[k] == x && sy[k] == y && sw[k] == w) slot = k;
    if (slot < 0) for (int k = 0; k < 2 && slot < 0; k++) if (!strip[k]) {
        strip[k] = malloc((size_t)w * H * sizeof(uint16_t));
        if (!strip[k]) break;
        sx[k] = x; sy[k] = y; sw[k] = w; slot = k;
        for (int yy = 0; yy < H; yy++) {
            bool top = yy < SH_TOP;
            uint32_t base = top ? 0xb98652 : 0x8d5b31;
            for (int xx = 0; xx < w; xx++) {
                int pct = 100 + sh_grain(x + xx, y + yy);
                if (xx < 2 || xx >= w - 2) pct -= 22;      /* the ends */
                if (top) pct += yy - 3;                    /* lighter toward the front lip */
                strip[k][yy * w + xx] = src_color(sh_shade(base, pct), 1.0f).v;
            }
        }
    }
    for (int yy = 0; yy < H; yy++)
        for (int xx = 0; xx < w; xx++) {
            uint16_t v;
            if (slot >= 0) v = strip[slot][yy * w + xx];
            else { int pct = 100 + sh_grain(x + xx, y + yy) - (xx < 2 || xx >= w - 2 ? 22 : 0) + (yy < SH_TOP ? yy - 3 : 0);   /* (no memory: as before) */
                   v = src_color(sh_shade(yy < SH_TOP ? 0xb98652 : 0x8d5b31, pct), 1.0f).v; }
            if (CTX_IN(c, x + xx, y + yy)) CTX_PX(c, x + xx, y + yy) = FB_SWAP16(v);
        }
    rect_fill(c, x, y + SH_TOP - 1, w, 1, 0xdcab74);       /* the lip catches the light */
    rect_fill(c, x, y + SH_TOP + SH_FACE - 1, w, 1, 0x3e2613);
}
static void sh_flask(ctx_t *c, int x, int y, uint32_t rgb) {   /* 7 x 8: EXPERIMENTAL's sign */
    rect_fill(c, x + 2, y, 3, 1, rgb); rect_fill(c, x + 3, y + 1, 1, 2, rgb);
    for (int r = 0; r < 5; r++) rect_fill(c, x + 3 - (r + 1) / 2, y + 3 + r, 1 + 2 * ((r + 1) / 2), 1, rgb);
}
/* a name plate: scale 2 letters (0.3.9.3, Strato: the 6 px ones were "barely visible"), on two lines when the name is
   wider than its tank's share of the shelf - broken at the space nearest the middle, or where the line is full */
static void sh_plate(ctx_t *c, int cx, int y, const char *name) {
    const int maxw = SH_TW + SH_GAP - 8, fit = (maxw + 2) / 12;    /* (a plate keeps 4 px from the next - seven letters on every glass; letters a line holds) */
    char l1[16] = "", l2[16] = "";
    int n = (int)strlen(name); if (n > 15) n = 15;
    const char *nl = strchr(name, '\n');
    if (nl) { int a = (int)(nl - name); memcpy(l1, name, (size_t)(a < 15 ? a : 15)); snprintf(l2, sizeof l2, "%s", nl + 1); }   /* (the keeper's two lines) */
    else if (text_w(name, 2) <= maxw) memcpy(l1, name, (size_t)n);
    else {
        int cut = -1;                                      /* at a space, the nearest the middle that leaves both lines whole */
        for (int i = 1; i < n; i++) if (name[i] == ' ' && i <= fit && n - i - 1 <= fit && (cut < 0 || abs(i - n / 2) < abs(cut - n / 2))) cut = i;
        if (cut > 0) { memcpy(l1, name, (size_t)cut); memcpy(l2, name + cut + 1, (size_t)(n - cut - 1)); }
        else { int k = (n + 1) / 2; if (k > fit) k = fit; memcpy(l1, name, (size_t)k); memcpy(l2, name + k, (size_t)(n - k > fit ? fit : n - k)); }   /* (one long word: halves) */
    }
    int tw = text_w(l1, 2) > text_w(l2, 2) ? text_w(l1, 2) : text_w(l2, 2);
    int w = tw + 4, h = l2[0] ? 34 : 18;
    if (w < 30) w = 30;
    int x = cx - w / 2;
    rect_fill(c, x, y, w, h, SH_BRASS);
    rect_edge(c, x, y, w, h, 0x7a6435);
    draw_text(c, x + 2 + (tw - text_w(l1, 2)) / 2, y + 2, 2, 0x2c2412, l1);
    if (l2[0]) draw_text(c, x + 2 + (tw - text_w(l2, 2)) / 2, y + 18, 2, 0x2c2412, l2);   /* (2 px between the lines - Strato: they touched) */
}
static void sh_pic(ctx_t *c, int x, int y, const uint16_t *pic, int w, int h, bool dim) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint16_t v = pic[j * w + i];
            if (dim) {                                     /* asleep on the shelf: half the light, a cool cast */
                int r = (v >> 11) * 9 / 20, g = ((v >> 5) & 63) * 9 / 20, b = (v & 31) * 11 / 20 + 1;
                v = (uint16_t)(r << 11 | g << 5 | (b > 31 ? 31 : b));
            }
            px(c, x + i, y + j, v);
        }
}
/* a little tank: lid, the picture between two glass sides, the base; a soft streak of reflection */
static void sh_tank(ctx_t *c, int x, int y, int w, int ih, const uint16_t *pic, bool dim) {
    rect_fill(c, x, y, w, SH_LID, 0x1b1f22);
    rect_fill(c, x, y, w, 1, 0x4d575c);
    if (pic) sh_pic(c, x + 2, y + SH_LID, pic, w - 4, ih, dim);
    else rect_fill(c, x + 2, y + SH_LID, w - 4, ih, 0x05080a);
    uint32_t side = dim ? 0x4e6268 : 0x9cc2ca;
    rect_fill(c, x, y + SH_LID, 2, ih, side); rect_fill(c, x + w - 2, y + SH_LID, 2, ih, side);
    for (int j = 0; j < ih * 2 / 3; j++)                   /* the streak, top left */
        for (int k = 0; k < 3; k++) px_blend(c, x + 8 + j / 3 + k, y + SH_LID + 2 + j, 0xffffff, dim ? 12 : 22);
    rect_fill(c, x, y + SH_LID + ih, w, SH_BASE, 0x15181a);
    rect_fill(c, x, y + SH_LID + ih + SH_BASE - 1, w, 1, 0x0a0c0d);
}
static void sh_rest_sign(ctx_t *c, int cx, int cy, int state) {   /* zZ dormant, || paused, on a dark disc */
    for (int dy = -9; dy <= 9; dy++)
        for (int dx = -9; dx <= 9; dx++) {
            int d = dx * dx + dy * dy;
            if (d <= 64) px_blend(c, cx + dx, cy + dy, 0x0b0f12, 220);
            else if (d <= 90) px_blend(c, cx + dx, cy + dy, 0x9fb4ba, 200);
        }
    uint32_t ink = 0xe2edf0;
    if (state == SHELF_PAUSED) { rect_fill(c, cx - 4, cy - 4, 3, 9, ink); rect_fill(c, cx + 2, cy - 4, 3, 9, ink); return; }
    draw_text(c, cx - 1, cy - 6, 1, ink, "Z");             /* the big Z, up and right ... */
    rect_fill(c, cx - 6, cy + 1, 4, 1, ink);               /* ... a little z, down and left */
    px(c, cx - 4, cy + 2, src_color(ink, 1).v); px(c, cx - 5, cy + 3, src_color(ink, 1).v);
    rect_fill(c, cx - 6, cy + 4, 4, 1, ink);
}
static void sh_dashed(ctx_t *c, int x, int y, int w, int h, uint32_t rgb) {
    for (int i = 0; i < w; i++) if ((i / 4) % 2 == 0) { rect_fill(c, x + i, y, 1, 2, rgb); rect_fill(c, x + i, y + h - 2, 1, 2, rgb); }
    for (int j = 0; j < h; j++) if ((j / 4) % 2 == 0) { rect_fill(c, x, y + j, 2, 1, rgb); rect_fill(c, x + w - 2, y + j, 2, 1, rgb); }
}
static void sh_arrow(ctx_t *c, int x, int y, bool right, bool live) {
    uint32_t rgb = live ? MSP_TEAL : 0x2c4a52;
    button(c, x, y, SH_ARW_W, MSP_CLOSE_H, 0x1c2f36, rgb, "", 2);
    int cx = x + SH_ARW_W / 2, cy = y + MSP_CLOSE_H / 2;
    for (int i = 0; i < 6; i++) {
        int px0 = right ? cx - 3 + i : cx + 2 - i, hh = 5 - i;
        rect_fill(c, px0, cy - hh, 1, 2 * hh + 1, rgb);
    }
}
static void sh_pager(ctx_t *c, int cx, int y, int page, int pages) {
    char t[16];
    snprintf(t, sizeof t, "%d OF %d", page, pages);
    int tw = text_w(t, 2);
    sh_arrow(c, cx - tw / 2 - 12 - SH_ARW_W, y, false, page > 1);
    draw_text(c, cx - tw / 2, y + (MSP_CLOSE_H - 14) / 2, 2, pages > 1 ? 0xffffff : MSP_QUIET, t);
    sh_arrow(c, cx + (tw + 1) / 2 + 12, y, true, page < pages);
}
/* the footer: [left] < 1 OF 2 > [right] on one line, or the pager over the pair (the bowl, the watch) */
static void sh_footer(ctx_t *c, const char *left, bool left_sd, int wallet, const char *right, int page, int pages) {
    int cw = MSP_CLOSE_W;
#if PAGE_BOWL || defined(TANK_WATCH)
    sh_pager(c, TANK_W / 2, SH_PAGER_Y, page, pages);
    int lx = (TANK_W - (SH_BTN_W + 12 + cw)) / 2, rx = lx + SH_BTN_W + 12, y = SH_FOOT_Y;   /* the pair centred as one group */
#else
    sh_pager(c, (SH_X0 + SH_BTN_W + TANK_W - SH_X0 - cw) / 2, SH_FOOT_Y, page, pages);
    int lx = SH_X0, rx = TANK_W - SH_X0 - cw, y = SH_FOOT_Y;
#endif
#if PAGE_BOWL || defined(TANK_WATCH)
    if (!right[0]) lx = (TANK_W - SH_BTN_W) / 2;           /* (alone: centred) */
#endif
    if (left_sd) {                                         /* the wallet: the coin and the count */
        char t[16]; snprintf(t, sizeof t, "%d", wallet);
        int w = 16 + 6 + text_w(t, 2), x = lx + (SH_BTN_W - w) / 2;
        blit_icon(c, x, y + (MSP_CLOSE_H - 16) / 2, world_view()->art.coin16, 255);
        draw_text(c, x + 22, y + (MSP_CLOSE_H - 14) / 2, 2, 0xffffff, t);
    } else button(c, lx, y, SH_BTN_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, left, 2);
    (void)rx;
    if (right[0]) button(c, rx, y, cw, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, right, 2);
}

void render_shelf_thumb_size(int *w, int *h) { *w = SH_PIC_W; *h = SH_IH; }
void render_shelf_thumb(const uint16_t *frame, uint16_t *thumb) {
    int cw, ch;                                            /* the crop: the picture's shape from the frame's middle */
#if PAGE_BOWL
    ch = 340;                                              /* the square inside the circle, a little over */
#else
    ch = TANK_H;
#endif
    cw = ch * SH_PIC_W / SH_IH;
    if (cw > TANK_W) { cw = TANK_W; ch = cw * SH_IH / SH_PIC_W; }
    int ox = (TANK_W - cw) / 2, oy = (TANK_H - ch) / 2;
    for (int j = 0; j < SH_IH; j++)
        for (int i = 0; i < SH_PIC_W; i++) {               /* a box filter over the source pixels each one covers */
            int x0 = ox + i * cw / SH_PIC_W, x1 = ox + (i + 1) * cw / SH_PIC_W, y0 = oy + j * ch / SH_IH, y1 = oy + (j + 1) * ch / SH_IH;
            int r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint16_t v = FB_SWAP16(frame[y * TANK_W + x]);
                    r += v >> 11; g += (v >> 5) & 63; b += v & 31; n++;
                }
            thumb[j * SH_PIC_W + i] = n ? (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n)) : 0;
        }
}
/* THE TOUCH LOCK (0.3.9.3, art approved by Strato): keepers asked for a way to stop taps reaching the tank. Taps can't
 * undo a lock, so its switch lives where only BOOT reaches - MY TANKS (BOOT twice). A padlock button by the title:
 * open and outlined while the glass takes taps, shut and filled while it does not (the shape and the fill say it,
 * never hue alone); a locked board's NOW tank wears the padlock where the others wear their rest signs, and a tap
 * on a locked tank shows how to get back (render_touch_lock_hint) */
static bool g_sh_touch_lock;
void render_shelf_set_touch_lock(bool on) { g_sh_touch_lock = on; }
#define SH_LOCK_W 44
static void sh_padlock(ctx_t *c, int cx, int cy, bool shut, uint32_t ink, uint32_t hole) {   /* 12 x 16: the body, its keyhole, the shackle */
    rect_fill(c, cx - 6, cy - 1, 12, 9, ink);
    rect_fill(c, cx - 1, cy + 1, 2, 4, hole);
    int top = shut ? cy - 8 : cy - 11;                     /* open: the shackle lifted, its right leg out of the body */
    rect_fill(c, cx - 4, top, 8, 2, ink);
    rect_fill(c, cx - 5, top + 1, 2, cy - 1 - top - 1, ink);
    rect_fill(c, cx + 3, top + 1, 2, shut ? cy - 1 - top - 1 : 4, ink);
}
static void sh_lock_btn(ctx_t *c, int x, int y, bool locked) {
    if (locked) button(c, x, y, SH_LOCK_W, MSP_CLOSE_H, MSP_TEAL, MSP_TEAL, "", 2);
    else        button(c, x, y, SH_LOCK_W, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "", 2);
    sh_padlock(c, x + SH_LOCK_W / 2, y + MSP_CLOSE_H / 2 + 1, locked, locked ? MSP_INK : MSP_TEAL, locked ? MSP_TEAL : 0x1c2f36);
}
/* MY TANKS's title: the terrarium's is longer - in the round glass's narrow cap, beside the padlock, it is drawn a size down */
#define SH_TITLE   world_view()->text.my_tanks
#define SH_TITLE_S (PAGE_BOWL ? world_view()->bowl_title_s : 3)
#define SH_TITLE_DY (SH_TITLE_S == 2 ? 4 : 0)
static void sh_lock_xy(int *x, int *y) {                   /* by the title: at the shelves' right edge, or beside the title on the round glass and the watch */
#if PAGE_BOWL || defined(TANK_WATCH)
    *x = (TANK_W - text_w(SH_TITLE, SH_TITLE_S) - 14 - SH_LOCK_W) / 2 + text_w(SH_TITLE, SH_TITLE_S) + 14;   /* (the title and the button centered as one group) */
#else
    *x = SH_X0 + SH_ROW_W - SH_LOCK_W;
#endif
    *y = SH_TITLE_Y + 10 - MSP_CLOSE_H / 2;
}
void render_touch_lock_hint(uint16_t *fb, int stride) {    /* a tap on a locked tank: what happened and the way back */
    ctx_t c = ctx_frame(fb, stride);
    const char *l1 = "TOUCH IS LOCKED", *l2 = "PRESS BOOT TWICE TO UNLOCK";
    int tw = text_w(l1, 2) > text_w(l2, 1) ? text_w(l1, 2) : text_w(l2, 1);
    int w = 14 + 16 + 12 + tw + 14, h = 46, x = (TANK_W - w) / 2, y = TANK_H * 62 / 100;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px_blend(&c, x + i, y + j, 0x0b0f12, 225);
    rect_fill(&c, x, y, w, 1, MSP_TEAL); rect_fill(&c, x, y + h - 1, w, 1, MSP_TEAL);
    rect_fill(&c, x, y, 1, h, MSP_TEAL); rect_fill(&c, x + w - 1, y, 1, h, MSP_TEAL);
    sh_padlock(&c, x + 14 + 6, y + h / 2 + 2, true, MSP_TEAL, 0x0b0f12);
    draw_text(&c, x + 14 + 16 + 12, y + 9, 2, 0xffffff, l1);
    draw_text(&c, x + 14 + 16 + 12, y + 29, 1, MSP_TEAL, l2);
}
/* MOVE (0.3.9.3, art approved by Strato): a tank held on the shelf lifts out of its place - raised, framed in teal - the
 * title says which, the empty places light as places to put it (a tank's place trades with it), CANCEL bottom left */
static bool g_sh_moving; static int g_sh_move = -1; static char g_sh_move_name[24];
void render_shelf_set_move(bool on, int i, const char *name) {
    g_sh_moving = on; g_sh_move = i; snprintf(g_sh_move_name, sizeof g_sh_move_name, "%s", name ? name : "");
    for (char *p = g_sh_move_name; *p; p++) if (*p == '\n') *p = ' ';
}
static int sh_slot_x(int i) { return SH_X0 + (i & 3) * (SH_TW + SH_GAP); }
static int sh_slot_y(int i) { return SH_ROW1_Y + (i >> 2) * (SH_SHELF_H + SH_ROW_GAP); }
void render_shelf(uint16_t *fb, int stride, const shelf_slot_t slot[8], int page, int pages) {
    ctx_t c = ctx_frame(fb, stride);
    sh_wall(&c);
    const bool moving = g_sh_moving;
    if (moving) {                                          /* MOVE: the title names the tank in hand */
        char t[32]; snprintf(t, sizeof t, "MOVE %s", g_sh_move_name);
        draw_text(&c, (TANK_W - text_w(t, 2)) / 2, SH_TITLE_Y + 4, 2, 0xffffff, t);
    } else {
#if PAGE_BOWL || defined(TANK_WATCH)
    draw_text(&c, (TANK_W - text_w(SH_TITLE, SH_TITLE_S) - 14 - SH_LOCK_W) / 2, SH_TITLE_Y + SH_TITLE_DY, SH_TITLE_S, 0xffffff, SH_TITLE);   /* (beside the padlock) */
#else
    draw_text(&c, (TANK_W - text_w(SH_TITLE, SH_TITLE_S)) / 2, SH_TITLE_Y + SH_TITLE_DY, SH_TITLE_S, 0xffffff, SH_TITLE);
#endif
    }
    for (int row = 0; row < 2; row++)
        sh_plank(&c, SH_X0 - SH_OVER, sh_slot_y(row * 4) + SH_BOX_H, SH_ROW_W + 2 * SH_OVER);
    for (int i = 0; i < 8; i++) {
        const shelf_slot_t *s = &slot[i];
        int x = sh_slot_x(i), y = sh_slot_y(i), face = y + SH_BOX_H + SH_TOP;
        if (s->state == SHELF_EMPTY) {
            uint32_t col = moving ? MSP_TEAL : 0x5d6e72;      /* (moving: a place to put it, lit) */
            sh_dashed(&c, x, y, SH_TW, SH_BOX_H, col);
            int cx = x + SH_TW / 2, cy = y + SH_BOX_H / 2;
            rect_fill(&c, cx - 9, cy - 1, 19, 3, moving ? MSP_TEAL : MSP_QUIET); rect_fill(&c, cx - 1, cy - 9, 3, 19, moving ? MSP_TEAL : MSP_QUIET);
            continue;
        }
        const bool held = moving && i == g_sh_move;
        if (held) {                                        /* the tank in hand: lifted 10 px, framed, its place a shadow */
            for (int yy = 0; yy < 6; yy++) rect_fill(&c, x + 4, y + SH_BOX_H - 6 + yy, SH_TW - 8, 1, sh_shade(0x3a2a1e, 60 + yy * 4));
            y -= 10;
            rect_edge(&c, x - 3, y - 3, SH_TW + 6, SH_BOX_H + 6, MSP_TEAL); rect_edge(&c, x - 2, y - 2, SH_TW + 4, SH_BOX_H + 4, MSP_TEAL);
        }
        bool now = s->state == SHELF_NOW;
        if (now)                                           /* its light falls on the plank */
            for (int yy = 0; yy < SH_TOP; yy++)
                for (int xx = -8; xx < SH_TW + 8; xx++) {
                    int edge = xx < 0 ? -xx : xx >= SH_TW ? xx - SH_TW + 1 : 0;
                    px_blend(&c, x + xx, y + SH_BOX_H + yy, 0xfff0c8, 110 - edge * 12);
                }
        sh_tank(&c, x, y, SH_TW, SH_IH, s->thumb, !now);
        if (now) {                                         /* the NOW tag over the lid (scale 2, 0.3.9.3) */
            int w = text_w("NOW", 2) + 12, tx = x + (SH_TW - w) / 2, ty = y - 20;
            rect_fill(&c, tx, ty, w, 18, MSP_TEAL);
            draw_text(&c, tx + 6, ty + 2, 2, MSP_INK, "NOW");
        } else sh_rest_sign(&c, x + SH_TW - 13, y + SH_LID + 12, s->state);
        if (now && g_sh_touch_lock) {                      /* the locked glass: the padlock where the others wear their rest signs */
            int cx = x + SH_TW - 13, cy = y + SH_LID + 12;
            for (int dy = -9; dy <= 9; dy++) for (int dx = -9; dx <= 9; dx++) { int d = dx * dx + dy * dy;
                if (d <= 64) px_blend(&c, cx + dx, cy + dy, 0x0b0f12, 220); else if (d <= 90) px_blend(&c, cx + dx, cy + dy, 0x9fb4ba, 200); }
            sh_padlock(&c, cx, cy + 1, true, 0xe2edf0, 0x0b0f12);
        }
        if (s->experimental) {                             /* EXPERIMENTAL: the flask on the glass, low left, in the rest signs' disc (the plate is the name's) */
            int cx = x + 13, cy = y + SH_BOX_H - 15;
            for (int dy = -9; dy <= 9; dy++) for (int dx = -9; dx <= 9; dx++) { int d = dx * dx + dy * dy;
                if (d <= 64) px_blend(&c, cx + dx, cy + dy, 0x0b0f12, 220); else if (d <= 90) px_blend(&c, cx + dx, cy + dy, 0x9fb4ba, 200); }
            sh_flask(&c, cx - 3, cy - 4, 0xe2edf0);
        }
        sh_plate(&c, x + SH_TW / 2, face + 1, s->name);
    }
    sh_footer(&c, moving ? "CANCEL" : "LIBRARY", false, 0, moving ? "" : "CLOSE", page, pages);
    if (!moving) { int lx, ly; sh_lock_xy(&lx, &ly); sh_lock_btn(&c, lx, ly, g_sh_touch_lock); }
}

int render_shelf_tap(float x, float y) {
    const int s = 8;                                       /* a fingertip's slop round each button */
#if PAGE_BOWL || defined(TANK_WATCH)
    int lx = (TANK_W - (SH_BTN_W + 12 + MSP_CLOSE_W)) / 2, rx = lx + SH_BTN_W + 12, fy = SH_FOOT_Y, py = SH_PAGER_Y, pcx = TANK_W / 2;
#else
    int lx = SH_X0, rx = TANK_W - SH_X0 - MSP_CLOSE_W, fy = SH_FOOT_Y, py = SH_FOOT_Y, pcx = (SH_X0 + SH_BTN_W + TANK_W - SH_X0 - MSP_CLOSE_W) / 2;
#endif
    { int kx, ky; sh_lock_xy(&kx, &ky);                   /* the padlock by the title */
      if (x >= kx - s && x < kx + SH_LOCK_W + s && y >= ky - s && y < ky + MSP_CLOSE_H + s) return SHELF_TAP_TOUCH_LOCK; }
    if (y >= fy - s && y < fy + MSP_CLOSE_H + s) {
        if (x >= lx - s && x < lx + SH_BTN_W + s) return SHELF_TAP_LIBRARY;
        if (x >= rx - s && x < rx + MSP_CLOSE_W + s) return SHELF_TAP_CLOSE;
    }
    if (y >= py - s && y < py + MSP_CLOSE_H + s) {         /* the pager's arrows: either side of "1 OF 2" */
        if (x >= pcx - 120 && x < pcx - 30) return SHELF_TAP_PREV;
        if (x >= pcx + 30 && x < pcx + 120) return SHELF_TAP_NEXT;
    }
    for (int i = 0; i < 8; i++) {
        int sx = sh_slot_x(i), sy = sh_slot_y(i);
        if (x >= sx - 4 && x < sx + SH_TW + 4 && y >= sy - 4 && y < sy + SH_BOX_H + SH_TOP + SH_FACE) return SHELF_TAP_SLOT0 + i;
    }
    return SHELF_TAP_NONE;
}

/* a tank's card: over the shelves, dimmed. The little tank, its name, what
 * it is, how long it has rested; TIME: DORMANT | PAUSED (the one chosen is
 * filled - fill, not hue); a plain line saying what that means; SWAP IN
 * (lit), RENAME, MORE. A tap outside the card closes it. */
#if PAGE_BOWL
#define SHC_W 330
#else
#define SHC_W 344
#endif
#define SHC_SEG_H 30
#define SHC_TOP   100                                      /* the card's lines beside its picture: name + four at scale 2 */
#define SHC_NOTE  36                                       /* under TIME: its note at scale 2, then the buttons */
/* RENAME on a tank's card (0.3.9.3, art approved by Strato): a tank's name is its plate's two lines of SHR_N, set
 * each on its own (Strato: "give the users both lines of text to configure independently"), TYPED on the Wi-Fi
 * password page's keyboard (Strato: the small letter wheel was "fatiguing" - tiny letters, a lot of precise scrolling).
 * 7 x 3 big keys: two rows of fourteen characters a page (MORE turns it: A-N, O-Z ' ., digits - ! ? /) and the function
 * row MORE | DEL | SPACE | LINE | BACK | DONE (on the round glass in its widest band, six keys, the letters under it).
 * Above, the name as its plate will show it, in brass: the line being typed carries its seven marks and the blinking
 * bar; a tap on a line (or LINE) moves to it, CLEAR beside the plate empties both. A key acts when the finger lifts on
 * the key it landed on. A letter goes at the end of its line; a full first line carries on into the second; DEL on an
 * empty second line goes back into the first */
#define SHR_N     7
#if PAGE_BOWL                    /* the round glass: the function row in its widest band, six wider keys; the letters under it */
#define NK_PX 44
#define NK_W  40
#define NK_Y  222
#define NK_PY 72
#define NK_H  66
#define NK_S  3                  /* the plate's letters */
#define NK_PLATE_Y 70
#define NK_FN_Y 156              /* (the function row's top, its key and pitch) */
#define NK_FN_W 60
#define NK_FN_PX 64
#define NK_FN_H 54
#elif defined(TANK_WATCH)
#define NK_PX 56
#define NK_W  52
#define NK_Y  184
#define NK_PY 78
#define NK_H  72
#define NK_S  4
#define NK_PLATE_Y 70
#elif defined(TANK_SQUARE)
#define NK_PX 66
#define NK_W  62
#define NK_Y  176
#define NK_PY 92
#define NK_H  84
#define NK_S  4
#define NK_PLATE_Y 58
#else                            /* the 1.8: the bottom row clear of the 40 px corners */
#define NK_PX 60
#define NK_W  56
#define NK_Y  126
#define NK_PY 76
#define NK_H  70
#define NK_S  4
#define NK_PLATE_Y 34
#endif
#define NK_X0 ((TANK_W - (6 * NK_PX + NK_W)) / 2)
#ifdef NK_FN_Y
#define NK_FH NK_FN_H
#else
#define NK_FH NK_H
#endif
#define NK_GW (6 * NK_S)
#define NK_LH (7 * NK_S)
#define NK_PW (SHR_N * NK_GW - NK_S + 24)
#define NK_PH (2 * NK_LH + 30)
#define NK_PX0 ((TANK_W - NK_PW) / 2 - 40)
#define NK_CLEAR_W 68
static const char *const NK_PAGES[3] = { "ABCDEFGHIJKLMN", "OPQRSTUVWXYZ'.", "1234567890-!?/" };
const char *render_shelf_rename_keys(int page) { return NK_PAGES[page < 0 || page > 2 ? 0 : page]; }
/* a function key's box: SHK_MORE .. SHK_DONE */
static void nk_fn_box(int key, int *x, int *y, int *w, int *h) {
    int col = key == SHK_MORE ? 0 : key == SHK_DEL ? 1 : key == SHK_SPACE ? 2 : key == SHK_LINE ? 4 : key == SHK_BACK ? 5 : 6;
#ifdef NK_FN_Y
    int k = col < 3 ? col : col - 1;
    *x = (TANK_W - (5 * NK_FN_PX + NK_FN_W)) / 2 + k * NK_FN_PX; *y = NK_FN_Y; *w = NK_FN_W; *h = NK_FN_H;
#else
    *x = NK_X0 + col * NK_PX; *y = NK_Y + 2 * NK_PY; *w = col == 2 ? NK_PX + NK_W : NK_W; *h = NK_H;
#endif
}
static void nk_box(int key, int *x, int *y, int *w, int *h) {   /* any key's box */
    if (key >= SHK_CHAR0 && key < SHK_CHAR0 + 14) { int cell = key - SHK_CHAR0; *x = NK_X0 + (cell % 7) * NK_PX; *y = NK_Y + (cell / 7) * NK_PY; *w = NK_W; *h = NK_H; return; }
    if (key == SHK_LINE1 || key == SHK_LINE2) { *x = NK_PX0; *y = NK_PLATE_Y + (key == SHK_LINE2 ? NK_PH / 2 : 0); *w = NK_PW; *h = NK_PH / 2; return; }
    if (key == SHK_CLEAR) { *x = NK_PX0 + NK_PW + 12; *y = NK_PLATE_Y + (NK_PH - MSP_CLOSE_H) / 2; *w = NK_CLEAR_W; *h = MSP_CLOSE_H; return; }
    nk_fn_box(key, x, y, w, h);
}
int render_shelf_rename_hit(float x, float y) {            /* the key under a finger (a few px of slop round each) */
    for (int key = SHK_CHAR0; key <= SHK_CLEAR; key++) {
        if (key >= SHK_CHAR0 + 14 && key < SHK_MORE) continue;
        int bx, by, bw, bh; nk_box(key, &bx, &by, &bw, &bh);
        int m = key == SHK_LINE1 || key == SHK_LINE2 ? 0 : 2;   /* (the keys stand 4 px apart: half of it each) */
        if (x >= bx - m && x < bx + bw + m && y >= by - m && y < by + bh + m) return key;
    }
    return SHK_NONE;
}
void render_shelf_rename_key_xy(int key, float *x, float *y) { int bx, by, bw, bh; nk_box(key, &bx, &by, &bw, &bh); *x = bx + bw * 0.5f; *y = by + bh * 0.5f; }
/* a stored name -> its two lines: split at the line break, or (a name from before the two lines) as the plate wraps it */
static void shr_split(const char *name, char l1[SHR_N + 1], char l2[SHR_N + 1]) {
    memset(l1, 0, SHR_N + 1); memset(l2, 0, SHR_N + 1);
    const char *nl = strchr(name, '\n');
    int n = (int)strlen(name);
    if (nl) { int a = (int)(nl - name); memcpy(l1, name, (size_t)(a < SHR_N ? a : SHR_N)); int b = n - a - 1; memcpy(l2, nl + 1, (size_t)(b < SHR_N ? b : SHR_N)); return; }
    if (n <= SHR_N) { memcpy(l1, name, (size_t)n); return; }
    int cut = -1;
    for (int i = 1; i < n; i++) if (name[i] == ' ' && i <= SHR_N && n - i - 1 <= SHR_N && (cut < 0 || abs(i - n / 2) < abs(cut - n / 2))) cut = i;
    if (cut > 0) { memcpy(l1, name, (size_t)cut); memcpy(l2, name + cut + 1, (size_t)(n - cut - 1)); }
    else { int k = (n + 1) / 2; if (k > SHR_N) k = SHR_N; memcpy(l1, name, (size_t)k); memcpy(l2, name + k, (size_t)(n - k > SHR_N ? SHR_N : n - k)); }
}
static bool nk_typable(char ch) { if (ch == ' ') return true; for (int p = 0; p < 3; p++) if (strchr(NK_PAGES[p], ch)) return true; return false; }
void render_shelf_rename_begin(shelf_rename_t *r, const char *name) {
    memset(r, 0, sizeof *r);
    char l[2][SHR_N + 1]; shr_split(name ? name : "", l[0], l[1]);
    for (int row = 0; row < 2; row++) {
        int n = 0;
        for (const char *p = l[row]; *p && n < SHR_N; p++) { char ch = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p; if (ch && nk_typable(ch)) r->line[row][n++] = ch; }   /* (what the keys cannot type: dropped) */
        r->line[row][n] = 0;
    }
    r->row = r->line[1][0] ? 1 : 0;                        /* at the end of the name: ready to add */
}
void render_shelf_rename_name(const shelf_rename_t *r, char *out, size_t cap) {   /* "LINE 1\nLINE 2" (spaces at the ends gone; one line when the second is blank) */
    char l[2][SHR_N + 1];
    for (int row = 0; row < 2; row++) {
        const char *p = r->line[row]; while (*p == ' ') p++;
        int n = (int)strlen(p); while (n > 0 && p[n - 1] == ' ') n--;
        memcpy(l[row], p, (size_t)n); l[row][n] = 0;
    }
    if (l[0][0] && l[1][0]) snprintf(out, cap, "%s\n%s", l[0], l[1]); else snprintf(out, cap, "%s", l[0][0] ? l[0] : l[1]);
}
static void nk_type(shelf_rename_t *r, char ch) {
    if ((int)strlen(r->line[r->row]) >= SHR_N && r->row == 0) r->row = 1;   /* a full first line carries on into the second */
    int n = (int)strlen(r->line[r->row]);
    if (n < SHR_N) { r->line[r->row][n] = ch; r->line[r->row][n + 1] = 0; }
}
int render_shelf_rename_touch(shelf_rename_t *r, float x, float y, bool down) {
    int ret = SHR_NONE;
    if (down && !r->down) { r->px = x; r->py = y; }
    else if (!down && r->down) {                           /* the lift: the key it landed on, if it lifted there too */
        int k = render_shelf_rename_hit(r->px, r->py);
        if (k && k == render_shelf_rename_hit(x, y)) {
            ret = SHR_TICK;
            if (k >= SHK_CHAR0 && k < SHK_CHAR0 + 14) nk_type(r, NK_PAGES[r->page][k - SHK_CHAR0]);
            else if (k == SHK_SPACE) nk_type(r, ' ');
            else if (k == SHK_DEL) {
                if (!r->line[r->row][0] && r->row == 1) r->row = 0;   /* an empty second line: back into the first */
                int n = (int)strlen(r->line[r->row]); if (n) r->line[r->row][n - 1] = 0;
            }
            else if (k == SHK_MORE) r->page = (r->page + 1) % 3;
            else if (k == SHK_LINE) r->row ^= 1;
            else if (k == SHK_LINE1 || k == SHK_LINE2) r->row = k == SHK_LINE2;
            else if (k == SHK_CLEAR) { r->line[0][0] = r->line[1][0] = 0; r->row = 0; }
            else if (k == SHK_BACK) ret = SHR_CANCEL;
            else if (k == SHK_DONE) ret = SHR_DONE;
        }
    }
    r->down = down;
    return ret;
}
void render_shelf_rename(uint16_t *fb, int stride, const shelf_slot_t *s, const shelf_rename_t *r, int part, double clock) {
    (void)s;
    ctx_t c = ctx_frame(fb, stride);
    if (part & 1) {
        for (int y = 0; y < TANK_H; y++)
            for (int x = 0; x < TANK_W; x++) px565_blend(&c, x, y, 0, 244);   /* (near black: the shelves only a hint behind) */
    }
    if (!(part & 2)) return;
    { const char *ti = world_view()->text.rename_tank; draw_text(&c, (TANK_W - text_w(ti, 2)) / 2, NK_PLATE_Y - 26, 2, 0xffffff, ti); }
    rect_fill(&c, NK_PX0, NK_PLATE_Y, NK_PW, NK_PH, SH_BRASS); rect_edge(&c, NK_PX0, NK_PLATE_Y, NK_PW, NK_PH, 0x7a6435);
    for (int k = 0; k < 2; k++) {
        int ty = NK_PLATE_Y + 9 + k * (NK_LH + 12), tx = NK_PX0 + 12;
        draw_text(&c, tx, ty, NK_S, 0x2c2412, r->line[k]);
        if (k == r->row) {
            for (int i = 0; i < SHR_N; i++) rect_fill(&c, tx + i * NK_GW, ty + NK_LH + 3, NK_GW - NK_S - 1, 2, 0x7a6435);   /* its seven places */
            int n = (int)strlen(r->line[k]);
            if (n < SHR_N && ((int)(clock * 2) & 1)) rect_fill(&c, tx + n * NK_GW, ty - 2, NK_S, NK_LH + 4, 0x2c2412);   /* the bar */
        }
    }
    { int x, y, w, h; nk_box(SHK_CLEAR, &x, &y, &w, &h); button(&c, x, y, w, h, 0x1c2f36, MSP_TEAL, "CLEAR", 2); }
    for (int cell = 0; cell < 14; cell++) {
        int x, y, w, h; nk_box(SHK_CHAR0 + cell, &x, &y, &w, &h);
        char lbl[2] = { NK_PAGES[r->page][cell], 0 };
        rect_fill(&c, x, y, w, h, 0x16262c); rect_edge(&c, x, y, w, h, 0x3d5d66);
        draw_text(&c, x + (w - text_w(lbl, 3)) / 2, y + (h - 21) / 2, 3, 0xffffff, lbl);
    }
    static const struct { int key; const char *lbl; } FK[6] = { { SHK_MORE, "MORE" }, { SHK_DEL, "DEL" }, { SHK_SPACE, "SPACE" }, { SHK_LINE, "LINE" }, { SHK_BACK, "BACK" }, { SHK_DONE, "DONE" } };
    for (int f = 0; f < 6; f++) {
        int x, y, w, h; nk_fn_box(FK[f].key, &x, &y, &w, &h);
        if (FK[f].key == SHK_DONE) lit_button(&c, x, y, w, h, "DONE");
        else if (FK[f].key == SHK_BACK) button(&c, x, y, w, h, 0x16262c, 0x3d5d66, "BACK", 2);   /* (it leaves the page: quieter) */
        else button(&c, x, y, w, h, 0x1c2f36, MSP_TEAL, FK[f].lbl, 2);
        if (FK[f].key == SHK_SPACE) rect_fill(&c, x + 10, y + h / 2 + 10, w - 20, 3, MSP_TEAL);   /* the space bar's bar */
        if (FK[f].key == SHK_MORE) for (int i = 0; i < 3; i++) rect_fill(&c, x + w / 2 - 10 + i * 8, y + h - 10, 4, 4, i == r->page ? MSP_TEAL : 0x3d5d66);   /* which page */
    }
}

static bool g_shc_glass;                                   /* the card drawn last was the tank on the glass's (its tap map) */
void render_shelf_card(uint16_t *fb, int stride, const shelf_slot_t *s, const char *kind, int shelf_days, int age_days) {
    ctx_t c = ctx_frame(fb, stride);
    for (int y = 0; y < TANK_H; y++)
        for (int x = 0; x < TANK_W; x++) px565_blend(&c, x, y, 0, 150);
    int top = SH_BOX_H > SHC_TOP ? SH_BOX_H : SHC_TOP;     /* the picture or the text, whichever is taller */
    int h = 16 + top + 14 + SHC_SEG_H + SHC_NOTE + MSP_CLOSE_H + 16;
    int mx = (TANK_W - SHC_W) / 2, my = (TANK_H - h) / 2;
    rect_fill(&c, mx, my, SHC_W, h, MSP_INK);
    rect_edge(&c, mx, my, SHC_W, h, MSP_TEAL); rect_edge(&c, mx + 1, my + 1, SHC_W - 2, h - 2, MSP_TEAL);
    sh_tank(&c, mx + 16, my + 16, SH_TW, SH_IH, s->thumb, false);
    int tx = mx + 16 + SH_TW + 14, ty = my + 18;
    char t[48];
    { char nm[24]; snprintf(nm, sizeof nm, "%s", s->name); for (char *p = nm; *p; p++) if (*p == '\n') *p = ' ';   /* (its two lines as one) */
      draw_text(&c, tx, ty, 2, 0xffffff, nm); ty += 24; }                /* (every line scale 2 since 0.3.9.3: the 6 px ones read as specks) */
    draw_text(&c, tx, ty, 2, MSP_TEAL, kind); ty += 18;
    if (s->experimental) { sh_flask(&c, tx, ty + 2, MSP_QUIET); draw_text(&c, tx + 10, ty, 2, MSP_QUIET, "EXPERIMENTAL"); ty += 18; }
    const bool glass = s->state == SHELF_NOW;              /* the tank on the glass (0.3.9.3): its name and RENAME, nothing to swap or rest */
    g_shc_glass = glass;
    if (glass) snprintf(t, sizeof t, "ON THE GLASS"); else snprintf(t, sizeof t, "SHELVED %d DAY%s", shelf_days, shelf_days == 1 ? "" : "S");
    draw_text(&c, tx, ty, 2, MSP_TEAL, t); ty += 18;
    if (age_days >= 0) { snprintf(t, sizeof t, "%d DAYS OLD", age_days); draw_text(&c, tx, ty, 2, MSP_TEAL, t); }
    int y = my + 16 + top + 14;                            /* TIME: DORMANT | PAUSED */
    if (glass) {                                           /* (in its place: what the shelf does with it) */
        const char *l1 = world_view()->text.swap_in, *l2 = "TO PUT THIS ONE AWAY";
        draw_text(&c, mx + (SHC_W - text_w(l1, 2)) / 2, y + 4, 2, MSP_QUIET, l1);
        draw_text(&c, mx + (SHC_W - text_w(l2, 2)) / 2, y + 24, 2, MSP_QUIET, l2);
        y += SHC_SEG_H + SHC_NOTE;
        button(&c, mx + (SHC_W - 98) / 2, y, 98, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "RENAME", 2);
        return;
    }
    draw_text(&c, mx + 16, y + (SHC_SEG_H - 14) / 2, 2, MSP_TEAL, "TIME");
    int sx = mx + 16 + text_w("TIME", 2) + 14, sw = mx + SHC_W - 16 - sx, half = sw / 2;
    bool paused = s->state == SHELF_PAUSED;
    for (int k = 0; k < 2; k++) {
        bool on = (k == 1) == paused;
        const char *lab = k ? "PAUSED" : "DORMANT";
        int bx = sx + k * half, bw = k ? sw - half : half;
        if (on) { rect_fill(&c, bx, y, bw, SHC_SEG_H, MSP_TEAL); }
        else { rect_fill(&c, bx, y, bw, SHC_SEG_H, 0x1c2f36); rect_edge(&c, bx, y, bw, SHC_SEG_H, MSP_TEAL); }
        draw_text(&c, bx + (bw - text_w(lab, 2)) / 2, y + (SHC_SEG_H - 14) / 2, 2, on ? MSP_INK : MSP_QUIET, lab);
    }
    y += SHC_SEG_H + 9;
    const char *note = paused ? "WAITS AS YOU LEFT IT" : "LIVES ON, AS IF ASLEEP";
    draw_text(&c, mx + (SHC_W - text_w(note, 2)) / 2, y, 2, MSP_QUIET, note);
    y += SHC_NOTE - 9;
    int bw0 = 120, bw1 = 98, bw2 = 74, gap = (SHC_W - 32 - bw0 - bw1 - bw2) / 2, bx = mx + 16;
    if (g_shc_swap >= 0) swap_button(&c, bx, y, bw0, MSP_CLOSE_H, g_shc_swap);   /* SWAP IN tapped: it answers at once */
    else lit_button(&c, bx, y, bw0, MSP_CLOSE_H, "SWAP IN");
    bx += bw0 + gap;
    button(&c, bx, y, bw1, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "RENAME", 2); bx += bw1 + gap;
    button(&c, bx, y, bw2, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "MORE", 2);
}

/* MORE on a tank's card: REMOVE (0.3.9.3, art approved by Strato; D5). The card again, its picture and name, and what
 * removing means in plain words: the tank and its fish are gone for good, its pieces sell back (the sand dollar and
 * the sum). BACK, and HOLD TO SELL - held 2 s, the fill growing across the button as the finger stays (the fill
 * says how far, never a hue); lifted early, nothing happens */
#define SHM_TOP (SH_BOX_H > 92 ? SH_BOX_H : 92)
#define SHM_H   (16 + SHM_TOP + 14 + 3 * 20 + 16 + MSP_CLOSE_H + 16)
#define SHM_BW  92
static void shm_btns(int *mx, int *my, int *by, int *rx, int *rw) {   /* the panel, its row of buttons, HOLD TO SELL's box */
    *mx = (TANK_W - SHC_W) / 2; *my = (TANK_H - SHM_H) / 2;
    *by = *my + 16 + SHM_TOP + 14 + 3 * 20 + 16;
    *rw = SHC_W - 32 - SHM_BW - 12; *rx = *mx + 16 + SHM_BW + 12;
}
int render_shelf_remove_hit(float x, float y) {
    int mx, my, by, rx, rw; shm_btns(&mx, &my, &by, &rx, &rw); const int s = 6;
    if (x < mx || x >= mx + SHC_W || y < my || y >= my + SHM_H) return SHM_TAP_OUTSIDE;
    if (y >= by - s && y < by + MSP_CLOSE_H + s) {
        if (x >= mx + 16 - s && x < mx + 16 + SHM_BW + s) return SHM_TAP_BACK;
        if (x >= rx - s && x < rx + rw + s) return SHM_TAP_HOLD;
    }
    return SHM_TAP_NONE;
}
void render_shelf_remove(uint16_t *fb, int stride, const shelf_slot_t *s, int refund, float hold, int part) {
    ctx_t c = ctx_frame(fb, stride);
    if (part & 1)
        for (int y = 0; y < TANK_H; y++)
            for (int x = 0; x < TANK_W; x++) px565_blend(&c, x, y, 0, 150);
    if (!(part & 2)) return;
    const int top = SHM_TOP, h = SHM_H;
    const int mx = (TANK_W - SHC_W) / 2, my = (TANK_H - h) / 2;
    rect_fill(&c, mx, my, SHC_W, h, MSP_INK);
    rect_edge(&c, mx, my, SHC_W, h, MSP_TEAL); rect_edge(&c, mx + 1, my + 1, SHC_W - 2, h - 2, MSP_TEAL);
    sh_tank(&c, mx + 16, my + 16, SH_TW, SH_IH, s->thumb, true);
    int tx = mx + 16 + SH_TW + 14, ty = my + 18;
    char nm[24]; snprintf(nm, sizeof nm, "%s", s->name); for (char *p = nm; *p; p++) if (*p == '\n') *p = ' ';
    draw_text(&c, tx, ty, 2, 0xffffff, "REMOVE"); ty += 22;
    draw_text(&c, tx, ty, 2, 0xffffff, nm); ty += 22;
    draw_text(&c, tx, ty, 2, MSP_TEAL, "FROM THE SHELF?");
    int y = my + 16 + top + 14;
    const char *l1 = world_view()->text.gone_l1, *l2 = world_view()->text.gone_l2;
    draw_text(&c, mx + (SHC_W - text_w(l1, 2)) / 2, y, 2, 0xffffff, l1); y += 20;
    draw_text(&c, mx + (SHC_W - text_w(l2, 2)) / 2, y, 2, 0xffffff, l2); y += 20;
    char t[32]; snprintf(t, sizeof t, "+%d FOR ITS PIECES", refund);
    int w = 16 + 6 + text_w(t, 2), x = mx + (SHC_W - w) / 2;
    blit_icon(&c, x, y - 1, world_view()->art.coin16, 255);
    draw_text(&c, x + 22, y, 2, MSP_TEAL, t);
    int bmx, bmy, rx, rw; shm_btns(&bmx, &bmy, &y, &rx, &rw);
    const int bw = SHM_BW;
    button(&c, mx + 16, y, bw, MSP_CLOSE_H, 0x1c2f36, MSP_TEAL, "BACK", 2);
    rect_fill(&c, rx, y, rw, MSP_CLOSE_H, 0x1c2f36);
    int fw = (int)(rw * (hold < 0 ? 0 : hold > 1 ? 1 : hold));
    if (fw > 0) rect_fill(&c, rx, y, fw, MSP_CLOSE_H, 0x2f7f8a);   /* the hold, filling (a deep teal: the white words read on both) */
    rect_edge(&c, rx, y, rw, MSP_CLOSE_H, MSP_TEAL);
    const char *lb = "HOLD TO SELL";   /* (Strato: "hold to sell") */
    draw_text(&c, rx + (rw - text_w(lb, 2)) / 2, y + (MSP_CLOSE_H - 14) / 2, 2, 0xffffff, lb);
}

/* LOADING (0.3.9.3, art approved by Strato: "i rather add a loading spinner if its some kind of first load churn"): a
 * tank's first seconds after a SWAP IN or a power-on bake its grass (THE GRASS LOOP, ~40 ms a moment) - behind this,
 * at full speed, instead of under a stuttering tank. Deep water, a ring of eight bubbles with a bright one going round
 * (the shape moves, not only the shade), the tank's name under it, then line (NULL: FILLING THE TANK). Strato 10-31,
 * after Machito asked why a wake said FILLING THE TANK: that line is a SWAP IN's alone - any other start (a long
 * sleep ends in a restart) says WAKING THE TANK, and the time check before it is this same screen (render_clock_sync),
 * so a board that asks the internet shows one screen whose words change, not two */
void render_loading(uint16_t *fb, int stride, const char *name, double clock, const char *line) {
    ctx_t c = ctx_frame(fb, stride);
    const world_look_t *lk = &world_view()->look;
    for (int y = 0; y < TANK_H; y++) {                     /* deep water, a little lighter at the top (the terrarium's: a desert dusk) */
        uint32_t col = sh_mix(lk->bg[0], lk->bg[1], y * 256 / TANK_H);
        rect_fill(&c, 0, y, TANK_W, 1, col);
    }
    const int cx = TANK_W / 2, cy = TANK_H / 2 - 24, R = 40;
    const int lead = (int)(clock * 10) % 8;                /* (a step a tenth of a second: once round in 0.8 s) */
    for (int k = 0; k < 8; k++) {
        float a = k * 6.2831853f / 8 - 1.5707963f;
        int bx = cx + (int)lroundf(cosf(a) * R), by = cy + (int)lroundf(sinf(a) * R);
        int age = (lead - k + 8) % 8;                      /* 0 = the bright one, then its trail */
        int r = age == 0 ? 11 : age == 1 ? 10 : 9;
        uint32_t rim = age == 0 ? lk->rim[0] : age == 1 ? lk->rim[1] : age == 2 ? lk->rim[2] : lk->rim[3];
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            int d = dx * dx + dy * dy;
            if (d > r * r) continue;
            px(&c, bx + dx, by + dy, src_color(d >= (r - 2) * (r - 2) ? rim : sh_mix(rim, lk->rim_dark, 150), 1.0f).v);   /* a bubble: its rim bright */
        }
        if (age == 0) rect_fill(&c, bx - 4, by - 4, 2, 2, 0xffffff);         /* (its glint) */
    }
    char nm[24]; snprintf(nm, sizeof nm, "%s", name ? name : ""); for (char *p = nm; *p; p++) if (*p == '\n') *p = ' ';
    draw_text(&c, (TANK_W - text_w(nm, 2)) / 2, cy + R + 26, 2, 0xffffff, nm);
    if (!line) line = world_view()->text.filling;
    draw_text(&c, (TANK_W - text_w(line, 2)) / 2, cy + R + 48, 2, MSP_TEAL, line);
}

/* the card's places (as render_shelf_card draws them) */
int render_shelf_card_tap(float x, float y) {
    int top = SH_BOX_H > SHC_TOP ? SH_BOX_H : SHC_TOP, h = 16 + top + 14 + SHC_SEG_H + SHC_NOTE + MSP_CLOSE_H + 16;
    int mx = (TANK_W - SHC_W) / 2, my = (TANK_H - h) / 2, s = 6;
    if (x < mx || x >= mx + SHC_W || y < my || y >= my + h) return SHC_TAP_OUTSIDE;
    int sy = my + 16 + top + 14, sx = mx + 16 + text_w("TIME", 2) + 14, sw = mx + SHC_W - 16 - sx;
    if (g_shc_glass) {                                     /* the tank on the glass: RENAME alone */
        int by = sy + SHC_SEG_H + SHC_NOTE, bx = mx + (SHC_W - 98) / 2;
        return y >= by - s && y < by + MSP_CLOSE_H + s && x >= bx - s && x < bx + 98 + s ? SHC_TAP_RENAME : SHC_TAP_NONE;
    }
    if (y >= sy - s && y < sy + SHC_SEG_H + s && x >= sx - s) return x < sx + sw / 2 ? SHC_TAP_DORMANT : SHC_TAP_PAUSED;
    int by = sy + SHC_SEG_H + SHC_NOTE;
    if (y >= by - s && y < by + MSP_CLOSE_H + s) {
        int bw0 = 120, bw1 = 98, bw2 = 74, gap = (SHC_W - 32 - bw0 - bw1 - bw2) / 2, bx = mx + 16;
        if (x >= bx - s && x < bx + bw0 + gap / 2) return SHC_TAP_SWAP;
        bx += bw0 + gap;
        if (x >= bx - gap / 2 && x < bx + bw1 + gap / 2) return SHC_TAP_RENAME;
        return SHC_TAP_MORE;
    }
    return SHC_TAP_NONE;
}

/* THE LIBRARY: the tank types, a row each - the picture, the name, its one
 * line, EXPERIMENTAL (word + flask) or INSTALLED; GET / NEW TANK on the
 * right with what it costs under it. The wallet bottom left, BACK to the
 * shelves bottom right; LAST CHECKED under the title (the list works with
 * no Wi-Fi). */
static int sh_wrap(ctx_t *c, int x, int y, int maxw, int pitch, int max_lines, uint32_t rgb, const char *s) {
    char line[64]; int n = 0, lines = 0, maxc = maxw / 6;
    if (maxc > 63) maxc = 63;
    while (*s && lines < max_lines) {
        const char *w = s; while (*w == ' ') w++;
        const char *e = w; while (*e && *e != ' ') e++;
        int wl = (int)(e - w);
        if (n && n + 1 + wl > maxc) { line[n] = 0; draw_text(c, x, y, 1, rgb, line); y += pitch; lines++; n = 0; continue; }
        if (n) line[n++] = ' ';
        memcpy(line + n, w, (size_t)wl); n += wl; s = e;
    }
    if (n && lines < max_lines) { line[n] = 0; draw_text(c, x, y, 1, rgb, line); lines++; }
    return lines;
}
void render_library(uint16_t *fb, int stride, const library_row_t *row, int n, int wallet, const char *checked) {
    ctx_t c = ctx_frame(fb, stride);
    rect_fill(&c, 0, 0, TANK_W, TANK_H, MSP_INK);
    draw_text(&c, (TANK_W - text_w("THE LIBRARY", 3)) / 2, SH_TITLE_Y, 3, 0xffffff, "THE LIBRARY");
    draw_text(&c, (TANK_W - text_w(checked, 1)) / 2, SH_TITLE_Y + 27, 1, MSP_QUIET, checked);
    int x0 = SH_LIB_X0, x1 = TANK_W - SH_LIB_X0, bw = 116;
    int y = SH_TITLE_Y + 46, pitch = SH_BOX_H + 22;
    for (int i = 0; i < n && i < 2; i++, y += pitch) {
        const library_row_t *r = &row[i];
        sh_tank(&c, x0, y, SH_TW, SH_IH, r->thumb, false);
        int tx = x0 + SH_TW + 12, tw = x1 - bw - 10 - tx;
        draw_text(&c, tx, y + 2, 2, 0xffffff, r->name);
        int ln = sh_wrap(&c, tx, y + 24, tw, 11, (SH_BOX_H - 38) / 11, MSP_TEAL, r->line);   /* as many lines as the picture is tall */
        int tagy = y + 24 + ln * 11 + 6;
        if (r->experimental) { sh_flask(&c, tx, tagy - 1, MSP_QUIET); draw_text(&c, tx + 10, tagy, 1, MSP_QUIET, "EXPERIMENTAL"); }
        else if (r->installed) draw_text(&c, tx, tagy, 1, MSP_QUIET, "INSTALLED");
        int bx = x1 - bw, by = y + 10;
        const char *act = r->installed ? world_view()->text.new_tank : "GET";
        if (r->price > wallet) {                           /* not enough yet: an outline with dim words (fill, not hue) */
            rect_edge(&c, bx, by, bw, MSP_CLOSE_H, MSP_DIM);
            draw_text(&c, bx + (bw - text_w(act, 2)) / 2, by + (MSP_CLOSE_H - 14) / 2, 2, MSP_DIM, act);
        } else lit_button(&c, bx, by, bw, MSP_CLOSE_H, act);
        if (r->price) {
            char t[16]; snprintf(t, sizeof t, "%d", r->price);
            int w = 16 + 5 + text_w(t, 2), px0 = bx + (bw - w) / 2;
            blit_icon(&c, px0, by + MSP_CLOSE_H + 8, world_view()->art.coin16, 255);
            draw_text(&c, px0 + 21, by + MSP_CLOSE_H + 9, 2, 0xffffff, t);
        } else draw_text(&c, bx + (bw - text_w("FREE", 2)) / 2, by + MSP_CLOSE_H + 9, 2, 0xffffff, "FREE");
        if (i + 1 < n) rect_fill(&c, x0, y + pitch - 11, x1 - x0, 1, MSP_DIM);
    }
    sh_footer(&c, NULL, true, wallet, "BACK", 1, 1);
}
int render_library_tap(float x, float y, int n) {
    int r = render_shelf_tap(x, y);                        /* the footer is the shelf's: BACK stands where CLOSE does */
    if (r == SHELF_TAP_CLOSE) return LIB_TAP_BACK;
    int x1 = TANK_W - SH_LIB_X0, bw = 116, pitch = SH_BOX_H + 22, y0 = SH_TITLE_Y + 46;
    for (int i = 0; i < n && i < 2; i++) {
        int by = y0 + i * pitch + 10;
        if (x >= x1 - bw - 8 && x < x1 + 8 && y >= by - 8 && y < by + MSP_CLOSE_H + 30) return LIB_TAP_ROW0 + i;   /* the button and the price under it */
    }
    return LIB_TAP_NONE;
}
