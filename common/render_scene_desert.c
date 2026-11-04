/* render_scene_desert.c - the desert terrarium's static scene (pocket-terrarium P3a): render_scene_desert.h.
 *
 * Painted row by row into the frame (the scene cache's bake: once per light switch or moisture step). What varies
 * along a column (the strata's sway, the sand's back edge, the wall's streaks) and along a row (the glass, the
 * lid's shade, the lamp's reach) is worked out once; a pixel is then a handful of hashes and multiplies:
 *   outside the glass   black;
 *   the WALL            sandstone in broad strata, each with a lit ledge on top and the next one's shadow under
 *                       its foot, mottled, streaked, darker toward the lid;
 *   the SAND            the floor from the wall's foot to the front glass (the bowl's: down to the glass), darker
 *                       toward the back, wind ripples, grains, pebbles, two succulents at the back, the hides' and
 *                       the rock's contact shadows, and at the humid box's door its moisture (dark and damp, or
 *                       pale with dust when it is dry);
 *   the HIDES           two stone caves domed over an arched door that faces the open floor - warm sandstone at the
 *                       warm end, slate at the cool end - and the humid box: clay, ribbed, a round door, moss on its
 *                       lid and over its sill (green damp, straw-coloured dry), droplets when damp;
 *   the BASKING ROCK    a flat slab, its top lit, its front face in shade;
 *   lit                 the LAMP: its glare at the top of the glass, a warm cone down to the spot, its pool on the
 *                       sand and the rock, the wall warmed round it; the warm end a touch warmer, the cool end bluer;
 *   dark                the lamp out, a moonlit blue over everything;
 * then the night's dim, the vignette and a 4x4 Bayer dither to RGB565, as render.c bake_scene does. */
#include "render_scene_desert.h"
#include "render.h"                 /* FB_SWAP16; tank.h's TANK_*, PLACE_Y, PLACE_MARGIN */
#include <math.h>
#include <string.h>

static const scene_board_t SCENE_BOARD[SCENE_BOARDS] = {
    { "rect",   448, 368, SCENE_GLASS_RECT,      0.0f, 368 },   /* the 1.8 */
    { "round",  466, 466, SCENE_GLASS_ROUND,   233.0f, 396 },   /* the 1.75C pendant: a bowl */
    { "watch",  410, 502, SCENE_GLASS_CORNERS, 100.0f, 502 },   /* the 2.06 watch, portrait */
    { "square", 480, 480, SCENE_GLASS_CORNERS,  40.0f, 480 },   /* the 2.16 square */
};
scene_board_t scene_board_of(int k) { return SCENE_BOARD[k < 0 ? 0 : k >= SCENE_BOARDS ? SCENE_BOARDS - 1 : k]; }
scene_board_t scene_board_here(void) {
#if defined(TANK_ROUND)
    scene_board_t b = { "here", TANK_W, TANK_H, SCENE_GLASS_ROUND, TANK_RAD, TANK_BOT };
#elif defined(TANK_CORNER_R)
    scene_board_t b = { "here", TANK_W, TANK_H, SCENE_GLASS_CORNERS, TANK_CORNER_R, TANK_BOT };
#else
    scene_board_t b = { "here", TANK_W, TANK_H, SCENE_GLASS_RECT, 0.0f, TANK_BOT };
#endif
    return b;
}

static inline float sd_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float sd_corner_inset(float r, float v, float len) {      /* tank.c corner_inset */
    float d = v < r ? r - v : v > len - r ? v - (len - r) : 0;
    if (d <= 0) return 0;
    return d >= r ? r : r - sqrtf(r * r - d * d);
}
float scene_glass_x0(const scene_board_t *b, float y) {
    if (b->glass == SCENE_GLASS_ROUND) { float d = y - b->r, r2 = b->r * b->r - d * d; return b->r - (r2 > 0 ? sqrtf(r2) : 0); }
    if (b->glass == SCENE_GLASS_CORNERS) return sd_corner_inset(b->r, y, (float)b->h);
    return 0;
}
float scene_glass_x1(const scene_board_t *b, float y) { return (float)b->w - scene_glass_x0(b, y); }
static float sd_glass_top(const scene_board_t *b, float x) {    /* the glass above x */
    if (b->glass == SCENE_GLASS_ROUND) { float d = x - b->r, r2 = b->r * b->r - d * d; return b->r - (r2 > 0 ? sqrtf(r2) : 0); }
    if (b->glass == SCENE_GLASS_CORNERS) return sd_corner_inset(b->r, x, (float)b->w);
    return 0;
}
float scene_place_y(const scene_board_t *b) { return (float)b->bot - (float)(TANK_BOT - PLACE_Y); }
float scene_floor_x(const scene_board_t *b, float u) {       /* tank.c floor_span: the glass at PLACE_Y, PLACE_MARGIN in */
    const float py = scene_place_y(b), x0 = scene_glass_x0(b, py) + PLACE_MARGIN, x1 = scene_glass_x1(b, py) - PLACE_MARGIN;
    return x0 + (x1 - x0) * u;
}

