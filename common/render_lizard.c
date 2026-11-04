/* render_lizard.c - the lizard body plan (pocket-terrarium P3a): draw_lizard and the pose cache behind it.
 *
 * THE BODY is a SPINE CHAIN: LZ_N nodes from the snout to the tail tip, each with a half height and a half width,
 * measured in fractions of the pack's length (geom.length_px x the scale): a broad head (geom.head_w), a short
 * neck, a plump trunk, a tail whose girth is the fat reserve (pose.fat x geom.tail_fat). A hatchling's head and
 * eyes are bigger for its size. The pose bends it - the neck's and the head's pitch, the trunk's slope and arch,
 * the tail's lift and curl, how high the belly is carried - and the ground holds up whatever would sink into it,
 * so a tail lies along the sand and a basking belly is flat on it. The yaw turns it about the vertical, the head
 * first and the tail a beat behind: the turn on the spot swings it through head-on. Seen from the side and a
 * little from above, a point's screen y is its height up plus LZ_TILT of its depth toward the glass.
 *
 * Drawn the crab's way (render.c cr_build): every cell of the body's grid is COMPUTED from the chain at its centre
 * (no smoothing: crisp pixel art) - the nearest segment's place along the body and round it pick the tone: the
 * light from above in three steps, the belly, the markings (a hatchling's bands, an adult's spots, both laid on
 * the body's own surface so they bend with it), the lips' line, the nostril, the ear, the shed's loose skin and
 * stuck rings - and one pass steps every cell on the silhouette, or behind a nearer part, down by its own tone
 * (no drawn line). The grid is built only for a body not drawn lately (LZ_SLOTS of them stay, the least recent
 * goes), with what the live parts hang from (lz_sum_t), and blitted with the morph's palette every frame. THE
 * LEGS, THE EYES, THE TONGUE AND THE MOUTH are drawn live over it: they move every frame and the body under them
 * holds still - so a walking lizard is one grid, a blink or a flick costs no build, and only a new body (a pose,
 * a step of a turn, the breath, the stalking tail's tip) builds. The legs walk in DIAGONAL PAIRS (the near front
 * with the far hind), each foot on the sand for LZ_DUTY of the cycle; a leg is two bones bent by a pole (elbows
 * back, knees forward, both out), a foot and toes. Everything live is drawn in runs along rows (a capsule's row
 * is an interval), and the contact shadow in three steps.
 *
 * Everything is in the frame's pixels through lz_canvas_t: clipped to its window, its mark called per run so
 * render.c can keep its dirty and touched masks. */
#include "render_lizard.h"
#include "render.h"                 /* FB_SWAP16 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LZ_TILT     0.22f           /* screen y per px of depth toward the glass: the view a little from above */
#define LZ_N        25              /* the spine's nodes */
#define LZ_SHOULDER 7               /* the node the front legs hang from */
#define LZ_HIP      13              /* ... the hind legs: the middle of its length - the anchor and the turn's pivot */
#define LZ_GW       128             /* the body grid's most cells across ... */
#define LZ_GH       48              /* ... and down (an elder at 1.3, lunging or turned head-on, fits: the sheet counts) */
#define LZ_SLOTS    24              /* the bodies kept: three geckos' poses, breaths and turn steps (measured: a busy minute
                                       of three builds 36 bodies at 24, 137 at 20 - the working set) */
#define LZ_PALS     4               /* the palettes kept (a morph, a stage, a shed, the night) */
#define LZ_DUTY     0.62f           /* a foot's share of the walk cycle on the sand */

/* along the body, snout 0 .. tail tip 1 */
static const float LZ_S[LZ_N] = { 0.000f, 0.030f, 0.062f, 0.095f, 0.128f, 0.160f, 0.195f, 0.250f, 0.290f, 0.330f, 0.370f, 0.410f,
                                  0.455f, 0.500f, 0.550f, 0.600f, 0.650f, 0.700f, 0.750f, 0.800f, 0.850f, 0.895f, 0.935f, 0.970f, 1.000f };
/* the half height of the head and the trunk, x the length (an adult's) */
static const float LZ_RH[LZ_HIP + 1] = { 0.024f, 0.038f, 0.049f, 0.055f, 0.058f, 0.050f, 0.041f, 0.057f, 0.069f, 0.076f, 0.080f, 0.080f,
                                         0.076f, 0.066f };
/* the head's half width along it, x geom.head_w / 2 */
static const float LZ_HW[LZ_SHOULDER] = { 0.30f, 0.55f, 0.78f, 0.93f, 1.00f, 0.92f, 0.76f };
/* the tail from the hip, x the length: no reserve at all, and a full one shown whole (geom.tail_fat 1) */
static const float LZ_RT_THIN[LZ_N - LZ_HIP] = { 0.066f, 0.046f, 0.040f, 0.035f, 0.030f, 0.025f, 0.020f, 0.016f, 0.012f, 0.008f, 0.005f, 0.003f };
static const float LZ_RT_FAT[LZ_N - LZ_HIP]  = { 0.066f, 0.082f, 0.094f, 0.092f, 0.082f, 0.068f, 0.052f, 0.036f, 0.023f, 0.013f, 0.006f, 0.003f };
#define LZ_T_HEAD 0.165f            /* the head ends here (the ear behind the eye) */
#define LZ_T_TAIL 0.550f            /* the tail starts here */
#define LZ_T_EYE  0.072f

/* ---- the morphs (docs/species-pack.md: the gecko's five). The pack gives a morph's name, body and accent; the
 * rest of its look is art and lives here, by name: a hatchling's bands and the ground between, the light rings,
 * the belly, the hue the shading steps toward, the eye, how spotted an adult is and the mack snow's yellow wash.
 * A morph not listed is drawn from its body and accent alone. ---- */
typedef struct {
    const char *name;
    uint32_t ground, pattern;          /* an adult's ground and markings (0: the pack's body / accent) */
    uint32_t hatch_band, hatch_ground; /* a hatchling's bands and the ground between (hatch_band 0: none - patternless) */
    uint32_t light, belly, shade;      /* the light rings and tail bands, the belly, the shadows' hue */
    uint32_t iris, pupil, rim;         /* the eye */
    float    spots;                    /* an adult's spots, 0..1 of the places a spot can be (0: patternless) */
    float    wash;                     /* an adult's yellow wash over its pale ground */
} lz_art_t;
static const lz_art_t LZ_ARTS[] = {
    { "NORMAL",    0,        0, 0x3a2733, 0xf1c447, 0xf3eee0, 0xf6eedb, 0x7c3d16, 0x5c5244, 0x110f0e, 0x272220, 0.85f, 0.00f },
    { "TANGERINE", 0,        0, 0x4c2b1e, 0xf6a03e, 0xfbeedc, 0xfaeedd, 0x8c3410, 0x5c4a3a, 0x130f0d, 0x2a1f18, 0.42f, 0.00f },
    { "ALBINO",    0xf0c3a0, 0, 0xb27a5e, 0xf6d4ae, 0xfbf0e6, 0xfdf3ea, 0xa44a3c, 0xeab8b6, 0xb02a3c, 0xa4606a, 0.80f, 0.00f },
    { "BLIZZARD",  0,        0, 0,        0,        0xf4f2ec, 0xf6f4ee, 0x6c6a74, 0x2c2c32, 0x0b0b0e, 0x1c1c22, 0.00f, 0.00f },
    { "MACK SNOW", 0,        0, 0x1f1f24, 0xf3f1ea, 0xfafaf6, 0xfafaf4, 0x5a5c6a, 0x3c3c40, 0x0e0e10, 0x202024, 0.85f, 0.40f },
};
#define LZ_N_ARTS ((int)(sizeof LZ_ARTS / sizeof LZ_ARTS[0]))

