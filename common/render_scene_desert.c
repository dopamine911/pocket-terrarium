/* render_scene_desert.c - the desert terrarium's static scene (pocket-terrarium P3a, P2c): render_scene_desert.h.
 *
 * Painted row by row into the frame (the scene cache's bake: once per light switch or moisture step). What varies
 * along a column (the strata's sway, the sand's back edge, the wall's streaks) and along a row (the glass, the
 * lid's shade, the lamp's reach, the ground's pieces in it) is worked out once; a pixel is then a handful of hashes and
 * multiplies:
 *   outside the glass   black;
 *   the WALL            sandstone in broad strata, each with a lit ledge on top and the next one's shadow under
 *                       its foot, mottled, streaked, darker toward the lid; the ground's soft shadow on it, away from
 *                       the lamp, and under a shelf;
 *   the SAND            the floor from the wall's foot to the front glass (the bowl's: down to the glass), darker
 *                       toward the back, wind ripples, grains, pebbles, the hides' and the terraces' contact shadows, and
 *                       at the humid box's door its moisture (dark and damp, or pale with dust when it is dry);
 *   the GROUND          (P2c: the habitat's terrain, terrain.h - where the engine walks its lizards) each rock a sandstone
 *                       TERRACE standing on the sand - its top face lit where the feet stand, its front face in layers,
 *                       each a little wider than the one above, a seam of shadow between - or a SHELF on the back wall
 *                       (rock_t.shelf: a slab of the same stone, its underside dark and the wall under it in its
 *                       shadow); each ramp a flagstone SLAB from one surface's edge up to the other's, its foot sunk in
 *                       the sand; of two pieces over a pixel the one standing lower on the glass is in front;
 *   the HIDES           two stone caves domed over an arched door that faces the open floor - warm sandstone at the
 *                       warm end, slate at the cool end - and the humid box: clay, ribbed, a round door, moss on its
 *                       lid and over its sill (green damp, straw-coloured dry), droplets when damp; in front of the ground;
 *   two SUCCULENTS      one on the basking rock's free end, one at the back of the open sand;
 *   lit                 the LAMP over the basking spot on the high rock: its glare at the top of the glass, a warm cone
 *                       down to the spot, its pool on the rock's top, the wall warmed round it; the warm end a touch
 *                       warmer, the cool end bluer;
 *   dark                the lamp out, a moonlit blue over everything;
 * then the night's dim, the vignette and a 4x4 Bayer dither to RGB565, as render.c bake_scene does. */
#include "render_scene_desert.h"
#include "render.h"                 /* FB_SWAP16 */
#include <math.h>
#include <string.h>

static inline float sd_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint32_t sd_hash(int a, int b) {
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}
static inline float sd_hash01(int a, int b) { return (float)(sd_hash(a, b) & 1023) / 1023.0f; }
static float sd_noise(float x, int salt) {                       /* a smooth noise along x, 0..1 */
    int i = (int)floorf(x); float f = x - i;
    float a = (float)(sd_hash(i, salt) & 1023) / 1023.0f, b = (float)(sd_hash(i + 1, salt) & 1023) / 1023.0f;
    f = f * f * (3 - 2 * f);
    return a + (b - a) * f;
}

/* the hides' sizes by kind: the caves domed with an arched door, the humid box with a round one */
static const struct { float half_w, height, door_hw, door_h; } SD_HIDE[HIDE_KINDS] = {
    { 35.0f, 34.0f, 11.0f, 18.0f },     /* warm: a sandstone cave */
    { 30.0f, 28.0f,  9.0f, 18.0f },     /* humid: a clay box (door_h: where the round hole's middle sits, x 0.52) */
    { 34.0f, 31.0f, 11.0f, 17.0f },     /* cool: a slate cave */
};
#define SD_WALL_DY   92.0f              /* the sand's back edge over TANK_BOT at least: a floor a lizard lives on, not a strip ... */
#define SD_WALL_K    0.24f              /* ... and a quarter of a taller glass (the watch, the square, the bowl) */
#define SD_HIDE_DY   27.0f              /* the hides' foot over TANK_BOT: just behind the back lane (PLACE_Y - 5) */
#define SD_W_MAX     512                /* the widest frame (the square's 480) */
/* the ground's pieces */
#define SD_TOP_HW    5.0f               /* a rock's top face: this far either side of the feet's line (seen a little from above) */
#define SD_SLAB_HW   4.0f               /* ... a slab's */
#define SD_SLAB_FACE 9.0f               /* a slab's front face under its top */
#define SD_LIP       14.0f              /* a rock's top past either end of its walking line */
#define SD_SHELF_D   18.0f              /* a shelf's front face, its top face to its underside */
#define SD_ROCK_FOOT 31.0f              /* a terrace's foot over the glass's bottom: just behind the hides (SD_HIDE_DY) */
#define SD_EXT_MAX   12.0f              /* a bed's edge never stands further past the top face's (the rows' bounds) */