/* the hides' sizes by kind: the caves domed with an arched door, the humid box with a round one */
static const struct { float half_w, height, door_hw, door_h; } SD_HIDE[HIDE_KINDS] = {
    { 35.0f, 34.0f, 11.0f, 18.0f },     /* warm: a sandstone cave */
    { 30.0f, 28.0f,  9.0f, 18.0f },     /* humid: a clay box (door_h: where the round hole's middle sits, x 0.52) */
    { 34.0f, 31.0f, 11.0f, 17.0f },     /* cool: a slate cave */
};
#define SD_WALL_DY  92.0f               /* the sand's back edge over TANK_BOT at least: a floor a lizard lives on, not a strip ... */
#define SD_WALL_K   0.24f               /* ... and a quarter of a taller glass (the watch, the square, the bowl) */
#define SD_HIDE_DY  27.0f               /* the hides' foot over TANK_BOT: just behind the back lane (PLACE_Y - 5) */
#define SD_ROCK_HW  30.0f
#define SD_ROCK_DY  21.0f               /* the rock's top, its middle, over TANK_BOT (the middle lane's feet: 16) */
#define SD_W_MAX    512                 /* the widest frame (the square's 480) */

void scene_desert_layout(const scene_board_t *b, const habitat_t *h, scene_desert_t *d) {
    memset(d, 0, sizeof *d);
    const float floor_d = SD_WALL_K * b->h > SD_WALL_DY ? SD_WALL_K * b->h : SD_WALL_DY;
    d->wall_y = b->bot - floor_d; d->sand_y = b->bot - 16.0f;
    const float base = b->bot - SD_HIDE_DY;
    d->n_hides = h->n_hides > HABITAT_HIDES_MAX ? HABITAT_HIDES_MAX : h->n_hides;
    for (int k = 0; k < d->n_hides; k++) {
        scene_hide_t *hd = &d->hide[k];
        const int kind = h->hides[k].kind < HIDE_KINDS ? h->hides[k].kind : HIDE_WARM;
        hd->kind = kind; hd->point_x = scene_floor_x(b, h->hides[k].u);
        hd->half_w = SD_HIDE[kind].half_w; hd->height = SD_HIDE[kind].height;
        hd->door_hw = SD_HIDE[kind].door_hw; hd->door_h = SD_HIDE[kind].door_h;
        hd->base_y = base;
        /* inside the glass at its foot and at its shoulder (the bowl's sides and the corners close in) */
        float g0 = scene_glass_x0(b, base), g1 = scene_glass_x1(b, base);
        float s0 = scene_glass_x0(b, base - hd->height * 0.6f), s1 = scene_glass_x1(b, base - hd->height * 0.6f);
        float lo = (g0 > s0 ? g0 : s0) + hd->half_w + 4, hi = (g1 < s1 ? g1 : s1) - hd->half_w - 4;
        hd->cx = lo > hi ? 0.5f * (lo + hi) : sd_clamp(hd->point_x, lo, hi);
        hd->opens = h->hides[k].u < 0.5f ? 1 : -1;                                  /* toward the open floor */
        hd->door_x = hd->cx + hd->opens * (kind == HIDE_HUMID ? 0.30f : 0.34f) * hd->half_w;
        hd->door_y = base;
    }
    d->bask_x = scene_floor_x(b, h->bask_u);
    d->rock_x0 = d->bask_x - SD_ROCK_HW; d->rock_x1 = d->bask_x + SD_ROCK_HW;
    d->rock_top = b->bot - SD_ROCK_DY; d->rock_lift = d->sand_y - d->rock_top;
}
float scene_desert_lift(const scene_desert_t *d, float x) {
    const float half = 0.5f * (d->rock_x1 - d->rock_x0), off = fabsf(x - 0.5f * (d->rock_x0 + d->rock_x1));
    if (off <= half - 10) return d->rock_lift;
    if (off >= half + 4) return 0;
    return d->rock_lift * (half + 4 - off) / 14.0f;
}
void scene_desert_peek_at(const scene_desert_t *d, int k, float half_len, float peek, float *x, float *floor_y, float *yaw) {
    const scene_hide_t *hd = &d->hide[k < 0 ? 0 : k >= d->n_hides ? d->n_hides - 1 : k];
    const float f = (float)hd->opens, cut = hd->door_x - f * hd->door_hw * 0.35f;   /* where it leaves the dark: just inside the door */
    *x = cut - f * half_len * (1 - 2 * peek);
    *floor_y = hd->door_y;
    *yaw = f;
}
int scene_desert_moist_step(float m) { return (int)(sd_clamp(m, 0, 1) * 8 + 0.5f); }