/* ---- small helpers ---- */
static inline float lz_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int   lz_floori(float v) { int i = (int)v; return i - (v < (float)i); }   /* (floorf is a libm call on the board) */
static inline float lz_frac(float v) { return v - (float)lz_floori(v); }
static inline uint32_t lz_hash(int a, int b, uint32_t seed) {
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static uint32_t lz_mix(uint32_t a, uint32_t b, float p) {
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255, br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return ((uint32_t)(ar + (br - ar) * p) << 16) | ((uint32_t)(ag + (bg - ag) * p) << 8) | (uint32_t)(ab + (bb - ab) * p);
}
static uint32_t lz_desat(uint32_t c, float k) {          /* k of the way to its own grey */
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255, l = (r * 77 + g * 151 + b * 28) >> 8;
    uint32_t grey = (uint32_t)l << 16 | (uint32_t)l << 8 | (uint32_t)l;
    return lz_mix(c, grey, k);
}
static inline uint16_t lz_565(uint32_t rgb, float dim) {  /* in the frame's byte order */
    uint32_t r = (uint32_t)(((rgb >> 16) & 255) * dim), g = (uint32_t)(((rgb >> 8) & 255) * dim), b = (uint32_t)((rgb & 255) * dim);
    return FB_SWAP16((uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
}
/* asin from a table of 65 steps, linear between (the place round the body from the height across it; a sqrt and a
   polynomial a cell cost the build a tenth of its time) */
static const float LZ_ASIN[65] = {
    0.00000f, 0.01563f, 0.03126f, 0.04689f, 0.06254f, 0.07820f, 0.09389f, 0.10959f, 0.12533f, 0.14109f, 0.15689f, 0.17273f, 0.18862f,
    0.20455f, 0.22053f, 0.23658f, 0.25268f, 0.26885f, 0.28510f, 0.30142f, 0.31782f, 0.33432f, 0.35091f, 0.36760f, 0.38440f, 0.40131f,
    0.41835f, 0.43551f, 0.45282f, 0.47027f, 0.48788f, 0.50565f, 0.52360f, 0.54174f, 0.56008f, 0.57863f, 0.59741f, 0.61643f, 0.63571f,
    0.65527f, 0.67513f, 0.69531f, 0.71584f, 0.73674f, 0.75804f, 0.77978f, 0.80200f, 0.82475f, 0.84806f, 0.87201f, 0.89667f, 0.92210f,
    0.94843f, 0.97575f, 1.00423f, 1.03405f, 1.06544f, 1.09872f, 1.13433f, 1.17289f, 1.21538f, 1.26340f, 1.32014f, 1.39379f, 1.57080f };
static inline float lz_asin(float v) {
    float a = fabsf(v) * 64; if (a > 63.999f) a = 63.999f;
    const int i = (int)a; const float r = LZ_ASIN[i] + (LZ_ASIN[i + 1] - LZ_ASIN[i]) * (a - i);
    return v < 0 ? -r : r;
}
/* a value noise along x (-1..1), for the bands' and the loose skin's ragged edges */
static float lz_wob(float x, uint32_t seed) {
    int i = lz_floori(x); float f = x - i;
    float a = (float)(lz_hash(i, 7, seed) & 1023) / 511.5f - 1, b = (float)(lz_hash(i + 1, 7, seed) & 1023) / 511.5f - 1;
    f = f * f * (3 - 2 * f);
    return a + (b - a) * f;
}
/* the row y (its centre) through a capsule A..B of radius r: its interval [x0, x1]; false when the row misses it */
static bool lz_cap_row(float ax, float ay, float bx, float by, float r, float yc, float *x0, float *x1) {
    float lo = 1e9f, hi = -1e9f, d = yc - ay;
    if (d * d <= r * r) { float h = sqrtf(r * r - d * d); lo = ax - h; hi = ax + h; }               /* the end discs */
    d = yc - by;
    if (d * d <= r * r) { float h = sqrtf(r * r - d * d); if (bx - h < lo) lo = bx - h; if (bx + h > hi) hi = bx + h; }
    const float vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy;
    if (L2 > 1e-6f) {                                                               /* the band between them */
        const float L = sqrtf(L2), qy = yc - ay;
        float c0 = -1e9f, c1 = 1e9f;                                              /* x - ax: within r of the line ... */
        if (fabsf(vy) > 1e-6f) { c0 = (qy * vx - r * L) / vy; c1 = (qy * vx + r * L) / vy; if (c0 > c1) { float t = c0; c0 = c1; c1 = t; } }
        else if (fabsf(qy) > r) c0 = 1e9f;
        if (fabsf(vx) > 1e-6f) {                                                   /* ... and alongside the segment */
            float d0 = -qy * vy / vx, d1 = (L2 - qy * vy) / vx; if (d0 > d1) { float t = d0; d0 = d1; d1 = t; }
            if (d0 > c0) c0 = d0;
            if (d1 < c1) c1 = d1;
        } else if (qy * vy < 0 || qy * vy > L2) c0 = 1e9f;
        if (c0 <= c1) { if (ax + c0 < lo) lo = ax + c0; if (ax + c1 > hi) hi = ax + c1; }
    }
    if (lo > hi) return false;
    *x0 = lo; *x1 = hi;
    return true;
}

/* ---- the tones of the body's grid ---- */
enum {
    T_NONE,
    T_HI, T_BASE, T_SHADE, T_DEEP,          /* the ground colour in the light's steps (and the silhouette's) */
    T_PHI, T_PBASE, T_PSHADE, T_PDEEP,      /* the markings: bands and spots */
    T_LHI, T_LBASE, T_LSHADE, T_LDEEP,      /* the light rings (a hatchling's collar and tail), an adult's tail bands */
    T_BELLY, T_BELLYSH, T_BELLYDEEP,        /* the belly and the throat */
    T_MOUTH, T_NOSTRIL,                     /* the lips' line; the nostril and the ear */
    T_STUCK, T_STUCKSH,                     /* a stuck shed's white rings */
    T_FLAKE,                                /* the shed: the loose skin's lifted edge, a curl of it standing off the body */
    T_N
};
#define T_LOOSE 0x20                         /* + a tone: under the loosened old skin - the same tone, paled */
static const uint8_t T_DOWN[T_N] = {         /* a tone one step darker (the silhouette, the edge behind a nearer part) */
    T_NONE, T_BASE, T_SHADE, T_DEEP, T_DEEP, T_PBASE, T_PSHADE, T_PDEEP, T_PDEEP, T_LBASE, T_LSHADE, T_LDEEP, T_LDEEP,
    T_BELLYSH, T_BELLYDEEP, T_BELLYDEEP, T_MOUTH, T_NOSTRIL, T_STUCKSH, T_STUCKSH, T_STUCKSH };
/* the live parts' colours, after the grid's 64 in the palette */
enum { P_RIM = 64, P_IRIS, P_IRIS2, P_PUPIL, P_GLINT, P_LID, P_LIDLINE, P_TONGUE, P_TONGUE_TIP, P_MOUTH_IN, P_SAND, P_SAND2,
       P_LEG, P_LEG_E, P_LEGF, P_LEGF_E, P_TOE, P_TOEF, P_STUCK, P_N };

/* ---- the pose's shape: what the body does (the legs are apart) ---- */
enum { C_STAND, C_FLICK, C_STALK, C_LUNGE, C_BASK, C_DIG, C_REST, C_PEEK, C_STANDOFF };
typedef struct {
    float cl;           /* the belly's clearance under the trunk, px at the adult's size (under 0: lying on the ground) */
    float neck, head;   /* their slope toward the tail: + the head carried higher, the nose up */
    float trunk, arch;  /* the trunk's slope (+ the hips lower), the back's arch (rad) */
    float tail0, tail1; /* the tail's slope at its base and at its tip (+ down) */
    float tip;          /* the last third lifted (the stalk's wiggle) */
    float wave;         /* the tail's middle waved (the standoff) */
    float stretch;      /* the neck and the trunk lengthened (the lunge) */
    float breath;       /* -1..1: the trunk's girth */
    float curl;         /* the tail curled round toward the glass (rest) */
    float ground;       /* how much of its half height a part lying on the ground keeps (the rest is pressed flat) */
} lz_shape_t;

/* the cache's key: everything a body's grid is built from, quantized (the build uses the same steps) */
typedef struct {
    const void *geom, *morph;
    uint32_t seed;
    uint16_t scale_q;
    uint8_t  cls, stage, pat, shed, shed_k, peek, breath, wig, reach, yaw_h, yaw_t, fat;
} lz_key_t;
#define Q_YAW   4                    /* the turn's steps from side-on to head-on (a turn builds ~2 x Q_YAW bodies, then keeps them) */
#define Q_FAT   8
#define Q_SHED  10
#define Q_PEEK  12
#define Q_WIG   4
#define Q_REACH 3

/* what the live parts hang from, kept with a body: the chain at the shoulder, the hip, the eye, the snout, the
   snout's next node and the jaw's hinge; the trunk's footprint on the sand (the contact shadow) */
enum { SUM_SHOULDER, SUM_HIP, SUM_EYE, SUM_SNOUT, SUM_NOSE2, SUM_HINGE, SUM_N };
static const float LZ_SUM_S[SUM_N] = { 0.250f, 0.500f, LZ_T_EYE, 0.0f, 0.062f, 0.090f };
typedef struct {
    float P[SUM_N][3], r[SUM_N], w[SUM_N], phi[SUM_N];
    float shx0, shx1, shz0, shz1;            /* the footprint: screen x, and depth x LZ_TILT, from the anchor */
    float sl, juv, scale;
} lz_sum_t;

typedef struct {
    uint32_t key, used;
    int16_t  gx0, gy0;                       /* the grid's corner from the anchor (px, facing right) */
    uint8_t  gw, gh;
    uint8_t  x0[LZ_GH], x1[LZ_GH];           /* each row's first and last cell drawn (x0 > x1: none) */
    lz_sum_t sum;
    uint8_t  tn[LZ_GW * LZ_GH];
} lz_slot_t;
typedef struct { uint16_t c[P_N]; } lz_pal_t;
typedef struct { const void *art, *morph; uint8_t stage, banded, shed; float dim; uint32_t used; lz_pal_t pal; } lz_palc_t;
#define LZ_LAST 8                            /* the lizards whose last body is remembered (the budget's fallback) */
typedef struct { uint32_t seed, key, used; const void *geom; int slot; } lz_last_t;
typedef struct {
    lz_slot_t slot[LZ_SLOTS];
    lz_palc_t pal[LZ_PALS];
    lz_last_t last[LZ_LAST];
    int16_t   zq[LZ_GW * LZ_GH];             /* the build's depth (x16) ... */
    uint8_t   tq[LZ_GW * LZ_GH];             /* ... and place along the body (x255) per cell */
    uint32_t  clock;
} lz_scratch_t;
static lz_scratch_t *g_lz;
static bool g_lz_nocache;
static int  g_lz_builds, g_lz_clipped, g_lz_budget = -1;
static int  g_box[4]; static bool g_box_ok;
size_t lizard_scratch_bytes(void) { return sizeof(lz_scratch_t); }
void   lizard_set_scratch(void *buf) { g_lz = (lz_scratch_t *)buf; if (g_lz) memset(g_lz, 0, sizeof *g_lz); }
void   lizard_frame(int builds) { g_lz_budget = builds; }
void   lizard_cache_on(bool on) { g_lz_nocache = !on; }
int    lizard_cache_builds(void) { return g_lz_builds; }
int    lizard_cache_clipped(void) { return g_lz_clipped; }
static lz_scratch_t *lz_mem(void) { if (!g_lz) g_lz = (lz_scratch_t *)calloc(1, sizeof(lz_scratch_t)); return g_lz; }
bool   lizard_last_box(int *x0, int *y0, int *x1, int *y1) {
    if (!g_box_ok) return false;
    *x0 = g_box[0]; *y0 = g_box[1]; *x1 = g_box[2]; *y1 = g_box[3];
    return true;
}

float lizard_stage_scale(int stage) {
    static const float K[4] = { 0.42f, 0.68f, 1.0f, 1.08f };   /* a hatchling ~8 cm of an adult's ~20 */
    return K[stage < 0 ? 0 : stage > 3 ? 3 : stage];
}
lizard_pose_t lizard_pose(int kind, float yaw) {
    lizard_pose_t p; memset(&p, 0, sizeof p);
    p.kind = (uint8_t)kind; p.yaw = yaw; p.yaw_tail = yaw; p.fat = 0.5f; p.peek = 0.28f; p.shed = SHED_NORMAL;
    return p;
}

/* ---- the skeleton (built with a body; the live parts read its summary) ---- */
typedef struct {
    float X[LZ_N], Y[LZ_N], Z[LZ_N];        /* 3D from the anchor on the floor: along the screen, up, toward the glass */
    float sx[LZ_N], sy[LZ_N];               /* the screen (y down) */
    float r[LZ_N], w[LZ_N], phi[LZ_N];      /* half height, half width, heading in the floor plane (0 right, pi/2 the glass) */
    float L, sl, juv;                       /* the length; the limbs' scale; 1 a hatchling .. 0 grown */
} lz_spine_t;

static void lz_shape_of(int cls, const lz_key_t *k, lz_shape_t *s) {
    memset(s, 0, sizeof *s);
    s->stretch = 1; s->ground = 0.88f;
    const float breath = (float)k->breath - 1, reach = (float)k->reach / Q_REACH;
    const float wig = sinf(6.2831853f * (float)k->wig / Q_WIG);
    switch (cls) {
    default:
    case C_STAND:    s->cl = 2.2f; s->neck = 0.26f; s->head = 0.06f; s->tail0 = 0.34f; break;
    case C_FLICK:    s->cl = 2.5f; s->neck = 0.18f; s->head = -0.12f; s->tail0 = 0.34f; break;
    case C_PEEK:     s->cl = 2.4f; s->neck = 0.30f; s->head = 0.12f; s->tail0 = 0.34f; break;
    case C_STALK:    s->cl = 0.9f; s->neck = 0.04f; s->head = -0.10f; s->tail0 = 0.20f; s->tip = 0.55f + 0.35f * wig; break;
    case C_LUNGE:    s->cl = 1.6f + 1.2f * reach; s->neck = -0.04f; s->head = -0.10f - 0.06f * reach; s->tail0 = 0.26f;
                     s->stretch = 1 + 0.12f * reach; break;
    case C_BASK:     s->cl = -1.4f; s->neck = 0.34f; s->head = 0.16f; s->tail0 = 0.16f; s->breath = breath; s->ground = 0.80f; break;
    case C_DIG:      s->cl = 0.8f; s->neck = -0.30f; s->head = -0.36f; s->trunk = -0.13f; s->tail0 = 0.12f; break;
    case C_REST:     s->cl = -1.2f; s->neck = -0.24f; s->head = -0.06f; s->tail0 = 0.16f; s->breath = breath; s->curl = 1;
                     s->ground = 0.80f; break;
    case C_STANDOFF: s->cl = 5.5f; s->neck = 0.02f; s->head = -0.22f; s->arch = 0.11f; s->tail0 = -0.50f; s->tail1 = 0.10f;
                     s->wave = 0.24f * wig; break;
    }
}

static void lz_spine(lz_spine_t *sp, const lizard_look_t *lk, const lz_shape_t *sh, float scale, float yaw_h, float yaw_t, float fat) {
    const lizard_geom_t *g = lk->geom;
    const float L = g->length_px * scale;
    const float juv = lk->stage == STAGE_FRY ? 1.0f : lk->stage == STAGE_JUV ? 0.5f : 0.0f;
    sp->L = L; sp->juv = juv; sp->sl = powf(scale, 0.9f);
    const float fe = lz_clamp(fat, 0, 1) * lz_clamp(g->tail_fat, 0, 1);
    for (int i = 0; i < LZ_N; i++) {
        float r;
        if (i <= LZ_HIP) {
            r = LZ_RH[i] * L;
            if (i < LZ_SHOULDER) r *= 1 + 0.22f * juv;                                  /* a hatchling's big head */
            else if (i > LZ_SHOULDER && i < LZ_HIP) r *= (1 - 0.08f * juv) * (1 + 0.045f * sh->breath);
        } else {
            int k = i - LZ_HIP;
            r = L * (LZ_RT_THIN[k] + (LZ_RT_FAT[k] - LZ_RT_THIN[k]) * fe);
        }
        sp->r[i] = r;
        sp->w[i] = i < LZ_SHOULDER ? 0.5f * g->head_w * scale * LZ_HW[i] * (1 + 0.15f * juv) : r * (i <= LZ_HIP ? 1.35f : 1.12f);
    }
    /* the profile: u along the body (+ toward the head), h the height, from the hip both ways by each segment's slope */
    float u[LZ_N], h[LZ_N];
    u[LZ_HIP] = 0; h[LZ_HIP] = 0;
    for (int i = LZ_HIP - 1; i >= 0; i--) {
        float seg = (LZ_S[i + 1] - LZ_S[i]) * L, d;
        if (i >= LZ_SHOULDER) { d = sh->trunk + (i < 10 ? -sh->arch : sh->arch); seg *= sh->stretch; }
        else if (i >= 5)      { d = sh->neck; seg *= sh->stretch; }
        else                  d = sh->head;
        u[i] = u[i + 1] + seg * cosf(d); h[i] = h[i + 1] + seg * sinf(d);
    }
    for (int i = LZ_HIP; i < LZ_N - 1; i++) {
        float seg = (LZ_S[i + 1] - LZ_S[i]) * L, ut = (0.5f * (LZ_S[i] + LZ_S[i + 1]) - 0.5f) / 0.5f;
        float sm = ut * ut * (3 - 2 * ut);
        float d = sh->tail0 + (sh->tail1 - sh->tail0) * sm;
        if (ut > 0.62f) d -= sh->tip * (ut - 0.62f) / 0.38f * 2.2f;                      /* the tip lifted (a stalker's wiggle) */
        if (sh->wave != 0) d += sh->wave * sinf(3.14159f * ut * 2.0f);
        u[i + 1] = u[i] - seg * cosf(d); h[i + 1] = h[i] - seg * sinf(d);
    }
    /* seated: the trunk's lowest point at the clearance, then the ground holds up what sinks into it */
    float low = 1e9f;
    for (int i = LZ_SHOULDER; i <= LZ_HIP; i++) if (h[i] - sp->r[i] < low) low = h[i] - sp->r[i];
    const float lift = sh->cl * sp->sl - low;
    for (int i = 0; i < LZ_N; i++) {
        h[i] += lift;
        float gmin = sp->r[i] * sh->ground;
        if (h[i] < gmin) h[i] = gmin;
    }
    /* the turn: each node's heading, the head's at the front, the tail's from 0.8 back, between them in turn; the
       resting tail curls round toward the glass */
    const float ph = acosf(lz_clamp(yaw_h, -1, 1)), pt = acosf(lz_clamp(yaw_t, -1, 1));
    for (int i = 0; i < LZ_N; i++) {
        float s = LZ_S[i], a = s <= 0.25f ? 0 : s >= 0.80f ? 1 : (s - 0.25f) / 0.55f;
        sp->phi[i] = ph + (pt - ph) * a;
        if (sh->curl > 0 && s > 0.60f) { float c = (s - 0.60f) / 0.40f; sp->phi[i] -= sh->curl * 1.7f * c * c; }
    }
    sp->X[LZ_HIP] = 0; sp->Z[LZ_HIP] = 0;
    for (int i = LZ_HIP - 1; i >= 0; i--) {
        float du = u[i] - u[i + 1], pm = 0.5f * (sp->phi[i] + sp->phi[i + 1]);
        sp->X[i] = sp->X[i + 1] + du * cosf(pm); sp->Z[i] = sp->Z[i + 1] + du * sinf(pm);
    }
    for (int i = LZ_HIP; i < LZ_N - 1; i++) {
        float du = u[i + 1] - u[i], pm = 0.5f * (sp->phi[i] + sp->phi[i + 1]);
        sp->X[i + 1] = sp->X[i] + du * cosf(pm); sp->Z[i + 1] = sp->Z[i] + du * sinf(pm);
    }
    for (int i = 0; i < LZ_N; i++) { sp->Y[i] = h[i]; sp->sx[i] = sp->X[i]; sp->sy[i] = -h[i] + sp->Z[i] * LZ_TILT; }
}
/* the chain at s (0..1), and what the live parts read of it */
static void lz_at(const lz_spine_t *sp, float s, float P[3], float *r, float *w, float *phi) {
    int i = 0; while (i < LZ_N - 2 && LZ_S[i + 1] < s) i++;
    float f = lz_clamp((s - LZ_S[i]) / (LZ_S[i + 1] - LZ_S[i]), 0, 1);
    P[0] = sp->X[i] + (sp->X[i + 1] - sp->X[i]) * f; P[1] = sp->Y[i] + (sp->Y[i + 1] - sp->Y[i]) * f; P[2] = sp->Z[i] + (sp->Z[i + 1] - sp->Z[i]) * f;
    *r = sp->r[i] + (sp->r[i + 1] - sp->r[i]) * f;
    *w = sp->w[i] + (sp->w[i + 1] - sp->w[i]) * f;
    *phi = sp->phi[i] + (sp->phi[i + 1] - sp->phi[i]) * f;
}
static void lz_summarize(lz_sum_t *m, const lz_spine_t *sp, float scale) {
    for (int k = 0; k < SUM_N; k++) lz_at(sp, LZ_SUM_S[k], m->P[k], &m->r[k], &m->w[k], &m->phi[k]);
    m->shx0 = m->shz0 = 1e9f; m->shx1 = m->shz1 = -1e9f;
    for (int i = 2; i <= 17; i++) {
        float gx = sp->X[i], gz = sp->Z[i] * LZ_TILT, e = sp->w[i] * 0.9f;
        if (gx - e < m->shx0) m->shx0 = gx - e;
        if (gx + e > m->shx1) m->shx1 = gx + e;
        if (gz - e * LZ_TILT < m->shz0) m->shz0 = gz - e * LZ_TILT;
        if (gz + e * LZ_TILT > m->shz1) m->shz1 = gz + e * LZ_TILT;
    }
    m->sl = sp->sl; m->juv = sp->juv; m->scale = scale;
}

/* ---- the build: the body's tones into a grid ---- */
typedef struct {
    float L0, scale, iscale;             /* the length at scale 1, the scale (and 1 over it) */
    uint32_t seed;
    bool  banded;                        /* a hatchling's or a juvenile's bands */
    float breakup;                       /* ... breaking into spots (a juvenile) */
    float spots;                         /* an adult's spots (0: none) */
    int   shed; float shed_k, peek;
} lz_bctx_t;

static inline bool lz_spot(float s, float a, float cs, float rmin, float rmax, float density, uint32_t seed) {
    const float ic = 1.0f / cs;                                                      /* (a constant at every call: folded) */
    int ci = lz_floori(s * ic), cj = lz_floori(a * ic);
    uint32_t hh = lz_hash(ci, cj, seed);
    if ((float)(hh & 255) >= density * 256.0f) return false;
    float rad = rmin + (rmax - rmin) * (float)((hh >> 8) & 15) * (1.0f / 15.0f);
    float room = cs - 2 * rad; if (room < 0) room = 0;
    float cx = ci * cs + rad + room * (float)((hh >> 12) & 255) * (1.0f / 255.0f), cy = cj * cs + rad + room * (float)((hh >> 20) & 255) * (1.0f / 255.0f);
    float dx = s - cx, dy = a - cy;
    return dx * dx + dy * dy < rad * rad;
}
/* the markings at a point of the body: 0 the ground, 1 a dark marking, 2 a light ring */
static int lz_pat(const lz_bctx_t *b, float t, float s, float a, float v) {
    if (b->banded) {
        float tb = t + 0.014f * lz_wob(a * 0.45f + 3.0f, b->seed);
        int p;
        if (tb < 0.050f) p = 0;                                                 /* the snout */
        else if (tb < LZ_T_HEAD + 0.003f) p = v < 0.30f ? 1 : 0;                 /* the dark cap */
        else if (tb < 0.222f) p = 2;                                            /* the collar */
        else if (tb < LZ_T_TAIL + 0.01f) p = (tb < 0.300f || (tb >= 0.356f && tb < 0.440f) || tb >= 0.496f) ? 1 : 0;
        else { float q = (tb - 0.560f) / 0.440f * 4.6f; p = lz_frac(q) < 0.42f ? 2 : 1; }   /* the tail's rings */
        if (b->breakup > 0 && t > 0.05f) {                                       /* a juvenile's bands breaking into spots */
            if (p == 1 && lz_spot(s, a, 4.4f, 0.9f, 1.4f, 0.55f * b->breakup, b->seed ^ 0x5bd1e995u)) p = 0;
            else if (p == 0 && lz_spot(s, a, 5.0f, 1.0f, 1.5f, 0.45f * b->breakup, b->seed ^ 0x27d4eb2fu)) p = 1;
        }
        return p;
    }
    if (b->spots <= 0) return 0;
    int p = 0;
    if (t > 0.585f && lz_frac((t - 0.585f) / 0.415f * 4.0f) < 0.30f) p = 2;      /* an adult's faint tail bands */
    if (t < LZ_T_HEAD) { if (lz_spot(s, a, 2.6f, 0.55f, 0.85f, b->spots * 0.85f, b->seed)) p = 1; }   /* the head's fine spots */
    else if (lz_spot(s, a, 3.5f, 0.70f, 1.15f, b->spots, b->seed ^ 0x9e3779b9u)) p = 1;
    return p;
}
static uint8_t lz_tone(const lz_bctx_t *b, float t, float v, float a, float R) {
    const float s = t * b->L0;
    uint8_t tone;
    if (t < LZ_T_HEAD && t > 0.006f && t < 0.118f && fabsf(v - (0.40f - 1.25f * (t - 0.006f))) * R < 0.55f)
        tone = T_MOUTH;                                                         /* the lips' line, rising to the back */
    else if (t < 0.021f && t > 0.008f && v > -0.45f && v < -0.10f) tone = T_NOSTRIL;
    else if (t > 0.130f && t < 0.147f && v > -0.12f && v < 0.22f) tone = T_NOSTRIL;   /* the ear behind the eye */
    else {
        const float belly = t < LZ_T_HEAD ? 0.62f : t < LZ_T_TAIL ? 0.70f : 0.80f;
        const int sh = v < -0.56f ? 0 : v < 0.22f ? 1 : 2;
        if (v > belly) tone = v > 0.88f ? T_BELLYSH : T_BELLY;
        else {
            int p = lz_pat(b, t, s, a, v);
            tone = (uint8_t)((p == 1 ? T_PHI : p == 2 ? T_LHI : T_HI) + sh);
        }
        if (b->shed == SHED_STUCK && t > 0.880f && lz_frac((t - 0.880f) / 0.120f * 3.0f) < 0.50f)   /* stuck: the old skin's rings, */
            tone = (lz_hash((int)(s * 2), (int)(a * 2), b->seed) & 3) == 0 || v > 0.35f ? T_STUCKSH : T_STUCK;   /* ragged, round the tail tip */
    }
    if (b->shed == SHED_SHEDDING) {                                            /* the old skin loose from the nose back to a ragged */
        const float tp = b->shed_k + 0.035f * lz_wob(a * 0.6f + 11.0f, b->seed);   /* front: its lifted edge pale, a shadow under it */
        if (t < tp - 0.022f) tone |= T_LOOSE;
        else if (t < tp) tone = T_FLAKE;
        else if (t < tp + 0.012f && tone < T_MOUTH) tone = T_DOWN[T_DOWN[tone]];
    }
    return tone;
}

static void lz_build(lz_slot_t *sl, const lz_spine_t *sp, const lz_bctx_t *b) {
    lz_scratch_t *m = lz_mem();
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < LZ_N; i++) {
        if (LZ_S[i] > b->peek + 0.04f) break;
        float e = sp->r[i] > sp->w[i] ? sp->r[i] : sp->w[i];
        if (sp->sx[i] - e < minx) minx = sp->sx[i] - e;
        if (sp->sx[i] + e > maxx) maxx = sp->sx[i] + e;
        if (sp->sy[i] - e < miny) miny = sp->sy[i] - e;
        if (sp->sy[i] + e > maxy) maxy = sp->sy[i] + e;
    }
    int gx0 = lz_floori(minx) - 2, gy0 = lz_floori(miny) - 2, gw = (int)ceilf(maxx) - gx0 + 3, gh = (int)ceilf(maxy) - gy0 + 3;
    if (gw > LZ_GW) { gx0 += (gw - LZ_GW) / 2; gw = LZ_GW; g_lz_clipped++; }     /* (never at the sizes the packs use: the sheet counts) */
    if (gh > LZ_GH) { gy0 += gh - LZ_GH; gh = LZ_GH; g_lz_clipped++; }
    sl->gx0 = (int16_t)gx0; sl->gy0 = (int16_t)gy0; sl->gw = (uint8_t)gw; sl->gh = (uint8_t)gh;
    const int n = gw * gh;
    for (int c = 0; c < n; c++) m->zq[c] = INT16_MIN;
    memset(sl->tn, 0, (size_t)n);
    memset(m->tq, 0, (size_t)n);
    /* each segment: a capsule on the screen, wider across where it points at the glass (its width shows: x is shrunk
       by kx), whose cells - row by row, inside its interval - nearer than what is there take the segment's place
       along the body and round it */
    for (int i = 0; i < LZ_N - 1; i++) {
        if (LZ_S[i] > b->peek) break;
        const float ay = sp->sy[i], by = sp->sy[i + 1];
        const float ra = sp->r[i], rb = sp->r[i + 1], za = sp->Z[i], zb = sp->Z[i + 1];
        const float sphi = fabsf(sinf(0.5f * (sp->phi[i] + sp->phi[i + 1])));
        const float wr = 0.5f * (sp->w[i] / (ra > 0.1f ? ra : 0.1f) + sp->w[i + 1] / (rb > 0.1f ? rb : 0.1f));
        const float kx = 1.0f / (1 + (wr - 1) * sphi), ikx = 1.0f / kx;
        const float ax = sp->sx[i] * kx, bx = sp->sx[i + 1] * kx;                 /* (in the shrunk x) */
        const float rmax = ra > rb ? ra : rb;
        int y0 = lz_floori((ay < by ? ay : by) - rmax) - gy0, y1 = (int)ceilf((ay > by ? ay : by) + rmax) - gy0;
        if (y0 < 0) y0 = 0;
        if (y1 > gh - 1) y1 = gh - 1;
        const float vx = bx - ax, vy = by - ay, vv = vx * vx + vy * vy, inv = vv > 1e-6f ? 1.0f / vv : 0;
        for (int gy = y0; gy <= y1; gy++) {
            const float py = gy0 + gy + 0.5f, qy = py - ay;
            float lo, hi;
            if (!lz_cap_row(ax, ay, bx, by, rmax, py, &lo, &hi)) continue;
            int x0 = (int)ceilf(lo * ikx - 0.5f) - gx0, x1 = lz_floori(hi * ikx - 0.5f) - gx0;
            if (x0 < 0) x0 = 0;
            if (x1 > gw - 1) x1 = gw - 1;
            for (int gx = x0; gx <= x1; gx++) {
                const float qx = (gx0 + gx + 0.5f) * kx - ax;
                const float ue = (qx * vx + qy * vy) * inv;                            /* along it, past its ends too ... */
                const float uu = ue < 0 ? 0 : ue > 1 ? 1 : ue;
                const float z = za + (zb - za) * uu;
                const int c = gy * gw + gx; const int16_t zi = (int16_t)(z * 16);      /* (|z| is a body's length at most) */
                if (zi <= m->zq[c]) continue;                                          /* a part as near is there already */
                const float dx = qx - uu * vx, dy = qy - uu * vy, d2 = dx * dx + dy * dy, R = ra + (rb - ra) * uu;
                if (d2 >= R * R) continue;
                if (py > z * LZ_TILT + 0.5f) continue;                                 /* under the sand at its depth */
                const float t = lz_clamp(LZ_S[i] + (LZ_S[i + 1] - LZ_S[i]) * ue, 0, 1);  /* ... so a cap's cells keep their own place along the
                                                                                           body, whichever segment reaches them first */
                if (t > b->peek) continue;
                const float v = lz_clamp(dy / R, -1, 1);
                m->zq[c] = zi; m->tq[c] = (uint8_t)(t * 255);
                sl->tn[c] = lz_tone(b, t, v, R * b->iscale * lz_asin(v), R);
            }
        }
    }
    /* each row's extent, for the pass below and the blit */
    for (int gy = 0; gy < gh; gy++) {
        int a = 255, z = -1;
        const uint8_t *row = sl->tn + gy * gw;
        for (int gx = 0; gx < gw; gx++) if (row[gx]) { if (a == 255) a = gx; z = gx; }
        sl->x0[gy] = (uint8_t)(a == 255 ? 1 : a); sl->x1[gy] = (uint8_t)(z < 0 ? 0 : z);
    }
    /* the silhouette and the edge behind a nearer part step down; shedding or stuck, curls of old skin stand off the
       peel's edge and the stuck rings; peeking, the part still in the dark of the hide's door sinks into it */
    static const int NB[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    const bool flakes = b->shed == SHED_SHEDDING || b->shed == SHED_STUCK;
    for (int gy = 0; gy < gh; gy++) {
        int a = sl->x0[gy], z = sl->x1[gy];                                         /* this row's and its neighbours' extent, a cell out */
        if (gy > 0 && sl->x0[gy - 1] <= sl->x1[gy - 1]) { if (sl->x0[gy - 1] < a) a = sl->x0[gy - 1]; if (sl->x1[gy - 1] > z) z = sl->x1[gy - 1]; }
        if (gy < gh - 1 && sl->x0[gy + 1] <= sl->x1[gy + 1]) { if (sl->x0[gy + 1] < a) a = sl->x0[gy + 1]; if (sl->x1[gy + 1] > z) z = sl->x1[gy + 1]; }
        if (a > z) continue;
        a = a > 0 ? a - 1 : 0; z = z < gw - 1 ? z + 1 : gw - 1;
        for (int gx = a; gx <= z; gx++) {
            const int c = gy * gw + gx; uint8_t tn = sl->tn[c];
            if (!tn) {
                if (!flakes) continue;
                for (int k = 0; k < 4; k++) {                                          /* (k 2: the cell above - a curl stands up, never hangs) */
                    int nx = gx + NB[k][0], ny = gy + NB[k][1];
                    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || k == 2) continue;
                    uint8_t nt = sl->tn[ny * gw + nx];
                    if (!nt || (nt & 0x80)) continue;
                    nt &= 31;
                    if ((nt == T_FLAKE || nt == T_STUCK || nt == T_STUCKSH) && (lz_hash(gx, gy, b->seed) & 7) < 3) { sl->tn[c] = T_FLAKE | 0x80; break; }
                }
                continue;
            }
            if (tn & 0x80) continue;                                                    /* (a curl just placed) */
            bool edge = false, behind = false;
            for (int k = 0; k < 4; k++) {
                int nx = gx + NB[k][0], ny = gy + NB[k][1];
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) { edge = true; continue; }
                int nc = ny * gw + nx; uint8_t nt = sl->tn[nc];
                if (!nt || (nt & 0x80)) edge = true;
                else if (m->zq[nc] > m->zq[c] + 48) behind = true;                     /* 3 px nearer: a part in front */
            }
            uint8_t base = tn & 31, loose = tn & T_LOOSE;
            if (edge || behind) base = T_DOWN[base];
            if (b->peek < 1) {
                const float tt = m->tq[c] / 255.0f;
                if (tt > b->peek - 0.03f) base = T_NOSTRIL; else if (tt > b->peek - 0.08f) base = T_DOWN[T_DOWN[base]];
            }
            sl->tn[c] = (uint8_t)(base | loose | 0x40);                                /* (0x40: stepped; cleared below) */
        }
    }
    for (int gy = 0; gy < gh; gy++) {                                               /* the pass's marks off; the extents again (the curls) */
        int a = 255, z = -1;
        uint8_t *row = sl->tn + gy * gw;
        for (int gx = 0; gx < gw; gx++) if (row[gx]) { row[gx] &= 0x3f; if (a == 255) a = gx; z = gx; }
        sl->x0[gy] = (uint8_t)(a == 255 ? 1 : a); sl->x1[gy] = (uint8_t)(z < 0 ? 0 : z);
    }
    g_lz_builds++;
}

/* ---- the palette (the last few kept: a morph, a stage, a shed, the night's dim) ---- */
static const lz_art_t *lz_art_of(const morph_t *m, lz_art_t *own) {
    if (m && m->name) for (int i = 0; i < LZ_N_ARTS; i++) if (strcmp(m->name, LZ_ARTS[i].name) == 0) return &LZ_ARTS[i];
    uint32_t body = m ? m->body : 0xd8b878, acc = m ? m->accent : 0x403028;    /* a morph not listed: its two colours */
    own->name = NULL; own->ground = body; own->pattern = acc;
    own->hatch_band = acc; own->hatch_ground = body;
    own->light = lz_mix(body, 0xffffff, 0.70f); own->belly = lz_mix(body, 0xffffff, 0.75f);
    own->shade = lz_mix(body, 0x301808, 0.70f);
    own->iris = 0x504840; own->pupil = 0x101010; own->rim = 0x242020; own->spots = 0.80f; own->wash = 0;
    return own;
}
static void lz_palette(lz_pal_t *P, const lizard_look_t *lk, const lz_art_t *art, bool banded, int shed, float dim) {
    const morph_t *m = lk->morph;
    uint32_t g = art->ground ? art->ground : m ? m->body : 0xd8b878;
    uint32_t p = art->pattern ? art->pattern : m ? m->accent : 0x403028;
    uint32_t l = art->light, b = art->belly, sh = art->shade;
    if (banded && art->hatch_band) { g = art->hatch_ground; p = art->hatch_band; }
    else if (!banded) {
        l = lz_mix(g, l, 0.55f);                                                    /* an adult's tail bands are faint */
        if (art->wash > 0) g = lz_mix(g, 0xf0dc84, art->wash);                       /* the mack snow's yellow back */
    }
    if (lk->stage == STAGE_ELDER) { g = lz_mix(g, 0xd6cebe, 0.14f); p = lz_mix(p, 0x5a524a, 0.12f); }   /* faded a little */
    uint32_t c[T_N];
    c[T_NONE] = 0;
    c[T_HI] = lz_mix(g, 0xffffff, 0.30f); c[T_BASE] = g; c[T_SHADE] = lz_mix(g, sh, 0.36f); c[T_DEEP] = lz_mix(g, sh, 0.66f);
    c[T_PHI] = lz_mix(p, 0xffffff, 0.10f); c[T_PBASE] = p; c[T_PSHADE] = lz_mix(p, 0x000000, 0.22f); c[T_PDEEP] = lz_mix(p, 0x000000, 0.42f);
    c[T_LHI] = lz_mix(l, 0xffffff, 0.25f); c[T_LBASE] = l; c[T_LSHADE] = lz_mix(l, sh, 0.30f); c[T_LDEEP] = lz_mix(l, sh, 0.56f);
    c[T_BELLY] = b; c[T_BELLYSH] = lz_mix(b, sh, 0.24f); c[T_BELLYDEEP] = lz_mix(b, sh, 0.46f);
    c[T_MOUTH] = lz_mix(sh, 0x000000, 0.45f); c[T_NOSTRIL] = lz_mix(sh, 0x000000, 0.60f);
    c[T_STUCK] = 0xf6f4ee; c[T_STUCKSH] = 0xc9c4ba; c[T_FLAKE] = 0xe9e5dc;
    const bool dull = shed == SHED_SOON || shed == SHED_SHEDDING;               /* docs/shed.md: paling, dull */
    for (int k = 1; k < T_N; k++) {
        uint32_t v = c[k];
        if (dull && k < T_STUCK) v = lz_mix(lz_desat(v, 0.50f), 0xd6d2ca, 0.20f);
        P->c[k] = lz_565(v, dim);
        P->c[k | T_LOOSE] = lz_565(lz_desat(lz_mix(c[k], 0xf0ece6, 0.68f), 0.45f), dim);   /* the old skin, loose: papery */
    }
    for (int k = T_N; k < 32; k++) P->c[k] = P->c[k | T_LOOSE] = P->c[T_BASE];
    P->c[0] = P->c[T_LOOSE] = P->c[T_BASE];
    /* the live parts */
    uint32_t iris = art->iris, iris2 = lz_mix(art->iris, 0x000000, 0.30f), pupil = art->pupil, rim = art->rim;
    if (shed == SHED_SOON) { iris = 0x9aaabb; iris2 = 0x8494a6; pupil = 0x5c6a7a; rim = 0x6c7888; }   /* the eyes go blue-grey */
    const uint32_t lid = banded && art->hatch_band ? p : g;                       /* the lids are the head's own */
    uint32_t gl = g, ps = p;
    if (dull) { gl = lz_mix(lz_desat(g, 0.50f), 0xd6d2ca, 0.20f); ps = lz_mix(lz_desat(p, 0.50f), 0xd6d2ca, 0.20f); }
    const uint32_t lidc = dull ? (lid == p ? ps : gl) : lid;
    P->c[P_RIM] = lz_565(rim, dim); P->c[P_IRIS] = lz_565(iris, dim); P->c[P_IRIS2] = lz_565(iris2, dim);
    P->c[P_PUPIL] = lz_565(pupil, dim); P->c[P_GLINT] = lz_565(0xffffff, dim);
    P->c[P_LID] = lz_565(lidc, dim); P->c[P_LIDLINE] = lz_565(lz_mix(lidc, 0x000000, 0.42f), dim);
    P->c[P_TONGUE] = lz_565(0xe0687a, dim); P->c[P_TONGUE_TIP] = lz_565(0xf4a6b0, dim); P->c[P_MOUTH_IN] = lz_565(0xa63a4a, dim);
    P->c[P_SAND] = lz_565(0xb48c60, dim); P->c[P_SAND2] = lz_565(0x8e6c48, dim);   /* sand from below the top: darker, damper */
    P->c[P_LEG] = lz_565(gl, dim); P->c[P_LEG_E] = lz_565(lz_mix(gl, sh, 0.45f), dim);
    P->c[P_LEGF] = lz_565(lz_mix(gl, sh, 0.40f), dim); P->c[P_LEGF_E] = lz_565(lz_mix(gl, sh, 0.70f), dim);
    P->c[P_TOE] = lz_565(lz_mix(gl, b, 0.35f), dim); P->c[P_TOEF] = lz_565(lz_mix(gl, sh, 0.50f), dim);
    P->c[P_STUCK] = lz_565(0xf6f4ee, dim);
}
static const lz_pal_t *lz_palette_of(lz_scratch_t *m, const lizard_look_t *lk, const lz_art_t *art, bool banded, int shed, float dim) {
    int old = 0;
    for (int i = 0; i < LZ_PALS; i++) {
        lz_palc_t *e = &m->pal[i];
        if (e->used && e->art == (const void *)art && e->morph == (const void *)lk->morph && e->stage == lk->stage && e->banded == banded
            && e->shed == shed && e->dim == dim && art != NULL && art->name != NULL) { e->used = ++m->clock; return &e->pal; }
        if (m->pal[i].used < m->pal[old].used) old = i;
    }
    lz_palc_t *e = &m->pal[old];                                                     /* (a morph not listed: its art is on the stack - never kept) */
    lz_palette(&e->pal, lk, art, banded, shed, dim);
    e->art = art; e->morph = lk->morph; e->stage = lk->stage; e->banded = (uint8_t)banded; e->shed = (uint8_t)shed; e->dim = dim;
    e->used = art->name ? ++m->clock : 0;
    return &e->pal;
}

/* ---- writing pixels: clipped to the canvas, marked by the run ---- */
typedef struct { const lz_canvas_t *cv; int bx0, by0, bx1, by1; bool any; } lz_out_t;
static inline void lz_box(lz_out_t *o, int x0, int x1, int y) {
    if (!o->any) { o->bx0 = x0; o->bx1 = x1; o->by0 = o->by1 = y; o->any = true; return; }
    if (x0 < o->bx0) o->bx0 = x0;
    if (x1 > o->bx1) o->bx1 = x1;
    if (y < o->by0) o->by0 = y;
    if (y > o->by1) o->by1 = y;
}
static inline bool lz_clip(const lz_canvas_t *cv, int *x0, int *x1, int y) {
    if ((unsigned)(y - cv->oy) >= (unsigned)cv->h) return false;
    if (*x0 < cv->ox) *x0 = cv->ox;
    if (*x1 > cv->ox + cv->w - 1) *x1 = cv->ox + cv->w - 1;
    return *x0 <= *x1;
}
static void lz_run(lz_out_t *o, int x0, int x1, int y, uint16_t c) {
    const lz_canvas_t *cv = o->cv;
    if (!lz_clip(cv, &x0, &x1, y)) return;
    uint16_t *p = cv->fb + (size_t)(y - cv->oy) * cv->stride + (x0 - cv->ox);
    for (int x = x0; x <= x1; x++) *p++ = c;
    if (cv->mark) cv->mark(cv->user, x0, x1, y);
    lz_box(o, x0, x1, y);
}
static inline void lz_px(lz_out_t *o, int x, int y, uint16_t c) { lz_run(o, x, x, y, c); }
static void lz_darken_run(lz_out_t *o, int x0, int x1, int y, int a) {             /* toward black by a/256 */
    const lz_canvas_t *cv = o->cv;
    if (a <= 0 || !lz_clip(cv, &x0, &x1, y)) return;
    uint16_t *p = cv->fb + (size_t)(y - cv->oy) * cv->stride + (x0 - cv->ox);
    const int inv = 256 - a;
    for (int x = x0; x <= x1; x++, p++) {
        const uint16_t v = FB_SWAP16(*p);
        *p = FB_SWAP16((uint16_t)(((((v >> 11) * inv) >> 8) << 11) | (((((v >> 5) & 63) * inv) >> 8) << 5) | (((v & 31) * inv) >> 8)));
    }
    if (cv->mark) cv->mark(cv->user, x0, x1, y);
    lz_box(o, x0, x1, y);
}
/* a row of a soft ellipse's shadow: x0..x1 on row y, d = ((x - cx) / rx)^2 + dy2 under 1, darkened by
   strong (1 - d)^2 + 18 (1 - d) - the soft falloff, worked out along the run (no divide, no root) */
static void lz_shadow_run(lz_out_t *o, int x0, int x1, int y, float cx, float irx, float dy2, float strong) {
    const lz_canvas_t *cv = o->cv;
    if (!lz_clip(cv, &x0, &x1, y)) return;
    uint16_t *p = cv->fb + (size_t)(y - cv->oy) * cv->stride + (x0 - cv->ox);
    for (int x = x0; x <= x1; x++, p++) {
        const float dx = (x + 0.5f - cx) * irx, e = 1 - dx * dx - dy2;
        if (e <= 0) continue;
        const int inv = 256 - (int)(strong * e * e + 18 * e);
        const uint16_t v = FB_SWAP16(*p);
        *p = FB_SWAP16((uint16_t)(((((v >> 11) * inv) >> 8) << 11) | (((((v >> 5) & 63) * inv) >> 8) << 5) | (((v & 31) * inv) >> 8)));
    }
    if (cv->mark) cv->mark(cv->user, x0, x1, y);
    lz_box(o, x0, x1, y);
}
/* a capsule A..B of radius r, row by row; shade 1: its lower rim in the edge colour (a limb lit from above - the
   capsule in the edge colour a little lower, the fill over it a little thinner) */
static void lz_capsule1(lz_out_t *o, float ax, float ay, float bx, float by, float r, uint16_t c) {
    const int y0 = lz_floori((ay < by ? ay : by) - r), y1 = (int)ceilf((ay > by ? ay : by) + r);
    for (int y = y0; y <= y1; y++) {
        float lo, hi;
        if (!lz_cap_row(ax, ay, bx, by, r, y + 0.5f, &lo, &hi)) continue;
        const int x0 = (int)ceilf(lo - 0.5f), x1 = lz_floori(hi - 0.5f);
        if (x0 <= x1) lz_run(o, x0, x1, y, c);
    }
}
static void lz_capsule(lz_out_t *o, float ax, float ay, float bx, float by, float r, uint16_t fill, uint16_t edge, bool shade) {
    if (!shade || r < 1.0f) { lz_capsule1(o, ax, ay, bx, by, r, fill); return; }
    lz_capsule1(o, ax, ay + 0.6f, bx, by + 0.6f, r, edge);
    lz_capsule1(o, ax, ay, bx, by, r - 0.5f, fill);
}
static void lz_ellipse(lz_out_t *o, float cx, float cy, float rx, float ry, uint16_t fill, uint16_t edge) {
    for (int y = lz_floori(cy - ry); y <= (int)ceilf(cy + ry); y++) {
        const float dy = (y + 0.5f - cy) / ry, k = 1 - dy * dy;
        if (k <= 0) continue;
        const float hw = rx * sqrtf(k);
        const int x0 = (int)ceilf(cx - hw - 0.5f), x1 = lz_floori(cx + hw - 0.5f);
        if (x0 <= x1) lz_run(o, x0, x1, y, dy > 0.4f ? edge : fill);
    }
}

/* ---- the legs ---- */
typedef struct { float root[3], knee[3], foot[3], toe[4][3]; float r1, r2; bool near, hind, show; } lz_leg_t;
static void lz_v3(float o[3], float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void lz_ik(lz_leg_t *lg, float l1, float l2, const float pole[3]) {
    float d[3] = { lg->foot[0] - lg->root[0], lg->foot[1] - lg->root[1], lg->foot[2] - lg->root[2] };
    float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 1e-3f) dl = 1e-3f;
    if (dl > (l1 + l2) * 0.995f) {                                               /* out of reach: the foot comes in */
        float k = (l1 + l2) * 0.995f / dl;
        for (int j = 0; j < 3; j++) { d[j] *= k; lg->foot[j] = lg->root[j] + d[j]; }
        dl = (l1 + l2) * 0.995f;
    }
    float n[3] = { d[0] / dl, d[1] / dl, d[2] / dl };
    float a = (l1 * l1 - l2 * l2 + dl * dl) / (2 * dl), hh = l1 * l1 - a * a; hh = hh > 0 ? sqrtf(hh) : 0;
    float pd = pole[0] * n[0] + pole[1] * n[1] + pole[2] * n[2];
    float b[3] = { pole[0] - pd * n[0], pole[1] - pd * n[1], pole[2] - pd * n[2] };
    float bl = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]); if (bl < 1e-4f) { b[0] = 0; b[1] = 1; b[2] = 0; bl = 1; }
    for (int j = 0; j < 3; j++) lg->knee[j] = lg->root[j] + n[j] * a + b[j] / bl * hh;
}
/* where the four feet are for the pose, and the bones to them. 0 near front, 1 far front, 2 near hind, 3 far hind */
static void lz_legs(lz_leg_t lg[4], const lz_sum_t *sm, const lizard_look_t *lk, int kind, int cls, float phase, float reach, float peek) {
    const float limb = lk->geom->limb_len * sm->sl;
    static const float TOE_C[4] = { 0.8678f, 0.9856f, 0.9856f, 0.8678f }, TOE_S[4] = { -0.4969f, -0.1692f, 0.1692f, 0.4969f };   /* the fan: +-0.52, +-0.17 */
    static const float TOE_LEN[4] = { 0.17f, 0.21f, 0.23f, 0.19f };
    for (int k = 0; k < 4; k++) {
        lz_leg_t *L = &lg[k];
        const bool hind = k >= 2, near = (k & 1) == 0;
        const int at = hind ? SUM_HIP : SUM_SHOULDER;
        const float side = near ? 1.0f : -1.0f;
        L->hind = hind; L->near = near; L->show = LZ_SUM_S[at] < peek - 0.02f;
        const float *P = sm->P[at], r = sm->r[at], w = sm->w[at], phi = sm->phi[at];
        const float fx = cosf(phi), fz = sinf(phi), lx = -fz * side, lz = fx * side;   /* forward, and out to its side */
        lz_v3(L->root, P[0] + lx * w * 0.62f, P[1] - 0.30f * r, P[2] + lz * w * 0.62f);
        /* the foot, in the body's frame at the root: along, out, up (x limb) */
        float al, out, up = 0;
        float stride = 0, liftk = 0, ph = phase + ((k == 0 || k == 3) ? 0.0f : 0.5f);   /* the diagonal pairs */
        switch (cls) {
        case C_STALK:    al = hind ? -0.02f : 0.30f; out = hind ? 0.42f : 0.40f; stride = kind == LZ_POSE_STALK ? 0.18f : 0; liftk = 0.12f; break;
        case C_LUNGE:    al = hind ? -0.08f - 0.40f * reach : 0.25f + 0.55f * reach; out = hind ? 0.34f : 0.30f;
                         up = hind ? 0 : 0.30f * sinf(3.14159f * reach); break;
        case C_BASK:     al = hind ? -0.02f : 0.12f; out = hind ? 0.52f : 0.56f; break;
        case C_REST:     al = hind ? -0.40f : -0.32f; out = hind ? 0.18f : 0.08f; break;
        case C_STANDOFF: al = hind ? -0.10f : 0.16f; out = hind ? 0.20f : 0.16f; break;
        case C_DIG:
            if (hind) { al = -0.18f; out = 0.30f; }
            else {                                                               /* the front feet scrabble, in turn: down in front of the */
                float q = lz_frac(phase * 1.0f + (near ? 0 : 0.5f));             /* chin, raked back, lifted forward again */
                if (q < 0.5f) { float e = q / 0.5f; al = 0.62f - 0.85f * e; up = 0.0f; }
                else { float e = (q - 0.5f) / 0.5f; al = -0.23f + 0.85f * e; up = 0.42f * sinf(3.14159f * e); }
                out = 0.22f;
            }
            break;
        default:         al = hind ? -0.06f : 0.22f; out = hind ? 0.30f : 0.26f;
                         if (kind == LZ_POSE_WALK) { stride = 0.45f; liftk = 0.22f; }
                         else if (kind == LZ_POSE_TURN) { stride = 0.22f; liftk = 0.16f; }
                         break;
        }
        if (stride > 0) {                                                        /* the walk: on the sand, then a step forward */
            float q = lz_frac(ph);
            if (q < LZ_DUTY) al += stride * (0.5f - q / LZ_DUTY);
            else { float e = (q - LZ_DUTY) / (1 - LZ_DUTY); al += stride * (e - 0.5f); up = liftk * sinf(3.14159f * e); }
        }
        const float ox = P[0] + fx * al * limb, oz = P[2] + fz * al * limb;
        lz_v3(L->foot, ox + lx * (w + out * limb), up * limb, oz + lz * (w + out * limb));
        const float l1 = limb * (hind ? 0.56f : 0.50f), l2 = limb * (hind ? 0.60f : 0.54f);
        float pole[3];
        if (hind) lz_v3(pole, 0.75f * fx + 0.40f * lx, 0.60f, 0.75f * fz + 0.40f * lz);   /* the knee forward and out */
        else      lz_v3(pole, -0.85f * fx + 0.35f * lx, 0.55f, -0.85f * fz + 0.35f * lz); /* the elbow back and out */
        lz_ik(L, l1, l2, pole);
        L->r1 = limb * 0.15f; L->r2 = limb * 0.125f;
        /* the toes: four fanned on the sand, the front's forward and out (0.35 rad), the hind's out and back (0.95) */
        const float cb = hind ? 0.5817f : 0.9394f, sb = hind ? 0.8134f : 0.3429f;
        const float dx = cb * fx + sb * lx, dz = cb * fz + sb * lz;                  /* the foot's way */
        for (int j = 0; j < 4; j++) {
            float tx = TOE_C[j] * dx - TOE_S[j] * dz, tz = TOE_C[j] * dz + TOE_S[j] * dx, len = TOE_LEN[j] * limb;
            if (len < 1.3f) len = 1.3f;
            float ty = L->foot[1] > 0.3f ? L->foot[1] - 0.25f * len : 0;
            lz_v3(L->toe[j], L->foot[0] + tx * len, ty, L->foot[2] + tz * len);
        }
    }
}