/* ---- the ground's pieces ----
 * A rock's front face is BEDS of sandstone under its top face, 6..13 px each, their seams swaying a little along x: a
 * HARD bed stands out past the one above (lighter, its top catching the light), a SOFT one weathers back (darker, in the
 * shadow of the bed over it) - so its sides are ribbed, not stepped - widening a little toward the sand; a few cracks
 * run down a few beds. A shelf is two or three beds, rounding off under toward the wall. A slab's top is its walking
 * line's, its face under it a ragged edge of flagstone. The beds, their edges and the cracks are laid out once
 * (scene_desert_layout); a pixel looks its bed up and sways it by the small tables below */
static float   SD_SWAY[256];            /* a seam's sway along x: smooth, periodic over 256 px, within +-2.5 */
static float   SD_WOB[64];              /* a bed's edge's wobble down y: smooth, periodic over 64 px */
static uint8_t SD_MOT[64][64];          /* the stone's mottle: smooth, periodic over 64 px both ways */
static bool    sd_tables;
static void sd_tables_make(void) {
    if (sd_tables) return;
    const float T = 6.2831853f;
    for (int i = 0; i < 256; i++) SD_SWAY[i] = 1.2f * sinf(T * 3 * i / 256 + 0.4f) + 0.8f * sinf(T * 7 * i / 256 + 1.9f) + 0.35f * sinf(T * 13 * i / 256 + 4.1f);
    for (int i = 0; i < 64; i++) SD_WOB[i] = 0.9f * sinf(T * 2 * i / 64 + 1.0f) + 0.5f * sinf(T * 5 * i / 64 + 2.5f);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {                                          /* value noise on an 8 px grid, wrapped */
            const int gx = x >> 3, gy = y >> 3; float fx = (x & 7) / 8.0f, fy = (y & 7) / 8.0f;
            fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
            const float a = sd_hash01(gx & 7, (gy & 7) + 900), b = sd_hash01((gx + 1) & 7, (gy & 7) + 900);
            const float c = sd_hash01(gx & 7, ((gy + 1) & 7) + 900), d = sd_hash01((gx + 1) & 7, ((gy + 1) & 7) + 900);
            const float v = (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
            SD_MOT[y][x] = (uint8_t)(v * 255);
        }
    sd_tables = true;
}
static inline float sd_sway(int j, int x) { return j ? SD_SWAY[(x + j * 37) & 255] : 0; }   /* (the top bed's seam is the top face's edge) */
static inline bool  sd_hard(const scene_rock_t *r, int j) { return (r->hard >> j) & 1u; }
static inline float sd_top(const scene_desert_t *d, const scene_rock_t *r, float px) {   /* its top: the feet's line, past its ends level */
    if (!r->straight) return terrain_y(&d->tr.path[r->path], px) + TERRAIN_FEET_DY;
    return r->y0 + (sd_clamp(px, r->x0, r->x1) - r->x0) * r->k;
}

void scene_desert_layout(const glass_t *b, const habitat_t *h, scene_desert_t *d) {
    memset(d, 0, sizeof *d);
    d->glass = *b;
    terrain_lay(h, b, &d->tr);
    const float floor_d = SD_WALL_K * b->h > SD_WALL_DY ? SD_WALL_K * b->h : SD_WALL_DY;
    d->wall_y = b->bot - floor_d; d->sand_y = glass_place_y(b) + TERRAIN_FEET_DY;
    const float base = b->bot - SD_HIDE_DY;
    d->n_hides = h->n_hides > HABITAT_HIDES_MAX ? HABITAT_HIDES_MAX : h->n_hides;
    for (int k = 0; k < d->n_hides; k++) {
        scene_hide_t *hd = &d->hide[k];
        const int kind = h->hides[k].kind < HIDE_KINDS ? h->hides[k].kind : HIDE_WARM;
        hd->kind = kind; hd->point_x = glass_floor_x(b, h->hides[k].u);
        hd->half_w = SD_HIDE[kind].half_w; hd->height = SD_HIDE[kind].height;
        hd->door_hw = SD_HIDE[kind].door_hw; hd->door_h = SD_HIDE[kind].door_h;
        hd->base_y = base;
        /* inside the glass at its foot and at its shoulder (the bowl's sides and the corners close in) */
        float g0 = glass_x0(b, base), g1 = glass_x1(b, base);
        float s0 = glass_x0(b, base - hd->height * 0.6f), s1 = glass_x1(b, base - hd->height * 0.6f);
        float lo = (g0 > s0 ? g0 : s0) + hd->half_w + 4, hi = (g1 < s1 ? g1 : s1) - hd->half_w - 4;
        hd->cx = lo > hi ? 0.5f * (lo + hi) : sd_clamp(hd->point_x, lo, hi);
        hd->opens = h->hides[k].u < 0.5f ? 1 : -1;                                  /* toward the open floor */
        hd->door_x = hd->cx + hd->opens * (kind == HIDE_HUMID ? 0.30f : 0.34f) * hd->half_w;
        hd->door_y = base;
    }
    d->bask_x = d->tr.bask_x; d->bask_y = d->tr.bask_y;
    /* the ground's pieces: every rock and every ramp */
    for (int p = 1; p < d->tr.n_paths && d->n_rocks < SCENE_ROCKS_MAX; p++) {
        const terrain_path_t *q = &d->tr.path[p];
        scene_rock_t *r = &d->rock[d->n_rocks++];
        r->path = p; r->slab = q->kind == PATH_RAMP; r->salt = p * 37 + 11;
        r->shelf = !r->slab && p - 1 < h->n_rocks && h->rocks[p - 1].shelf;
        r->x0 = q->x[0]; r->x1 = q->x[q->n - 1]; r->y0 = q->y[0] + TERRAIN_FEET_DY;
        r->straight = q->n == 2 || q->x[1] - q->x[0] <= 0;
        r->k = r->x1 > r->x0 ? (q->y[q->n - 1] - q->y[0]) / (r->x1 - r->x0) : 0;
        r->l = r->x0 - (r->slab ? 0 : SD_LIP); r->r = r->x1 + (r->slab ? 0 : SD_LIP);
        if (r->slab) continue;
        float lo_y = q->y[0]; for (int i = 1; i < q->n; i++) if (q->y[i] > lo_y) lo_y = q->y[i];
        const float face0 = lo_y + TERRAIN_FEET_DY + SD_TOP_HW;
        r->foot = r->shelf ? face0 + SD_SHELF_D : b->bot - SD_ROCK_FOOT;
        /* its beds, to its foot: hard and soft by turns (now and then two alike), the hard ones out, a little wider down */
        float y = 0; int j = 0;
        while (j < SCENE_BEDS) {
            const bool hard = j == 0 || (((j & 1) == 0) ^ ((sd_hash(j, r->salt + 1) & 7) == 0));
            if (hard) r->hard |= 1u << j;
            r->ext_l[j] = j ? sd_clamp(0.7f * j, 0, 7) + (hard ? 2.5f : -1.5f) + 2.4f * (sd_hash01(j, r->salt + 2) - 0.5f) : 0;
            r->ext_r[j] = j ? sd_clamp(0.7f * j, 0, 7) + (hard ? 2.5f : -1.5f) + 2.4f * (sd_hash01(j, r->salt + 3) - 0.5f) : 0;
            r->bed_y[j] = y;
            y += (float)(6 + (int)(sd_hash(j, r->salt) % 8));
            j++;
            if (face0 + y >= r->foot) break;
        }
        r->n_beds = j; r->bed_y[j] = r->foot - face0 + 1;
        for (int k = 0; k < 4; k++) {                                       /* its cracks: where, from, how long, the slant */
            r->crack[k][0] = r->l + 4 + (r->r - r->l - 8) * sd_hash01(k, r->salt + 51);
            r->crack[k][1] = (r->foot - face0) * 0.6f * sd_hash01(k, r->salt + 52);
            r->crack[k][2] = 14 + 30 * sd_hash01(k, r->salt + 53);
            r->crack[k][3] = (sd_hash01(k, r->salt + 54) - 0.5f) * 0.45f;
        }
    }
}
void scene_desert_peek_at(const scene_desert_t *d, int k, float half_len, float peek, float *x, float *floor_y, float *yaw) {
    const scene_hide_t *hd = &d->hide[k < 0 ? 0 : k >= d->n_hides ? d->n_hides - 1 : k];
    const float f = (float)hd->opens, cut = hd->door_x - f * hd->door_hw * 0.35f;   /* where it leaves the dark: just inside the door */
    *x = cut - f * half_len * (1 - 2 * peek);
    *floor_y = hd->door_y;
    *yaw = f;
}
int scene_desert_moist_step(float m) { return (int)(sd_clamp(m, 0, 1) * 8 + 0.5f); }

/* ---- the pieces at a pixel ---- */
enum { SD_NONE, SD_TOP, SD_FACE, SD_UNDER };
typedef struct { int part; float top; int bed; bool hard; float ly, lh, out, rel; } sd_hit_t;   /* ly: into its bed (or across its top 0..1);
                                                                                                 out: past the bed above's edge; rel: under its top face */
/* the part of piece r at (px, py), its top's y there */
static int sd_piece(const scene_desert_t *d, const scene_rock_t *r, float px, float py, sd_hit_t *hit) {
    const float top = sd_top(d, r, px);
    hit->top = top;
    const int xi = (int)px;
    if (r->slab) {
        if (px < r->l || px > r->r) return SD_NONE;
        const float dy = py - top, face = SD_SLAB_FACE + 0.9f * SD_SWAY[(xi * 3 + r->salt) & 255];
        if (dy < -SD_SLAB_HW || py > d->sand_y + 4.0f) return SD_NONE;         /* (its foot sunk in the sand) */
        if (dy <= SD_SLAB_HW) { hit->part = SD_TOP; hit->ly = (dy + SD_SLAB_HW) / (2 * SD_SLAB_HW); return SD_TOP; }
        if (dy <= SD_SLAB_HW + face) { hit->part = SD_FACE; hit->ly = dy - SD_SLAB_HW; hit->lh = face; hit->bed = 0; hit->out = 0; return SD_FACE; }
        return SD_NONE;
    }
    if (px < r->l - SD_EXT_MAX || px > r->r + SD_EXT_MAX) return SD_NONE;
    const float back = top - SD_TOP_HW + 0.45f * SD_SWAY[(xi * 3 + r->salt * 5) & 255];
    if (py < back || py > r->foot) return SD_NONE;
    const float face0 = top + SD_TOP_HW;
    if (py <= face0) {
        if (px < r->l || px > r->r) return SD_NONE;
        hit->part = SD_TOP; hit->ly = (py - back) / (face0 - back); return SD_TOP;
    }
    /* the front face: its bed by the seams' tops, swayed */
    const float rel = py - face0;
    int j = 0;
    while (j + 1 < r->n_beds && rel >= r->bed_y[j + 1]) j++;
    if (j > 0 && rel < r->bed_y[j] + sd_sway(j, xi)) j--;
    else if (j + 1 < r->n_beds && rel >= r->bed_y[j + 1] + sd_sway(j + 1, xi)) j++;
    const int wob = ((int)py + j * 13) & 63;
    float l = r->l - (j ? r->ext_l[j] + SD_WOB[wob] : 0), rr = r->r + (j ? r->ext_r[j] + SD_WOB[(wob + 29) & 63] : 0);
    if (r->shelf) {                                                             /* a shelf rounds off under toward the wall */
        const float k = rel / (r->foot - face0);
        l = r->l + k * k * 9 - (j ? 1.5f : 0); rr = r->r - k * k * 9 + (j ? 1.5f : 0);
    }
    if (px < l || px > rr) return SD_NONE;
    hit->part = r->shelf && r->foot - py < 2.5f ? SD_UNDER : SD_FACE;
    hit->bed = j; hit->hard = sd_hard(r, j); hit->rel = rel;
    hit->ly = rel - (r->bed_y[j] + sd_sway(j, xi)); hit->lh = r->bed_y[j + 1] - r->bed_y[j];
    if (!j || r->shelf) hit->out = 0;
    else {
        const int wa = ((int)py + (j - 1) * 13) & 63;
        const float ul = r->l - (j > 1 ? r->ext_l[j - 1] + SD_WOB[wa] : 0), ur = r->r + (j > 1 ? r->ext_r[j - 1] + SD_WOB[(wa + 29) & 63] : 0);
        hit->out = px < ul ? ul - px : px > ur ? px - ur : 0;
    }
    return hit->part;
}
/* the frontmost piece at (px, py) of the listed ones (NULL: all): its index, -1 none - of two over a pixel the one whose
   top stands lower on the glass */
static int sd_front(const scene_desert_t *d, const int *list, int n, float px, float py, sd_hit_t *best) {
    int at = -1; sd_hit_t h;
    for (int k = 0; k < n; k++) {
        const int i = list ? list[k] : k;
        if (sd_piece(d, &d->rock[i], px, py, &h) == SD_NONE) continue;
        if (at < 0 || h.top > best->top) { *best = h; at = i; }
    }
    return at;
}
int scene_desert_ground_at(const scene_desert_t *d, float x, float y) {
    sd_tables_make();
    sd_hit_t h; const int i = sd_front(d, NULL, d->n_rocks, x, y, &h);
    return i < 0 ? 0 : d->rock[i].path;
}
/* the stretch of row y piece r covers, near enough for its shadow: false = none */
static bool sd_span(const scene_desert_t *d, const scene_rock_t *r, float y, float *x0, float *x1) {
    if (r->slab) {
        if (y > d->sand_y + 4.0f) return false;
        const float lo = y - SD_SLAB_HW - SD_SLAB_FACE, hi = y + SD_SLAB_HW;      /* the tops that cover it: lo..hi */
        if (fabsf(r->k) < 1e-4f) { if (r->y0 < lo || r->y0 > hi) return false; *x0 = r->x0; *x1 = r->x1; return true; }
        float a = r->x0 + (lo - r->y0) / r->k, b = r->x0 + (hi - r->y0) / r->k;
        if (a > b) { const float t = a; a = b; b = t; }
        *x0 = fmaxf(a, r->x0); *x1 = fminf(b, r->x1);
        return *x0 <= *x1;
    }
    const float top = r->straight ? fminf(r->y0, r->y0 + (r->x1 - r->x0) * r->k) : r->y0;
    if (y < top - SD_TOP_HW || y > r->foot) return false;
    const float rel = y - (top + SD_TOP_HW);
    if (rel <= 0 || r->shelf) { *x0 = r->l; *x1 = r->r; return true; }
    int j = 0; while (j + 1 < r->n_beds && rel >= r->bed_y[j + 1]) j++;
    *x0 = r->l - r->ext_l[j]; *x1 = r->r + r->ext_r[j];
    return true;
}

/* ---- painting ---- */
typedef struct { float r, g, b; } sd_rgb_t;
static inline sd_rgb_t sd_hex(uint32_t c) { sd_rgb_t o = { (float)(c >> 16 & 255), (float)(c >> 8 & 255), (float)(c & 255) }; return o; }
static inline sd_rgb_t sd_mix(sd_rgb_t a, sd_rgb_t b, float p) { sd_rgb_t o = { a.r + (b.r - a.r) * p, a.g + (b.g - a.g) * p, a.b + (b.b - a.b) * p }; return o; }
static inline sd_rgb_t sd_mul(sd_rgb_t a, float k) { sd_rgb_t o = { a.r * k, a.g * k, a.b * k }; return o; }

#define SD_PEBBLES 22
#define SD_PLANTS  2
#define SD_BLOT    24                   /* the wall's blotches: a smooth noise on a 24 px grid */
typedef struct { float x, y, irx, iry; int tone, y0, y1; } sd_pebble_t;
typedef struct {
    const glass_t *b;
    scene_desert_t d;
    float moist[HABITAT_HIDES_MAX];
    float lamp_x, lamp_r, lamp_top, spot_y;
    sd_pebble_t peb[SD_PEBBLES]; int n_peb;
    float plant_x[SD_PLANTS], plant_y[SD_PLANTS]; int n_plants;
    float sint[64];                     /* a sine, 64 steps a turn: the strata's edges */
    float rock_y0[SCENE_ROCKS_MAX], rock_y1[SCENE_ROCKS_MAX];   /* the rows each piece reaches */
} sd_ctx_t;

/* the wall: strata SD_LAYER deep on average, each edge its own gentle wave (two of the sine's harmonics, its own
   phase), a lit ledge along each edge and the shadow under the next; blotches, mottle and grit over it */
static const uint32_t SD_STONE[4] = { 0x845a3e, 0x92664a, 0x7a5440, 0x9c6e4e };
#define SD_WLAYER 30
static inline float sd_strata_edge(const sd_ctx_t *c, int li, int x) {      /* layer li's top at column x */
    const uint32_t h = sd_hash(li, 23);
    return li * SD_WLAYER + (float)(h & 15) - 7.5f + 3.2f * c->sint[(x / 3 + (int)(h >> 8)) & 63] + 1.3f * c->sint[(x * 2 / 3 + (int)(h >> 16)) & 63];
}
static sd_rgb_t sd_wall(const sd_ctx_t *c, int x, int y, float ly, float blot) {
    int li = (int)floorf(ly / SD_WLAYER);
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
/* a rock: sandstone lighter and warmer than the wall - its top face lit, brightest at its front lip; its front face's beds
   in shade under the lip, a hard bed creamier and lit along the top it sticks out with, a soft one redder and shadowed
   under the bed over it; the side toward the lamp lit, the far side and the foot darker; its cracks; mottle and grit over
   it all; a shelf's underside dark. A ramp's slab: weathered flagstone, its top lit with a lichen spot here and there, its
   ragged edge in shade */
static const uint32_t SD_ROCK[6] = { 0x4a2a1a, 0x74442a, 0x9a623c, 0xbc8454, 0xd6a46e, 0xecc28c };
static const uint32_t SD_SLATE[5] = { 0x3c3430, 0x5a4c44, 0x7a6a5e, 0x9c8a78, 0xc0ac96 };
static sd_rgb_t sd_ground(const sd_ctx_t *c, const scene_rock_t *r, const sd_hit_t *h, int x, int y) {
    const uint32_t g = sd_hash(x * 5 + r->salt, y * 11 + 3);
    const float px = x + 0.5f, py = y + 0.5f;
    const float mid = 0.5f * (r->l + r->r), side = (px - mid) / (0.5f * (r->r - r->l) + 1) * (c->lamp_x < mid ? -1.0f : 1.0f);   /* +1 toward the lamp */
    const float mot = 0.90f + 0.20f / 255.0f * SD_MOT[(y + r->salt * 7) & 63][(x + r->salt * 13) & 63];
    int t;
    if (r->slab) {
        if (h->part == SD_TOP) t = h->ly < 0.25f ? 2 : h->ly > 0.78f ? 4 : 3;
        else t = h->ly < 1.5f ? 1 : h->ly > h->lh - 1.2f ? 0 : 2;
        if ((g & 15) == 0) t = t > 0 ? t - 1 : 0;
        sd_rgb_t s = sd_mul(sd_hex(SD_SLATE[t]), mot);
        if (h->part == SD_TOP && (sd_hash(x / 3, y / 2 + r->salt) & 31) == 0) s = sd_mix(s, sd_hex(0xb4b896), 0.45f);   /* lichen */
        return s;
    }
    bool cream = false, red = false;
    if (h->part == SD_TOP) {
        t = h->ly < 0.18f ? 3 : h->ly > 0.84f ? 5 : 4;
        if (px < r->l + 2.5f || px > r->r - 2.5f) t--;                          /* its rounded ends */
        if ((g & 15) == 0) t--; else if ((g & 31) == 1 && t < 5) t++;
    } else if (h->part == SD_UNDER) {
        t = 0;
    } else {
        cream = h->hard; red = !h->hard;
        t = h->hard ? 3 : 2;
        if (h->bed == 0 && h->ly < 2.5f) t = 1;                                 /* the shadow under the lip */
        else if (!h->hard && h->ly < 2.5f) t = 1;                               /* a soft bed under the hard one over it */
        else if (h->hard && h->out > 0.5f && h->ly < 2.0f) t = 4;               /* a hard bed's top, sticking out: lit */
        if (!r->shelf && r->foot - py < 8) t--;                                 /* toward the sand */
        for (int k = 0; k < 4; k++) {                                           /* its cracks */
            const float *ck = r->crack[k], a = h->rel - ck[1];
            if (a >= 0 && a <= ck[2] && fabsf(px - (ck[0] + ck[3] * a)) < 0.75f) { t = 1; break; }
        }
        if ((g & 31) == 0) t--; else if ((g & 63) == 1) t++;
    }
    t = t < 0 ? 0 : t > 5 ? 5 : t;
    const float deep = h->part == SD_FACE && !r->shelf ? sd_clamp(h->rel / (r->foot - h->top - SD_TOP_HW), 0, 1) : 0;   /* down a terrace's face the */
    const float lamp = h->part == SD_FACE ? 1 + 0.13f * sd_clamp(side, -1.2f, 1.2f) : 1;                             /* shade deepens; its side toward */
    sd_rgb_t s = sd_mul(sd_hex(SD_ROCK[t]), mot * lamp * (1 - 0.20f * deep));                                       /* the lamp is lit, the far one not */
    if (cream) s = sd_mix(s, sd_hex(0xf2dcb4), 0.12f);
    else if (red) s = sd_mix(s, sd_hex(0x8a3a22), 0.10f);
    return s;
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
    if (shade_row) {                                                            /* contact shadows: the hides, the terraces, the slabs' feet */
        float shade = 0;
        for (int j = 0; j < c->d.n_hides; j++) {
            const scene_hide_t *hd = &c->d.hide[j];
            float ex = (px - hd->cx) / (hd->half_w + 7), ey = (py - hd->base_y - 1) * (1.0f / 6.0f), e = ex * ex + ey * ey;
            if (e < 1) shade += 0.42f * (1 - e);
        }
        for (int j = 0; j < c->d.n_rocks; j++) {
            const scene_rock_t *r = &c->d.rock[j];
            if (r->slab) {                                                      /* a slab's foot in the sand */
                const terrain_path_t *q = &c->d.tr.path[r->path];
                const int lo = q->y[0] > q->y[q->n - 1] ? 0 : q->n - 1;
                if (q->y[lo] < glass_place_y(c->b) - 1) continue;
                float ex = (px - q->x[lo]) * (1.0f / 16), ey = (py - c->d.sand_y - 3) * 0.25f, e = ex * ex + ey * ey;
                if (e < 1) shade += 0.30f * (1 - e);
            } else if (!r->shelf) {
                const float dy = py - r->foot;
                if (dy < -1 || dy > 7 || px < r->l - 20 || px > r->r + 20) continue;
                shade += 0.38f * (1 - (dy + 1) / 8);
            }
        }
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


void scene_desert_draw(uint16_t *fb, int stride, const glass_t *b, const habitat_t *h, bool lit, const float *moisture,
                       float dim, const uint8_t *vig) {
    static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    static sd_ctx_t c;                  /* (the layout holds the ground: kept off the stack; one bake at a time) */
    sd_tables_make();
    memset(&c, 0, sizeof c);
    c.b = b;
    scene_desert_layout(b, h, &c.d);
    for (int k = 0; k < c.d.n_hides; k++) c.moist[k] = (float)scene_desert_moist_step(moisture ? moisture[k] : h->moist_start) / 8.0f;
    c.lamp_x = c.d.bask_x; c.lamp_r = h->bask_r_px > 0 ? h->bask_r_px : 70; c.lamp_top = glass_top(b, c.lamp_x);
    c.spot_y = c.d.bask_y + TERRAIN_FEET_DY;
    for (int k = 0; k < 64; k++) c.sint[k] = sinf((float)k * 6.2831853f / 64);
    const float wall_y = c.d.wall_y;
    for (int k = 0; k < c.d.n_rocks; k++) {                                    /* the rows each piece of the ground reaches */
        const scene_rock_t *r = &c.d.rock[k];
        const float ya = sd_top(&c.d, r, r->x0), yb = sd_top(&c.d, r, r->x1);
        c.rock_y0[k] = fminf(ya, yb) - SD_TOP_HW - 3;
        c.rock_y1[k] = r->slab ? fmaxf(ya, yb) + SD_SLAB_HW + SD_SLAB_FACE + 3 : r->foot + 1;
    }
    /* the pebbles, off the hides; the succulents: one on the basking rock's free end, one at the back of the open sand */
    for (int k = 0; k < SD_PEBBLES; k++) {
        const uint32_t ph = sd_hash(k, 4242);
        sd_pebble_t *p = &c.peb[c.n_peb];
        p->x = 8 + (float)(ph & 1023) / 1023.0f * (b->w - 16);
        p->y = wall_y + 8 + (float)((ph >> 10) & 255) / 255.0f * (b->bot - wall_y - 12);
        const float rx = 1.6f + (float)((ph >> 18) & 3) * 0.7f;
        p->irx = 1 / rx; p->iry = 1 / (rx * 0.62f); p->tone = (int)((ph >> 21) % 3);
        bool off = false;
        for (int j = 0; j < c.d.n_hides && !off; j++) off = fabsf(p->x - c.d.hide[j].cx) < c.d.hide[j].half_w + 8 && p->y > c.d.hide[j].base_y - c.d.hide[j].height - 4 && p->y < c.d.hide[j].base_y + 9;
        if (off) continue;
        p->y0 = (int)floorf(p->y - rx * 0.62f); p->y1 = (int)ceilf(p->y + rx * 0.62f);
        c.n_peb++;
    }
    {
        const terrain_t *tr = &c.d.tr;
        if (tr->bask_path > 0) {
            const terrain_path_t *q = &tr->path[tr->bask_path];
            bool lfree = true, rfree = true;
            for (int k = 0; k < tr->n_links; k++) {
                const bool its = tr->link[k].a == tr->bask_path || tr->link[k].b == tr->bask_path;
                if (its && fabsf(tr->link[k].x - q->x[0]) < 1) lfree = false;
                if (its && fabsf(tr->link[k].x - q->x[q->n - 1]) < 1) rfree = false;
            }
            if (lfree || rfree) {
                const float x = lfree ? q->x[0] - SD_LIP + 7 : q->x[q->n - 1] + SD_LIP - 7;
                c.plant_x[c.n_plants] = x; c.plant_y[c.n_plants] = terrain_y(q, x) + TERRAIN_FEET_DY - SD_TOP_HW + 3; c.n_plants++;
            }
        }
        static const float PLANT_U[4] = { 0.80f, 0.70f, 0.62f, 0.90f };
        for (int k = 0; k < 4 && c.n_plants < SD_PLANTS; k++) {
            const float x = glass_floor_x(b, PLANT_U[k]), y = wall_y + 16;
            bool clear = scene_desert_ground_at(&c.d, x - 12, y) == 0 && scene_desert_ground_at(&c.d, x + 12, y) == 0 && scene_desert_ground_at(&c.d, x, y - 12) == 0;
            for (int j = 0; j < c.d.n_hides; j++) clear &= fabsf(x - c.d.hide[j].cx) > c.d.hide[j].half_w + 14;
            if (clear) { c.plant_x[c.n_plants] = x; c.plant_y[c.n_plants] = y; c.n_plants++; }
        }
    }
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
    for (int k = 0; k < c.d.n_rocks; k++) if (!c.d.rock[k].slab && !c.d.rock[k].shelf && c.d.rock[k].foot - 2 < shadow_y0) shadow_y0 = (int)(c.d.rock[k].foot - 2);
    const sd_rgb_t LAMP = sd_hex(0xffd59a), NIGHT = sd_hex(0x182238), GLARE = sd_hex(0xfff2d4);
    for (int y = 0; y < b->h; y++) {
        uint16_t *row = fb + (size_t)y * stride;
        const float fy = y + 0.5f;
        int gx0, gx1;                                                           /* the glass along this row */
        if (b->kind == GLASS_ROUND) {
            float dd = fy - b->r, r2 = b->r * b->r - dd * dd;
            if (r2 <= 0) { gx0 = 1; gx1 = 0; }
            else { float hw = sqrtf(r2); gx0 = (int)ceilf(b->r - hw - 0.5f); gx1 = (int)floorf(b->r + hw - 0.5f); }
        } else { float in = glass_x0(b, fy); gx0 = (int)ceilf(in - 0.5f); gx1 = (int)floorf(b->w - in - 0.5f); }
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
        const float wdy = (fy - (c.spot_y - 70)) * (1.0f / 160.0f), wdy2 = wdy * wdy;   /* the lamp's spot on the wall */
        const float pdy = (fy - (c.spot_y - 2)) * (1.0f / 26.0f), pdy2 = pdy * pdy;      /* ... its pool on the rock */
        const float beam_t = (fy - c.lamp_top) / (c.spot_y - c.lamp_top);
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
        /* the pieces in this row, each with the stretch it may cover; the stretches two and four rows up, for their shadow
           on what is behind them (away from the lamp), and the shelves' shadow down the wall under them */
        int nrk = 0, rk[SCENE_ROCKS_MAX]; float rx0[SCENE_ROCKS_MAX], rx1[SCENE_ROCKS_MAX];
        int ns2 = 0, ns4 = 0; float s2a[SCENE_ROCKS_MAX], s2b[SCENE_ROCKS_MAX], s4a[SCENE_ROCKS_MAX], s4b[SCENE_ROCKS_MAX];
        int nsh = 0; float sha[SCENE_ROCKS_MAX], shb[SCENE_ROCKS_MAX], shk[SCENE_ROCKS_MAX];
        for (int k = 0; k < c.d.n_rocks; k++) {
            const scene_rock_t *r = &c.d.rock[k];
            float a, bb;
            if (fy >= c.rock_y0[k] && fy <= c.rock_y1[k]) {
                rk[nrk] = k;
                if (r->slab) { rx0[nrk] = r->l; rx1[nrk] = r->r; nrk++; }
                else if (sd_span(&c.d, r, fy, &a, &bb)) { rx0[nrk] = fminf(a, r->l) - 4; rx1[nrk] = fmaxf(bb, r->r) + 4; nrk++; }
                else { rx0[nrk] = r->l - SD_EXT_MAX; rx1[nrk] = r->r + SD_EXT_MAX; nrk++; }   /* (its top's sway past the row's bounds) */
            }
            if (sd_span(&c.d, r, fy - 2, &a, &bb)) { s2a[ns2] = a; s2b[ns2] = bb; ns2++; }
            if (sd_span(&c.d, r, fy - 4, &a, &bb)) { s4a[ns4] = a; s4b[ns4] = bb; ns4++; }
            if (r->shelf && fy > r->foot && fy < r->foot + 22) { sha[nsh] = r->l + 6; shb[nsh] = r->r - 6; shk[nsh] = 0.62f + 0.38f * (fy - r->foot) / 22; nsh++; }
        }
        const bool shade_row = y >= shadow_y0 && y <= shadow_y1;
        bool plant_row = false;
        for (int k = 0; k < c.n_plants; k++) if (fy > c.plant_y[k] - 13 && fy < c.plant_y[k] + 2) plant_row = true;
        for (int x = gx0; x <= gx1; x++) {
            const float fx = x + 0.5f;
            const bool sand = fy * 8 >= edge[x];
            int ri = -1; sd_hit_t hit;
            for (int q = 0; q < nrk; q++) {                                     /* the frontmost piece here: the lowest top */
                if (fx < rx0[q] || fx > rx1[q]) continue;
                sd_hit_t hq;
                if (sd_piece(&c.d, &c.d.rock[rk[q]], fx, fy, &hq) == SD_NONE) continue;
                if (ri < 0 || hq.top > hit.top) { hit = hq; ri = rk[q]; }
            }
            sd_rgb_t s;
            if (ri >= 0) s = sd_ground(&c, &c.d.rock[ri], &hit, x, y);
            else {
                if (sand) {
                    s = sd_sand(&c, x, y, depth, rip[x], npeb, peb, shade_row, moist_row);
                    if (fy * 8 < edge[x] + 20) s = sd_mul(s, 0.80f);              /* the wall's foot */
                } else {
                    const int kb = x / SD_BLOT; float f = (float)(x % SD_BLOT) / SD_BLOT; f = f * f * (3 - 2 * f);
                    s = sd_mul(sd_wall(&c, x, y, fy + sway[x] * 0.125f, blot[kb] + (blot[kb + 1] - blot[kb]) * f), lid_k);
                }
                float k = 1; const float sx = fx > c.lamp_x ? 7.0f : -7.0f;    /* the ground's shadow behind it, away from the lamp */
                for (int q = 0; q < ns4; q++) if (fx - sx >= s4a[q] && fx - sx <= s4b[q]) { k *= 0.80f; break; }
                for (int q = 0; q < ns2; q++) if (fx - sx * 0.5f >= s2a[q] && fx - sx * 0.5f <= s2b[q]) { k *= 0.90f; break; }
                for (int q = 0; q < nsh; q++) if (fx >= sha[q] && fx <= shb[q]) k *= shk[q];   /* under a shelf */
                if (k < 1) s = sd_mul(s, k);
            }
            sd_rgb_t o; bool prop = false;
            if (hide_row)
                for (int k = 0; k < c.d.n_hides && !prop; k++) {
                    const scene_hide_t *hd = &c.d.hide[k];
                    if (fx < hd->cx - hd->half_w - 3 || fx > hd->cx + hd->half_w + 3) continue;
                    prop = hd->kind == HIDE_HUMID ? sd_box(&c, hd, c.moist[k], x, y, &o) : sd_cave(&c, hd, x, y, &o);
                }
            if (!prop && plant_row)
                for (int k = 0; k < c.n_plants && !prop; k++) prop = sd_plant(fx, fy, c.plant_x[k], c.plant_y[k], &o);
            if (prop) s = o;
            const bool solid = sand || prop || ri >= 0;
            if (lit) {
                const float ldx = fx - c.lamp_x;
                float gain = 0;
                if (!solid) { float gx = ldx * (1.0f / 110.0f), d = gx * gx + wdy2; if (d < 1) gain += 0.62f * (1 - d) * (1 - d); }
                else { float gx = ldx * ilr, d = gx * gx + pdy2; if (d < 1) gain += 0.72f * (1 - d) * (1 - d); }
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