/* ---- painting ---- */
typedef struct { float r, g, b; } sd_rgb_t;
static inline sd_rgb_t sd_hex(uint32_t c) { sd_rgb_t o = { (float)(c >> 16 & 255), (float)(c >> 8 & 255), (float)(c & 255) }; return o; }
static inline sd_rgb_t sd_mix(sd_rgb_t a, sd_rgb_t b, float p) { sd_rgb_t o = { a.r + (b.r - a.r) * p, a.g + (b.g - a.g) * p, a.b + (b.b - a.b) * p }; return o; }
static inline sd_rgb_t sd_mul(sd_rgb_t a, float k) { sd_rgb_t o = { a.r * k, a.g * k, a.b * k }; return o; }
static inline uint32_t sd_hash(int a, int b) {
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}
static float sd_noise(float x, int salt) {                       /* a smooth noise along x, 0..1 */
    int i = (int)floorf(x); float f = x - i;
    float a = (float)(sd_hash(i, salt) & 1023) / 1023.0f, b = (float)(sd_hash(i + 1, salt) & 1023) / 1023.0f;
    f = f * f * (3 - 2 * f);
    return a + (b - a) * f;
}

#define SD_PEBBLES 22
#define SD_PLANTS  2
#define SD_BLOT    24                   /* the wall's blotches: a smooth noise on a 24 px grid */
typedef struct { float x, y, irx, iry; int tone, y0, y1; } sd_pebble_t;
typedef struct {
    const scene_board_t *b;
    scene_desert_t d;
    float moist[HABITAT_HIDES_MAX];
    float lamp_x, lamp_r, lamp_top;
    sd_pebble_t peb[SD_PEBBLES]; int n_peb;
    float plant_x[SD_PLANTS], plant_y[SD_PLANTS];
    float sint[64];                     /* a sine, 64 steps a turn: the strata's edges */
} sd_ctx_t;

/* the wall: strata SD_LAYER deep on average, each edge its own gentle wave (two of the sine's harmonics, its own
   phase), a lit ledge along each edge and the shadow under the next; blotches, mottle and grit over it */