/* ---- the anchor's transform ---- */
typedef struct { int ax, ay; bool mirror; } lz_xf_t;
static inline void lz_scr(const lz_xf_t *xf, const float p[3], float *x, float *y) {
    float lx = p[0], ly = -p[1] + p[2] * LZ_TILT;
    *x = xf->mirror ? xf->ax - lx : xf->ax + lx; *y = xf->ay + ly;
}
static void lz_draw_leg(lz_out_t *o, const lz_xf_t *xf, const lz_leg_t *L, const lz_pal_t *P, bool stuck) {
    float rx, ry, kx, ky, fx, fy;
    lz_scr(xf, L->root, &rx, &ry); lz_scr(xf, L->knee, &kx, &ky); lz_scr(xf, L->foot, &fx, &fy);
    const uint16_t c = L->near ? P->c[P_LEG] : P->c[P_LEGF], e = L->near ? P->c[P_LEG_E] : P->c[P_LEGF_E];
    const uint16_t toe = L->near ? P->c[P_TOE] : P->c[P_TOEF];
    for (int j = 0; j < 4; j++) {                                                /* the toes first: the foot sits on them */
        float tx, ty; lz_scr(xf, L->toe[j], &tx, &ty);
        lz_capsule1(o, fx, fy, tx, ty, 0.55f, toe);
        if (stuck) lz_capsule1(o, fx + (tx - fx) * 0.45f, fy + (ty - fy) * 0.45f, tx, ty, 0.55f, P->c[P_STUCK]);   /* a white ring */
    }
    float fr = L->r2 * 1.15f; if (fr < 0.7f) fr = 0.7f;
    lz_ellipse(o, fx, fy - 0.2f, fr * 1.25f, fr * 0.8f, c, e);
    float r2 = L->r2 < 0.55f ? 0.55f : L->r2, r1 = L->r1 < 0.6f ? 0.6f : L->r1;
    lz_capsule(o, kx, ky, fx, fy, r2, c, e, true);
    lz_capsule(o, rx, ry, kx, ky, r1, c, e, true);
}