static const uint32_t SD_STONE[4] = { 0x845a3e, 0x92664a, 0x7a5440, 0x9c6e4e };
#define SD_LAYER 30
static inline float sd_strata_edge(const sd_ctx_t *c, int li, int x) {      /* layer li's top at column x */
    const uint32_t h = sd_hash(li, 23);
    return li * SD_LAYER + (float)(h & 15) - 7.5f + 3.2f * c->sint[(x / 3 + (int)(h >> 8)) & 63] + 1.3f * c->sint[(x * 2 / 3 + (int)(h >> 16)) & 63];
}
static sd_rgb_t sd_wall(const sd_ctx_t *c, int x, int y, float ly, float blot) {
    int li = (int)floorf(ly / SD_LAYER);
    float b0 = sd_strata_edge(c, li, x), b1 = sd_strata_edge(c, li + 1, x);
    if (ly < b0) { b1 = b0; li--; b0 = sd_strata_edge(c, li, x); }
    else if (ly >= b1) { b0 = b1; li++; b1 = sd_strata_edge(c, li + 1, x); }
    const uint32_t lh = sd_hash(li, 17);
    float k = (0.92f + 0.12f * (float)((lh >> 4) & 255) / 255.0f) * blot;
    const float top = ly - b0, foot = b1 - ly;
    if (top < 1.0f) k *= 1.16f;                                                 /* the ledge's lit edge */
    else if (foot < 2.0f) k *= 0.72f;                                           /* the next ledge's shadow */
    else if (foot < 3.5f) k *= 0.87f;
    const uint32_t m = sd_hash(x >> 2, y >> 1);                                  /* mottle */
    k *= 0.95f + 0.08f * (float)(m & 255) / 255.0f;
    const uint32_t g = sd_hash(x, y + 7777);                                    /* grit */
    if ((g & 31) == 0) k *= 0.85f; else if ((g & 31) == 1) k *= 1.09f;
    return sd_mul(sd_hex(SD_STONE[lh & 3]), k);
}
/* a succulent: an echeveria's rosette from the side, a dozen px across, its leaves pointed and pink at the tips */
static bool sd_plant(float px, float py, float cx, float cy, sd_rgb_t *out) {
    static const float SA[7] = { 0.0f, -0.5227f, 0.5227f, -0.8674f, 0.8674f, -0.9927f, 0.9927f };   /* sin, cos of each leaf's lean: */
    static const float CA[7] = { 1.0f, 0.8525f, 0.8525f, 0.4976f, 0.4976f, 0.1205f, 0.1205f };       /* 0, +-0.55, +-1.05, +-1.45 rad */
    static const float LEN[7] = { 12.0f, 11.5f, 11.5f, 10.5f, 10.5f, 9.0f, 9.0f };
    const float dx = px - cx, dy = py - cy;
    if (dx < -13 || dx > 13 || dy < -13 || dy > 2.0f) return false;
    for (int k = 6; k >= 0; k--) {                                               /* the outer, lower leaves are the nearer */
        const float u = dx * SA[k] - dy * CA[k], v = dx * CA[k] + dy * SA[k], L = LEN[k];
        if (u < 0 || u > L) continue;
        float q = u / L, w = 3.0f * (q < 0.35f ? q / 0.35f : (1 - q) / 0.65f) + 0.4f;
        if (fabsf(v) > w) continue;
        sd_rgb_t c = sd_hex(k < 1 ? 0xa6cc98 : k < 3 ? 0x8aba84 : k < 5 ? 0x6ea06c : 0x588658);
        if (q > 0.84f) c = sd_hex(0xd8889a);
        else if (v > w - 0.9f) c = sd_mul(c, 0.80f);
        else if (v < -w + 0.9f) c = sd_mul(c, 1.10f);
        *out = c; return true;
    }
    return false;
}
/* a stone cave: a lumpy dome, lit from above and the lamp's side, strata and speckle, an arched door with its dark */
static bool sd_cave(const sd_ctx_t *c, const scene_hide_t *hd, int x, int y, sd_rgb_t *out) {
    const float px = x + 0.5f, py = y + 0.5f;
    const float dx = (px - hd->cx) / hd->half_w, dy = (hd->base_y - py) / hd->height;
    if (dy < -0.02f || dy > 1.12f || dx < -1.12f || dx > 1.12f) return false;
    const int salt = hd->kind * 19 + 3;
    const float rr = 1.0f + 0.10f * (sd_noise(dx * 4.0f + 9, salt) - 0.5f) + 0.06f * (sd_noise(dx * 11.0f, salt + 1) - 0.5f);
    const float e = dx * dx + dy * dy * (0.86f + 0.14f * dy);                   /* a dome, a little flat on top */
    if (e > rr * rr) return false;
    const bool cool = hd->kind == HIDE_COOL;
    static const uint32_t WARM[5] = { 0x3e1e14, 0x6e3622, 0x9a5132, 0xc07246, 0xdc9868 };
    static const uint32_t COOL[5] = { 0x262a32, 0x40485a, 0x5f6a7c, 0x84909e, 0xadb6c0 };
    const uint32_t *T = cool ? COOL : WARM;
    const float ddx = (px - hd->door_x) / hd->door_hw, ddy = (hd->base_y - py) / hd->door_h;   /* the door: an arch from the sand */
    if (ddy >= -0.02f && ddy < 1.0f && ddx * ddx + ddy * ddy * ddy * ddy < 1.0f) {
        float k = ddx * ddx + ddy * ddy;                                       /* darker the deeper in */
        sd_rgb_t in = sd_mix(sd_hex(0x0e0806), sd_hex(cool ? 0x2a2e36 : 0x3a2016), sd_clamp(k * 0.9f, 0, 1));
        if (ddy < 0.12f) in = sd_mix(in, sd_hex(0x6a5038), 0.35f * (1 - fabsf(ddx)));   /* the sand inside, faintly */
        *out = in; return true;
    }
    float nz2 = 1 - dx * dx - dy * dy; float nz = nz2 > 0 ? sqrtf(nz2) : 0;
    const float side = c->lamp_x > hd->cx ? 1.0f : -1.0f;
    float lum = 0.62f * dy + 0.30f * side * dx + 0.45f * nz;
    if (e > rr * rr * 0.86f) lum -= 0.25f;                                      /* the rim turning away */
    const uint32_t h = sd_hash(x, y * 3 + salt);
    lum += ((float)(h & 255) / 255.0f - 0.5f) * 0.14f;
    const float strata = (hd->base_y - py) + 2.0f * sinf(px * 0.21f + salt);    /* layers across it */
    const float sf = strata / 6.5f - floorf(strata / 6.5f);
    if (sf < 0.16f) lum -= 0.20f;
    int t = lum < 0.10f ? 0 : lum < 0.35f ? 1 : lum < 0.60f ? 2 : lum < 0.85f ? 3 : 4;
    float lx = (px - hd->door_x) / (hd->door_hw + 2.5f), ly = (hd->base_y - py) / (hd->door_h + 2.5f);
    if (ly > 0.35f && lx * lx + ly * ly * ly * ly < 1.0f) t = t < 3 ? 3 : t;   /* the lip over the door, lit */
    if (e > rr * rr * 0.93f && dy > 0.04f) t = t > 0 ? t - 1 : 0;              /* its silhouette a step down */
    *out = sd_hex(T[t]);
    return true;
}
/* the humid box: clay, ribbed, a round door with moss over its sill, moss on the lid, droplets when damp */
static bool sd_box(const sd_ctx_t *c, const scene_hide_t *hd, float m, int x, int y, sd_rgb_t *out) {
    const float px = x + 0.5f, py = y + 0.5f, top = hd->base_y - hd->height;
    const float ax = fabsf(px - hd->cx);
    const sd_rgb_t moss_lo = m > 0.25f ? sd_hex(0x3f7a2e) : sd_hex(0x7a7240), moss_hi = m > 0.25f ? sd_hex(0x78b84c) : sd_hex(0xa89c5c);
    if (ax < hd->half_w - 3 && py < top + 1.5f) {                                /* moss tufts along the lid */
        float tuft = 1.0f + 3.2f * sd_noise((px - hd->cx) * 0.33f + 5, 77) * sd_noise((px - hd->cx) * 0.11f + 2, 78);
        if (py >= top - tuft) { *out = (sd_hash(x, y) & 3) == 0 ? moss_hi : sd_mix(moss_lo, moss_hi, (top - py) / (tuft + 0.01f)); return true; }
    }
    if (ax > hd->half_w || py < top || py > hd->base_y + 0.5f) return false;
    const float rc = 7.0f;                                                      /* the lid's round corners */
    if (py < top + rc && ax > hd->half_w - rc) {
        float qx = ax - (hd->half_w - rc), qy = top + rc - py;
        if (qx * qx + qy * qy > rc * rc) return false;
    }
    const float hy = hd->base_y - hd->door_h * 0.52f, hx = hd->door_x;          /* the door: a round hole */
    const float ddx = px - hx, ddy = py - hy, dd = ddx * ddx + ddy * ddy, R = hd->door_hw;
    if (dd < R * R) {
        if (ddy > R * 0.35f - 2.2f * sd_noise(px * 0.5f, 79)) { *out = (sd_hash(x, y + 9) & 3) ? moss_lo : moss_hi; return true; }
        *out = sd_mix(sd_hex(0x0e0a08), sd_hex(0x2c1a12), sd_clamp(dd / (R * R), 0, 1));
        return true;
    }
    static const uint32_t CLAY[5] = { 0x4a2416, 0x7a3c24, 0xa25a38, 0xc07650, 0xd89a72 };
    float lum = 0.55f;
    if (py < top + 2.5f) lum = 0.92f;                                           /* the lid's lit rim */
    else if (py < top + 5) lum = 0.72f;
    const float sx = ax / hd->half_w;
    if (sx > 0.80f) lum -= 0.22f * (sx - 0.80f) / 0.20f + 0.08f;               /* rounding away at the sides */
    if ((px < hd->cx) == (c->lamp_x < hd->cx) && sx > 0.6f) lum += 0.10f;      /* the side toward the lamp */
    const float rib = (hd->base_y - py) / 6.0f - floorf((hd->base_y - py) / 6.0f);
    if (rib < 0.17f && py > top + 4) lum -= 0.18f; else if (rib < 0.34f && py > top + 4) lum += 0.06f;
    if (dd < (R + 1.6f) * (R + 1.6f)) lum = ddy < 0 ? lum - 0.25f : lum + 0.12f;   /* the hole's rim */
    if (py > hd->base_y - 2) lum -= 0.14f;
    const uint32_t h = sd_hash(x * 7, y);
    lum += ((float)(h & 255) / 255.0f - 0.5f) * 0.08f;
    int t = lum < 0.18f ? 0 : lum < 0.40f ? 1 : lum < 0.62f ? 2 : lum < 0.84f ? 3 : 4;
    sd_rgb_t s = sd_hex(CLAY[t]);
    if (m >= 0.35f && (h >> 9 & 63) < (uint32_t)(m * 5)) s = sd_hex(0xdcf2ff);  /* droplets */
    else if (m >= 0.35f) s = sd_mul(s, 0.90f);                                   /* the clay darker, damp */
    *out = s;
    return true;
}
/* the basking rock: a flat slab, its top face lit, its front face in shade */
static bool sd_rock(const sd_ctx_t *c, int x, int y, sd_rgb_t *out) {
    const float px = x + 0.5f, py = y + 0.5f, cx = c->d.bask_x, ty = c->d.rock_top;
    const float rx = SD_ROCK_HW * (1.0f + 0.10f * (sd_noise(px * 0.15f, 61) - 0.5f)), ry = 5.5f;
    const float dx = (px - cx) / rx;
    if (dx < -1 || dx > 1) return false;
    const float half = ry * sqrtf(1 - dx * dx);
    const float front = c->b->bot - 11.0f - 1.5f * dx * dx;                     /* the front face's foot */
    if (py < ty - half - 0.3f || py > front) return false;
    static const uint32_t SLATE[5] = { 0x3a3230, 0x564a44, 0x766860, 0x988878, 0xbca894 };
    const uint32_t h = sd_hash(x * 5, y * 11 + 3);
    if (py <= ty + half) {
        float lum = 0.70f - 0.25f * fabsf(dx) + ((float)(h & 255) / 255.0f - 0.5f) * 0.12f;
        if (py > ty + half - 1.2f) lum += 0.12f;                                /* its front rim catches the light */
        if (py < ty - half + 1.0f) lum -= 0.20f;
        int t = lum < 0.38f ? 1 : lum < 0.60f ? 2 : lum < 0.80f ? 3 : 4;
        *out = sd_hex(SLATE[t]);
    } else {
        float k = (py - (ty + half)) / (front - (ty + half) + 0.01f);
        int t = k < 0.35f ? 2 : k < 0.8f ? 1 : 0;
        if ((h & 15) == 0) t = t > 0 ? t - 1 : 0;
        *out = sd_hex(SLATE[t]);
    }
    return true;
}
/* the sand at (x, y): its colour across and in depth, ripples, grains, the row's pebbles; the contact shadows and
   the humid door's moisture only on the rows they reach */
static sd_rgb_t sd_sand(const sd_ctx_t *c, int x, int y, float depth, int rip, int npeb, const int *peb, bool shade_row, bool moist_row) {
    const float u = (float)x / c->b->w;
    sd_rgb_t s = { 236.0f - 18.0f * u, 196.0f, 142.0f + 26.0f * u };           /* warm end 0xecc48e .. cool end 0xdac4a8 */
    float k = 0.80f + 0.20f * depth;                                            /* the back in the wall's shade */
    if (((y * 3 + rip) & 15) < 2) k *= 0.94f;                                    /* wind ripples */
    const uint32_t h = sd_hash(x * 3 + 1, y * 5 + 2);
    switch (h & 15) {
    case 0: case 1: k *= 0.90f; break;
    case 2: k *= 1.07f; break;
    case 3: k *= 0.95f; break;
    case 4: if ((h >> 8 & 7) == 0) k *= 0.80f; break;
    default: break;
    }
    s = sd_mul(s, k);
    const float px = x + 0.5f, py = y + 0.5f;
    for (int j = 0; j < npeb; j++) {
        const sd_pebble_t *p = &c->peb[peb[j]];
        float ex = (px - p->x) * p->irx, ey = (py - p->y) * p->iry;
        if (ex * ex + ey * ey > 1) continue;
        static const uint32_t PEB[3] = { 0x9a8676, 0x8c7a6a, 0xb09a84 };
        s = sd_hex(PEB[p->tone]);
        if (ey < -0.3f && ex < 0.2f) s = sd_mul(s, 1.22f); else if (ey > 0.45f) s = sd_mul(s, 0.72f);
    }
    if (shade_row) {                                                            /* contact shadows: the hides, the rock */
        float shade = 0;
        for (int j = 0; j < c->d.n_hides; j++) {
            const scene_hide_t *hd = &c->d.hide[j];
            float ex = (px - hd->cx) / (hd->half_w + 7), ey = (py - hd->base_y - 1) * (1.0f / 6.0f), e = ex * ex + ey * ey;
            if (e < 1) shade += 0.42f * (1 - e);
        }
        float ex = (px - c->d.bask_x) * (1.0f / (SD_ROCK_HW + 6)), ey = (py - (c->b->bot - 11.0f)) * 0.2f, e = ex * ex + ey * ey;
        if (e < 1) shade += 0.38f * (1 - e);
        if (shade > 0) s = sd_mul(s, 1 - sd_clamp(shade, 0, 0.55f));
    }
    if (moist_row)
        for (int j = 0; j < c->d.n_hides; j++) {                                /* the humid box's door: damp, or dusty */
            const scene_hide_t *hd = &c->d.hide[j];
            if (hd->kind != HIDE_HUMID) continue;
            float ex = (px - hd->door_x) * (1.0f / 22.0f), ey = (py - hd->base_y - 4) * (1.0f / 7.0f), e = ex * ex + ey * ey;
            if (e >= 1) continue;
            float edge = (1 - e) * (0.75f + 0.25f * (float)(sd_hash(x >> 1, y) & 255) / 255.0f);
            const float m = c->moist[j];
            if (m > 0.02f) s = sd_mix(s, sd_hex(0x6e5038), sd_clamp(edge * 1.3f, 0, 1) * 0.85f * m);
            else s = sd_mix(s, sd_hex(0xf0e4cc), edge * 0.50f);
        }
    return s;
}