/* ---- the eye: rim, iris, the slit pupil, a glint, and the lids ---- */
static void lz_draw_eye(lz_out_t *o, float ex, float ey, float rx, float ry, float blink, float front, const lz_pal_t *P) {
    const bool shut = blink >= 0.88f;
    const float glx = ex + front * 0.30f * rx, gly = ey - 0.42f * ry, irx = 1 / rx, iry = 1 / ry;
    for (int y = lz_floori(ey - ry); y <= (int)ceilf(ey + ry); y++)
        for (int x = lz_floori(ex - rx); x <= (int)ceilf(ex + rx); x++) {
            float dx = (x + 0.5f - ex) * irx, dy = (y + 0.5f - ey) * iry, d = dx * dx + dy * dy;
            if (d > 1.0f) continue;
            uint16_t c;
            if (shut) {
                float seam = 0.20f + 0.25f * dx * dx;
                c = fabsf(dy - seam) * ry < 0.50f && fabsf(dx) < 0.86f ? P->c[P_LIDLINE] : P->c[P_LID];
            } else {
                float top = -1 + 1.70f * blink, bot = 1 - 0.55f * blink;
                if (dy < top) c = P->c[P_LID];
                else if (dy > bot) c = P->c[P_LID];
                else if (blink > 0.08f && (dy - top) * ry < 0.9f) c = P->c[P_LIDLINE];   /* the upper lid's edge */
                else if (d > 0.60f) c = P->c[P_RIM];
                else if (fabsf(x + 0.5f - ex) < 0.62f && fabsf(dy) < 0.80f) c = P->c[P_PUPIL];
                else c = dy > 0.20f ? P->c[P_IRIS2] : P->c[P_IRIS];
                if (!(dy < top || dy > bot) && lz_floori(glx) == x && lz_floori(gly) == y && rx >= 1.4f) c = P->c[P_GLINT];
            }
            lz_px(o, x, y, c);
        }
    if (rx < 1.4f && !shut && blink < 0.5f) lz_px(o, lz_floori(ex), lz_floori(ey), P->c[P_PUPIL]);   /* (a hatchling's tiny eye) */
}

/* a filled triangle (the open mouth) */
static void lz_tri(lz_out_t *o, const float *xs, const float *ys, uint16_t c) {
    float miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 3; i++) { if (ys[i] < miny) miny = ys[i]; if (ys[i] > maxy) maxy = ys[i]; }
    for (int y = lz_floori(miny); y <= (int)ceilf(maxy); y++) {
        float yc = y + 0.5f, x0 = 1e9f, x1 = -1e9f;
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3; float ay = ys[i], by = ys[j];
            if ((ay <= yc && by > yc) || (by <= yc && ay > yc)) {
                float x = xs[i] + (xs[j] - xs[i]) * (yc - ay) / (by - ay);
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
            }
        }
        const int a = lz_floori(x0 + 0.5f), b = lz_floori(x1 - 0.5f);
        if (a <= b) lz_run(o, a, b, y, c);
    }
}

/* ---- the draw ---- */
static int lz_class(int kind) {
    switch (kind) {
    case LZ_POSE_FLICK: return C_FLICK;
    case LZ_POSE_STALK: return C_STALK;
    case LZ_POSE_LUNGE: return C_LUNGE;
    case LZ_POSE_BASK: return C_BASK;
    case LZ_POSE_DIG: return C_DIG;
    case LZ_POSE_REST: return C_REST;
    case LZ_POSE_PEEK: return C_PEEK;
    case LZ_POSE_STANDOFF: return C_STANDOFF;
    default: return C_STAND;                                                      /* STAND, WALK, TURN */
    }
}
static int lz_pat_kind(const pattern_t *p) {                                     /* 1 banded, 2 spotted, 0 plain */
    if (!p || !p->name) return 0;
    return strcmp(p->name, "BANDED") == 0 ? 1 : strcmp(p->name, "SPOTTED") == 0 ? 2 : 0;
}