void scene_desert_draw(uint16_t *fb, int stride, const scene_board_t *b, const habitat_t *h, bool lit, const float *moisture,
                       float dim, const uint8_t *vig) {
    static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    sd_ctx_t c; memset(&c, 0, sizeof c);
    c.b = b;
    scene_desert_layout(b, h, &c.d);
    for (int k = 0; k < c.d.n_hides; k++) c.moist[k] = (float)scene_desert_moist_step(moisture ? moisture[k] : h->moist_start) / 8.0f;
    c.lamp_x = c.d.bask_x; c.lamp_r = h->bask_r_px > 0 ? h->bask_r_px : 70; c.lamp_top = sd_glass_top(b, c.lamp_x);
    for (int k = 0; k < 64; k++) c.sint[k] = sinf((float)k * 6.2831853f / 64);
    const float wall_y = c.d.wall_y, sand_y = c.d.sand_y;
    /* the pebbles, off the hides and the rock; two succulents at the back */
    for (int k = 0; k < SD_PEBBLES; k++) {
        const uint32_t ph = sd_hash(k, 4242);
        sd_pebble_t *p = &c.peb[c.n_peb];
        p->x = 8 + (float)(ph & 1023) / 1023.0f * (b->w - 16);
        p->y = wall_y + 8 + (float)((ph >> 10) & 255) / 255.0f * (b->bot - wall_y - 12);
        const float rx = 1.6f + (float)((ph >> 18) & 3) * 0.7f;
        p->irx = 1 / rx; p->iry = 1 / (rx * 0.62f); p->tone = (int)((ph >> 21) % 3);
        bool off = p->y > c.d.rock_top - 8 && p->y < b->bot - 8 && p->x > c.d.rock_x0 - 6 && p->x < c.d.rock_x1 + 6;
        for (int j = 0; j < c.d.n_hides && !off; j++) off = fabsf(p->x - c.d.hide[j].cx) < c.d.hide[j].half_w + 8 && p->y > c.d.hide[j].base_y - c.d.hide[j].height - 4 && p->y < c.d.hide[j].base_y + 9;
        if (off) continue;
        p->y0 = (int)floorf(p->y - rx * 0.62f); p->y1 = (int)ceilf(p->y + rx * 0.62f);
        c.n_peb++;
    }
    static const float PLANT_U[SD_PLANTS] = { 0.41f, 0.80f };
    for (int k = 0; k < SD_PLANTS; k++) { c.plant_x[k] = scene_floor_x(b, PLANT_U[k]); c.plant_y[k] = wall_y + 16 + 4 * k; }
    /* along each column: the strata's sway, the sand's back edge, the ripples' phase */
    int16_t sway[SD_W_MAX], edge[SD_W_MAX]; int8_t rip[SD_W_MAX];
    const int W = b->w < SD_W_MAX ? b->w : SD_W_MAX;
    for (int x = 0; x < W; x++) {
        const float fx = (float)x;
        sway[x] = (int16_t)(8 * (10.0f * (sd_noise(fx * 0.008f, 1) - 0.5f) + 2.0f * sinf(fx * 0.031f + 1.3f)));
        edge[x] = (int16_t)(8 * (wall_y - 5.0f * sd_noise(fx * 0.018f, 5) - 1.2f * sd_noise(fx * 0.25f, 33)));
        rip[x] = (int8_t)(int)(4.0f * sinf(fx * 0.07f) + 3.0f * sinf(fx * 0.023f + 2));
    }
    float blot[SD_W_MAX / SD_BLOT + 3];                                          /* the wall's blotches along this row, at the grid's columns */
    const int nb = W / SD_BLOT + 2;
    int shadow_y0 = b->bot - 18, shadow_y1 = b->bot - 5;                          /* the rows a contact shadow reaches */
    for (int k = 0; k < c.d.n_hides; k++) if (c.d.hide[k].base_y - 6 < shadow_y0) shadow_y0 = (int)(c.d.hide[k].base_y - 6);
    const sd_rgb_t LAMP = sd_hex(0xffd59a), NIGHT = sd_hex(0x182238), GLARE = sd_hex(0xfff2d4);
    for (int y = 0; y < b->h; y++) {
        uint16_t *row = fb + (size_t)y * stride;
        const float fy = y + 0.5f;
        int gx0, gx1;                                                           /* the glass along this row */
        if (b->glass == SCENE_GLASS_ROUND) {
            float d = fy - b->r, r2 = b->r * b->r - d * d;
            if (r2 <= 0) { gx0 = 1; gx1 = 0; }
            else { float hw = sqrtf(r2); gx0 = (int)ceilf(b->r - hw - 0.5f); gx1 = (int)floorf(b->r + hw - 0.5f); }
        } else { float in = scene_glass_x0(b, fy); gx0 = (int)ceilf(in - 0.5f); gx1 = (int)floorf(b->w - in - 0.5f); }
        if (gx0 < 0) gx0 = 0;
        if (gx1 > W - 1) gx1 = W - 1;
        for (int x = 0; x < gx0 && x < b->w; x++) row[x] = 0;
        for (int x = gx1 + 1 > 0 ? gx1 + 1 : 0; x < b->w; x++) row[x] = 0;
        /* the row's own: the lid's shade, the floor's depth, the wall's blotches, the lamp's reach, what stands in it */
        const float lid = sd_clamp(fy / (wall_y * 0.8f), 0, 1), lid_k = 0.52f + 0.48f * lid * (2 - lid);
        const float depth = sd_clamp((fy - wall_y) / (b->bot - wall_y), 0, 1);
        if (fy < wall_y + 8) {
            const int j = y / SD_BLOT; float f = (float)(y % SD_BLOT) / SD_BLOT; f = f * f * (3 - 2 * f);
            for (int k = 0; k < nb; k++) {
                float a = (float)(sd_hash(k, j * 7 + 3) & 1023) / 1023.0f, bb = (float)(sd_hash(k, j * 7 + 10) & 1023) / 1023.0f;
                blot[k] = 0.86f + 0.28f * (a + (bb - a) * f);
            }
        }
        const float wdy = (fy - (wall_y - 70)) * (1.0f / 160.0f), wdy2 = wdy * wdy;  /* the lamp's spot on the wall */
        const float pdy = (fy - (sand_y - 6)) * (1.0f / 28.0f), pdy2 = pdy * pdy;    /* ... its pool on the floor */
        const float beam_t = (fy - c.lamp_top) / (sand_y - c.lamp_top);
        const float beam_hw = 12 + (fy - c.lamp_top) * 0.28f, beam_k = beam_t > 0 && beam_t < 1 ? 0.075f * (1 - 0.5f * beam_t) : 0;
        const float gdy = (fy - c.lamp_top - 2) * (1.0f / 8.0f), gdy2 = gdy * gdy;   /* ... its glare at the top of the glass */
        const float ilr = 1.0f / (c.lamp_r * 1.2f);
        int npeb = 0, peb[SD_PEBBLES];
        for (int k = 0; k < c.n_peb; k++) if (y >= c.peb[k].y0 && y <= c.peb[k].y1) peb[npeb++] = k;
        bool hide_row = false, moist_row = false;
        for (int k = 0; k < c.d.n_hides; k++) {
            if (fy > c.d.hide[k].base_y - c.d.hide[k].height - 6 && fy < c.d.hide[k].base_y + 1) hide_row = true;
            if (c.d.hide[k].kind == HIDE_HUMID && fy > c.d.hide[k].base_y - 3 && fy < c.d.hide[k].base_y + 11) moist_row = true;
        }
        const bool shade_row = y >= shadow_y0 && y <= shadow_y1;
        const bool rock_row = fy > c.d.rock_top - 7 && fy < b->bot - 9;
        bool plant_row = false;
        for (int k = 0; k < SD_PLANTS; k++) if (fy > c.plant_y[k] - 13 && fy < c.plant_y[k] + 2) plant_row = true;
        for (int x = gx0; x <= gx1; x++) {
            const float fx = x + 0.5f;
            const bool sand = fy * 8 >= edge[x];
            sd_rgb_t s;
            if (sand) {
                s = sd_sand(&c, x, y, depth, rip[x], npeb, peb, shade_row, moist_row);
                if (fy * 8 < edge[x] + 20) s = sd_mul(s, 0.80f);                  /* the wall's foot */
            } else {
                const int kb = x / SD_BLOT; float f = (float)(x % SD_BLOT) / SD_BLOT; f = f * f * (3 - 2 * f);
                s = sd_mul(sd_wall(&c, x, y, fy + sway[x] * 0.125f, blot[kb] + (blot[kb + 1] - blot[kb]) * f), lid_k);
            }
            sd_rgb_t o; bool prop = false;
            if (hide_row)
                for (int k = 0; k < c.d.n_hides && !prop; k++) {
                    const scene_hide_t *hd = &c.d.hide[k];
                    if (fx < hd->cx - hd->half_w - 3 || fx > hd->cx + hd->half_w + 3) continue;
                    prop = hd->kind == HIDE_HUMID ? sd_box(&c, hd, c.moist[k], x, y, &o) : sd_cave(&c, hd, x, y, &o);
                }
            if (!prop && rock_row && fx > c.d.rock_x0 - 4 && fx < c.d.rock_x1 + 4) prop = sd_rock(&c, x, y, &o);
            if (!prop && plant_row)
                for (int k = 0; k < SD_PLANTS && !prop; k++) prop = sd_plant(fx, fy, c.plant_x[k], c.plant_y[k], &o);
            if (prop) s = o;
            if (lit) {
                const float ldx = fx - c.lamp_x;
                float gain = 0;
                if (!sand) { float gx = ldx * (1.0f / 110.0f), d = gx * gx + wdy2; if (d < 1) gain += 0.62f * (1 - d) * (1 - d); }
                if (sand || prop) { float gx = ldx * ilr, d = gx * gx + pdy2; if (d < 1) gain += 0.72f * (1 - d) * (1 - d); }
                if (gain > 0) { s.r *= 1 + gain; s.g *= 1 + 0.80f * gain; s.b *= 1 + 0.42f * gain; }
                const float ax = fabsf(ldx);
                if (beam_k > 0 && ax < beam_hw) { float k = 1 - ax / beam_hw; s = sd_mix(s, LAMP, k * k * beam_k); }
                if (gdy2 < 1) { float gx = ldx * (1.0f / 20.0f), d = gx * gx + gdy2; if (d < 1) s = sd_mix(s, GLARE, 0.75f * (1 - d) * (1 - d)); }
                const float t = 1 - 2 * fx / b->w;                              /* the warm end warmer, the cool end bluer */
                s.r *= 1 + 0.07f * t; s.g *= 1 + 0.01f * t; s.b *= 1 - 0.09f * t;
            } else s = sd_mix(sd_mul(s, 0.80f), NIGHT, 0.30f);                 /* the lamp out: moonlit */
            int r = (int)(s.r * dim), g = (int)(s.g * dim), bl = (int)(s.b * dim);
            if (vig) { int inv = 256 - vig[(size_t)y * b->w + x]; r = r * inv >> 8; g = g * inv >> 8; bl = bl * inv >> 8; }
            const int dd = BAYER[y & 3][x & 3];
            int r5 = (r + (dd >> 1)) >> 3, g6 = (g + (dd >> 2)) >> 2, b5 = (bl + (dd >> 1)) >> 3;
            r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5; g6 = g6 < 0 ? 0 : g6 > 63 ? 63 : g6; b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
            row[x] = FB_SWAP16((uint16_t)((r5 << 11) | (g6 << 5) | b5));
        }
    }
}