void draw_lizard(const lz_canvas_t *cv, const lizard_look_t *lk, const lizard_pose_t *pose, float x, float floor_y, float scale) {
    g_box_ok = false;
    if (!cv || !lk || !lk->geom || !pose || pose->kind == LZ_POSE_HIDDEN || scale <= 0) return;
    lz_scratch_t *m = lz_mem();
    if (!m) return;
    const int kind = pose->kind, cls = lz_class(kind), patk = lz_pat_kind(lk->pattern);
    /* the key: the inputs quantized the way the build will use them */
    lz_key_t k; memset(&k, 0, sizeof k);
    float yh = lz_clamp(pose->yaw, -1, 1), yt = lz_clamp(pose->yaw_tail, -1, 1);
    const bool mirror = yh < 0;
    if (mirror) { yh = -yh; yt = -yt; }
    k.geom = lk->geom; k.morph = lk->morph; k.seed = lk->seed; k.stage = lk->stage; k.pat = (uint8_t)patk; k.cls = (uint8_t)cls;
    k.scale_q = (uint16_t)lz_clamp(scale * 64 + 0.5f, 1, 65535);
    k.yaw_h = (uint8_t)(yh * Q_YAW + 0.5f); k.yaw_t = (uint8_t)((yt + 1) * Q_YAW + 0.5f);
    k.fat = (uint8_t)(lz_clamp(pose->fat, 0, 1) * Q_FAT + 0.5f);
    k.shed = pose->shed;
    if (pose->shed == SHED_SHEDDING) k.shed_k = (uint8_t)(lz_clamp(pose->shed_k, 0, 1) * Q_SHED + 0.5f);
    k.peek = cls == C_PEEK ? (uint8_t)(lz_clamp(pose->peek, 0, 1) * Q_PEEK + 0.5f) : (uint8_t)Q_PEEK;
    if (cls == C_REST || cls == C_BASK) {                                        /* the breath: ~5 s a breath, three steps */
        float b = sinf(6.2831853f * 0.2f * pose->clock); k.breath = (uint8_t)(b > 0.45f ? 2 : b < -0.45f ? 0 : 1);
    } else k.breath = 1;
    if (cls == C_STALK) k.wig = (uint8_t)(lz_floori(lz_frac(pose->clock * 2.2f) * Q_WIG) % Q_WIG);   /* the tail tip, ~2 Hz */
    if (cls == C_STANDOFF) k.wig = (uint8_t)(lz_floori(lz_frac(pose->clock * 0.7f) * Q_WIG) % Q_WIG);
    const float reach = cls == C_LUNGE ? lz_clamp(pose->phase, 0, 1) : 0;
    if (cls == C_LUNGE) k.reach = (uint8_t)(reach * Q_REACH + 0.5f);
    const float qpeek = (float)k.peek / Q_PEEK, qreach = (float)k.reach / Q_REACH;
    /* the body's grid, and what the live parts hang from: cached */
    uint32_t key = 2166136261u;
    { const uint8_t *bp = (const uint8_t *)&k; for (size_t i = 0; i < sizeof k; i++) { key ^= bp[i]; key *= 16777619u; } }
    if (!key) key = 1;
    int slot = -1, old = 0;
    for (int i = 0; i < LZ_SLOTS; i++) {
        if (m->slot[i].key == key && !g_lz_nocache) { slot = i; break; }
        if (m->slot[i].used < m->slot[old].used) old = i;
    }
    int me = 0;                                                                  /* this lizard's last body (by its seed and its pack) */
    for (int i = 0; i < LZ_LAST; i++) {
        if (m->last[i].seed == lk->seed && m->last[i].geom == (const void *)lk->geom && m->last[i].used) { me = i; break; }
        if (m->last[i].used < m->last[me].used) me = i;
    }
    if (slot < 0 && g_lz_budget == 0 && m->last[me].seed == lk->seed && m->last[me].geom == (const void *)lk->geom && m->last[me].used
        && m->slot[m->last[me].slot].key == m->last[me].key)
        slot = m->last[me].slot;                                                 /* the frame's builds are spent: its last body a frame more */
    lz_art_t own; const lz_art_t *art = lz_art_of(lk->morph, &own);
    const bool banded = patk == 1 && art->hatch_band != 0;
    if (slot < 0) {
        const float qscale = k.scale_q / 64.0f, qyh = (float)k.yaw_h / Q_YAW, qyt = (float)k.yaw_t / Q_YAW - 1, qfat = (float)k.fat / Q_FAT;
        lz_shape_t sh; lz_shape_of(cls, &k, &sh);
        lz_spine_t sp; lz_spine(&sp, lk, &sh, qscale, qyh, qyt, qfat);
        lz_bctx_t b; memset(&b, 0, sizeof b);
        b.L0 = lk->geom->length_px; b.scale = qscale; b.iscale = 1.0f / qscale; b.seed = lk->seed;
        b.banded = banded; b.breakup = banded && lk->stage == STAGE_JUV ? 1.0f : 0.0f;
        b.spots = patk == 2 ? art->spots : 0;
        b.shed = pose->shed; b.shed_k = (float)k.shed_k / Q_SHED; b.peek = qpeek;
        slot = g_lz_nocache ? 0 : old;
        lz_build(&m->slot[slot], &sp, &b);
        lz_summarize(&m->slot[slot].sum, &sp, qscale);
        m->slot[slot].key = key;
        if (g_lz_budget > 0) g_lz_budget--;
    }
    lz_slot_t *S = &m->slot[slot];
    S->used = ++m->clock;
    m->last[me].seed = lk->seed; m->last[me].geom = lk->geom; m->last[me].slot = slot; m->last[me].key = S->key; m->last[me].used = m->clock;
    const lz_sum_t *sm = &S->sum;
    const lz_pal_t *P = lz_palette_of(m, lk, art, banded, pose->shed, cv->dim > 0 ? cv->dim : 1);
    /* the walk's bob: the body a pixel up as the diagonal pairs pass */
    const int bob = kind == LZ_POSE_WALK && sinf(12.566371f * pose->phase) > 0.35f ? 1 : 0;
    lz_xf_t xf = { lz_floori(x + 0.5f), lz_floori(floor_y + 0.5f) - bob, mirror };
    lz_out_t o; memset(&o, 0, sizeof o); o.cv = cv;
    lz_leg_t lg[4]; lz_legs(lg, sm, lk, kind, cls, lz_frac(pose->phase), qreach, qpeek);
    if (bob) for (int j = 0; j < 4; j++) { lg[j].foot[1] -= 1; for (int t = 0; t < 4; t++) lg[j].toe[t][1] -= 1; }   /* (the feet stay down) */
    /* 1 the contact shadow: the trunk's footprint on the sand, soft */
    if (qpeek >= 1) {
        const float cxl = 0.5f * (sm->shx0 + sm->shx1), cyl = 0.5f * (sm->shz0 + sm->shz1) + 1.0f + bob;
        const float rx = 0.5f * (sm->shx1 - sm->shx0) + 2, ry = 0.5f * (sm->shz1 - sm->shz0) + 1.6f + 0.9f * sm->sl;
        const float cx = mirror ? xf.ax - cxl : xf.ax + cxl, cy = xf.ay + cyl;
        const float strong = (cls == C_BASK || cls == C_REST) ? 120 : 92;
        for (int yy = lz_floori(cy - ry); yy <= (int)ceilf(cy + ry); yy++) {
            const float dy = (yy + 0.5f - cy) / ry, d2 = dy * dy;
            if (d2 >= 1) continue;
            const float hw = rx * sqrtf(1 - d2);
            lz_shadow_run(&o, (int)ceilf(cx - hw - 0.5f), lz_floori(cx + hw - 0.5f), yy, cx, 1 / rx, d2, strong);
        }
    }
    const bool stuck = pose->shed == SHED_STUCK;
    if (cls == C_DIG && qpeek >= 1) {                                            /* the dig's pit under the front feet */
        float fx = 0;
        for (int j = 0; j < 2; j++) { float a, b; lz_scr(&xf, lg[j].foot, &a, &b); fx += a * 0.5f; }
        const float fy = xf.ay + 0.5f, rx = 3.5f * sm->sl + 1, ry = 1.0f + 0.6f * sm->sl;
        for (int yy = lz_floori(fy - ry); yy <= (int)ceilf(fy + ry); yy++) {
            const float dy = (yy + 0.5f - fy) / ry, d2 = dy * dy;
            if (d2 >= 1) continue;
            const float hw = rx * sqrtf(1 - d2);
            lz_darken_run(&o, (int)ceilf(fx - hw - 0.5f), lz_floori(fx + hw - 0.5f), yy, (int)(110 * (1 - d2)));
        }
    }
    /* 2 the legs away from the glass */
    for (int j = 0; j < 4; j++) {
        const float c = cosf(sm->phi[lg[j].hind ? SUM_HIP : SUM_SHOULDER]);
        const bool front = lg[j].near ? c > -0.2f : c < -0.2f;                         /* (its side faces the glass) */
        lg[j].near = front;
        if (!front && lg[j].show) lz_draw_leg(&o, &xf, &lg[j], P, stuck);
    }
    /* 3 the body */
    for (int gy = 0; gy < S->gh; gy++) {
        const int y = xf.ay + S->gy0 + gy;
        if ((unsigned)(y - cv->oy) >= (unsigned)cv->h) continue;
        const uint8_t *row = S->tn + gy * S->gw;
        uint16_t *dst = cv->fb + (size_t)(y - cv->oy) * cv->stride;
        int ra = 1, rb = 0;                                                      /* the run being written, screen x (ra > rb: none) */
        for (int gx = S->x0[gy]; gx <= S->x1[gy]; gx++) {
            const uint8_t tn = row[gx];
            const int xx = mirror ? xf.ax - 1 - (S->gx0 + gx) : xf.ax + S->gx0 + gx;
            if (tn && (unsigned)(xx - cv->ox) < (unsigned)cv->w) {
                dst[xx - cv->ox] = P->c[tn];
                if (ra > rb) ra = rb = xx; else if (xx < ra) ra = xx; else rb = xx;
                continue;
            }
            if (ra <= rb) { if (cv->mark) cv->mark(cv->user, ra, rb, y); lz_box(&o, ra, rb, y); ra = 1; rb = 0; }
        }
        if (ra <= rb) { if (cv->mark) cv->mark(cv->user, ra, rb, y); lz_box(&o, ra, rb, y); }
    }
    /* 4 the near legs */
    for (int j = 0; j < 4; j++) if (lg[j].near && lg[j].show) lz_draw_leg(&o, &xf, &lg[j], P, stuck);
    /* 5 the eyes (the near one; the far one too where the head turns its face to the glass) */
    {
        const float *E = sm->P[SUM_EYE], r = sm->r[SUM_EYE], w = sm->w[SUM_EYE], phi = sm->phi[SUM_EYE];
        const float fx = cosf(phi), fz = sinf(phi);
        const float re = lk->geom->eye_r * powf(sm->scale, 0.75f) * (1 + 0.30f * sm->juv);
        float blink = lz_clamp(pose->blink, 0, 1);
        if (cls == C_REST) blink = 1;
        else if (cls == C_BASK && blink < 0.32f) blink = 0.32f;
        for (int side = 1; side >= -1; side -= 2) {
            const float lx = -fz * side, lz = fx * side;
            const float vis = 0.30f * fz + 0.92f * lz;                             /* its face to the glass */
            if (vis < 0.08f) continue;
            float C[3] = { E[0] + lx * w * 0.66f, E[1] + 0.20f * r, E[2] + lz * w * 0.66f }, ex, ey;
            lz_scr(&xf, C, &ex, &ey);
            const float front = (mirror ? -1.0f : 1.0f) * (fx >= 0 ? 1.0f : -1.0f);
            lz_draw_eye(&o, ex, ey, re * (0.45f + 0.55f * vis), re, blink, front, P);
        }
    }
    /* 6 the mouth: open at the end of a lunge; the tongue */
    {
        const float *N0 = sm->P[SUM_SNOUT], *N2 = sm->P[SUM_NOSE2], r0 = sm->r[SUM_SNOUT];
        float hx = N0[0] - N2[0], hy = -(N0[1] - N2[1]) + (N0[2] - N2[2]) * LZ_TILT, hl = sqrtf(hx * hx + hy * hy);
        if (hl < 1e-3f) hl = 1e-3f;
        hx /= hl; hy /= hl;
        if (mirror) hx = -hx;                                                    /* the head's way on the screen */
        const float turn = hx >= 0 ? 1.0f : -1.0f;                               /* (a rotation by turn x a: toward the chin) */
        float sx, sy; lz_scr(&xf, N0, &sx, &sy);
        const float open = cls == C_LUNGE ? lz_clamp((reach - 0.55f) / 0.35f, 0, 1) : 0;
        if (open > 0) {                                                          /* the jaw dropped from its hinge below the eye */
            float hxs, hys; lz_scr(&xf, sm->P[SUM_HINGE], &hxs, &hys); hys += 0.25f * r0 + 0.5f;
            const float ux = sx + hx * 0.8f, uy = sy + 0.20f * r0, a = turn * (0.20f + 0.45f * open), ca = cosf(a), sa = sinf(a);
            const float vx = ux - hxs, vy = uy - hys;
            float xs[3] = { hxs, ux, hxs + vx * ca - vy * sa }, ys[3] = { hys, uy, hys + vx * sa + vy * ca };
            lz_tri(&o, xs, ys, P->c[P_MOUTH_IN]);
        }
        const float tg = lz_clamp(pose->tongue, 0, 1);
        if (tg > 0.05f) {                                                        /* forward and a little down, a rounded tip */
            const float len = (0.8f + 4.6f * tg) * powf(sm->scale, 0.8f), a = turn * 0.32f, ca = cosf(a), sa = sinf(a);
            const float dx = hx * ca - hy * sa, dy = hx * sa + hy * ca;
            const float bx = sx + hx * 0.6f, by = sy + 0.25f * r0, ex = bx + dx * len, ey = by + dy * len;
            float tr = 0.62f * powf(sm->scale, 0.4f); if (tr < 0.55f) tr = 0.55f;
            lz_capsule1(&o, bx, by, ex, ey, tr, P->c[P_TONGUE]);
            lz_px(&o, lz_floori(ex), lz_floori(ey), P->c[P_TONGUE_TIP]);
        }
    }
    /* 7 the dig's sand, thrown back from the front feet */
    if (cls == C_DIG) {
        float fx = 0, fy = 0;
        for (int j = 0; j < 2; j++) { float a, b; lz_scr(&xf, lg[j].foot, &a, &b); fx += a * 0.5f; fy += b * 0.5f; }
        const float back = mirror ? 1.0f : -1.0f;
        for (int g = 0; g < 5; g++) {
            float q = lz_frac(pose->phase * 2.0f + g * 0.2f);
            float gx = fx + back * (2 + 15 * q) * sm->sl, gy = fy - (8 * sinf(3.14159f * q) - 1.5f * q) * sm->sl;
            uint16_t c = (g & 1) ? P->c[P_SAND2] : P->c[P_SAND];
            lz_run(&o, lz_floori(gx), lz_floori(gx) + (sm->sl > 0.8f && g < 3 ? 1 : 0), lz_floori(gy), c);
        }
    }
    if (o.any) { g_box[0] = o.bx0; g_box[1] = o.by0; g_box[2] = o.bx1; g_box[3] = o.by1; g_box_ok = true; }
}
