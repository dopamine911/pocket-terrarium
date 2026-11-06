/* scene.c - THE AQUARIUM's scene (issue #4 phase 3: moved verbatim out of render.c, whose bulk it was).
 *
 * render_tank's frame in the aquarium - the water and its pebbles, the grass and its cache and loop, the pieces (castle, coral,
 * reef cluster, pumpkin lantern, volcano), the critters (snail, shrimp, urchin, seahorse, crab), swim depth - and the public
 * render.h functions that tune and test it. render.c reaches it through world_view() (view.c); it draws through the renderer's
 * pixel layer (render_core.h) and the fish body (bodies/fish). */
#include "render_core.h"
#include "world_view.h"
#include "worlds/aquarium/scene.h"
#include "bodies/fish/fish.h"
#include "pt_util.h"
#include "advanced.h"
#include "icons.h"
#include "progression.h"
#include "collection.h"
#include "sounds.h"
#include "notice.h"
#include "gift.h"
#include "tank_events.h"
#include "setup.h"
#include "version.h"
#include <stdlib.h>
#include <stdio.h>

/* THE GRASS CACHE (2026-10-24, Strato: "keep the back grass in a cached scene
 * that updates a few times a second"). The grass was a third of a full frame
 * (27 ms on the square), and its sway is slow - a frond's tip moves under 4 px
 * a second. So the BACK half of it (the beds behind the fish and the even
 * fronds of the woven beds) is painted into a copy of the scene, g_sceneg,
 * at most every g_sg_period seconds; frames are primed from that copy and
 * draw only the front fronds live. The pieces behind the grass that animate
 * every frame (a crown's tentacles, the candle, the lava and the volcano's
 * bubbles) must stay UNDER it: the cache records which pixels the back grass
 * covers (g_bgmask) and those overlays skip them (g_under). A scene rebake,
 * a placement page or a page in between: the frame draws its grass as before
 * and the cache is rebuilt at its end. Off unless the platform hands over the
 * buffers and turns it on (director `grasscache`, the sim's PT_GRASS_CACHE). */
static uint16_t *g_sceneg;                     /* the scene + the back grass */
static uint32_t *g_bgmask;                     /* the back grass's pixels in it (RENDER_DIRTY_WORDS) */
static bool      g_sg_on;
static float     g_sg_period = 0.16f;
static bool      g_sg_ok;
static unsigned  g_sg_scene_ep, g_sg_ep = 1;
static double    g_sg_clock;
static int       g_sg_top = 0;                 /* the highest row the cached grass touched */
static bool      g_bg_rec;                     /* while the cache's grass is painted: record into g_bgmask */
static int64_t   g_sg_us; static int g_sg_n;   /* the refreshes' cost (the director's line) */
void render_set_grass_cache(uint16_t *buf, uint32_t *mask) { g_sceneg = buf; g_bgmask = mask; g_sg_ok = false; }
void render_set_grass_cache_on(bool on, int period_ms) { bool was = g_sg_on; g_sg_on = on && g_sceneg && g_bgmask; if (period_ms > 0) g_sg_period = period_ms / 1000.0f; if (!was || !g_sg_on) g_sg_ok = false; }   /* (a new period alone keeps the cache) */
bool render_grass_cache_on(void) { return g_sg_on; }
int  render_grass_cache_ms(void) { return (int)(g_sg_period * 1000 + 0.5f); }
void render_grass_cache_prof(int64_t *us, int *n) { *us = g_sg_us; *n = g_sg_n; g_sg_us = 0; g_sg_n = 0; }
const uint16_t *aquarium_scene_alt(unsigned *epoch) {   /* render_scene_buf: the scene + the back grass, when the grass cache is current */
    if (g_sg_on && g_sg_ok && g_sg_scene_ep == g_scene_epoch && g_scene_epoch) { if (epoch) *epoch = 0x80000000u | g_sg_ep; return g_sceneg; }
    return NULL;
}
/* THE SWAY REPEATS (0.3.9.3, the grass loop below): every motion of a frond is a whole number of turns in
 * VEG_PERIOD seconds - the sway at the grass's old 0.9 rad/s, and the bend that runs up a frond (0.6 rad/s
 * until now, which with the 0.9 repeated only every 21 s) and the sword plant's lazier 0.6 at that speed
 * too - so a few stored frames of it loop seamlessly. veg_clock is the sway's own clock, the frame's taken
 * round the period (or a phase forced by the loop's bake). */
#define VEG_PERIOD   7.0f
#define VEG_W        (6.2831853f / VEG_PERIOD)
static float g_veg_clock_force = -1;
static inline float veg_clock(double clock) { return g_veg_clock_force >= 0 ? g_veg_clock_force : (float)fmod(clock, VEG_PERIOD); }
/* the clock's drawing face (tank.h tank_anim), worked out once a frame - the S3 does a double's fmod in software */
static double g_anim_of = -1; static float g_anim;
static inline float anim(const tank_t *t) { if (t->clock != g_anim_of) { g_anim = tank_anim(t->clock); g_anim_of = t->clock; } return g_anim; }
/* THE GRASS LOOP (0.3.9.3, Strato: "refactoring the seagrass from the ground up ... the biggest source of lag").
 * With the weave gone every grass bed BEHIND the fish is background, and its sway repeats every VEG_PERIOD
 * seconds (veg_clock): so the back grass is BAKED. The loop holds up to GL_MAX BANDS - the scene's rows the grass
 * can reach with the grass of one moment of the sway in them (fewer if the memory budget says so) - and between
 * them GL_FINE fine moments a band (up to GL_FMAX, gl_pick_f), each kept as its DIFFERENCE from the band nearest it (the pixels that moved:
 * a blade's base hardly does, so a quarter of the grass's cache lines on average). A frame is primed with the
 * scene above the band and its band below (the firmware's prefetch: two DMA copies, render_grass_loop_parts),
 * then its fine moment's difference is laid over it. No back grass is drawn in a frame at all. Each band keeps a
 * mask of its grass, and a difference says which of its pixels are grass (the overlays behind it - a crown, the
 * candle, the lava - stay under it, as with the cache above). The loop goes stale when what the grass looks like
 * changes (the scene, a frond's height, a bed moving, its depth): until it is rebaked - one band or difference a
 * frame, the bands first, once the grass has held still a few frames - a frame of it draws its grass the old way
 * (a frame whose difference is not baked yet shows its band's own moment). Beds IN FRONT stay live after the fish.
 * Measured on the square: 14.6 fps with the grass drawn, 18.2 with ten bands alone (too steppy: a tall blade's
 * tip moved ~1.3 px between them, three times a second), 16.0 with every moment as runs over the scene (the CPU
 * touching the whole band again), 18.6 with no grass at all; four fine moments a band on the clock, 17.4 (Strato: "still
 * a little steppy" - moments on the clock's grid against frames on their own); eight a band STEPPED one a frame
 * (render_grass_loop_lock), 56 moments in 0.8 MB of differences, 17.3-18.9. */
#define GL_MAX 16                                /* bands at most (an epoch keeps 5 bits of one) */
#define GL_FINE 8                                /* fine moments a band, as planned: its own and the next seven */
#define GL_FMAX 12                               /* ... at most: stepping, a board whose memory holds fewer bands takes more (gl_pick_f) */
#define GL_F_LEAST 6                             /* ... and its fewest fine moments a band, before a band fewer (0.4: a tall bed's arena) */
#define GL_MIN_BANDS 3                           /* the fewest a loop is planned with (0.4, 2026-11-01, Strato: the tall-grass fix in 0.4 - four was the floor,
                                                    and a grown bed on the square (a 410-row band) needed ~2.9 MB for four: no loop, the grass live,
                                                    10.6 fps with visitors in. Three fit in ~2.4 MB, 36 moments of the sway instead of 48) */
static void   *(*g_gl_alloc)(size_t);
static void    (*g_gl_free)(void *);
static size_t  g_gl_budget;
static bool    g_gl_on, g_gl_bypass;          /* (bypass: the sim's A/B - the loop kept, not used) */
static int     g_gl_n;                          /* bands in the loop (fine moments: g_gl_f times as many) */
static int     g_gl_fcap_n;                     /* (0.4) the bands g_gl_fcap was measured for (an overflow: its moments a band fit at that many bands) */
static size_t  g_gl_fit_avg;
static int     g_gl_fsel, g_gl_fsel_n;          /* (0.4, THE MOST MOMENTS) the moments a band the budget holds at the planned count of bands */                    /* ... and the moment's difference it measured, bytes (0: none yet) */
static int     g_gl_f = GL_FINE, g_gl_fcap = GL_FMAX;   /* fine moments a band, and the most the differences have held (an overflow lowers it) */
static bool    g_gl_replan;                     /* a difference did not fit: the loop planned again with fewer */
static int     g_gl_y0, g_gl_rows, g_gl_cap;    /* the band's first row, its rows, the rows allocated */
static uint16_t *g_gl_band[GL_MAX];
static uint32_t *g_gl_mask[GL_MAX];
static uint32_t *g_gl_omask[GL_MAX];               /* SWIM DEPTH (0.3.9.4): each band's ODD fronds' pixels (a fish THROUGH the grass); none: THROUGH hides BEHIND */
static bool    g_gl_ok[GL_MAX];
static uint16_t g_gl_ver[GL_MAX];               /* a band's bake count: a frame primed with an older one copies again */
static uint16_t *g_gl_diff; static size_t g_gl_dwords, g_gl_dused;   /* the fine moments' differences, one after another as they are baked */
static uint32_t g_gl_doff[GL_MAX * GL_FMAX];    /* (where each starts) */
static int     g_gl_nomem;                      /* rows the platform could not spare a loop for (not tried again for them ... */
static double  g_gl_nomem_t = -1;               /* ... until GL_RETRY_S after: the tank clock it gave up at */
#define GL_RETRY_S 30.0f                        /* (2026-11-01: a plan at a boot, PSRAM still held by the start, gave up on the square
                                                   for the day - a minute later 3.7 MB was free and the loop never came; Strato: retry) */
static int     g_gl_top_bed = -1, g_gl_top_segs, g_gl_top_n;   /* (what the band's top came from, for the log: the tallest frond's bed and segments, the beds counted) */
static int     g_gl_why;                        /* ... and where the plan gave up (1 the tables, 2 the bake's buffers, 3 fewer than four bands or no arena; render_grass_loop_nomem) */
static uint8_t g_gl_fok[GL_MAX * GL_FMAX];      /* a fine moment: 0 stale, 1 baked, 2 its difference too big (its band's moment instead) */
static uint16_t *g_gl_bake; static uint32_t *g_gl_bmask;   /* a fine moment's bake, and a frame's mask of its grass ... */
static uint32_t *g_gl_pmask;                    /* ... the bake's own mask of it (0.3.9.4: a bake in slices spans frames that use g_gl_bmask) */
static uint32_t g_gl_key, g_gl_key_last;        /* what the grass looks like (gl_key): the loop's, and the last frame's */
static int     g_gl_still;                      /* frames the look has held: a bake waits for GL_SETTLE (night falling, a trim: the first frames still move) */
#define GL_SETTLE 3
static int     g_gl_bakes; static int64_t g_gl_bake_us;   /* (the display line) */
static bool    g_gl_warm;                       /* the loop has been whole since it was turned on (LOADING: render_grass_loop_warming) */
static int     g_gl_lock, g_gl_cur = -1;        /* moments the loop steps through a frame at a time (0: by the clock), the last frame's */
static float   g_gl_dt_avg;                     /* (THE PACE) the frames' average length, for a loop short of moments */
static double  g_gl_step_t = -1; static float g_gl_step_acc;   /* THE PACE (gl_next): the clock of the last stepped frame; the fraction of a moment carried */
void render_set_grass_loop(void *(*alloc)(size_t), void (*release)(void *), size_t budget) { g_gl_alloc = alloc; g_gl_free = release; g_gl_budget = budget; }
static void gl_patch_reset(void);                /* THE PATCH and the ring (0.3.9.4, with the loop's pieces below) */
static void gl_ring_reset(void);
static void gl_meta_free(void);
static int  gl_exact(void);
static void gl_release(void) {
    for (int k = 0; k < GL_MAX; k++) { if (g_gl_band[k] && g_gl_free) g_gl_free(g_gl_band[k]); if (g_gl_mask[k] && g_gl_free) g_gl_free(g_gl_mask[k]); if (g_gl_omask[k] && g_gl_free) g_gl_free(g_gl_omask[k]); g_gl_band[k] = NULL; g_gl_mask[k] = NULL; g_gl_omask[k] = NULL; g_gl_ok[k] = false; }
    if (g_gl_free) { if (g_gl_diff) g_gl_free(g_gl_diff); if (g_gl_bake) g_gl_free(g_gl_bake); if (g_gl_bmask) g_gl_free(g_gl_bmask); if (g_gl_pmask) g_gl_free(g_gl_pmask); }
    g_gl_diff = NULL; g_gl_bake = NULL; g_gl_bmask = NULL; g_gl_pmask = NULL; g_gl_dused = 0;
    memset(g_gl_fok, 0, sizeof g_gl_fok);
    gl_patch_reset(); gl_ring_reset(); gl_meta_free();
    g_gl_n = 0; g_gl_cap = 0;
}
void render_grass_loop_on(bool on) { g_gl_on = on && g_gl_alloc && g_gl_free; g_gl_nomem = 0; g_gl_warm = false; if (!g_gl_on) gl_release(); }
void render_grass_loop_lock(int moments) { if (moments != g_gl_lock) { g_gl_lock = moments; g_gl_nomem = 0; g_gl_cur = -1; g_gl_step_t = -1; g_gl_step_acc = 0; g_gl_fcap = GL_FMAX; g_gl_fcap_n = 0; g_gl_fit_avg = 0; g_gl_fsel_n = 0; gl_release(); } }   /* (planned again for them) */
int  render_grass_loop_moment(void) { return g_gl_cur; }
int  render_grass_loop_nomem(int *why) { *why = g_gl_why; return g_gl_nomem; }   /* rows the plan gave up on (0: it did not), and where */
void render_grass_loop_top(int *bed, int *segs, int *beds) { *bed = g_gl_top_bed; *segs = g_gl_top_segs; *beds = g_gl_top_n; }   /* (the log) the tallest back frond the band is sized for */
void render_grass_loop_arena(int *used_kb, int *cap_kb) { *used_kb = (int)(g_gl_dused * 2 / 1024); *cap_kb = (int)(g_gl_dwords * 2 / 1024); }
bool render_grass_loop_is_on(void) { return g_gl_on; }
void render_grass_loop_bypass(bool on) { g_gl_bypass = on; }
void render_force_veg_clock(float c) { g_veg_clock_force = c; }
float render_sway_moment(const tank_t *t) { return veg_clock(t->clock); }
int  render_grass_loop_n(void) { return g_gl_n * g_gl_f; }
int  render_grass_loop_ready(void) { return gl_exact(); }   /* moments baked at the look as it is (THE PATCH: a moment stale inside the boxes is served, not counted) */
void render_grass_loop_prof(int *n, int *bakes, int64_t *us, int *rows) { *n = g_gl_n * g_gl_f; *bakes = g_gl_bakes; *us = g_gl_bake_us; *rows = g_gl_rows; g_gl_bakes = 0; g_gl_bake_us = 0; }
/* the loop spans HALF the sway's period: a frond's shape is its sway times the bend running up it, and half a
   period on flips the sign of both - the same shape (the bands of moments k and k + n/2 came out identical) */
#define GL_SPAN (VEG_PERIOD * 0.5f)
static inline int gl_fine(double clock) { int nf = g_gl_n * g_gl_f; return nf ? (int)((float)fmod(clock, GL_SPAN) * nf / GL_SPAN) % nf : 0; }
static inline float gl_ftime(int j) { return j * GL_SPAN / (g_gl_n * g_gl_f); }
static inline int gl_base(int j) { return (j + g_gl_f / 2) / g_gl_f % g_gl_n; }   /* the band nearest a fine moment */
/* a band's own fine moments, as gl_base groups them: from GL_DLO before its moment to GL_DHI after - f/2 either side for
   an odd f, one fewer after for an even one (0.3.9.3: the clamp took the even case for both, and with 11 a band - the
   watch, the square - a frame primed with its band was held a moment back every frame: the grass froze) */
#define GL_DLO (g_gl_f / 2)
#define GL_DHI (g_gl_f - 1 - g_gl_f / 2)
/* the next frame's fine moment: by the clock - or, stepping (render_grass_loop_lock), the one after the last frame's,
   so the grass moves exactly once a frame, with the fish (Strato: moments on the clock's grid "vary from the frame
   rate of the rest of the tank" - a frame that repeats one or skips one is a step the eye catches) */
/* THE PACE (0.3.9.4, Strato on the square at 13-14 fps: the fronds "glitchy and not smooth"; another keeper's light tank swaying
   twice as fast as the full one): stepping ran ONE moment a frame, so the sway's period was the frame rate's (GRASS_LOOP_MOMENTS
   planned for ~17.5 fps on the square) and every uneven frame was a jerk of the grass. Now a stepped frame advances by the clock -
   the moments the sway has walked since the last frame, carried fractionally - one at the least (never a repeat), GL_STEP_MAX at
   most (never a jump): the period is the sway's at any frame rate, a long frame is a slightly longer step, not a stall */
#define GL_STEP_MAX 3
static inline int gl_steps(double clock, float *acc) {   /* the moments a stepped frame at `clock` advances, and the fraction it would carry (nothing changed) */
    const int nf = g_gl_n * g_gl_f;
    const float dt = g_gl_step_t >= 0 && clock > g_gl_step_t ? (float)(clock - g_gl_step_t) : 0;
    float want = g_gl_step_acc + dt * nf / GL_SPAN;
    int steps = (int)want;
    const float rate = g_gl_dt_avg * nf / GL_SPAN;   /* moments a frame at the AVERAGE frame (one short frame never makes a repeat): under a half,
                                                         a step every frame sways it 2x+ too fast */
    const int least = g_gl_dt_avg <= 0 || rate >= 0.5f ? 1 : 0;    /* (0.4, 11-01: a loop SHORT of its moments - tall grass, three bands, 21 of 48 on the square - holds a
                                                      moment a few frames: one a frame swayed it 4x too fast, Strato: "smooth but wiggling too fast") */
    if (steps < least) { steps = least; want = least; }
    if (steps > GL_STEP_MAX) { steps = GL_STEP_MAX; want = GL_STEP_MAX; }
    if (acc) *acc = want - steps;
    return steps;
}
static inline int gl_next(double clock) {               /* PURE: the prefetch asks for the next frame's moment before the frame does (a first cut carried the fraction here
                                                          and both asks advanced it: the sway lurched 1-3 moments at random - Strato: the fronds "glitchy", a fish through them flickering) */
    const int nf = g_gl_n * g_gl_f;
    if (!(g_gl_lock && g_gl_cur >= 0 && g_gl_cur < nf)) return gl_fine(clock);
    return (g_gl_cur + gl_steps(clock, NULL)) % nf;
}
static inline void gl_stepped(int j, double clock) {    /* the frame took moment j: the carry and the clock move on here alone */
    if (g_gl_cur >= 0) { float acc; gl_steps(clock, &acc); g_gl_step_acc = acc; }
    if (g_gl_step_t >= 0 && clock > g_gl_step_t && clock - g_gl_step_t < 0.5f)
        g_gl_dt_avg = g_gl_dt_avg > 0 ? g_gl_dt_avg * 0.95f + (float)(clock - g_gl_step_t) * 0.05f : (float)(clock - g_gl_step_t);
    g_gl_cur = j; g_gl_step_t = clock;
}
static uint32_t *g_om;                           /* SWIM DEPTH: while set, the odd fronds' pixels recorded here too (g_veg_odd) */
static bool      g_veg_odd;                      /* (draw_veg: the frond being painted is an odd one) */
/* SWIM DEPTH (0.3.9.4, docs/swim-depth.md): a fish BEHIND or THROUGH the back grass. Before it is drawn the pixels
   under its box are kept (and their dirty bits); after, the grass's own pixels inside the box come back over it -
   every frond's (BEHIND) or the odd fronds' (THROUGH). The grass's pixels: the loop's band mask (its fine moment's,
   exact) by day, or the frame's own when the grass was drawn live (the night, a stale loop) - recorded as it is
   drawn; the odd fronds': each band's g_gl_omask, or the live frame's. Memory from the platform, one block */
static uint16_t *g_dep_save; static uint32_t *g_dep_dirty, *g_fl_all, *g_fl_odd;
static uint32_t *g_fbg_front, *g_fbg_thru; static bool g_fbg_on, g_fbg_used;   /* a bed IN FRONT (the sword plant): the pixels of fish IN FRONT of it, THROUGH it;
                                                       veg_put keeps its leaves off them (every leaf / the even ones) while g_fbg_on */
static uint32_t *g_keep; static bool g_keep_on;     /* phase 2: the pixels of fish IN FRONT of the pieces this frame - the restore of the pieces
                                                       IN FRONT, their overlays (crowns, lava, the lantern's face) and the arch's shade keep off them */
static inline bool keep_at(int x, int y) { return g_keep_on && ((g_keep[y * DIRTY_WORDS_PER_ROW + (x >> 5)] >> (x & 31)) & 1); }
static uint32_t *g_bedm; static bool g_bedm_on; static uint32_t g_bed_over_k;   /* the sword plant IN FRONT's leaves this frame (recorded as it is drawn), and
                                                       the pieces ranked below it: their per-frame overlays (the pumpkin's face and flame, the volcano's
                                                       lava, a coral's crown) keep off its leaves (0.3.9.4, Strato on the square: the plant raised over the
                                                       pumpkin, the face clipping through it) */
static inline bool bed_over(int k, int x, int y) {
    return g_bedm_on && k >= 0 && k < 32 && (g_bed_over_k >> k & 1) && ((g_bedm[y * DIRTY_WORDS_PER_ROW + (x >> 5)] >> (x & 31)) & 1);
}
#define DEP_CAP_PX (200 * 200)
#define DEP_CAP_ROWS 200
static short g_fbb[N_FISH_SLOTS][4]; static bool g_fbb_ok[N_FISH_SLOTS];   /* each swimmer's drawn box, the last frame - visitors too (their slots
                                                   run past N_FISH_MAX: sized for home fish, two visitors wrote past it and the square crashed, 2026-11-01) */
_Static_assert(sizeof g_fbb / sizeof g_fbb[0] >= N_FISH_SLOTS && sizeof g_fbb_ok >= N_FISH_SLOTS, "a box for every swimmer, visitors too");
static bool g_dep_off;                           /* (the sim's grass tests: every fish IN FRONT - THROUGH at a fine moment takes its
                                                    band's odd fronds, a pixel off at a frond's edge, which a pixel-for-pixel test of
                                                    the grass would read as the grass's own difference) */
void render_depth_off(bool off) { g_dep_off = off; }
static bool g_dep_keep_old;                         /* (--selftest-front-keep's control: the restore behind a piece ignores the fish IN FRONT, as before 0.3.9.6) */
void render_debug_front_keep_old(bool on) { g_dep_keep_old = on; }
static int64_t g_dep_us; static int g_dep_n;      /* the keep and the put-back's own time, and how many fish did it (render_depth_prof) */
void render_depth_prof(int64_t *us, int *n) { *us = g_dep_us; *n = g_dep_n; g_dep_us = 0; g_dep_n = 0; }
size_t render_depth_buf_bytes(void) {
    return DEP_CAP_PX * 2 + (size_t)DEP_CAP_ROWS * DIRTY_WORDS_PER_ROW * 4 + 6 * (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4;
}
void render_set_depth_buf(void *buf) {
    if (!buf) { g_dep_save = NULL; g_dep_dirty = g_fl_all = g_fl_odd = g_keep = g_fbg_front = g_fbg_thru = g_bedm = NULL; return; }
    uint8_t *p = buf;
    g_dep_save = (uint16_t *)p; p += DEP_CAP_PX * 2;
    g_dep_dirty = (uint32_t *)p; p += (size_t)DEP_CAP_ROWS * DIRTY_WORDS_PER_ROW * 4;
    g_fl_all = (uint32_t *)p; p += (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4;
    g_fl_odd = (uint32_t *)p; p += (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4;
    g_keep = (uint32_t *)p; p += (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4;
    g_fbg_front = (uint32_t *)p; p += (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4;
    g_fbg_thru = (uint32_t *)p; p += (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4;
    g_bedm = (uint32_t *)p;
}
static inline void mark_run(uint32_t *row, int x0, int x1) {
    int w0 = x0 >> 5, w1 = x1 >> 5;
    if (w0 == w1) row[w0] |= (0xFFFFFFFFu >> (31 - (x1 & 31))) & (0xFFFFFFFFu << (x0 & 31));
    else { row[w0] |= 0xFFFFFFFFu << (x0 & 31); for (int w = w0 + 1; w < w1; w++) row[w] = 0xFFFFFFFFu; row[w1] |= 0xFFFFFFFFu >> (31 - (x1 & 31)); }
}
static inline void mask_clear_run(uint32_t *row, int x0, int x1) {
    int w0 = x0 >> 5, w1 = x1 >> 5;
    uint32_t lo = 0xFFFFFFFFu << (x0 & 31), hi = 0xFFFFFFFFu >> (31 - (x1 & 31));
    if (w0 == w1) { row[w0] &= ~(lo & hi); return; }
    row[w0] &= ~lo; for (int w = w0 + 1; w < w1; w++) row[w] = 0; row[w1] &= ~hi;
}
/* THE PATCH (0.3.9.4, with THE GRASS LOOP's pieces): a box of the band repainted - the grass's marks clipped to it (g_bg_clip), the
   fronds and rows clear of it skipped (g_veg_cull: a frond reaches GL_BED_PAD px past its root at most) */
typedef struct { short x0, y0, x1, y1; } gl_rect_t;                    /* inclusive, frame coordinates */
static bool g_bg_clip; static gl_rect_t g_bg_clip_r;
static bool g_veg_cull;
static const uint16_t *(*g_veg_rowclip)(int y);                          /* THE PATCH: a frond row's span clipped to the row's stale intervals (gl_wrow_of; NULL: none) */
static int  g_span_xc = -1;                                              /* the vignette's middle for every run of one frond row's span (veg_span): a run a piece splits
                                                                            the span into keeps the whole span's alpha, so what a piece does inside its box never changes a
                                                                            pixel outside it (THE PATCH's rule); -1: a span on its own */
#define GL_BED_PAD 12                                                    /* a frond's reach past its root: the sway (4; the sword 2.5) and the half-width (2.4; the sword's 7.2) */
static inline void bg_mark(int x0, int x1, int y) {
    if (!g_bg_rec || (unsigned)y >= (unsigned)TANK_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > TANK_W - 1) x1 = TANK_W - 1;
    if (g_bg_clip) {
        if (y < g_bg_clip_r.y0 || y > g_bg_clip_r.y1) return;
        if (x0 < g_bg_clip_r.x0) x0 = g_bg_clip_r.x0;
        if (x1 > g_bg_clip_r.x1) x1 = g_bg_clip_r.x1;
    }
    if (x0 > x1) return;
    mark_run(g_bgmask + y * DIRTY_WORDS_PER_ROW, x0, x1);
    if (g_om && g_veg_odd) mark_run(g_om + y * DIRTY_WORDS_PER_ROW, x0, x1);
    if (y < g_sg_top) g_sg_top = y;
}

static bool g_page_animals;                      /* (the tests: the creatures drawn under a placement page all the same - the layering checks) */
void render_debug_page_animals(bool show) { g_page_animals = show; }

/* ---- the shrimp (2026-09-29): cherry shrimp from 18 x 8 pixel grids,
 * facing right, drawn pixel by pixel through the fish's turn - the yaw
 * foreshortens x - so they stay procedural (Strato: "all aquatic marine life
 * sprites should be procedurally drawn. raster icons only for menus").
 * What makes six pixels a shrimp: the hunched back, the body thinning to a
 * fanned tail, one eye up front, antennae longer than the body; the tail,
 * legs and antennae are see-through. Character is in the motion: paddling
 * legs, a peck at the floor, the tail-flick escape. (working-assets/shrimp/
 * has the approved drafts.) Tones: o belly edge, r cherry, h the back's
 * light, s a glint, e the eye, a antenna, l legs, t tail fan. */
#define SHRIMP_TONES 8
static const char SHRIMP_KEYS[SHRIMP_TONES + 1] = "orhsealt";
static const uint32_t SHRIMP_RGB[SHRIMP_TONES]  = { 0x6e0f1c, 0xd0262e, 0xff5a4a, 0xffb8a0, 0x100408, 0xff7a70, 0xc0303a, 0xff7060 };
static const uint8_t  SHRIMP_ALPHA[SHRIMP_TONES] = { 255, 255, 255, 255, 255, 140, 190, 205 };
static const char *const SHRIMP_IDLE_A[8] = {
    "..................", "......ohhs......aa", "....ohrrrhhhs..a..", "..ohrrrrrrrrrea...",
    ".trrrrrrrrrrrrrr..", "tto..oorrrrrroo...", "t......l.l.l.l.aa.", "................a." };
static const char *const SHRIMP_IDLE_B[8] = {
    "................a.", "......ohhs......a.", "....ohrrrhhhs..a..", "..ohrrrrrrrrrea...",
    ".trrrrrrrrrrrrrr..", "tto..oorrrrrroo...", "t.....l.l.l.l..aa.", ".................." };
static const char *const SHRIMP_PECK[8] = {      /* nose to the sand */
    "..................", "......ohhs........", "....ohrrrhhhs.....", "..ohrrrrrrrrrr.aa.",
    ".trrrrrrrrrrrrea..", "tto..oorrrrrrrrr..", "t......l.l.l.lo.a.", "................a." };
static const char *const SHRIMP_DART[8] = {      /* the escape: tail snapped under, antennae streaming back */
    "..................", "aaaa..ohhs........", "....ohrrrhhhs.....", "...orrrrrrrrrea...",
    "...ttrrrrrrrrrr...", "...tt.oorrrroo....", "..................", ".................." };
static void draw_shrimp(ctx_t *c, const shrimp_t *q, const src_t *pal) {
    if (g_no_animals) return;
    float sp2 = q->vx * q->vx + q->vy * q->vy;
    const char *const *fr = q->dart > 0 ? SHRIMP_DART
                          : q->pecking ? (fmodf(q->ph * 3, 1) < 0.5f ? SHRIMP_PECK : SHRIMP_IDLE_A)
                          : (fmodf(q->ph * (sp2 > 9 ? 8 : 2.5f), 1) < 0.5f ? SHRIMP_IDLE_A : SHRIMP_IDLE_B);
    float s = q->yaw;                                   /* head-on it is a sliver, never nothing */
    if (s > -0.3f && s < 0.3f) s = s < 0 ? -0.3f : 0.3f;
    int oy = (int)floorf(q->y + 0.5f) - 4;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 18; x++) {
            char ch = fr[y][x];
            if (ch == '.') continue;
            const char *k = strchr(SHRIMP_KEYS, ch);
            if (!k) continue;
            int ti = (int)(k - SHRIMP_KEYS);
            px_blend_s(c, (int)floorf(q->x + (x - 9) * s + 0.5f), oy + y, &pal[ti], SHRIMP_ALPHA[ti]);
        }
}

/* a bed of swaying seaweed fronds; seed varies phase/heights between beds.
 * n and max_seg come from tank_veg_bed (growth-driven: the beds keep growing
 * up and out until the keeper trims them; at VEG_NUB they are green stubble).
 * layer: 0 = the fronds drawn BEHIND the fish, 1 = the fronds drawn in FRONT
 * (alternating), so a fish that dips into a canopy swims woven through it
 * instead of floating on top. */

/* the snail: drawn procedurally (2026-09-16), defined after the castle
 * block below - it shares the castle's position hash */
static void draw_snail(ctx_t *c, const tank_t *t, bool upright_pass);

/* ---- the pumpkin lantern's night light (0.3.6; Strato 2026-10-17: "its
 * glow should gently illuminate the other decorations and critters in the
 * tank") ----
 * With the tank light off the lantern's candle lights everything near it,
 * not only the water behind: the light at a pixel is one table lookup by its
 * squared distance (g_pk_lut: warm at the heart, going gold-green with
 * distance - the water drinks the red first), added before the vignette.
 * Where it lands:
 *   - the baked scene: the water and the floor once, dithered (pk_halo),
 *     and every piece painted into the bake - the final-paint paths (the
 *     castle, the corals, the mounds) call PK_LIGHT_PX before their vignette;
 *   - every frame, what is painted live: each frond span (one light per
 *     span, as its vignette) and - in one pass over the dirty mask before
 *     the vignette sweep (pk_light_dirty) - the fish, the shrimp, the snail,
 *     the urchin, the seahorse, the food, the bubbles, the crowns.
 * g_pk_lx < 0 = no light (the light on, no pumpkin, a page drawing):
 * render_tank sets it for its own frame and clears it on the way out. */
#define PK_HALO_R   176                       /* the reach along the floor, px (a castle's half width and more) */
#define PK_HALO_RY  150                       /* and up: it spreads along the floor a little more */
#define PK_LUT_K    (16777216 / (PK_HALO_R * PK_HALO_R))   /* d2 -> table index (256 = the edge) */
#define PK_DY_K     (256 * PK_HALO_R * PK_HALO_R / (PK_HALO_RY * PK_HALO_RY))   /* dy^2 -> its share of d2, x256 (no divide per pixel) */
static int g_pk_lx = -1, g_pk_ly;
/* the light by squared distance, 128 steps: its RGB in 8 bits (the bake's
 * dithered water) and packed as 565 increments in g_pk_lut - 256 bytes of
 * internal RAM, looked up thousands of times a frame (a PSRAM table cost
 * cache misses across the whole render) */
static uint16_t g_pk_lut[128], g_pk_lut_shade[128], g_pk_lut_half[128];   /* ... the spill where something stands in its way, and a half step at a shadow's soft edge */
#define PK_HALF 0.55f
static bool g_pk_lut_ok;
/* the castle in the way (2026-10-17, Strato: "the castle should block the
 * pumpkin's glow"): with the lantern BEHIND a castle IN FRONT, the castle's
 * silhouette is a shadow - its stone takes PK_SPILL of the light (the water
 * scatters a little round it) and so does anything drawn inside it - but the
 * arch's opening and the windows let the light through, so the gateway glows
 * and a fish passing it is lit. pk_shaded says which, in the frame's
 * coordinates (defined with the castle, below). */
#define PK_SPILL 0.22f
static bool g_pk_under_castle;                /* the castle stands over the lantern: its silhouette's rules */
/* THE PIECES (2026-10-17): the castle, the coral, the cluster and the pumpkin
 * are one list, back to front (tank_decor_order: depth, then rank); render.c
 * paints that list and nothing else decides who covers whom. Declared here,
 * defined with the pieces below. */
static int  g_npc;                            /* this frame's pieces */
static int  g_pk_k = -1;                      /* the lantern's index in the list, -1 = none */
static int  g_clip_k = -1;                    /* an overlay (a crown, the candle) of piece g_clip_k is being drawn: the pieces above it cover it */
static bool g_cst_clip;                       /* the castle is being painted again inside a box (render_tank's live pass, 0.3.9): castle_put skips the rest */
static int  g_cst_clip_x0, g_cst_clip_y0, g_cst_clip_x1, g_cst_clip_y1;
static int  g_castle_cx = -1;                 /* the castle's centre whatever its depth (-1 = none): its shadow's geometry */
static bool pc_covered_above(int k, int x, int y);
/* and everything else in front of a lantern BEHIND (Strato 2026-10-17: "add
 * the same shadow drawing to other things that fall in front of the pumpkin
 * like blades of sea grass and the sword plant and corals"): a blade, a
 * leaf, a coral or cluster IN FRONT is backlit - its own pixels take the
 * spill (g_pk_shade_all while they are drawn) - and casts a shadow on the
 * sand under its footprint (g_pk_cast, read by pk_shade): dark under a
 * solid piece, the spill under a grass bed (light leaks between the
 * blades), and never on the lantern's own patch of sand */
static bool g_pk_shadows;                     /* the lantern is BEHIND and lit: pk_shade has a say */
static bool g_pk_shade_all;                   /* what is being drawn stands in front of the lantern */
#define PK_CAST_MAX (VEG_BEDS_MAX * 4 + 2)
static struct { short x0, x1; signed char level; } g_pk_cast[PK_CAST_MAX];
static int g_pk_cast_n;
#define PK_FLOOR_Y (TANK_BOT - 18)            /* below this the sand begins (its top varies by a few px) */
static int  pk_shade(int x, int y);           /* 0 = lit, 1 = half, 2 = the spill, 3 = dark */
static void pk_rgb8(int i, int *r, int *g, int *b) {
    float d = sqrtf((i + 0.5f) / 128.0f), a = 0.88f * powf(1 - d, 1.7f);   /* a long soft tail: the neighbours catch it */
    *r = (int)(a * (196 - 84 * d)); *g = (int)(a * (116 - 4 * d)); *b = (int)(a * (34 + 22 * d));
}
static void pk_lut_fill(void) {
    if (g_pk_lut_ok) return;
    for (int i = 0; i < 128; i++) {
        int r, g, b; pk_rgb8(i, &r, &g, &b);
        g_pk_lut[i] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        int hr = (int)(r * PK_HALF), hg = (int)(g * PK_HALF), hb = (int)(b * PK_HALF);
        g_pk_lut_half[i] = (uint16_t)((hr >> 3) << 11 | (hg >> 2) << 5 | (hb >> 3));
        r = (int)(r * PK_SPILL); g = (int)(g * PK_SPILL); b = (int)(b * PK_SPILL);
        g_pk_lut_shade[i] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
    }
    g_pk_lut_ok = true;
}
static inline int pk_light_i(int x, int y) {      /* the table step at (x, y), -1 = outside the light */
    int dx = x - g_pk_lx, dy = y - g_pk_ly;
    if (dx <= -PK_HALO_R || dx >= PK_HALO_R || dy <= -PK_HALO_RY || dy >= PK_HALO_RY) return -1;
    int i = (dx * dx + (dy * dy * PK_DY_K >> 8)) * PK_LUT_K >> 17;
    return i < 128 ? i : -1;
}
static inline uint32_t pk_light_at(int x, int y) {
    int i = pk_light_i(x, y);
    if (i < 0) return 0;
    int sh = g_pk_shadows ? pk_shade(x, y) : 0;
    if (g_pk_shade_all && sh < 2) sh = 2;
    return sh == 0 ? g_pk_lut[i] : sh == 1 ? g_pk_lut_half[i] : sh == 2 ? g_pk_lut_shade[i] : 0;
}
static inline uint16_t px_lit(uint16_t v, uint32_t l) {   /* a 565 colour plus the light's 565 increments, saturated */
    int r = (v >> 11) + (int)(l >> 11), g = ((v >> 5) & 63) + (int)((l >> 5) & 63), b = (v & 31) + (int)(l & 31);
    return (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}
#define PK_LIGHT_PX(p, x, y) do { if (g_pk_lx >= 0) { uint32_t _l = pk_light_at((x), (y)); if (_l) *(p) = FB_SWAP16(px_lit(FB_SWAP16(*(p)), _l)); } } while (0)

/* a span that is FINAL: blended, then vignetted right here, and NOT marked
 * dirty, so the re-apply sweep never touches it (scene cache mode only) */
int g_veg_bench_dry;                              /* director `vegbench` (2026-10-23): 1 = span_final does everything but the stores; 2 = and no vignette alpha; 3 = veg_put returns at once */
/* the vignette's CLEAR ZONE per row (2026-10-24): vig_alpha is 0 within this
 * many px of the frame's centre column on row y (255: nowhere on the row).
 * Built once; a span whose middle is inside it skips the alpha's arithmetic -
 * most of the grass is (vegbench: 0.25 us of a 1.7 us span on the square) */
static uint8_t g_vig_half[TANK_H];
static bool    g_vig_half_ok;
static void vig_half_fill(void) {
    for (int y = 0; y < TANK_H; y++) {
        if (vig_alpha(TANK_W / 2, y)) { g_vig_half[y] = 255; continue; }
        int d = 0;
        while (TANK_W / 2 + d + 1 < TANK_W && d < 254 && !vig_alpha(TANK_W / 2 + d + 1, y)) d++;
        g_vig_half[y] = (uint8_t)d;
    }
    g_vig_half_ok = true;
}
static inline int vig_alpha_span(int xc, int y) {
    if (!g_vig_half_ok) vig_half_fill();
    if ((unsigned)y < (unsigned)TANK_H && g_vig_half[y] != 255) {
        int dd = xc - TANK_W / 2; if (dd < 0) dd = -dd;
        if (dd <= g_vig_half[y]) return 0;
    }
    return vig_alpha(xc, y);
}
static inline void span_final(ctx_t *c, int x0, int x1, int y, const src_t *s, int alpha) {
    if ((unsigned)(y - c->oy) >= (unsigned)c->h) return;
    int xc = g_span_xc >= 0 ? g_span_xc : (x0 + x1) >> 1;   /* the span's middle as drawn WHOLE - the frond row's, not the run's (0.3.9.4: a span clipped to a
                                                               box or split by a piece - THE PATCH - keeps the vignette the whole span has); on the glass */
    if (xc < 0) xc = 0;
    if (xc > TANK_W - 1) xc = TANK_W - 1;
    if (x0 < c->ox) x0 = c->ox;
    if (x1 > c->ox + c->w - 1) x1 = c->ox + c->w - 1;
    if (x0 > x1) return;
    TOUCH_SPAN(c, x0, x1, y);
    uint16_t *p = &CTX_PX(c, x0, y);
    /* ONE vignette alpha per span, computed (a 5 px frond span changes it by
       < 4%, invisible) rather than read: the PSRAM LUT costs a cache line
       per span and fronds are vertical, which was most of the frond cost */
    int a = g_veg_bench_dry >= 2 ? 0 : vig_alpha_span(xc, y);
    uint32_t l = g_pk_lx >= 0 ? pk_light_at(xc, y) : 0;   /* the lantern's light, one per span likewise */
    if (l || a) dirty_clear_span(x0, x1, y);    /* lit here: a fish under it must not be lit again by the dirty pass; in the vignette's ring: a crab or a
                                                   fish under it must not have the sweep dim this span a second time (2026-10-24, the watch: a front
                                                   bed's fronds a deeper green over the crab backed into the castle; the matrix) */
    if (alpha >= 255) {                         /* opaque: a store, no PSRAM read */
        uint16_t v = FB_SWAP16(l ? px_lit(s->v, l) : s->v);
        if (g_veg_bench_dry) return;
        for (int x = x0; x <= x1; x++, p++) { *p = v; if (a) px_darken(p, a); }
        return;
    }
    for (int x = x0; x <= x1; x++, p++) {
        blend565(p, s, alpha);
        if (l) *p = FB_SWAP16(px_lit(FB_SWAP16(*p), l));
        if (a) px_darken(p, a);
    }
}

/* water gradient colour of row y (shared by the scene bake and the frond
 * pre-tint below) */
static inline uint32_t water_rgb(int y) {
    float p = (float)y / TANK_BOT;                 /* the gradient runs surface to floor; under a bowl's sand line it holds */
    if (p > 1) p = 1;
    return p < 0.45f ? rgb_mix(0x0a3c46, 0x08272f, p / 0.45f)
                     : rgb_mix(0x08272f, 0x031015, (p - 0.45f) / 0.55f);
}

/* Fronds are drawn OPAQUE from a per-row pre-tinted palette (2026-09-04):
 * the 220/255 blend against the water that gave them their depth tint is
 * folded in here, once per row per colour, when the day/night dim changes -
 * so each frond pixel is a store instead of a PSRAM read-modify-write
 * (a ceiling-high jungle was ~16 of 19.5 ms on the device). Only what a
 * frond covers OTHER than water changes: a fish behind a front-layer frond
 * no longer shows through at 14%. */
#define VEG_ALPHA 220
static uint16_t g_veg_row[4][TANK_H];   /* grass pair, then the sword plant's yellow-green pair */
static float    g_veg_row_dim = -1;
static void veg_tint_fill(float dim) {
    static const uint32_t frond[4] = { 0x3f8b55, 0x2e7d4f, 0x8dbb48, 0x6c9d38 };
    if (g_veg_row_dim == dim) return;
    for (int y = 0; y < TANK_H; y++) {
        uint32_t w = water_rgb(y);
        for (int k = 0; k < 4; k++)
            g_veg_row[k][y] = rgb565(rgb_mix(w, frond[k], VEG_ALPHA / 255.0f), dim);
    }
    g_veg_row_dim = dim;
}

#define VEG_SEG_DY   VEG_SEG_PX           /* segment pitch, px of height (tank.h: 3.2, the watch 4.3) */
#define VEG_SEG_RY   2.2f                 /* the old segment ellipse's half-height */
#define VEG_MAX_SEGS (VEG_SEGS_FULL + 5)  /* tank_veg_bed tops out at VEG_SEGS_FULL */
/* final: the scene cache is live, so each frond span applies its own vignette
 * (see span_final) and needs no re-apply rect - a full canopy used to hand
 * the sweep three bed-sized boxes, the largest PSRAM traffic in the frame. */
/* a bed's depth (2026-09-16): the grass beds are woven with the fish
 * (alternate fronds behind and in front); the plant is wherever the keeper
 * put it - all behind, woven, or all in front (tank_decor_z) */
static int bed_z(const tank_t *t, int b) { return tank_bed_z(t, b); }   /* BACK or FRONT (0.3.9.3: no weave) */
/* a frond span - minus the castle's silhouette while the castle stands IN
 * FRONT of the grass (the pieces outside it are drawn; the mask and
 * g_veg_mask_cx live with the castle, below). Off the castle it is one span. */
/* DEPTH (0.3.9, tank.h DECOR_FY): a piece stands on the floor line of its
 * depth - BEHIND at the back of the sand, IN FRONT 8 px lower at its front
 * edge - so every piece's FY below is its own line plus this frame's shift
 * for it (pieces_build sets them from the tank's depths; 0 with no piece).
 * The castle's native line is the back line, the cell sprites' (the coral,
 * the cluster, the lantern) 6 px under it (sunk into the pebbles). */
static int g_dz_castle, g_dz_coral, g_dz_cl, g_dz_pk, g_dz_vo;
static inline int decor_dz(int z, int native_fy) { return DECOR_FY(z) - native_fy; }
/* and its HAZE with the water: BEHIND a little more, IN FRONT a little less (the
 * same shift says which: the back line's is 0 or -6, the front line's +8 or +2) */
static inline float decor_haze(float alpha, int dz) { float a = dz > 0 ? alpha + 0.05f : alpha - 0.08f; return a > 0.97f ? 0.97f : a; }
#define CASTLE_FY   (TANK_BOT - 16 + g_dz_castle)
#define CASTLE_ROWS 164
#define CASTLE_ARCH_R 26                     /* the opening's half-width (and the vault's radius) */
#define CASTLE_ARCH_S 24                     /* the spring line: straight jambs below, the vault above */
#define CASTLE_TRIM   5                      /* the brick trim's width */
static uint32_t g_castle_mask[CASTLE_ROWS + 12][224 / 32];        /* its silhouette, centre-relative (shape only: valid at any x) */
/* ... and its FRONT ROW alone (what stands over the fish when it is IN FRONT): castle_front() in the PSRAM decor scratch (internal RAM is spoken for) */
static uint32_t (*castle_front(void))[224 / 32];
static int      g_veg_mask_cx;
/* the arch passes (2026-10-17): with the castle IN FRONT its arch shows the
 * back of the tank, so what the frame draws AFTER the fish (the odd fronds,
 * a bed IN FRONT) must go BEFORE them inside the arch - a fish in the
 * gateway is in front of what it frames. -1: only the arch's pixels (the
 * early pass); +1: everything but them (the late pass); 0: no rule. */
static int      g_veg_arch_pass;
static bool     castle_arch(int x, int y);
/* the pieces a bed's spans leave alone (2026-10-17): a bit per piece in the
 * list - every piece IN FRONT of the plant layer for a grass bed, and for
 * the sword plant also the pieces at its own depth ranked above it
 * (render_tank sets it per bed: veg_skip_for) */
static uint32_t g_veg_skip;
/* the CONTACT SHADOW (pc_contact_shadow, below): a piece's silhouette shifted down and to the right darkens what is behind it in the
   scene - PC_SHADOW_NEAR px over, hard; PC_SHADOW_FAR, soft. A frond painted over that rim takes the same shadow (veg_rim_shade): the
   piece is in front of it, so its shadow falls on the frond too (0.3.9.4, Strato on the square: "grass clicking on the edges" of the
   pumpkin - the grass loop, the live grass and the sword plant all skipped the piece's BODY and painted bright over its shadow's rim, a
   1-2 px strip down its right side that came and went with the sway) */
#define PC_SHADOW_A_NEAR 72                /* (the offsets PC_SHADOW_NEAR / _FAR are render.h's: the tests read them) */
#define PC_SHADOW_A_FAR  34
static uint32_t g_veg_rim_n, g_veg_rim_f; static int g_veg_rim_x0;   /* this span's rim bits (near, far) from g_veg_rim_x0 on; 0 off the rim */
static bool pc_covers(int k, int x, int y);
static bool pc_in_box_rim(int k, int x0, int x1, int y);   /* the span meets piece k's box, its contact shadow's rim included */
static uint32_t pc_row_bits(int k, int x0, int n, int y);   /* piece k's mask over n (<= 32) pixels from x0 on row y */
static uint8_t g_veg_row_near[TANK_H];
/* the grass in ROW PAIRS (2026-10-24, Strato: "explore making the grass cheaper
 * to draw ... open to an altered look"): with g_veg_step 2 each frond is walked
 * every second row and each step paints two rows with one span's setup - one
 * clip, one vignette alpha, one light - so a frond costs about half the spans.
 * Its edges step in 2 px instead of 1 (the corals' cells are 2 px already).
 * Director `vegstep 1|2`, the sim's PT_VEG_STEP. */
int g_veg_step = 1;
int g_veg_thin;                                   /* director `vegthin 1` (2026-10-24, a COST PROBE only): every third frond not drawn - what a sparser bed would save */
void render_set_veg_step(int s) { g_veg_step = s == 2 ? 2 : s == 0 ? 0 : 1; }   /* (0: the grass not drawn - the director's A/B of its whole cost) */
int  render_veg_step(void) { return g_veg_step; }
static inline void span_final2(ctx_t *c, int x0, int x1, int y, uint16_t v0, uint16_t v1) {   /* rows y and y-1, opaque, final */
    if ((unsigned)(y - 1 - c->oy) >= (unsigned)(c->h - 1)) {                /* (a pair at the window's edge: row by row) */
        src_t s0 = { 0, 0, 0, v0 }, s1 = { 0, 0, 0, v1 };
        span_final(c, x0, x1, y, &s0, 255); span_final(c, x0, x1, y - 1, &s1, 255);
        return;
    }
    int xc = g_span_xc >= 0 ? g_span_xc : (x0 + x1) >> 1;   /* (the whole span's middle, as span_final) */
    if (xc < 0) xc = 0;
    if (xc > TANK_W - 1) xc = TANK_W - 1;
    if (x0 < c->ox) x0 = c->ox;
    if (x1 > c->ox + c->w - 1) x1 = c->ox + c->w - 1;
    if (x0 > x1) return;
    TOUCH_SPAN(c, x0, x1, y); TOUCH_SPAN(c, x0, x1, y - 1);
    int a = vig_alpha_span(xc, y);
    uint32_t l = g_pk_lx >= 0 ? pk_light_at(xc, y) : 0;
    if (l || a) { dirty_clear_span(x0, x1, y); dirty_clear_span(x0, x1, y - 1); }   /* (as span_final: lit or in the ring, nothing dirty stays under a final pair) */
    uint16_t w0 = FB_SWAP16(l ? px_lit(v0, l) : v0), w1 = FB_SWAP16(l ? px_lit(v1, l) : v1);
    uint16_t *p = &CTX_PX(c, x0, y), *q = &CTX_PX(c, x0, y - 1);
    if (a) for (int x = x0; x <= x1; x++, p++, q++) { *p = w0; px_darken(p, a); *q = w1; px_darken(q, a); }
    else   for (int x = x0; x <= x1; x++, p++, q++) { *p = w0; *q = w1; }
}
/* the back grass in the open arch of a castle IN FRONT (the back beds, the
 * even fronds of the woven beds) is FINAL - vignetted on the spot, never in
 * the dirty mask - so since the arch's shade went into the bake and the
 * per-frame pass re-shades only the dirty pixels (2026-10-23), those fronds
 * came back at full brightness (Strato's 1.8, the same evening: "every other
 * frond in the arch"). A final span that crosses the arch takes the tunnel's
 * shade itself, by the same alpha; the grass cache's bake goes through here
 * too, so a frame primed from it matches. Off while the lantern behind the
 * castle lights the tunnel, as arch_shade is (render_tank sets both). */
static bool g_veg_arch_shade;                     /* this frame: a castle IN FRONT whose tunnel is shaded */
static bool g_veg_arch_night;
static int  arch_shade_alpha(int y, bool night);
static void veg_arch_shade(ctx_t *c, int x0, int x1, int y) {
    if (!CTX_IN(c, x0, y) && !CTX_IN(c, x1, y)) return;
    int a = arch_shade_alpha(y, g_veg_arch_night);
    for (int x = x0; x <= x1; x++)
        if (castle_arch(x, y) && CTX_IN(c, x, y)) px_darken(&CTX_PX(c, x, y), a);
}
static inline void veg_put_run(ctx_t *c, int x0, int x1, int y, const src_t *s, bool final, bool arch);
static inline void veg_put(ctx_t *c, int x0, int x1, int y, const src_t *s, bool final, bool arch) {
    if (g_fbg_on && (unsigned)y < (unsigned)TANK_H) {   /* SWIM DEPTH: a bed IN FRONT keeps off the fish in front of it (and its even leaves off those
                                                          going through) - the span in its unguarded runs */
        const uint32_t *rf = g_fbg_front + y * DIRTY_WORDS_PER_ROW, *rt = g_fbg_thru + y * DIRTY_WORDS_PER_ROW;
        if (x0 < 0) x0 = 0;
        if (x1 > TANK_W - 1) x1 = TANK_W - 1;
        int run = -1;
        for (int x = x0; x <= x1 + 1; x++) {
            bool gd = x <= x1 && ((rf[x >> 5] >> (x & 31) & 1) || (!g_veg_odd && (rt[x >> 5] >> (x & 31) & 1)));
            if (x > x1 || gd) { if (run >= 0) { veg_put_run(c, run, x - 1, y, s, final, arch); run = -1; } }
            else if (run < 0) run = x;
        }
        return;
    }
    veg_put_run(c, x0, x1, y, s, final, arch);
}
/* the piece's contact shadow over the frond's pixels in its rim (the run as painted: a pixel the run left to a fish is left alone) */
static bool g_rim_off;                    /* (the A/B, render_debug_rim_off: the fronds bright over the rim, as before 0.3.9.4) */
void render_debug_rim_off(bool off) { g_rim_off = off; }
static inline void veg_rim_shade(ctx_t *c, int x0, int x1, int y) {
    if (!(g_veg_rim_n | g_veg_rim_f) || g_rim_off) return;
    if ((unsigned)(y - c->oy) >= (unsigned)c->h) return;
    if (x0 < c->ox) x0 = c->ox;
    if (x1 > c->ox + c->w - 1) x1 = c->ox + c->w - 1;
    for (int x = x0; x <= x1; x++) {
        unsigned i = (unsigned)(x - g_veg_rim_x0);
        if (i >= 32) continue;
        if (g_veg_rim_n >> i & 1) px_darken(&CTX_PX(c, x, y), PC_SHADOW_A_NEAR);
        else if (g_veg_rim_f >> i & 1) px_darken(&CTX_PX(c, x, y), PC_SHADOW_A_FAR);
    }
}
static inline void veg_put_run(ctx_t *c, int x0, int x1, int y, const src_t *s, bool final, bool arch) {
    if (g_veg_bench_dry >= 3) return;
    bg_mark(x0, x1, y);
    if (final && g_veg_arch_pass >= 0) {
        span_final(c, x0, x1, y, s, 255);
        if (arch) {
            if (g_veg_arch_shade && g_veg_arch_pass == 0) veg_arch_shade(c, x0, x1, y);   /* (the late pass never enters the arch) */
            else dirty_clear_span(x0, x1, y);         /* a bed IN FRONT across the opening: final, so the arch's dirty pass must not shade it over a crab or a fish */
        }
    } else span(c, x0, x1, y, s, 255);               /* the early arch pass: marked, so the arch's shade finds it (2026-10-23) and the sweep vignettes it */
    veg_rim_shade(c, x0, x1, y);
}
static inline void veg_span_runs(ctx_t *c, int x0, int x1, int y, const src_t *s, bool final);
static inline void veg_span(ctx_t *c, int x0, int x1, int y, const src_t *s, bool final) {   /* one frond row: its runs, all with the span's vignette */
    int xc = (x0 + x1) >> 1;
    if (xc < 0) xc = 0;
    if (xc > TANK_W - 1) xc = TANK_W - 1;
    g_span_xc = xc;
    if (g_veg_rowclip) {                                                  /* THE PATCH: only the row's stale columns (a box's repaint follows the piece's footprint) */
        const uint16_t *iv = g_veg_rowclip(y);
        if (iv) for (int q = 0; q < 4; q += 2) {
            const int a = x0 > iv[q] ? x0 : iv[q], b = x1 < iv[q + 1] ? x1 : iv[q + 1];
            if (a <= b) veg_span_runs(c, a, b, y, s, final);
        }
    } else veg_span_runs(c, x0, x1, y, s, final);
    g_span_xc = -1;
}
static inline void veg_span_runs(ctx_t *c, int x0, int x1, int y, const src_t *s, bool final) {
    if ((unsigned)y < (unsigned)TANK_H && !g_veg_row_near[y]) {   /* this row meets no piece and no arch (draw_veg's table): the span, or nothing in the early arch pass */
        if (g_veg_arch_pass >= 0) veg_put(c, x0, x1, y, s, final, false);
        return;
    }
    uint32_t near = 0;
    for (int k = 0; k < g_npc; k++) if (((g_veg_skip >> k) & 1) && pc_in_box_rim(k, x0, x1, y)) near |= 1u << k;
    bool arch_box = g_veg_mask_cx >= 0 && y <= CASTLE_FY && y >= CASTLE_FY - CASTLE_ARCH_S - CASTLE_ARCH_R
                    && x1 >= g_veg_mask_cx - CASTLE_ARCH_R && x0 <= g_veg_mask_cx + CASTLE_ARCH_R;
    if (g_veg_arch_pass < 0 && !arch_box) return;      /* the early arch pass: nothing outside the arch */
    if (!near && !(g_veg_arch_pass && arch_box)) {      /* the usual frond: one span */
        veg_put(c, x0, x1, y, s, final, arch_box);
        return;
    }
    /* near a piece: the pieces' bits over the span, one look per piece
       (a frond is a few px wide: one or two mask words), then the runs they
       leave. A span over 32 px (none today) goes pixel by pixel. */
    int n = x1 - x0 + 1;
    uint32_t m = 0, rn = 0, rf = 0, full = n < 32 ? (1u << n) - 1 : 0xffffffffu;
    if (n <= 32) {
        for (int k = 0; k < g_npc; k++) if ((near >> k) & 1) {
            m |= pc_row_bits(k, x0, n, y);
            rn |= pc_row_bits(k, x0 - PC_SHADOW_NEAR, n, y - PC_SHADOW_NEAR);   /* the piece's contact shadow: where its body stands up and to the left */
            rf |= pc_row_bits(k, x0 - PC_SHADOW_FAR, n, y - PC_SHADOW_FAR);
        }
        if (g_veg_arch_pass && arch_box)                 /* the arch: the early pass draws only it, the late one skips it */
            for (int i = 0; i < n; i++) { bool a = castle_arch(x0 + i, y); if (g_veg_arch_pass > 0 ? a : !a) m |= 1u << i; }
        rn &= ~m & full; rf &= ~m & ~rn & full;
        g_veg_rim_x0 = x0; g_veg_rim_n = rn; g_veg_rim_f = rf;   /* (veg_rim_shade, after each run) */
        if (!m) { veg_put(c, x0, x1, y, s, final, arch_box); g_veg_rim_n = g_veg_rim_f = 0; return; }
        if (m == full) { g_veg_rim_n = g_veg_rim_f = 0; return; }
    }
    int run = -1;
    for (int i = 0; i <= n; i++) {
        bool in = i == n;
        if (!in) {
            if (n <= 32) in = (m >> i) & 1;
            else {
                for (int k = 0; k < g_npc && !in; k++) if (((near >> k) & 1) && pc_covers(k, x0 + i, y)) in = true;
                if (!in && g_veg_arch_pass && arch_box) { bool a = castle_arch(x0 + i, y); if (g_veg_arch_pass > 0 ? a : !a) in = true; }
            }
        }
        if (!in) { if (run < 0) run = x0 + i; }
        else if (run >= 0) {
            veg_put(c, run, x0 + i - 1, y, s, final, arch_box);
            run = -1;
        }
    }
    g_veg_rim_n = g_veg_rim_f = 0;
}
/* layer: 0 = the even fronds, 1 = the odd ones, -1 = every frond */
static const int VEG_SEED[VEG_BEDS_MAX] = { 0, 7, 3, 5 };   /* each bed's sway phase (the seahorse's blade reads it too) */
/* g_veg_row_near (declared with veg_span above): per row of the bed being drawn: the pieces whose boxes its spans can meet (bits by
                                                              * piece), and bit 7 = the row crosses a castle's open arch (draw_veg fills it once a
                                                              * bed; veg_span reads it: most rows meet nothing and take the fast path - 2026-10-23) */
static int g_veg_force_layer = -1;                 /* (EXPLORATION 10-31, the sim's --depth-shot: only the even or the odd fronds) */
void render_proto_veg_layer(int layer) { g_veg_force_layer = layer; }
/* THE HELD HEIGHTS (0.3.9.4, the flicker on Strato's square and 1.8): while the grass loop is up, EVERY drawing of the back grass -
   its bands and moments, a frame drawn live while the loop is stale or parked, a box repainted, the seahorse's blade - takes the
   frond heights the loop was last baked toward (g_gl_seg, the stale event's), not the tank's of this frame. Before, a live frame
   between band frames drew the current heights, and a frond the urchin had nibbled since the bake jumped 3-4 px every band
   frame for as long as a rebake took. A height change shows when the loop takes it (at once for a trim, GL_SEG_WAIT_S else). */
static const uint8_t (*g_veg_seg_hold)[VEG_FRONDS_MAX];
#define GL_SEG_JUMP   4                           /* (the loop's rule, below: a frond this many segments off is taken at once - the hold reaches no further,
                                                     so another tank drawn under the same loop, the sim's tests' bare copies, keeps its own heights) */
static inline int veg_segs_of(const tank_t *t, int b, int i, float *bx) {
    int sg = tank_veg_frond(t, b, i, bx);
    if (g_veg_seg_hold && b < VEG_BEDS_MAX && i < VEG_FRONDS_MAX && bed_z(t, b) == DECOR_Z_BACK) {
        const int h = g_veg_seg_hold[b][i], d = h > sg ? h - sg : sg - h;
        if (d < GL_SEG_JUMP) sg = h;
    }
    return sg;
}
static void draw_veg(ctx_t *c, const tank_t *t, int b, int seed, int layer, bool final) {
    if (!g_veg_step) return;
    if (g_veg_force_layer >= 0) layer = g_veg_force_layer;                         /* director `vegstep 0`: the A/B of the grass's whole cost */
    veg_tint_fill(c->dim);
    int n; float bx0, bx1; tank_veg_bed(t, b, &bx0, &bx1, NULL, &n);
    /* the sword plant (bed 3, the shop): broad lanceolate leaves - a narrow
       stem, 7 px half-width at 40% of the way up, a point at the tip - a
       lazier sway, its own yellow-green pair */
    bool sword = tank_veg_kind(t, b) == VEG_KIND_SWORD;
    float root = 2.4f, tip = 0.4f, amp = sword ? 2.5f : 4.0f;
    /* once a bed, not a span (2026-10-23: the tests below ran for every one of a frame's
       ~6000 spans): per row, the pieces whose boxes the bed's spans can meet (+-8: the
       sway and the half-width), and whether the row crosses a castle's open arch */
    { int y_lo = (int)(TANK_BOT - 16 - (VEG_MAX_SEGS - 1) * VEG_SEG_DY - VEG_SEG_RY), y_hi = (int)(TANK_BOT - 16 + VEG_SEG_RY);
      if (y_lo < 0) y_lo = 0;
      if (y_hi > TANK_H - 1) y_hi = TANK_H - 1;
      int xa = (int)bx0 - 8, xb = (int)bx1 + 8;
      for (int y = y_lo; y <= y_hi; y++) {
          uint8_t m = 0;
          for (int k = 0; k < g_npc; k++) if (((g_veg_skip >> k) & 1) && pc_in_box_rim(k, xa, xb, y)) m |= (uint8_t)(1u << k);
          if (g_veg_mask_cx >= 0 && y <= CASTLE_FY && y >= CASTLE_FY - CASTLE_ARCH_S - CASTLE_ARCH_R
              && xb >= g_veg_mask_cx - CASTLE_ARCH_R && xa <= g_veg_mask_cx + CASTLE_ARCH_R) m |= 0x80;
          g_veg_row_near[y] = m;
      } }
    for (int i = 0; i < n; i++) {
        if (layer >= 0 && (i & 1) != layer) continue;
        g_veg_odd = i & 1;                         /* (SWIM DEPTH: its pixels into g_om too, when recording) */
        if (g_veg_thin && b != 3 && i % 3 == 2) continue;   /* (the cost probe: a sparser bed) */
        float bx;
        /* each frond's own height (tank_t.veg_h): what the keeper cut is
           exactly what shows - no render-side variation on top */
        int segs = veg_segs_of(t, b, i, &bx);
        if (g_veg_arch_pass < 0 && (bx + 14 < g_veg_mask_cx - CASTLE_ARCH_R || bx - 14 > g_veg_mask_cx + CASTLE_ARCH_R)) continue;   /* the early arch pass: only the fronds that reach the arch */
        if (g_veg_cull && (bx + GL_BED_PAD < c->ox || bx - GL_BED_PAD > c->ox + c->w - 1)) continue;   /* THE PATCH: a frond clear of the box */
        if (segs < 1) segs = 1;                    /* nubs: always a bit of green */
        if (segs > VEG_MAX_SEGS) segs = VEG_MAX_SEGS;
        const float vc = veg_clock(t->clock) * VEG_W;
        float sway = fast_sin(vc + (i + seed) * 1.7f) * amp;
        /* the swaying chain of segment centres (the frond's spine) */
        float xs[VEG_MAX_SEGS + 1];
        for (int seg = 0; seg <= segs; seg++)
            xs[seg] = bx + sway * seg / (float)segs * fast_sin(seg * 0.4f + vc + i + seed);
        /* one span per pixel row, centred on the spine, half-width tapering
         * toward the tip: the same silhouette the old chain of overlapping
         * ellipses drew (their union was a 5 px ribbon), at ~a quarter fewer
         * pixels and without a sqrt per row. Full canopy = ~1000 segments.
         * The rows are WALKED (2026-10-23: the interpolation chain per row -
         * the segment, its fraction, the taper - was 0.86 us of a 1.86 us
         * span on the square): the spine's x and the half-width advance by a
         * constant a row within a segment, the taper by a constant a row
         * (the sword's in two legs), so a row costs a few adds. */
        const uint16_t *pal = g_veg_row[(sword ? 2 : 0) + ((i + seed) & 1)];
        int y_bot = (int)(TANK_BOT - 16 + VEG_SEG_RY);
        int y_top = (int)(TANK_BOT - 16 - (segs - 1) * VEG_SEG_DY - VEG_SEG_RY);
        const float dsp = 1.0f / VEG_SEG_DY, segf = (float)(segs - 1), inv = segs > 1 ? 1.0f / (segs - 1) : 1.0f;
        /* the taper: grass thins root -> tip in a line; the sword widens to u = 0.4 and thins to the point */
        const float dh_grass = -(root - tip) * inv * dsp;
        const float sw_peak = 0.4f * segf;                      /* the sword's widest segment */
        const float dh_sw_up = 6.0f * 2.5f * inv * dsp, dh_sw_dn = -6.0f * (1.0f / 0.6f) * inv * dsp;
        const int st = g_veg_step;                              /* rows a step: 1, or 2 (row pairs, above) */
        float sp = (TANK_BOT - 16 - y_bot) * dsp;              /* the first row sits under the root: sp < 0 is clamped */
        int k = 0; float cx = xs[0], dcx = (xs[1] - xs[0]) * dsp, half, dhalf;
        if (sword) { half = 1.2f; dhalf = dh_sw_up; } else { half = root; dhalf = dh_grass; }
        for (int y = y_bot; y >= y_top; y -= st, sp += dsp * st) {
            if (sp >= 0) {
                if (sp >= segf) {                               /* the tip rows: held at the last segment */
                    if (dcx != 0 || dhalf != 0) { half = sword ? 1.2f : tip; dcx = 0; dhalf = 0; }
                    cx = xs[segs - 1];
                } else {
                    int kk = (int)sp;
                    if (kk != k) {                              /* into the next segment: its own slope, re-anchored (no drift) */
                        k = kk; dcx = (xs[k + 1] - xs[k]) * dsp; cx = xs[k] + (xs[k + 1] - xs[k]) * (sp - k);
                        if (sword) { dhalf = sp < sw_peak ? dh_sw_up : dh_sw_dn; half = sp < sw_peak ? 1.2f + 6.0f * (sp * inv) * 2.5f : 1.2f + 6.0f * (1 - sp * inv) * (1.0f / 0.6f); }
                        else half = root - (root - tip) * sp * inv;
                    } else { cx += dcx * st; half += dhalf * st; }
                    if (sword && dhalf > 0 && sp >= sw_peak) { dhalf = dh_sw_dn; half = 1.2f + 6.0f * (1 - sp * inv) * (1.0f / 0.6f); }
                }
            }
            float h = half < tip ? tip : half;
            int x0 = (int)(cx - h), x1 = (int)(cx + h);
            if (g_veg_cull && (y + 1 < c->oy || y - 1 > c->oy + c->h - 1)) continue;   /* THE PATCH: a row clear of the box (the walk above has moved on) */
            if (st == 2 && y >= 1) {                            /* a row pair: one span's work when the pair meets nothing */
                if (final && g_veg_arch_pass >= 0 && !g_veg_row_near[y] && !g_veg_row_near[y - 1] && !g_veg_bench_dry) { span_final2(c, x0, x1, y, pal[y], pal[y - 1]); bg_mark(x0, x1, y); bg_mark(x0, x1, y - 1); }
                else { src_t s0; s0.v = pal[y]; veg_span(c, x0, x1, y, &s0, final); src_t s1; s1.v = pal[y - 1]; veg_span(c, x0, x1, y - 1, &s1, final); }
                continue;
            }
            src_t s; s.v = pal[y];                              /* opaque: only .v is read */
            veg_span(c, x0, x1, y, &s, final);
        }
    }
}


/* director `vegbench` (2026-10-23): one bed's fronds drawn 20 times into fb, with and without the stores */
void render_veg_bench(const tank_t *t, uint16_t *fb, int bed, int64_t (*clock_us)(void), int64_t *full_us, int64_t dry_us[3], int *spans) {
    ctx_t c = ctx_full(fb, TANK_W, 1.0f);
    g_veg_skip = 0; g_veg_mask_cx = -1; g_veg_arch_pass = 0;
    int n; tank_veg_bed(t, bed, NULL, NULL, NULL, &n);
    int rows = 0; for (int i = 0; i < n; i++) { float bx; int s = tank_veg_frond(t, bed, i, &bx); if (s < 1) s = 1; rows += (int)(s * VEG_SEG_DY + 2 * VEG_SEG_RY); }
    *spans = rows;
    int64_t t0 = clock_us(); for (int k = 0; k < 20; k++) draw_veg(&c, t, bed, VEG_SEED[bed], -1, true); *full_us = (clock_us() - t0) / 20;
    for (int m = 1; m <= 3; m++) {
        g_veg_bench_dry = m;
        t0 = clock_us(); for (int k = 0; k < 20; k++) draw_veg(&c, t, bed, VEG_SEED[bed], -1, true); dry_us[m - 1] = (clock_us() - t0) / 20;
    }
    g_veg_bench_dry = 0;
}
/* ---- the castle (2026-09-16, from Strato's castle-v2 mockup) ----
 * A swim-through decoration drawn the way the fish and the fronds are -
 * spans and fills from a little geometry, no bitmap - so it sits in the same
 * water as everything else instead of a pixel-art sprite clashing with it.
 * Two layers: the KEEP (towers, battlements, the base, the dark courtyard
 * behind the arch) is static and bakes into the scene cache with the floor;
 * the GATE WALL (the curtain wall round the arch, its brick trim and jambs)
 * draws after the fish every frame, so a fish crossing the arch is tucked
 * behind the jambs and seen through the opening - it swims THROUGH. Stone
 * is a brick-course pattern from a position hash (the pebbled floor's
 * trick), golden-lit from the upper left like the mockup, mossed toward the
 * base, and pre-tinted with the water of its row like the fronds - the gate
 * a touch less than the keep, so the keep reads farther back. Geometry is
 * in px about the centre x on the floor line; ~176 x 150 px, the arch
 * opening 52 x 50 (an adult fish is ~37 x 22). */
/* CASTLE_FY (the floor line the keep stands on) and CASTLE_ROWS (rows above
 * it the palette covers) are defined with veg_span above */
enum { CT_LIGHT, CT_MID, CT_MIDDK, CT_DARK, CT_MORTAR, CT_MOSS_A, CT_MOSS_B,
       CT_BRICK_L, CT_BRICK, CT_BRICK_D, CT_ROOF_L, CT_ROOF, CT_ROOF_D, CT_INSIDE, CT_N };
static const uint32_t CASTLE_RGB[CT_N] = {
    0xd9c58c, 0xa99b7b, 0x87795f, 0x5d5446, 0x6a6151, 0x7aa33c, 0x527f2e,
    0xe08a58, 0xc25f38, 0x8b3d24, 0xe4cd8a, 0xc4a95e, 0x8f7a42, 0x061219 };
/* [keep | gate][tone][row], row 0 = CASTLE_FY - CASTLE_ROWS */
static uint16_t g_castle_row[2][CT_N][CASTLE_ROWS + 12];
static float    g_castle_dim = -1;
static int      g_castle_dzk = 99;                                   /* the shift (= the depth) the rows were hazed for */
/* the castle's silhouette, one bit per pixel in local coords (x - cx + 112,
 * 0..223; row 0 = CASTLE_FY - CASTLE_ROWS): every pixel castle_put writes
 * sets its bit, so the first full draw fills it. While the castle stands IN
 * FRONT of the grass, frond spans skip the pixels inside it (veg_span). */
#define CASTLE_MASK_W 224
static void castle_tint_fill(float dim) {
    if (g_castle_dim == dim && g_castle_dzk == g_dz_castle) return;
    float keep = decor_haze(0.70f, g_dz_castle), gate = decor_haze(0.82f, g_dz_castle);   /* by its depth (0.3.9) */
    for (int r = 0; r < CASTLE_ROWS + 12; r++) {
        int y = CASTLE_FY - CASTLE_ROWS + r;
        uint32_t w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y);
        for (int k = 0; k < CT_N; k++) {
            g_castle_row[0][k][r] = rgb565(rgb_mix(w, CASTLE_RGB[k], k == CT_INSIDE ? 0.6f : keep), dim);
            g_castle_row[1][k][r] = rgb565(rgb_mix(w, CASTLE_RGB[k], k == CT_INSIDE ? 0.6f : gate), dim);
        }
    }
    g_castle_dim = dim; g_castle_dzk = g_dz_castle;
}
/* moss creeps up from the base: 4 px cells, a chance that fades with height,
 * feathered per pixel so the patches have ragged edges */
static inline int castle_moss(int lx, int y, int extra_pct) {
    int d = CASTLE_FY - y;
    int pct = (d < 36 ? 26 - d * 26 / 36 : 0) + extra_pct;
    if (pct <= 0) return -1;
    uint32_t h = chash((lx + 4096) >> 2, y >> 2);
    if ((int)(h % 100) >= pct) return -1;
    if ((chash(lx, y) & 7) == 0) return -1;
    return (h >> 8) & 1 ? CT_MOSS_A : CT_MOSS_B;
}
/* a pixel's tone. mode 0 = WALL (5 px brick courses, 9 px bricks), 1 = ROOF
 * (4 px tile courses), 2 = RUBBLE (the base: no courses, mossy), 3 = the
 * brick JAMBS (courses), 4 = INSIDE. u = 0..1 across the element: the left
 * fifth catches the light, the right fifth falls into shadow. */
static int castle_tone(int mode, int lx, int y, float u) {
    if (mode == 4) return CT_INSIDE;
    int d = CASTLE_FY - y;
    int lit = u < 0.22f ? -1 : u > 0.78f ? 1 : 0;
    if (mode == 2) {
        int m = castle_moss(lx, y, 18); if (m >= 0) return m;
        static const int8_t pick[8] = { CT_MIDDK, CT_DARK, CT_DARK, CT_MIDDK, CT_MID, CT_DARK, CT_MIDDK, CT_DARK };
        int t = pick[chash(lx >> 1, y) & 7] + (lit < 0 ? -1 : 0);
        return t < CT_MID ? CT_MID : t;
    }
    int pitch = mode == 1 ? 4 : 5, len = mode == 1 ? 6 : 9;
    int course = d / pitch;
    if (d % pitch == 0) return mode == 3 ? CT_BRICK_D : mode == 1 ? CT_ROOF_D : CT_MORTAR;
    int bx = lx + 4096 + ((course & 1) ? len / 2 : 0);
    if (bx % len == 0) return mode == 3 ? CT_BRICK_D : mode == 1 ? CT_ROOF_D : CT_MORTAR;
    if (mode == 3) {
        static const int8_t pick[4] = { CT_BRICK, CT_BRICK, CT_BRICK_L, CT_BRICK_D };
        int t = pick[chash(course, bx / len) & 3] + lit;
        return t < CT_BRICK_L ? CT_BRICK_L : t > CT_BRICK_D ? CT_BRICK_D : t;
    }
    if (mode == 1) {
        static const int8_t pick[4] = { CT_ROOF, CT_ROOF, CT_ROOF_L, CT_ROOF_D };
        int t = pick[chash(course, bx / len) & 3] + lit;
        return t < CT_ROOF_L ? CT_ROOF_L : t > CT_ROOF_D ? CT_ROOF_D : t;
    }
    int m = castle_moss(lx, y, 0); if (m >= 0) return m;
    static const int8_t pick[8] = { CT_MID, CT_MID, CT_MID, CT_MIDDK, CT_MID, CT_MIDDK, CT_LIGHT, CT_MIDDK };
    int t = pick[chash(course, bx / len) & 7] + lit;
    return t < CT_LIGHT ? CT_LIGHT : t > CT_DARK ? CT_DARK : t;
}
typedef struct { ctx_t *c; int cx; int layer; bool final; } cst_t;
static inline void castle_put(const cst_t *k, int x, int y, uint16_t v) {
    ctx_t *c = k->c;
    if (!CTX_IN(c, x, y)) return;
    if (g_cst_clip && (x < g_cst_clip_x0 || x > g_cst_clip_x1 || y < g_cst_clip_y0 || y > g_cst_clip_y1)) return;   /* a repaint inside a box (0.3.9) */
    { int lx = x - k->cx + CASTLE_MASK_W / 2, r = y - (CASTLE_FY - CASTLE_ROWS);
      if ((unsigned)lx < CASTLE_MASK_W && (unsigned)r < CASTLE_ROWS + 12) {
          g_castle_mask[r][lx >> 5] |= 1u << (lx & 31);
          if (k->layer == 1) castle_front()[r][lx >> 5] |= 1u << (lx & 31);
      } }
    uint16_t *p = &CTX_PX(c, x, y);
    *p = FB_SWAP16(v); TOUCH_PX(c, x, y);
    if (k->final) {                          /* vignetted here, and untagged: the sweep must not darken it again */
        PK_LIGHT_PX(p, x, y);
        int a = g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
        if (a) px_darken(p, a);
        if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
    } else dirty_px(x, y);
}
/* one row of an element: local x lx0..lx1 on tank row y; ux0..ux1 = the
 * element's full width on that row, for the lighting */
static void castle_span(const cst_t *k, int lx0, int lx1, int y, int mode, float ux0, float ux1) {
    int r = y - (CASTLE_FY - CASTLE_ROWS);
    if (r < 0 || r >= CASTLE_ROWS + 12) return;
    const ctx_t *c = k->c;                       /* clip to the window first: tones cost */
    if ((unsigned)(y - c->oy) >= (unsigned)c->h) return;
    if (k->cx + lx0 < c->ox) lx0 = c->ox - k->cx;
    if (k->cx + lx1 > c->ox + c->w - 1) lx1 = c->ox + c->w - 1 - k->cx;
    float uw = ux1 > ux0 ? ux1 - ux0 : 1;
    for (int lx = lx0; lx <= lx1; lx++)
        castle_put(k, k->cx + lx, y, g_castle_row[k->layer][castle_tone(mode, lx, y, (lx - ux0) / uw)][r]);
}
static void castle_rect(const cst_t *k, int lx0, int lx1, int ytop, int ybot, int mode) {
    for (int y = ytop; y <= ybot; y++) castle_span(k, lx0, lx1, y, mode, (float)lx0, (float)lx1);
}
/* a conical roof: half_base wide at ybase, a point at yapex, plus an eave
 * a pixel wider than the tower */
static void castle_cone(const cst_t *k, int lxc, int half_base, int ybase, int yapex) {
    for (int y = yapex; y <= ybase; y++) {
        float t = (y - yapex) / (float)(ybase - yapex);
        int half = (int)(half_base * t + 0.5f);
        castle_span(k, lxc - half, lxc + half, y, 1, (float)(lxc - half), (float)(lxc + half));
    }
}
/* merlons along a top edge: 7 wide, 7 tall, on a 12 px pitch */
static void castle_merlons(const cst_t *k, int lx0, int lx1, int ytop) {
    for (int x = lx0; x + 6 <= lx1; x += 12) castle_rect(k, x, x + 6, ytop - 7, ytop - 1, 0);
}
/* an arched window: w wide, from ybot up to ytop, the vault a half-disc */
static void castle_window(const cst_t *k, int lxc, int w, int ytop, int ybot) {
    float r = w / 2.0f;
    for (int y = ytop; y <= ybot; y++) {
        float dy = (ytop + r) - y;
        float half = dy > 0 ? r * ell_half(dy / r) : r;
        castle_span(k, (int)(lxc - half + 0.5f), (int)(lxc + half - 0.5f), y, 4, 0, 1);
    }
}
/* the gate: the arch's brick trim (a ring of voussoirs over the vault, jambs
 * down the sides) and the opening. A static tone map for the ring: the
 * voussoir index needs an angle, and the gate redraws every frame. */
static int8_t g_ring[CASTLE_ARCH_R + CASTLE_TRIM + 1][2 * (CASTLE_ARCH_R + CASTLE_TRIM) + 1];
static bool   g_ring_filled = false;
static void castle_ring_fill(void) {
    if (g_ring_filled) return;
    int R = CASTLE_ARCH_R + CASTLE_TRIM;
    for (int j = 0; j <= R; j++)
        for (int i = 0; i <= 2 * R; i++) {
            float dx = i - R, dy = (float)j;                 /* dy up from the spring line */
            float d = sqrtf(dx * dx + dy * dy);
            int8_t t = -1;
            if (d >= CASTLE_ARCH_R && d < R + 0.5f) {
                float ang = atan2f(dy, dx) / 3.14159265f * 9;   /* 9 voussoirs across the half-turn */
                int v = (int)ang; float fr = ang - v;
                t = fr < 0.14f ? CT_BRICK_D : (chash(v, 3) & 3) == 0 ? CT_BRICK_L : (chash(v, 3) & 3) == 1 ? CT_BRICK_D : CT_BRICK;
                if (v < 3 && t == CT_BRICK) t = CT_BRICK_L;    /* the lit side */
                if (v > 5 && t == CT_BRICK) t = CT_BRICK_D;
            }
            g_ring[j][i] = t;
        }
    g_ring_filled = true;
}
static void castle_gate_trim(const cst_t *k) {
    int R = CASTLE_ARCH_R + CASTLE_TRIM, ys = CASTLE_FY - CASTLE_ARCH_S;
    castle_ring_fill();
    for (int j = 0; j <= R; j++) {
        int y = ys - j, r = y - (CASTLE_FY - CASTLE_ROWS);
        if (r < 0) continue;
        for (int i = 0; i <= 2 * R; i++)
            if (g_ring[j][i] >= 0) castle_put(k, k->cx + i - R, y, g_castle_row[k->layer][g_ring[j][i]][r]);
    }
    castle_rect(k, -R, -CASTLE_ARCH_R - 1, ys + 1, CASTLE_FY, 3);
    castle_rect(k,  CASTLE_ARCH_R + 1, R,  ys + 1, CASTLE_FY, 3);
}
/* the gate wall: the curtain between the left tower and the right, minus the
 * opening. Drawn in the keep pass (baked) and again after the fish. */
#define CASTLE_GATE_X0 (-34)
#define CASTLE_GATE_X1  55
#define CASTLE_WALL_TOP (CASTLE_FY - 58)
/* the keep's curtain wall behind the gate, minus the opening (2026-10-17,
 * Strato: "let visibility pass straight through the castle arch so we see to
 * the back of the tank"): the arch paints NOTHING, so the water, the floor
 * and the pieces baked behind the castle show in it, the grass and the plant
 * draw through it (veg_span: the opening is not in the castle's mask), and a
 * fish in the gateway stays in front of them (render_tank's arch passes).
 * castle_opening's dark courtyard is gone with it. */
static void castle_back_wall(const cst_t *k) {
    int ys = CASTLE_FY - CASTLE_ARCH_S, R = CASTLE_ARCH_R;
    for (int y = CASTLE_WALL_TOP; y <= CASTLE_FY; y++) {
        float half = y > ys ? R : (ys - y) < R ? R * ell_half((ys - y) / (float)R) : 0;
        int h = (int)(half - 0.5f);
        if (half < 1) castle_span(k, -44, 44, y, 0, -44, 44);
        else { castle_span(k, -44, -h - 1, y, 0, -44, 44); castle_span(k, h + 1, 44, y, 0, -44, 44); }
    }
}
static void castle_gate_wall(const cst_t *k) {
    int ys = CASTLE_FY - CASTLE_ARCH_S, R = CASTLE_ARCH_R + CASTLE_TRIM;
    for (int y = CASTLE_WALL_TOP; y <= CASTLE_FY; y++) {
        float half = y > ys ? R : (ys - y) < R ? R * ell_half((ys - y) / (float)R) : 0;
        int h = (int)half;
        if (h < 1) castle_span(k, CASTLE_GATE_X0, CASTLE_GATE_X1, y, 0, -44, 44);
        else {
            castle_span(k, CASTLE_GATE_X0, -h - 1, y, 0, -44, 44);
            castle_span(k, h + 1, CASTLE_GATE_X1, y, 0, -44, 44);
        }
    }
    castle_merlons(k, -44, 44, CASTLE_WALL_TOP);
    castle_gate_trim(k);
}
/* the FRONT ROW: the gate wall and the three towers flush with it. Drawn in
 * the keep pass (baked) and again over the fish, clipped to their rects. */
static void castle_front_row(const cst_t *k) {
    int FY = CASTLE_FY;
    castle_gate_wall(k);
    /* the left tower, pointed */
    castle_rect(k, -62, -34, FY - 78, FY, 0);
    castle_cone(k, -48, 18, FY - 78, FY - 108);
    castle_window(k, -48, 7, FY - 62, FY - 48);
    /* the short far-left tower */
    castle_rect(k, -86, -60, FY - 46, FY, 0);
    castle_cone(k, -73, 17, FY - 46, FY - 72);
    castle_window(k, -73, 6, FY - 30, FY - 19);
    /* the open far-right tower, crenellated */
    castle_rect(k, 56, 86, FY - 70, FY, 0);
    castle_merlons(k, 56, 86, FY - 70);
    castle_window(k, 71, 7, FY - 44, FY - 30);
}
/* the whole castle (front = 0: the keep, then the front row over it - the
 * scene bake), or the front row alone (front = 1: over the fish) */
static void draw_castle(ctx_t *c, int cx, int front, bool final) {
    castle_tint_fill(c->dim);
    cst_t k = { c, cx, 1, final };
    if (front) { castle_front_row(&k); return; }
    k.layer = 0;
    int FY = CASTLE_FY;
    /* the base: a low rubble mound the towers stand on */
    for (int y = FY - 9; y <= FY + 6; y++) {
        float w = ell_half((y - (FY - 1)) / 8.0f);
        if (w <= 0) continue;
        int half = (int)(92 * w);
        castle_span(&k, -half, half, y, 2, (float)-half, (float)half);
    }
    /* the rear tower, tallest, right of centre: behind the wall */
    castle_rect(&k, 24, 54, FY - 108, FY, 0);
    castle_cone(&k, 39, 19, FY - 108, FY - 142);
    castle_window(&k, 39, 7, FY - 92, FY - 78);
    /* the curtain wall behind the gate's opening, then the courtyard's dark */
    castle_back_wall(&k);                    /* the arch is OPEN (2026-10-17): the tank behind shows through it */
    k.layer = 1;
    castle_front_row(&k);
}

/* ---- the coral, procedural (2026-09-23; Strato's coral-single.png: a
 * branching orange fan in chunky pixel art - rounded branches, a dark red
 * rim, sunlit yellow tips). Built ONCE as a tone sprite on a 2 px cell grid
 * (CORAL_CW x CORAL_CH cells): a skeleton of capsules (the trunk and its
 * branches), each cell classed by its signed distance to the nearest one -
 * the rim, a lit edge up-left, the tips, and a hashed speckle inside. The
 * colour is the keeper's (tank_coral_rgb): five tones derived from it and
 * hazed with the water per row, like the castle's stone. ~60 x 92 px. */
#define CORAL_CELL 2
#define CORAL_CW   32
#define CORAL_CH   46
#define CORAL_W    (CORAL_CW * CORAL_CELL)
#define CORAL_H    (CORAL_CH * CORAL_CELL)
#define CORAL_FY   (TANK_BOT - 14 + DECOR_SINK + g_dz_coral)   /* the base, sunk into the pebbles (a mound covers the joint); on its depth's line (0.3.9) */
enum { CO_NONE = 0, CO_RIM, CO_BODY, CO_SHADE, CO_LIT, CO_TIP, CO_N };
static int      g_coral_q = -1;               /* the growth step the sprite was built for (CORAL_Q steps) */
#define CORAL_Q 128                           /* ~5.6 h per step over the 30 days: a rebuild each */
static int      g_coral_cells;                /* filled cells in the sprite (the selftest) */
static uint32_t g_coral_rgb = 1;              /* what the rows hold (1 = never) */
static float    g_coral_dim = -1;
static int      g_coral_dzk = 99;
/* the decor scratch (2026-09-24): every table the coral and the reef cluster
 * build - sprites, tinted rows, the distance grids - lives in ONE block the
 * platform hands over (render_set_decor_scratch: the firmware gives PSRAM,
 * since internal RAM is down to ~23 KB free and the display driver needs
 * 57 KB of DMA memory at boot - the first cluster build's statics cost it
 * that and the tank booted black). Without one it is calloc'd (the sim). */
#define CL_CW   72
#define CL_CH   60
#define CL_H    (CL_CH * 2)
#define CL_TONES (1 + 5 * 5)
#define PK_CELL 2                                 /* the pumpkin lantern (below): a 2 px cell sprite like the coral's */
#define PK_CW   44
#define PK_CH   41
#define PK_N    16
#define VO_CELL 2                                 /* the bubble volcano (0.3.9, below): a 2 px cell sprite like the lantern's */
#define VO_CW   92
#define VO_CH   104
#define VO_W    (VO_CW * VO_CELL)
#define VO_H    (VO_CH * VO_CELL)
#define VO_GLOW_MAX 640
enum { VO_NONE = 0, VO_ROCK_L, VO_ROCK, VO_ROCK_D, VO_ROCK_DD, VO_SCORCH, VO_MOSS_A, VO_MOSS_B, VO_CAVE, VO_CAVE_MID,
       VO_CAVE_LIT, VO_LAVA_DIM, VO_LAVA, VO_LAVA_HOT, VO_LAVA_CORE, VO_N };          /* VO_CAVE_LIT.. glow: the lava's */
#define CR_CACHE_N 24                              /* the crab's grids kept (draw_crab): a walk is 16 poses, a climb ~26 */
typedef struct {
    uint8_t  co_sprite[CORAL_CH][CORAL_CW];
    uint16_t co_row[CO_N][CORAL_H];           /* [tone][row], row 0 = CORAL_FY - CORAL_H */
    int8_t   co_sd[CORAL_CH][CORAL_CW];
    uint8_t  co_tp[CORAL_CH][(CORAL_CW + 7) / 8];
    uint8_t  cl_sprite[CL_CH][CL_CW];
    uint16_t cl_row[CL_TONES][CL_H];
    int8_t   cl_sd[CL_CH][CL_CW];
    uint8_t  cl_dp[CL_CH][(CL_CW + 7) / 8];
    uint32_t pm[5][VO_H + 4][(CASTLE_MASK_W + 31) / 32];           /* the pieces' masks (2026-10-17): what each piece covers, by its ITEM's slot (pc_mslot:
                                                                      the castle, the coral, the cluster, the pumpkin, the volcano - 0.3.9.4: by its place in
                                                                      the list before, which shifted under a baked piece when a piece ahead of it went live)
                                                                    * in the baked scene (sprite + mound); row 0 = its box's top, bit 0 its left
                                                                    * (the volcano, 0.3.9, is the tallest box) */
    uint32_t pm_live[VO_H + 4][(CASTLE_MASK_W + 31) / 32];        /* the page's LIVE piece's mask, this frame (0.4): apart from its baked one in pm, so a
                                                                      CANCEL leaves the scene's masks as the scene has them (the coral-sliver lesson) */
    uint32_t castle_front[CASTLE_ROWS + 12][(CASTLE_MASK_W + 31) / 32];   /* the castle's front row, centre-relative (castle_put) */
    /* the seahorse's frame scratch (0.3.6): its sprite (a byte a pixel), the
     * tail's distance map and its path - in PSRAM, never the stack or .bss */
    uint8_t  sh_spr[39 * 23], sh_d[39 * 23], sh_fr[39 * 23], sh_fl[39 * 23];
    uint8_t  sh_spr_card[39 * 23];                          /* the card's portrait: its own, so the tank's cached sprite
                                                             * is not overwritten under it (the tail wiggled, 10-15) */
    float    sh_rowx[39], sh_yoff;                          /* the upright draw's snapped row shifts and row offset (hysteresis) */
    float    sh_pts[140][3];                                /* x, y, arc length */
    uint8_t  sh_behind[140];
    uint8_t  sn_glass[35 * 35];                             /* the snail on the glass: what its heading alone decides (draw_snail) */
    uint8_t  pk_sprite[PK_CH][PK_CW];                       /* the pumpkin lantern's tones, built once */
    uint16_t pk_row[PK_N][PK_CH * PK_CELL];                 /* its rind tones per row, hazed and dimmed */
    uint8_t  pk_body[PK_CH][PK_CW], pk_hole[PK_CH][PK_CW];  /* the build's grids (internal RAM is spoken for) */
    uint8_t  vo_sprite[VO_CH][VO_CW];                       /* the bubble volcano's tones (0.3.9), built once */
    uint16_t vo_row[VO_N][VO_CH * VO_CELL];                 /* its rock tones per row, hazed and dimmed */
    uint16_t vo_glow[VO_GLOW_MAX];                          /* its lava cells (j * VO_CW + i): redrawn every frame */
    uint8_t  ur_tones[33 * 37];                              /* the urchin's tone grid, rebuilt a few times a second (2026-10-23) */
    uint8_t  cr_tones[CR_CACHE_N][50 * 25], cr_parts[50 * 25];   /* the strawberry crab's (0.3.9): its tone grids, one per pose seen lately (CR_GW x CR_GH), and the
                                                                   build's parts (scratch) */
    uint32_t cr_keys[CR_CACHE_N], cr_used[CR_CACHE_N];      /* ... each grid's pose (cr_hash; 0 = empty) and when it was last drawn (the least recent goes) */
    uint8_t  cr_card_tones[50 * 25], cr_card_parts[50 * 25]; /* its card's portrait, the roost (the tank's grid may hold a profile) */
    uint8_t  sn_tones[19 * 37];                              /* the upright snail's, likewise */
    uint16_t shelf_thumb[3][32 * 32];                       /* the shelf's little tanks (0.3.9): the plant, the coral, the cluster
                                                             * as they are, 32 x 32 (the shop icon's size), box-filtered from a
                                                             * full-size paint (shelf_build); the modal doubles it */
} decor_scratch_t;
static decor_scratch_t *g_ds;
static const uint8_t *g_cr_tn;                    /* the crab's grid as drawn this frame (draw_crab): crab_covers reads it */
static bool g_cr_nocache;                         /* the selftest's switch: a build every pose change, as before the cache */
static int  g_cr_builds;                          /* builds so far (the selftest counts them) */
void render_crab_cache(bool on) { g_cr_nocache = !on; }
int  render_crab_builds(void) { return g_cr_builds; }
static bool  g_pk_built;                          /* the pumpkin's sprite is in the scratch */
static float g_pk_dim = -1;                       /* the dim its rind rows were tinted for */
static int   g_pk_dzk = 99;                       /* and the shift (its depth) */
size_t render_decor_scratch_size(void) { return sizeof(decor_scratch_t); }
static float g_sng_heading;
void   render_set_decor_scratch(void *buf) { g_ds = (decor_scratch_t *)buf; if (g_ds) memset(g_ds, 0, sizeof *g_ds); g_cr_tn = NULL; g_coral_q = -1; g_coral_rgb = 1; g_sng_heading = 1e9f; g_pk_built = false; g_pk_dim = -1; }
static inline decor_scratch_t *ds(void) { if (!g_ds) g_ds = (decor_scratch_t *)calloc(1, sizeof(decor_scratch_t)); return g_ds; }
static uint32_t (*castle_front(void))[224 / 32] { return ds()->castle_front; }
/* the bake of a piece IN FRONT records what it covers (bake_scene sets these) */
static uint32_t *g_rec; static int g_rec_x0, g_rec_y0, g_rec_w, g_rec_rows, g_rec_wpr;
static inline void rec_px(int x, int y) {
    if (!g_rec) return;
    int lx = x - g_rec_x0, r = y - g_rec_y0;
    if ((unsigned)lx < (unsigned)g_rec_w && (unsigned)r < (unsigned)g_rec_rows) g_rec[r * g_rec_wpr + (lx >> 5)] |= 1u << (lx & 31);
}
typedef struct { float x0, y0, x1, y1, r; uint8_t tip; float g0, g1; } coral_seg_t;
/* cell coordinates, x from the left edge, y UP from the base row. g0..g1 is
 * the segment's GROWTH window (tank_coral_growth, 2026-09-23): absent below
 * g0, reaching out along its line until g1 - a child branch's window opens
 * where its parent's closes, so the fan comes in trunk first, low branches,
 * then the crown over the month. */
static const coral_seg_t CORAL_SEGS[] = {
    { 16, 0, 17, 20, 3.0f, 0, 0.00f, 0.30f },   /* the trunk: thick, like the art's */
    { 17, 20, 18, 33, 2.7f, 0, 0.30f, 0.60f },
    { 18, 33, 18, 41, 2.3f, 1, 0.60f, 0.85f },  /* its tip */
    { 16, 7, 6, 15, 2.5f, 0, 0.12f, 0.40f },    /* left, low */
    { 6, 15, 3, 22, 2.2f, 1, 0.40f, 0.62f },
    { 17, 12, 26, 18, 2.4f, 0, 0.20f, 0.48f },  /* right, low */
    { 26, 18, 29, 25, 2.1f, 1, 0.48f, 0.70f },
    { 17, 20, 9, 28, 2.4f, 0, 0.36f, 0.62f },   /* left, mid */
    { 9, 28, 7, 35, 2.1f, 1, 0.62f, 0.82f },
    { 18, 26, 25, 33, 2.3f, 0, 0.45f, 0.72f },  /* right, mid */
    { 25, 33, 27, 40, 2.0f, 1, 0.72f, 0.92f },
    { 18, 31, 12, 39, 2.2f, 1, 0.62f, 1.00f },  /* left, high: the last to finish */
    { 17, 3, 22, 7, 1.9f, 1, 0.05f, 0.20f },    /* a nub */
};
#define CORAL_NSEG ((int)(sizeof CORAL_SEGS / sizeof CORAL_SEGS[0]))
/* signed distance to the skeleton AT growth g, in cells; *tip = within a
 * lit cap (a finished tip's, or the reaching end of a branch still growing) */
#define CORAL_Y_SCALE 0.88f                                  /* the fan tops out ~76 px up (Strato: a modest height, never the grass's) */
#define CORAL_TOP_X   18.0f                                  /* the crown's root: the trunk tip's end, in cells */
#define CORAL_TOP_Y   (41.0f * CORAL_Y_SCALE)
static float coral_sd(float px, float py, float g, bool *tip) {
    float best = 1e9f; *tip = false;
    if (g > 1) g = 1;
    float thin = 0.72f + 0.28f * g;                          /* a young coral is slimmer all over */
    for (int i = 0; i < CORAL_NSEG; i++) {
        const coral_seg_t *s = &CORAL_SEGS[i];
        float f = (g - s->g0) / (s->g1 - s->g0);             /* how far along its line it has reached */
        if (f <= 0) continue;
        if (f > 1) f = 1;
        float sy0 = s->y0 * CORAL_Y_SCALE, sy1 = s->y1 * CORAL_Y_SCALE;
        float x1 = s->x0 + (s->x1 - s->x0) * f, y1 = sy0 + (sy1 - sy0) * f;
        float r = s->r * thin * (0.75f + 0.25f * f);
        float dx = x1 - s->x0, dy = y1 - sy0, len2 = dx * dx + dy * dy;
        float u = len2 > 1e-6f ? ((px - s->x0) * dx + (py - sy0) * dy) / len2 : 0;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        float ex = px - (s->x0 + dx * u), ey = py - (sy0 + dy * u);
        float d = sqrtf(ex * ex + ey * ey) - r;
        if (d < best) best = d;
        if (s->tip || f < 1) {                               /* the lit cap: the last r + 1 cells of a tip */
            float tx = px - x1, ty = py - y1;
            if (tx * tx + ty * ty <= (r + 0.6f) * (r + 0.6f)) *tip = true;
        }
    }
    return best;
}
/* the distance grids are int8 eighths of a cell (the sign and the few
 * thresholds are all the classing needs): internal RAM is tight on the
 * board - the float grids of 2026-09-24 morning cost the display driver its
 * semaphore and the tank booted black */
static inline int8_t sd_q(float d) { d *= 8; return (int8_t)(d < -127 ? -127 : d > 127 ? 127 : d); }
static void coral_build(float g) {
    int8_t  (*sd)[CORAL_CW] = ds()->co_sd;
    uint8_t (*tp)[(CORAL_CW + 7) / 8] = ds()->co_tp;
    g_coral_cells = 0;
    for (int j = 0; j < CORAL_CH; j++)
        for (int i = 0; i < CORAL_CW; i++) {
            bool tip; sd[j][i] = sd_q(coral_sd(i + 0.5f, (CORAL_CH - 1 - j) + 0.5f, g, &tip));
            if (tip) tp[j][i >> 3] |= (uint8_t)(1 << (i & 7)); else tp[j][i >> 3] &= (uint8_t)~(1 << (i & 7));
        }
    for (int j = 0; j < CORAL_CH; j++)
        for (int i = 0; i < CORAL_CW; i++) {
            int d = sd[j][i];
            bool tip = (tp[j][i >> 3] >> (i & 7)) & 1;
            uint8_t t = CO_NONE;
            if (d < 0) {
                /* the light comes from the upper left (the art's): an edge cell
                   with open water above or to its left is LIT, one with open
                   water below or to its right is the dark rim - the outline
                   that keeps the branches apart */
                bool out_l = i == 0 || sd[j][i - 1] >= 0, out_u = j == 0 || sd[j - 1][i] >= 0;
                bool out_r = i == CORAL_CW - 1 || sd[j][i + 1] >= 0, out_d = j == CORAL_CH - 1 || sd[j + 1][i] >= 0;
                bool edge = out_l || out_u || out_r || out_d;
                if (tip) t = (out_r || out_d) && !(out_l || out_u) ? CO_RIM : CO_TIP;
                else if (edge) t = (out_r || out_d) ? CO_RIM : CO_LIT;
                else {
                    uint32_t h = chash(i, j) % 100;
                    t = h < 22 ? CO_SHADE : h < 28 ? CO_LIT : CO_BODY;
                }
            }
            ds()->co_sprite[j][i] = t;
            g_coral_cells += t != CO_NONE;
        }
}
int render_coral_cells(float growth) {                       /* the selftest: how much coral there is at a growth */
    coral_build(growth); g_coral_q = -1;
    return g_coral_cells;
}
static void coral_tint_fill(uint32_t rgb, float dim) {
    if (g_coral_rgb == rgb && g_coral_dim == dim && g_coral_dzk == g_dz_coral) return;
    float haze = decor_haze(0.90f, g_dz_coral);
    uint32_t tone[CO_N];
    tone[CO_NONE]  = 0;
    tone[CO_RIM]   = rgb_mix(rgb, 0x30060e, 0.58f);
    tone[CO_BODY]  = rgb;
    tone[CO_SHADE] = rgb_mix(rgb, 0x000000, 0.24f);
    tone[CO_LIT]   = rgb_mix(rgb, 0xfff0b0, 0.36f);
    tone[CO_TIP]   = rgb_mix(rgb, 0xffe8a0, 0.58f);
    for (int r = 0; r < CORAL_H; r++) {
        int y = CORAL_FY - CORAL_H + r;
        uint32_t w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y);
        for (int k = 1; k < CO_N; k++) ds()->co_row[k][r] = rgb565(rgb_mix(w, tone[k], k == CO_TIP ? haze + 0.04f : haze), dim);   /* a light haze: the art's colour stays saturated */
    }
    g_coral_rgb = rgb; g_coral_dim = dim; g_coral_dzk = g_dz_coral;
}
/* the crown (2026-09-23, Strato: "after it's fully grown it should sprout a
 * ring of delicate tentacles out of the top"): CORAL_TENT one-pixel
 * filaments fanning up and out from the trunk's tip once growth passes 1,
 * reaching CORAL_TENT_L px at CORAL_FULL, each swaying on its own phase with
 * the tank clock and ending in a bright bead. Drawn every frame over the
 * body (dynamic: the caller's rect covers the vignette), so the crown moves
 * while the fan stands still. Returns its bounding box through the pointers. */
#define CORAL_TENT   9
#define CORAL_TENT_L 15.0f
/* a fan of n one-pixel tentacles from (ox, oy), angles a0..a1 (screen
 * radians: -pi/2 is straight up), each `len` px, swaying on the tank clock
 * with its own phase (seed), a bright bead at the end; the bounding box
 * through the pointers (x1 < x0 = nothing drawn). The coral's crown and the
 * cluster's bloom both use it (2026-09-24). */
static void draw_crown(ctx_t *c, float ox, float oy, int n_t, float a0, float a1, float len, uint32_t pale,
                       int seed, float clock, int *bx0, int *by0, int *bx1, int *by1) {
    *bx0 = *by0 = 1 << 20; *bx1 = *by1 = -1;
    if (n_t < 1) return;
    uint32_t bead = rgb_mix(pale, 0xffffff, 0.55f);
    for (int k = 0; k < n_t; k++) {
        float a = n_t == 1 ? (a0 + a1) * 0.5f : a0 + (a1 - a0) * k / (n_t - 1);
        float sway = 0.22f * fast_sin(clock * 1.3f + (k + seed) * 0.9f);
        float lx = ox, ly = oy;
        int n = (int)(len + 0.5f);
        for (int i = 1; i <= n; i++) {
            float s = (float)i / n;
            float ang = a + sway * s + (k - (n_t - 1) * 0.5f) * 0.05f * s;   /* the outer ones curl outward */
            float x = ox + cosf(ang) * len * s, y = oy + sinf(ang) * len * s;
            int ix = (int)(x + 0.5f), iy = (int)(y + 0.5f);
            if (ix == (int)(lx + 0.5f) && iy == (int)(ly + 0.5f)) continue;
            if (g_clip_k >= 0 && pc_covered_above(g_clip_k, ix, iy)) continue;   /* under a piece above its own */
            if (keep_at(ix, iy)) continue;                                      /* a fish in front of the pieces */
            if (bed_over(g_clip_k, ix, iy)) continue;                           /* the sword plant ranked above it */
            px_blend(c, ix, iy, i == n ? bead : pale, i == n ? 255 : 190);
            if (ix < *bx0) *bx0 = ix;
            if (ix > *bx1) *bx1 = ix;
            if (iy < *by0) *by0 = iy;
            if (iy > *by1) *by1 = iy;
            lx = x; ly = y;
        }
    }
    if (*bx1 >= *bx0) { *bx0 -= 1; *by0 -= 1; *bx1 += 1; *by1 += 1; }
}
static void draw_coral_crown(ctx_t *c, int cx, uint32_t rgb, float growth, float clock,
                             int *bx0, int *by0, int *bx1, int *by1) {
    float phase = (growth - 1.0f) / (CORAL_FULL - 1.0f);
    *bx0 = *by0 = 1 << 20; *bx1 = *by1 = -1;
    if (phase <= 0) return;
    if (phase > 1) phase = 1;
    float ox = cx - CORAL_W / 2 + CORAL_TOP_X * CORAL_CELL + 1, oy = CORAL_FY - CORAL_TOP_Y * CORAL_CELL - 1;
    float len = CORAL_TENT_L * (0.35f + 0.65f * phase);
    /* a fan from ~29 deg left of up to 29 deg right, past the horizontal */
    draw_crown(c, ox, oy, CORAL_TENT, -3.14159f * 0.16f, -3.14159f * 0.84f, len, rgb_mix(rgb, 0xffffff, 0.55f), 0, clock, bx0, by0, bx1, by1);
}
/* a low mound of floor stones round a piece's base (2026-09-24): the same
 * pebble tones and position hash as the floor, an ellipse half_w wide whose
 * crest sits 6 px above the base line and whose skirt runs 3 px below it,
 * drawn AFTER the piece so its bottom rows are buried - the coral's trunk
 * and the cluster's rock grow out of the floor instead of standing on it.
 * final: vignetted and untagged here (the scene-cache path). */
static void draw_floor_mound(ctx_t *c, int cx, int half_w, int rise, int dz, bool final) {
    const int fl = TANK_BOT - 14 + dz;                                     /* the floor line: the pebbles' highest top, on the piece's depth (0.3.9) */
    for (int y = fl - rise; y <= fl + 4; y++) {
        if (y < 0 || y >= TANK_H) continue;
        float w = ell_half((y - (fl + 2)) / (float)(rise + 2));           /* an ellipse whose crest is `rise` px above the floor line */
        if (w <= 0) continue;
        int half = (int)(half_w * w);
        for (int x = cx - half; x <= cx + half; x++) {
            if (!CTX_IN(c, x, y)) continue;
            uint32_t h2 = (uint32_t)((x * 73856093u) ^ (y * 19349663u));
            uint32_t tone = (h2 >> 4) % 16;
            uint32_t col = tone < 2 ? 0x2e3b2c : tone < 5 ? 0x22301f : tone < 8 ? 0x1a2418 : 0x101a12;
            float up = (float)(fl - y) / rise;                           /* 0 at the floor line, 1 at the crest */
            if (up > 0) col = rgb_mix(col, 0x56684f, 0.25f + 0.40f * up);   /* the hump rises into the light; its skirt is the floor */
            if (up > 0.15f && (chash(x, y) & 15) == 0) col = 0x6e7f66;   /* a few lit grains */
            uint16_t *p = &CTX_PX(c, x, y);
            *p = FB_SWAP16(rgb565(col, c->dim)); TOUCH_PX(c, x, y);
            rec_px(x, y);
            if (final) {
                PK_LIGHT_PX(p, x, y);
                int a = g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                if (a) px_darken(p, a);
                if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
            }
        }
    }
}
/* the coral at centre x (its base on the floor), in the keeper's colour.
 * final: vignetted here and untagged (the scene-cache path); otherwise the
 * frame's own sweep does it */
static void draw_coral(ctx_t *c, int cx, uint32_t rgb, float growth, bool final) {
    if (growth > 1) growth = 1;
    int q = (int)(growth * (CORAL_Q - 0.01f));
    if (q != g_coral_q) { coral_build(growth); g_coral_q = q; }
    coral_tint_fill(rgb, c->dim);
    int x0 = cx - CORAL_W / 2, y0 = CORAL_FY - CORAL_H;
    for (int j = 0; j < CORAL_CH; j++)
        for (int i = 0; i < CORAL_CW; i++) {
            uint8_t t = ds()->co_sprite[j][i];
            if (!t) continue;
            for (int dy = 0; dy < CORAL_CELL; dy++) {
                int y = y0 + j * CORAL_CELL + dy;
                uint16_t v = ds()->co_row[t][j * CORAL_CELL + dy];
                for (int dx = 0; dx < CORAL_CELL; dx++) {
                    int x = x0 + i * CORAL_CELL + dx;
                    if (!CTX_IN(c, x, y)) continue;
                    uint16_t *p = &CTX_PX(c, x, y);
                    *p = FB_SWAP16(v); TOUCH_PX(c, x, y);
                    rec_px(x, y);
                    if (final) {
                        PK_LIGHT_PX(p, x, y);
                        int a = g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                        if (a) px_darken(p, a);
                        if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
                    }
                }
            }
        }
    draw_floor_mound(c, cx, CORAL_HALF_W - 4, 9, g_dz_coral, final);              /* anchored: the trunk grows out of a hump of stones */
}

/* ---- the pumpkin lantern, procedural (0.3.6, SD_ITEM_PUMPKIN: the first
 * seasonal piece, in the shop in October; Strato's working-assets/
 * pumpkin-lantern, prototyped 2026-10-04) ----
 * A ribbed orange pumpkin with a crooked olive stem and a carved face - two
 * triangle eyes, a nose, a wide shouting mouth with two teeth - built ONCE
 * as a tone sprite on the coral's 2 px cell grid (PK_CW x PK_CH): five
 * overlapping lobes make the ribbed outline and the grooves between them,
 * the light comes from the upper left (the tank's, not the art's upper
 * right), and a mound of floor stones buries its base.
 * The candle is the point of it. Lights ON: the carved face shows a warm
 * lit inside and a little flame. Lights OFF: the face burns at full
 * brightness through the night's dim, the rind is a touch lit from within,
 * and its light falls on everything near it (the night light, above).
 * Per frame: BEHIND, the body is baked into the scene and only the carved
 * face and the flame are redrawn (~700 px of opaque stores) with one
 * flicker level for the frame - the candle; IN FRONT, the whole sprite
 * (~3.6k px) is drawn live over the grass and the fish. The light itself
 * stays still: a flickering light would be ~20k px of read-modify-write a
 * frame against the 25 fps / 4 s decisions budget, so the face does the
 * flickering. */
enum { PK_NONE = 0, PK_RIM, PK_SHADE, PK_BODY, PK_LIT, PK_GLEAM, PK_GROOVE,
       PK_STEM_D, PK_STEM, PK_STEM_L,
       PK_CUT, PK_CUT_HI, PK_IN0, PK_IN1, PK_IN2, PK_IN3 };      /* PK_CUT.. glow: candle-lit */
#define PK_EMIT  PK_CUT
#define PK_W     (PK_CW * PK_CELL)
#define PK_H     (PK_CH * PK_CELL)
#define PK_FY    (TANK_BOT - 14 + DECOR_SINK + g_dz_pk)         /* the base, sunk into the pebbles; on its depth's line (0.3.9) */
#define PK_S     1.2f                                           /* the drawing's units (below) per cell: the art is laid
                                                                 * out on a 36 x 34 grid and scaled up to PK_CW x PK_CH */
#define PK_MID   18.0f                                          /* the body's centre column, in drawing units */
#define PK_FLAME_X 18.0f                                        /* the candle on the floor inside, in drawing units */
#define PK_FLAME_Y 3.6f
static const uint32_t PK_RGB[PK_N] = {
    0, 0x8a2406, 0xc2420e, 0xe8641a, 0xf88a2c, 0xffd8a0, 0xa83410,
    0x3c4214, 0x66702a, 0x9aa24a,
    0xf89a36, 0xffc04a, 0x4a1206, 0x8a2a0a, 0xd8601a, 0xffa83a };
static const uint32_t PK_RGB_NIGHT[PK_N] = {                    /* the glow tones with the light off: the candle has the tank to itself */
    [PK_CUT] = 0xffa83a, [PK_CUT_HI] = 0xffd470,
    [PK_IN0] = 0xa8380c, [PK_IN1] = 0xe86818, [PK_IN2] = 0xffa630, [PK_IN3] = 0xffe6a0 };
static int g_pk_cells, g_pk_glow_cells;                         /* the sprite's filled / glowing cells (the sim's numbers) */
/* the five lobes: centre x offset, radii, in cells (y up from the base) */
static const float PK_LOX[5] = { -10.5f, -5.4f, 0.0f, 5.4f, 10.5f };
static const float PK_LRX[5] = { 6.8f, 7.0f, 7.0f, 7.0f, 6.8f };
static const float PK_LRY[5] = { 9.6f, 11.2f, 11.8f, 11.2f, 9.6f };
static bool pk_in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}
/* a carved hole at cell point (px, py)? */
static bool pk_hole(float px, float py) {
    if (pk_in_tri(px, py, 8.4f, 14.4f, 15.6f, 14.4f, 13.6f, 20.4f)) return true;    /* the eyes, slanted in */
    if (pk_in_tri(px, py, 20.4f, 14.4f, 27.6f, 14.4f, 22.4f, 20.4f)) return true;
    if (pk_in_tri(px, py, 16.2f, 11.8f, 19.8f, 11.8f, 18.0f, 14.4f)) return true;    /* the nose */
    float ex = (px - PK_MID) / 10.6f, ey = (py - 7.0f) / 4.6f;                       /* the mouth, wide open */
    if (ex * ex + ey * ey > 1 || py > 11.0f) return false;
    if (pk_in_tri(px, py, 11.6f, 11.4f, 13.6f, 11.4f, 12.6f, 8.9f)) return false;    /* its two teeth */
    if (pk_in_tri(px, py, 22.4f, 11.4f, 24.4f, 11.4f, 23.4f, 8.9f)) return false;
    return true;
}
/* the stem: a crooked capsule chain from the crown, leaning left at the top */
static float pk_stem_d(float px, float py, float *side) {
    static const float SX[4] = { 18.6f, 19.2f, 17.8f, 15.0f }, SY[4] = { 21.0f, 26.0f, 30.0f, 32.2f }, SR[3] = { 2.5f, 2.1f, 1.7f };
    float best = 1e9f; *side = 0;
    for (int i = 0; i < 3; i++) {
        float dx = SX[i + 1] - SX[i], dy = SY[i + 1] - SY[i], len2 = dx * dx + dy * dy;
        float u = ((px - SX[i]) * dx + (py - SY[i]) * dy) / len2;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        float ex = px - (SX[i] + dx * u), ey = py - (SY[i] + dy * u);
        float d = sqrtf(ex * ex + ey * ey) - SR[i];
        if (d < best) { best = d; float l = sqrtf(len2); *side = (ex * dy - ey * dx) / l / SR[i]; }   /* -1 left .. +1 right of the line */
    }
    return best;
}
static void pk_build(void) {
    uint8_t (*sp)[PK_CW] = ds()->pk_sprite;
    uint8_t (*body)[PK_CW] = ds()->pk_body, (*hole)[PK_CW] = ds()->pk_hole;
    for (int j = 0; j < PK_CH; j++)
        for (int i = 0; i < PK_CW; i++) {
            float px = (i + 0.5f) / PK_S - 0.33f, py = ((PK_CH - 1 - j) + 0.5f) / PK_S;
            float e1 = 9, e2 = 9; int k1 = -1;
            for (int k = 0; k < 5; k++) {
                float ex = (px - PK_MID - PK_LOX[k]) / PK_LRX[k], ey = (py - PK_LRY[k] - 0.2f) / PK_LRY[k];
                float e = ex * ex + ey * ey;
                if (e < e1) { e2 = e1; e1 = e; k1 = k; } else if (e < e2) e2 = e;
            }
            body[j][i] = e1 <= 1.0f ? (uint8_t)(k1 + 1) : 0;
            hole[j][i] = body[j][i] && pk_hole(px, py);
            uint8_t t = PK_NONE;
            if (body[j][i]) {
                int k = k1;
                float nx = (px - PK_MID - PK_LOX[k]) / PK_LRX[k], ny = (py - PK_LRY[k] - 0.2f) / PK_LRY[k];
                float lit = -nx * 0.55f + ny * 0.83f - 0.25f * (px - PK_MID) / 18.0f;   /* the right of the whole pumpkin falls off */
                t = lit > 0.50f ? PK_LIT : lit < -0.40f ? PK_SHADE : PK_BODY;
                bool face = py < 21.4f && py > 1.4f && fabsf(px - PK_MID) < 11.2f;   /* the carving: no rib lines across it */
                if (e2 <= 1.0f && e2 - e1 < 0.15f && !face) t = PK_GROOVE;                /* where two ribs meet */
                if (k == 1 && fabsf(nx + 0.52f) < 0.13f && ny > 0.38f && ny < 0.78f) t = PK_GLEAM;   /* a wet streak on the lit rib */
                if (t != PK_GLEAM && !face && (chash(i + 90, j + 90) & 31) == 0) t = t == PK_LIT ? PK_BODY : t == PK_BODY ? PK_SHADE : t;   /* a mottle */
            }
            float side, sd = pk_stem_d(px, py, &side);
            if (sd < 0 && (!body[j][i] || py > 20.0f))
                t = side > 0.35f ? PK_STEM_D : side < -0.30f || (chash(i, j + 7) & 7) == 0 ? PK_STEM_L : PK_STEM;
            sp[j][i] = t;
        }
    /* the outline, the cuts and the inside, from the neighbours */
    for (int j = 0; j < PK_CH; j++)
        for (int i = 0; i < PK_CW; i++) {
            uint8_t t = sp[j][i];
            if (!body[j][i] || (t >= PK_STEM_D && t <= PK_STEM_L)) continue;
            float px = (i + 0.5f) / PK_S - 0.33f, py = ((PK_CH - 1 - j) + 0.5f) / PK_S;
            if (hole[j][i]) {                                     /* the inside: lit by the candle on its floor */
                float dx = fabsf(px - PK_FLAME_X), dy = (py - PK_FLAME_Y) * 0.62f, d = sqrtf(dx * dx + dy * dy);
                if (py < 11.4f)                                   /* the mouth: a lit floor across it, the back wall darker up high */
                    sp[j][i] = py < 4.8f ? (dx < 4.0f ? PK_IN3 : PK_IN2) : py < 7.2f ? (dx < 6.5f ? PK_IN2 : PK_IN1) : py < 9.4f ? PK_IN1 : PK_IN0;
                else sp[j][i] = d < 10.5f ? PK_IN1 : PK_IN0;      /* the eyes and the nose: the lit dome inside */
                continue;
            }
            bool hu = j > 0 && hole[j - 1][i], hd = j < PK_CH - 1 && hole[j + 1][i];
            bool hl = i > 0 && hole[j][i - 1], hr = i < PK_CW - 1 && hole[j][i + 1];
            if (hu || hd || hl || hr) { sp[j][i] = hu ? PK_CUT_HI : PK_CUT; continue; }   /* the carved lip: the one under a hole catches the flame */
            bool ol = i == 0 || !body[j][i - 1], ou = j == 0 || (!body[j - 1][i] && sp[j - 1][i] == PK_NONE);
            bool orr = i == PK_CW - 1 || !body[j][i + 1], od = j == PK_CH - 1 || !body[j + 1][i];
            if (orr || od) sp[j][i] = PK_RIM;
            else if (ol || ou) sp[j][i] = PK_LIT;
        }
    g_pk_cells = g_pk_glow_cells = 0;
    for (int j = 0; j < PK_CH; j++)
        for (int i = 0; i < PK_CW; i++) { g_pk_cells += sp[j][i] != PK_NONE; g_pk_glow_cells += sp[j][i] >= PK_EMIT; }
    g_pk_built = true;
}
int render_pumpkin_px(int *glow) {                               /* its pixels, and how many of them are redrawn a frame (+ the flame's ~22) */
    if (!g_pk_built) pk_build();
    if (glow) *glow = g_pk_glow_cells * PK_CELL * PK_CELL + 22;
    return g_pk_cells * PK_CELL * PK_CELL;
}
/* the rind's rows: hazed with the water like the coral's, dimmed - and at
 * night dimmed less (the shell is lit a little from within) and warmed */
static void pk_tint_fill(float dim) {
    if (g_pk_dim == dim && g_pk_dzk == g_dz_pk) return;
    bool night = dim < 1.0f;
    float haze = decor_haze(0.90f, g_dz_pk);
    float rd = night ? 0.62f : 1.0f;
    for (int r = 0; r < PK_H; r++) {
        int y = PK_FY - PK_H + r;
        uint32_t w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y);
        for (int k = 1; k < PK_EMIT; k++) {
            uint32_t col = PK_RGB[k];
            if (night && k < PK_STEM_D) col = rgb_mix(col, 0xff6a10, 0.18f);
            ds()->pk_row[k][r] = rgb565(rgb_mix(w, col, haze), k >= PK_STEM_D ? dim : rd);
        }
    }
    g_pk_dim = dim; g_pk_dzk = g_dz_pk;
}
/* the candle this frame: a level 0.78..1.0 from a few incommensurate beats,
 * with now and then a deeper gutter */
static float pk_flicker(float clock) {
    float f = 0.90f + 0.045f * fast_sin(clock * 11.3f + 8 * TAU) + 0.035f * fast_sin(clock * 17.9f + 1.3f + 8 * TAU)
                    + 0.025f * fast_sin(clock * 4.7f + 0.4f + 8 * TAU);
    float g = fast_sin(clock * 0.37f + 8 * TAU);
    if (g > 0.93f) f -= (g - 0.93f) * 1.6f;
    return f;
}
/* the pumpkin BEHIND under a castle IN FRONT (2026-10-17, Strato's 1.8: a
 * fish in the arch showed as a patch of castle over the pumpkin): the castle
 * is over it - the bake paints the castle after it, and every frame the face
 * and the flame leave the castle's pixels alone (the castle's silhouette,
 * g_castle_mask, relative to its centre) */
static inline bool castle_covers(int x, int y) {
    if (g_castle_cx < 0) return false;
    int r = y - (CASTLE_FY - CASTLE_ROWS), lx = x - g_castle_cx + CASTLE_MASK_W / 2;
    return (unsigned)r < CASTLE_ROWS + 12 && (unsigned)lx < CASTLE_MASK_W && ((g_castle_mask[r][lx >> 5] >> (lx & 31)) & 1);
}
/* the castle's openings, in the frame's coordinates: the arch (castle_opening's
 * shape) and the four windows (castle_window's: a half-disc vault over a
 * slot), where the light behind it comes through */
static bool castle_arch_at(int cx, int x, int y) {   /* inside the arch's opening of a castle centred at cx */
    int lx = x - cx, ys = CASTLE_FY - CASTLE_ARCH_S;
    if (y > CASTLE_FY || y < ys - CASTLE_ARCH_R) return false;
    float half = y > ys ? CASTLE_ARCH_R : CASTLE_ARCH_R * ell_half((ys - y) / (float)CASTLE_ARCH_R);
    return half >= 1 && lx >= (int)(-half + 0.5f) && lx <= (int)(half - 0.5f);
}
static bool castle_arch(int x, int y) { return castle_arch_at(g_veg_mask_cx, x, y); }   /* (the castle IN FRONT: the grass's arch passes) */
/* the arch's SHADE (2026-10-17, Strato: "anything seen through the arch
 * should appear a bit darker, shadowed"): the gateway is a tunnel, so what
 * shows in it - the water and the floor behind, the grass, a fish passing,
 * the rubble - is darkened, more under the vault than at the foot. A castle
 * BEHIND: in the bake, right after it (what is painted over it later is not
 * in the tunnel); IN FRONT: every frame after the fish, skipping the pieces
 * stacked over the castle. Lighter at night, and none while the lantern
 * behind the castle lights the tunnel - that glow is the point of it. */
static int arch_shade_alpha(int y, bool night) {
    int top = CASTLE_FY - CASTLE_ARCH_S - CASTLE_ARCH_R;
    float t = (float)(y - top) / (float)(CASTLE_ARCH_S + CASTLE_ARCH_R);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return night ? (int)(48 - 22 * t) : (int)(105 - 50 * t);
}
static bool pc_covered_above(int k, int x, int y);
static void arch_shade(ctx_t *c, int kc, int cx, bool night, bool over_check) {
    if (night && g_pk_under_castle && g_pk_lx >= 0) return;
    for (int y = CASTLE_FY - CASTLE_ARCH_S - CASTLE_ARCH_R; y <= CASTLE_FY; y++) {
        int a = arch_shade_alpha(y, night);
        for (int x = cx - CASTLE_ARCH_R; x <= cx + CASTLE_ARCH_R; x++) {
            if (!CTX_IN(c, x, y) || !castle_arch_at(cx, x, y)) continue;
            if (over_check && pc_covered_above(kc, x, y)) continue;
            px_darken(&CTX_PX(c, x, y), a); TOUCH_PX(c, x, y);
        }
    }
}
/* a castle IN FRONT that is in the scene: the bake shaded the tunnel (pc_draw),
 * so each frame only what was DRAWN in it - a fish, a frond of the early
 * arch pass, a bubble - takes the shade (2026-10-23: the whole opening was
 * re-shaded every frame, 2.9 ms of a square frame) */
static void arch_shade_dirty(ctx_t *c, int kc, int cx, bool night) {
    if (night && g_pk_under_castle && g_pk_lx >= 0) return;
    if (!g_dirty) { arch_shade(c, kc, cx, night, true); return; }
    int x0 = cx - CASTLE_ARCH_R, x1 = cx + CASTLE_ARCH_R;
    int wa = (x0 < 0 ? 0 : x0) >> 5, wb = (x1 > TANK_W - 1 ? TANK_W - 1 : x1) >> 5;
    for (int y = CASTLE_FY - CASTLE_ARCH_S - CASTLE_ARCH_R; y <= CASTLE_FY; y++) {
        if ((unsigned)y >= (unsigned)TANK_H) continue;
        int a = arch_shade_alpha(y, night);
        const uint32_t *drow = g_dirty + y * DIRTY_WORDS_PER_ROW;
        for (int w = wa; w <= wb; w++) {
            uint32_t bits = drow[w];
            while (bits) {
                int x = (w << 5) + __builtin_ctz(bits); bits &= bits - 1;
                if (x < x0 || x > x1 || !CTX_IN(c, x, y) || !castle_arch_at(cx, x, y)) continue;
                if (pc_covered_above(kc, x, y) || keep_at(x, y)) continue;   /* (a fish in front of the castle: no tunnel shade on it) */
                px_darken(&CTX_PX(c, x, y), a);
            }
        }
    }
}
static bool castle_open(int x, int y) {
    int lx = x - g_castle_cx, ys = CASTLE_FY - CASTLE_ARCH_S;
    if (y <= CASTLE_FY && y >= ys - CASTLE_ARCH_R) {                        /* the arch, at the castle's own centre */
        float half = y > ys ? CASTLE_ARCH_R : CASTLE_ARCH_R * ell_half((ys - y) / (float)CASTLE_ARCH_R);
        if (half >= 1 && lx >= (int)(-half + 0.5f) && lx <= (int)(half - 0.5f)) return true;
    }
    static const struct { int lxc, w, top, bot; } W[4] = {                 /* castle_front_row's and the keep's windows (rows up from CASTLE_FY) */
        { 39, 7, 92, 78 }, { -48, 7, 62, 48 }, { -73, 6, 30, 19 }, { 71, 7, 44, 30 } };
    for (int k = 0; k < 4; k++) {
        int top = CASTLE_FY - W[k].top, bot = CASTLE_FY - W[k].bot;
        if (y < top || y > bot) continue;
        float r = W[k].w / 2.0f, dy = (top + r) - y, half = dy > 0 ? r * ell_half(dy / r) : r;
        if (lx >= (int)(W[k].lxc - half + 0.5f) && lx <= (int)(W[k].lxc + half - 0.5f)) return true;
    }
    return false;
}
/* the castle's shadow: its stone takes the spill, its openings the whole
 * light - and below its base line the SAND in front of it is dark (Strato
 * 2026-10-17: "light would never reach the sand below / in front of the
 * castle"), its footprint plus a 3 px step of spill at either edge, but for
 * the pool the gate throws on it: the arch's width, fanning out with the
 * distance and leaning away from the lantern's side (the light comes through
 * the gate at a slant), never more than the arch's own width off centre */
static int pk_shade(int x, int y) {
    if (g_pk_under_castle) {
        int lx = x - g_castle_cx;
        if (y > CASTLE_FY) {
            int alx = lx < 0 ? -lx : lx;
            if (alx <= CASTLE_HALF_W + 3) {
                float d = (float)(y - CASTLE_FY), pool = CASTLE_ARCH_R + d * 0.6f;
                float c = -(float)(g_pk_lx - g_castle_cx) * d / 80.0f;
                if (c > CASTLE_ARCH_R) c = CASTLE_ARCH_R;
                if (c < -CASTLE_ARCH_R) c = -CASTLE_ARCH_R;
                if (fabsf(lx - c) <= pool) return 0;              /* the gate's pool: the castle's word */
                return alx > CASTLE_HALF_W ? 2 : 3;
            }
        } else if (castle_covers(x, y)) return castle_open(x, y) ? 0 : 2;
    }
    if (y <= PK_FLOOR_Y) return 0;
    int lvl = 0;                                                  /* the sand: under what stands in front of the lantern */
    for (int k = 0; k < g_pk_cast_n && lvl < 3; k++) {
        if (x < g_pk_cast[k].x0 - 3 || x > g_pk_cast[k].x1 + 3) continue;
        int l = g_pk_cast[k].level;
        if (l == 3 && (x < g_pk_cast[k].x0 || x > g_pk_cast[k].x1)) l = 2;   /* a solid piece's edge: a step of spill */
        if (l > lvl) lvl = l;
    }
    return lvl;
}
/* the floor casts for this frame: a (clamped) span of sand each, in front of
 * the lantern BEHIND. A bed's is the spill, minus the lantern's own patch. */
static void pk_cast_add(int x0, int x1, int level) {
    if (x1 < x0 || g_pk_cast_n >= PK_CAST_MAX) return;
    g_pk_cast[g_pk_cast_n].x0 = (short)x0; g_pk_cast[g_pk_cast_n].x1 = (short)x1; g_pk_cast[g_pk_cast_n].level = (signed char)level; g_pk_cast_n++;
}
static void pk_casts_pieces(const tank_t *t, int px0, int px1);   /* (with the pieces, below) */
static void pk_casts(const tank_t *t, int pkx) {
    g_pk_cast_n = 0;
    int px0 = pkx - PUMPKIN_HALF_W, px1 = pkx + PUMPKIN_HALF_W;      /* the lantern's own patch of sand: lit, a 5 px half step either side */
    bool grass_above = g_pk_k >= 0 && tank_decor_z(t, 8) == DECOR_Z_BACK;   /* the grass beds stand in front of a lantern BEHIND */
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (b == 3 ? !tank_decor_above(t, 0, 8) : !grass_above) continue;    /* (the sword plant: by its own rank) */
        float x0, x1; tank_veg_bed(t, b, &x0, &x1, NULL, NULL);
        int a = (int)x0, z = (int)x1;
        if (z < px0 - 5 || a > px1 + 5) { pk_cast_add(a, z, 2); continue; }
        if (a < px0 - 5) pk_cast_add(a, px0 - 6, 2);
        pk_cast_add(px0 - 5 > a ? px0 - 5 : a, px0 - 1, 1);
        pk_cast_add(px1 + 1, px1 + 5 < z ? px1 + 5 : z, 1);
        if (z > px1 + 5) pk_cast_add(px1 + 6, z, 2);
    }
    pk_casts_pieces(t, px0, px1);
}
static bool g_baking;                                     /* bake_scene paints the pieces: a piece's mask is this bake's only once it has been painted */
static const tank_t *g_guard;                             /* the critters in the FRONT SLOT (before a piece IN FRONT; the crab in a front volcano's cave, on its
                                                             flank): the per-frame overlays - the volcano's lava and embers, the pumpkin's face and flame - keep off
                                                             them (docs/render-stack.md; the matrix found the pumpkin's face painted over a crab walking past; 0.3.9.1,
                                                             Strato on the 1.8: the lava in the cave over the urchin walking past it - the snail and the urchin too) */
enum { GUARD_CRAB = 1, GUARD_SNAIL = 2, GUARD_URCHIN = 4 };
static unsigned g_guard_who;                              /* ... which of them */
static bool slot_covers(int x, int y);
static inline void pk_store(ctx_t *c, int x, int y, uint16_t v, bool final) {
    if (!CTX_IN(c, x, y) || under_bg(x, y)) return;
    if (g_pk_k >= 0 && !g_baking && pc_covered_above(g_pk_k, x, y)) return;   /* under a piece above it (the castle's stone, a coral IN FRONT, ...) - never
                                                                    in the bake: there the pieces above are painted after it, and their masks are still the LAST bake's
                                                                    (a coral or a cluster a growth step smaller left a hole the water showed through: bake_scene) */
    if (g_guard && slot_covers(x, y)) return;                    /* a critter in the front slot before the lantern: its face stays under it */
    if (bed_over(g_pk_k, x, y)) return;                          /* the sword plant ranked above it: its leaves stay over the face */
    if (keep_at(x, y)) return;                                   /* a fish in front of the pieces (SWIM DEPTH phase 2) */
    uint16_t *p = &CTX_PX(c, x, y);
    rec_px(x, y);
    *p = FB_SWAP16(v); TOUCH_PX(c, x, y);
    if (final) {
        static int ky = -1, kx = -1, ka;       /* one vignette alpha per 8 px of a row, computed (the PSRAM LUT costs a cache line a read) */
        if (y != ky || (x >> 3) != kx) { ky = y; kx = x >> 3; ka = vig_alpha((x & ~7) + 4, y); }
        if (ka) px_darken(p, ka);
        if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
    } else dirty_px(x, y);
}
/* the glowing part: the carved face's cells and the flame. Every frame,
 * over the baked body (and in the bake, at the steady level). night: the
 * candle's own tones, which ignore the night's dim */
static void draw_pumpkin_glow(ctx_t *c, int cx, float clock, bool night, bool steady, bool final) {
    if (!g_pk_built) pk_build();
    float f = steady ? 0.92f : pk_flicker(clock);
    uint16_t v[PK_N];
    for (int k = PK_EMIT; k < PK_N; k++) {
        uint32_t col = night ? PK_RGB_NIGHT[k] : PK_RGB[k];
        float lv = f * (k == PK_IN3 ? 1.04f : 1.0f);
        if (!night && k == PK_CUT) lv = 1.0f;                     /* by day the lip is mostly daylight */
        v[k] = rgb565(col, lv > 1 ? 1 : lv);
    }
    int x0 = cx - PK_W / 2, y0 = PK_FY - PK_H;
    for (int j = PK_CH - (int)(22 * PK_S); j < PK_CH; j++)       /* the face's rows only: the stem and the crown carry none */
        for (int i = (int)(6 * PK_S); i < PK_CW - (int)(6 * PK_S); i++) {
            uint8_t t = ds()->pk_sprite[j][i];
            if (t < PK_EMIT) continue;
            for (int dy = 0; dy < PK_CELL; dy++)
                for (int dx = 0; dx < PK_CELL; dx++) pk_store(c, x0 + i * PK_CELL + dx, y0 + j * PK_CELL + dy, v[t], final);
        }
    /* the flame: a small teardrop on a stub of candle, leaning with the flicker */
    int fx = x0 + (int)((PK_FLAME_X + 0.33f) * PK_S * PK_CELL), fy = PK_FY - (int)(PK_FLAME_Y * PK_S * PK_CELL);
    uint16_t wax = rgb565(night ? 0xf8e2b0 : 0xe8d4a8, f), core = rgb565(0xfffbe8, 1.0f),
             mid = rgb565(0xffd860, f > 0.95f ? 1.0f : f + 0.05f), tip = rgb565(0xff9a28, f);
    for (int y = fy - 1; y <= fy + 2; y++) for (int x = fx - 2; x <= fx + 1; x++) pk_store(c, x, y, wax, final);
    int h = steady ? 6 : 5 + (f > 0.93f) + (f > 0.86f);
    int lean = steady ? 0 : (int)floorf(1.4f * fast_sin(clock * 6.1f + 8 * TAU) + 0.5f);
    for (int k = 1; k <= h; k++) {
        int y = fy - 1 - k, x = fx - 1 + (k > h / 2 ? lean : 0);
        uint16_t a = k <= 2 ? core : k <= h - 2 ? mid : tip;
        pk_store(c, x, y, a, final);
        if (k <= h - 2) pk_store(c, x + 1, y, k <= 2 ? mid : tip, final);
    }
}
/* the light over the water and the floor, in the bake: the pixels already
 * carry the porthole vignette, so the light is darkened by it too, and it
 * is dithered so its falloff never bands. (The decor painted after it lights
 * itself - PK_LIGHT_PX in its final paint.) */
static void pk_halo(uint16_t *sc) {
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    uint8_t rgb[128][3];                           /* the light in 8 bits, for the dither */
    for (int i = 0; i < 128; i++) { int r, g, b; pk_rgb8(i, &r, &g, &b); rgb[i][0] = (uint8_t)r; rgb[i][1] = (uint8_t)g; rgb[i][2] = (uint8_t)b; }
    for (int y = g_pk_ly - PK_HALO_RY + 1; y < g_pk_ly + PK_HALO_RY; y++)
        for (int x = g_pk_lx - PK_HALO_R + 1; x < g_pk_lx + PK_HALO_R; x++) {
            if ((unsigned)x >= (unsigned)TANK_W || (unsigned)y >= (unsigned)TANK_H) continue;
            int i = pk_light_i(x, y);
            if (i < 0) continue;
            int inv = 256 - (g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y));
            if (g_pk_shadows) { int sh = pk_shade(x, y); if (sh == 3) continue; if (sh == 2) inv = (int)(inv * PK_SPILL); else if (sh == 1) inv = (int)(inv * PK_HALF); }   /* in a shadow */
            int lr = rgb[i][0] * inv >> 8, lg = rgb[i][1] * inv >> 8, lb = rgb[i][2] * inv >> 8;
            uint16_t *p = &sc[y * TANK_W + x];
            int d = bayer[y & 3][x & 3];
            uint16_t cur = FB_SWAP16(*p);
            int rr = (((cur >> 11) & 31) << 3) + 4 + lr, gg = (((cur >> 5) & 63) << 2) + 2 + lg, bb = ((cur & 31) << 3) + 4 + lb;
            int r5 = (rr + (d >> 1) - 4) >> 3, g6 = (gg + (d >> 2) - 2) >> 2, b5 = (bb + (d >> 1) - 4) >> 3;
            if (r5 > 31) r5 = 31;
            if (g6 > 63) g6 = 63;
            if (b5 > 31) b5 = 31;
            *p = FB_SWAP16((uint16_t)((r5 << 11) | (g6 << 5) | b5));
        }
}
/* every frame, before the vignette sweep: what was painted live in the
 * light's box and is still marked (fish, critters, food, bubbles, crowns)
 * takes the light; the sweep then vignettes it as before */
static void pk_light_dirty(ctx_t *c) {
    int y0 = g_pk_ly - PK_HALO_RY + 1, y1 = g_pk_ly + PK_HALO_RY - 1, x0 = g_pk_lx - PK_HALO_R + 1, x1 = g_pk_lx + PK_HALO_R - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > TANK_H - 1) y1 = TANK_H - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > TANK_W - 1) x1 = TANK_W - 1;
    for (int y = y0; y <= y1; y++) {
        const uint32_t *drow = g_dirty + y * DIRTY_WORDS_PER_ROW;
        for (int w = x0 >> 5; w <= x1 >> 5; w++) {
            uint32_t bits = drow[w];
            while (bits) {
                int x = (w << 5) + __builtin_ctz(bits); bits &= bits - 1;
                if (x < x0 || x > x1) continue;
                uint32_t l = pk_light_at(x, y);
                if (l) { uint16_t *p = &c->fb[y * c->stride + x]; *p = FB_SWAP16(px_lit(FB_SWAP16(*p), l)); }
            }
        }
    }
}
/* the lantern at centre x: the rind, the face (steady in the bake, the
 * candle's flicker live), and with mound the floor stones round its base
 * (lit by the light while it is on) */
static void draw_pumpkin(ctx_t *c, int cx, bool night, float clock, bool steady, bool mound, bool final) {
    if (!g_pk_built) pk_build();
    pk_tint_fill(c->dim);
    int x0 = cx - PK_W / 2, y0 = PK_FY - PK_H;
    for (int j = 0; j < PK_CH; j++)
        for (int i = 0; i < PK_CW; i++) {
            uint8_t t = ds()->pk_sprite[j][i];
            if (!t || t >= PK_EMIT) continue;
            for (int dy = 0; dy < PK_CELL; dy++) {
                uint16_t v = ds()->pk_row[t][j * PK_CELL + dy];
                for (int dx = 0; dx < PK_CELL; dx++) pk_store(c, x0 + i * PK_CELL + dx, y0 + j * PK_CELL + dy, v, final);
            }
        }
    draw_pumpkin_glow(c, cx, clock, night, steady, final);
    if (mound) draw_floor_mound(c, cx, PK_W / 2 - 4, 5, g_dz_pk, final);
}

/* ---- the bubble volcano, procedural (0.3.9; DonB's #ideas thread of
 * 2026-10-22, four keepers' 🦐: "a bubble volcano (think Finding Nemo) that
 * could maybe have glowing red bubbles visible when tank lights are low";
 * the reference is working-assets/volcano/Volcano Sprite.png: a cone of
 * stacked purple-gray boulders, a lava pool in the crater, orange cracks
 * down the flanks, a cave mouth at the foot lit from inside, moss on the
 * lower rocks, lit bubbles climbing from the crater). Built ONCE as a tone
 * sprite on a 2 px cell grid like the lantern's: the cone's silhouette from
 * a profile with a jagged edge, the boulders from the nearest of a jittered
 * grid of points (a crevice where two are as near), lit from the upper left
 * like every piece, the cracks as capsule chains down from the pool, the
 * moss by the castle's rule. The ROCK is baked, hazed and dimmed with the
 * water per row; the LAVA (the pool, the cracks, the cave's floor) is
 * emissive - its own tones, no dim - and is redrawn every frame with a slow
 * breathing flicker; the BUBBLES rise live from the pool, lit orbs that cool
 * from yellow to red and fade out over ~100 px - by night, the tank's embers.
 * About the castle's width (184 px) and taller (~200 px over the floor). */
#define VO_FY   (TANK_BOT - 16 + g_dz_vo)               /* the sprite's bottom row: the back line (the castle's), shifted to its depth's (0.3.9) */
#define VO_MID  46.0f                                   /* the cone's axis, in cells */
#define VO_BASE 4.0f                                    /* the cone's foot, cell rows up from the sprite's bottom */
#define VO_TOP  100.0f                                  /* the far rim's top */
#define VO_POOL_Y 93.0f                                 /* the lava pool's centre row */
#define VO_POOL_RX 9.5f
#define VO_POOL_RY 3.2f
#define VO_EMIT VO_CAVE_LIT
#define VO_NB   7                                       /* bubbles in the air at once */
#define VO_RISE 104                                     /* how far a bubble climbs before it fades, px */
static const uint32_t VO_RGB[VO_N] = {
    0, 0x74667a, 0x52475a, 0x362e3d, 0x1c1620, 0x5a2e28, 0x7aa33c, 0x527f2e, 0x1e0c0c, 0x48180e,
    0x9a3012, 0xc04012, 0xf26a1c, 0xffa826, 0xffe070 };
static const uint32_t VO_RGB_NIGHT[VO_N] = {                    /* the lava with the light off: brighter, it has the tank to itself */
    [VO_CAVE_LIT] = 0xb8380e, [VO_LAVA_DIM] = 0xd84810, [VO_LAVA] = 0xff7a1e, [VO_LAVA_HOT] = 0xffbc38, [VO_LAVA_CORE] = 0xfff2a8 };
static bool  g_vo_built;
static float g_vo_dim = -1;
static int   g_vo_dzk = 99;
static int   g_vo_cells, g_vo_glow_n;
static int   g_vo_style;                                        /* 0 stacked boulders (Strato's pick 10-23), 1 smooth rock (the sim's PT_VOLCANO_SMOOTH, kept for a look) */
void render_set_volcano_style(int style) { if (style != g_vo_style) { g_vo_style = style; g_vo_built = false; } }
/* smooth value noise on an 8-cell grid, -1..1 (the smooth rock's shading) */
static float vo_noise(float px, float py, int seed) {
    float gx = px / 8.0f, gy = py / 8.0f;
    int i0 = (int)floorf(gx), j0 = (int)floorf(gy);
    float fx = gx - i0, fy = gy - j0;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
#define VO_NV(i, j) (((chash((i) + (seed), (j) - (seed)) & 1023) / 511.5f) - 1.0f)
    return (VO_NV(i0, j0) * (1 - fx) + VO_NV(i0 + 1, j0) * fx) * (1 - fy) + (VO_NV(i0, j0 + 1) * (1 - fx) + VO_NV(i0 + 1, j0 + 1) * fx) * fy;
#undef VO_NV
}
/* the cone's half width at cell row y, per side (s = -1 left, +1 right): a
 * cone that leans a little, a shoulder on the left, lumps and a rough edge */
static float vo_half(float y, int s) {
    float u = (y - VO_BASE) / (VO_TOP - VO_BASE);
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    float h = 11.0f + 35.0f * powf(1 - u, 0.85f);
    float g = s < 0 ? (u - 0.32f) / 0.16f : (u - 0.58f) / 0.12f;
    h += (s < 0 ? 5.0f : 2.5f) * expf(-g * g);
    h += 1.2f * fast_sin(y * 0.47f + (s < 0 ? 0.3f : 2.1f) + 8 * TAU) + 0.8f * fast_sin(y * 0.19f + (s < 0 ? 1.7f : 0.2f) + 8 * TAU);
    h += (float)((int)(chash((int)y >> 2, s + 5) % 3) - 1) * 0.6f;
    return h;
}
/* the boulders: the nearest two of a jittered grid of points (Worley), the
 * nearest's centre and hash for its lighting and its own shade */
#define VO_GX 7.5f
#define VO_GY 6.0f
static void vo_boulder(float px, float py, float *d1, float *d2, float *fx, float *fy, uint32_t *fh) {
    int gi = (int)floorf(px / VO_GX), gj = (int)floorf(py / VO_GY);
    *d1 = *d2 = 1e9f; *fx = *fy = 0; *fh = 0;
    for (int dj = -1; dj <= 1; dj++)
        for (int di = -1; di <= 1; di++) {
            int ci = gi + di, cj = gj + dj;
            uint32_t h = chash(ci + 31, cj + 17);
            float cx = (ci + 0.5f + (((h >> 3) & 255) / 255.0f - 0.5f) * 0.8f) * VO_GX;
            float cy = (cj + 0.5f + (((h >> 11) & 255) / 255.0f - 0.5f) * 0.8f) * VO_GY;
            float ex = px - cx, ey = (py - cy) * 1.2f, d = sqrtf(ex * ex + ey * ey);
            if (d < *d1) { *d2 = *d1; *d1 = d; *fx = cx; *fy = cy; *fh = h; }
            else if (d < *d2) *d2 = d;
        }
}
/* signed distance to a chain of capsules (dx from the axis, y pairs), its
 * width tapering w0 -> w1 along the chain */
static float vo_chain_sd(float px, float py, const int8_t *p, int n, float w0, float w1) {
    float best = 1e9f;
    for (int i = 0; i + 1 < n; i++) {
        float ax = VO_MID + p[2 * i], ay = p[2 * i + 1], bx = VO_MID + p[2 * i + 2], by = p[2 * i + 3];
        float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        float u = ((px - ax) * dx + (py - ay) * dy) / len2;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        float ex = px - (ax + dx * u), ey = py - (ay + dy * u);
        float w = w0 + (w1 - w0) * ((i + u) / (n - 1));
        float d = sqrtf(ex * ex + ey * ey) - w;
        if (d < best) best = d;
    }
    return best;
}
static void vo_build(void) {
    uint8_t (*sp)[VO_CW] = ds()->vo_sprite;
    static const int8_t CA[] = { -5, 90, -8, 80, -6, 70, -10, 58, -8, 47, -11, 36 };    /* the cracks, from the pool's lip down */
    static const int8_t CB[] = { 5, 90, 8, 80, 6, 70, 10, 58, 9, 46, 13, 34 };
    static const int8_t CC[] = { -1, 89, 1, 78, -2, 67, 0, 59 };
    static const int8_t CD[] = { 9, 88, 14, 76, 12, 64, 16, 52 };
    static const struct { int8_t x, y, rx, ry; } BO[6] = {                           /* the boulders at the foot */
        { -40, 6, 9, 5 }, { -29, 4, 7, 4 }, { 38, 6, 10, 5 }, { 27, 3, 6, 4 }, { -47, 3, 6, 3 }, { 47, 3, 6, 3 } };
    for (int j = 0; j < VO_CH; j++)
        for (int i = 0; i < VO_CW; i++) {
            float px = i + 0.5f, py = (VO_CH - 1 - j) + 0.5f;
            uint8_t t = VO_NONE;
            bool rock = false; float nx = 0;
            if (py >= VO_BASE) {                                                   /* the cone */
                int s = px < VO_MID ? -1 : 1;
                float half = vo_half(py, s);
                if (py > 94) half -= (py - 94) * 0.9f;                             /* the far rim narrows behind the pool */
                if (fabsf(px - VO_MID) <= half && py <= VO_TOP + 0.5f) { rock = true; nx = (px - VO_MID) / half; }
            }
            if (!rock && py < 8) {                                                 /* the rubble skirt */
                float w = ell_half((py - 3.0f) / 4.8f);
                if (w > 0 && fabsf(px - VO_MID) <= 49 * w) { rock = true; nx = (px - VO_MID) / 49.0f; }
            }
            if (rock) {
                float u = (py - VO_BASE) / (VO_TOP - VO_BASE);
                if (u < 0) u = 0;
                if (g_vo_style == 0) {                                             /* stacked boulders */
                    float d1, d2, fx, fy; uint32_t fh;
                    vo_boulder(px, py, &d1, &d2, &fx, &fy, &fh);
                    float lx = (px - fx) / 4.0f, ly = (py - fy) / 4.0f;
                    float lit = -0.62f * nx + 0.5f * (-lx * 0.45f + ly * 0.85f) + (((fh >> 19) & 15) / 15.0f - 0.5f) * 0.4f;
                    t = lit > 0.42f ? VO_ROCK_L : lit > -0.18f ? VO_ROCK : VO_ROCK_D;
                    if (d2 - d1 < 0.9f) t = VO_ROCK_DD;                            /* the crevice between two boulders */
                    else if ((chash(i + 300, j + 300) & 15) == 0) t = t == VO_ROCK_L ? VO_ROCK : t == VO_ROCK ? VO_ROCK_D : VO_ROCK_DD;   /* a mottle */
                } else {                                                           /* smooth rock: the cone's shading, soft lumps, faint ledges */
                    float nz = 0.6f * vo_noise(px, py, 3) + 0.3f * vo_noise(px * 2, py * 2, 7);
                    float lit = -0.62f * nx + 0.12f + 0.35f * nz + ((chash(i + 900, j + 900) & 7) / 7.0f - 0.5f) * 0.16f;   /* a soft edge between the tones */
                    t = lit > 0.40f ? VO_ROCK_L : lit > -0.12f ? VO_ROCK : lit > -0.55f ? VO_ROCK_D : VO_ROCK_DD;
                    float st = py * 0.75f + 2.0f * vo_noise(px * 0.7f, py * 0.7f, 11);
                    if (st - floorf(st / 7.0f) * 7.0f < 0.55f) t = t == VO_ROCK_L ? VO_ROCK : t == VO_ROCK ? VO_ROCK_D : VO_ROCK_DD;   /* a ledge's underside */
                }
                if (u < 0.5f && t != VO_ROCK_DD) {                                 /* moss creeps up from the foot */
                    int p = (int)(40 * (1 - u / 0.5f));
                    if ((int)(chash(i >> 1, j >> 1) % 100) < p && (chash(i + 7, j + 3) & 3) != 0)
                        t = (chash(i >> 1, (j >> 1) + 99) & 1) ? VO_MOSS_A : VO_MOSS_B;
                }
            }
            for (int b = 0; b < 6; b++) {                                          /* the boulders over the foot */
                float ex = (px - (VO_MID + BO[b].x)) / BO[b].rx, ey = (py - BO[b].y) / BO[b].ry, e = ex * ex + ey * ey;
                if (e > 1) continue;
                float lit = -ex * 0.5f + ey * 0.8f;
                t = e > 0.72f && (ex > 0.2f || ey < -0.2f) ? VO_ROCK_DD : lit > 0.35f ? VO_ROCK_L : lit > -0.2f ? VO_ROCK : VO_ROCK_D;
                if (ey > 0.3f && (chash(i + 50, j + 50) % 5) == 0) t = VO_MOSS_A;
                rock = true;
            }
            if (rock) {                                                            /* the cave mouth: a vault at the foot, lit from its floor */
                float spring = VO_BASE + 10, R = 13.0f;
                float ch = py <= spring ? R : R * ell_half((py - spring) / R);
                float jag = (chash((int)py, 9) & 1) ? 0.6f : 0;
                if (py >= VO_BASE + 2 && ch >= 0.5f && fabsf(px - VO_MID) <= ch + jag) {   /* its floor at the sand line, not under it */
                    float dy = py - VO_BASE - 2, in = ch - fabsf(px - VO_MID);   /* the floor glows, feathered up and toward the jambs */
                    int g = (int)(chash(i + 11, j + 5) % 100);
                    t = dy < 1.5f && in > 1.5f ? VO_CAVE_LIT : dy < 3.5f && in > 2.5f && g < 55 - (int)(dy * 10) ? VO_CAVE_LIT
                      : dy < 6.5f && g < 80 - (int)(dy * 9) ? VO_CAVE_MID : VO_CAVE;
                }
            }
            if (rock && t < VO_CAVE) {                                             /* the pool and the cracks, over the rock */
                float sd = vo_chain_sd(px, py, CA, 6, 0.8f, 0.45f), s2;
                if ((s2 = vo_chain_sd(px, py, CB, 6, 0.8f, 0.45f)) < sd) sd = s2;
                if ((s2 = vo_chain_sd(px, py, CC, 4, 0.75f, 0.45f)) < sd) sd = s2;
                if ((s2 = vo_chain_sd(px, py, CD, 4, 0.75f, 0.45f)) < sd) sd = s2;
                float ex = (px - VO_MID) / VO_POOL_RX, ey = (py - VO_POOL_Y) / VO_POOL_RY, e = ex * ex + ey * ey;
                if (e <= 1) t = ey > 0.3f ? VO_LAVA : e < 0.28f ? VO_LAVA_CORE : e < 0.7f || ey < -0.45f ? VO_LAVA_HOT : VO_LAVA;   /* the far side darker, the near lip bright */
                else if (sd < 0) t = py > 82 ? VO_LAVA_HOT : py > 58 ? VO_LAVA : VO_LAVA_DIM;
                else if (sd < 1.0f || e < 1.45f) t = VO_SCORCH;                    /* the rock at a crack's edge, scorched */
            }
            sp[j][i] = t;
        }
    /* the lip over the cave catches the light; the counts and the lava's cell list */
    g_vo_cells = g_vo_glow_n = 0;
    for (int j = 0; j < VO_CH; j++)
        for (int i = 0; i < VO_CW; i++) {
            uint8_t t = sp[j][i];
            if (t >= VO_ROCK_L && t <= VO_ROCK_D && j + 1 < VO_CH && sp[j + 1][i] >= VO_CAVE && sp[j + 1][i] <= VO_CAVE_LIT) sp[j][i] = t = VO_ROCK_L;
            g_vo_cells += t != VO_NONE;
            if (t >= VO_EMIT && g_vo_glow_n < VO_GLOW_MAX) ds()->vo_glow[g_vo_glow_n++] = (uint16_t)(j * VO_CW + i);
        }
    g_vo_built = true;
}
int render_volcano_px(int *glow) {                               /* its pixels, and how many are redrawn a frame (the lava; the bubbles apart) */
    if (!g_vo_built) vo_build();
    if (glow) *glow = g_vo_glow_n * VO_CELL * VO_CELL;
    return g_vo_cells * VO_CELL * VO_CELL;
}
/* the rock's rows: hazed with the water like the castle's stone and dimmed;
 * by night the rows near the crater take a warm cast from the lava */
static void vo_tint_fill(float dim) {
    if (g_vo_dim == dim && g_vo_dzk == g_dz_vo) return;
    bool night = dim < 1.0f;
    float haze = decor_haze(0.88f, g_dz_vo);
    for (int r = 0; r < VO_H; r++) {
        int y = VO_FY - VO_H + r;
        uint32_t w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y);
        float warm = night ? 0.22f * (1 - r / 40.0f) : 0;
        for (int k = 1; k < VO_EMIT; k++) {
            uint32_t col = VO_RGB[k];
            if (warm > 0) col = rgb_mix(col, 0xff5a10, warm);
            ds()->vo_row[k][r] = rgb565(rgb_mix(w, col, haze), dim);
        }
    }
    g_vo_dim = dim; g_vo_dzk = g_dz_vo;
}
static inline void vo_store(ctx_t *c, int x, int y, uint16_t v, bool final) {
    if (!CTX_IN(c, x, y) || under_bg(x, y)) return;
    if (g_clip_k >= 0 && pc_covered_above(g_clip_k, x, y)) return;   /* under a piece above it */
    if (g_guard && slot_covers(x, y)) return;                        /* the crab in its cave, a critter passing it: the embers stay under them (Strato, the pendant; the 1.8) */
    if (bed_over(g_clip_k, x, y)) return;                            /* the sword plant ranked above it */
    if (keep_at(x, y)) return;                                       /* a fish in front of the pieces */
    rec_px(x, y);
    uint16_t *p = &CTX_PX(c, x, y);
    *p = FB_SWAP16(v); TOUCH_PX(c, x, y);
    if (final) {
        PK_LIGHT_PX(p, x, y);
        int a = g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
        if (a) px_darken(p, a);
        if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
    } else dirty_px(x, y);
}
/* the lava breathes: a level 0.84..1.0 from two slow beats and a faster shimmer */
static float vo_flicker(float clock) {
    return 0.93f + 0.04f * fast_sin(clock * 2.7f + 8 * TAU) + 0.025f * fast_sin(clock * 0.9f + 1.1f + 8 * TAU) + 0.015f * fast_sin(clock * 9.3f + 0.4f + 8 * TAU);
}
/* the lava this frame: the pool, the cracks and the cave's floor, over the
 * baked rock (and in the bake, at the steady level) */
static void draw_volcano_glow(ctx_t *c, int cx, float clock, bool night, bool steady, bool final) {
    if (!g_vo_built) vo_build();
    float f = steady ? 0.95f : vo_flicker(clock);
    uint16_t v[VO_N];
    for (int k = VO_EMIT; k < VO_N; k++) {
        float lv = f * (k == VO_LAVA_CORE ? 1.04f : k == VO_CAVE_LIT ? 0.92f : 1.0f);
        v[k] = rgb565(night ? VO_RGB_NIGHT[k] : VO_RGB[k], lv > 1 ? 1 : lv);
    }
    int x0 = cx - VO_W / 2, y0 = VO_FY - VO_H;
    const uint16_t *g = ds()->vo_glow;
    for (int n = 0; n < g_vo_glow_n; n++) {
        int j = g[n] / VO_CW, i = g[n] % VO_CW;
        uint8_t t = ds()->vo_sprite[j][i];
        for (int dy = 0; dy < VO_CELL; dy++)
            for (int dx = 0; dx < VO_CELL; dx++) vo_store(c, x0 + i * VO_CELL + dx, y0 + j * VO_CELL + dy, v[t], final);
    }
}
/* the volcano at centre x: the rock, then the lava at its steady level */
static void draw_volcano(ctx_t *c, int cx, bool night, bool final) {
    if (!g_vo_built) vo_build();
    vo_tint_fill(c->dim);
    int x0 = cx - VO_W / 2, y0 = VO_FY - VO_H;
    for (int j = 0; j < VO_CH; j++)
        for (int i = 0; i < VO_CW; i++) {
            uint8_t t = ds()->vo_sprite[j][i];
            if (!t || t >= VO_EMIT) continue;
            for (int dy = 0; dy < VO_CELL; dy++) {
                uint16_t v = ds()->vo_row[t][j * VO_CELL + dy];
                for (int dx = 0; dx < VO_CELL; dx++) vo_store(c, x0 + i * VO_CELL + dx, y0 + j * VO_CELL + dy, v, final);
            }
        }
    draw_volcano_glow(c, cx, 0, night, true, final);
}
/* the bubbles: VO_NB in the air, each on its own period and phase, rising
 * from the pool, wobbling, swelling as they climb, cooling from yellow to a
 * dull red and shrinking away at the top. By night their own light (no dim)
 * with a soft ring. Returns their box for the vignette sweep. */
static void draw_volcano_bubbles(ctx_t *c, int cx, double clock, bool night, int *bx0, int *by0, int *bx1, int *by1) {
    ctx_t ce = *c;
    if (night) ce.dim = 1.0f;
    float py = VO_FY - VO_H + (VO_CH - VO_POOL_Y) * VO_CELL;
    const float an = tank_anim(clock);
    for (int k = 0; k < VO_NB; k++) {
        float P = 2.9f + 0.41f * k, ph = (float)fmod(clock / P + k * 0.618f, 1.0);   /* (each on its own period: from the double) */
        float y = py - ph * VO_RISE;
        float x = cx + (k % 3 - 1) * 5.0f * ph + 3.5f * ph * fast_sin(an * 1.3f + k * 2.1f + 8 * TAU);
        float r = 1.7f + 1.5f * ph;
        if (ph > 0.84f) r *= (1 - ph) / 0.16f;
        if (r < 0.6f) continue;
        uint32_t col = ph < 0.35f ? rgb_mix(0xffd060, 0xf8862a, ph / 0.35f) : ph < 0.7f ? rgb_mix(0xf8862a, 0xe04818, (ph - 0.35f) / 0.35f) : rgb_mix(0xe04818, 0xa02810, (ph - 0.7f) / 0.3f);
        if (night) fill_ellipse(&ce, x, y, r + 1.2f, r + 1.2f, col, 50);
        fill_ellipse(&ce, x, y, r, r, col, night ? 255 : 230);
        px_blend(&ce, (int)(x - r * 0.4f), (int)(y - r * 0.4f), 0xfff2c0, 150);
    }
    *bx0 = cx - 18; *bx1 = cx + 18; *by0 = (int)py - VO_RISE - 6; *by1 = (int)py + 6;
}
/* the light it throws with the tank's light off, in the bake: the crater's
 * on the water above and round it, the cave mouth's on the sand before it,
 * dithered like the lantern's halo (the fish passing through it are not lit:
 * the preview) */
static void vo_halo_src(uint16_t *sc, float lx, float ly, float rx, float ry_up, float ry_dn, int lr, int lg, int lb) {
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    int y0 = (int)(ly - ry_up), y1 = (int)(ly + ry_dn), x0 = (int)(lx - rx), x1 = (int)(lx + rx);
    for (int y = y0; y <= y1; y++) {
        if ((unsigned)y >= (unsigned)TANK_H) continue;
        float dy = (y - ly) / (y < ly ? ry_up : ry_dn);
        for (int x = x0; x <= x1; x++) {
            if ((unsigned)x >= (unsigned)TANK_W) continue;
            float dx = (x - lx) / rx, d = sqrtf(dx * dx + dy * dy);
            if (d >= 1) continue;
            float f = (1 - d) * (1 - d) * sqrtf(1 - d);
            int inv = 256 - (g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y));
            int ar = (int)(lr * f) * inv >> 8, ag = (int)(lg * f) * inv >> 8, ab = (int)(lb * f) * inv >> 8;
            uint16_t *p = &sc[y * TANK_W + x];
            int dd = bayer[y & 3][x & 3];
            uint16_t cur = FB_SWAP16(*p);
            int rr = (((cur >> 11) & 31) << 3) + 4 + ar, gg = (((cur >> 5) & 63) << 2) + 2 + ag, bb = ((cur & 31) << 3) + 4 + ab;
            int r5 = (rr + (dd >> 1) - 4) >> 3, g6 = (gg + (dd >> 2) - 2) >> 2, b5 = (bb + (dd >> 1) - 4) >> 3;
            if (r5 > 31) r5 = 31;
            if (g6 > 63) g6 = 63;
            if (b5 > 31) b5 = 31;
            *p = FB_SWAP16((uint16_t)((r5 << 11) | (g6 << 5) | b5));
        }
    }
}
static void vo_halo(uint16_t *sc, int cx) {
    float py = VO_FY - VO_H + (VO_CH - VO_POOL_Y) * VO_CELL, cy = VO_FY - VO_H + (VO_CH - VO_BASE - 3) * VO_CELL;
    vo_halo_src(sc, cx, py, 130, 120, 70, 92, 34, 6);
    vo_halo_src(sc, cx, cy, 70, 30, 24, 76, 22, 4);
}

/* ---- the snail, procedural (2026-09-16; Strato: "explore doing the same
 * for the snail so the aesthetic matches") ----
 * Drawn the castle's way in place of the two sprites: a little geometry with
 * a per-pixel tone rule, the castle's position hash for the foot's mottle,
 * golden light from the upper left (the lower right of the shell falls into
 * the dark tones, the rim shades, a wet gleam sits high on the left), mixed
 * with the water of its row like the fronds - a touch stronger on the pane
 * than on the floor, so the glass snail reads closer - and no outline. And
 * it moves (Strato: "the fish animations are so nice, maybe you can add a
 * subtle animation"): PEDAL WAVES run along the foot, head-ward on the glass
 * and back along the belly upright, the eye stalks sway against each other,
 * and the shell heaves a pixel with the wave.
 * UPRIGHT faces +x, the origin at the sole under the shell (snail_y +
 * SNAIL_SOLE_DY), mirrored to the heading; ~34 x 17 px. ON THE GLASS the
 * head points +y from the foot's centre, turned to the heading like the
 * sprite was; ~22 x 25 px. The tone rule runs over ~750 px per pose. */
enum { SN_S0, SN_S1, SN_S2, SN_S3, SN_S4, SN_F0, SN_F1, SN_F2, SN_F3, SN_TIP, SN_EYE, SN_NONE };
static const uint32_t SNAIL_RGB[SN_NONE] = {
    0xf8c868, 0xe6953c, 0xc4641e, 0x8c3d16, 0x4a2010,      /* the shell: lit .. the groove */
    0xf2ebe4, 0xd8cec6, 0xb3a7a6, 0x847a80,                /* the foot: lit .. its edge */
    0x1a1c34, 0x0a0c18 };                                  /* stalk tips; the eye and the mouth */
#define SNAIL_SOLE_DY   6.0f                               /* the sole below snail_y (the sprite's centre) */
#define SNAIL_TINT_FLOOR 0.84f
#define SNAIL_TINT_GLASS 0.90f
/* within w of the segment (x0,y0)-(x1,y1); *u = 0..1 along it */
static inline bool sn_seg(float lx, float ly, float x0, float y0, float x1, float y1, float w, float *u) {
    float vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy;
    float t = ((lx - x0) * vx + (ly - y0) * vy) / l2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = lx - (x0 + t * vx), ey = ly - (y0 + t * vy);
    *u = t;
    return ex * ex + ey * ey < w * w;
}
/* the shell's tone from its lighting terms: band = the whorl's lit / mid /
 * groove, lit = -1..1 (upper left .. lower right), rim = on the edge */
static inline int sn_shell_tone(int band, float lit, bool rim) {
    int tone = band;
    if (lit > 0.35f) tone--; else if (lit < -0.15f) tone++;
    if (lit < -0.55f) tone++;
    if (rim) tone = lit < 0.25f ? (tone > 3 ? tone : 3) : (tone < 1 ? tone : 1);
    return tone;
}
/* UPRIGHT: the tone at local (lx, ly), or SN_NONE. top = the foot's ridge at
 * this column (computed once per column by the caller) */
typedef struct { float heave, sw1, sw2; } sn_sway_t;       /* this frame's heave and stalk sways: once, not per pixel (2026-10-03) */
static int snail_upright_tone(int lx, int ly, float top, float clock, const sn_sway_t *sw) {
    float heave = sw->heave;
    float sw1 = sw->sw1, sw2 = sw->sw2;
    float u;
    /* the eye stalks, swaying against each other */
    if (sn_seg(lx, ly, 12.5f, -7.5f, 17.5f + sw1, -13.5f, 0.7f, &u)) return u > 0.78f ? SN_TIP : SN_F1;
    if (sn_seg(lx, ly, 13.5f, -6.5f, 18.5f + sw2, -9.5f, 0.7f, &u)) return u > 0.78f ? SN_TIP : SN_F1;
    /* the shell: a disc, the whorl a spiral about an apex left of centre */
    const float cx = -3.5f, cy = -9.5f + heave, R = 8.6f;
    float dx = lx - cx, dy = ly - cy;
    float r = sqrtf(dx * dx + dy * dy * 1.0816f);
    if (r <= R) {
        float ax = lx - (cx - 1.5f), ay = ly - (cy + 0.5f);
        float ar = sqrtf(ax * ax + ay * ay), th = atan2f(ay, ax);
        const float pitch = 2.25f;                                 /* ~4 turns: Strato asked for one more groove */
        float w = fmodf(ar - th / TAU * pitch, pitch); if (w < 0) w += pitch; w /= pitch;
        int tone = sn_shell_tone(w < 0.5f ? 1 : w < 0.74f ? 2 : 3, (-dx * 0.55f - dy * 0.83f) / R, r > R - 1.3f);
        if (ar < 2.2f) tone = 3;                                   /* the apex whorl is tight and dark */
        { float gx = (dx + 3.4f) * 0.8f, gy = dy + 4.6f; if (gx * gx + gy * gy < 4.0f) tone = 0; }   /* a wet gleam */
        return SN_S0 + clampi(tone, 0, 4);
    }
    /* the foot: a low lens under the shell, the head rising at the front */
    if (lx >= -15 && lx <= 13 && ly >= top && ly <= 0) {
        int tone = 1;
        if (ly < top + 1.4f) tone = 0;                             /* the lit ridge */
        if (ly > -0.9f) tone = 2;                                  /* the belly line */
        if (r < R + 1.8f && dy > 0 && tone < 3) tone++;            /* the shell's shadow */
        if (ly > -2.2f && tone <= 1 && fast_sin(lx * 1.1f + clock * 3.0f + TAU) > 0.7f) tone = 0;   /* a pedal wave runs back */
        if ((chash(lx, ly) & 7) == 0) tone = clampi(tone + 1, 0, 3);
        return SN_F0 + tone;
    }
    /* the head */
    { float hx = (lx - 11.5f) * 1.05f, hy = ly + 5.2f;
      if (hx * hx + hy * hy <= 9.0f) {
          float ex = lx - 12.6f, ey = ly + 5.6f;
          if (ex * ex + ey * ey < 0.64f) return SN_EYE;
          return (-(lx - 11.5f) * 0.5f - hy) > 0.9f ? SN_F0 : SN_F1;
      } }
    return SN_NONE;
}
/* ON THE GLASS: local (lx, ly) is turned to the heading; (sx, sy) is the
 * screen offset, which the light and the shadow side follow (the light
 * stays upper-left however it crawls) */
/* (the body under the stalks: the foot, whose pedal waves move with the
 * clock - *foot says so - or the shell, which depends on the place alone) */
static int snail_glass_body(float lx, float ly, float sx, float sy, float clock, bool *foot) {
    *foot = false;
    float lit = (-sx * 0.55f - sy * 0.83f) / 11.0f;
    /* the foot: an egg, broad under the shell, narrowing to the head */
    float ey = ly / 11.0f;
    if (ey >= -1 && ey <= 1) {
        float rx = 6.6f * sqrtf(1 - ey * ey) * (1 - 0.18f * ey);
        float alx = lx < 0 ? -lx : lx;
        if (rx > 0.3f && alx <= rx) {
            *foot = true;
            int tone = 1;
            if (fast_sin(ly * 0.75f - clock * 2.6f + 64 * TAU) > 0.6f) tone = 0;   /* pedal waves crawl head-ward */
            float edge = alx / rx;
            if (edge > 0.8f) tone = 2;                             /* the foot's edge */
            else if (edge > 0.62f && lit < 0 && tone < 1) tone = 1; /* the shadow side falls off */
            if (alx < 1.2f && ly > 2 && tone < 2) tone = 2;        /* the pedal groove */
            if (ly < -5 && edge < 0.5f && tone > 1) tone = 1;
            if ((chash((int)floorf(lx), (int)floorf(ly)) & 7) == 0) tone = clampi(tone + 1, 0, 3);
            if (alx < 1.5f && ly >= 7.4f && ly <= 8.8f) return SN_EYE;   /* the mouth */
            return SN_F0 + tone;
        }
    }
    /* the shell behind: a crescent of whorl lobes round the top and sides */
    float qx = lx / 11.0f, qy = (ly + 2.5f) / 10.6f;
    float sr = sqrtf(qx * qx + qy * qy);
    if (sr <= 1 && ly < 6.5f) {
        float lobe = fast_sin(atan2f(ly + 2.5f, lx) * 5.0f - 0.3f + 8 * TAU);
        int tone = sn_shell_tone(lobe > 0.15f ? 1 : 2, lit, sr > 0.88f);
        if (sr < 0.78f) tone++;                                    /* shadow where the foot meets it */
        { float gx = sx + 5.0f, gy = sy + 7.8f; if (gx * gx + gy * gy < 2.9f) tone = 0; }   /* gleam */
        return SN_S0 + clampi(tone, 0, 4);
    }
    return SN_NONE;
}
/* stalk_y[0] / [1]: the left and right stalk's tip height this frame */
static int snail_glass_tone(float lx, float ly, float sx, float sy, float clock, const float *stalk_y) {
    float u;
    for (int sgn = -1; sgn <= 1; sgn += 2)                        /* the stalks: stubs by the head */
        if (sn_seg(lx, ly, sgn * 2.2f, 10.0f, sgn * 3.6f, stalk_y[sgn > 0], 0.6f, &u))
            return u > 0.7f ? SN_TIP : SN_F2;
    bool foot;
    return snail_glass_body(lx, ly, sx, sy, clock, &foot);
}
/* a row of snail pixels: the water of the row and each tone's colour over it
 * are worked out once per row and tone, not per pixel */
typedef struct { int y; uint32_t w; uint16_t have; src_t s[SN_NONE]; } sn_row_t;
static inline void snail_put(ctx_t *c, int x, int y, int tone, float tint, sn_row_t *r) {
    if (tone == SN_NONE || !CTX_IN(c, x, y)) return;
    if (r->y != y) { r->y = y; r->have = 0; r->w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y); }
    if (!(r->have >> tone & 1)) { r->s[tone] = src_color(rgb_mix(r->w, SNAIL_RGB[tone], tint), c->dim); r->have |= (uint16_t)(1u << tone); }
    px_blend_s(c, x, y, &r->s[tone], 255);
}
/* ON THE GLASS the shell and the empty corners depend only on the heading:
 * kept per pixel of the 35 x 35 box until the snail turns (the decor
 * scratch, PSRAM). SNG_DYN = the foot, or within reach of a stalk: asked
 * every frame. 2026-10-03: on the glass it cost the 1.8 ~4.5 ms a frame - a
 * sqrtf and an atan2f for every shell pixel, two divides and two sines for
 * every pixel of the box. */
#define SNG_N    35
#define SNG_NONE 0xff
#define SNG_DYN  0xfe
static float g_sng_heading = 1e9f;                     /* the heading sn_glass was worked out for */
/* UPRIGHT on the floor - a side view, mirrored to face the way it walks,
 * drawn in the scene with the fish (vignetted, behind the front fronds) -
 * or flat ON THE GLASS, its underside to the viewer, drawn after the algae
 * like the film itself (it is on the pane). */
static void draw_snail(ctx_t *c, const tank_t *t, bool upright_pass) {
    if (g_no_animals) return;
    if (!(t->sd_unlocks & SD_ITEM_SNAIL) || t->snail_x < 0) return;
    bool upright = tank_snail_upright(t);
    if (upright != upright_pass) return;
    int ox = (int)t->snail_x, oy = (int)(t->snail_y + SNAIL_SOLE_DY);
    sn_row_t row = { .y = -(1 << 20) };
    if (upright) {
        bool flip = cosf(t->snail_heading) < 0;
        const float a = anim(t);
        sn_sway_t sw = { 0.6f * fast_sin(a * 2.4f), fast_sin(a * 1.7f), fast_sin(a * 1.7f + 2.2f) };
        float top[37];                                             /* the foot's ridge per column, then row by row (one water colour a row) */
        for (int lx = -16; lx <= 20; lx++) {
            float xf = (lx + 15) / 28.5f;
            top[lx + 16] = -(2.4f + 3.2f * powf(sinf(xf < 0 ? 0 : xf > 1 ? 1 : xf * 3.14159f), 0.6f));
            if (lx > 8) { float h = -(4.8f + (lx - 8) * 0.55f); if (h < top[lx + 16]) top[lx + 16] = h; }
        }
        static double s_t = -1e9;                                  /* its tones a few times a second, not a frame (2026-10-23) */
        uint8_t *tn = ds()->sn_tones;
        if (t->clock - s_t >= 0.08f || t->clock < s_t) {
            for (int ly = -18; ly <= 0; ly++)
                for (int lx = -16; lx <= 20; lx++) tn[(ly + 18) * 37 + lx + 16] = (uint8_t)snail_upright_tone(lx, ly, top[lx + 16], a, &sw);
            s_t = t->clock;
        }
        for (int ly = -18; ly <= 0; ly++)
            for (int lx = -16; lx <= 20; lx++)
                snail_put(c, flip ? ox - lx : ox + lx, oy + ly, tn[(ly + 18) * 37 + lx + 16], SNAIL_TINT_FLOOR, &row);
    } else {
        float a = t->snail_heading - 1.5708f;                      /* the head points +y locally */
        float ca = cosf(a), sa = sinf(a);
        int cx = (int)t->snail_x, cy = (int)t->snail_y;
        const float an = anim(t);
        float stalk_y[2] = { 11.6f + 0.4f * fast_sin(an * 2 + -1 + TAU), 11.6f + 0.4f * fast_sin(an * 2 + 1 + TAU) };
        uint8_t *cls = ds()->sn_glass;
        if (g_sng_heading != t->snail_heading) {                   /* it turned: what the heading alone decides, again */
            g_sng_heading = t->snail_heading;
            for (int dy = -17; dy <= 17; dy++)
                for (int dx = -17; dx <= 17; dx++) {
                    float lx = ca * dx + sa * dy, ly = -sa * dx + ca * dy;
                    float alx = lx < 0 ? -lx : lx;
                    bool foot = false; int tone = SNG_DYN;
                    if (!(alx >= 1.5f && alx <= 4.3f && ly >= 9.3f && ly <= 12.7f))   /* out of both stalks' reach, wherever they sway */
                        tone = snail_glass_body(lx, ly, (float)dx, (float)dy, 0, &foot);
                    cls[(dy + 17) * SNG_N + dx + 17] = foot ? SNG_DYN : tone == SN_NONE ? SNG_NONE : (uint8_t)tone;
                }
        }
        for (int dy = -17; dy <= 17; dy++)
            for (int dx = -17; dx <= 17; dx++) {
                int k = cls[(dy + 17) * SNG_N + dx + 17];
                if (k == SNG_NONE) continue;
                if (k == SNG_DYN) {
                    float lx = ca * dx + sa * dy, ly = -sa * dx + ca * dy;   /* inverse rotation */
                    k = snail_glass_tone(lx, ly, (float)dx, (float)dy, an, stalk_y);
                }
                snail_put(c, cx + dx, cy + dy, k, SNAIL_TINT_GLASS, &row);
            }
    }
}
/* does the snail on the floor (upright), as draw_snail painted it this frame (ds()->sn_tones), cover (x, y)? */
static bool snail_covers(const tank_t *t, int x, int y) {
    if (!(t->sd_unlocks & SD_ITEM_SNAIL) || t->snail_x < 0 || !tank_snail_upright(t)) return false;
    const int ox = (int)t->snail_x, oy = (int)(t->snail_y + SNAIL_SOLE_DY);
    const int lx = cosf(t->snail_heading) < 0 ? ox - x : x - ox, ly = y - oy;
    if (lx < -16 || lx > 20 || ly < -18 || ly > 0) return false;
    return ds()->sn_tones[(ly + 18) * 37 + lx + 16] != SN_NONE;
}
/* ---- the urchin, procedural (2026-10-02, SD_ITEM_URCHIN) ----
 * After Strato's sea-urchin-v1 (working-assets): a domed purple urchin seen
 * a little from above, a crown of short pointed spines with pale tips, warm
 * light from the upper left, no outline. Drawn the snail's way - a tone rule
 * per pixel over a ~37 x 24 box, mixed with the water of its row - in his six
 * tones. The spines are its life: they wave slowly, each on its own beat,
 * and work faster while it chews. The dome's centre is (urchin_x,
 * URCHIN_FLOOR_Y); its flat base sits on the root line, UR_BASE below. */
enum { UR_T0, UR_T1, UR_T2, UR_T3, UR_T4, UR_T5, UR_NONE };
static const uint32_t URCHIN_RGB[UR_NONE] = { 0x320746, 0x66107d, 0x9732b1, 0xbe53bc, 0xf88ab8, 0xfff0d3 };
#define UR_SPINES 11
#define UR_FACE    5
#define UR_RX     10.0f
#define UR_RY     8.0f
#define UR_BASE   7.0f
#define UR_TINT   0.93f
typedef struct { float dx[UR_SPINES + UR_FACE], dy[UR_SPINES + UR_FACE], len[UR_SPINES + UR_FACE];
                 float rim[UR_SPINES], end[UR_SPINES], den[UR_SPINES]; } ur_pose_t;   /* per crown spine: where it leaves the dome, its tip, the wedge's taper -
                                                                                        once a frame here, not per pixel (2026-10-03: a sqrtf and two
                                                                                        divides per spine per pixel cost the 1.8 milliseconds a frame) */
/* the spines this frame: a fan from just under the left side, over the top,
 * to just under the right (the floor hides the rest), long and short in
 * turn; then a few short ones on the dome's face, pointing at the viewer
 * and down - the pale streaks of the art */
static void urchin_pose(ur_pose_t *p, float clock, bool chewing) {
    float amp = chewing ? 0.13f : 0.07f, sp = chewing ? 2.6f : 0.8f;
    for (int k = 0; k < UR_SPINES; k++) {
        float a = -3.14159f - 0.32f + k * (3.14159f + 0.64f) / (UR_SPINES - 1);
        a += amp * fast_sin(clock * sp + k * 1.7f + 8 * TAU);
        p->dx[k] = cosf(a); p->dy[k] = sinf(a);
        p->len[k] = (k & 1) ? 4.0f : 6.0f;
        p->rim[k] = 1.0f / sqrtf(p->dx[k] * p->dx[k] / (UR_RX * UR_RX) + p->dy[k] * p->dy[k] / (UR_RY * UR_RY));
        p->end[k] = p->rim[k] + p->len[k];
        p->den[k] = p->end[k] - p->rim[k] + 2;
    }
    static const float FACE_A[UR_FACE] = { -2.4f, -1.9f, 2.2f, 1.2f, 0.5f };
    for (int k = 0; k < UR_FACE; k++) {
        float a = FACE_A[k] + 0.6f * amp * fast_sin(clock * sp + k * 2.3f + 8 * TAU);
        p->dx[UR_SPINES + k] = cosf(a); p->dy[UR_SPINES + k] = sinf(a);
        p->len[UR_SPINES + k] = 4.5f;
    }
}
/* the tone at local (lx, ly) from the dome's centre, or UR_NONE */
static int urchin_tone(float lx, float ly, const ur_pose_t *p) {
    if (ly > UR_BASE) return UR_NONE;
    float lit = (-lx * 0.55f - ly * 0.83f) / UR_RX;                /* -1..1: lower right .. upper left */
    float ex = lx / UR_RX, ey = ly / UR_RY, er = ex * ex + ey * ey;
    /* the face spines first: they stand over the dome */
    for (int k = UR_SPINES; k < UR_SPINES + UR_FACE; k++) {
        static const float FX[UR_FACE] = { -4.5f, 0.5f, -5.0f, 2.0f, 5.5f }, FY[UR_FACE] = { -2.5f, -4.5f, 3.0f, 2.5f, 0.0f };
        float vx = lx - FX[k - UR_SPINES], vy = ly - FY[k - UR_SPINES];
        float u = vx * p->dx[k] + vy * p->dy[k], w = vx * p->dy[k] - vy * p->dx[k];
        if (u < 0 || u > p->len[k]) continue;
        float half = 1.6f * (1 - u / p->len[k]) + 0.35f;
        if (w * w > half * half) continue;
        return u > p->len[k] - 1.6f ? UR_T5 : lit > 0 ? UR_T4 : UR_T3;
    }
    if (er <= 1.0f) {                                               /* the dome */
        int tone = 2;
        if (lit > 0.15f) tone = 3;
        if (lit > 0.55f && er > 0.20f) tone = 4;
        if (lit < -0.30f) tone = 1;
        if (lit < -0.75f || ly > UR_BASE - 1.2f) tone = 0;
        int ix = (int)floorf(lx), iy = (int)floorf(ly);
        uint32_t h = chash(ix + 40, iy + 40);
        if ((h & 7) == 0) tone = tone > 0 ? tone - 1 : 0;           /* the mottle */
        else if ((h & 15) == 1 && tone < 4) tone++;
        return UR_T0 + tone;
    }
    /* the crown: a spine is a wedge from inside the dome out to a fine point */
    for (int k = 0; k < UR_SPINES; k++) {
        float u = lx * p->dx[k] + ly * p->dy[k], w = lx * p->dy[k] - ly * p->dx[k];
        float rim = p->rim[k], end = p->end[k];
        if (u < rim - 2 || u > end) continue;
        float half = 2.6f * (end - u) / p->den[k] + 0.3f;
        if (w * w > half * half) continue;
        float slit = -p->dx[k] * 0.55f - p->dy[k] * 0.83f;          /* the spine's own side of the light */
        if (u > end - 1.8f) return slit > -0.5f ? UR_T5 : UR_T4;    /* pale tips */
        if (u > end - 3.8f) return slit > -0.3f ? UR_T4 : UR_T3;
        return slit > 0.2f ? UR_T3 : slit > -0.5f ? UR_T2 : UR_T1;
    }
    return UR_NONE;
}
static void draw_urchin(ctx_t *c, const tank_t *t) {
    if (g_no_animals) return;
    if (!(t->sd_unlocks & SD_ITEM_URCHIN) || t->urchin_x < 0) return;
    /* its tones a few times a second, not a frame (2026-10-23: the spines' pose per pixel
       was 2-3 ms of a square frame); the row's palette once a row, not a pixel */
    static double s_t = -1e9; static bool s_chew;
    bool chew = tank_urchin_chewing(t);
    uint8_t *tn = ds()->ur_tones;
    if (t->clock - s_t >= 0.08f || t->clock < s_t || chew != s_chew) {
        ur_pose_t p; urchin_pose(&p, anim(t), chew);
        for (int ly = -16; ly <= (int)UR_BASE; ly++)
            for (int lx = -18; lx <= 18; lx++) tn[(ly + 16) * 37 + lx + 18] = (uint8_t)urchin_tone((float)lx + 0.5f, (float)ly + 0.5f, &p);
        s_t = t->clock; s_chew = chew;
    }
    int cx = (int)floorf(t->urchin_x + 0.5f), cy = (int)(URCHIN_FLOOR_Y + t->urchin_dy);   /* (urchin_dy: on a front piece's base line in its front lane) */
    for (int ly = -16; ly <= (int)UR_BASE; ly++) {
        int y = cy + ly;
        uint32_t w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y);
        src_t pal[UR_NONE];
        for (int k = 0; k < UR_NONE; k++) pal[k] = src_color(rgb_mix(w, URCHIN_RGB[k], UR_TINT), c->dim);
        const uint8_t *row = tn + (ly + 16) * 37;
        for (int lx = -18; lx <= 18; lx++) {
            int tone = row[lx + 18];
            if (tone == UR_NONE) continue;
            px_blend_s(c, cx + lx, y, &pal[tone], 255);
        }
    }
}
/* does the urchin, as draw_urchin painted it this frame (ds()->ur_tones), cover (x, y)? */
static bool urchin_covers(const tank_t *t, int x, int y) {
    if (!(t->sd_unlocks & SD_ITEM_URCHIN) || t->urchin_x < 0) return false;
    const int lx = x - (int)floorf(t->urchin_x + 0.5f), ly = y - (int)(URCHIN_FLOOR_Y + t->urchin_dy);
    if (lx < -18 || lx > 18 || ly < -16 || ly > (int)UR_BASE) return false;
    return ds()->ur_tones[(ly + 16) * 37 + lx + 18] != UR_NONE;
}

/* ---- the strawberry crab, procedural (0.3.9, SD_ITEM_CRAB) ----
 * After Strato's strawberry-crab sheets (working-assets/crab/ref7..ref10 and
 * the pixel crabs he sent 10-23). The prototypes are working-assets/crab/
 * strawberry_side.py (the profile) and strawberry_proc.py (the roost),
 * whose coordinates these are: crab units = px, x across its middle, y down
 * from above it to its feet at CR_FLOOR_SIDE / CR_FLOOR_FRONT. Every cell is
 * COMPUTED from a few shapes at its centre (no smoothing, so it stays crisp
 * pixel art): a dome with three tones and single cream seeds, round eyes with
 * a glint, a tan face, claws (an oval palm, two pale jaws), legs as cubic
 * curves ROOTED UNDER the shell (it is drawn over their roots) and curled in
 * at the toe; then one pass darkens every cell on a boundary between two
 * parts or against the water by its own tone's step (no drawn line: the other
 * creatures are tone-stepped too; never into the sand). Two views:
 *  - the PROFILE (walk, rest, dig, eat, claws up), facing cr_facing;
 *  - the ROOST (CR_ROOST, Strato: "facing forward, straight toward the
 *    glass, tucked in on itself"): the front view, breathing, its legs
 *    settling in over its first second, claws open while poked.
 * Drawn the urchin's way: the tones go into a grid rebuilt only when the
 * pose changes (a key of mode, gait step, dig beat, breath, poke), and the
 * grid is blitted every frame with the row's palette mixed with the water.
 * On the volcano's flank the grid is sampled through a rotation (cr_tilt)
 * about its footing. */
#define CR_GX0          (-25)
#define CR_GW             50
#define CR_GH             25          /* rows: crab y from floor - 25 to floor (the bottom row stands on the sand) */
#define CR_TINT        0.90f
#define CR_FLOOR_SIDE  21.0f
#define CR_FLOOR_FRONT 22.0f
#define CR_LX (-0.50f)
#define CR_LY (-0.70f)
#define CR_LZ  0.51f
enum { CRT_NONE, CRT_O, CRT_R, CRT_RD, CRT_RH, CRT_D, CRT_L, CRT_LD, CRT_LK, CRT_P, CRT_Q, CRT_F, CRT_G, CRT_M, CRT_E, CRT_W, CRT_T, CRT_X, CRT_SD, CRT_SDD, CRT_N };   /* SD, SDD: the sand dollar it digs up, gold (the shop's coin) */
static const uint32_t CR_RGB[CRT_N] = { 0, 0x5e0a0e, 0xe8281a, 0xb8180f, 0xff5a40, 0xf8dccc, 0xf24a30, 0xc0301e, 0xff8a66,
                                        0xf6c8a0, 0xd09070, 0xe8b48a, 0xc4885e, 0xd0201a, 0x1a0c08, 0xffffff, 0xe89a50, 0x2a0a08, 0xf4c838, 0xb07c16 };
/* part ids (the outline goes between them): 1 shell, 2 face, 3 the near eye, 4 the far eye,
 * 10/11 claws, 12 the far claw, 20+ near legs (2 per leg: root half, toe half), 40+ far legs */
#define CR_LEG_PTS 11
typedef struct { float p[CR_LEG_PTS][2]; float x0, y0, x1, y1, w0, w1; bool far; } cr_leg_t;
typedef struct { float ax0, ay0, ax1, ay1, cx, cy, rx, ry, j1[4], j2[4], plx, ply; int bite; } cr_sclaw_t;   /* bite: a tone held in the jaws (CRT_NONE = empty) */
typedef struct {
    bool       front;
    float      dy;                           /* the body's drop (rest, a step's bob) or rise (the roost's breath) */
    cr_leg_t   leg[8]; int nleg;
    cr_sclaw_t claw, far_claw;               /* the profile */
    float      eye[2][3]; bool eye_far[2];   /* the profile: x, y, r */
    float      fclaw[2][5];                  /* the roost: cx, cy, rx, ry, side */
    float      open;                         /* the roost's jaws, 0 shut .. 1 open */
    int        salt;
    int        find;                         /* a SAND DOLLAR held up in its claw: 0 none, 1 / 2 the glint's two beats */
    float      fx, fy;                       /* ... where (crab units) */
} cr_pose_t;
static inline int cr_tone3(float in, int lit, int base, int dark, float hi, float lo) { return in > hi ? lit : in < lo ? dark : base; }
static bool cr_seed(float px, float py, float pitch, int salt) {   /* one seed per cell of a jittered grid (the prototypes' seed_at) */
    float gx = floorf((px + 40) / pitch), gy = floorf((py + 40) / pitch);
    uint32_t h = chash((int)gx + salt, (int)gy + 3 * salt);
    float sx = gx * pitch - 40 + 0.6f + (float)(h & 7) / 7.0f * (pitch - 1.6f);
    float sy = gy * pitch - 40 + 0.6f + (float)((h >> 3) & 7) / 7.0f * (pitch - 1.6f);
    return floorf(px) == floorf(sx) && floorf(py) == floorf(sy);
}
static void cr_leg(cr_leg_t *lg, const float q[4][2], float w0, float w1, bool far) {
    lg->x0 = lg->y0 = 1e9f; lg->x1 = lg->y1 = -1e9f;
    for (int i = 0; i < CR_LEG_PTS; i++) {
        float s = (float)i / (CR_LEG_PTS - 1), a = (1 - s) * (1 - s) * (1 - s), b = 3 * (1 - s) * (1 - s) * s, c = 3 * (1 - s) * s * s, d = s * s * s;
        float x = a * q[0][0] + b * q[1][0] + c * q[2][0] + d * q[3][0], y = a * q[0][1] + b * q[1][1] + c * q[2][1] + d * q[3][1];
        lg->p[i][0] = x; lg->p[i][1] = y;
        if (x < lg->x0) lg->x0 = x;
        if (x > lg->x1) lg->x1 = x;
        if (y < lg->y0) lg->y0 = y;
        if (y > lg->y1) lg->y1 = y;
    }
    lg->x0 -= w0; lg->y0 -= w0; lg->x1 += w0; lg->y1 += w0; lg->w0 = w0; lg->w1 = w1; lg->far = far;
}
/* the nearest point of a leg: distance, how far along (0 root .. 1 toe), signed side (+ right of the way it runs); false = outside its box */
static bool cr_leg_near(const cr_leg_t *lg, float px, float py, float *d, float *u, float *side) {
    if (px < lg->x0 || px > lg->x1 || py < lg->y0 || py > lg->y1) return false;
    float best = 1e9f;
    for (int i = 0; i < CR_LEG_PTS - 1; i++) {
        float x0 = lg->p[i][0], y0 = lg->p[i][1], vx = lg->p[i + 1][0] - x0, vy = lg->p[i + 1][1] - y0, l2 = vx * vx + vy * vy;
        if (l2 < 1e-9f) l2 = 1e-9f;
        float t = ((px - x0) * vx + (py - y0) * vy) / l2; t = t < 0 ? 0 : t > 1 ? 1 : t;
        float ex = px - x0 - t * vx, ey = py - y0 - t * vy, dd = ex * ex + ey * ey;
        if (dd < best) { best = dd; *u = (i + t) / (CR_LEG_PTS - 1); *side = ((px - x0) * vy - (py - y0) * vx) / sqrtf(l2); }
    }
    *d = sqrtf(best);
    return true;
}
static float cr_segd(float px, float py, const float *s, float *t) {
    float vx = s[2] - s[0], vy = s[3] - s[1], l2 = vx * vx + vy * vy;
    if (l2 < 1e-9f) l2 = 1e-9f;
    float tt = ((px - s[0]) * vx + (py - s[1]) * vy) / l2; tt = tt < 0 ? 0 : tt > 1 ? 1 : tt;
    *t = tt;
    float ex = px - s[0] - tt * vx, ey = py - s[1] - tt * vy;
    return sqrtf(ex * ex + ey * ey);
}
static void cr_set_sclaw(cr_sclaw_t *c, float ax0, float ay0, float ax1, float ay1, float cx, float cy, float rx, float ry,
                         float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
    c->ax0 = ax0; c->ay0 = ay0; c->ax1 = ax1; c->ay1 = ay1; c->cx = cx; c->cy = cy; c->rx = rx; c->ry = ry;
    c->j1[0] = a0; c->j1[1] = a1; c->j1[2] = a2; c->j1[3] = a3; c->j2[0] = b0; c->j2[1] = b1; c->j2[2] = b2; c->j2[3] = b3; c->bite = CRT_NONE;
}
/* the roost (the front view): settle 1 -> 0 over its first moments, a stretch's claws, a poke, a breath (the shell up a pixel) */
static void cr_pose_roost(cr_pose_t *p, float settle, float stretch, bool poke, bool breath) {
    float flex = poke ? -0.8f : 0.6f - 1.6f * settle - 0.6f * stretch;
    p->front = true;
    p->dy = (breath && !poke) ? -1.0f : 0.0f;
    p->open = poke ? 1.0f : 0.7f * stretch;
    p->nleg = 0;
    for (int s = -1; s <= 1; s += 2) {
        float f = flex;
        const float outer[4][2] = { { s * 11.5f, 8.0f }, { s * (17.8f - f), 9.5f }, { s * (17.8f - f), 18.5f }, { s * (12.0f - f * 0.5f), 21.6f } };
        const float inner[4][2] = { { s * 9.5f, 11.5f }, { s * (13.8f - f), 13.0f }, { s * (13.4f - f), 18.5f }, { s * (10.6f - f * 0.4f), 21.2f } };
        cr_leg(&p->leg[p->nleg++], outer, 2.5f, 1.5f, false);
        cr_leg(&p->leg[p->nleg++], inner, 2.0f, 1.2f, false);
    }
    const float fc[2][5] = { { -7.4f, 16.8f, 6.6f, 4.0f, -1 }, { 7.4f, 16.8f, 6.6f, 4.0f, 1 } };
    memcpy(p->fclaw, fc, sizeof fc);
}
static void cr_pose(cr_pose_t *p, const tank_t *t) {
    memset(p, 0, sizeof *p);
    const int mode = t->cr_mode;
    const bool poke = t->cr_poke > 0 || t->cr_scurry || t->cr_find > 0;   /* a tap's claws up; running scared, claws up too; a find held up */
    if (t->cr_find > 0) p->find = 1 + (((int)(t->clock * 2)) & 1);     /* its glint beats twice a second (the grid rebuilds on the beat) */
    p->salt = 7 + t->cr_variant;
    p->front = mode == CR_ROOST || t->cr_find > 0;                   /* a find: it turns to the glass and holds the coin up in both claws (Strato 2026-10-24) */
    if (p->front) {                                                   /* the roost: strawberry_proc.py pose() */
        float settle = t->cr_mode_s < 1.2f ? floorf((1 - t->cr_mode_s / 1.2f) * 6) / 6 : 0;   /* it settles in: legs spread, then drawn under */
        float stretch = 0;
        if (t->cr_fidget_kind == CR_FID_STRETCH) stretch = floorf(fast_sin((1 - t->cr_fidget / CR_STRETCH_S) * 3.14159f + 4 * TAU) * 6 + 0.5f) / 6;   /* a slow open and close of its claws */
        if (stretch < 0) stretch = 0;
        cr_pose_roost(p, t->cr_find > 0 ? 0 : settle, stretch, poke, fast_sin(anim(t) * 0.9f + 4 * TAU) > 0.35f);
        if (t->cr_find > 0) {                                         /* the claws raised over its head, the coin between them (cr_draw_disc leaves the claws' px over its lower edge) */
            const float fc[2][5] = { { -8.6f, 3.2f, 5.4f, 3.4f, -1 }, { 8.6f, 3.2f, 5.4f, 3.4f, 1 } };
            memcpy(p->fclaw, fc, sizeof fc); p->open = 0; p->dy = 0;
        }
        return;
    }
    /* the profile: strawberry_side.py pose() */
    static const float NEAR[4][9] = { { 5.0f, 12.0f, 10.0f, 13.0f, 13.0f, 17.5f, 13.5f, CR_FLOOR_SIDE, 0.0f },
                                      { 0.5f, 12.5f, 3.5f, 14.0f, 6.0f, 18.0f, 6.0f, CR_FLOOR_SIDE, 0.5f },
                                      { -4.0f, 12.5f, -8.0f, 13.5f, -10.0f, 18.0f, -9.5f, CR_FLOOR_SIDE, 0.0f },
                                      { -8.5f, 12.0f, -15.0f, 12.5f, -18.5f, 17.0f, -18.5f, CR_FLOOR_SIDE, 0.5f } };
    static const float FAR[4][9] = { { 7.0f, 11.5f, 12.5f, 12.5f, 16.0f, 16.5f, 16.5f, CR_FLOOR_SIDE - 0.5f, 0.5f },
                                     { -1.5f, 12.0f, 0.0f, 13.5f, 1.5f, 17.5f, 1.0f, CR_FLOOR_SIDE - 0.5f, 0.0f },
                                     { -6.0f, 12.0f, -11.5f, 13.0f, -14.0f, 17.0f, -14.0f, CR_FLOOR_SIDE - 0.5f, 0.5f },
                                     { -10.0f, 11.5f, -17.5f, 11.5f, -21.0f, 15.5f, -21.5f, CR_FLOOR_SIDE - 0.5f, 0.0f } };
    const bool nibble = mode == CR_CLIMB && t->cr_nibble > 0;      /* a stop on the flank, nibbling the rock (0.3.9.1) */
    const bool walking = tank_crab_walking(t) && !nibble && !(mode == CR_CLIMB && t->cr_climb >= 1 && t->cr_climb < 1 + 25.0f / 75.0f);
    const float gait = floorf(fmodf(t->cr_gait, TAU) / TAU * 16) / 16 * TAU;   /* 16 steps a stride: the grid rebuilds on a step, not a frame */
    const bool resting = (mode == CR_REST || mode == CR_DIG) && !poke && !walking;
    float dy = walking ? 0.45f * fast_sin(gait * 2 + 4 * TAU) : resting ? 1.2f : 0.0f;
    /* the big claw lies between two keys - down at the sand ahead (L 0) and up at its face (L 1) - its jaws parted by
       `open`. A dig and a meal are one pick of CR_PICK_S: down with the jaws open, a pinch, a bite carried up to its
       face, and down again (Strato 10-23: the claw "didn't look quite right"); a groom lifts it empty */
    float L = walking ? 0.35f : 0.3f, open = 0; int bite = CRT_NONE;
    if ((mode == CR_DIG || mode == CR_EAT || nibble) && !poke) {
        float u = floorf(fmodf((nibble ? CR_NIBBLE_S - t->cr_nibble : t->cr_mode_s) / CR_PICK_S, 1.0f) * 12) / 12;
        if (u < 0.3f) { L = 1 - u / 0.3f; open = u >= 0.1f ? 1 : 0; }
        else if (u < 0.42f) { L = 0; open = u < 0.36f ? 0.5f : 0; }
        else if (u < 0.75f) { L = (u - 0.42f) / 0.33f; bite = mode == CR_EAT ? CRT_X : CRT_G; }
        else { L = 1; bite = mode == CR_EAT ? CRT_X : CRT_G; }
        dy += 0.8f * (1 - L);                                         /* it leans in to the pick */
    } else if (t->cr_fidget_kind == CR_FID_GROOM) {
        float g = floorf((1 - t->cr_fidget / CR_GROOM_S) * 8) / 8;
        L = 0.3f + 0.7f * fast_sin(g * 3.14159f + 4 * TAU);
    } else if (t->cr_fidget_kind == CR_FID_SETTLE) dy += 1.0f;        /* it sits a little lower for a moment */
    p->dy = dy;
    const float k = resting ? 0.35f : 0.0f;                           /* at rest the toes draw in under it */
    p->nleg = 0;
    for (int pass = 0; pass < 2; pass++) {
        const float (*L)[9] = pass == 0 ? FAR : NEAR;
        for (int i = 0; i < 4; i++) {
            float fx = L[i][6], fy = L[i][7];
            if (walking) {                                            /* a stance sweeps the foot back 3.2 px, a swing lifts it forward */
                float f = fmodf(gait / TAU + L[i][8], 1.0f);
                if (f < 0.6f) fx += 1.6f - 3.2f * (f / 0.6f);
                else { float g = (f - 0.6f) / 0.4f; fx += -1.6f + 3.2f * g; fy -= 2.2f * fast_sin(g * 3.14159f + 4 * TAU); }
            }
            const float q[4][2] = { { L[i][0], L[i][1] + dy },
                                    { L[i][2] - (L[i][2] - L[i][0]) * k * 0.4f, L[i][3] + dy },
                                    { L[i][4] + (fx - L[i][6]) * 0.6f - (L[i][4] - L[i][0]) * k, L[i][5] + dy * 0.5f },
                                    { fx - (fx - L[i][0]) * k, fy } };
            if (pass == 0) cr_leg(&p->leg[p->nleg++], q, 2.1f, 1.2f, true);
            else cr_leg(&p->leg[p->nleg++], q, 2.5f, 1.2f, false);
        }
    }
    if (poke)                                                         /* claws up: both raised, jaws open */
        cr_set_sclaw(&p->claw, 8.5f, 12.0f + dy, 12.0f, 9.0f + dy, 14.5f, 6.5f + dy, 4.6f, 3.2f,
                     17.5f, 4.5f + dy, 20.5f, 2.5f + dy, 18.5f, 7.2f + dy, 21.5f, 6.4f + dy);
    else {
        static const float SAND[16]  = { 8.5f, 13.0f, 13.5f, 16.5f,  16.8f, 18.0f, 4.4f, 2.6f,  20.6f, 17.0f, 23.6f, 19.4f,  20.2f, 19.6f, 22.6f, 20.6f };
        static const float MOUTH[16] = { 8.5f, 13.0f, 11.5f, 13.2f,  13.8f, 11.6f, 3.6f, 2.7f,  13.2f,  9.6f, 11.6f,  7.8f,  15.4f,  9.4f, 14.6f,  7.0f };
        float k8[16];
        for (int i = 0; i < 16; i++) k8[i] = SAND[i] + (MOUTH[i] - SAND[i]) * L;
        k8[1] += dy; k8[3] += dy; k8[5] += dy; k8[9] += dy * L; k8[11] += dy * L; k8[13] += dy * L; k8[15] += dy * L;   /* the body's lean; at the sand the jaws stay on it */
        k8[10] += -1.0f * L * open; k8[11] += -1.6f * (1 - L) * open;  /* the jaws part: up and down at the sand, apart at the face */
        k8[14] += 1.0f * L * open;  k8[15] += 1.2f * (1 - L) * open;
        cr_set_sclaw(&p->claw, k8[0], k8[1], k8[2], k8[3], k8[4], k8[5], k8[6], k8[7], k8[8], k8[9], k8[10], k8[11], k8[12], k8[13], k8[14], k8[15]);
        if (bite != CRT_NONE) { p->claw.bite = bite; p->claw.plx = (k8[10] + k8[14]) * 0.5f; p->claw.ply = (k8[11] + k8[15]) * 0.5f; }
    }
    if (poke)
        cr_set_sclaw(&p->far_claw, 9.5f, 11.0f + dy, 12.0f, 8.0f + dy, 13.5f, 5.2f + dy, 3.2f, 2.4f,
                     15.8f, 3.8f + dy, 18.0f, 2.4f + dy, 16.4f, 6.0f + dy, 18.6f, 5.4f + dy);
    else
        cr_set_sclaw(&p->far_claw, 9.5f, 12.0f + dy, 12.5f, 12.6f + dy, 14.6f, 13.2f + dy, 3.0f, 2.2f,
                     17.2f, 12.4f + dy, 19.4f, 12.8f + dy, 17.2f, 14.2f + dy, 19.0f, 14.2f + dy);
    if (p->find) { p->fx = 20.2f; p->fy = 1.2f + dy; }               /* held up in the raised claw's jaws (the poke pose), over its head */
    p->eye[0][0] = 6.5f; p->eye[0][1] = 6.5f + dy; p->eye[0][2] = 2.7f; p->eye_far[0] = false;
    p->eye[1][0] = 10.0f; p->eye[1][1] = 4.4f + dy; p->eye[1][2] = 1.9f; p->eye_far[1] = true;
}
/* the profile's claw (strawberry_side.py claw_px): an arm, an oval palm, two jaws, maybe a pellet */
static int cr_sclaw_tone(float px, float py, const cr_sclaw_t *c, bool far) {
    int hit = CRT_NONE;
    float t;
    const float arm[4] = { c->ax0, c->ay0, c->ax1, c->ay1 };
    float d = cr_segd(px, py, arm, &t);
    if (d <= 1.6f - 0.2f * t) hit = d < 0.9f ? CRT_L : CRT_LD;
    float ex = (px - c->cx) / c->rx, ey = (py - c->cy) / c->ry, v = ex * ex + ey * ey;
    if (v <= 1.0f) {
        hit = cr_tone3(-ex * 0.5f - ey * 0.85f, CRT_LK, CRT_L, CRT_LD, 0.45f, -0.45f);
        if (v < 0.6f && cr_seed(px, py, 3.6f, 13)) hit = CRT_D;
    }
    for (int j = 0; j < 2; j++) {
        float dj = cr_segd(px, py, j == 0 ? c->j1 : c->j2, &t);
        if (dj <= 1.25f - 0.6f * t && (hit == CRT_NONE || t > 0.15f)) hit = (j == 0 || t > 0.5f) ? CRT_P : CRT_Q;
    }
    if (c->bite != CRT_NONE && (px - c->plx) * (px - c->plx) + (py - c->ply) * (py - c->ply) <= 1.69f) hit = c->bite;
    if (hit != CRT_NONE && far) hit = hit == CRT_LK ? CRT_L : hit == CRT_L ? CRT_LD : hit == CRT_LD ? CRT_RD : hit == CRT_P ? CRT_Q : hit == CRT_D ? CRT_LD : hit;
    return hit;
}
/* the tone and the part at crab-unit (px, py); painter's order, the last hit wins */
static int cr_cell(float px, float py, const cr_pose_t *p, int *part) {
    int tone = CRT_NONE; *part = 0;
    float d, u, side;
    if (p->front) {                                                   /* strawberry_proc.py crab_px */
        if (py >= 10.5f && py <= 15.5f && fabsf(px) <= 11.5f - (py - 11.5f) * 1.6f) {   /* the face under the lip, the fangs */
            tone = (fabsf(px) > 9.5f - (py - 11.5f) * 1.6f || py > 13.5f) ? CRT_G : CRT_F;
            if (fabsf(px) >= 2.0f && fabsf(px) <= 3.0f && py < 13.0f) tone = CRT_M;
            *part = 2;
        }
        for (int i = 0; i < p->nleg; i++) {
            const cr_leg_t *lg = &p->leg[i];
            if (!cr_leg_near(lg, px, py, &d, &u, &side)) continue;
            float w = lg->w0 + (lg->w1 - lg->w0) * u;
            if (d > w) continue;
            float out = side * (lg->p[0][0] < 0 ? 1.0f : -1.0f);
            tone = out < -0.15f * w ? CRT_LK : out > 0.75f * w ? CRT_LD : CRT_L;
            if (u > 0.9f) tone = CRT_P;
            *part = 20 + i * 2 + (u < 0.5f ? 0 : 1);
        }
        float y = py - p->dy;
        if (y > -0.3f && y < 11.6f) {                                 /* the shell: a dome 14.3 wide at the lip */
            float tt = (11.6f - y) / 11.9f, hw = 14.3f * powf(fmaxf(0.0f, 1 - tt * tt), 0.45f);
            if (fabsf(px) <= hw) {
                float nx = px / 14.3f, ny = tt, n2 = nx * nx + ny * ny, nz = sqrtf(n2 < 1 ? 1 - n2 : 0);
                tone = cr_tone3(nx * CR_LX - ny * CR_LY + nz * CR_LZ, CRT_RH, CRT_R, CRT_RD, 0.80f, 0.08f);
                bool clear = true;
                for (int k = -1; k <= 1; k += 2) if ((px - k * 5.5f) * (px - k * 5.5f) + (y - 8.5f) * (y - 8.5f) <= 3.9f * 3.9f) clear = false;
                if (clear && fabsf(px) < hw - 1.2f && y > 0.9f && y < 10.6f && cr_seed(px, y, 2.9f, p->salt)) tone = CRT_D;
                *part = 1;
                for (int k = -1; k <= 1; k += 2) {                    /* the eyes on the lip */
                    float ex = k * 5.5f, ey = 8.5f, ddx = px - ex, ddy = y - ey, e = sqrtf(ddx * ddx + ddy * ddy);
                    if (e <= 2.95f) {
                        bool g = ddx >= -2.0f && ddx < 0 && ddy >= -2.0f && ddy < 0 && e < 2.3f;
                        tone = g ? CRT_W : (ddy > 1.6f && fabsf(ddx) > 0.6f) ? CRT_T : CRT_E; *part = 3;
                    } else if (ddy > 2.95f && ddy <= 3.9f && fabsf(ddx) <= 1.5f) { tone = CRT_T; *part = 3; }
                }
            }
        }
        for (int j = 0; j < 2; j++) {                                 /* the claws folded in front */
            const float *c = p->fclaw[j];
            float ex = (px - c[0]) / c[2], ey = (py - c[1]) / c[3], v = ex * ex + ey * ey;
            if (v > 1.0f) continue;
            tone = cr_tone3(-ex * 0.6f - ey * 0.8f, CRT_LK, CRT_L, CRT_LD, 0.45f, -0.45f);
            if (v < 0.6f && cr_seed(px, py, 4.4f, 11 + j)) tone = CRT_D;
            float inner = -c[4] * (px - c[0]);
            if (inner > c[2] * 0.45f && py > c[1] - 1.6f - 1.2f * p->open) {   /* the pincers: pale, the jaws parting as it opens */
                tone = py > c[1] - 0.6f ? CRT_P : CRT_Q;
                float gap = 1.0f + 2.6f * p->open;
                if (py >= c[1] + 0.9f && py < c[1] + 0.9f + gap) tone = CRT_O;
            }
            *part = 10 + j;
        }
        return tone;
    }
    /* the profile (strawberry_side.py crab_px): far legs, far claw, near legs, face, shell, eyes, the big claw */
    for (int i = 0; i < p->nleg; i++) {
        const cr_leg_t *lg = &p->leg[i];
        if (!lg->far || !cr_leg_near(lg, px, py, &d, &u, &side)) continue;
        if (d <= 2.1f - 0.9f * u) { tone = u > 0.88f ? CRT_Q : side > 0.2f ? CRT_LD : CRT_L; *part = 40 + i * 2 + (u < 0.5f ? 0 : 1); }
    }
    int ct = cr_sclaw_tone(px, py, &p->far_claw, true);
    if (ct != CRT_NONE) { tone = ct; *part = 12; }
    for (int i = 0; i < p->nleg; i++) {
        const cr_leg_t *lg = &p->leg[i];
        if (lg->far || !cr_leg_near(lg, px, py, &d, &u, &side)) continue;
        float w = 2.5f - 1.3f * u;
        if (d > w) continue;
        bool back = lg->p[0][0] > lg->p[CR_LEG_PTS - 1][0];           /* a leg reaching back: its outer side is the other one */
        float sd = back ? -side : side;
        tone = sd < -0.25f * w ? CRT_LK : sd > 0.7f * w ? CRT_LD : CRT_L;
        if (u > 0.88f) tone = CRT_P;
        *part = 20 + i * 2 + (u < 0.5f ? 0 : 1);
    }
    {   float fx = (px - 8.5f) / 5.0f, fy = (py - (12.3f + p->dy)) / 2.8f;   /* the tan face under the front */
        if (fx * fx + fy * fy <= 1.0f) { tone = (fy > 0.7f || fx < -0.7f) ? CRT_G : CRT_F; *part = 2; } }
    {   float y = py - p->dy;                                         /* the shell: a long dome, higher at the front */
        const float x0 = -1.0f, rx = px > x0 ? 12.5f : 14.0f, uu = (px - x0) / rx;
        if (fabsf(uu) < 1.0f) {
            float top = 12.5f - 13.0f * sqrtf(1 - uu * uu) * (1.0f + 0.12f * uu), bot = 12.5f - 2.6f * uu * uu * uu * uu;
            if (top < bot && y >= top && y <= bot) {
                float ny = (y - (top + bot) * 0.5f) / fmaxf(1.0f, (bot - top) * 0.5f);
                ny = ny < -1 ? -1 : ny > 1 ? 1 : ny;
                float n2 = 0.6f * uu * uu + 0.6f * ny * ny, nz = sqrtf(n2 < 1 ? 1 - n2 : 0);
                tone = cr_tone3(uu * CR_LX + ny * CR_LY + nz * CR_LZ, CRT_RH, CRT_R, CRT_RD, 0.78f, 0.12f);
                bool clear = true;
                for (int k = 0; k < 2; k++) { float ddx = px - p->eye[k][0], ddy = py - p->eye[k][1], r = p->eye[k][2] + 1.4f; if (ddx * ddx + ddy * ddy < r * r) clear = false; }
                if (clear && y > top + 1.0f && y < bot - 1.0f && cr_seed(px, y, 2.9f, p->salt)) tone = CRT_D;
                *part = 1;
            }
        } }
    for (int k = 0; k < 2; k++) {                                     /* the big eye on the flank, the far one over the front edge */
        float ddx = px - p->eye[k][0], ddy = py - p->eye[k][1], R = p->eye[k][2], e = sqrtf(ddx * ddx + ddy * ddy);
        if (e > R) continue;
        bool far = p->eye_far[k];
        bool g = !far ? (ddx >= -2.0f && ddx < 0 && ddy >= -2.0f && ddy < 0 && e < R - 0.3f) : (ddx >= -1.0f && ddx < 0 && ddy >= -1.0f && ddy < 0);
        tone = g ? CRT_W : CRT_E;
        if (!far && ddy > 1.2f && fabsf(ddx) > 0.6f) tone = CRT_T;
        *part = far ? 4 : 3;
    }
    ct = cr_sclaw_tone(px, py, &p->claw, false);
    if (ct != CRT_NONE) { tone = ct; *part = 10; }
    return tone;
}
/* the grid: every cell's tone, then its EDGES - a cell that borders the water
   or another part takes its own tone's darker step (the shell's dark red, the
   limbs' dark, the pincers' deeper pale, the face's tan), never a drawn line:
   the shrimp, the urchin, the snail and the seahorse separate their parts by
   tone steps alone, and a maroon line round the crab made it the one creature
   in another illustration style (Strato 2026-10-24: "parity"; the prototypes
   in working-assets/crab still draw the line). The eyes keep their rims. */
static int cr_dark_of(int tone) {
    switch (tone) {
    case CRT_R: case CRT_RH: case CRT_D: case CRT_RD: return CRT_RD;
    case CRT_L: case CRT_LK: case CRT_LD: return CRT_LD;
    case CRT_P: case CRT_Q: return CRT_Q;
    case CRT_F: case CRT_G: case CRT_M: return CRT_G;
    case CRT_SD: return CRT_SDD;
    default: return tone;
    }
}
static void cr_build(uint8_t *tn, uint8_t *pt, const cr_pose_t *p) {
    const float floor_u = p->front ? CR_FLOOR_FRONT : CR_FLOOR_SIDE;
    for (int gy = 0; gy < CR_GH; gy++)
        for (int gx = 0; gx < CR_GW; gx++) {
            int part, tone = cr_cell(CR_GX0 + gx + 0.5f, floor_u - CR_GH + gy + 0.5f, p, &part);
            tn[gy * CR_GW + gx] = (uint8_t)tone; pt[gy * CR_GW + gx] = (uint8_t)(tone == CRT_NONE ? 0 : part);
        }
    for (int gy = 0; gy < CR_GH; gy++)                                /* the edge pass reads the parts, never the tones it just wrote */
        for (int gx = 0; gx < CR_GW; gx++) {
            const int t0 = pt[gy * CR_GW + gx];
            if (!t0 || t0 == 3 || t0 == 4) continue;
            bool edge = false;
            const int N[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            for (int k = 0; k < 4 && !edge; k++) {
                int nx = gx + N[k][0], ny = gy + N[k][1];
                if (ny >= CR_GH) continue;                            /* the sand under it */
                int n = (nx < 0 || nx >= CR_GW || ny < 0) ? 0 : pt[ny * CR_GW + nx];
                if (n == t0 || n == 3) continue;
                if (p->front && t0 == 2 && n == 1) continue;          /* the lip's step is on the shell's side */
                edge = true;
            }
            if (edge) tn[gy * CR_GW + gx] = (uint8_t)cr_dark_of(tn[gy * CR_GW + gx]);
        }
}
static uint32_t cr_hash(const cr_pose_t *p) {                        /* FNV-1a over the pose (memset to 0 first: no stray padding) */
    const uint8_t *b = (const uint8_t *)p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *p; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;
}
/* the SAND DOLLAR it holds up (2026-10-24 evening, Strato at the glass after the first staged find: "it was so small
 * ... nearly as big as the crab itself holding it above its head like the icon"): a gold disc the badge icon's size,
 * two fifths of the crab's width, over its head between the raised claws and overlapping the top of its shell - drawn
 * OUTSIDE the tone grid (25 rows end at its head), with its five-petal mark and the glint that beats twice a second.
 * (0.3.9 as released held a 7 px disc in the right claw's jaws, inside the grid.) */
#define CR_DISC_R     9.5f
#define CR_DISC_UP    3.0f                       /* the disc's middle this far over the grid's top row: its lower edge between the raised claws */
static inline bool cr_claw_tone(int tone) { return tone == CRT_L || tone == CRT_LD || tone == CRT_LK || tone == CRT_P || tone == CRT_Q || tone == CRT_O || tone == CRT_D; }
/* tn: the crab's tone grid as blitted 1:1 at (cx_px, feet), or NULL - a claw's px in it stays over the coin: it HOLDS it */
static void cr_draw_disc(ctx_t *c, const tank_t *t, float cx, float cy, float sc, const uint8_t *tn, int cx_px, int feet) {
    const float r = CR_DISC_R * sc;
    const int phase = ((int)(t->clock * 2)) & 1;
    for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); y++) {
        if (y < 0 || y >= TANK_H) continue;
        const uint32_t w = water_rgb(y);
        src_t gold = src_color(rgb_mix(w, CR_RGB[CRT_SD], CR_TINT), c->dim), dark = src_color(rgb_mix(w, CR_RGB[CRT_SDD], CR_TINT), c->dim), glint = src_color(CR_RGB[CRT_W], c->dim);
        for (int x = (int)floorf(cx - r); x <= (int)ceilf(cx + r); x++) {
            const float ddx = (x + 0.5f - cx) / sc, ddy = (y + 0.5f - cy) / sc, e2 = ddx * ddx + ddy * ddy;
            if (e2 > CR_DISC_R * CR_DISC_R) continue;
            const src_t *s = &gold;
            if (e2 > (CR_DISC_R - 1.0f) * (CR_DISC_R - 1.0f)) s = &dark;                                              /* the rim */
            else if (e2 < 1.3f) s = &dark;                                                                            /* the mark: a dot ... */
            else if (e2 > 4.0f && e2 < 42.0f) {                                                                       /* ... and five petals */
                const float a = atan2f(ddy, ddx), k = fmodf(a + 3.1415926f, 2 * 3.1415926f / 5);
                if (fabsf(k - 3.1415926f / 5) < 0.14f) s = &dark;
            }
            if (phase && ddx > 2.2f && ddx < 5.0f && ddy < -2.2f && ddy > -5.0f && e2 < 30.0f) s = &glint;          /* the glint's beat */
            if (tn) { const int gx = x - cx_px - CR_GX0, gy = y - feet + CR_GH - 1;
                      if (gx >= 0 && gx < CR_GW && gy >= 0 && gy < CR_GH && cr_claw_tone(tn[gy * CR_GW + gx])) continue; }   /* the claw's px: over the coin */
            px_blend_s(c, x, y, s, 255);
        }
    }
}
static uint32_t g_cr_clip;                                /* THE CRAB'S STACK (crab_stack): the pieces before its shelter - it is not painted under them */
static bool cr_clipped(int x, int y);                     /* (below, with the pieces' masks) */
static void draw_crab(ctx_t *c, const tank_t *t) {
    if (g_no_animals) return;
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0) return;
    /* the grid is built only for a pose not drawn lately (0.3.9.1, C1704 on Discord: "lag when the crab walks" - the walk
       rebuilt it on 60% of frames, a run on every one, a few ms each on the 1.8): the last CR_CACHE_N poses keep their
       grids, and a walk cycles through 16 */
    static uint32_t s_clock;
    const int mode = t->cr_mode;
    cr_pose_t p; cr_pose(&p, t);                                      /* cheap: no cell is computed here */
    const uint32_t key = cr_hash(&p);                                 /* (cr_pose quantizes: the same picture, the same key) */
    decor_scratch_t *d = ds();
    int slot = -1, old = 0;
    for (int i = 0; i < CR_CACHE_N; i++) {
        if (d->cr_keys[i] == key && !g_cr_nocache) { slot = i; break; }
        if (d->cr_used[i] < d->cr_used[old]) old = i;
    }
    if (slot < 0) { slot = g_cr_nocache ? 0 : old; cr_build(d->cr_tones[slot], d->cr_parts, &p); d->cr_keys[slot] = key; g_cr_builds++; }
    d->cr_used[slot] = ++s_clock;
    const uint8_t *tn = d->cr_tones[slot];
    g_cr_tn = tn;
    const int f = (t->cr_facing < 0 && mode != CR_ROOST) ? -1 : 1;
    const int feet = (int)floorf(CR_FLOOR_Y - t->cr_y);              /* the bottom row's y */
    /* backed into its shelter's dark (cr_back, tank.c: a motion in and out, never a switch): with it the crab draws
       SMALLER, a few px HIGHER (the cave's floor receding) and DARKER - everything but its eyes' glints and a held
       sand dollar sinks toward black (half dark at the back: a crab in shadow, its glints bright) */
    const float back = t->cr_back;
    float dark = 0.55f * back;
    if (t->cr_tilt == 0 && back <= 0.001f) {
        const int cx = (int)floorf(t->cr_x + 0.5f);
        for (int gy = 0; gy < CR_GH; gy++) {
            const int y = feet - (CR_GH - 1) + gy;
            if (y < 0 || y >= TANK_H) continue;
            const uint8_t *row = tn + gy * CR_GW;
            bool any = false;
            for (int gx = 0; gx < CR_GW; gx++) if (row[gx]) { any = true; break; }
            if (!any) continue;
            const uint32_t w = water_rgb(y);
            src_t pal[CRT_N];
            for (int k = 1; k < CRT_N; k++) {
                uint32_t rgb = rgb_mix(w, CR_RGB[k], CR_TINT);
                if (dark > 0 && k != CRT_W && k != CRT_E && k != CRT_SD && k != CRT_SDD) rgb = rgb_mix(rgb, 0x050304, dark);
                pal[k] = src_color(rgb, c->dim);
            }
            for (int gx = 0; gx < CR_GW; gx++) {
                int tone = row[gx];
                if (tone == CRT_NONE) continue;
                int x = f > 0 ? cx + CR_GX0 + gx : cx - 1 - (CR_GX0 + gx);
                if (g_cr_clip && cr_clipped(x, y)) continue;
                px_blend_s(c, x, y, &pal[tone], 255);
            }
        }
        if (t->cr_find > 0) cr_draw_disc(c, t, cx, (float)(feet - CR_GH) - CR_DISC_UP, 1.0f, tn, cx, feet);   /* the sand dollar held up in both claws */
        return;
    }
    /* on the flank: (cr_x, its feet) is its FOOTING on the rock - the grid's (0, the bottom) - and the grid is
       sampled through a rotation about it by cr_tilt (tank.c: its underside against the slope). Backed into the
       dark the same sampler draws it through a SCALE about its feet (down to 0.82) with the feet lifted (up to 4 px) */
    const float sc = 1.0f - 0.18f * back, lift = 4.0f * back;
    const float ox = t->cr_x, oy = (float)(feet + 1) - lift, ca = cosf(t->cr_tilt), sa = sinf(t->cr_tilt);
    const int R = 34;
    for (int y = (int)oy - R; y <= (int)oy + R; y++) {
        if (y < 0 || y >= TANK_H) continue;
        src_t pal[CRT_N]; bool have = false;
        for (int x = (int)ox - R; x <= (int)ox + R; x++) {
            float dx = x + 0.5f - ox, dy = y + 0.5f - oy;
            float lx = (ca * dx + sa * dy) * f / sc, ly = (-sa * dx + ca * dy) / sc;   /* back into the grid: x from its middle, y up from its feet */
            int gx = (int)floorf(lx) - CR_GX0, gy = (int)floorf(ly) + CR_GH;
            if (gx < 0 || gx >= CR_GW || gy < 0 || gy >= CR_GH) continue;
            int tone = tn[gy * CR_GW + gx];
            if (tone == CRT_NONE) continue;
            if (!have) {
                uint32_t w = water_rgb(y);
                for (int k = 1; k < CRT_N; k++) {
                    uint32_t rgb = rgb_mix(w, CR_RGB[k], CR_TINT);
                    if (dark > 0 && k != CRT_W && k != CRT_E && k != CRT_SD && k != CRT_SDD) rgb = rgb_mix(rgb, 0x050304, dark);
                    pal[k] = src_color(rgb, c->dim);
                }
                have = true;
            }
            if (g_cr_clip && cr_clipped(x, y)) continue;
            px_blend_s(c, x, y, &pal[tone], 255);
        }
    }
    if (t->cr_find > 0) cr_draw_disc(c, t, ox, oy - (CR_GH + CR_DISC_UP) * sc, sc, NULL, 0, 0);   /* the sand dollar over its head, scaled with it (never tilted: a find is a dig's) */
}

/* does the crab, as draw_crab painted it this frame (g_cr_tn: the grid it drew), cover (x, y)?
   The painter's two mappings run backwards: the 1:1 blit, or the sampler's rotation / scale about its feet. */
static bool crab_covers(const tank_t *t, int x, int y) {
    if (!(t->sd_unlocks & SD_ITEM_CRAB) || t->cr_x < 0) return false;
    const int feet = (int)floorf(CR_FLOOR_Y - t->cr_y);
    if (t->cr_find > 0) {                                             /* the sand dollar over its head */
        const float ddx = x + 0.5f - t->cr_x, ddy = y + 0.5f - ((float)(feet - CR_GH) - CR_DISC_UP);
        if (ddx * ddx + ddy * ddy <= CR_DISC_R * CR_DISC_R) return true;
    }
    if (x < (int)t->cr_x - 36 || x > (int)t->cr_x + 36 || y < feet - 36 || y > feet + 36) return false;
    const int f = (t->cr_facing < 0 && t->cr_mode != CR_ROOST) ? -1 : 1;
    const float back = t->cr_back;
    int gx, gy;
    if (t->cr_tilt == 0 && back <= 0.001f) {
        const int cx = (int)floorf(t->cr_x + 0.5f);
        gx = f > 0 ? x - cx - CR_GX0 : cx - 1 - x - CR_GX0;
        gy = y - feet + CR_GH - 1;
    } else {
        const float sc = 1.0f - 0.18f * back, lift = 4.0f * back;
        const float ox = t->cr_x, oy = (float)(feet + 1) - lift, ca = cosf(t->cr_tilt), sa = sinf(t->cr_tilt);
        float dx = x + 0.5f - ox, dy = y + 0.5f - oy;
        float lx = (ca * dx + sa * dy) * f / sc, ly = (-sa * dx + ca * dy) / sc;
        gx = (int)floorf(lx) - CR_GX0; gy = (int)floorf(ly) + CR_GH;
    }
    if (gx < 0 || gx >= CR_GW || gy < 0 || gy >= CR_GH) return false;
    return g_cr_tn && g_cr_tn[gy * CR_GW + gx] != CRT_NONE;
}
/* is (x, y) one of the front slot's critters' (g_guard_who), as painted this frame? */
static bool slot_covers(int x, int y) {
    return ((g_guard_who & GUARD_CRAB) && crab_covers(g_guard, x, y)) || ((g_guard_who & GUARD_SNAIL) && snail_covers(g_guard, x, y))
        || ((g_guard_who & GUARD_URCHIN) && urchin_covers(g_guard, x, y));
}

/* ---- the seahorse (0.3.6; the prototype is working-assets/seahorse/
 * seahorse_proto.py, built from Strato's sprite - the local frame IS that
 * sprite's 23 x 39 grid, facing right, 1 cell = 1 px; the same geometry +
 * tone rule as the snail and the castle, lit from the upper left, grain
 * from a position hash, tinted with the row's water). The tail is a sampled
 * spine - a lead-in and a spiral - stamped into a distance map, so its rings
 * and edges come from the nearest sample instead of a search per pixel. Round
 * a blade the coil is centred on the blade's spine and the upper half of
 * every loop is BEHIND it: those pixels are skipped where they land on the
 * ribbon, so the blade shows through the coil, and a contact shadow sits on
 * the ribbon under each front crossing. That is the wrap (Strato: "it's
 * important we get the tail-wrapping and anchoring right"). */
#define SH_W 23
#define SH_H 39
#define SH_PTS_MAX 140
enum { SH_FREE = 0, SH_ONBLADE = 1, SH_GRIP = 2 };   /* the tail's pose: the open curl, round a blade, the tight grip */
enum { SHT_0, SHT_1, SHT_2, SHT_3, SHT_4, SHT_5, SHT_EYE, SHT_GLINT, SHT_FINM, SHT_FINR, SHT_N };
static const uint32_t SH_RGB[SHT_N]   = { 0xfff1b8, 0xfcd055, 0xfa9b19, 0xf3640e, 0xcb2f15, 0x8e1e0b, 0x1b3a63, 0xe4ecff, 0xffd98a, 0xf3640e };
static const uint8_t  SH_ALPHA[SHT_N] = { 255, 255, 255, 255, 255, 255, 255, 255, 150, 190 };
#define SH_TINT 0.82f
#define SHB_ON     0x80        /* a sprite byte: bits 0-3 the tone, bit 4 behind the blade, bit 5 the tail, bit 7 a pixel at all */
#define SHB_BEHIND 0x10
#define SHB_TAIL   0x20
static inline float sh_seg(float px, float py, float x0, float y0, float x1, float y1, float *u) {
    float vx = x1 - x0, vy = y1 - y0, L2 = vx * vx + vy * vy; if (L2 < 1e-6f) L2 = 1e-6f;
    float uu = ((px - x0) * vx + (py - y0) * vy) / L2; uu = uu < 0 ? 0 : uu > 1 ? 1 : uu;
    float dx = px - (x0 + uu * vx), dy = py - (y0 + uu * vy);
    if (u) *u = uu;
    return sqrtf(dx * dx + dy * dy);
}
static inline float sh_tail_hw(float frac) { return 2.4f - 1.8f * powf(frac, 0.8f); }
/* the tail's spine: a bezier lead-in from the body's base and a spiral; on a
 * blade the spiral is centred on the blade (local x SX, half-width SW) and
 * the upper half of each loop is flagged behind it */
/* the three poses' spirals (the coil's centre, radii, turns, squash, the
 * lead-in's control point); a hold is reached by LERPING the open curl's
 * numbers into the hold's over u = 0..1 (tank_t.sh_land / SH_LAND_S), so
 * the tail closes round a blade or a finger and opens again instead of
 * snapping between two drawings (Strato 10-15) */
typedef struct { float cx, cy, r0, r1, turns, ky, c0x, c0y; } sh_spiral_t;
static void sh_spiral(sh_spiral_t *o, int pose, float SX, float SW) {
    if (pose == SH_ONBLADE)   { o->cx = SX; o->cy = 33.4f; o->r0 = SW + 3.2f; o->r1 = SW + 0.5f; o->turns = 2.05f; o->ky = 0.86f; o->c0x = SX - o->r0 - 0.4f; o->c0y = 29.2f; }
    else if (pose == SH_GRIP) { o->cx = 10.5f; o->cy = 32.6f; o->r0 = 4.6f; o->r1 = 0.9f; o->turns = 2.1f; o->ky = 0.9f; o->c0x = 5.0f; o->c0y = 29.4f; }
    else                      { o->cx = 11.0f; o->cy = 33.2f; o->r0 = 6.2f; o->r1 = 1.3f; o->turns = 1.55f; o->ky = 0.78f; o->c0x = 4.8f; o->c0y = 29.6f; }
}
static inline float sh_lerp(float a, float b, float u) { return a + (b - a) * u; }
static int sh_tail_path(decor_scratch_t *sc, int pose, float u, float t, float SX, float SW) {
    sh_spiral_t a, b; sh_spiral(&a, SH_FREE, SX, SW); sh_spiral(&b, pose, SX, SW);
    float cx = sh_lerp(a.cx, b.cx, u), cy = sh_lerp(a.cy, b.cy, u), r0 = sh_lerp(a.r0, b.r0, u), r1 = sh_lerp(a.r1, b.r1, u);
    float turns = sh_lerp(a.turns, b.turns, u), ky = sh_lerp(a.ky, b.ky, u), c0x = sh_lerp(a.c0x, b.c0x, u), c0y = sh_lerp(a.c0y, b.c0y, u);
    bool wrap = pose == SH_ONBLADE && u > 0.5f;                          /* past half way the blade is inside the coil */
    float breathe = 0.18f * sinf(t * 1.1f);
    const float P0x = 7.0f, P0y = 26.0f, P2x = cx - r0, P2y = cy;
    int n = 0; float s = 0, lx = 0, ly = 0;
    for (int i = 0; i < 14; i++) {
        float u = i / 13.0f, a = (1 - u) * (1 - u), b = 2 * (1 - u) * u, c = u * u;
        float x = a * P0x + b * c0x + c * P2x, y = a * P0y + b * c0y + c * P2y;
        if (i) s += sqrtf((x - lx) * (x - lx) + (y - ly) * (y - ly));
        sc->sh_pts[n][0] = x; sc->sh_pts[n][1] = y; sc->sh_pts[n][2] = s; sc->sh_behind[n] = 0; n++; lx = x; ly = y;
    }
    float th_max = turns * 2 * 3.14159265f; int m = (int)(th_max / 0.17f);
    for (int i = 1; i <= m && n < SH_PTS_MAX; i++) {
        float th = th_max * i / m, f = th / th_max;
        float r = r0 + (r1 - r0) * powf(f, 0.9f) + breathe * f;
        float sn = sinf(th), x = cx - r * cosf(th), y = cy + ky * r * sn;
        s += sqrtf((x - lx) * (x - lx) + (y - ly) * (y - ly));
        sc->sh_pts[n][0] = x; sc->sh_pts[n][1] = y; sc->sh_pts[n][2] = s; sc->sh_behind[n] = wrap && sn < 0; n++; lx = x; ly = y;
    }
    return n;
}
/* the body, row-wise: the spine's x and the half-width of the bean at a row */
static bool sh_body(float ly, float *sx, float *w, float *tb) {
    if (ly < 13.2f || ly > 26.4f) return false;
    float tt = (26.0f - ly) / 12.8f; tt = tt < 0 ? 0 : tt > 1 ? 1 : tt;
    float sp = sinf(3.14159265f * tt);
    *sx = 7.0f + 1.0f * tt + 1.6f * sp; *w = 3.0f + 3.0f * powf(sp, 0.75f); *tb = tt;
    return true;
}
/* one local pixel's byte (0 = water). nod and the row's body numbers come
 * from the caller (once a frame / once a row, not once a pixel: the S3 has
 * no hardware sine or power) */
typedef struct { bool body; float sx, w, tb; } sh_row_t;
static uint8_t sh_tone(float lx, float ly, float t, float fin, const decor_scratch_t *sc, int k, float nod, const sh_row_t *row, float jut) {
    const float ex = 13.2f + jut, ey = 9.8f + nod;                        /* the snatch: the head and the snout jut forward, the neck stretches */
    const bool head_rows = ly < 15.5f;                                     /* the head, its crown, the snout and the eye live above row 15:
                                                                            * the body and tail rows skip their seven distance tests */
    float de = head_rows ? sqrtf((lx - ex) * (lx - ex) + (ly - ey) * (ly - ey)) : 99;
    if (de <= 1.75f) {                                                     /* the eye, its glint upper-left */
        float gx = lx - (ex - 0.7f), gy = ly - (ey - 0.7f);
        return SHB_ON | (gx * gx + gy * gy < 0.49f ? SHT_GLINT : SHT_EYE);
    }
    float u = 0, d = head_rows && lx > 13 ? sh_seg(lx, ly, 15.0f + jut, 11.0f + nod, 21.6f + jut, 12.5f + nod, &u) : 99;   /* the snout */
    float hw = 1.55f - 0.5f * u;
    if (d <= hw) {
        int tone = SHT_2;
        if (ly > 11.0f + nod + 0.6f + u * 1.2f) tone = SHT_3;
        if (d > hw - 0.7f && ly > 11.0f + nod) tone = SHT_4;
        if (u > 0.9f) tone = SHT_4;
        if ((chash((int)lx, (int)ly) & 7) == 0) tone = clampi(tone + 1, 1, 5);
        return SHB_ON | tone;
    }
    /* the coronet: short blunt nubs, the middle one tallest (Strato 10-15: the
     * first pass's sharp points read as spikes); one short cheek point */
    static const float SPIKES[5][5] = { { 8.9f, 4.0f, 8.9f, 1.7f, 1.25f }, { 10.3f, 4.0f, 10.5f, 2.0f, 1.15f }, { 6.8f, 4.6f, 6.2f, 3.3f, 0.95f },
                                        { 12.6f, 4.6f, 13.3f, 3.5f, 0.85f }, { 5.2f, 7.6f, 3.6f, 7.4f, 0.9f } };
    if (ly < 10.5f) for (int i = 0; i < 5; i++) {
        const float *q = SPIKES[i];
        d = sh_seg(lx, ly, q[0] + jut, q[1] + nod, q[2] + jut, q[3] + nod, &u);
        if (d <= q[4] * (1 - 0.15f * u)) return SHB_ON | ((u > 0.75f || d > q[4] * 0.55f) ? SHT_3 : SHT_2);
    }
    float dx = (lx - 10.4f - jut) / 5.5f, dy = (ly - 8.5f - nod) / 5.4f, r = head_rows ? sqrtf(dx * dx + dy * dy) : 99;   /* the head: a round ellipse */
    if (r <= 1.0f) {
        float lit = -dx * 0.6f - dy * 0.8f;
        int tone = SHT_2;
        if (lit > 0.45f) tone = SHT_1;
        if (lit < -0.45f) tone = SHT_3;
        if (r > 0.82f && lit < 0.1f) tone = SHT_4;
        if (de < 2.75f && (lx < ex || ly < ey)) tone = SHT_1;            /* the pale ring over the eye */
        if ((chash((int)lx, (int)ly) & 7) == 0) tone = clampi(tone + 1, 0, 5);
        return SHB_ON | tone;
    }
    d = ly > 10 && ly < 17.5f && lx > 4 && lx < 13 + jut ? sh_seg(lx, ly, 8.6f + jut, 11.8f + nod, 8.2f, 14.2f, &u) : 99;   /* the throat joins head and body (its top goes with the head) */
    bool throat = d <= 3.1f;
    float sx = row->sx, w = row->w;
    bool body = row->body;
    if (body || throat) {
        float un, ridge; bool inside;
        if (body) {
            un = (lx - sx) / w; inside = fabsf(un) <= 1.0f;
            ridge = fmodf(ly - 14.0f + un * 1.2f + 40.0f, 4.0f);
            if (!inside && un < 0 && fabsf(un) <= 1.0f + 1.3f / w && ridge < 1.4f) return SHB_ON | SHT_4;   /* a dorsal bump */
        } else { sx = 8.4f; w = 3.1f; un = (lx - 8.4f) / 3.1f; inside = true; ridge = 9; }
        if (inside) {
            int tone = SHT_2;
            if (un > 0.28f) {                                              /* the belly plates */
                tone = SHT_1;
                if (fmodf(ly - 15.0f + 30.0f, 3.0f) < 0.9f) tone = SHT_2;
                if (un > 0.86f) tone = SHT_3;
            } else {
                if (ridge < 1.0f) tone = SHT_3;                            /* the ring ridges */
                if (un < -0.78f) tone = SHT_4; else if (un < -0.45f) tone = SHT_3;
                if (un > -0.45f && un < 0.1f && ridge > 1.6f && ly < 20) tone = SHT_2;
            }
            if ((chash((int)lx, (int)ly + 7) & 7) == 0) tone = clampi(tone + 1, 0, 5);
            return SHB_ON | tone;
        }
    }
    float vx = lx - 5.2f, vy = ly - 20.3f, rr = lx < 11.5f && ly > 13.5f && ly < 27.5f ? sqrtf(vx * vx + vy * vy) : 0;   /* the dorsal fin: a fan of rays, fluttering (its corner only) */
    if (rr > 0.6f) {
        float ang = atan2f(vy, vx) * (180.0f / 3.14159265f); if (ang < 0) ang += 360;
        if (ang >= 118 && ang <= 242) {
            float a = (ang - 180) / 62.0f;
            float L = 5.6f * (1 - 0.45f * a * a) + 0.5f * sinf(fin + a * 2.5f);
            if (rr <= L) {
                float ray = fmodf((ang - 180) / 14.0f + 0.25f * sinf(fin + rr) + 8.0f, 1.0f);
                return SHB_ON | ((ray < 0.3f || rr > L - 0.8f) ? SHT_FINR : SHT_FINM);
            }
        }
    }
    if (sc->sh_d[k] != 255) {                                              /* the tail, from the distance map */
        float dd = sc->sh_d[k] / 32.0f, frac = sc->sh_fr[k] / 255.0f;
        hw = sh_tail_hw(frac);
        if (dd <= hw) {
            int tone = SHT_2;
            if (fmodf(frac * 60.0f, 2.6f) < 0.8f) tone = SHT_3;             /* the rings down the tail */
            if (dd > hw - 0.75f) tone = (sc->sh_fl[k] & 2) ? SHT_4 : SHT_3;  /* the shaded edge */
            if (frac > 0.93f) tone = SHT_4;
            if ((chash((int)lx, (int)ly + 3) & 7) == 0) tone = clampi(tone + 1, 0, 5);
            return SHB_ON | SHB_TAIL | ((sc->sh_fl[k] & 1) ? SHB_BEHIND : 0) | tone;
        }
    }
    return 0;
}
/* the sprite for this frame: the tail stamped into the distance map first,
 * then a byte per pixel */
static void sh_sprite(decor_scratch_t *sc, uint8_t *out, float t, int pose, float u, float SX, float SW, float fin, float jut) {   /* t: tank_anim's; fin: the dorsal fin's phase */
    int n = sh_tail_path(sc, pose, u, t, SX, SW);
    float L = sc->sh_pts[n - 1][2];
    memset(sc->sh_d, 255, SH_W * SH_H);
    float hwb = sh_tail_hw(0);
    for (int i = 0; i + 1 < n; i++) {
        const float *a = sc->sh_pts[i], *b = sc->sh_pts[i + 1];
        float hwa = hwb; hwb = sh_tail_hw(b[2] / L);
        float hw = hwa + 0.6f;
        int x0 = clampi((int)floorf(fminf(a[0], b[0]) - hw), 0, SH_W - 1), x1 = clampi((int)ceilf(fmaxf(a[0], b[0]) + hw), 0, SH_W - 1);
        int y0 = clampi((int)floorf(fminf(a[1], b[1]) - hw), 0, SH_H - 1), y1 = clampi((int)ceilf(fmaxf(a[1], b[1]) + hw), 0, SH_H - 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f, u;
                float d = sh_seg(px, py, a[0], a[1], b[0], b[1], &u);
                float frac = (a[2] + (b[2] - a[2]) * u) / L;
                if (d > hwa + (hwb - hwa) * u + 0.6f) continue;
                int di = (int)(d * 32); if (di > 254) di = 254;
                int k = y * SH_W + x;
                if (di < sc->sh_d[k]) {
                    float cross = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                    sc->sh_d[k] = (uint8_t)di; sc->sh_fr[k] = (uint8_t)(frac * 255);
                    sc->sh_fl[k] = (uint8_t)((sc->sh_behind[i] ? 1 : 0) | (cross > 0 ? 2 : 0));
                }
            }
    }
    float nod = 0.35f * sinf(t * 1.3f);
    for (int y = 0; y < SH_H; y++) {
        sh_row_t row; row.body = sh_body(y + 0.5f, &row.sx, &row.w, &row.tb);
        for (int x = 0; x < SH_W; x++) out[y * SH_W + x] = sh_tone(x + 0.5f, y + 0.5f, t, fin, sc, y * SH_W + x, nod, &row, jut);
    }
}
/* the sprite is rebuilt at ~10 Hz, not every frame (its animation - the nod,
 * the fin, the tail's breathing - is slow; a landing's morph is 0.7 s), and
 * at once when the pose or the morph moves: the build is the dear part on
 * the S3 (a sine, a power or a root for most pixels) */
static float g_sh_built_t = -1, g_sh_built_u = -1, g_sh_built_sw = -1, g_sh_built_jut = 0; static int g_sh_built_pose = -1;
static void sh_sprite_cached(decor_scratch_t *sc, float t, int pose, float u, float SX, float SW, float fin, float jut) {
    bool stale = g_sh_built_pose != pose || fabsf(g_sh_built_u - u) > 0.04f || fabsf(g_sh_built_sw - SW) > 0.1f
              || fabsf(g_sh_built_jut - jut) > 0.4f || t - g_sh_built_t >= 0.1f || t < g_sh_built_t;
    if (!stale) return;
    sh_sprite(sc, sc->sh_spr, t, pose, u, SX, SW, fin, jut);
    g_sh_built_t = t; g_sh_built_u = u; g_sh_built_sw = SW; g_sh_built_pose = pose; g_sh_built_jut = jut;
}
/* a blade's ribbon at a tank row, exactly as draw_veg lays it (its sway and
 * taper): the centre x and the half-width; false off the blade */
static bool veg_ribbon_at(const tank_t *t, int b, int i, int y, float moment, float *cx, float *half) {   /* moment: veg_clock's */
    float bx; int segs = veg_segs_of(t, b, i, &bx);   /* (the held heights: the blade as drawn) */
    if (segs < 1) segs = 1;
    if (segs > VEG_MAX_SEGS) segs = VEG_MAX_SEGS;
    bool sword = tank_veg_kind(t, b) == VEG_KIND_SWORD;
    int seed = VEG_SEED[b];
    const float vc = moment * VEG_W;             /* (draw_veg's sway, exactly) */
    float sway = fast_sin(vc + (i + seed) * 1.7f) * (sword ? 2.5f : 4.0f);
    if (g_veg_step == 2) { int yb = (int)(TANK_BOT - 16 + VEG_SEG_RY); y = yb - ((yb - y) & ~1); }   /* row pairs: the pair's lower row, as draw_veg paints it */
    float sp = (TANK_BOT - 16 - y) / VEG_SEG_DY;
    if (sp < -0.7f || sp > segs - 1 + 0.7f) return false;
    if (sp < 0) sp = 0;
    if (sp > segs - 1) sp = (float)(segs - 1);
    int k = (int)sp, k1 = k + 1 > segs ? segs : k + 1; float fr = sp - k;
    float xk  = bx + sway * k  / (float)segs * fast_sin(k  * 0.4f + vc + i + seed);
    float xk1 = bx + sway * k1 / (float)segs * fast_sin(k1 * 0.4f + vc + i + seed);
    *cx = xk + (xk1 - xk) * fr;
    float u = sp / (segs > 1 ? segs - 1 : 1);
    *half = sword ? 1.2f + 6.0f * (u < 0.4f ? u / 0.4f : (1 - u) / 0.6f) : fmaxf(0.4f, 2.4f - 2.0f * u);
    return true;
}
/* its pitch this frame, radians (the renderer's rule, below): forward with its speed */
static float seahorse_pitch(const tank_t *t) {
    if (t->sh_mode == SH_PERCH && t->sh_akind != SH_ANCHOR_NONE) return 0;
    float speed = sqrtf(t->sh_vx * t->sh_vx + t->sh_vy * t->sh_vy);
    float f = fabsf(t->sh_vx) / 42.0f + speed / 120.0f; if (f > 1) f = 1;
    return 30.0f * f * 3.14159265f / 180.0f;
}
/* its box this frame (the draw's loop and the dynamic rect): the sprite's
 * rectangle through its pitch about the anchor, plus the lean, the bob and a
 * margin - upright (most of the time) that is the 23 x 39 cells and little
 * more, not a box sized for the full pitch */
static void seahorse_box(const tank_t *t, int *x0, int *y0, int *x1, int *y1) {
    float th = seahorse_pitch(t), cs = cosf(th), sn = fabsf(sinf(th));
    float hw = (SH_W * 0.5f + 1) * cs + (SH_H * 0.5f) * sn + 6;        /* the rotated rect's half extents, about its middle */
    float hh = (SH_H * 0.5f + 1) * cs + (SH_W * 0.5f) * sn + 4;
    float cx = t->sh_x, cy = t->sh_y - 33.0f + SH_H * 0.5f;             /* its middle: the anchor row is ~33 cells down */
    *x0 = (int)floorf(cx - hw); *x1 = (int)ceilf(cx + hw);
    *y0 = (int)floorf(cy - hh); *y1 = (int)ceilf(cy + hh);
}
/* the pitch (Strato 10-15, from real seahorses: "sometimes the head will
 * pitch forward when it wants speed; slower movements are more vertical and
 * erect"): the body tips forward with its speed through the water, up to
 * SH_PITCH_MAX degrees at a full swim, about its middle, and stands up
 * again as it eases into a spot. On a blade and at the finger it is erect. */
#define SH_PITCH_MAX 30.0f
#define SH_PIVOT_X 9.0f
#define SH_PIVOT_Y 19.0f
static void draw_seahorse(ctx_t *c, const tank_t *t) {
    if (g_no_animals) return;
    if (!(t->sd_unlocks & SD_ITEM_SEAHORSE) || t->sh_x < 0 || t->sh_hidden) return;
    decor_scratch_t *sc = ds();
    bool held = t->sh_mode == SH_PERCH && t->sh_akind != SH_ANCHOR_NONE;
    bool onblade = held && t->sh_akind == SH_ANCHOR_BLADE;             /* the one anchor that sways */
    bool onpost = held && t->sh_akind == SH_ANCHOR_POST;
    bool onsand = held && t->sh_akind == SH_ANCHOR_SAND;
    /* the hold it is closing on (or opening from): the pose and how far along.
     * On the sand the open curl IS the pose: the tail curls under it. */
    int pose = t->sh_pose == SH_POSE_GRIP ? SH_GRIP : onsand ? SH_FREE : SH_ONBLADE;
    float u = t->sh_land / SH_LAND_S; u = u < 0 ? 0 : u > 1 ? 1 : u;
    u = u * u * (3 - 2 * u);                                             /* eased */
    const float an = anim(t), vm = veg_clock(t->clock);                 /* (once a frame: the S3 does a double's fmod in software) */
    float SX = 5.0f, SW = 1.5f, ax = t->sh_x, ay = t->sh_y, lean = 1.2f * fast_sin(an * 0.7f), pitch = 0;
    float speed = sqrtf(t->sh_vx * t->sh_vx + t->sh_vy * t->sh_vy);
    float flutter = t->sh_mode == SH_HITCH && !t->sh_hitched ? 1.6f : speed > 2 ? 1.0f + speed / 40.0f : 1.0f;
    float free_u = held ? 1 - u : 1;                                     /* how much of the open water is still in it */
    if (onblade) {                                                       /* the blade's sway and lean come in with the hold */
        float cx0 = ax, h0 = SW, cx1 = ax, h1;
        if (veg_ribbon_at(t, t->sh_bed, t->sh_frond, (int)ay, vm, &cx0, &h0)) { ax += (cx0 - ax) * u; SW = fmaxf(1.0f, fminf(3.5f, h0)); }
        veg_ribbon_at(t, t->sh_bed, t->sh_frond, (int)ay, veg_clock(t->clock + 0.05f), &cx1, &h1);
        float l = (cx1 - cx0) / 0.05f * 0.9f; l = l < -3 ? -3 : l > 3 ? 3 : l;
        lean = lean * (1 - u) + l * u;
    } else if (onpost) { SW = fmaxf(1.0f, fminf(3.5f, t->sh_ahw)); lean *= 1 - u; }   /* a post stands still: the coil hugs its middle */
    else if (onsand) lean *= 1 - u * 0.6f;
    if (free_u > 0) {                                                    /* off a blade it bobs in the water and pitches with speed */
        ay += free_u * 1.6f * fast_sin(an * 1.7f); ax += free_u * 0.8f * fast_sin(an * 0.9f + 1.0f);
        float f = fabsf(t->sh_vx) / 42.0f + speed / 120.0f; if (f > 1) f = 1;
        pitch = SH_PITCH_MAX * f * 3.14159265f / 180.0f;                /* forward = the way it faces */
    }
    sh_spiral_t pa, pb; sh_spiral(&pa, SH_FREE, SX, SW); sh_spiral(&pb, pose, SX, SW);
    float coilx = sh_lerp(pa.cx, pb.cx, u), coily = sh_lerp(pa.cy, pb.cy, u);
    static double s_fin_at = -1; static float s_fin;                  /* the dorsal fin's flutter: its own phase, so a change of speed */
    float fdt = s_fin_at >= 0 && t->clock > s_fin_at ? (float)(t->clock - s_fin_at) : 0;   /* changes its pace and never jumps it */
    s_fin_at = t->clock; s_fin = fmodf(s_fin + (fdt < 0.5f ? fdt : 0) * 9.0f * flutter, TAU);
    sh_sprite_cached(sc, an, pose, u, SX, SW, s_fin, tank_seahorse_jut(t));
    int fac = t->sh_facing >= 0 ? 1 : -1;
    /* local (mirrored) -> tank: p = (lxm - coilx_m, ly - coily) sheared by the lean, rotated
     * by the pitch about the body's middle, then the anchor. Pitched, it is drawn by the
     * INVERSE: every tank pixel of the box maps back to a local cell, so a rotated sprite
     * has no holes. UPRIGHT (held: most of the time) each row is a whole-pixel shift of the
     * cells, and that shift has HYSTERESIS: a row keeps its snapped place until the true
     * one has pulled 0.75 px away (Strato 10-15: with the slow sway a row hovered at a
     * half-pixel and "fought between two pixels" while its neighbours stayed put). */
    float coilx_m = fac > 0 ? coilx : SH_W - coilx, pivx_m = (fac > 0 ? SH_PIVOT_X : SH_W - SH_PIVOT_X) - coilx_m, pivy = SH_PIVOT_Y - coily;
    float th = fac > 0 ? pitch : -pitch, cs = cosf(th), sn = sinf(th);
    int bx0, by0, bx1, by1; seahorse_box(t, &bx0, &by0, &bx1, &by1);
    src_t pal[SHT_N]; int pal_y = -1;
    bool upright = pitch == 0;
    if (upright) {                                                       /* the row offset, snapped with hysteresis */
        float want = ay - coily;
        if (fabsf(want - sc->sh_yoff) > 0.75f) sc->sh_yoff = roundf(want);
        if (sc->sh_yoff == 0 && want != 0) sc->sh_yoff = roundf(want);  /* the first frame */
    }
    for (int py = by0; py <= by1; py++) {
        if (!CTX_IN(c, bx0 + 1, py) && !CTX_IN(c, bx1 - 1, py)) continue;
        float rcx = 0, rhalf = 0; bool ribbon = onblade ? veg_ribbon_at(t, t->sh_bed, t->sh_frond, py, vm, &rcx, &rhalf)
                                              : onpost && fabsf(py - t->sh_ay) <= 14 && (rcx = t->sh_ax, rhalf = t->sh_ahw, true);
        int row_ly = -1, row_xs = 0;
        if (upright) {
            row_ly = py - (int)sc->sh_yoff;                               /* this row's cell row */
            if (row_ly < 0 || row_ly >= SH_H) continue;
            float want = ax - coilx_m + lean * (33.0f - (row_ly + 0.5f)) / 33.0f;   /* where the row's cell 0 lands, exactly */
            if (fabsf(want - sc->sh_rowx[row_ly]) > 0.75f) sc->sh_rowx[row_ly] = roundf(want);
            row_xs = (int)sc->sh_rowx[row_ly];
        }
        for (int px = bx0; px <= bx1; px++) {
            if (!CTX_IN(c, px, py)) continue;
            int ilx, ily;
            if (upright) {
                int lxm = px - row_xs; ily = row_ly;
                if (lxm < 0 || lxm >= SH_W) continue;
                ilx = fac > 0 ? lxm : SH_W - 1 - lxm;
            } else {
                float qx = px + 0.5f - ax - pivx_m, qy = py + 0.5f - ay - pivy;          /* about the pivot */
                float rx = qx * cs + qy * sn + pivx_m, ry = -qx * sn + qy * cs + pivy;   /* un-rotated */
                float ly = ry + coily;
                float lxm = rx - lean * (33.0f - ly) / 33.0f + coilx_m;                  /* un-sheared */
                ilx = (int)floorf(fac > 0 ? lxm : SH_W - lxm); ily = (int)floorf(ly);
                if (ilx < 0 || ilx >= SH_W || ily < 0 || ily >= SH_H) continue;
            }
            uint8_t b = sc->sh_spr[ily * SH_W + ilx];
            if (!(b & SHB_ON)) continue;
            bool on_ribbon = ribbon && fabsf(px + 0.5f - rcx) <= rhalf + 0.3f;
            if ((b & SHB_BEHIND) && on_ribbon) continue;                   /* the blade shows through the coil */
            int tone = b & 15;
            if (pal_y != py) {                                             /* this row's tones, tinted with its water */
                uint32_t w = water_rgb(py < 0 ? 0 : py >= TANK_H ? TANK_H - 1 : py);
                for (int k = 0; k < SHT_N; k++) pal[k] = src_color(rgb_mix(w, SH_RGB[k], SH_TINT), c->dim);
                pal_y = py;
            }
            px_blend_s(c, px, py, &pal[tone], SH_ALPHA[tone]);
            /* a contact shadow on the ribbon under a front crossing of the tail */
            if ((b & SHB_TAIL) && on_ribbon && ily > 27 && ily + 1 < SH_H && !(sc->sh_spr[(ily + 1) * SH_W + ilx] & SHB_TAIL)) {
                float r2cx = rcx, r2half = rhalf;
                if ((onpost || veg_ribbon_at(t, t->sh_bed, t->sh_frond, py + 1, vm, &r2cx, &r2half)) && fabsf(px + 0.5f - r2cx) <= r2half + 0.3f)
                    px_blend(c, px, py + 1, 0x000000, 115);
            }
        }
    }
}

/* where the castle stands this frame: its centre x (-1 = not bought), its
 * depth, and whether the keeper is dragging it on the placement page (then
 * it is drawn live over a scene baked WITHOUT it, instead of a rebake per
 * frame) */
/* the coral likewise (2026-09-23): BEHIND it is baked into the scene; AMONG
 * and IN FRONT it is drawn every frame (a 60 x 92 sprite - cheap) */

/* ---- the reef cluster, procedural (2026-09-24; Strato's coral-cluster.png:
 * a branching coral, three purple tube sponges, a cyan brain coral and green
 * weed on a pile of grey stones). The same idiom as the coral, bigger: a
 * CL_CW x CL_CH grid of 2 px cells, five ELEMENTS drawn in painter's order
 * (rock, weed, the coral, the tubes, the brain, weed in front), each classed
 * by its own signed distance - rim below / right, lit above / left, a deep
 * variant per element (moss on the rock, the coral's tips, the tube mouths,
 * the brain's grooves, the weed's tips) - into one tone sprite. The size is
 * the growth's first phase: CLUSTER_SIZE_MIN of full on the day it is bought
 * (it must look mature at once), full at 1. The look is a scheme (three
 * presets: the coral / the tubes / the brain each), tinted per row. */
#define CL_CELL 2
#define CL_W    (CL_CW * CL_CELL)
#define CL_FY   (TANK_BOT - 14 + DECOR_SINK + g_dz_cl)   /* on its depth's line (0.3.9) */
#define CL_Q    64                                           /* sprite rebuilds over the size phase */
enum { CLE_ROCK, CLE_WEED, CLE_CORAL, CLE_TUBE, CLE_BRAIN, CLE_N };
enum { CLV_BODY, CLV_LIT, CLV_RIM, CLV_SHADE, CLV_DEEP, CLV_N };
#define CL_TONE(e, v) (1 + (e) * CLV_N + (v))
static int      g_cl_q = -1, g_cl_cells;
static int      g_cl_scheme = -1; static float g_cl_dim = -1; static int g_cl_dzk = 99;
/* geometry in unscaled cells: x from the left, y UP from the base row; the
 * size scales it about (36, 0) */
#define CL_CX 36.0f
typedef struct { float x0, y0, x1, y1, r; } cl_cap_t;
static const cl_cap_t CL_ROCKS[] = {                         /* stones, as capsules (a circle = a zero-length one) */
    { 8, 3, 8, 3, 6 }, { 20, 4, 20, 4, 7 }, { 33, 3, 33, 3, 6 }, { 46, 4, 46, 4, 7 }, { 58, 3, 58, 3, 6 }, { 66, 4, 66, 4, 5 },
    { 14, 7, 14, 7, 4 }, { 40, 7, 40, 7, 4 }, { 52, 8, 52, 8, 4 }, { 4, 6, 70, 6, 2.5f },
};
static const cl_cap_t CL_TUBES[] = {                         /* back to front */
    { 45, 5, 44, 22, 4.5f }, { 61, 4, 63, 27, 5.0f }, { 52, 4, 53, 34, 5.5f }, { 58, 3, 59, 13, 3.5f },
};
static const cl_cap_t CL_BRAIN[] = {
    { 35, 9, 35, 9, 7 }, { 28, 7, 28, 7, 5.5f }, { 42, 7, 42, 7, 5.5f }, { 35, 14, 35, 14, 5 }, { 31, 12, 31, 12, 4 }, { 39, 12, 39, 12, 4 },
};
static const cl_cap_t CL_WEED_BACK[] = {
    { 13, 3, 11, 11, 1.1f }, { 14, 3, 15, 12, 1.1f }, { 12, 3, 9, 9, 1.0f },
    { 30, 3, 29, 10, 1.1f }, { 31, 3, 33, 11, 1.0f },
    { 47, 3, 48, 12, 1.1f }, { 46, 3, 44, 10, 1.0f },
    { 66, 3, 68, 11, 1.1f }, { 65, 3, 63, 9, 1.0f }, { 67, 3, 67, 12, 1.0f },
};
static const cl_cap_t CL_WEED_FRONT[] = {
    { 21, 2, 19, 9, 1.1f }, { 22, 2, 24, 8, 1.0f }, { 41, 2, 43, 8, 1.0f }, { 40, 2, 39, 7, 1.0f }, { 56, 2, 57, 8, 1.0f },
};
#define CL_CORAL_X  24.0f                                    /* the branching coral's base, and its scale */
#define CL_CORAL_Y  5.0f
#define CL_CORAL_SX 0.95f
#define CL_CORAL_SY 0.85f
static inline float cl_cap_sd(float px, float py, const cl_cap_t *k, float rs) {
    float dx = k->x1 - k->x0, dy = k->y1 - k->y0, len2 = dx * dx + dy * dy;
    float u = len2 > 1e-6f ? ((px - k->x0) * dx + (py - k->y0) * dy) / len2 : 0;
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    float ex = px - (k->x0 + dx * u), ey = py - (k->y0 + dy * u);
    return sqrtf(ex * ex + ey * ey) - k->r * rs;
}
/* an element's signed distance at an unscaled point; *deep = the element's
 * deep variant here (a tube's mouth, a brain groove, the coral's tip cap) */
static float cl_sd(int e, int pass, float px, float py, bool *deep) {
    float best = 1e9f; *deep = false;
    if (e == CLE_ROCK) {
        for (int i = 0; i < (int)(sizeof CL_ROCKS / sizeof CL_ROCKS[0]); i++) { float d = cl_cap_sd(px, py, &CL_ROCKS[i], 1); if (d < best) best = d; }
        *deep = best < -1.5f && py > 4 && (chash((int)(px * 0.7f), (int)(py * 0.7f)) % 100) < 22;   /* moss between the stones */
    } else if (e == CLE_WEED) {
        const cl_cap_t *w = pass ? CL_WEED_FRONT : CL_WEED_BACK; int n = pass ? (int)(sizeof CL_WEED_FRONT / sizeof w[0]) : (int)(sizeof CL_WEED_BACK / sizeof w[0]);
        for (int i = 0; i < n; i++) { float d = cl_cap_sd(px, py, &w[i], 1); if (d < best) best = d;
            float tx = px - w[i].x1, ty = py - w[i].y1; if (tx * tx + ty * ty < 2.2f) *deep = true; }
    } else if (e == CLE_CORAL) {
        float qx = (px - CL_CORAL_X) / CL_CORAL_SX + 16.0f, qy = (py - CL_CORAL_Y) / CL_CORAL_SY;   /* into the coral's own cells */
        bool tip; best = coral_sd(qx, qy, 1.0f, &tip) * CL_CORAL_SY; *deep = tip;
    } else if (e == CLE_TUBE) {
        const cl_cap_t *k = &CL_TUBES[pass];
        best = cl_cap_sd(px, py, k, 1);
        float mx = (px - k->x1) / (k->r - 1.3f), my = (py - k->y1) / 1.7f;
        *deep = mx * mx + my * my < 1;                           /* the mouth */
    } else if (e == CLE_BRAIN) {
        float second = 1e9f;                                     /* the lobes: the seam where two meet is a groove */
        for (int i = 0; i < (int)(sizeof CL_BRAIN / sizeof CL_BRAIN[0]); i++) {
            float d = cl_cap_sd(px, py, &CL_BRAIN[i], 1);
            if (d < best) { second = best; best = d; } else if (d < second) second = d;
        }
        float v = fast_sin(px * 1.3f + py * 0.9f) + 0.6f * fast_sin(py * 2.1f - px * 0.5f);
        *deep = best < -0.8f && ((second < 0.5f && second > -1.2f) || v > 0.95f);   /* the seams, and a few winding grooves */
    }
    return best;
}
/* one element pass over the sprite: classed by its own distance field and
 * painted over what is there */
static void cl_paint(int e, int pass, float s) {
    int8_t  (*sd)[CL_CW] = ds()->cl_sd;                         /* eighths of a cell; the deep flags as bits (see sd_q) */
    uint8_t (*dp)[(CL_CW + 7) / 8] = ds()->cl_dp;
    for (int j = 0; j < CL_CH; j++)
        for (int i = 0; i < CL_CW; i++) {
            float px = CL_CX + (i + 0.5f - CL_CX) / s, py = (CL_CH - 1 - j + 0.5f) / s;   /* unscale the query */
            bool deep; sd[j][i] = sd_q(cl_sd(e, pass, px, py, &deep) * s);
            if (deep) dp[j][i >> 3] |= (uint8_t)(1 << (i & 7)); else dp[j][i >> 3] &= (uint8_t)~(1 << (i & 7));
        }
    for (int j = 0; j < CL_CH; j++)
        for (int i = 0; i < CL_CW; i++) {
            int d = sd[j][i];
            if (d >= 0) continue;
            bool deep = (dp[j][i >> 3] >> (i & 7)) & 1;
            bool out_l = i == 0 || sd[j][i - 1] >= 0, out_u = j == 0 || sd[j - 1][i] >= 0;
            bool out_r = i == CL_CW - 1 || sd[j][i + 1] >= 0, out_d = j == CL_CH - 1 || sd[j + 1][i] >= 0;
            int v;
            if (deep && e != CLE_ROCK) v = CLV_DEEP;
            else if (out_r || out_d) v = CLV_RIM;
            else if (out_l || out_u) v = CLV_LIT;
            else if (deep) v = CLV_DEEP;                        /* the rock's moss: never on its edge */
            else { uint32_t h = chash(i + 97 * e, j) % 100; v = h < 20 ? CLV_SHADE : h < 26 ? CLV_LIT : CLV_BODY; }
            if (e == CLE_TUBE && deep && (out_u || out_l)) v = CLV_RIM;   /* the mouth's far lip */
            ds()->cl_sprite[j][i] = (uint8_t)CL_TONE(e, v);
        }
}
static void cl_build(float g) {
    if (g > 1) g = 1;
    if (g < 0) g = 0;
    float s = CLUSTER_SIZE_MIN + (1.0f - CLUSTER_SIZE_MIN) * g;
    memset(ds()->cl_sprite, 0, sizeof ds()->cl_sprite);
    cl_paint(CLE_ROCK, 0, s);
    cl_paint(CLE_WEED, 0, s);
    cl_paint(CLE_CORAL, 0, s);
    for (int k = 0; k < 4; k++) cl_paint(CLE_TUBE, k, s);
    cl_paint(CLE_BRAIN, 0, s);
    cl_paint(CLE_WEED, 1, s);
    g_cl_cells = 0;
    for (int j = 0; j < CL_CH; j++) for (int i = 0; i < CL_CW; i++) g_cl_cells += ds()->cl_sprite[j][i] != 0;
}
int render_cluster_cells(float growth) { cl_build(growth); g_cl_q = -1; return g_cl_cells; }
static void cl_tint_fill(int scheme, float dim) {
    if (g_cl_scheme == scheme && g_cl_dim == dim && g_cl_dzk == g_dz_cl) return;
    float haze = decor_haze(0.90f, g_dz_cl);
    const cluster_scheme_t *sc = &CLUSTER_SCHEMES[scheme];
    uint32_t base[CLE_N] = { 0x9a9284, 0x8fc63a, sc->coral, sc->tube, sc->brain };
    uint32_t tone[CL_TONES]; tone[0] = 0;
    for (int e = 0; e < CLE_N; e++) {
        uint32_t c = base[e];
        tone[CL_TONE(e, CLV_BODY)]  = c;
        tone[CL_TONE(e, CLV_LIT)]   = rgb_mix(c, 0xfff0b0, e == CLE_ROCK ? 0.22f : 0.36f);
        tone[CL_TONE(e, CLV_RIM)]   = rgb_mix(c, e == CLE_ROCK ? 0x2a2620 : 0x20060e, 0.55f);
        tone[CL_TONE(e, CLV_SHADE)] = rgb_mix(c, 0x000000, 0.24f);
        tone[CL_TONE(e, CLV_DEEP)]  = e == CLE_ROCK ? 0x6f9a3a : e == CLE_CORAL ? rgb_mix(c, 0xffe8a0, 0.58f)
                                    : e == CLE_TUBE ? rgb_mix(c, 0x100418, 0.65f) : e == CLE_BRAIN ? rgb_mix(c, 0x0a3040, 0.45f) : rgb_mix(c, 0xe8ff80, 0.40f);
    }
    for (int r = 0; r < CL_H; r++) {
        int y = CL_FY - CL_H + r;
        uint32_t w = water_rgb(y < 0 ? 0 : y >= TANK_H ? TANK_H - 1 : y);
        for (int k = 1; k < CL_TONES; k++) ds()->cl_row[k][r] = rgb565(rgb_mix(w, tone[k], haze), dim);
    }
    g_cl_scheme = scheme; g_cl_dim = dim; g_cl_dzk = g_dz_cl;
}
static void draw_cluster(ctx_t *c, int cx, int scheme, float growth, bool final) {
    if (growth > 1) growth = 1;
    int q = (int)(growth * (CL_Q - 0.01f));
    if (q != g_cl_q) { cl_build(growth); g_cl_q = q; }
    cl_tint_fill(scheme, c->dim);
    int x0 = cx - CL_W / 2, y0 = CL_FY - CL_H;
    for (int j = 0; j < CL_CH; j++)
        for (int i = 0; i < CL_CW; i++) {
            uint8_t t = ds()->cl_sprite[j][i];
            if (!t) continue;
            for (int dy = 0; dy < CL_CELL; dy++) {
                int y = y0 + j * CL_CELL + dy;
                uint16_t v = ds()->cl_row[t][j * CL_CELL + dy];
                for (int dx = 0; dx < CL_CELL; dx++) {
                    int x = x0 + i * CL_CELL + dx;
                    if (!CTX_IN(c, x, y)) continue;
                    uint16_t *p = &CTX_PX(c, x, y);
                    *p = FB_SWAP16(v); TOUCH_PX(c, x, y);
                    rec_px(x, y);
                    if (final) {
                        PK_LIGHT_PX(p, x, y);
                        int a = g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                        if (a) px_darken(p, a);
                        if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
                    }
                }
            }
        }
    draw_floor_mound(c, cx, CLUSTER_HALF_W - 2, 6, g_dz_cl, final);            /* anchored: the rock sits in a low hump of the floor's stones */
}
/* the bloom (growth 1 -> CLUSTER_FULL): more and more tentacles, dealt one
 * at a time round the hosts - the four tube mouths, the coral's seven tips,
 * the brain's top - up to CL_TENT_MAX, each host's fan widening as it fills.
 * Per frame, swaying (draw_crown); the bounding box for the caller's rect. */
#define CL_TENT_MAX 48
#define CL_HOSTS    12
static void draw_cluster_crown(ctx_t *c, int cx, int scheme, float growth, float clock, int *bx0, int *by0, int *bx1, int *by1) {
    *bx0 = *by0 = 1 << 20; *bx1 = *by1 = -1;
    float phase = (growth - 1.0f) / (CLUSTER_FULL - 1.0f);
    if (phase <= 0) return;
    if (phase > 1) phase = 1;
    int total = (int)(phase * CL_TENT_MAX + 0.5f);
    if (total < 1) return;
    const cluster_scheme_t *sc = &CLUSTER_SCHEMES[scheme];
    float s = 1.0f;                                          /* full size by the time it blooms */
    float x0 = cx - CL_W / 2;
    for (int k = 0; k < CL_HOSTS; k++) {
        int n = total / CL_HOSTS + (k < total % CL_HOSTS ? 1 : 0);
        if (!n) continue;
        float hx, hy, len; uint32_t rgb;
        if (k < 4) { const cl_cap_t *t = &CL_TUBES[k]; hx = t->x1; hy = t->y1 + 0.5f; len = 11 + t->r; rgb = sc->tube; }
        else if (k < 11) {                                   /* the coral's tips, in CORAL_SEGS order */
            int ti = 0; const coral_seg_t *sg = NULL;
            for (int i = 0; i < CORAL_NSEG && !sg; i++) if (CORAL_SEGS[i].tip && ti++ == k - 4) sg = &CORAL_SEGS[i];
            if (!sg) continue;
            hx = CL_CORAL_X + (sg->x1 - 16.0f) * CL_CORAL_SX; hy = CL_CORAL_Y + sg->y1 * CORAL_Y_SCALE * CL_CORAL_SY; len = 9; rgb = sc->coral;
        } else { hx = 35; hy = 20; len = 8; rgb = sc->brain; }
        float ox = x0 + (CL_CX + (hx - CL_CX) * s) * CL_CELL + 1, oy = CL_FY - hy * s * CL_CELL - 1;
        float spread = 0.30f + 0.08f * n;                    /* radians each side of up, widening as the fan fills */
        int qx0, qy0, qx1, qy1;
        draw_crown(c, ox, oy, n, -1.5708f - spread, -1.5708f + spread, len, rgb_mix(rgb, 0xffffff, 0.55f), k * 7, clock, &qx0, &qy0, &qx1, &qy1);
        if (qx1 >= qx0) { if (qx0 < *bx0) *bx0 = qx0; if (qy0 < *by0) *by0 = qy0; if (qx1 > *bx1) *bx1 = qx1; if (qy1 > *by1) *by1 = qy1; }
    }
}

/* ---- THE PIECES (2026-10-17, Strato: a stack of the pumpkin, the grass,
 * the sword plant and a coral broke - "we need some global layering
 * mechanic") ----
 * The castle, the coral, the reef cluster and the pumpkin are ONE list, back
 * to front: tank_decor_order (depth BEHIND / IN FRONT of the plant layer,
 * then the keeper's rank; the sword plant is grass - bed 3 - and takes part
 * by its rank where it matters). Everything about who covers whom follows
 * from the list:
 *   - the bake paints the pieces in list order (the ones BEHIND, then - with
 *     the scene cache and no placement page up - the ones IN FRONT), each
 *     recording the pixels it covers in its own MASK (ds()->pm, by its item's slot);
 *   - a grass bed's spans skip the masks of the pieces above the bed
 *     (veg_skip_for): every piece IN FRONT for the grass, plus the pieces at
 *     its own depth ranked above it for the sword plant;
 *   - after the fish, every piece IN FRONT comes back over what was drawn on
 *     it this frame from the scene (pc_restore: the dirty mask says where, the
 *     scene holds the pixel, already lit and vignetted) - the castle's front
 *     row included (its keep stays behind the fish: the swim-through), so its
 *     old per-rect repaint is gone;
 *   - an overlay drawn every frame for a piece - a crown's tentacles, the
 *     candle's flicker - is clipped by the masks of the pieces above it
 *     (pc_covered_above, through g_clip_k / the lantern's g_pk_k);
 *   - the lantern's light: the pieces above it shade (the castle by its own
 *     rules) and cast on the sand; those below are lit.
 * A piece being dragged on its page is not in the scene: it is drawn live
 * (BEHIND: right over the scene; IN FRONT: after the grass), unmasked. The
 * castle's masks are static and centre-relative (castle_put), so it covers
 * even while dragged. */
#define PIECES_MAX 5
typedef struct {
    int  item;                            /* 2 the castle, 3 the coral, 4 the cluster, 8 the pumpkin, 9 the volcano */
    int  cx, z;                           /* its centre on the floor, its depth */
    bool placing;                         /* changed on its page (dragged, re-ranked, re-depthed, recolored): drawn LIVE this frame, out of the scene */
    bool baked;                           /* in the scene this frame (its mask is this frame's) */
    bool masked;                          /* a live piece whose mask was recorded this frame all the same (render_debug_live_mask: the tests) */
    int  x0, y0, w, rows;                 /* its box: the mask's frame */
} piece_t;
static piece_t g_pc[PIECES_MAX];
static bool g_scene_front;                /* the baked scene holds the pieces IN FRONT */
/* the scene's signature: what the bake depends on (render_tank rebakes on a change) */
typedef struct { int n, item[PIECES_MAX], cx[PIECES_MAX], z[PIECES_MAX], q[PIECES_MAX]; uint32_t col[PIECES_MAX]; float dim; bool front; int lx; } scene_sig_t;
static scene_sig_t g_sig; static bool g_sig_ok;
/* THE PATCH (0.3.9.4, with THE GRASS LOOP's pieces below): the pieces' masks of the scene before the last bake, kept at bake_scene for the
   window's columns - whose scene they are, and whose the masks in ds()->pm are */
static uint32_t *g_gl_pm_old; static scene_sig_t g_gl_pm_old_sig, g_gl_pm_cur_sig;
#define GL_PM_BYTES (sizeof ((decor_scratch_t *)0)->pm)
static bool g_pk_hidden;                  /* director `pumpkin off`: the frame-rate A/B */
void render_set_pumpkin_hidden(bool hidden) { g_pk_hidden = hidden; }
bool render_pumpkin_hidden(void) { return g_pk_hidden; }
static float pc_half_w(int item) { return item == 2 ? CASTLE_HALF_W : item == 3 ? CORAL_HALF_W : item == 4 ? CLUSTER_HALF_W : item == 9 ? VOLCANO_HALF_W : PUMPKIN_HALF_W; }
static int   pc_index(int item) { for (int k = 0; k < g_npc; k++) if (g_pc[k].item == item) return k; return -1; }
/* the solid pieces above the lantern cast on the sand (pk_casts; the castle has its own rules) */
static void pk_casts_pieces(const tank_t *t, int px0, int px1) {
    (void)t;
    for (int k = g_pk_k + 1; k < g_npc; k++) {
        if (g_pc[k].item == 2 || g_pc[k].placing) continue;
        int hw = (int)pc_half_w(g_pc[k].item) - 3;
        /* only where a piece stands IN FRONT of the lantern - over its own patch of sand - is the sand shaded; one
           beside it, nearer the glass, leaves the light where it falls (0.3.9.3, Bananna on the pendant: "item
           placement can block light from pumpkin" - every piece above it in the order cut the floor's light
           at its edge, however far to the side) */
        int a = g_pc[k].cx - hw > px0 ? g_pc[k].cx - hw : px0, z = g_pc[k].cx + hw < px1 ? g_pc[k].cx + hw : px1;   /* ... and only where it covers it */
        if (z < a) continue;
        pk_cast_add(a - 6, a - 1, 1); pk_cast_add(a, z, 3); pk_cast_add(z + 1, z + 6, 1);   /* (a soft half step either side) */
    }
}
static uint32_t g_boot_hide;                       /* THE BOOT SHOW: pieces left out of a scene (bit = item; render_boot_scene) */
/* this frame's list, from the tank's order; front: the pieces IN FRONT ride in the scene */
static void pieces_build(const tank_t *t, bool cached, bool front) {
    int items[DECOR_SLOTS], n = tank_decor_order(t, items);
    g_npc = 0; g_pk_k = -1; g_castle_cx = -1;
    g_dz_castle = g_dz_coral = g_dz_cl = g_dz_pk = g_dz_vo = 0;
    for (int i = 0; i < n && g_npc < PIECES_MAX; i++) {
        int item = items[i];
        if (item == 0 || (item == 8 && g_pk_hidden) || (g_boot_hide >> item & 1)) continue;
        piece_t *p = &g_pc[g_npc];
        p->item = item; p->cx = (int)tank_decor_x(t, item); p->z = tank_decor_z(t, item);
        p->placing = setup_is_place() && setup_item() == item && setup_place_live();   /* (a page just opened leaves its piece in the scene) */
        p->baked = cached && !p->placing && (p->z != DECOR_Z_FRONT || front);
        p->masked = false;
        switch (item) {                       /* its floor line by its depth first (0.3.9), then its box */
            case 2:  g_dz_castle = decor_dz(p->z, TANK_BOT - 16);
                     p->x0 = p->cx - CASTLE_MASK_W / 2; p->y0 = CASTLE_FY - CASTLE_ROWS; p->w = CASTLE_MASK_W; p->rows = CASTLE_ROWS + 12; g_castle_cx = p->cx; break;
            case 3:  g_dz_coral = decor_dz(p->z, TANK_BOT - 14 + DECOR_SINK);
                     p->x0 = p->cx - CORAL_W / 2; p->y0 = CORAL_FY - CORAL_H; p->w = CORAL_W; p->rows = CORAL_H + 4; break;
            case 4:  g_dz_cl = decor_dz(p->z, TANK_BOT - 14 + DECOR_SINK);
                     p->x0 = p->cx - CL_W / 2; p->y0 = CL_FY - CL_H; p->w = CL_W; p->rows = CL_H + 4; break;
            case 9:  g_dz_vo = decor_dz(p->z, TANK_BOT - 16);
                     p->x0 = p->cx - VO_W / 2; p->y0 = VO_FY - VO_H; p->w = VO_W; p->rows = VO_H + 4; break;
            default: g_dz_pk = decor_dz(p->z, TANK_BOT - 14 + DECOR_SINK);
                     p->x0 = p->cx - PK_W / 2; p->y0 = PK_FY - PK_H; p->w = PK_W; p->rows = PK_H + 4; break;
        }
        g_npc++;
    }
    /* depth first: every piece BEHIND, then every piece IN FRONT, each in its rank order */
    for (int i = 1; i < g_npc; i++) {
        piece_t v = g_pc[i]; int j = i;
        while (j > 0 && g_pc[j - 1].z > v.z) { g_pc[j] = g_pc[j - 1]; j--; }
        g_pc[j] = v;
    }
    g_pk_k = pc_index(8);
}
static inline int pc_mslot(int item) { return item == 2 ? 0 : item == 3 ? 1 : item == 4 ? 2 : item == 8 ? 3 : 4; }   /* a piece's mask (ds()->pm), by its item */
/* piece k's silhouette at (x, y): its recorded mask (this frame's scene), or the castle's static one */
static bool pc_covers(int k, int x, int y) {
    const piece_t *p = &g_pc[k];
    int lx = x - p->x0, r = y - p->y0;
    if ((unsigned)lx >= (unsigned)p->w || (unsigned)r >= (unsigned)p->rows) return false;
    if (p->item == 2) return (g_castle_mask[r][lx >> 5] >> (lx & 31)) & 1;
    return (p->baked || p->masked) && (((p->placing ? ds()->pm_live : ds()->pm[pc_mslot(p->item)])[r][lx >> 5] >> (lx & 31)) & 1);
}
/* ... and the part of it that stands over the fish (a piece IN FRONT whole; the castle's front row) */
static bool pc_front_covers(int k, int x, int y) {
    const piece_t *p = &g_pc[k];
    if (p->z != DECOR_Z_FRONT) return false;
    if (p->item == 2) {
        int lx = x - p->x0, r = y - p->y0;
        return (unsigned)lx < (unsigned)p->w && (unsigned)r < (unsigned)p->rows && ((castle_front()[r][lx >> 5] >> (lx & 31)) & 1);
    }
    return pc_covers(k, x, y);
}
static bool cr_clipped(int x, int y) {
    for (int k = 0; k < g_npc && k < 32; k++) if ((g_cr_clip >> k & 1) && pc_covers(k, x, y)) return true;
    return false;
}
/* SWIM DEPTH phase 2: what of a piece stands before a fish passing BEHIND it, whatever the piece's own depth - the
   castle's front row (the arch stays open: the fish shows in it, between the keep and the wall), any other piece whole */
static bool pc_body_covers(int k, int x, int y) {
    const piece_t *p = &g_pc[k];
    if (p->item == 2) {
        int lx = x - p->x0, r = y - p->y0;
        return (unsigned)lx < (unsigned)p->w && (unsigned)r < (unsigned)p->rows && ((castle_front()[r][lx >> 5] >> (lx & 31)) & 1);
    }
    return pc_covers(k, x, y);
}
bool render_bed_leaf(int x, int y) {   /* (tests: a leaf of the sword plant IN FRONT, as the last frame recorded it) */
    return g_bedm_on && g_bedm && ((g_bedm[y * DIRTY_WORDS_PER_ROW + (x >> 5)] >> (x & 31)) & 1);
}
bool render_piece_covers(int item, int x, int y) {   /* (the sim's --selftest-depth: a piece's own body, as the last frame recorded it) */
    for (int k = 0; k < g_npc; k++) if (g_pc[k].item == item) return pc_body_covers(k, x, y);
    return false;
}
static bool pc_in_box_rim(int k, int x0, int x1, int y) {   /* the span meets piece k's box, with its contact shadow's rim down and to the right */
    const piece_t *p = &g_pc[k];
    return y >= p->y0 && y < p->y0 + p->rows + PC_SHADOW_FAR && x1 >= p->x0 && x0 < p->x0 + p->w + PC_SHADOW_FAR;
}
/* n (<= 32) bits of a bit row from bit l on; bits outside 0..nw*32-1 read 0 */
static inline uint32_t row_bits(const uint32_t *row, int nw, int l, int n) {
    int sh = 0;
    if (l < 0) { sh = -l; if (sh >= n) return 0; n -= sh; l = 0; }
    int w = l >> 5;
    if (w >= nw) return 0;
    uint64_t v = row[w];
    if (w + 1 < nw) v |= (uint64_t)row[w + 1] << 32;
    uint32_t out = (uint32_t)(v >> (l & 31));
    if (n < 32) out &= (1u << n) - 1;
    return out << sh;
}
static uint32_t pc_row_bits(int k, int x0, int n, int y) {
    const piece_t *p = &g_pc[k]; int r = y - p->y0;
    if ((unsigned)r >= (unsigned)p->rows) return 0;
    const uint32_t *row = p->item == 2 ? g_castle_mask[r] : p->baked ? ds()->pm[pc_mslot(p->item)][r] : p->masked ? ds()->pm_live[r] : NULL;   /* (live: its own slot, as pc_covers) */
    return row ? row_bits(row, (p->w + 31) / 32, x0 - p->x0, n) : 0;
}
static bool pc_covered_above(int k, int x, int y) {
    for (int j = k + 1; j < g_npc; j++) if ((g_pc[j].baked || g_pc[j].item == 2) && pc_covers(j, x, y)) return true;
    return false;
}
bool render_decor_covers(int item, int x, int y) { int k = pc_index(item); return k >= 0 && pc_covers(k, x, y); }   /* (the tests) */
/* the CONTACT SHADOW (0.3.9, Strato: "we're losing depth ... everything flat
 * against the glass"): right after a piece is baked, its silhouette shifted
 * down and to the right (the light comes from the upper left) darkens what
 * stands behind it in the scene - the water, the sand, a piece behind it -
 * a hard step close in and a softer one further out, never on the piece
 * itself. The scene at that moment holds only what is behind it, so the
 * order does the rest. Bake only: nothing per frame. */
static void pc_contact_shadow(ctx_t *c, int k) {
    const piece_t *p = &g_pc[k];
    int xa = p->x0, xb = p->x0 + p->w - 1 + PC_SHADOW_FAR, ya = p->y0, yb = p->y0 + p->rows - 1 + PC_SHADOW_FAR;
    if (xa < 0) xa = 0;
    if (xb > TANK_W - 1) xb = TANK_W - 1;
    if (ya < 0) ya = 0;
    if (yb > TANK_H - 1) yb = TANK_H - 1;
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            if (pc_covers(k, x, y)) continue;
            int a = pc_covers(k, x - PC_SHADOW_NEAR, y - PC_SHADOW_NEAR) ? PC_SHADOW_A_NEAR : pc_covers(k, x - PC_SHADOW_FAR, y - PC_SHADOW_FAR) ? PC_SHADOW_A_FAR : 0;
            if (a) px_darken(&CTX_PX(c, x, y), a);
        }
}
/* the pieces a bed's spans leave alone (veg_span) */
static uint32_t veg_skip_for(const tank_t *t, int b) {
    int bz = bed_z(t, b); uint32_t m = 0;
    for (int k = 0; k < g_npc; k++) {
        if (!g_pc[k].baked && g_pc[k].item != 2 && !(b == 3 && g_pc[k].masked && g_pc[k].z == DECOR_Z_BACK)) continue;   /* drawn live this frame: IN FRONT over
                                                                         the grass anyway; BEHIND it is drawn under the grass, so the plant (over the seagrass since 0.4)
                                                                         skips it by its live mask where it is ranked above the plant (its page, the piece dragged) */
        if (g_pc[k].z == DECOR_Z_FRONT && bz != DECOR_Z_FRONT) m |= 1u << k;             /* in front of the plant layer */
        else if (b == 3 && bz != DECOR_Z_MIDDLE && g_pc[k].z == bz && tank_decor_above(t, g_pc[k].item, 0)) m |= 1u << k;   /* at the plant's depth, ranked above it */
    }
    return m;
}
/* a piece IN FRONT back over what was drawn on it this frame (the scene has it) - but not over the crab roosting in the arch of a
   castle IN FRONT ranked above the piece (g_rs_crab, set for that piece's restore): the piece stands behind the castle, so behind its
   arch and the crab in it (Strato 2026-11-02, the watch: both IN FRONT, the castle above, the volcano's rock restored over the crab) */
static const tank_t *g_rs_crab;
static bool g_rs_old;                                /* (--selftest-roost's control: THE CRAB'S STACK off - every piece's restore and overlay over the crab
                                                        in the arch, the crab over every piece before its shelter - as before 0.4) */
void render_debug_crab_stack_old(bool on) { g_rs_old = on; }
static void pc_restore(uint16_t *fb, int k) {
    const piece_t *p = &g_pc[k];
    int xa = p->x0 < 0 ? 0 : p->x0, xb = p->x0 + p->w - 1 > TANK_W - 1 ? TANK_W - 1 : p->x0 + p->w - 1;
    int ya = p->y0 < 0 ? 0 : p->y0, yb = p->y0 + p->rows - 1 > TANK_H - 1 ? TANK_H - 1 : p->y0 + p->rows - 1;
    for (int y = ya; y <= yb; y++) {
        uint32_t *drow = g_dirty + y * DIRTY_WORDS_PER_ROW;
        for (int w = xa >> 5; w <= xb >> 5; w++) {
            uint32_t bits = drow[w];
            if (g_keep_on) bits &= ~g_keep[y * DIRTY_WORDS_PER_ROW + w];   /* (a fish in front of the pieces stays in front) */
            while (bits) {
                int x = (w << 5) + __builtin_ctz(bits); bits &= bits - 1;
                if (x < xa || x > xb || !pc_front_covers(k, x, y) || (g_rs_crab && crab_covers(g_rs_crab, x, y))) continue;
                fb[y * TANK_W + x] = g_scene[y * TANK_W + x];
                drow[w] &= ~(1u << (x & 31));
            }
        }
    }
}
/* a piece drawn LIVE - the page's, changed on it, so out of the scene - is painted over whatever the scene holds inside its box,
   the pieces ranked above it included. Those come back from the scene where their masks cover it (a piece whole; the castle's
   keep and wall, its arch open), keeping off a fish IN FRONT of the pieces and the leaves of a bed IN FRONT that is ranked over
   the covering piece (0.3.9.4: before, every piece IN FRONT was live with a page up, drawn in order, so none needed this) */
static void pc_cover_live(uint16_t *fb, int k) {
    const piece_t *p = &g_pc[k];
    for (int j = k + 1; j < g_npc; j++) {
        const piece_t *q = &g_pc[j];
        if (!q->baked) continue;
        int xa = p->x0 > q->x0 ? p->x0 : q->x0, xb = (p->x0 + p->w < q->x0 + q->w ? p->x0 + p->w : q->x0 + q->w) - 1;
        int ya = p->y0 > q->y0 ? p->y0 : q->y0, yb = (p->y0 + p->rows < q->y0 + q->rows ? p->y0 + p->rows : q->y0 + q->rows) - 1;
        if (xa < 0) xa = 0;
        if (xb > TANK_W - 1) xb = TANK_W - 1;
        if (ya < 0) ya = 0;
        if (yb > TANK_H - 1) yb = TANK_H - 1;
        for (int y = ya; y <= yb; y++)
            for (int x = xa; x <= xb; x++) {
                if (!pc_covers(j, x, y) || keep_at(x, y) || bed_over(j, x, y)) continue;
                fb[y * TANK_W + x] = g_scene[y * TANK_W + x];
                if (g_dirty) g_dirty[y * DIRTY_WORDS_PER_ROW + (x >> 5)] &= ~(1u << (x & 31));
            }
    }
}
static bool g_live_rec;                   /* a live piece records its mask too (no shadow): the tests read it (render_piece_covers) */
void render_debug_live_mask(bool on) { g_live_rec = on; }
/* one piece, whole: in the bake (final, recording its mask) or live */
static void pc_draw(ctx_t *c, const tank_t *t, int k, bool final, bool record) {
    piece_t *p = &g_pc[k];
    if (record) {
        uint32_t (*pm)[(CASTLE_MASK_W + 31) / 32] = p->placing ? ds()->pm_live : ds()->pm[pc_mslot(p->item)];   /* (live: its own slot) */
        memset(pm, 0, sizeof ds()->pm_live);
        g_rec = &pm[0][0]; g_rec_x0 = p->x0; g_rec_y0 = p->y0; g_rec_w = p->w; g_rec_rows = p->rows; g_rec_wpr = (CASTLE_MASK_W + 31) / 32;
    }
    g_pk_shade_all = g_pk_lx >= 0 && g_pk_k >= 0 && k > g_pk_k && p->item != 2;   /* above the lantern: backlit (the castle has its own rules: the light through its arch and windows) */
    switch (p->item) {
        case 2:  draw_castle(c, p->cx, 0, final); if (p->z == DECOR_Z_BACK || record) arch_shade(c, k, p->cx, t->night, false); break;   /* IN FRONT: the bake carries the tunnel's shade too (2026-10-23); live, arch_shade_dirty does the rest */
        case 3:  draw_coral(c, p->cx, tank_coral_rgb(t), tank_coral_growth(t), final); break;
        case 4:  draw_cluster(c, p->cx, tank_cluster_scheme(t), tank_cluster_growth(t), final); break;
        case 9:  draw_volcano(c, p->cx, t->night, final); break;
        default: draw_pumpkin(c, p->cx, t->night, anim(t), final, true, final); break;
    }
    g_pk_shade_all = false;
    g_rec = NULL;
    if (record && !p->placing) pc_contact_shadow(c, k);
    if (record && p->placing) p->masked = true;      /* (a live piece casts no shadow; its mask: a live piece BEHIND ranked over the plant keeps its
                                                        leaves off (veg_skip_for), and the tests read it) */
}
/* a piece's overlay, drawn every frame over the baked body: a crown's
 * tentacles - clipped by the pieces above it (g_clip_k); its rect joins the
 * vignette sweep */
#define PC_CROWN(k) do { int qx0 = 0, qy0 = 0, qx1 = -1, qy1 = 0; \
        if (g_pc[k].item == 3) draw_coral_crown(&c, g_pc[k].cx, tank_coral_rgb(t), tank_coral_growth(t), anim(t), &qx0, &qy0, &qx1, &qy1); \
        else if (g_pc[k].item == 4) draw_cluster_crown(&c, g_pc[k].cx, tank_cluster_scheme(t), tank_cluster_growth(t), anim(t), &qx0, &qy0, &qx1, &qy1); \
        if (qx1 >= qx0) DYN_RECT(qx0, qy0, qx1, qy1); } while (0)
/* the volcano's: its lava breathes over the baked rock, its bubbles rise (their rect joins the sweep) */
#define PC_VOLCANO(k) do { if (g_pc[k].baked) draw_volcano_glow(&c, g_pc[k].cx, anim(t), t->night, false, true); \
        int qx0, qy0, qx1, qy1; draw_volcano_bubbles(&c, g_pc[k].cx, t->clock, t->night, &qx0, &qy0, &qx1, &qy1); DYN_RECT(qx0, qy0, qx1, qy1); } while (0)
#define PK_LIGHT_Y (PK_FY - (int)(8.0f * PK_S * PK_CELL))   /* the face's middle: where the light leaves */

/* the floor's colour at (x,y), from a position hash so it is stable every
 * frame. The rectangle: a 14 px strip of speckled stones in four tones. The
 * bowl: the same speckle in 2 px grains over a bed that runs ~70 px down to
 * the glass, pebbles scattered through it, darkening with depth. */
static inline uint32_t floor_rgb(int x, int y) {
#ifdef TANK_ROUND
    uint32_t g = (uint32_t)(x >> 1) * 0x9E3779B1u ^ (uint32_t)(y >> 1) * 0x85EBCA77u;
    g ^= g >> 15; g *= 0x2C1B3C6Du; g ^= g >> 12;
    uint32_t tone = g % 16;
    uint32_t col = tone < 2 ? 0x2e3b2c : tone < 5 ? 0x22301f : tone < 8 ? 0x1a2418 : 0x101a12;
    int top = TANK_BOT - 14, cx = x / 13, cy = (y - top) / 9;            /* one pebble, or none, per 13 x 9 cell */
    uint32_t h = (uint32_t)(cx + 7) * 0x9E3779B1u ^ (uint32_t)(cy + 3) * 0xC2B2AE35u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15;
    if (y > top + 4 && h % 8 < 3) {
        float px = cx * 13 + 4 + (h >> 4) % 6, py = top + cy * 9 + 4 + (h >> 8) % 3;
        float rx = 2.6f + (h >> 12) % 3, ry = 1.6f + ((h >> 16) % 3) * 0.5f;
        float dx = (x - px) / rx, dy = (y - py) / ry;
        if (dx * dx + dy * dy <= 1.0f) col = dy < -0.25f ? 0x44544a : (h >> 20) % 2 ? 0x334238 : 0x2a372c;   /* lit from above */
    }
    float depth = (float)(y - top) / (TANK_H - top);
    return rgb_mix(col, 0x02080a, depth < 0 ? 0 : depth * 0.55f);
#else
    uint32_t h2 = (uint32_t)((x * 73856093u) ^ (y * 19349663u));
    uint32_t tone = (h2 >> 4) % 16;
    return tone < 2 ? 0x2e3b2c : tone < 5 ? 0x22301f : tone < 8 ? 0x1a2418 : 0x101a12;
#endif
}
static void draw_scene(const tank_t *t, uint16_t *fb, int stride, float dim) {
    ctx_t c = ctx_full(fb, stride, dim);
    /* water gradient #0a3c46 → #08272f → #031015 */
    for (int y = 0; y < TANK_H; y++) {
        uint16_t v = FB_SWAP16(rgb565(water_rgb(y), dim));
        for (int x = 0; x < TANK_W; x++) fb[y * stride + x] = v;
    }
    /* pebbled bottom: irregular top edge, speckled stones in three tones.
       All variation comes from a position hash so it is stable every frame. */
    for (int x = 0; x < TANK_W; x++) {
        uint32_t h = (uint32_t)(x * 2654435761u);
        int top = TANK_BOT - 14 - (int)((h >> 8) % 5);
        for (int y = top; y < TANK_H; y++) {
            uint32_t col = floor_rgb(x, y);
            fb[y * stride + x] = FB_SWAP16(rgb565(col, dim));
        }
    }
    /* (the dark oval under the left bed - the "reef rock" - is gone, 2026-10-02:
       it stood for nothing; the reef is the cluster the keeper buys) */
    for (int k = 0; k < g_npc; k++)                 /* uncached: every piece, in order, every frame - a dragged piece too when it is BEHIND, and the
                                                       castle whole (its keep goes under the fish), as the cached path draws them (0.3.9: before,
                                                       a dragged castle had only its front row here) */
        if (!g_pc[k].placing || g_pc[k].z == DECOR_Z_BACK || g_pc[k].item == 2) pc_draw(&c, t, k, false, false);
}


/* The baked scene (scene cache mode). Water gradient x porthole vignette
 * computed in 8-bit and ORDERED-DITHERED to RGB565 (4x4 Bayer: at 322 ppi
 * the pattern is invisible, the 5/6-bit banding of a dark gradient and of
 * the vignette falloff is not - Strato saw it at the edges), then the floor
 * and the backdrop decor on top, vignetted per pixel. Fills the vignette LUT on the
 * way. Bake-only: fidelity here costs nothing per frame. The light shafts
 * are gone (2026-09-01, Strato: they never looked good on this screen). */
static void bake_scene(const tank_t *t, uint16_t *sc, float dim) {
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    /* the bake is the scene's own: nothing of a FRAME stands before the pieces in it. Called outside a frame (the boot show's scenes and
       its final bake, render_rebake_scene) the LAST frame's marks were still set - a fish IN FRONT of the pieces (g_keep), the sword
       plant's leaves (g_bedm), a critter in the front slot (g_guard) - and the pieces' stores keep off them: pieces baked with fish-shaped
       HOLES, unrecorded, so the grass loop painted fronds into them, until the next full rebake (0.3.9.4, Strato on the square right after
       a boot: "small cut outs in the left and right edge" of the pumpkin and fronds through the volcano where the pumpkin overlaps its foot;
       --square-boot stages it). Cleared for the bake, put back after. */
    const bool keep_on = g_keep_on, bedm_on = g_bedm_on, fbg_on = g_fbg_on;
    __typeof__(g_guard) guard = g_guard; __typeof__(g_guard_who) guard_who = g_guard_who; __typeof__(g_clip_k) clip_k = g_clip_k; __typeof__(g_under) under = g_under;
    g_keep_on = false; g_bedm_on = false; g_fbg_on = false; g_guard = NULL; g_guard_who = 0; g_clip_k = -1; g_under = NULL;
    if (g_gl_pm_old) { memcpy(g_gl_pm_old, ds()->pm, GL_PM_BYTES); g_gl_pm_old_sig = g_gl_pm_cur_sig; }   /* THE PATCH: the pieces' masks of the scene going out, and whose they are */
    for (int y = 0; y < TANK_H; y++) {
        uint32_t col = water_rgb(y);
        int r = (int)(((col >> 16) & 255) * dim), g = (int)(((col >> 8) & 255) * dim), b = (int)((col & 255) * dim);
        for (int x = 0; x < TANK_W; x++) {
            int a = vig_alpha(x, y);
            if (g_vig) g_vig[y * TANK_W + x] = (uint8_t)a;
            int inv = 256 - a, d = bayer[y & 3][x & 3];
            int rr = (r * inv) >> 8, gg = (g * inv) >> 8, bb = (b * inv) >> 8;
            int r5 = (rr + (d >> 1)) >> 3, g6 = (gg + (d >> 2)) >> 2, b5 = (bb + (d >> 1)) >> 3;
            if (r5 > 31) r5 = 31;
            if (g6 > 63) g6 = 63;
            if (b5 > 31) b5 = 31;
            sc[y * TANK_W + x] = FB_SWAP16((uint16_t)((r5 << 11) | (g6 << 5) | b5));
        }
    }
    /* pebbled bottom: irregular top edge, speckled stones in three tones,
       stable position hash; vignetted in 8-bit before quantising */
    for (int x = 0; x < TANK_W; x++) {
        uint32_t h = (uint32_t)(x * 2654435761u);
        int top = TANK_BOT - 14 - (int)((h >> 8) % 5);
        for (int y = top; y < TANK_H; y++) {
            uint32_t col = floor_rgb(x, y);
            int inv = 256 - (g_vig ? g_vig[y * TANK_W + x] : vig_alpha(x, y));
            int r = (int)(((col >> 16) & 255) * dim) * inv >> 8;
            int g = (int)(((col >> 8) & 255) * dim) * inv >> 8;
            int b = (int)((col & 255) * dim) * inv >> 8;
            sc[y * TANK_W + x] = FB_SWAP16((uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
        }
    }
    if (g_pk_lx >= 0) pk_halo(sc);                 /* the lantern's light on the water and the floor (the decor lights itself) */
    ctx_t c = ctx_full(sc, TANK_W, dim);
    if (t->night) for (int k = 0; k < g_npc; k++) if (g_pc[k].item == 9 && g_pc[k].baked) vo_halo(sc, g_pc[k].cx);   /* the volcano's embers on the water and the sand (0.3.9) */
    const bool baking = g_baking; g_baking = true;  /* (the lantern keeps off no piece above it: those are painted after it - coral-sliver, 2026-11-02) */
    for (int k = 0; k < g_npc; k++)                 /* the pieces, back to front, each recording its mask */
        if (g_pc[k].baked) pc_draw(&c, t, k, true, true);
    g_baking = baking;
    g_gl_pm_cur_sig = g_sig;
    g_keep_on = keep_on; g_bedm_on = bedm_on; g_fbg_on = fbg_on; g_guard = guard; g_guard_who = guard_who; g_clip_k = clip_k; g_under = under;
}

/* ---- the grass loop's pieces (THE GRASS LOOP, above) ---- */
/* what the back grass looks like, but for its sway: the scene, the beds, their fronds - and apart from it, the fronds'
   heights (segs: each frond's segment count into seg[], when asked). A frond a segment taller or shorter (3.2 px; it grows,
   the urchin nibbles) left the whole loop stale: on the 1.8, 9 rebakes in a minute while the urchin ate, ~12-17 fps.
   Now a small change WAITS (GL_SEG_WAIT_S from when it first showed, the loop showing the heights it was baked with); a
   frond GL_SEG_JUMP segments off (a trim, a real bite), anything else, or a loop not yet whole rebakes at once. (A first
   cut waited 2 min from the BAKE: an old loop rebaked at the first segment, a growing tank every ~2 min, each some
   seconds at ~10 fps on the square - Strato's "once in a while the frame rate really slows down") */
#define GL_SEG_WAIT_S 900.0f
static uint8_t g_gl_seg[VEG_BEDS_MAX][VEG_FRONDS_MAX];   /* the heights the loop was baked with */
static uint32_t g_gl_seg_h, g_gl_hard_last;             /* ... their hash; the last frame's look but for the heights */
static double  g_gl_seg_t = -1;                        /* the tank clock the heights first moved at (-1: they have not) */
static bool    g_gl_seg_ok;                            /* g_gl_seg holds a look's heights (a stale event copied them): THE HELD HEIGHTS apply */
static uint32_t gl_key(const tank_t *t, uint32_t *segs, uint8_t seg[VEG_BEDS_MAX][VEG_FRONDS_MAX]) {
    uint32_t h = 2166136261u, g = 2166136261u;
#define GL_MIX(h, v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    { const uint8_t *p = (const uint8_t *)&g_sig; for (size_t k = 0; k < sizeof g_sig; k++) GL_MIX(h, p[k]); }   /* WHAT the scene is, not how often it
                                                                                    was drawn: the day comes back to its own loop (0.3.9.3: a light switch rebaked it
                                                                                    twice - ~17 s at 10-12 fps each way on the square - and a menu closing once) */
    { uint32_t d; memcpy(&d, &g_scene_dim, sizeof d); GL_MIX(h, d); }
    GL_MIX(h, g_veg_step); GL_MIX(h, g_veg_arch_shade); GL_MIX(h, g_veg_mask_cx);
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (bed_z(t, b) != DECOR_Z_BACK) continue;
        int n; float bx0; tank_veg_bed(t, b, &bx0, NULL, NULL, &n);
        GL_MIX(h, b); GL_MIX(h, tank_veg_kind(t, b)); GL_MIX(h, (int)(bx0 * 4)); GL_MIX(h, n);
        if (segs) for (int i = 0; i < n; i++) { float bx; int sg = tank_veg_frond(t, b, i, &bx); GL_MIX(g, sg); if (seg && b < VEG_BEDS_MAX && i < VEG_FRONDS_MAX) seg[b][i] = (uint8_t)sg; }
    }
#undef GL_MIX
    if (segs) *segs = g;
    return h;
}
/* once a frame, before the frame uses the loop: is it stale (the rule above)? The look but for the heights is returned;
   the whole look (heights too) is left in g_gl_full_now - the bake waits for it to hold still */
static uint32_t g_gl_full_now;
/* THE LIGHT OFF (0.3.9.3): the loop sits it out - its grass drawn live, as before the loop, stepping on with the frames so
   the sway never jumps - and is neither used nor marked stale, so the day's loop is there the moment the light comes
   back. There is no memory for a second (night) loop on the square: a light switch was a full rebake both ways */
static bool g_gl_dark;
static bool g_gl_parked;                        /* a piece is drawn live (its page): the loop sits it out - not used, not rebaked (render_tank) */

/* ---- THE PATCH and the ring (0.3.9.4) ----
 * Strato on the square, 10-31: DONE after a move - "a huge framerate drop". The loop's 66 moments were baked again whole,
 * 36 ms each with the back grass drawn live meanwhile: ~25 s at 7-9 fps (and once the fine moments' arena ran out, a replan
 * ran it twice - 155 bakes). Most of what makes the loop stale changes the look only inside a few BOXES: a piece moved,
 * bought, sold, shelved or hidden (its old place and its new one, each with its contact shadow's rim), a frond grown past
 * the wait (its strip), the sword plant BEHIND moved (its old strip and its new one), a piece newly ranked over a bed. A
 * frond is opaque and its color its own - the scene is never under a frond's pixel - so outside the boxes every band and
 * every difference is right pixel for pixel: only the runs a piece IN FRONT kept off, the rim it shaded and the scene under
 * them are in the boxes. So the loop keeps all of it. A frame is primed from its band and difference as ever and the
 * boxes alone are repainted live (the new scene copied in, the back grass of the frame's moment drawn over it, clipped
 * to the box - gl_patch_rect); the bake repaints a band inside the boxes in place, and a fine moment's difference is
 * SPLICED (gl_diff_splice: its rows outside the boxes copied word for word, inside them the old runs kept up to the box's
 * edge and the box encoded afresh) into the arena, which is a RING now - the oldest difference is replaced first, the new
 * one written at the head, so the live set never needs room for a second copy of itself. Whatever is not boxes (the
 * light's dim, a trim of a whole bed, the band growing past its room) still bakes whole - in SLICES (gl_bake_step: the
 * band's copy a few rows at a time, each bed, the encode a few rows at a time) under a frame's budget, so no frame
 * carries a 36 ms bake; and the fine moments' room is planned from the last whole loop's measured differences and
 * checked early in a bake, so a replan comes once, before most of the work, never after it. */
#define GL_WIN_MAX 6
#define GL_WIN_MOST 90                                                   /* the boxes' share of the band at most, in percent (past it the whole bake costs no more) */
static gl_rect_t g_gl_win[GL_WIN_MAX]; static int g_gl_nwin;             /* the boxes the loop is stale inside (inside the band) */
static struct { int k, j, phase, row, bed; size_t w, off, cap, op; } g_gl_bs;   /* the whole bake under way, in slices (gl_bake_step): its band (k >= 0) or fine moment, its
                                                                            phase (0 none, 1 the scene's copy, 2 the beds, 3 the encode), where it is in it */
static bool      g_gl_patch;                                             /* stale inside g_gl_win only: served with the boxes repainted, rebaked inside them */
static uint16_t  g_gl_gen;                                               /* the look's generation: a band or a moment baked at it is exact */
static uint16_t  g_gl_bgen[GL_MAX];
static uint16_t *g_gl_fgen;                                              /* ... each fine moment's (with the ring's tables, in the loop's memory) */
static scene_sig_t g_gl_sig;                                             /* the scene the loop was last baked toward */
static float     g_gl_dim_baked = -1; static int g_gl_step_baked, g_gl_mcx_baked = -1; static bool g_gl_ashade_baked;
static struct { bool back; int kind, n; float x0, x1; uint32_t skip; } g_gl_bed[VEG_BEDS_MAX];   /* each bed as baked: in the back grass, its extent, the items its spans kept off */
/* the ring: the differences in the order written (g_gl_dq, the oldest first), each at g_gl_doff[j] for g_gl_dlen[j] words; the
   next one goes at the head - the room there runs to the arena's end (then it wraps to 0) or to the tail, the oldest's start */
static uint32_t *g_gl_dlen; static uint8_t *g_gl_dq; static int g_gl_dqn; static size_t g_gl_dhead, g_gl_dtail;
static void     *g_gl_meta;                                              /* (the block the three tables live in: gl_geometry) */
static size_t    g_gl_davg;                                              /* words a fine moment's difference ran, the last time the loop was whole (the next plan's estimate) */
static int       g_gl_pwin;                                              /* THE PATCH's rebake: the box being patched next (a band's or a moment's, one a slice) */
static bool      g_gl_abort_next;                                        /* (the tests) the next splice refused: the fallback */
static uint16_t *g_gl_wrow;                                              /* per band row: up to two stale intervals x0,x1,x0,x1 (x0 > x1: none) - the window's columns */
static bool      g_gl_patch_page;                                        /* THE EDITOR: this rebake follows a placement page's DONE - the creatures stay hidden until the grass has settled */
static int       g_gl_page_closed;                                       /* (frames since a placement page closed, counting down) */
static int       g_gl_pcap = -1;                                        /* THE PATCH: the fine moment whose boxes the last frame painted live - its pixels and grass bits
                                                                            copied into g_gl_bake / g_gl_pmask, so its bake is the splice alone (-1: none) */
static void gl_patch_reset(void) { g_gl_patch = false; g_gl_nwin = 0; g_gl_pwin = 0; g_gl_pcap = -1; g_gl_patch_page = false; }
static void gl_ring_reset(void) { g_gl_dqn = 0; g_gl_dhead = g_gl_dtail = 0; g_gl_dused = 0; }
static void gl_meta_free(void) { if (g_gl_meta && g_gl_free) g_gl_free(g_gl_meta); g_gl_meta = NULL; g_gl_fgen = NULL; g_gl_dlen = NULL; g_gl_dq = NULL; g_gl_pm_old = NULL; g_gl_wrow = NULL; }
static int  gl_exact(void) {                                             /* moments baked at the look as it is */
    int r = 0, nf = g_gl_n * g_gl_f;
    if (!g_gl_fgen) return 0;
    for (int j = 0; j < nf; j++) r += g_gl_ok[j / g_gl_f] && g_gl_fok[j] == 1 && g_gl_fgen[j] == g_gl_gen;
    return r;
}
static bool gl_serving(void) {                                           /* every band and every moment there (at this look, or stale inside the boxes only) */
    int nf = g_gl_n * g_gl_f;
    if (!nf) return false;
    for (int j = 0; j < nf; j++) if (!g_gl_ok[j / g_gl_f] || g_gl_fok[j] != 1) return false;
    return true;
}
static size_t gl_ring_room(size_t *off) {                                /* the words free at the head */
    if (g_gl_dqn == 0) { g_gl_dhead = g_gl_dtail = 0; }
    *off = g_gl_dhead;
    return g_gl_dhead >= g_gl_dtail ? g_gl_dwords - g_gl_dhead : g_gl_dtail - g_gl_dhead;
}
static bool gl_ring_wrap(size_t *off, size_t *cap) {                     /* the head's room ran out at the arena's end: from 0 to the tail instead, when that is more */
    if (g_gl_dqn == 0 || g_gl_dhead < g_gl_dtail || g_gl_dtail <= *cap) return false;
    *off = 0; *cap = g_gl_dtail; return true;
}
static void gl_ring_commit(int j, size_t off, size_t len) {
    g_gl_doff[j] = (uint32_t)off; g_gl_dlen[j] = (uint32_t)len;
    g_gl_dq[g_gl_dqn++] = (uint8_t)j; g_gl_dhead = off + len; g_gl_dused += len;
    if (g_gl_dqn == 1) g_gl_dtail = off;
}
static void gl_ring_drop(int j) {
    int i = 0; while (i < g_gl_dqn && g_gl_dq[i] != j) i++;
    if (i == g_gl_dqn) return;
    g_gl_dused -= g_gl_dlen[j];
    for (; i + 1 < g_gl_dqn; i++) g_gl_dq[i] = g_gl_dq[i + 1];
    g_gl_dqn--;
    if (g_gl_dqn == 0) g_gl_dhead = g_gl_dtail = 0; else g_gl_dtail = g_gl_doff[g_gl_dq[0]];
}
/* the ring PACKED: every live difference moved down in the order written, the holes (places of differences written anew at the
   head) closed - when the head's room runs out (the order shown is not the ring's: a moment's new difference that outgrew its
   old place went to the head and left a hole, and the tail moves only as the oldest is replaced). Never while the ring is
   wrapped (then the order written is not the order in memory); ~480 KB moved once in a while, never a second copy */
static bool gl_ring_pack(void) {
    if (g_gl_dqn == 0 || g_gl_dhead < g_gl_dtail) return false;
    size_t at = 0;
    for (int i = 0; i < g_gl_dqn; i++) {
        const int j = g_gl_dq[i];
        if (g_gl_doff[j] != at) memmove(g_gl_diff + at, g_gl_diff + g_gl_doff[j], (size_t)g_gl_dlen[j] * 2);
        g_gl_doff[j] = (uint32_t)at; at += g_gl_dlen[j];
    }
    g_gl_dtail = 0; g_gl_dhead = at; g_gl_dused = at;
    return true;
}
static void gl_davg_update(void) { size_t tot = 0; for (int i = 0; i < g_gl_dqn; i++) tot += g_gl_dlen[g_gl_dq[i]]; if (g_gl_dqn) g_gl_davg = tot / (size_t)g_gl_dqn; }
/* a piece's box in the scene from its item, its centre and its depth (pieces_build's rule), with its contact shadow's rim */
static void pc_box_of(int item, int cx, int z, int *x0, int *y0, int *x1, int *y1) {
    int fy = DECOR_FY(z), w, h, extra = 4;
    switch (item) {
        case 2:  w = CASTLE_MASK_W; h = CASTLE_ROWS; extra = 12; break;
        case 3:  w = CORAL_W; h = CORAL_H; break;
        case 4:  w = CL_W; h = CL_H; break;
        case 9:  w = VO_W; h = VO_H; break;
        default: w = PK_W; h = PK_H; break;
    }
    *x0 = cx - w / 2; *y0 = fy - h; *x1 = *x0 + w - 1 + PC_SHADOW_FAR; *y1 = *y0 + h + extra - 1 + PC_SHADOW_FAR;
}
static uint32_t gl_skip_items(const tank_t *t, int b) {                  /* the items a bed's spans keep off (veg_skip_for, by item) */
    uint32_t m = veg_skip_for(t, b), r = 0;
    for (int k = 0; k < g_npc; k++) if (m >> k & 1) r |= 1u << g_pc[k].item;
    return r;
}
static void gl_state_record(const tank_t *t) {                           /* the look the loop is baked toward, for the next window */
    g_gl_sig = g_sig; g_gl_dim_baked = g_scene_dim; g_gl_step_baked = g_veg_step; g_gl_mcx_baked = g_veg_mask_cx; g_gl_ashade_baked = g_veg_arch_shade;
    for (int b = 0; b < VEG_BEDS_MAX; b++) {
        bool back = b < tank_veg_beds(t) && bed_z(t, b) == DECOR_Z_BACK;
        g_gl_bed[b].back = back;
        if (!back) continue;
        tank_veg_bed(t, b, &g_gl_bed[b].x0, &g_gl_bed[b].x1, NULL, &g_gl_bed[b].n);
        g_gl_bed[b].kind = tank_veg_kind(t, b); g_gl_bed[b].skip = gl_skip_items(t, b);
    }
}
/* a box into the window, merged with any it meets (a bed's strip, a piece's box; clipped to the band) - false: more boxes than GL_WIN_MAX */
static bool gl_rect_add(gl_rect_t *w, int *n, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (x1 > TANK_W - 1) x1 = TANK_W - 1;
    if (y0 < g_gl_y0) y0 = g_gl_y0;
    if (y1 > TANK_H - 1) y1 = TANK_H - 1;
    if (x0 > x1 || y0 > y1) return true;                                 /* (nothing of it in the band) */
    for (;;) {
        int i = 0;
        while (i < *n && !(x0 <= w[i].x1 + 1 && x1 >= w[i].x0 - 1 && y0 <= w[i].y1 + 1 && y1 >= w[i].y0 - 1)) i++;
        if (i == *n) break;
        if (w[i].x0 < x0) x0 = w[i].x0;
        if (w[i].x1 > x1) x1 = w[i].x1;
        if (w[i].y0 < y0) y0 = w[i].y0;
        if (w[i].y1 > y1) y1 = w[i].y1;
        w[i] = w[--*n];                                                  /* (the merged box can meet another: round again) */
    }
    if (*n >= GL_WIN_MAX) return false;
    w[*n].x0 = (short)x0; w[*n].y0 = (short)y0; w[*n].x1 = (short)x1; w[*n].y1 = (short)y1; (*n)++;
    return true;
}
static int gl_top(const tank_t *t);
/* the window's COLUMNS: per band row up to two stale intervals - what a box's repaint, the frame's and the bake's, and the splice
   really touch. A piece's are its mask's rows, the body's reach on each row and PC_SHADOW_FAR right and down for its contact
   shadow's rim (the runs a piece splits and shades lie there); a bed's strip or a piece without a mask to read is its box whole */
static void gl_wrow_reset(void) { if (g_gl_wrow) for (int r = 0; r < TANK_H; r++) { g_gl_wrow[r * 4] = 1; g_gl_wrow[r * 4 + 1] = 0; g_gl_wrow[r * 4 + 2] = 1; g_gl_wrow[r * 4 + 3] = 0; } }
static void gl_wrow_add(int y, int x0, int x1) {
    if (!g_gl_wrow || y < g_gl_y0 || y >= g_gl_y0 + g_gl_rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 > TANK_W - 1) x1 = TANK_W - 1;
    if (x0 > x1) return;
    uint16_t *iv = g_gl_wrow + (y - g_gl_y0) * 4;
    int a[3], b[3], n = 0;                                               /* the row's intervals and the new one, sorted; the nearest pair merged while there are three */
    for (int q = 0; q < 4; q += 2) if (iv[q] <= iv[q + 1]) { a[n] = iv[q]; b[n] = iv[q + 1]; n++; }
    a[n] = x0; b[n] = x1; n++;
    for (int i = 1; i < n; i++) { int ta = a[i], tb = b[i], j = i; while (j > 0 && a[j - 1] > ta) { a[j] = a[j - 1]; b[j] = b[j - 1]; j--; } a[j] = ta; b[j] = tb; }
    for (;;) {                                                           /* overlapping or touching: one */
        bool again = false;
        for (int i = 0; i + 1 < n && !again; i++) if (a[i + 1] <= b[i] + 1) { if (b[i + 1] > b[i]) b[i] = b[i + 1]; for (int k = i + 1; k + 1 < n; k++) { a[k] = a[k + 1]; b[k] = b[k + 1]; } n--; again = true; }
        if (!again) break;
    }
    while (n > 2) {                                                      /* three apart: the nearest two become one */
        int best = 0; for (int i = 1; i + 1 < n; i++) if (a[i + 1] - b[i] < a[best + 1] - b[best]) best = i;
        b[best] = b[best + 1]; for (int k = best + 1; k + 1 < n; k++) { a[k] = a[k + 1]; b[k] = b[k + 1]; } n--;
    }
    iv[0] = (uint16_t)a[0]; iv[1] = (uint16_t)b[0];
    if (n > 1) { iv[2] = (uint16_t)a[1]; iv[3] = (uint16_t)b[1]; } else { iv[2] = 1; iv[3] = 0; }
}
static const uint16_t *gl_wrow_of(int y) { return g_gl_wrow && y >= g_gl_y0 && y < g_gl_y0 + g_gl_rows ? g_gl_wrow + (y - g_gl_y0) * 4 : NULL; }
static void gl_wrow_rect(int x0, int y0, int x1, int y1) { for (int y = y0; y <= y1; y++) gl_wrow_add(y, x0, x1); }
/* a piece's footprint from its mask (rows of `w` px from (x0, y0), `nw` words a row): each row's reach, with the rim */
static void gl_wrow_mask(const uint32_t *m, int nw, int rows, int x0, int y0, int w) {
    for (int r = 0; r < rows; r++) {
        const uint32_t *row = m + (size_t)r * nw; int lo = -1, hi = -1;
        for (int wd = 0; wd < nw; wd++) { if (!row[wd]) continue; int a = wd * 32 + __builtin_ctz(row[wd]), b = wd * 32 + 31 - __builtin_clz(row[wd]); if (lo < 0) lo = a; hi = b; }
        if (lo < 0 || lo >= w) continue;
        if (hi > w - 1) hi = w - 1;
        for (int d = 0; d <= PC_SHADOW_FAR; d++) gl_wrow_add(y0 + r + d, x0 + lo, x0 + hi + PC_SHADOW_FAR);
    }
}
/* a piece's columns into the window: its mask's (the one in ds()->pm for the scene as it is, g_gl_pm_old's for the one before it -
   when that is the loop's) or, with no mask to read, its box whole */
static void gl_wrow_piece(int item, int cx, int z, bool old) {
    int x0, y0, x1, y1; pc_box_of(item, cx, z, &x0, &y0, &x1, &y1);
    const int w = x1 - x0 + 1 - PC_SHADOW_FAR, rows = y1 - y0 + 1 - PC_SHADOW_FAR;
    if (item == 2) { gl_wrow_mask(&g_castle_mask[0][0], 224 / 32, CASTLE_ROWS + 12, x0, y0, w); return; }
    const uint32_t *m = NULL;
    if (!old) m = &ds()->pm[pc_mslot(item)][0][0];
    else if (g_gl_pm_old && !memcmp(&g_gl_pm_old_sig, &g_gl_sig, sizeof g_gl_sig)) m = g_gl_pm_old + (size_t)pc_mslot(item) * (VO_H + 4) * ((CASTLE_MASK_W + 31) / 32);
    if (m) gl_wrow_mask(m, (CASTLE_MASK_W + 31) / 32, rows, x0, y0, w);
    else gl_wrow_rect(x0, y0, x1, y1);
}
/* the boxes the look changed inside since the loop was baked toward g_gl_sig (into w, on top of any there) - false: not a
   change of boxes (the dim, the grass in row pairs, the band's top; or more of them than the window holds, or most of
   the band): the loop is baked whole */
static bool gl_window(const tank_t *t, const uint8_t seg[VEG_BEDS_MAX][VEG_FRONDS_MAX], gl_rect_t *w, int *nw) {
    if (g_scene_dim != g_gl_dim_baked || g_veg_step != g_gl_step_baked || g_sig.front != g_gl_sig.front || g_sig.lx != g_gl_sig.lx) return false;
    if (gl_top(t) != g_gl_y0) return false;
    bool castle = false;
    /* the pieces, by ITEM: one whose place, depth, growth or color changed puts its old box and its new one in; one that left or
       came, its one box; one unchanged whose order against a piece whose box meets its own flipped (ranks swapped), both boxes.
       (By place in the list, as first written, a piece sold or hidden dragged every piece after it in: on Strato's square
       `pumpkin off` took the volcano's whole box with it - 32 ms a patched moment, 9 fps) */
    for (int item = 0; item < 16; item++) {
        int io = -1, ic = -1;
        for (int i = 0; i < g_gl_sig.n; i++) if (g_gl_sig.item[i] == item) io = i;
        for (int i = 0; i < g_sig.n; i++) if (g_sig.item[i] == item) ic = i;
        if (io < 0 && ic < 0) continue;
        bool changed = io < 0 || ic < 0 || g_sig.cx[ic] != g_gl_sig.cx[io] || g_sig.z[ic] != g_gl_sig.z[io] || g_sig.q[ic] != g_gl_sig.q[io] || g_sig.col[ic] != g_gl_sig.col[io];
        if (!changed) {
            int ax0, ay0, ax1, ay1; pc_box_of(item, g_sig.cx[ic], g_sig.z[ic], &ax0, &ay0, &ax1, &ay1);
            for (int j = 0; j < g_sig.n && !changed; j++) {
                if (j == ic) continue;
                int jo = -1; for (int i = 0; i < g_gl_sig.n; i++) if (g_gl_sig.item[i] == g_sig.item[j]) jo = i;
                if (jo < 0 || (j < ic) == (jo < io)) continue;               /* (a piece new to the scene: its own box holds what it paints over) */
                int bx0, by0, bx1, by1; pc_box_of(g_sig.item[j], g_sig.cx[j], g_sig.z[j], &bx0, &by0, &bx1, &by1);
                changed = bx0 <= ax1 && bx1 >= ax0 && by0 <= ay1 && by1 >= ay0;
            }
        }
        if (!changed) continue;
        int x0, y0, x1, y1;
        if (io >= 0) { pc_box_of(item, g_gl_sig.cx[io], g_gl_sig.z[io], &x0, &y0, &x1, &y1); if (!gl_rect_add(w, nw, x0, y0, x1, y1)) return false; gl_wrow_piece(item, g_gl_sig.cx[io], g_gl_sig.z[io], true); }
        if (ic >= 0) { pc_box_of(item, g_sig.cx[ic], g_sig.z[ic], &x0, &y0, &x1, &y1); if (!gl_rect_add(w, nw, x0, y0, x1, y1)) return false; gl_wrow_piece(item, g_sig.cx[ic], g_sig.z[ic], false); }
        castle |= item == 2;
    }
    if ((g_veg_mask_cx != g_gl_mcx_baked || g_veg_arch_shade != g_gl_ashade_baked) && !castle) return false;   /* (the arch's rules moved without the castle: not understood) */
    for (int b = 0; b < VEG_BEDS_MAX; b++) {                              /* the beds: one in or out of the back grass, moved, a frond more; the pieces its spans keep off */
        const bool back = b < tank_veg_beds(t) && bed_z(t, b) == DECOR_Z_BACK, was = g_gl_bed[b].back;
        float x0 = 0, x1 = 0; int nf = 0; uint32_t skip = 0;
        if (back) { tank_veg_bed(t, b, &x0, &x1, NULL, &nf); skip = gl_skip_items(t, b); }
        if (back != was || (back && (g_gl_bed[b].kind != (int)tank_veg_kind(t, b) || g_gl_bed[b].n != nf || g_gl_bed[b].x0 != x0 || g_gl_bed[b].x1 != x1))) {
            if (was && !gl_rect_add(w, nw, (int)g_gl_bed[b].x0 - GL_BED_PAD, g_gl_y0, (int)g_gl_bed[b].x1 + GL_BED_PAD, TANK_H - 1)) return false;
            if (back && !gl_rect_add(w, nw, (int)x0 - GL_BED_PAD, g_gl_y0, (int)x1 + GL_BED_PAD, TANK_H - 1)) return false;
            if (was) gl_wrow_rect((int)g_gl_bed[b].x0 - GL_BED_PAD, g_gl_y0, (int)g_gl_bed[b].x1 + GL_BED_PAD, TANK_H - 1);
            if (back) gl_wrow_rect((int)x0 - GL_BED_PAD, g_gl_y0, (int)x1 + GL_BED_PAD, TANK_H - 1);
            continue;
        }
        if (!back) continue;
        for (uint32_t d = skip ^ g_gl_bed[b].skip; d; d &= d - 1) {       /* a piece newly kept off or no longer: its box, where it stood and where it stands */
            const int item = __builtin_ctz(d); int bx0, by0, bx1, by1;
            for (int i = 0; i < g_sig.n; i++) if (g_sig.item[i] == item) { pc_box_of(item, g_sig.cx[i], g_sig.z[i], &bx0, &by0, &bx1, &by1); if (!gl_rect_add(w, nw, bx0, by0, bx1, by1)) return false; gl_wrow_piece(item, g_sig.cx[i], g_sig.z[i], false); }
            for (int i = 0; i < g_gl_sig.n; i++) if (g_gl_sig.item[i] == item) { pc_box_of(item, g_gl_sig.cx[i], g_gl_sig.z[i], &bx0, &by0, &bx1, &by1); if (!gl_rect_add(w, nw, bx0, by0, bx1, by1)) return false; gl_wrow_piece(item, g_gl_sig.cx[i], g_gl_sig.z[i], true); }
        }
        for (int i = 0; i < nf && i < VEG_FRONDS_MAX; i++)                 /* a frond taller or shorter: its strip */
            if (seg[b][i] != g_gl_seg[b][i]) { float bx; tank_veg_frond(t, b, i, &bx); if (!gl_rect_add(w, nw, (int)bx - GL_BED_PAD, g_gl_y0, (int)bx + GL_BED_PAD, TANK_H - 1)) return false; gl_wrow_rect((int)bx - GL_BED_PAD, g_gl_y0, (int)bx + GL_BED_PAD, TANK_H - 1); }
    }
    if (!*nw) return false;                                              /* (the look changed and no box says where: not understood) */
    long area = 0;
    for (int r = 0; r < g_gl_rows; r++) { const uint16_t *iv = g_gl_wrow + r * 4; for (int q = 0; q < 4; q += 2) if (iv[q] <= iv[q + 1]) area += iv[q + 1] - iv[q] + 1; }
    return area * 100 < (long)TANK_W * g_gl_rows * GL_WIN_MOST;
}
static uint32_t gl_stale_check(const tank_t *t) {
    static uint8_t seg[VEG_BEDS_MAX][VEG_FRONDS_MAX];
    memset(seg, 0, sizeof seg);
    uint32_t sh, hard = gl_key(t, &sh, seg);
    g_gl_full_now = hard ^ (sh * 2654435761u);
    if (hard != g_gl_hard_last) g_gl_still = 0;        /* (the heights alone - a nibble, growth - do not hold a bake up: THE HELD HEIGHTS keep them out of the look) */
    bool stale = hard != g_gl_key;
    if (sh == g_gl_seg_h) g_gl_seg_t = -1;               /* (the heights as baked: nothing waits) */
    else if (!stale) {                                   /* the heights moved: at once, or later */
        int nf = g_gl_n * g_gl_f, jump = 0;
        if (g_gl_seg_t < 0 || t->clock < g_gl_seg_t) g_gl_seg_t = t->clock;   /* (the wait starts with the change) */
        for (int b = 0; b < VEG_BEDS_MAX; b++) for (int i = 0; i < VEG_FRONDS_MAX; i++) { int d = (int)seg[b][i] - (int)g_gl_seg[b][i]; if (d < 0) d = -d; if (d > jump) jump = d; }
        stale = !nf || !gl_serving() || jump >= GL_SEG_JUMP || t->clock - g_gl_seg_t >= GL_SEG_WAIT_S;   /* (a loop stale inside its boxes is serving: it waits like a whole one) */
    }
    if (stale) {
        /* THE PATCH: a loop that is whole (or stale inside boxes already) and a change that is boxes keeps its bands and differences */
        gl_rect_t w[GL_WIN_MAX]; int nw = 0;
        bool partial = g_gl_n > 0 && g_gl_fgen && (g_gl_patch || gl_serving());
        if (partial && !g_gl_patch) gl_wrow_reset();                        /* (a rebake under way keeps its columns: they stay stale) */
        if (partial && g_gl_patch) for (int i = 0; i < g_gl_nwin && partial; i++) partial = gl_rect_add(w, &nw, g_gl_win[i].x0, g_gl_win[i].y0, g_gl_win[i].x1, g_gl_win[i].y1);   /* (a rebake under way: its boxes stay stale) */
        if (partial && g_gl_dqn > 0 && g_gl_dwords - g_gl_dused < 3 * (g_gl_dused / (size_t)g_gl_dqn)) partial = false;   /* the ring with room for fewer than three differences: whole
                                                                                    again, packed from 0 (a share of the arena was wrong: on Strato's square the headroom rule
                                                                                    left the arena at 91% full from the first bake and every move baked whole at 9 fps) */
        if (partial) partial = gl_window(t, seg, w, &nw);
        g_gl_key = hard; g_gl_still = 0; g_gl_gen++;
        memcpy(g_gl_seg, seg, sizeof g_gl_seg); g_gl_seg_h = sh; g_gl_seg_t = -1; g_gl_seg_ok = true;
        if (partial) { memcpy(g_gl_win, w, sizeof w); g_gl_nwin = nw; g_gl_patch = true; g_gl_pwin = 0; g_gl_pcap = -1; if (g_gl_page_closed) g_gl_patch_page = true; }   /* (a frame's captured boxes were the old window's) */
        else {
            for (int k = 0; k < GL_MAX; k++) g_gl_ok[k] = false;
            memset(g_gl_fok, 0, sizeof g_gl_fok); gl_ring_reset(); gl_patch_reset();
        }
        gl_state_record(t);
        g_gl_bs.phase = 0;                                               /* (a sliced bake under way starts over) */
    }
    return hard;
}
/* fine moments a band: GL_FINE by the clock; stepping, the sway's moments over the bands the memory held - so each board's
   sway keeps its period at its frame rate (measured: the 1.8 8 bands x 11 = 88, the pendant 7 x 12 = 84) - no more than
   the differences have fit (g_gl_fcap) */
static int gl_pick_f(int bands) {
    if (!g_gl_lock || bands <= 0) return GL_FINE;
    int f = (g_gl_lock + bands / 2) / bands;
    if ((!g_gl_fcap_n || g_gl_fcap_n == bands) && f > g_gl_fcap) f = g_gl_fcap;   /* (an overflow's cap holds at the count it was measured at) */
    if (g_gl_fsel_n == bands && f > g_gl_fsel) f = g_gl_fsel;
    return f < 2 ? 2 : f;
}
#define GL_TOP_ROOM 16                             /* rows the band takes above the tallest frond's tip (THE PATCH: a frond a segment taller - every
                                                      half hour on a growing tank - stays boxes for the loop, not a new band; the band never shrinks) */
static int gl_top(const tank_t *t) {               /* the band's first row */
    int top = TANK_H;
    g_gl_top_bed = -1; g_gl_top_segs = 0; g_gl_top_n = tank_veg_beds(t);
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (bed_z(t, b) != DECOR_Z_BACK) continue;
        int n; tank_veg_bed(t, b, NULL, NULL, NULL, &n);
        for (int i = 0; i < n; i++) {
            float bx; int segs = tank_veg_frond(t, b, i, &bx);
            if (segs < 1) segs = 1;
            if (segs > VEG_MAX_SEGS) segs = VEG_MAX_SEGS;
            int y = (int)(TANK_BOT - 16 - (segs - 1) * VEG_SEG_DY - VEG_SEG_RY) - 3;
            if (y < top) { top = y; g_gl_top_bed = b; g_gl_top_segs = segs; }
        }
    }
    if (top >= TANK_H) top = TANK_H - 16;
    if (g_gl_n > 0 && top >= g_gl_y0) return g_gl_y0;   /* the band as it is holds the grass */
    top -= GL_TOP_ROOM;
    if (top < 0) top = 0;
    while (top > 0 && (top * TANK_W * 2) % 64) top--;   /* the DMA's copies start on a 64-byte line */
    return top;
}
#define GL_META_TAB   ((GL_MAX * GL_FMAX) * (2 + 4 + 1))                /* THE PATCH's tables: each fine moment's generation, its difference's length, the ring's order ... */
#define GL_META_BYTES ((GL_META_TAB + GL_PM_BYTES + (size_t)TANK_H * 8 + 63) & ~(size_t)63)   /* ... the pieces' masks as baked (GL_PM_BYTES, with the scene's signature) and the window's columns per band row */
static bool gl_geometry(const tank_t *t) {         /* the band the back grass needs, and memory for the loop */
    const int top = gl_top(t);
    int rows = TANK_H - top;
    if (rows > g_gl_cap) {                           /* (taller grass: the loop again, with room to grow) */
        gl_release();
        if (g_gl_nomem && rows >= g_gl_nomem && g_gl_nomem_t >= 0 && t->clock >= g_gl_nomem_t && t->clock - g_gl_nomem_t < GL_RETRY_S)
            return false;                            /* (a band this tall or taller found no memory: a smaller one is tried at once, the same again
                                                        GL_RETRY_S later - 0.3.9.4: a first plan at a boot saw a band of
                                                        428 rows on Strato's square and every right-sized one after it was refused) */
        int cap = rows + 24 > TANK_H ? TANK_H : rows + 24;
        size_t band = (((size_t)cap * TANK_W * 2) + 127) & ~(size_t)63, mask = (size_t)cap * DIRTY_WORDS_PER_ROW * 4;
        size_t diff = band / 20;                     /* a fine moment's difference, about (they ran ~6 KB a 206 KB band on the square): the plan's floor */
        size_t dwant = diff;                         /* ... and what the last whole loop measured, a quarter over: the room asked for when there is room.
                                                        (A first cut made the measure the FLOOR too: after a replan on Strato's square the arena at that floor
                                                        failed the PSRAM headroom rule, the plan gave up with four bands fewer, and the loop was gone for the
                                                        day - the grass live at 11 fps and no one the wiser until the log) */
        if (g_gl_davg && g_gl_davg * 2 * 5 / 4 > dwant) dwant = g_gl_davg * 2 * 5 / 4;
        long n = ((long)g_gl_budget - (long)(band + mask)) / (long)(band + mask + (GL_FINE - 1) * diff);   /* (less a fine moment's bake) */
        if (g_gl_lock && n > (g_gl_lock + GL_FINE - 1) / GL_FINE) n = (g_gl_lock + GL_FINE - 1) / GL_FINE;   /* (stepping: the moments a sway wants, no more) */
        if (n > GL_MAX) n = GL_MAX;
        if (g_gl_lock && n == GL_MIN_BANDS + 1) {    /* (only three or four: a taller choice re-planned the pendant 48 -> 36, the 1.8 88 -> 84 - more bands
                                                        than memory holds, dropped again, fragment the arena's room) THE MOST MOMENTS (0.4, 11-01): a band costs a dozen moments' differences - on a tall bed (the square's
                                                        403 rows) four bands left the arena two moments a band, three would hold nine. So the count of bands
                                                        whose moments, as the budget holds them, are the most; on short grass that is the most bands, as before */
            const size_t dd = g_gl_fit_avg > diff ? g_gl_fit_avg : diff;   /* (a moment's difference: measured at an overflow, or the plan's floor - the
                                                        estimate the arena is sized by: a more careful one took 36 where 48 fit) */
            long best = -1; int bn = (int)n, bf = 0;
            for (int k = (int)n; k >= GL_MIN_BANDS; k--) {
                long room = (long)g_gl_budget - (long)(k + 1) * (long)(band + mask);
                if (room <= 0) continue;
                long f = 1 + room / ((long)k * (long)dd), fp = (g_gl_lock + k / 2) / k;
                if (fp > GL_FMAX) fp = GL_FMAX;
                if (f > fp) f = fp;
                if (k == g_gl_fcap_n && f > g_gl_fcap) f = g_gl_fcap;   /* (an overflow at this many: what fit) */
                if (f >= 2 && k * f > best) { best = k * f; bn = k; bf = (int)f; }
            }
            if (best > 0) { n = bn; g_gl_fsel = bf; g_gl_fsel_n = bn; }   /* (gl_pick_f: the moments a band the budget holds at this many) */
        }
        if (!(g_gl_meta = g_gl_alloc(GL_META_BYTES))) { g_gl_nomem = rows; g_gl_nomem_t = t->clock; g_gl_why = 1; return false; }
        memset(g_gl_meta, 0, GL_META_BYTES);
        g_gl_fgen = (uint16_t *)g_gl_meta; g_gl_dlen = (uint32_t *)((uint8_t *)g_gl_meta + GL_MAX * GL_FMAX * 2); g_gl_dq = (uint8_t *)g_gl_meta + GL_MAX * GL_FMAX * 6;
        g_gl_pm_old = (uint32_t *)(((uintptr_t)((uint8_t *)g_gl_meta + GL_META_TAB) + 3) & ~(uintptr_t)3); g_gl_wrow = (uint16_t *)((uint8_t *)g_gl_pm_old + GL_PM_BYTES);
        memset(&g_gl_pm_old_sig, 0xff, sizeof g_gl_pm_old_sig);
        g_gl_bake = g_gl_alloc(band); g_gl_bmask = g_gl_alloc(mask); g_gl_pmask = g_gl_alloc(mask);
        int got = 0;
        if (g_gl_bake && g_gl_bmask && g_gl_pmask)   /* as many bands as the platform spares */
            for (; got < n; got++) {
                g_gl_band[got] = g_gl_alloc(band); g_gl_mask[got] = g_gl_alloc(mask);
                if (!g_gl_band[got] || !g_gl_mask[got]) break;
            }
        if (got < n && got >= 0) { if (g_gl_band[got]) g_gl_free(g_gl_band[got]); if (g_gl_mask[got]) g_gl_free(g_gl_mask[got]); g_gl_band[got] = NULL; g_gl_mask[got] = NULL; }
        g_gl_f = gl_pick_f(got);
        size_t dmin = (size_t)got * (g_gl_f - 1) * diff, dbytes = (size_t)g_gl_budget - (size_t)(got + 1) * (band + mask);   /* the differences: the rest of
                                                                                    the budget (a tall bed's run bigger), at least the estimate */
        if ((long)g_gl_budget < (long)((got + 1) * (band + mask)) || dbytes < dmin) dbytes = dmin;
        if (dbytes < (size_t)got * (g_gl_f - 1) * dwant) dbytes = (size_t)got * (g_gl_f - 1) * dwant;   /* (the measure's room, when the headroom allows: the shrink below stops at the floor) */
        while (got >= GL_MIN_BANDS) {                /* (no room for them: fewer moments a band, then a band fewer, and try again) */
            while (!(g_gl_diff = g_gl_alloc(dbytes)) && dbytes > dmin) dbytes = dbytes / 4 * 3 < dmin ? dmin : dbytes / 4 * 3;   /* (the floor itself
                                                        tried last - 0.4, 11-01: it stopped a quarter above it, and the square's grass grown a few rows
                                                        more found no loop where its floor would have fit) */
            if (g_gl_diff) break;
            if (g_gl_lock && got == GL_MIN_BANDS && g_gl_f > GL_F_LEAST) {   /* (at the fewest bands, stepping: fewer moments a band, held at the
                                                        sway's pace (THE PACE) - above it a band fewer keeps more moments, as before: the pendant 48, not 30) */
                g_gl_f--; g_gl_fcap = g_gl_f; g_gl_fcap_n = got; dmin = (size_t)got * (g_gl_f - 1) * diff; dbytes = dmin;
                continue;
            }
            got--; g_gl_free(g_gl_band[got]); g_gl_free(g_gl_mask[got]); g_gl_band[got] = NULL; g_gl_mask[got] = NULL;
            g_gl_f = gl_pick_f(got); dmin = (size_t)got * (g_gl_f - 1) * diff; dbytes = dmin + (band + mask);
        }
        if (got < GL_MIN_BANDS || !g_gl_diff) { g_gl_why = !g_gl_bake || !g_gl_bmask || !g_gl_pmask ? 2 : 3; gl_release(); g_gl_nomem = rows; g_gl_nomem_t = t->clock; return false; }   /* too few to sway smoothly: the grass is drawn the old way */
        for (int k = 0; k < got; k++)                 /* (the odd fronds' masks beside the bands, outside the budget: all or none) */
            if (!(g_gl_omask[k] = g_gl_alloc(mask))) { for (int j = 0; j <= k; j++) { if (g_gl_omask[j]) g_gl_free(g_gl_omask[j]); g_gl_omask[j] = NULL; } break; }
        g_gl_n = got; g_gl_cap = cap; g_gl_dwords = dbytes / 2; gl_ring_reset(); gl_patch_reset(); g_gl_bs.phase = 0;
        g_gl_nomem = 0; g_gl_why = 0;                 /* (planned: a retry that found its memory) */
    }
    if (top != g_gl_y0 || rows != g_gl_rows) { for (int k = 0; k < g_gl_n; k++) g_gl_ok[k] = false; memset(g_gl_fok, 0, sizeof g_gl_fok); gl_ring_reset(); gl_patch_reset(); g_gl_bs.phase = 0; }
    g_gl_y0 = top; g_gl_rows = rows;
    return g_gl_n > 0;
}
/* THE RUN (0.4, 2026-11-01): a difference is, per row, its count of runs, then each run as ONE header word - x (bits 0-8),
   length - 1 (bits 9-13), CARRIED (bit 14: its colors follow), BARE (bit 15: not grass at the moment) - and its colors when
   carried. A bare run's pixels are the scene's (a bake copies the scene and paints only grass), so it carries none and is laid
   from g_scene; were one ever not, it carries them. The runs average 1.2 px (a blade shifts a pixel: one bare, one grass, rarely
   side by side), and the old run - x, length, then colors - spent two words of header on each: the moments ~half the size now,
   twice as many in an arena (the square's tall grass: 15 moments, too steppy - docs/grass-loop-compact.md) */
#define GLR_X(h)     ((h) & 0x1ff)
#define GLR_LEN(h)   ((((h) >> 9) & 31) + 1)
#define GLR_CARRY    0x4000u
#define GLR_BARE     0x8000u
#define GLR_MAXLEN   32
#define GLR_WORDS(h) (1 + (((h) & GLR_CARRY) ? GLR_LEN(h) : 0))
_Static_assert(TANK_W <= 512, "a run's x is 9 bits");
static inline bool gl_run_put(uint16_t *dst, size_t cap, size_t *w, int *run, int *count, int x, uint16_t c, bool bare, uint16_t sc) {   /* pixel x onto the row's runs */
    const uint16_t fl = bare ? (c == sc ? GLR_BARE : (GLR_BARE | GLR_CARRY)) : GLR_CARRY;
    if (*run >= 0) {                                 /* (the open run - the last written - extended when it is the same kind, next to it, not full) */
        const uint16_t h = dst[*run];
        if ((h & (GLR_BARE | GLR_CARRY)) == fl && GLR_X(h) + GLR_LEN(h) == x && GLR_LEN(h) < GLR_MAXLEN) {
            if (fl & GLR_CARRY) { if (*w >= cap) return false; dst[(*w)++] = c; }
            dst[*run] = (uint16_t)(h + (1u << 9));
            return true;
        }
    }
    if (*w + 2 > cap) return false;
    *run = (int)*w; dst[(*w)++] = (uint16_t)(x | fl);
    if (fl & GLR_CARRY) dst[(*w)++] = c;
    (*count)++;
    return true;
}
/* fine moment j's difference from band k, from its bake (g_gl_bake, g_gl_pmask), as THE RUN has it. Rows [r0, r1) appended at w
   (0.3.9.4: a slice at a time): the new size in words, or -1 past `cap` */
static long gl_diff_encode_rows(uint16_t *dst, size_t cap, size_t w, int r0, int r1, int k) {
    for (int r = r0; r < r1; r++) {
        const uint32_t *ma = g_gl_pmask + r * DIRTY_WORDS_PER_ROW, *mb = g_gl_mask[k] + r * DIRTY_WORDS_PER_ROW;
        const uint16_t *pa = g_gl_bake + r * TANK_W, *pb = g_gl_band[k] + r * TANK_W, *ps = g_scene + (size_t)(g_gl_y0 + r) * TANK_W;
        if (w >= cap) return -1;
        size_t at = w++; int n = 0, run = -1;
        for (int wd = 0; wd < DIRTY_WORDS_PER_ROW; wd++) {
            uint32_t any = ma[wd] | mb[wd];          /* (off both masks both are the scene: the same) */
            if (!any) { run = -1; continue; }
            for (int x = wd * 32, e = x + 32 < TANK_W ? x + 32 : TANK_W; x < e; x++) {
                uint32_t bit = 1u << (x & 31); bool ga = ma[wd] & bit;
                if (!((any & bit) && (pa[x] != pb[x] || ga != (bool)(mb[wd] & bit)))) { run = -1; continue; }
                if (!gl_run_put(dst, cap, &w, &run, &n, x, pa[x], !ga, ps[x])) return -1;
            }
        }
        dst[at] = (uint16_t)n;
    }
    return (long)w;
}
static void gl_diff_lay_rows(uint16_t *row, int j) {   /* fine moment j over its band's rows (row: the band's first) */
    const uint16_t *p = g_gl_diff + g_gl_doff[j], *sc = g_scene + (size_t)g_gl_y0 * TANK_W;
    for (int r = 0; r < g_gl_rows; r++, row += TANK_W, sc += TANK_W)
        for (int n = *p++; n > 0; n--) {
            const uint16_t h = *p++; const int x = GLR_X(h), len = GLR_LEN(h);
            if (h & GLR_CARRY) { memcpy(row + x, p, (size_t)len * 2); p += len; }
            else memcpy(row + x, sc + x, (size_t)len * 2);   /* (bare: the scene's) */
        }
}
static void gl_diff_lay(uint16_t *fb, int j) { gl_diff_lay_rows(fb + (size_t)g_gl_y0 * TANK_W, j); }   /* ... over its band's frame */
static void gl_diff_mask(int j, int k) {           /* fine moment j's grass as a mask, in g_gl_bmask: band k's, its difference applied */
    memcpy(g_gl_bmask, g_gl_mask[k], (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4);
    const uint16_t *p = g_gl_diff + g_gl_doff[j];
    for (int r = 0; r < g_gl_rows; r++) {
        uint32_t *m = g_gl_bmask + r * DIRTY_WORDS_PER_ROW;
        for (int n = *p++; n > 0; n--) {
            const uint16_t h = *p; const int x = GLR_X(h), len = GLR_LEN(h); const bool bare = h & GLR_BARE; p += GLR_WORDS(h);
            for (int i = x; i < x + len; i++) { if (bare) m[i >> 5] &= ~(1u << (i & 31)); else m[i >> 5] |= 1u << (i & 31); }
        }
    }
}
bool render_grass_loop_parts(double clock, const uint16_t **scene, size_t *top_bytes, const uint16_t **band, size_t *band_off, size_t *band_bytes, unsigned *ep) {
    if (!g_gl_on || g_gl_bypass || g_gl_dark || g_gl_parked || !g_gl_n || !g_scene || !g_scene_epoch || g_gl_key != g_gl_hard_last) return false;
    int k = gl_base(gl_next(clock));
    if (!g_gl_ok[k]) return false;
    *scene = g_scene; *top_bytes = (size_t)g_gl_y0 * TANK_W * 2;
    *band = g_gl_band[k]; *band_off = *top_bytes; *band_bytes = (((size_t)g_gl_rows * TANK_W * 2) + 63) & ~(size_t)63;
    *ep = 0x40000000u | ((uint32_t)g_gl_ver[k] << 5) | (uint32_t)k;
    return true;
}
static float g_gl_dim_last = 1; static int g_gl_pkz_last;   /* (the last frame's, for a bake outside one) */
/* the bake's recording: the grass's pixels into a band-shaped mask (dm; om: the odd fronds'), none of the frame's marks
   (the band is not the frame - and a bake outside a frame must never see a frame's marks), the sway at moment j */
typedef struct { uint32_t *touched, *bgmask, *dirty, *om; const uint32_t *under; float force; bool rec, fbg; } gl_rec_t;
static void gl_rec_begin(gl_rec_t *s, uint32_t *dm, uint32_t *om, int j) {
    s->touched = g_touched; s->bgmask = g_bgmask; s->dirty = g_dirty; s->om = g_om; s->under = g_under; s->force = g_veg_clock_force; s->rec = g_bg_rec; s->fbg = g_fbg_on;
    g_touched = NULL; g_dirty = NULL; g_under = NULL; g_fbg_on = false;   /* (a frame's marks a span reads: none for a bake, as bake_scene has it) */
    g_bgmask = dm - g_gl_y0 * DIRTY_WORDS_PER_ROW; g_bg_rec = true; g_veg_arch_pass = 0;
    g_om = om ? om - g_gl_y0 * DIRTY_WORDS_PER_ROW : NULL;
    g_veg_clock_force = gl_ftime(j);
    g_veg_seg_hold = g_gl_seg_ok ? g_gl_seg : NULL;
}
static void gl_rec_end(const gl_rec_t *s) { g_veg_clock_force = s->force; g_bg_rec = s->rec; g_bgmask = s->bgmask; g_touched = s->touched; g_dirty = s->dirty; g_om = s->om; g_under = s->under; g_fbg_on = s->fbg; }
static void gl_bed_draw(ctx_t *c, const tank_t *t, int b, int pkz) {   /* one back bed into a band or a box, as the bake paints it */
    g_veg_skip = veg_skip_for(t, b);
    g_pk_shade_all = g_pk_lx >= 0 && (b == 3 ? tank_decor_above(t, 0, 8) : pkz == DECOR_Z_BACK);
    draw_veg(c, t, b, VEG_SEED[b], -1, true); g_pk_shade_all = false;
}
/* THE PATCH: box i of the window repainted at moment j into buf (its rows from buf_y0: a band's g_gl_y0, a frame's 0) - the new
   scene copied in, its grass bits off, the back grass of the moment drawn over it clipped to the box, recorded into dm (and om) */
static void gl_patch_rect(const tank_t *t, uint16_t *buf, int buf_y0, uint32_t *dm, uint32_t *om, int j, float dim, int pkz, int i, int half) {
    gl_rect_t rr = g_gl_win[i]; const gl_rect_t *r = &rr;                  /* (half 0 / 1: its upper / lower rows - a slice each; -1: whole) */
    if (half >= 0) { const int mid = (rr.y0 + rr.y1) / 2; if (half) { if (rr.y1 == rr.y0) return; rr.y0 = (short)(mid + 1); } else rr.y1 = (short)mid; }
    const int wds = r->x1 - r->x0 + 1;
    for (int y = r->y0; y <= r->y1; y++) {                                /* the row's stale columns inside the box: the new scene, the grass bits off */
        const uint16_t *iv = gl_wrow_of(y);
        if (!iv) continue;
        for (int q = 0; q < 4; q += 2) {
            const int a = iv[q] > r->x0 ? iv[q] : r->x0, b = iv[q + 1] < r->x1 ? iv[q + 1] : r->x1;
            if (a > b) continue;
            memcpy(buf + (size_t)(y - buf_y0) * TANK_W + a, g_scene + (size_t)y * TANK_W + a, (size_t)(b - a + 1) * 2);
            mask_clear_run(dm + (size_t)(y - g_gl_y0) * DIRTY_WORDS_PER_ROW, a, b);
            if (om) mask_clear_run(om + (size_t)(y - g_gl_y0) * DIRTY_WORDS_PER_ROW, a, b);
        }
    }
    gl_rec_t s; gl_rec_begin(&s, dm, om, j);
    g_bg_clip = true; g_bg_clip_r = *r; g_veg_cull = true; g_veg_rowclip = gl_wrow_of;
    ctx_t cb = { buf + (size_t)(r->y0 - buf_y0) * TANK_W + r->x0, TANK_W, dim, r->x0, r->y0, wds, r->y1 - r->y0 + 1 };
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (bed_z(t, b) != DECOR_Z_BACK) continue;
        float bx0, bx1; tank_veg_bed(t, b, &bx0, &bx1, NULL, NULL);
        if (bx1 + GL_BED_PAD < r->x0 || bx0 - GL_BED_PAD > r->x1) continue;   /* (the bed clear of the box) */
        gl_bed_draw(&cb, t, b, pkz);
    }
    g_bg_clip = false; g_veg_cull = false; g_veg_rowclip = NULL;
    gl_rec_end(&s);
}
/* THE SPLICE: fine moment j's new difference against band k from its old one - the rows outside the boxes word for word,
   inside them the old runs kept up to a box's edge and the box encoded afresh from g_gl_bake / g_gl_pmask (the moment's
   pixels and grass inside the boxes, gl_patch_rect). Its size in words, or -1 past `cap` */
static bool gl_enc_row(uint16_t *dst, size_t cap, size_t *w, int *count, int r, int a, int b, int k) {   /* row r, x in [a, b] */
    const uint32_t *ma = g_gl_pmask + r * DIRTY_WORDS_PER_ROW, *mb = g_gl_mask[k] + r * DIRTY_WORDS_PER_ROW;
    const uint16_t *pa = g_gl_bake + r * TANK_W, *pb = g_gl_band[k] + r * TANK_W;
    int run = -1;
    for (int x = a; x <= b; x++) {
        const uint32_t bit = 1u << (x & 31); const int wd = x >> 5;
        const uint32_t any = ma[wd] | mb[wd];
        if (!any) { run = -1; x |= 31; continue; }                     /* (no grass in the word in either: on to the next - most of a tall box is bare water) */
        if (!(any & bit)) { run = -1; continue; }
        const bool ga = ma[wd] & bit;
        if (pa[x] == pb[x] && ga == (bool)(mb[wd] & bit)) { run = -1; continue; }
        if (!gl_run_put(dst, cap, w, &run, count, x, pa[x], !ga, g_scene[(size_t)(g_gl_y0 + r) * TANK_W + x])) return false;
    }
    return true;
}
static bool gl_copy_runs(uint16_t *dst, size_t cap, size_t *w, int *count, const uint16_t *runs, int n, int lo, int hi) {   /* the old runs cut to [lo, hi] */
    for (int i = 0; i < n; i++) {
        const uint16_t h = runs[0]; const int x = GLR_X(h), len = GLR_LEN(h); const uint16_t fl = h & (GLR_BARE | GLR_CARRY); const uint16_t *px = runs + 1;
        runs += GLR_WORDS(h);
        const int s = x > lo ? x : lo, e = x + len - 1 < hi ? x + len - 1 : hi;
        if (s > e) continue;
        const size_t carried = (fl & GLR_CARRY) ? (size_t)(e - s + 1) : 0;
        if (*w + 1 + carried > cap) return false;
        dst[(*w)++] = (uint16_t)(s | fl | ((e - s) << 9));
        if (carried) { memcpy(dst + *w, px + (s - x), carried * 2); *w += carried; }
        (*count)++;
    }
    return true;
}
static long gl_diff_splice_rows(uint16_t *dst, size_t cap, size_t w, int r0, int r1, int k, const uint16_t *old0, size_t *op) {   /* rows [r0, r1) appended at w; *op: words of the old one read so far */
    const uint16_t *old = old0 + *op;
    for (int r = r0; r < r1; r++) {
        const int y = g_gl_y0 + r, n = *old++; const uint16_t *runs = old;
        size_t ow = 0; for (int i = 0; i < n; i++) ow += GLR_WORDS(runs[ow]);   /* the old row's words */
        old += ow;
        int ia[2 * GL_WIN_MAX], ib[2 * GL_WIN_MAX], m = 0;                           /* the row's stale columns INSIDE each box, by x: what a repaint painted (gl_patch_rect
                                                                                        clips the row's intervals to its box). A row keeps two intervals and three boxes in a
                                                                                        row merge the nearest two: encoded whole, the gap between those boxes - never painted
                                                                                        in g_gl_bake - went into the moment as stale pixels. Strato's square, 10-31 night: dark
                                                                                        vertical lines up the fronds that stood in such a gap, on their own after growth's
                                                                                        strips or after a move; `--selftest-grasspatch`, three fronds nibbled across the beds */
        { const uint16_t *iv = g_gl_wrow + r * 4;
          for (int i = 0; i < g_gl_nwin; i++) {
              const gl_rect_t *rc = &g_gl_win[i];
              if (y < rc->y0 || y > rc->y1) continue;
              for (int q = 0; q < 4; q += 2) {
                  if (iv[q] > iv[q + 1]) continue;
                  const int a = iv[q] > rc->x0 ? iv[q] : rc->x0, b = iv[q + 1] < rc->x1 ? iv[q + 1] : rc->x1;
                  if (a <= b && m < 2 * GL_WIN_MAX) { ia[m] = a; ib[m] = b; m++; }
              }
          }
          for (int i = 1; i < m; i++) { int ta = ia[i], tb = ib[i], j = i; while (j > 0 && ia[j - 1] > ta) { ia[j] = ia[j - 1]; ib[j] = ib[j - 1]; j--; } ia[j] = ta; ib[j] = tb; } }
        if (!m) { if (w + 1 + ow > cap) return -1; dst[w++] = (uint16_t)n; memcpy(dst + w, runs, ow * 2); w += ow; continue; }
        if (w >= cap) return -1;
        const size_t at = w++; int count = 0, c = 0;
        for (int i = 0; i < m; i++) {
            int a = ia[i], b = ib[i];
            if (a < c) a = c;                                                        /* (boxes never overlap: gl_rect_add merges them) */
            if (a > b) continue;
            if (a > c && !gl_copy_runs(dst, cap, &w, &count, runs, n, c, a - 1)) return -1;
            if (!gl_enc_row(dst, cap, &w, &count, r, a, b, k)) return -1;
            c = b + 1;
        }
        if (c <= TANK_W - 1 && !gl_copy_runs(dst, cap, &w, &count, runs, n, c, TANK_W - 1)) return -1;
        dst[at] = (uint16_t)count;
    }
    *op = (size_t)(old - old0);
    return (long)w;
}
/* the fine moments' room checked early in a whole bake: the first differences say what the rest will take, and a loop that
   will not fit is planned again NOW, with the count that fits - not after it ran out (0.3.9.3: f - 1 at the overflow, and
   the whole loop again, twice on Strato's square: 155 bakes) */
static int gl_fit_f(void) {                        /* fine moments a band whose differences fit the arena, from the live average */
    if (!g_gl_dqn) return g_gl_f - 1;
    size_t avg = g_gl_dused / (size_t)g_gl_dqn; if (!avg) avg = 1;
    long f = (long)(g_gl_dwords * 93 / 100 / (avg * (size_t)g_gl_n)) + 1;
    if (f >= g_gl_f) f = g_gl_f - 1;
    return f < 2 ? 2 : (int)f;
}
/* (an EARLY check from the first few differences replanned a loop that would have fit: the first moments baked after a reset
   are the ones farthest from their bands, the biggest differences - on Strato's square it replanned every 20 s after a
   settle, whole bakes at 8 fps. The check is the overflow's alone now, with the measured fit) */
#define GL_COPY_ROWS 40                            /* rows of the scene copied into a band in a slice (~38 KB on the square) */
#define GL_ENC_ROWS  40                            /* rows of a fine moment encoded in a slice */
/* one SLICE of the whole bake of THE GRASS LOOP's next band or fine moment due (the next one shown first): 2 = a moment finished
   with it, 1 = a slice done, 0 = nothing left to bake. The band (or g_gl_bake) is written in place over the slices: a band's
   g_gl_ok is off until it is done, a fine moment's g_gl_fok */
static int gl_bake_step(const tank_t *t, float dim, int pkz) {
    const int nf = g_gl_n * g_gl_f;
    if (g_gl_bs.phase == 0) {
        const int due = g_gl_lock && g_gl_cur >= 0 && g_gl_cur < nf ? (g_gl_cur + 4) % nf : gl_fine(t->clock + 0.25f);
        int k = gl_base(due), j = -1;
        if (g_gl_ok[k]) { k = -1; for (int i = 0; i < g_gl_n; i++) if (!g_gl_ok[i]) { k = i; break; } }
        if (k < 0) for (int i = 0; i < nf; i++) {
            int f = (due + i) % nf;
            if (g_gl_fok[f]) continue;
            if (f % g_gl_f == 0) { g_gl_fok[f] = 1; g_gl_fgen[f] = g_gl_bgen[f / g_gl_f]; continue; }   /* (a band's own moment: nothing to lay) */
            j = f; break;
        }
        if (k < 0 && j < 0) { gl_davg_update(); return 0; }
        g_gl_bs.k = k; g_gl_bs.j = j; g_gl_bs.phase = 1; g_gl_bs.row = 0; g_gl_bs.bed = 0; g_gl_bs.w = 0;
        memset(k >= 0 ? g_gl_mask[k] : g_gl_pmask, 0, (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4);
        if (k >= 0 && g_gl_omask[k]) memset(g_gl_omask[k], 0, (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4);
    }
    const int k = g_gl_bs.k, j = g_gl_bs.j, m = k >= 0 ? k * g_gl_f : j;
    uint16_t *dst = k >= 0 ? g_gl_band[k] : g_gl_bake; uint32_t *dm = k >= 0 ? g_gl_mask[k] : g_gl_pmask;
    const int64_t q0 = render_clock_us ? render_clock_us() : 0;
    int r = 1;
    if (g_gl_bs.phase == 1) {                                   /* the scene's rows copied in, a slice of rows at a time */
        const int r0 = g_gl_bs.row, r1 = r0 + GL_COPY_ROWS < g_gl_rows ? r0 + GL_COPY_ROWS : g_gl_rows;
        memcpy(dst + (size_t)r0 * TANK_W, g_scene + (size_t)(g_gl_y0 + r0) * TANK_W, (size_t)(r1 - r0) * TANK_W * 2);
        g_gl_bs.row = r1;
        if (r1 >= g_gl_rows) { g_gl_bs.phase = 2; g_gl_bs.bed = 0; }
    } else if (g_gl_bs.phase == 2) {                            /* the back grass of the moment: a bed's upper half, its lower half, a slice each */
        int b = g_gl_bs.bed / 2;
        while (b < tank_veg_beds(t) && bed_z(t, b) != DECOR_Z_BACK) { b++; g_gl_bs.bed = b * 2; }
        if (b < tank_veg_beds(t)) {
            const int half = g_gl_bs.bed & 1, ry0 = half ? g_gl_y0 + g_gl_rows / 2 : g_gl_y0, ry1 = half ? g_gl_y0 + g_gl_rows - 1 : g_gl_y0 + g_gl_rows / 2 - 1;
            gl_rec_t s; gl_rec_begin(&s, dm, k >= 0 ? g_gl_omask[k] : NULL, m);
            g_bg_clip = true; g_bg_clip_r.x0 = 0; g_bg_clip_r.x1 = TANK_W - 1; g_bg_clip_r.y0 = (short)ry0; g_bg_clip_r.y1 = (short)ry1; g_veg_cull = true;
            ctx_t cb = { dst + (size_t)(ry0 - g_gl_y0) * TANK_W, TANK_W, dim, 0, ry0, TANK_W, ry1 - ry0 + 1 };
            gl_bed_draw(&cb, t, b, pkz);
            g_bg_clip = false; g_veg_cull = false;
            gl_rec_end(&s);
            g_gl_bs.bed++;
        } else if (k >= 0) {                                    /* a band, done: its fine moments' differences were from the old one */
            g_gl_ok[k] = true; g_gl_ver[k]++; g_gl_bgen[k] = g_gl_gen; g_gl_fok[m] = 1; g_gl_fgen[m] = g_gl_gen;
            for (int i = -GL_DLO; i <= GL_DHI; i++) { int f = (m + i + nf) % nf; if (f != m) { if (g_gl_fok[f] == 1) gl_ring_drop(f); g_gl_fok[f] = 0; } }
            g_gl_bs.phase = 0; r = 2;
        } else { g_gl_bs.phase = 3; g_gl_bs.row = 0; g_gl_bs.w = 0; g_gl_bs.cap = gl_ring_room(&g_gl_bs.off); }
    } else {                                                    /* a fine moment: its difference from its band, a slice of rows at a time, at the ring's head */
        const int r0 = g_gl_bs.row, r1 = r0 + GL_ENC_ROWS < g_gl_rows ? r0 + GL_ENC_ROWS : g_gl_rows;
        long w = gl_diff_encode_rows(g_gl_diff + g_gl_bs.off, g_gl_bs.cap, g_gl_bs.w, r0, r1, gl_base(j));
        if (w < 0 && g_gl_bs.off == g_gl_dhead) {               /* no room at the arena's end: the rows so far moved to 0, where there is more */
            size_t off = g_gl_bs.off, cap = g_gl_bs.cap;
            if (gl_ring_wrap(&off, &cap) && g_gl_bs.w <= cap) {
                memmove(g_gl_diff + off, g_gl_diff + g_gl_bs.off, g_gl_bs.w * 2);
                g_gl_bs.off = off; g_gl_bs.cap = cap;
                w = gl_diff_encode_rows(g_gl_diff + off, cap, g_gl_bs.w, r0, r1, gl_base(j));
            }
        }
        if (w < 0) {                                            /* the arena is full: the moment shows its band's (stepping: a step the eye would catch - planned again, with the count that fits) */
            g_gl_fok[j] = 2; g_gl_bs.phase = 0; r = 2;
            if (g_gl_lock && g_gl_f > 2) { g_gl_fcap = gl_fit_f(); g_gl_fcap_n = g_gl_n; if (g_gl_dqn) g_gl_fit_avg = g_gl_dused / (size_t)g_gl_dqn * 2; g_gl_replan = true; }
        } else {
            g_gl_bs.w = (size_t)w; g_gl_bs.row = r1;
            if (r1 >= g_gl_rows) {
                gl_ring_commit(j, g_gl_bs.off, g_gl_bs.w); g_gl_fok[j] = 1; g_gl_fgen[j] = g_gl_gen;
                g_gl_bs.phase = 0; r = 2;
            }
        }
    }
    if (render_clock_us) g_gl_bake_us += render_clock_us() - q0;
    if (r == 2) g_gl_bakes++;
    return r;
}
static void gl_patch_abort(void) {                  /* the ring could not take a spliced difference: the fine moments are baked whole again (their bands stay, at this look) */
    for (int j = 0; j < g_gl_n * g_gl_f; j++) g_gl_fok[j] = j % g_gl_f == 0 ? 1 : 0;
    gl_ring_reset(); gl_patch_reset(); g_gl_bs.phase = 0;
}
/* one slice of THE PATCH's rebake: a band's box repainted in place (the bands first: a fine moment's new difference is from its
   new band), a fine moment's box into g_gl_bake, or its splice into the ring - the oldest difference first. 2 = a band or moment
   finished, 1 = a slice done, 0 = every moment at this look (the loop exact again) */
static int gl_patch_step(const tank_t *t, float dim, int pkz) {
    const int64_t q0 = render_clock_us ? render_clock_us() : 0;
    int r = 1, k = -1;
    for (int i = 0; i < g_gl_n && k < 0; i++) if (g_gl_bgen[i] != g_gl_gen) k = i;
    if (k >= 0) {
        gl_patch_rect(t, g_gl_band[k], g_gl_y0, g_gl_mask[k], g_gl_omask[k], k * g_gl_f, dim, pkz, g_gl_pwin / 2, g_gl_pwin & 1);
        if (++g_gl_pwin >= 2 * g_gl_nwin) { g_gl_pwin = 0; g_gl_bgen[k] = g_gl_gen; g_gl_ver[k]++; g_gl_fok[k * g_gl_f] = 1; g_gl_fgen[k * g_gl_f] = g_gl_gen; r = 2; }
    } else {
        const int nf = g_gl_n * g_gl_f;
        int j = -1;
        if (g_gl_pcap >= 0 && g_gl_pwin == 0 && g_gl_bs.phase != 3 && g_gl_fok[g_gl_pcap] == 1 && g_gl_fgen[g_gl_pcap] != g_gl_gen) j = g_gl_pcap;   /* the frame painted its boxes: the splice alone */
        else {                                                   /* else the next one due (the next shown first, as the whole bake), its boxes painted a half a slice */
            g_gl_pcap = -1;
            const int due = g_gl_lock && g_gl_cur >= 0 && g_gl_cur < nf ? (g_gl_cur + 1) % nf : gl_fine(t->clock + 0.25f);
            if (g_gl_pwin > 0 || g_gl_bs.phase == 3) j = g_gl_bs.j;   /* (the one under way) */
            else for (int i = 0; i < nf && j < 0; i++) { int f = (due + i) % nf; if (g_gl_fok[f] == 1 && f % g_gl_f != 0 && g_gl_fgen[f] != g_gl_gen) j = f; }
        }
        if (j < 0) { gl_patch_reset(); gl_davg_update(); if (g_gl_dhead >= g_gl_dtail) gl_ring_pack(); return 0; }   /* every moment at this look (and the ring packed: the splices' holes closed) */
        if (j != g_gl_pcap && g_gl_bs.phase != 3 && g_gl_pwin < 2 * g_gl_nwin) { g_gl_bs.j = j; gl_patch_rect(t, g_gl_bake, g_gl_y0, g_gl_pmask, NULL, j, dim, pkz, g_gl_pwin / 2, g_gl_pwin & 1); g_gl_pwin++; }
        else {                                                   /* the splice, GL_ENC_ROWS rows a slice, at the ring's head */
            const int kb = gl_base(j);
            if (g_gl_bs.phase != 3) {
                size_t off, cap = gl_ring_room(&off);
                if (cap < (size_t)g_gl_dlen[j] * 2 + 2048 && gl_ring_pack()) cap = gl_ring_room(&off);   /* room for one bigger than the old, or the holes closed first */
                if (cap < (size_t)g_gl_dlen[j] + 1024) gl_ring_wrap(&off, &cap);
                if (g_gl_abort_next || cap < (size_t)g_gl_dlen[j] + 1024) { g_gl_abort_next = false; g_gl_pwin = 0; g_gl_pcap = -1; gl_patch_abort(); r = 2; goto out; }
                g_gl_bs.phase = 3; g_gl_bs.j = j; g_gl_bs.row = 0; g_gl_bs.w = 0; g_gl_bs.off = off; g_gl_bs.cap = cap; g_gl_bs.op = 0;
            }
            const int r1 = g_gl_bs.row + GL_ENC_ROWS < g_gl_rows ? g_gl_bs.row + GL_ENC_ROWS : g_gl_rows;
            long w = gl_diff_splice_rows(g_gl_diff + g_gl_bs.off, g_gl_bs.cap, g_gl_bs.w, g_gl_bs.row, r1, kb, g_gl_diff + g_gl_doff[j], &g_gl_bs.op);
            if (w < 0) { g_gl_bs.phase = 0; g_gl_pwin = 0; g_gl_pcap = -1; gl_patch_abort(); r = 2; goto out; }
            g_gl_bs.w = (size_t)w; g_gl_bs.row = r1;
            if (r1 >= g_gl_rows) {
                g_gl_bs.phase = 0; g_gl_pwin = 0; g_gl_pcap = -1;
                if (g_gl_bs.w <= g_gl_dlen[j]) {                 /* it fits where the old one was: in place (the moments come in the order shown, not the ring's) */
                    memcpy(g_gl_diff + g_gl_doff[j], g_gl_diff + g_gl_bs.off, g_gl_bs.w * 2);
                    g_gl_dused -= g_gl_dlen[j] - g_gl_bs.w; g_gl_dlen[j] = (uint32_t)g_gl_bs.w;
                } else { gl_ring_drop(j); gl_ring_commit(j, g_gl_bs.off, g_gl_bs.w); }   /* (bigger: at the head; its old place a hole until the tail passes it or the ring is packed) */
                g_gl_fgen[j] = g_gl_gen;
                r = 2;
            }
        }
    }
out:
    if (render_clock_us) g_gl_bake_us += render_clock_us() - q0;
    if (r == 2) g_gl_bakes++;
    return r;
}
static int g_gl_budget_us;                      /* (the tests) a frame's budget for the slices, in place of GL_BAKE_US (0: as built) */
void render_grass_loop_debug_budget(int us) { g_gl_budget_us = us; }
#define GL_BAKE_US 4500                            /* a frame's budget for the bake's slices (the square's frame is ~70 ms: with the live grass or the boxes
                                                      repainted too it holds ~11 fps) ... */
#define GL_SLICES  32                              /* ... or, with no clock (the sim's tests), slices enough for a moment a frame, as before */
static bool g_gl_task;                          /* THE SECOND CORE (0.3.9.4, Strato: "there's got to be a better solution"): the platform bakes the slices on a
                                                   task of its own between the frames (render_grass_loop_bake_slice, under its render lock) - none at a frame's end */
void render_set_grass_loop_task(bool on) { g_gl_task = on; }
static void gl_bake_frame(const tank_t *t, float dim, int pkz) {   /* the end of a frame: slices until the budget is spent (at least one) */
    const int64_t t0 = render_clock_us ? render_clock_us() : 0;
    for (int slices = 0;;) {
        const int r = g_gl_patch ? gl_patch_step(t, dim, pkz) : gl_bake_step(t, dim, pkz);
        if (!r || g_gl_replan) break;
        slices++;
        if (render_clock_us ? render_clock_us() - t0 >= (g_gl_budget_us ? g_gl_budget_us : GL_BAKE_US) : slices >= GL_SLICES) break;
    }
}
/* LOADING (0.3.9.3): a tank's first seconds - its grass not yet baked - are covered by render_loading while the platform
   bakes it at full speed (render_grass_loop_bake_more) instead of under a stuttering tank. Not with the light off (the
   loop sits out), not when the platform could not spare the memory, and only until the loop is first whole */
bool render_grass_loop_warming(void) {
    if (!g_gl_on || g_gl_bypass || g_gl_dark || g_gl_warm || g_gl_nomem) return false;
    int n = g_gl_n * g_gl_f;
    if (n && render_grass_loop_ready() == n) { g_gl_warm = true; return false; }
    return true;
}
int render_grass_loop_bake_slice(const tank_t *t) {       /* THE SECOND CORE: one slice, between frames (the frame's look must have settled: the frame itself counts
                                                              GL_SETTLE); 2 = a moment done with it, 1 = a slice, 0 = nothing to bake (the night, a page's live piece, whole) */
    if (!g_gl_on || g_gl_bypass || g_gl_dark || g_gl_parked || !g_scene || g_gl_still < GL_SETTLE || !gl_geometry(t)) return 0;
    const int r = g_gl_patch ? gl_patch_step(t, g_gl_dim_last, g_gl_pkz_last) : gl_bake_step(t, g_gl_dim_last, g_gl_pkz_last);
    if (g_gl_replan) { g_gl_replan = false; gl_release(); g_gl_nomem = 0; }
    return r;
}
int render_grass_loop_bake_more(const tank_t *t, int n) {     /* up to n whole moments (bands or fine moments), every slice of each */
    if (!g_gl_on || g_gl_bypass || g_gl_dark || !g_scene || g_gl_still < GL_SETTLE || !gl_geometry(t)) return 0;
    int baked = 0;
    while (baked < n && !g_gl_replan) {
        const int r = g_gl_patch ? gl_patch_step(t, g_gl_dim_last, g_gl_pkz_last) : gl_bake_step(t, g_gl_dim_last, g_gl_pkz_last);
        if (!r) break;
        if (r == 2) baked++;
    }
    if (g_gl_replan) { g_gl_replan = false; gl_release(); g_gl_nomem = 0; }
    return baked;
}
/* (the tests) THE PATCH's state: whether the loop is stale inside boxes, and the boxes (x0, y0, x1, y1 each) */
bool render_grass_loop_patching(void) { return g_gl_patch; }
void render_grass_loop_debug_abort(void) { g_gl_abort_next = true; }
int  render_grass_loop_boxes(int box[][4]) {
    for (int i = 0; i < g_gl_nwin; i++) { box[i][0] = g_gl_win[i].x0; box[i][1] = g_gl_win[i].y0; box[i][2] = g_gl_win[i].x1; box[i][3] = g_gl_win[i].y1; }
    return g_gl_nwin;
}
/* (the tests) moment j as the loop holds it: its band's rows with its difference laid (px: g_gl_rows x TANK_W, the band's first
   row first) and its grass mask (the same rows, DIRTY_WORDS_PER_ROW words each) - false: not baked */
bool render_grass_loop_dump(int j, uint16_t *px, uint32_t *mask, int *y0, int *rows) {
    const int nf = g_gl_n * g_gl_f;
    if (j < 0 || j >= nf) return false;
    const int k = gl_base(j);
    if (!g_gl_ok[k] || g_gl_fok[j] != 1) return false;
    memcpy(px, g_gl_band[k], (size_t)g_gl_rows * TANK_W * 2);
    if (j != k * g_gl_f) { gl_diff_lay_rows(px, j); gl_diff_mask(j, k); memcpy(mask, g_gl_bmask, (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4); }
    else memcpy(mask, g_gl_mask[k], (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4);
    *y0 = g_gl_y0; *rows = g_gl_rows;
    return true;
}
/* THE BOOT SHOW (2026-10-31, after mysiak on Discord: "decorations with inhabitants would materialise as they are
   loaded"; common/boot_show.c plays it). The pieces in the tank, back to front as they are drawn */
int render_boot_pieces(const tank_t *t, int items[DECOR_SLOTS]) {
    pieces_build(t, true, true);
    for (int k = 0; k < g_npc; k++) items[k] = g_pc[k].item;
    return g_npc;
}
/* the day scene - water, sand, pieces - with the items in `hide` left out, into dst. It records the pieces' masks as
   it draws them, which the grass loop's bake reads: render_boot_scene_done puts the tank's own back */
void render_boot_scene(const tank_t *t, uint16_t *dst, uint32_t hide) {
    g_boot_hide = hide; pieces_build(t, true, true);
    int keep_lx = g_pk_lx; g_pk_lx = -1;
    bake_scene(t, dst, 1.0f);
    g_pk_lx = keep_lx; g_boot_hide = 0;
}
void render_boot_scene_done(const tank_t *t) {   /* (the scene baked again, as it was: its pieces' masks with it) */
    if (!g_scene || !g_sig_ok) return;
    pieces_build(t, true, g_scene_front);
    bake_scene(t, g_scene, g_scene_dim < 0 ? 1.0f : g_scene_dim);
}
/* the grass loop's baked band nearest the clock's moment: the scene with the back grass over it, rows [*y0, TANK_H).
   NULL while none is baked */
const uint16_t *render_boot_grass(const tank_t *t, int *y0) {
    if (!g_gl_on || g_gl_bypass || !g_gl_n || !g_gl_f) return NULL;
    int k0 = gl_base(gl_fine(t->clock));
    for (int d = 0; d <= g_gl_n / 2; d++) {
        int a = (k0 + d) % g_gl_n, b = (k0 - d + g_gl_n) % g_gl_n;
        if (g_gl_ok[a] && g_gl_band[a]) { *y0 = g_gl_y0; return g_gl_band[a]; }
        if (g_gl_ok[b] && g_gl_band[b]) { *y0 = g_gl_y0; return g_gl_band[b]; }
    }
    return NULL;
}
/* ... and with no loop to read (0.3.9.6: 0.3.9.5 parked the loop, and the show's grass with it; or the loop could not
   have its memory): the same thing drawn live into dst - the scene's rows from *y0 copied in, the back grass of the
   clock's moment painted over them, as the bake paints a band - the way render_tank draws it live when the loop is
   stale or off. dst is the show's own frame; nothing is recorded. false: no scene yet */
bool render_boot_grass_live(const tank_t *t, uint16_t *dst, int *y0) {
    if (!g_scene || !g_sig_ok || !g_scene_epoch) return false;
    int top = TANK_H;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        if (bed_z(t, b) != DECOR_Z_BACK) continue;
        int n; tank_veg_bed(t, b, NULL, NULL, NULL, &n);
        for (int i = 0; i < n; i++) {
            float bx; int segs = tank_veg_frond(t, b, i, &bx);
            if (segs < 1) segs = 1;
            if (segs > VEG_MAX_SEGS) segs = VEG_MAX_SEGS;
            int y = (int)(TANK_BOT - 16 - (segs - 1) * VEG_SEG_DY - VEG_SEG_RY) - 3;
            if (y < top) top = y;
        }
    }
    if (top >= TANK_H) top = TANK_H - 16;
    top -= GL_TOP_ROOM;
    if (top < 0) top = 0;
    memcpy(dst + (size_t)top * TANK_W, g_scene + (size_t)top * TANK_W, (size_t)(TANK_H - top) * TANK_W * sizeof(uint16_t));
    uint32_t *keep_t = g_touched, *keep_d = g_dirty, *keep_om = g_om; const uint32_t *keep_u = g_under;
    const bool keep_rec = g_bg_rec, keep_fbg = g_fbg_on; const int keep_top = g_sg_top; const float keep_force = g_veg_clock_force;
    g_touched = NULL; g_dirty = NULL; g_under = NULL; g_fbg_on = false; g_bg_rec = false; g_om = NULL; g_veg_arch_pass = 0; g_veg_clock_force = -1;
    g_bg_clip = true; g_bg_clip_r.x0 = 0; g_bg_clip_r.x1 = TANK_W - 1; g_bg_clip_r.y0 = (short)top; g_bg_clip_r.y1 = (short)(TANK_H - 1); g_veg_cull = true;
    ctx_t cb = { dst + (size_t)top * TANK_W, TANK_W, g_scene_dim < 0 ? 1.0f : g_scene_dim, 0, top, TANK_W, TANK_H - top };
    const int pkz = g_pk_k >= 0 ? g_pc[g_pk_k].z : DECOR_Z_BACK;
    for (int b = 0; b < tank_veg_beds(t); b++) if (bed_z(t, b) == DECOR_Z_BACK) gl_bed_draw(&cb, t, b, pkz);
    g_bg_clip = false; g_veg_cull = false;
    g_touched = keep_t; g_dirty = keep_d; g_om = keep_om; g_under = keep_u; g_bg_rec = keep_rec; g_fbg_on = keep_fbg; g_sg_top = keep_top; g_veg_clock_force = keep_force;
    *y0 = top;
    return true;
}
/* SWIM DEPTH (0.3.9.4, docs/swim-depth.md): one animal drawn at its own depth - against the back grass (dep: IN FRONT,
   BEHIND, THROUGH) and the pieces (pdep: IN FRONT, BEHIND) and a bed IN FRONT. Before it is drawn, what is under its box
   (last frame's drawn box bb with room for a frame's travel, or r round cx, cy) is kept; after, the grass's pixels it is
   behind come back (every frond, or the odd ones), and the pieces' it passes behind (pc_body_covers: a piece IN FRONT
   from the scene, one BEHIND from what was under it). IN FRONT of a piece IN FRONT, or of a bed IN FRONT, it marks its
   pixels (g_keep, g_fbg_*): the pieces' restore, their overlays, the arch's shade and the bed keep off them.
   The frame's masks: g_dm_* (set in render_tank). true: it drew - its box in bb */
static const uint32_t *g_dm_all, *g_dm_odd; static int g_dm_y0;
static bool g_dbg_grass_on; static const uint32_t *g_dbg_dm; static int g_dbg_dm_y0;   /* (tests: the frame's grass mask kept for render_debug_grass_at) */
static bool g_dbg_root_cut;                                                              /* (tests: the grass's put-back over an animal cut at the roots again, as 0.3.9.4 had it) */
void render_debug_root_cut(bool on) { g_dbg_root_cut = on; }
void render_debug_grass_mask(bool on) { g_dbg_grass_on = on; }
bool render_debug_grass_at(int x, int y) { return g_dbg_dm && (unsigned)x < (unsigned)TANK_W && y >= g_dbg_dm_y0 && y < TANK_H && ((g_dbg_dm[(y - g_dbg_dm_y0) * DIRTY_WORDS_PER_ROW + (x >> 5)] >> (x & 31)) & 1); }
static int g_fbed_n, g_fbed_x0[VEG_BEDS_MAX], g_fbed_x1[VEG_BEDS_MAX];
static uint32_t g_fbed_over;                         /* the pieces (g_pc bits) a bed IN FRONT stands before: ranked below it */
static bool g_dep_pieces;
static uint32_t g_dep_over;                          /* (the caller's, for one dep_draw: pieces that stand before it WHOLE whatever its depths - the seahorse's
                                                        bed's stack, 0.3.9.7 - put back from what was under it; cleared by dep_draw) */
static bool g_dbg_sh_stack_off;                      /* (tests: the seahorse's bed's stack ignored, as 0.3.9.6 drew it) */
void render_debug_seahorse_stack_off(bool on) { g_dbg_sh_stack_off = on; }
static bool g_dbg_castle_row_only;                 /* (tests / director: 0.3.9.6's castle - only its front row over an animal behind the grass) */
void render_debug_castle_row_only(bool on) { g_dbg_castle_row_only = on; }
static bool g_dbg_fish_slot_order;                 /* (tests: the animal pass by the old ranks and slots alone, whatever their depths - before the pendant's fix) */
void render_debug_fish_slot_order(bool on) { g_dbg_fish_slot_order = on; }
static bool g_dbg_dep_home_only;                   /* (tests: only the home fish's depths ask for the grass masks, as before 0.4) */
void render_debug_dep_home_only(bool on) { g_dbg_dep_home_only = on; }
/* an animal's LAYER in the animal pass, back to front (docs/render-stack.md ranks 4-7): g its depth against the grass, p
   against the pieces - or DEP_LANE, its back lane (behind the pieces IN FRONT, before the pieces BEHIND). One stack (tank.h
   depth_piece_cover): the pieces BEHIND under the grass, the pieces IN FRONT over it, so
     L0 behind the grass and the pieces BEHIND        (BEHIND / BEHIND)
     L1 behind the grass, before the pieces BEHIND    (BEHIND / IN FRONT, BEHIND / its lane)
     L2 through the grass                             (THROUGH, whatever p: behind every piece IN FRONT, before every one BEHIND)
     L3 before the grass, behind the pieces IN FRONT  (IN FRONT / BEHIND, IN FRONT / its lane)
     L4 before the grass and every piece              (IN FRONT / IN FRONT) */
static inline int anim_layer(int g, int p) {
    if (g == FISH_DEPTH_THROUGH) return 2;
    const bool lane = p > FISH_DEPTH_THROUGH;        /* (DEP_LANE and its kin) */
    if (g == FISH_DEPTH_BEHIND) return !lane && p == FISH_DEPTH_BEHIND ? 0 : 1;
    return lane || p == FISH_DEPTH_BEHIND ? 3 : 4;
}
static uint32_t veg_skip_for(const tank_t *t, int b);   /* (below) */
static uint32_t g_dep_over_row;                      /* (... and the castle's front row alone before it: the seahorse on its rear tower) */
static uint32_t sh_stack(const tank_t *t) {          /* THE BED'S STACK (tank.h sh_obed): the pieces before the bed of the frond it is coiled on;
                                                        THE PIECE'S STACK (sh_opost, 0.4): the pieces before the piece whose post it holds -
                                                        and the castle's front row before its rear tower (g_dep_over_row) */
    g_dep_over_row = 0;
    if (g_dbg_sh_stack_off) return 0;
    if (t->sh_obed >= 0 && t->sh_obed < tank_veg_beds(t)) return veg_skip_for(t, t->sh_obed);
    if (t->sh_opost < 0) return 0;
    uint32_t m = 0;
    for (int k = 0; k < g_npc && k < 32; k++) {
        if (!g_pc[k].baked && g_pc[k].item != 2) continue;              /* (drawn live this frame: over it anyway, or dragged) */
        if (g_pc[k].item == t->sh_opost) { if (t->sh_orow && g_pc[k].item == 2) g_dep_over_row |= 1u << k; continue; }
        if (tank_decor_above(t, g_pc[k].item, t->sh_opost)) m |= 1u << k;
    }
    return m;
}
/* THE CRAB'S STACK (Strato 2026-11-02, the watch): the crab roosting in a shelter is INSIDE it - at the shelter's place in the stack.
   The pieces ranked above its shelter (tank_decor_above) stand before it whole, the ones below stand behind it - the castle and the
   volcano (tank.c crab_cave takes the frontmost of those), and the coral, the cluster and the pumpkin too. The crab is painted only
   where none of the pieces before it covers (g_cr_clip, draw_crab: nothing to put back, the grass and the lantern's glow before those
   pieces stay as they are); the pieces IN FRONT behind it keep their restore (g_rs_crab) and their overlays (g_guard) off it */
static uint32_t crab_stack(const tank_t *t) {
    if (g_rs_old || t->cr_cave < 0 || !tank_decor_in(t, t->cr_cave)) return 0;
    uint32_t m = 0;
    for (int k = 0; k < g_npc && k < 32; k++) {
        if (!g_pc[k].baked && g_pc[k].item != 2) continue;              /* (drawn live this frame: over it anyway, or dragged) */
        if (g_pc[k].item != t->cr_cave && tank_decor_above(t, g_pc[k].item, t->cr_cave)) m |= 1u << k;
    }
    return m;
}
typedef void (*dep_draw_t)(ctx_t *c, const tank_t *t, int i);
/* (DEP_ROOT_Y is render.h's: the tests read it) */
#define DEP_LANE 0xff                                /* (pdep: an animal whose lane decides the pieces - the seahorse, the floor critters) */
#define DEP_NOBED 0xfe                               /* (... and inside a piece: a crab in its shelter - a bed IN FRONT stays over it) */
#define DEP_LANE_F 0xfd                              /* (... in its FRONT lane: before every piece IN FRONT; DEP_LANE is the back lane, behind them) */
static void dep_fish(ctx_t *c, const tank_t *t, int i) { draw_fish(c, t, &t->fish[i]); }
static void draw_seahorse(ctx_t *c, const tank_t *t);   /* below */
static void dep_seahorse(ctx_t *c, const tank_t *t, int i) { (void)i; draw_seahorse(c, t); }
static short g_hbb[4]; static bool g_hbb_ok;         /* the seahorse's drawn box, the last frame */
static void draw_crab(ctx_t *c, const tank_t *t);    /* below */
static void draw_urchin(ctx_t *c, const tank_t *t);
static void dep_crab(ctx_t *c, const tank_t *t, int i) { (void)i; draw_crab(c, t); }
static void dep_snail(ctx_t *c, const tank_t *t, int i) { (void)i; draw_snail(c, t, true); }
static void dep_urchin(ctx_t *c, const tank_t *t, int i) { (void)i; draw_urchin(c, t); }
static short g_cbb[4], g_snbb[4], g_ubb[4]; static bool g_cbb_ok, g_snbb_ok, g_ubb_ok;   /* the floor critters' drawn boxes */
#define DEP_CRAB(c, fb, t)   dep_draw((c), (fb), (t), 0, dep_crab, g_cbb, &g_cbb_ok, (t)->cr_x, CR_FLOOR_Y - (t)->cr_y - 12, 44, (t)->cr_gdepth, \
                                      ((t)->cr_mode == CR_ROOST || (t)->cr_mode == CR_SHELTER) && (t)->cr_find <= 0 ? DEP_NOBED : DEP_LANE)
#define DEP_SNAIL(c, fb, t)  dep_draw((c), (fb), (t), 0, dep_snail, g_snbb, &g_snbb_ok, (t)->snail_x, (t)->snail_y, 34, (t)->snail_gdepth, DEP_LANE)
#define DEP_URCHIN(c, fb, t) dep_draw((c), (fb), (t), 0, dep_urchin, g_ubb, &g_ubb_ok, (t)->urchin_x, URCHIN_FLOOR_Y + (t)->urchin_dy - 6, 32, (t)->urchin_gdepth, DEP_LANE)
/* ... in the FRONT slot (their front lane: before the pieces IN FRONT, so before the grass those stand before): in front of it */
#define DEP_CRAB_F(c, fb, t)   dep_draw((c), (fb), (t), 0, dep_crab, g_cbb, &g_cbb_ok, (t)->cr_x, CR_FLOOR_Y - (t)->cr_y - 12, 44, FISH_DEPTH_FRONT, \
                                        ((t)->cr_mode == CR_ROOST || (t)->cr_mode == CR_SHELTER) && (t)->cr_find <= 0 ? DEP_NOBED : DEP_LANE_F)
#define DEP_SNAIL_F(c, fb, t)  dep_draw((c), (fb), (t), 0, dep_snail, g_snbb, &g_snbb_ok, (t)->snail_x, (t)->snail_y, 34, FISH_DEPTH_FRONT, DEP_LANE_F)
#define DEP_URCHIN_F(c, fb, t) dep_draw((c), (fb), (t), 0, dep_urchin, g_ubb, &g_ubb_ok, (t)->urchin_x, URCHIN_FLOOR_Y + (t)->urchin_dy - 6, 32, FISH_DEPTH_FRONT, DEP_LANE_F)
static src_t g_shr_pal[SHRIMP_TONES];                /* (this frame's shrimp tones, for dep_shrimp) */
static void draw_shrimp(ctx_t *c, const shrimp_t *q, const src_t *pal);   /* below */
static void dep_shrimp(ctx_t *c, const tank_t *t, int i) { draw_shrimp(c, &t->shrimp[i], g_shr_pal); }
static short g_sbb[SHRIMP_MAX][4]; static bool g_sbb_ok[SHRIMP_MAX];   /* each shrimp's drawn box, the last frame */
static bool dep_draw(ctx_t *c, uint16_t *fb, const tank_t *t, int i, dep_draw_t draw, short bb[4], bool *bb_ok,
                     float cx, float cy, int r, int dep, int pdep) {
    const bool grass = dep != FISH_DEPTH_FRONT && g_dm_all;
    const uint32_t *dm_all = g_dm_all, *dm_odd = g_dm_odd; const int dm_y0 = g_dm_y0;
    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    uint32_t pk_behind = 0, pk_front = 0, pk_over = 0;   /* the pieces its box meets: to pass behind, or to stay in front of (IN FRONT ones);
                                                        whole before it (g_dep_over) */
    uint32_t pk_whole = 0;                           /* (of pk_behind: all of it over the animal, the castle's keep too - else its front row) */
    const uint32_t over_row = g_dep_pieces ? g_dep_over_row : 0; g_dep_over_row = 0;   /* (of over: by its front row - the castle's) */
    const uint32_t over = g_dep_pieces ? (g_dep_over | over_row) : 0; g_dep_over = 0;
    if (grass || (g_dep_pieces && pdep <= FISH_DEPTH_BEHIND) || over) {   /* its box: last frame's, with room for a frame's travel */
        if (*bb_ok) { bx0 = bb[0] - 8; by0 = bb[1] - 8; bx1 = bb[2] + 8; by1 = bb[3] + 8; }   /* (a dash is ~6 px a frame) */
        else { bx0 = (int)cx - r; bx1 = (int)cx + r; by0 = (int)cy - r; by1 = (int)cy + r; }
        if (bx0 < 0) bx0 = 0;
        if (bx1 > TANK_W - 1) bx1 = TANK_W - 1;
        if (by0 < 0) by0 = 0;
        if (by1 > TANK_H - 1) by1 = TANK_H - 1;
        if (g_dep_pieces && pdep <= FISH_DEPTH_BEHIND) for (int k = 0; k < g_npc && k < 32; k++) {   /* (pdep DEP_LANE: its lane decides the pieces) */
            const piece_t *p = &g_pc[k];
            if (!p->baked || p->x0 > bx1 || p->x0 + p->w <= bx0 || p->y0 > by1 || p->y0 + p->rows <= by0) continue;
            /* one stack, never a loop (Strato on the square 10-31: mira behind the grass but over the pumpkin IN FRONT - her fins
               between its edge and the fronds, bits of frond floating over her): a piece IN FRONT stands before the grass, so an
               animal behind or through the grass is behind it too; a piece BEHIND stands behind the grass, so one in front of
               the grass is in front of it too. Only the rest is the animal's choice */
            const int cover = depth_piece_cover(p->z, p->item == 2, dep, pdep);   /* (tank.h: behind a piece BEHIND only when it chose both,
                                                                         behind the whole castle IN FRONT when behind or through the grass; a depth
                                                                         change that changes this over a piece waits - tank.c pieces_keep) */
            if (cover) pk_behind |= 1u << k;
            else if (p->z == DECOR_Z_FRONT) pk_front |= 1u << k;
            if (cover == 2 && !(g_dbg_castle_row_only && p->item == 2)) pk_whole |= 1u << k;
        }
        for (int k = 0; over && k < g_npc && k < 32; k++) {   /* (whole before it: the castle's keep and towers too, not just its front row) */
            const piece_t *p = &g_pc[k];
            if (!(over >> k & 1) || p->x0 > bx1 || p->x0 + p->w <= bx0 || p->y0 > by1 || p->y0 + p->rows <= by0) continue;
            pk_over |= 1u << k; pk_behind &= ~(1u << k); pk_front &= ~(1u << k);
        }
        if (!grass && !pk_behind && !pk_front && !pk_over) bx1 = -1;   /* (nothing to be behind, nothing to stay in front of) */
        if (bx1 < bx0 || by1 < by0 || (bx1 - bx0 + 1) * (by1 - by0 + 1) > DEP_CAP_PX || by1 - by0 + 1 > DEP_CAP_ROWS) bx1 = -1;
    }
    const int bw = bx1 - bx0 + 1, w0 = bx0 >> 5, nw = bx1 >= 0 ? (bx1 >> 5) - w0 + 1 : 0;
    const bool keep_px = grass || pk_behind || pk_over;   /* (pixels to put back; only the dirty bits to stay in front) */
    int64_t dq0 = bx1 >= 0 ? PROF_MARK() : 0;
    if (bx1 >= 0) for (int y = by0; y <= by1; y++) {   /* what is under it, kept - the rows anything comes back to */
        if (g_dirty) memcpy(g_dep_dirty + (size_t)(y - by0) * DIRTY_WORDS_PER_ROW, g_dirty + (size_t)y * DIRTY_WORDS_PER_ROW + w0, (size_t)nw * 4);
        if (!keep_px) continue;
        if (!pk_behind && !pk_over) {                /* (behind the grass alone: the rows with grass in the box) */
            if (y < dm_y0) continue;
            const uint32_t *ma = dm_all + (size_t)(y - dm_y0) * DIRTY_WORDS_PER_ROW; uint32_t any = 0;
            for (int w = w0; w < w0 + nw; w++) any |= ma[w];
            if (!any) continue;
        }
        memcpy(g_dep_save + (size_t)(y - by0) * bw, fb + (size_t)y * TANK_W + bx0, (size_t)bw * 2);
    }
    int64_t dq1 = bx1 >= 0 ? PROF_MARK() : 0;
    if (bx1 >= 0 && pk_front && g_keep) {           /* in front of the pieces IN FRONT: its pixels kept from their restore and overlays */
        if (!g_keep_on) { memset(g_keep, 0, (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4); g_keep_on = true; }
        g_keep_rec = g_keep;
    }
    if (g_fbed_n && dep != FISH_DEPTH_BEHIND && pdep != DEP_NOBED && !(pk_behind & g_fbed_over) && g_fbg_front) {   /* a bed IN FRONT its box meets: in front
                                                                   of it, or through it (Strato 10-31, the snail on the square: the critters too) - not a crab in its
                                                                   shelter, nor an animal behind a piece the bed stands before (the bed, the piece, the animal: one stack) */
        int gx0 = (int)cx - r, gx1 = (int)cx + r;
        if (*bb_ok) { gx0 = bb[0] - 8; gx1 = bb[2] + 8; }
        bool meets = false;
        for (int q = 0; q < g_fbed_n && !meets; q++) meets = gx1 >= g_fbed_x0[q] && gx0 <= g_fbed_x1[q];
        if (meets && pdep == DEP_LANE)               /* a back lane: behind the pieces IN FRONT - so behind the bed where one it stands before is about */
            for (int k = 0; k < g_npc && k < 32 && meets; k++) {
                const piece_t *p = &g_pc[k];
                if ((g_fbed_over >> k & 1) && gx1 >= p->x0 && gx0 < p->x0 + p->w) meets = false;
            }
        if (meets) {
            if (!g_fbg_used) { memset(g_fbg_front, 0, (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 8); g_fbg_used = true; }   /* (both masks: they sit side by side) */
            g_keep_rec2 = dep == FISH_DEPTH_THROUGH ? g_fbg_thru : g_fbg_front;
        }
    }
    const bool bb_was = g_bb_on;                     /* (the front slot's clearing runs with it on) */
    g_bb_on = true; g_bb_x0 = g_bb_y0 = 1 << 20; g_bb_x1 = g_bb_y1 = -1;
    draw(c, t, i);
    g_bb_on = bb_was; g_keep_rec = g_keep_rec2 = NULL;
    int64_t dq2 = bx1 >= 0 ? PROF_MARK() : 0;
    const bool drew = g_bb_x1 >= g_bb_x0;
    if (drew) { bb[0] = (short)g_bb_x0; bb[1] = (short)g_bb_y0; bb[2] = (short)g_bb_x1; bb[3] = (short)g_bb_y1; }
    *bb_ok = drew;
    if (bx1 < 0) return drew;
    const bool through = dep == FISH_DEPTH_THROUGH && dm_odd;
    /* the grass's own pixels back over it, EVERY ROW - a frond before an animal is before all of it, its foot on the sand
       included. 0.3.9.4 cut the put-back at DEP_ROOT_Y so a critter on the sand kept its feet and shadow over the roots;
       for the urchin (and the snail, the crab, a fish ducked into the grass) that cut every frond mid-body: over its spines
       and the top of the dome, the lower dome over the frond's foot - "a grass frond that should be drawn behind the urchin
       overlaps a bit of the top of the urchin" (Strato on the square, 10-31 night; 0.3.9.6). The crab in a shelter BEHIND
       already came back whole (0.3.9.4 item 21) */
    const int y_cut = g_dbg_root_cut && pdep != DEP_NOBED ? DEP_ROOT_Y : TANK_H;   /* (the A/B: PT_ROOT_CUT=1 in the sim's tests - the 0.3.9.4 cut back; they fail) */
    if (grass) for (int y = by0 < dm_y0 ? dm_y0 : by0; y <= by1 && y < y_cut; y++) {
        const uint32_t *ma = dm_all + (size_t)(y - dm_y0) * DIRTY_WORDS_PER_ROW, *mo = through ? dm_odd + (size_t)(y - dm_y0) * DIRTY_WORDS_PER_ROW : NULL;
        const uint16_t *sv = g_dep_save + (size_t)(y - by0) * bw; uint16_t *row = fb + (size_t)y * TANK_W;
        const uint32_t *sd = g_dep_dirty + (size_t)(y - by0) * DIRTY_WORDS_PER_ROW; uint32_t *dr = g_dirty ? g_dirty + (size_t)y * DIRTY_WORDS_PER_ROW : NULL;
        for (int w = w0; w <= bx1 >> 5; w++) {        /* 32 at a time: only the grass's pixels the frame drew on (dirty) are looked at */
            uint32_t m = ma[w];
            if (mo) m &= mo[w];
            if (dr) m &= dr[w];
            if (w == w0) m &= 0xFFFFFFFFu << (bx0 & 31);
            if (w == bx1 >> 5) m &= 0xFFFFFFFFu >> (31 - (bx1 & 31));
            for (int k = 0; pk_whole && m && k < g_npc && k < 32; k++)   /* (a piece whole over it - the castle's keep for one behind the grass - comes back
                                                                            below, whole: its pixels are left to it) */
                if (pk_whole >> k & 1) m &= ~pc_row_bits(k, w << 5, 32, y);
            while (m) {
                int x = (w << 5) + __builtin_ctz(m); uint32_t bit = m & -m; m &= m - 1;
                if (row[x] == sv[x - bx0]) continue;
                row[x] = sv[x - bx0];
                if (dr && !(sd[w - w0] & bit)) dr[w] &= ~bit;   /* (a pixel that was final before the animal is final again: no second vignette) */
            }
        }
    }
    for (int k = 0; pk_behind && k < g_npc && k < 32; k++) {   /* the pieces it passes behind: theirs back over it - a piece IN FRONT from the scene
                                                                  (what was under it may be a critter in its arch), one BEHIND from what was under
                                                                  it (the back grass stands over it) */
        if (!(pk_behind >> k & 1)) continue;
        const piece_t *p = &g_pc[k];
        const bool from_scene = p->z == DECOR_Z_FRONT;
        int ya = p->y0 > by0 ? p->y0 : by0, yb = p->y0 + p->rows - 1 < by1 ? p->y0 + p->rows - 1 : by1;
        int xa = p->x0 > bx0 ? p->x0 : bx0, xb = p->x0 + p->w - 1 < bx1 ? p->x0 + p->w - 1 : bx1;
        const bool whole = pk_whole >> k & 1;
        for (int y = ya; y <= yb; y++) {
            const uint16_t *sv = g_dep_save + (size_t)(y - by0) * bw; uint16_t *row = fb + (size_t)y * TANK_W;
            const uint32_t *sd = g_dep_dirty + (size_t)(y - by0) * DIRTY_WORDS_PER_ROW; uint32_t *dr = g_dirty ? g_dirty + (size_t)y * DIRTY_WORDS_PER_ROW : NULL;
            for (int xw = xa & ~31; xw <= xb; xw += 32) {   /* 32 at a time: a piece whole by its mask's row, the castle's front row pixel by pixel */
              const int w = xw >> 5;
              uint32_t m = 0;
              if (whole) m = pc_row_bits(k, xw, 32, y);
              else for (int b = xw < xa ? xa - xw : 0; b < 32 && xw + b <= xb; b++) if (pc_body_covers(k, xw + b, y)) m |= 1u << b;
              if (xw < xa) m &= 0xFFFFFFFFu << (xa - xw);
              if (xw + 31 > xb) m &= 0xFFFFFFFFu >> (xw + 31 - xb);
              if (dr) m &= dr[w];
              while (m) {
                const int x = xw + __builtin_ctz(m); const uint32_t bit = m & -m; m &= m - 1;
                const bool scene_px = from_scene && (g_dep_keep_old || !keep_at(x, y));   /* a fish drawn before it IN FRONT of the pieces: what was there (that
                                                                       fish), not the scene - 0.3.9.6, Bananna's pendant: a resting fish IN FRONT of the pumpkin had its
                                                                       tail cut by the carved face where a fish behind the pumpkin passed */
                uint16_t v = scene_px ? g_scene[y * TANK_W + x] : sv[x - bx0];
                if (row[x] == v) continue;
                row[x] = v;
                if (dr && (scene_px || !(sd[w - w0] & bit))) dr[w] &= ~bit;   /* (the scene's pixel is final) */
              }
            }
        }
    }
    for (int k = 0; pk_over && k < g_npc && k < 32; k++) {   /* the pieces whole before it (g_dep_over): theirs back over it, from what was under it -
                                                                  the piece as this frame had it there, the grass or a fish before it included */
        if (!(pk_over >> k & 1)) continue;
        const piece_t *p = &g_pc[k];
        int ya = p->y0 > by0 ? p->y0 : by0, yb = p->y0 + p->rows - 1 < by1 ? p->y0 + p->rows - 1 : by1;
        int xa = p->x0 > bx0 ? p->x0 : bx0, xb = p->x0 + p->w - 1 < bx1 ? p->x0 + p->w - 1 : bx1;
        for (int y = ya; y <= yb; y++) {
            const uint16_t *sv = g_dep_save + (size_t)(y - by0) * bw; uint16_t *row = fb + (size_t)y * TANK_W;
            const uint32_t *sd = g_dep_dirty + (size_t)(y - by0) * DIRTY_WORDS_PER_ROW; uint32_t *dr = g_dirty ? g_dirty + (size_t)y * DIRTY_WORDS_PER_ROW : NULL;
            for (int x = xa; x <= xb; x++) {
                int w = x >> 5; uint32_t bit = 1u << (x & 31);
                if ((dr && !(dr[w] & bit)) || !((over_row >> k & 1) ? pc_body_covers(k, x, y) : pc_covers(k, x, y))) continue;
                if (row[x] == sv[x - bx0]) continue;
                row[x] = sv[x - bx0];
                if (dr && !(sd[w - w0] & bit)) dr[w] &= ~bit;
            }
        }
    }
    if (render_clock_us) { g_dep_us += (dq1 - dq0) + (PROF_MARK() - dq2); g_dep_n++; }
    return drew;
}
/* (director `pkcheck`, 0.3.9.4: Strato saw fronds over a pumpkin's edges on the square) - every baked moment of the grass
   loop checked against the scene inside a piece's body: pixels where the loop holds grass over the piece. The bands as
   they are; each fine moment laid over its band in scratch (a frame from alloc). Returns the worst count; *bands, *fines:
   how many moments had any */
int render_debug_piece_grass(int item, int *bands, int *fines, void *(*alloc)(size_t), void (*release)(void *)) {
    *bands = *fines = 0;
    int k = -1; for (int q = 0; q < g_npc; q++) if (g_pc[q].item == item) k = q;
    if (k < 0 || !g_gl_n || !g_scene) return -1;
    uint16_t *tmp = alloc ? alloc((size_t)TANK_W * TANK_H * 2) : NULL;
    int worst = 0; const int nf = g_gl_n * g_gl_f;
    for (int j = 0; j < nf; j++) {
        int kb = j / g_gl_f; if (!g_gl_ok[kb]) continue;
        const bool fine = j % g_gl_f != 0;
        if (fine && (!tmp || g_gl_fok[j] != 1)) continue;
        const uint16_t *band = g_gl_band[kb];
        if (fine) { memcpy(tmp + (size_t)g_gl_y0 * TANK_W, band, (size_t)g_gl_rows * TANK_W * 2); gl_diff_lay(tmp, j); }
        int bad = 0;
        for (int r = 0; r < g_gl_rows; r++) for (int x = 0; x < TANK_W; x++) {
            int y = g_gl_y0 + r;
            if (!pc_body_covers(k, x, y)) continue;
            uint16_t v = fine ? tmp[(size_t)y * TANK_W + x] : band[(size_t)r * TANK_W + x];
            if (v != g_scene[(size_t)y * TANK_W + x]) bad++;
        }
        if (bad) { if (fine) (*fines)++; else (*bands)++; }
        if (bad > worst) worst = bad;
    }
    if (tmp && release) release(tmp);
    return worst;
}


/* render_tank in the aquarium: the water, the grass, the pieces, the fish and the critters among them, the film on the glass */
void aquarium_render(const tank_t *t, uint16_t *fb, int stride) {
    float dim = t->night ? 0.45f : 1.0f;
    g_keep_on = false; g_fbg_on = g_fbg_used = false; g_bedm_on = false;   /* (SWIM DEPTH: set by the fish below, read through the passes after them) */
    ctx_t c = ctx_full(fb, stride, dim);
    int64_t p0 = PROF_MARK();
    bool cached = g_scene && g_dirty && stride == TANK_W;
    g_dirty_hold = false;                          /* the tank draws in the frame's coordinates: its marks count */
    if (cached) memset(g_dirty, 0, TANK_H * DIRTY_WORDS_PER_ROW * sizeof(uint32_t));
    if (g_touched) memset(g_touched, 0, TANK_H * DIRTY_WORDS_PER_ROW * sizeof(uint32_t));   /* this frame's marks begin here */
    struct { short x0, y0, x1, y1; } rects[6 + MAX_FOOD + MAX_BUBBLE + N_FISH_MAX + VEG_BEDS_MAX + SHRIMP_MAX];
    int nr = 0;
#define DYN_RECT(cx0, cy0, cx1, cy1) do { if (cached && nr < (int)(sizeof rects / sizeof rects[0])) { \
        rects[nr].x0 = (short)(cx0); rects[nr].y0 = (short)(cy0); \
        rects[nr].x1 = (short)(cx1); rects[nr].y1 = (short)(cy1); nr++; } } while (0)

    bool front = cached;                           /* the pieces IN FRONT ride in the scene - a placement page's own piece too, until it is changed on
                                                      the page (dragged, re-ranked, re-depthed, recolored; setup_place_live): from then on that piece
                                                      alone is drawn live, out of the scene, until the page closes. (0.3.9.4. Before, a page up took
                                                      EVERY piece IN FRONT out of the scene, drew them all live every frame with the back grass live too,
                                                      and parked the grass loop - Strato on the square: "the frame rate drops to a crawl") */
    bool sgv = false;                              /* the frame is primed from the grass cache (below) */
    int gl_k = -1, gl_j = -1; bool gl_primed = false, gl_forced = false, gl_patched = false, gl_patched_odd = false; uint32_t gl_key_now = 0;   /* ... or from the grass loop: its moment */
    pieces_build(t, cached, front);
    {   /* THE EDITOR (Strato 10-31): the creatures out of every frame a placement page is up, and until the grass has settled after its DONE -
           they come back with the repaint, not over it */
        static bool was_place;
        const bool place = setup_is_place();
        if (was_place && !place) g_gl_page_closed = 4; else if (g_gl_page_closed) g_gl_page_closed--;
        was_place = place;
        g_no_animals = place && !g_page_animals;     /* (with the bake on the second core they come back at DONE itself; g_gl_patch_page kept for the log) */
    }
    bool pc_live = false;                          /* a piece drawn live this frame (the page's, changed on it) */
    for (int k = 0; k < g_npc; k++) pc_live |= g_pc[k].placing;
    const bool loop_ok = cached && !pc_live;       /* THE GRASS LOOP sits a live piece out: its bands hold the piece where it stood in the scene, so
                                                      they are neither used (the back grass drawn live meanwhile) nor rebaked; the scene's new look is
                                                      taken up when the page closes - one rebake, not two, and none if the piece comes back as it was */
    g_gl_parked = pc_live;
    g_veg_seg_hold = cached && g_gl_on && g_gl_n > 0 && g_gl_seg_ok && !t->night ? g_gl_seg : NULL;   /* THE HELD HEIGHTS, this frame (the night draws live: nothing to disagree with) */
    int pkx = g_pk_k >= 0 ? g_pc[g_pk_k].cx : -1, pkz = g_pk_k >= 0 ? g_pc[g_pk_k].z : DECOR_Z_BACK;
    bool pk_in_scene = g_pk_k >= 0 && !g_pc[g_pk_k].placing;
    g_pk_lx = t->night && pk_in_scene ? pkx : -1; g_pk_ly = PK_LIGHT_Y;     /* the lantern's light, this frame */
    if (g_pk_lx >= 0) pk_lut_fill();
    { int kc = pc_index(2); g_veg_mask_cx = kc >= 0 && g_pc[kc].z == DECOR_Z_FRONT ? g_pc[kc].cx : -1;   /* the castle IN FRONT: its arch's passes */
      g_pk_under_castle = kc >= 0 && g_pk_k >= 0 && kc > g_pk_k;                                          /* the castle over the lantern: its shadow's rules */
      g_veg_arch_night = t->night;                                                                        /* the back grass in the arch shades itself (veg_put), by arch_shade's rule */
      g_veg_arch_shade = g_veg_mask_cx >= 0 && g_pc[kc].baked && !(t->night && g_pk_under_castle && g_pk_lx >= 0); }   /* (a castle not in the scene - dragged, no cache - has its whole arch shaded at the frame's end instead) */
    g_pk_shadows = g_pk_lx >= 0 && (pkz == DECOR_Z_BACK || g_pk_k < g_npc - 1);                           /* something stands in front of the lantern */
    if (g_pk_shadows) pk_casts(t, pkx); else g_pk_cast_n = 0;
    if (cached) {
        /* the scene's signature: what the bake depends on; a change rebakes */
        scene_sig_t sig; memset(&sig, 0, sizeof sig);
        for (int k = 0; k < g_npc; k++) {
            if (!g_pc[k].baked) continue;
            int n = sig.n++;
            sig.item[n] = g_pc[k].item; sig.cx[n] = g_pc[k].cx; sig.z[n] = g_pc[k].z;
            if (g_pc[k].item == 3) { sig.q[n] = (int)(tank_coral_growth(t) * (CORAL_Q - 0.01f)); sig.col[n] = tank_coral_rgb(t); }
            if (g_pc[k].item == 4) { float lg = tank_cluster_growth(t); sig.q[n] = (int)((lg > 1 ? 1 : lg) * (CL_Q - 0.01f)); sig.col[n] = (uint32_t)tank_cluster_scheme(t); }
        }
        sig.dim = dim; sig.front = front; sig.lx = g_pk_lx;
        if (!g_sig_ok || memcmp(&sig, &g_sig, sizeof sig) || g_scene_dim != dim) {
            g_sig = sig; g_sig_ok = true; g_scene_front = front;
            /* rebuild the static scene with the vignette baked in (the
               per-frame pass then only re-darkens dynamic patches) */
            bake_scene(t, g_scene, dim);
            if (g_vig) g_vig_filled = true;
            g_scene_dim = dim; g_scene_epoch++;
        }
        sgv = loop_ok && g_sg_on && g_sg_ok && g_sg_scene_ep == g_scene_epoch;   /* primed with the back grass in it */
        gl_k = -1;
        g_gl_dark = t->night;
        if (loop_ok && g_gl_on && !g_gl_bypass && g_gl_n && !sgv && g_gl_dark) {   /* the light off: live grass, at the stepped moment */
            int j = gl_next(t->clock);
            if (g_gl_lock) gl_stepped(j, t->clock);
            if (g_veg_clock_force < 0) { g_veg_clock_force = gl_ftime(j); gl_forced = true; }   /* (the loop's moments, so the day picks up where the night left it) */
        } else if (loop_ok && g_gl_on && !g_gl_bypass && g_gl_n && !sgv) {    /* THE GRASS LOOP: the frame's band and fine moment, baked */
            gl_key_now = gl_stale_check(t);
            int j = gl_next(t->clock), nf = g_gl_n * g_gl_f;
            if (gl_key_now == g_gl_key) {
                if (g_primed_fb == fb && (g_primed_epoch & 0xC0000000u) == 0x40000000u) {   /* the prefetch chose its band: the fine moment nearest the clock of it */
                    int k = (int)(g_primed_epoch & 31);
                    if (k < g_gl_n && g_gl_ok[k] && g_gl_ver[k] == (uint16_t)(g_primed_epoch >> 5)) {
                        int d = ((j - k * g_gl_f) % nf + nf + nf / 2) % nf - nf / 2;
                        if (d < -GL_DLO) d = -GL_DLO;
                        if (d > GL_DHI) d = GL_DHI;
                        j = (k * g_gl_f + d + nf) % nf; gl_k = k; gl_primed = true;
                    }
                }
                if (gl_k < 0) { int k = gl_base(j); if (g_gl_ok[k]) gl_k = k; }
                if (gl_k >= 0) gl_j = g_gl_fok[j] == 1 ? j : gl_k * g_gl_f;   /* (its difference not baked: the band's own moment) */
                if (gl_k >= 0 && g_gl_lock && g_gl_fok[j] == 0) gl_k = -1;   /* (... stepping, drawn live at its own: a repeated moment is a step) */
            }
            if (g_gl_lock) {
                gl_stepped(j, t->clock);
                if (gl_k < 0 && g_veg_clock_force < 0) { g_veg_clock_force = gl_ftime(j); gl_forced = true; }   /* (stale: the grass drawn live at the
                                                                                    frame's moment all the same - no jump as the loop comes back) */
            }
        }
        if (gl_k >= 0) {
            if (!gl_primed) {
                memcpy(fb, g_scene, (size_t)g_gl_y0 * TANK_W * sizeof(uint16_t));
                memcpy(fb + g_gl_y0 * TANK_W, g_gl_band[gl_k], (size_t)g_gl_rows * TANK_W * sizeof(uint16_t));
            }
            if (gl_j != gl_k * g_gl_f) gl_diff_lay(fb, gl_j);
            sgv = true;                                 /* (the back grass is in the frame: nothing below draws it) */
            if (g_veg_clock_force < 0) { g_veg_clock_force = gl_ftime(gl_j); gl_forced = true; }   /* every reader of the sway at its moment: a seahorse on a
                                                                                                                  blade, a bed IN FRONT */
            if (g_gl_patch && g_gl_fgen[gl_j] != g_gl_gen) {   /* THE PATCH: the moment is stale inside the boxes - repainted there now, its masks with it */
                if (gl_j != gl_k * g_gl_f) gl_diff_mask(gl_j, gl_k); else memcpy(g_gl_bmask, g_gl_mask[gl_k], (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4);
                uint32_t *om = NULL;
                if (g_fl_odd && g_gl_omask[gl_k]) { memcpy(g_fl_odd, g_gl_omask[gl_k], (size_t)g_gl_rows * DIRTY_WORDS_PER_ROW * 4); om = g_fl_odd; }
                for (int i = 0; i < g_gl_nwin; i++) gl_patch_rect(t, fb, 0, g_gl_bmask, om, gl_j, dim, pkz, i, -1);
                gl_patched = true; gl_patched_odd = om != NULL;
                if (gl_j != gl_k * g_gl_f && g_gl_fok[gl_j] == 1 && g_gl_bgen[gl_k] == g_gl_gen && g_gl_pwin == 0 && g_gl_bs.phase != 3 && g_gl_bake && g_gl_pmask) {   /* the boxes as painted are the moment's
                                                                                    bake: into g_gl_bake and its mask, so the bake at the frame's end is the splice alone */
                    for (int i = 0; i < g_gl_nwin; i++) {
                        const gl_rect_t *r = &g_gl_win[i];
                        for (int y = r->y0; y <= r->y1; y++) {
                            const uint16_t *iv = gl_wrow_of(y);
                            if (!iv) continue;
                            for (int q = 0; q < 4; q += 2) {
                                const int a = iv[q] > r->x0 ? iv[q] : r->x0, b = iv[q + 1] < r->x1 ? iv[q + 1] : r->x1;
                                if (a <= b) memcpy(g_gl_bake + (size_t)(y - g_gl_y0) * TANK_W + a, fb + (size_t)y * TANK_W + a, (size_t)(b - a + 1) * 2);
                            }
                            memcpy(g_gl_pmask + (size_t)(y - g_gl_y0) * DIRTY_WORDS_PER_ROW, g_gl_bmask + (size_t)(y - g_gl_y0) * DIRTY_WORDS_PER_ROW, DIRTY_WORDS_PER_ROW * 4);
                        }
                    }
                    g_gl_pcap = gl_j;
                }
            }
        } else { unsigned sep = sgv ? (0x80000000u | g_sg_ep) : g_scene_epoch;
          if (!(g_primed_fb == fb && g_primed_epoch == sep))
              memcpy(fb, sgv ? g_sceneg : g_scene, TANK_W * TANK_H * sizeof(uint16_t)); }
        g_primed_fb = NULL;
        /* a piece BEHIND that is not in the scene (dragged): live, right over it;
           the castle too, whole, when it is not baked (its front row again after
           the fish, below) */
        for (int k = 0; k < g_npc; k++)
            if (!g_pc[k].baked && (g_pc[k].z == DECOR_Z_BACK || g_pc[k].item == 2)) { pc_draw(&c, t, k, true, g_live_rec || (g_pc[k].z == DECOR_Z_BACK && g_pc[k].item != 2)); pc_cover_live(fb, k); }   /* (BEHIND: its mask, for the plant) */
    } else draw_scene(t, fb, stride, dim);
    /* the overlays of the pieces BEHIND: a crown over its baked fan, the
       candle's face over the lantern's body - under the grass */
    if (sgv) {                                       /* the grass is in the frame already: these stay under it */
        if (gl_k >= 0) {                                /* (the loop's: its band's mask - its fine moment's, when anything is under the grass) */
            const uint32_t *m = g_gl_mask[gl_k];
            if (gl_patched) m = g_gl_bmask;             /* (THE PATCH left the moment's mask there, the boxes repainted) */
            else if (gl_j != gl_k * g_gl_f) {
                bool ov = false;
                for (int k = 0; k < g_npc; k++) ov |= g_pc[k].z == DECOR_Z_BACK && (g_pc[k].item == 3 || g_pc[k].item == 4 || g_pc[k].item == 8 || g_pc[k].item == 9);
                if (ov) { gl_diff_mask(gl_j, gl_k); m = g_gl_bmask; }
            }
            g_under = m - g_gl_y0 * DIRTY_WORDS_PER_ROW; g_under_y0 = g_gl_y0;
        }
        else { g_under = g_bgmask; g_under_y0 = 0; }
    }
    for (int k = 0; k < g_npc; k++) {
        if (g_pc[k].z != DECOR_Z_BACK) continue;
        g_clip_k = k;
        if (g_pc[k].item == 3 || g_pc[k].item == 4) PC_CROWN(k);
        else if (g_pc[k].item == 8 && g_pc[k].baked) draw_pumpkin_glow(&c, g_pc[k].cx, anim(t), t->night, false, true);
        else if (g_pc[k].item == 9) PC_VOLCANO(k);
        g_clip_k = -1;
    }
    g_under = NULL; g_under_y0 = 0;
    PROF_ADD(0, p0);

    PROF_ADD(1, p0);   /* stage 1 (light shafts) retired 2026-09-01 */
    /* vegetation, BACK layer: the reef bed (left - milestone lushness widens
       its base, never withers) and two decor beds; all three keep growing up
       and out with tank_t.veg_growth until the keeper trims them (slash the
       canopy). Geometry comes from tank_veg_bed so physics and pixels agree.
       The alternating FRONT fronds draw after the fish, below. Each bed's
       spans skip the pieces above it (veg_skip_for), and a bed in front of
       the lantern is backlit (g_pk_shade_all). */
    const int *veg_seed = VEG_SEED;
#define VEG_PASS(b, layer) do { g_veg_skip = veg_skip_for(t, (b)); \
        g_pk_shade_all = g_pk_lx >= 0 && ((b) == 3 ? tank_decor_above(t, 0, 8) : pkz == DECOR_Z_BACK); \
        draw_veg(&c, t, (b), veg_seed[(b)], (layer), cached); g_pk_shade_all = false; } while (0)
    bool dep_any = false;                            /* SWIM DEPTH: a fish BEHIND or THROUGH the grass this frame */
    for (int i = 0; i < N_FISH_SLOTS; i++)           /* (every fish here, visitors too - 0.4: only the home fish counted, so a visitor
                                                        behind the grass with every home fish in front had no grass mask, drawn over the fronds) */
        if (tank_swims(t, i) && (i < t->n_fish || !g_dbg_dep_home_only)) dep_any |= t->fish[i].gdepth != FISH_DEPTH_FRONT;
    if (t->sd_unlocks & SD_ITEM_SHRIMP) dep_any |= t->shr_gdepth != FISH_DEPTH_FRONT;   /* (the shrimp's school, phase 3) */
    if ((t->sd_unlocks & SD_ITEM_SEAHORSE) && t->sh_x >= 0) dep_any |= t->sh_gdepth != FISH_DEPTH_FRONT;   /* (the seahorse) */
    if ((t->sd_unlocks & SD_ITEM_CRAB) && t->cr_x >= 0) dep_any |= t->cr_gdepth != FISH_DEPTH_FRONT;              /* (the floor critters) */
    if (tank_snail_upright(t)) dep_any |= t->snail_gdepth != FISH_DEPTH_FRONT;
    if ((t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0) dep_any |= t->urchin_gdepth != FISH_DEPTH_FRONT;
    dep_any = dep_any && g_dep_save && cached && !g_dep_off;
    g_dep_pieces = g_dep_save && cached && !g_dep_off && g_npc > 0;   /* ... and against the pieces (phase 2) */
    bool fl_live = false;
    if (!sgv) {                                      /* (the grass cache holds them: the frame was primed with them) */
        uint32_t *keep_bm = g_bgmask; bool keep_rec = g_bg_rec; int keep_top = g_sg_top;
        if (dep_any) {                               /* (drawn live: its pixels and its odd fronds' recorded for the fish) */
            memset(g_fl_all, 0, (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4); memset(g_fl_odd, 0, (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4);
            g_bgmask = g_fl_all; g_bg_rec = true; g_om = g_fl_odd; fl_live = true;
        }
        for (int b = 0; b < tank_veg_beds(t); b++)      /* a BACK-layer piece first: behind the grass too */
            if (bed_z(t, b) == DECOR_Z_BACK) VEG_PASS(b, -1);
        if (dep_any) { g_bgmask = keep_bm; g_bg_rec = keep_rec; g_om = NULL; g_sg_top = keep_top; }
        for (int b = 0; b < tank_veg_beds(t); b++)
            if (bed_z(t, b) == DECOR_Z_MIDDLE) VEG_PASS(b, 0);
    }
    /* THE CRAB'S PLACES in the frame (2026-10-24, Strato on the 1.8 and the square: "if the crab is meant to be
       inside the cave or castle, nothing visible should really be behind it"):
       - cr_deep: in or on its way into a shelter (ROOST, SHELTER) it is the DEEPEST thing on the sand - drawn here,
         before the shrimp and the fish, under the snail and the urchin, under the front fronds. A castle at either
         depth works (its arch is open: the front row restores over the wall, the crab shows in the opening) and a
         volcano BEHIND (baked in the scene);
       - cr_slot: a volcano IN FRONT (its cave is painted rock: the restore would cover it) and the CLIMB on one, and
         its front LANE on the sand (tank.h THE LANES; a dig is in front whatever the lane) - the front slot after
         the pieces IN FRONT are restored, FIRST in it so the snail and the urchin in their front lane pass in front
         of it (Strato, the 1.8), under the front fronds (which keep off the volcano's silhouette anyway); the
         volcano's per-frame lava and embers keep off the crab (g_guard in vo_store - Strato, the pendant);
       - cr_top: a sand dollar held up - over the fronds, so the keeper sees it wherever it dug;
       - else the critter pass with the snail and the urchin (its back lane on the sand). */
    const bool cr_in = (t->sd_unlocks & SD_ITEM_CRAB) && t->cr_x >= 0;
    const bool cr_vol_front = cr_in && tank_decor_in(t, 9) && tank_decor_z(t, 9) == DECOR_Z_FRONT
                              && (t->cr_mode == CR_CLIMB || (t->cr_cave == 9 && (t->cr_mode == CR_ROOST || t->cr_mode == CR_SHELTER)));
    const bool cr_deep = cr_in && t->cr_find <= 0 && !cr_vol_front && (t->cr_mode == CR_ROOST || t->cr_mode == CR_SHELTER);
    /* ... and the deep crab follows its SHELTER'S DEPTH against the fronds (Strato 2026-10-24, the watch: the castle IN
       FRONT, the crab in its arch painted behind the fronds seen through it - "the castle is different in that you can
       see all the way to the back fronds through the arch"): a shelter BEHIND stands behind the fronds, so the crab in
       it is under every frond (drawn before the arch pass, the back fronds painted over it again); a castle IN FRONT
       stands before them, so the crab in its arch is over the fronds in the opening - drawn after the arch pass, still
       before the shrimp and the fish, and the castle's front row restores over it where it meets the wall */
    const bool cr_deep_front = cr_deep && t->cr_cave >= 0 && tank_decor_in(t, t->cr_cave) && tank_decor_z(t, t->cr_cave) == DECOR_Z_FRONT;
    const bool cr_deep_back = cr_deep && !cr_deep_front;
    g_cr_clip = (cr_deep || (cr_vol_front && t->cr_mode != CR_CLIMB)) ? crab_stack(t) : 0;   /* (THE CRAB'S STACK: in its shelter, not under the pieces before it) */
    bool cr_top = false, cr_slot = false;                        /* (decided with the lanes, below: with a page up or on the bare path the
                                                                    front volcano's crab goes to the top lane, after the live piece) */
    if (cr_deep_back) {
        /* the frame primed with the back grass in it (THE GRASS LOOP's band, the grass cache): the crab is drawn at
           depth BEHIND the grass - dep_draw puts every frond's pixels back over it from the frame's grass mask - and
           inside its shelter (DEP_NOBED: no piece comes back over it; a bed IN FRONT stays over it). Before, it was
           drawn plain over the primed grass and the weave-back below ran only when the grass was live: on every board
           since the loop (0.3.9.3) a crab in a volcano BEHIND's cave or a castle BEHIND's arch sat on top of the
           fronds (C1704 on Discord 10-31, "the crab always renders in front of the seagrass"; Strato on the 1.8) */
        bool deep_done = false;
        if (sgv && cached && g_dep_save && !g_dep_off) {
            const uint32_t *keep_all = g_dm_all, *keep_odd = g_dm_odd; const int keep_y0 = g_dm_y0;
            if (gl_k >= 0) {                                 /* the loop's mask for this frame's moment (THE PATCH: the boxes repainted, in g_gl_bmask) */
                if (gl_patched) { g_dm_all = g_gl_bmask; g_dm_odd = gl_patched_odd ? g_fl_odd : NULL; }
                else if (gl_j != gl_k * g_gl_f) { gl_diff_mask(gl_j, gl_k); g_dm_all = g_gl_bmask; g_dm_odd = g_gl_omask[gl_k]; }
                else { g_dm_all = g_gl_mask[gl_k]; g_dm_odd = g_gl_omask[gl_k]; }
                g_dm_y0 = g_gl_y0;
            }
            else if (g_bgmask) { g_dm_all = g_bgmask; g_dm_odd = g_fl_odd; g_dm_y0 = 0; }
            if (g_dm_all) {
                dep_draw(&c, fb, t, 0, dep_crab, g_cbb, &g_cbb_ok, t->cr_x, CR_FLOOR_Y - t->cr_y - 12, 44, FISH_DEPTH_BEHIND, DEP_NOBED);
                deep_done = true;
            }
            g_dm_all = keep_all; g_dm_odd = keep_odd; g_dm_y0 = keep_y0;
        }
        if (!deep_done) draw_crab(&c, t);                        /* (its rect is marked in the critter pass) */
        /* THE WEAVE kept (Strato 2026-10-24, the square: one frond still drawn behind the crab in the cave): the
           back fronds - a MIDDLE bed's even ones, a BACK bed's all - again over its rect, after it, not final (the
           sweep vignettes them with the rect), so no frond of any bed sits behind a crab in its cave - the live
           grass's way; with the grass primed the dep_draw above did it */
        if (!sgv && !deep_done) {
            const int feet = (int)floorf(CR_FLOOR_Y - t->cr_y);
            const int wx0 = (int)t->cr_x - 28, wy0 = feet - CR_GH - 8, wx1 = (int)t->cr_x + 28, wy1 = feet + 1;
            ctx_t cw = c; cw.fb = &CTX_PX(&c, wx0, wy0); cw.ox = wx0; cw.oy = wy0; cw.w = wx1 - wx0 + 1; cw.h = wy1 - wy0 + 1;
            for (int b = 0; b < tank_veg_beds(t); b++) {
                float bx0, bx1; tank_veg_bed(t, b, &bx0, &bx1, NULL, NULL);
                int z = bed_z(t, b);
                if (z == DECOR_Z_FRONT || bx1 + 14 < wx0 || bx0 - 14 > wx1) continue;
                g_veg_skip = veg_skip_for(t, b);
                g_pk_shade_all = g_pk_lx >= 0 && (b == 3 ? tank_decor_above(t, 0, 8) : pkz == DECOR_Z_BACK);
                draw_veg(&cw, t, b, veg_seed[b], z == DECOR_Z_BACK ? -1 : 0, false);
                g_pk_shade_all = false;
            }
        }
    }
        /* no DYN_RECT: with the scene cache each frond span vignettes itself */
    if (g_veg_mask_cx >= 0) {                        /* a castle IN FRONT: inside its open arch the late fronds go before the fish */
        DYN_RECT(g_veg_mask_cx - CASTLE_ARCH_R, CASTLE_FY - CASTLE_ARCH_S - CASTLE_ARCH_R, g_veg_mask_cx + CASTLE_ARCH_R, CASTLE_FY);   /* (those are marked, not final: the sweep vignettes them) */
        g_veg_arch_pass = -1;
        for (int b = 0; b < tank_veg_beds(t); b++)
            if (bed_z(t, b) == DECOR_Z_MIDDLE) VEG_PASS(b, 1);     /* (a bed placed IN FRONT stands BEFORE the castle: no arch pass, no tunnel shade - the late pass draws it whole) */
        g_veg_arch_pass = 0;
    }
    PROF_ADD(2, p0);
    /* the airstone the column rises from, on the floor where the keeper put
       it (setup): three stones and a glint - dynamic, since it can move */
    {
        float ax = t->bubble_x, ay = TANK_BOT - 17;
        fill_ellipse(&c, ax - 6, ay + 1, 7, 4, 0x2a3634, 255);
        fill_ellipse(&c, ax + 5, ay + 2, 6, 3.5f, 0x22302c, 255);
        fill_ellipse(&c, ax, ay - 2, 6, 3.5f, 0x3a4a48, 255);
        fill_ellipse(&c, ax - 1, ay - 3, 2, 1.2f, 0x5a6a68, 200);
        DYN_RECT((int)ax - 14, (int)ay - 7, (int)ax + 12, (int)ay + 7);
    }
    /* food pellets */
    for (int i = 0; i < MAX_FOOD; i++)
        if (t->food[i].alive) {
            fill_ellipse(&c, t->food[i].x, t->food[i].y, 2.6f, 2.6f, 0xffbd59, 255);
            fill_ellipse(&c, t->food[i].x - 0.8f, t->food[i].y - 0.8f, 1.0f, 1.0f, 0xffe9bd, 255);
            DYN_RECT((int)t->food[i].x - 5, (int)t->food[i].y - 5, (int)t->food[i].x + 5, (int)t->food[i].y + 5);
        }
    /* bubbles */
    for (int i = 0; i < MAX_BUBBLE; i++) {
        const bubble_t *b = &t->bubble[i];
        float r = b->column ? 2.6f : 1.8f;
        fill_ellipse(&c, b->x, b->y, r, r, 0x9fd8e2, 60);
        px_blend(&c, (int)(b->x - r * 0.4f), (int)(b->y - r * 0.4f), 0xffffff, 120);
        DYN_RECT((int)b->x - 5, (int)b->y - 5, (int)b->x + 5, (int)b->y + 5);
    }
    if (cr_deep_front) draw_crab(&c, t);                         /* in a castle IN FRONT's arch: over the fronds seen through it, under the shrimp and the fish (its rect is marked in the critter pass) */
    /* SWIM DEPTH: this frame's grass masks and beds IN FRONT, for every animal's dep_draw below */
    g_dm_all = g_dm_odd = NULL; g_dm_y0 = 0; g_fbed_n = 0; g_fbed_over = 0;
    if (dep_any || g_dbg_grass_on) {
        if (gl_k >= 0) {                             /* the loop's: the fine moment's own mask, the band's odd fronds */
            if (gl_patched) { g_dm_all = g_gl_bmask; g_dm_odd = gl_patched_odd ? g_fl_odd : NULL; }   /* (THE PATCH: both with the boxes repainted) */
            else { if (gl_j != gl_k * g_gl_f) { gl_diff_mask(gl_j, gl_k); g_dm_all = g_gl_bmask; } else g_dm_all = g_gl_mask[gl_k]; g_dm_odd = g_gl_omask[gl_k]; }
            g_dm_y0 = g_gl_y0;
        } else if (fl_live) { g_dm_all = g_fl_all; g_dm_odd = g_fl_odd; }
        else if (sgv && g_bgmask) { g_dm_all = g_bgmask; g_dm_odd = g_fl_odd; }   /* (the grass cache's, recorded at its refresh) */
    }
    if (g_dbg_grass_on) { g_dbg_dm = g_dm_all; g_dbg_dm_y0 = g_dm_y0; }
    if (cached && g_dep_save && !g_dep_off)
        for (int b = 0; b < tank_veg_beds(t); b++) {   /* the beds IN FRONT (the sword plant placed so): their spans */
            if (bed_z(t, b) != DECOR_Z_FRONT || t->veg_growth[b] < VEG_BARE) continue;
            float x0, x1, top; int nf; tank_veg_bed(t, b, &x0, &x1, &top, &nf);
            g_fbed_x0[g_fbed_n] = (int)x0 - 16; g_fbed_x1[g_fbed_n] = (int)x1 + 16; g_fbed_n++;
            uint32_t skip = veg_skip_for(t, b);          /* (the pieces it keeps off: ranked above it, or before its layer) */
            for (int k = 0; k < g_npc && k < 32; k++) if (g_pc[k].z == DECOR_Z_FRONT && !(skip >> k & 1)) g_fbed_over |= 1u << k;
        }
    /* THE LANES' front slot holds only while every piece IN FRONT is in the scene. With a placement page up (a
       piece dragged live, over everything) - or on the bare path, which bakes nothing - a critter in its front
       lane goes to the TOP lane instead, after the live pieces, so it stays before them as it was before the
       baked ones (Strato 2026-10-24, the 1.8: the crab "dropped to the layer below behind the volcano when
       opening the move menu"); over the fronds for as long as the page is up */
    bool lanes = true;
    for (int k = 0; k < g_npc; k++) if (g_pc[k].z == DECOR_Z_FRONT && !g_pc[k].baked) lanes = false;
    const bool snail_lane = lanes && t->snail_front, urchin_lane = lanes && t->urchin_front;
    const bool snail_top = !lanes && t->snail_front, urchin_top = !lanes && t->urchin_front;
    /* the strawberry crab on the floor (0.3.9): its places are decided above (THE CRAB'S PLACES); in the animal pass
       below only its back lane on the sand */
    const bool cr_lane_case = (t->cr_lane_front || t->cr_mode == CR_DIG) && t->cr_mode != CR_CLIMB && t->cr_mode != CR_ROOST && t->cr_mode != CR_SHELTER;
    cr_top = cr_in && (t->cr_find > 0 || (!lanes && (cr_vol_front || cr_lane_case)));   /* a page up / the bare path: the live pieces are drawn over everything, so a crab before one goes after them */
    cr_slot = cr_in && !cr_top && lanes && (cr_vol_front || cr_lane_case);
    const bool sh_in = (t->sd_unlocks & SD_ITEM_SEAHORSE) && t->sh_x >= 0;
    if (t->sd_unlocks & SD_ITEM_SHRIMP) for (int k = 0; k < SHRIMP_TONES; k++) g_shr_pal[k] = src_color(SHRIMP_RGB[k], c.dim);
    PROF_ADD(3, p0);
    /* THE ANIMAL PASS, DEEPEST FIRST (docs/render-stack.md ranks 4-7; Strato 11-02, the pendant: "one fish behind the
       seagrass and one fish in front. when they layer, the seagrass gets the drawing wrong" - and then: the deeper animal
       is always under, every animal). dep_draw lays one animal over what is already drawn and puts the grass and the
       pieces it is behind back over it from what was under it - so an animal drawn after one in front of it went over that
       one everywhere but on the fronds, where the put-back brought it back: the front animal in frond-shaped stripes
       across the one behind. So one pass, sorted back to front by layer (anim_layer: L0 behind the grass and the pieces
       .. L4 before both), the old ranks deciding only at one layer (stable): the school of shrimp (4), the seahorse's
       back lane (5) - before the fish, so a fish passes between it and what it holds (Strato 10-17) - the fish by slot
       (6), visitors too with their badges, then the floor critters' back lane (7): THE STACK of the sand's critters
       (Strato 10-24, the square), the crab lowest, then the snail, then the urchin. The deep crab (1, 3), the front
       slot (9) and the top lane (13) stay where they are. `--selftest-fish-pair` (PT_FISH_PAIR_OLD=1: the ranks alone) */
    enum { AN_SHRIMP, AN_SEAHORSE, AN_FISH, AN_CRAB, AN_SNAIL, AN_URCHIN };
    uint16_t an[N_FISH_SLOTS + 5]; uint8_t al[N_FISH_SLOTS + 5]; int na = 0;
    const bool by_layer = !g_dep_off && !g_dbg_fish_slot_order;
    #define AN_ADD(kind, idx, g, p) do { int j_ = na++; const uint8_t l_ = by_layer ? (uint8_t)anim_layer((g), (p)) : 0;   /* (an insertion, stable: at most a dozen) */ \
                                         while (j_ > 0 && al[j_ - 1] > l_) { an[j_] = an[j_ - 1]; al[j_] = al[j_ - 1]; j_--; } \
                                         an[j_] = (uint16_t)((kind) << 8 | (idx)); al[j_] = l_; } while (0)
    if ((t->sd_unlocks & SD_ITEM_SHRIMP) && t->shrimp_n > 0) AN_ADD(AN_SHRIMP, 0, t->shr_gdepth, t->shr_pdepth);
    if (sh_in && !t->sh_front) AN_ADD(AN_SEAHORSE, 0, t->sh_gdepth, DEP_LANE);
    for (int i = 0; i < N_FISH_SLOTS; i++) if (tank_swims(t, i)) AN_ADD(AN_FISH, i, t->fish[i].gdepth, t->fish[i].pdepth);
    if (cr_in && !cr_top && !cr_deep && !cr_slot) AN_ADD(AN_CRAB, 0, t->cr_gdepth, DEP_LANE);
    if (tank_snail_upright(t) && !snail_lane && !snail_top) AN_ADD(AN_SNAIL, 0, t->snail_gdepth, DEP_LANE);           /* (its front lane: the slot below, or the top lane with a page up) */
    if ((t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0 && !urchin_lane && !urchin_top) AN_ADD(AN_URCHIN, 0, t->urchin_gdepth, DEP_LANE);
    #undef AN_ADD
    for (int q = 0; q < na; q++) {
        const int i = an[q] & 0xff;
        switch (an[q] >> 8) {
        case AN_SHRIMP:                              /* each at the school's depth (SWIM DEPTH phase 3, dep_draw) */
            for (int s = 0; s < t->shrimp_n; s++) {
                dep_draw(&c, fb, t, s, dep_shrimp, g_sbb[s], &g_sbb_ok[s], t->shrimp[s].x, t->shrimp[s].y, 14, t->shr_gdepth, t->shr_pdepth);
                DYN_RECT((int)t->shrimp[s].x - 10, (int)t->shrimp[s].y - 5, (int)t->shrimp[s].x + 10, (int)t->shrimp[s].y + 5);
            }
            PROF_ADD(3, p0); break;
        case AN_SEAHORSE:                            /* its BACK lane: it holds something behind the fish - an even frond, a piece BEHIND;
                                                        its FRONT lane (an odd frond, a piece IN FRONT) is drawn after the pieces in front */
            g_dep_over = sh_stack(t); dep_draw(&c, fb, t, 0, dep_seahorse, g_hbb, &g_hbb_ok, t->sh_x, t->sh_y, 40, t->sh_gdepth, DEP_LANE);
            PROF_ADD(3, p0); break;
        case AN_FISH: {                              /* ours that are here, and visitors (a fish next door is not drawn); a visitor wears its
                                                        home tank's badge, small, above it: a guest, and from where */
            const fish_t *f = &t->fish[i];
            if (dep_draw(&c, fb, t, i, dep_fish, g_fbb[i], &g_fbb_ok[i], f->x, f->y, (int)(62 * f->size) + 8, f->gdepth, f->pdepth))
                DYN_RECT(g_fbb[i][0], g_fbb[i][1], g_fbb[i][2], g_fbb[i][3]);
#if PT_NET
            if (tank_is_visitor(i) && t->vis_badge[i - N_FISH_MAX] >= 0) {
                g_bb_on = true; g_bb_x0 = g_bb_y0 = 1 << 20; g_bb_x1 = g_bb_y1 = -1;
                link_badge_draw(&c, (int)f->x, (int)(f->y - 15 * f->size - 9), 5, t->vis_badge[i - N_FISH_MAX], 220);
                g_bb_on = false;
                if (g_bb_x1 >= g_bb_x0) DYN_RECT(g_bb_x0, g_bb_y0, g_bb_x1, g_bb_y1);
            }
#endif
            PROF_ADD(7, p0); break;                  /* (the split of the stage, 2026-10-23: the fish) */
        }
        case AN_CRAB:   DEP_CRAB(&c, fb, t);   PROF_ADD(8, p0); break;   /* (the snail, the urchin and the crab) */
        case AN_SNAIL:  DEP_SNAIL(&c, fb, t);  PROF_ADD(8, p0); break;
        case AN_URCHIN: DEP_URCHIN(&c, fb, t); PROF_ADD(8, p0); break;
        }
    }
    /* their rects, wherever they were drawn this frame (the seahorse either lane; the crab in any place) */
    if (sh_in) { int bx0, by0, bx1, by1; seahorse_box(t, &bx0, &by0, &bx1, &by1); DYN_RECT(bx0, by0, bx1, by1); }
    if (cr_in) {
        const int feet = (int)floorf(CR_FLOOR_Y - t->cr_y);
        if (t->cr_tilt == 0) DYN_RECT((int)t->cr_x - 27, feet - CR_GH - (t->cr_find > 0 ? (int)(CR_DISC_UP + CR_DISC_R) + 1 : 0), (int)t->cr_x + 27, feet + 1);   /* (+ the sand dollar over its head) */
        else DYN_RECT((int)t->cr_x - 36, feet - 35, (int)t->cr_x + 36, feet + 36);
    }
    if (tank_snail_upright(t)) DYN_RECT((int)t->snail_x - 21, (int)t->snail_y - 13, (int)t->snail_x + 21, (int)t->snail_y + 7);
    if ((t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0)
        DYN_RECT((int)t->urchin_x - 19, (int)URCHIN_FLOOR_Y - 17, (int)t->urchin_x + 19, (int)(URCHIN_FLOOR_Y + UR_BASE + t->urchin_dy) + 1);
    PROF_ADD(8, p0);
    /* the pieces IN FRONT back over the fish (and the snail, the food, the
       bubbles), only where they were drawn: the scene holds them. The castle's
       front row comes back the same way - a fish in the arch swims THROUGH,
       its keep stays behind the fish. A castle IN FRONT that is not in the
       scene (dragged, or a page up): its front row again, live. */
    for (int k = 0; k < g_npc; k++) {
        if (g_pc[k].z != DECOR_Z_FRONT) continue;
        g_rs_crab = cr_deep_front && !g_rs_old && g_pc[k].item != t->cr_cave && tank_decor_above(t, t->cr_cave, g_pc[k].item) ? t : NULL;   /* (behind the crab's castle) */
        if (g_pc[k].baked) pc_restore(fb, k);
        else if (g_pc[k].item == 2) draw_castle(&c, g_pc[k].cx, 1, cached);
    }
    g_rs_crab = NULL;
    PROF_ADD(9, p0);                                 /* the pieces IN FRONT back over them */
    /* THE FRONT SLOT (2026-10-24, a keeper's ask): the sand's critters in their front lane - over the pieces IN
       FRONT just restored, under the front fronds that follow (woven through the grass like a fish). Their rects
       are marked above. The snail's lane lived above the fronds until today. */
    const bool keep_clr = g_keep_on;                  /* (SWIM DEPTH: what the front slot paints over a fish is its own - the pieces' and the bed's marks) */
    if (keep_clr || g_fbg_used) { g_keep_clr_m = keep_clr ? g_keep : NULL; g_fbg_clr1 = g_fbg_used ? g_fbg_front : NULL; g_fbg_clr2 = g_fbg_used ? g_fbg_thru : NULL;
                                  g_keep_clr = true; g_bb_on = true; g_bb_x0 = g_bb_y0 = 1 << 20; g_bb_x1 = g_bb_y1 = -1; }
    if (cr_slot) {                                                                                 /* the crab lowest (the stack above) */
        /* ... in a front volcano's cave (roost, shelter) it is inside the volcano: a fish passing IN FRONT of the volcano
           stays over it (SWIM DEPTH phase 2; the stack's rule - the crab in its shelter is the deepest thing) */
        const bool hold = g_keep_on && cr_vol_front && t->cr_mode != CR_CLIMB;
        int hx0 = (int)t->cr_x - 48, hx1 = (int)t->cr_x + 48, hy1 = (int)(CR_FLOOR_Y - t->cr_y) + 8, hy0 = hy1 - 72, n = 0;
        if (hx0 < 0) hx0 = 0;
        if (hx1 > TANK_W - 1) hx1 = TANK_W - 1;
        if (hy0 < 0) hy0 = 0;
        if (hy1 > TANK_H - 1) hy1 = TANK_H - 1;
        if (hold) for (int y = hy0; y <= hy1; y++) for (int x = hx0; x <= hx1; x++) if (keep_at(x, y) && n < DEP_CAP_PX) g_dep_save[n++] = fb[y * TANK_W + x];
        const bool kc = g_keep_clr; if (hold) g_keep_clr = false;   /* (the marks stand: the fish goes back over it) */
        if (hold) draw_crab(&c, t); else DEP_CRAB_F(&c, fb, t);     /* (in its cave: no grass about it) */
        g_keep_clr = kc;
        n = 0;
        if (hold) for (int y = hy0; y <= hy1; y++) for (int x = hx0; x <= hx1; x++) if (keep_at(x, y) && n < DEP_CAP_PX) fb[y * TANK_W + x] = g_dep_save[n++];
    }
    if (tank_snail_upright(t) && snail_lane) DEP_SNAIL_F(&c, fb, t);
    if ((t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0 && urchin_lane) DEP_URCHIN_F(&c, fb, t);
    if (keep_clr || g_fbg_used) { g_keep_clr = false; g_bb_on = false; g_fbg_clr1 = g_fbg_clr2 = NULL; }
    g_cr_clip = 0;                                   /* (the crab is drawn by now - a held sand dollar's top lane is out of its shelter) */
    /* vegetation, FRONT layer: the alternating fronds drawn over the fish,
       so a fish inside a canopy is woven through it (each span applies its
       own vignette and untags itself, so the fish rects below skip it) */
    if (g_veg_mask_cx >= 0) g_veg_arch_pass = 1;     /* (the arch's pixels were drawn before the fish) */
    for (int b = 0; b < tank_veg_beds(t); b++)
        if (bed_z(t, b) == DECOR_Z_MIDDLE) VEG_PASS(b, 1);
    g_veg_arch_pass = 0;                             /* a bed placed IN FRONT last, WHOLE: over the grass, the fish, the critters and every piece it is
                                                        ranked above - the castle's arch included, and its fronds there take no tunnel shade (2026-10-24,
                                                        Strato on the 1.8: the sword plant IN FRONT drawn behind the crab in the castle, and tinted) */
    /* a live piece IN FRONT (its page: dragged, re-ranked) ranked UNDER the sword plant IN FRONT is drawn here, before the plant's
       leaves, so they fall over it as the order says; one ranked above it comes after the leaves (rank 11, below). 0.3.9.4, Strato on
       the square: "behind the pumpkin" picked, the preview still had the plant on top - and the volcano's page the same: a live piece
       had no mask, so the bed's "over" set took it for one under the plant and its body kept off the leaves */
    const bool plant_front = tank_veg_beds(t) > 3 && bed_z(t, 3) == DECOR_Z_FRONT;
#define PC_LIVE_UNDER_PLANT(k) (plant_front && g_pc[k].z == DECOR_Z_FRONT && !g_pc[k].baked && g_pc[k].item != 2 && tank_decor_above(t, 0, g_pc[k].item))
    for (int k = 0; k < g_npc; k++)
        if (PC_LIVE_UNDER_PLANT(k)) { pc_draw(&c, t, k, cached, g_live_rec); if (cached) pc_cover_live(fb, k); }
    { const bool shade = g_veg_arch_shade; g_veg_arch_shade = false;
      for (int b = 0; b < tank_veg_beds(t); b++)
          if (bed_z(t, b) == DECOR_Z_FRONT) {
              g_fbg_on = g_fbg_used;                     /* (SWIM DEPTH: off the fish in front of it) */
              uint32_t *keep_bm = g_bgmask; bool keep_rec = g_bg_rec; int keep_top = g_sg_top;
              if (g_bedm) {                              /* (its leaves recorded: the overlays of the pieces it stands before keep off them) */
                  if (!g_bedm_on) memset(g_bedm, 0, (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4);
                  g_bgmask = g_bedm; g_bg_rec = true; g_om = NULL; g_bedm_on = true;
                  g_bed_over_k = 0;                      /* the pieces its leaves go over: every one but those IN FRONT ranked above it - a live piece
                                                            (no mask to skip) by its rank like the rest (0.3.9.4) */
                  for (int k = 0; k < g_npc && k < 32; k++)
                      if (!(g_pc[k].z == DECOR_Z_FRONT && b == 3 && tank_decor_above(t, g_pc[k].item, 0))) g_bed_over_k |= 1u << k;
              }
              VEG_PASS(b, -1);
              if (g_bedm) { g_bgmask = keep_bm; g_bg_rec = keep_rec; g_sg_top = keep_top; }
              g_fbg_on = false;
          }
      g_veg_arch_shade = shade; }
#undef VEG_PASS
    PROF_ADD(10, p0);                                /* the front grass */
    /* the pieces IN FRONT that are not in the scene (a placement page is up,
       or no scene cache): live, in order, over everything; then every piece
       IN FRONT's overlay - its crown, the candle - clipped by the pieces
       above it */
    for (int k = 0; k < g_npc; k++)
        if (g_pc[k].z == DECOR_Z_FRONT && !g_pc[k].baked && g_pc[k].item != 2 && !PC_LIVE_UNDER_PLANT(k)) { pc_draw(&c, t, k, cached, g_live_rec); if (cached) pc_cover_live(fb, k); }
#undef PC_LIVE_UNDER_PLANT
    /* a castle IN FRONT ranked above other pieces IN FRONT, none of them in the
       scene (a placement page is up, or no cache): the pass above drew those
       over its keep and its front row. The castle again, whole, inside each
       one's box - the keep over them, as the scene has it. The fish there were
       under them already, and the arch's opening is not painted, so nothing
       else moves (0.3.9, Strato: "when I tap and hold the castle, it falls
       behind the volcano visually even when it shows IN FRONT OF THE VOLCANO;
       once I press DONE it snaps back") */
    { int kc = pc_index(2);
      if (kc >= 0 && g_pc[kc].z == DECOR_Z_FRONT && !g_pc[kc].baked)
          for (int j = 0; j < kc; j++) {
              const piece_t *a = &g_pc[j], *b = &g_pc[kc];
              if (a->z != DECOR_Z_FRONT || a->baked) continue;
              int x0 = a->x0 > b->x0 ? a->x0 : b->x0, x1 = (a->x0 + a->w < b->x0 + b->w ? a->x0 + a->w : b->x0 + b->w) - 1;
              int y0 = a->y0 > b->y0 ? a->y0 : b->y0, y1 = (a->y0 + a->rows < b->y0 + b->rows ? a->y0 + a->rows : b->y0 + b->rows) - 1;
              if (x0 > x1 || y0 > y1) continue;
              g_cst_clip = true; g_cst_clip_x0 = x0; g_cst_clip_y0 = y0; g_cst_clip_x1 = x1; g_cst_clip_y1 = y1;
              draw_castle(&c, b->cx, 0, cached);
              g_cst_clip = false;
          }
    }
    g_guard_who = (cr_slot ? GUARD_CRAB : 0u) | (tank_snail_upright(t) && snail_lane ? GUARD_SNAIL : 0u)
                | ((t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0 && urchin_lane ? GUARD_URCHIN : 0u);
    g_guard = g_guard_who ? t : NULL;                        /* the front slot's critters - the crab in the front volcano's cave, on its flank, or digging / resting /
                                                                  walking before it in its front lane; the snail and the urchin in theirs: the lava's cells keep off them
                                                                  (Strato, the 1.8: a dig at the cave mouth; 0.3.9.1, the urchin walking past the cave) */
    const unsigned guard0 = g_guard_who;
    const uint32_t cr_st = (cr_deep_front || (cr_slot && cr_vol_front && t->cr_mode != CR_CLIMB)) ? crab_stack(t) : 0;
    for (int k = 0; k < g_npc; k++) {
        if (g_pc[k].z != DECOR_Z_FRONT) continue;
        g_clip_k = k;
        if (!g_rs_old && (cr_deep_front || (cr_slot && cr_vol_front && t->cr_mode != CR_CLIMB))) {   /* THE CRAB'S STACK: a piece before its shelter paints its overlay
                                                                                                   over the crab, one behind (and the volcano round it) keeps off it */
            g_guard_who = (k < 32 && (cr_st >> k & 1)) ? (guard0 & ~(unsigned)GUARD_CRAB) : (guard0 | (unsigned)GUARD_CRAB);
            g_guard = g_guard_who ? t : NULL;
        }
        if (g_pc[k].item == 3 || g_pc[k].item == 4) PC_CROWN(k);
        else if (g_pc[k].item == 8 && g_pc[k].baked) draw_pumpkin_glow(&c, g_pc[k].cx, anim(t), t->night, false, true);
        else if (g_pc[k].item == 9) PC_VOLCANO(k);
        g_clip_k = -1;
    }
    g_guard = NULL; g_guard_who = 0;
    PROF_ADD(11, p0);                                /* the front pieces' overlays (crowns, the candle, the lava) */
    if ((t->sd_unlocks & SD_ITEM_SEAHORSE) && t->sh_x >= 0 && t->sh_front)   /* the seahorse's front lane (its rect is marked above), at its depth against the grass */
        g_dep_over = sh_stack(t), dep_draw(&c, fb, t, 0, dep_seahorse, g_hbb, &g_hbb_ok, t->sh_x, t->sh_y, 40, FISH_DEPTH_FRONT, DEP_LANE_F);   /* (its front lane: before the grass too) */
    if (cr_top) draw_crab(&c, t);                                                                      /* the crab holding a find up; a page up: its front lane (its rect is marked above) */
    if (tank_snail_upright(t) && snail_top) draw_snail(&c, t, true);                                   /* a page up: the snail's and the urchin's front lane, after the live pieces */
    if ((t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0 && urchin_top) draw_urchin(&c, t);
    PROF_ADD(12, p0);                                /* the seahorse's front lane, the crab's top lane */
    { int kc = pc_index(2); if (kc >= 0 && g_pc[kc].z == DECOR_Z_FRONT) { if (g_pc[kc].baked && cached) arch_shade_dirty(&c, kc, g_pc[kc].cx, t->night); else arch_shade(&c, kc, g_pc[kc].cx, t->night, true); } }   /* the tunnel through a castle IN FRONT: what passed it this frame (the scene carries the rest) */
    PROF_ADD(13, p0);                                /* the arch's shade */
    PROF_ADD(4, p0);
    /* porthole vignette: darken corners toward AMOLED black. With a scene
       cache the full-frame pass is baked into the scene and only the dynamic
       patches are re-darkened; without one, the classic per-pixel pass runs. */
    if (cached && g_pk_lx >= 0) pk_light_dirty(&c);   /* the lantern lights what moved near it (before its vignette) */
    if (cached) {
        /* one row sweep over the union of the dynamic rects: pixels marked
           in the dirty mask were drawn this frame (and not already vignetted
           inline) and get the vignette re-applied exactly once. */
        for (int y = 0; y < TANK_H; y++) {
            float dyf = (y - TANK_H * 0.5f) / (TANK_H * 0.5f);
            float rem = 0.72f - dyf * dyf;
            int x_in = rem > 0 ? (int)(TANK_W * 0.5f * (1 - sqrtf(rem))) : TANK_W / 2;
            if (x_in <= 0) continue;
            short iv[sizeof rects / sizeof rects[0]][2]; int ni = 0;
            for (int i = 0; i < nr; i++)
                if (y >= rects[i].y0 && y <= rects[i].y1) {
                    int a0 = rects[i].x0 < 0 ? 0 : rects[i].x0;
                    int a1 = rects[i].x1 >= TANK_W ? TANK_W - 1 : rects[i].x1;
                    if (a0 > a1) continue;
                    int j = ni++;                       /* insertion sort by x0 */
                    while (j > 0 && iv[j - 1][0] > a0) { iv[j][0] = iv[j - 1][0]; iv[j][1] = iv[j - 1][1]; j--; }
                    iv[j][0] = (short)a0; iv[j][1] = (short)a1;
                }
            int end = -1;                               /* merged sweep */
            for (int i = 0; i < ni; i++) {
                int a0 = iv[i][0] > end + 1 ? iv[i][0] : end + 1;
                int a1 = iv[i][1];
                if (a1 > end) end = a1;
                for (int s = 0; s < 2; s++) {           /* clip to the two ring spans */
                    int r0 = s ? (TANK_W - x_in > a0 ? TANK_W - x_in : a0) : a0;
                    int r1 = s ? a1 : (x_in - 1 < a1 ? x_in - 1 : a1);
                    const uint32_t *drow = g_dirty + y * DIRTY_WORDS_PER_ROW;
                    for (int x = r0; x <= r1; x++) {
                        uint32_t w = drow[x >> 5] >> (x & 31);
                        if (!w) { x |= 31; continue; }         /* nothing else in this word */
                        if (!(w & 1)) continue;
                        uint16_t *p = &c.fb[y * c.stride + x];
                        int a = (g_vig && g_vig_filled) ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                        if (a) px_darken(p, a);
                    }
                }
            }
        }
    } else {
        for (int y = 0; y < TANK_H; y++) {
            float dy = (y - TANK_H * 0.5f) / (TANK_H * 0.5f);
            float rem = 0.72f - dy * dy;
            int x_in = rem > 0 ? (int)(TANK_W * 0.5f * (1 - sqrtf(rem))) : TANK_W / 2;
            for (int side = 0; side < 2; side++)
                for (int k = 0; k < x_in; k++) {
                    int x = side ? TANK_W - 1 - k : k;
                    float dx = (x - TANK_W * 0.5f) / (TANK_W * 0.5f);
                    float d2 = dx * dx + dy * dy;
                    if (d2 > 0.72f) {
                        int a = (int)((d2 - 0.72f) * 220);
                        if (a > 0) px_blend(&c, x, y, 0x000000, a > 255 ? 255 : a);
                    }
                }
        }
    }
    /* algae film sits ON the glass - over the water, the fish, even the
     * vignette (which is why it draws after the re-darken pass: nothing
     * behind it needs repair, and next frame's scene restore erases wiped
     * cells for free) */
    PROF_ADD(5, p0);
    draw_algae(&c, t);
    draw_snail(&c, t, false);                    /* on the glass, over the film */
    /* the grass cache: rebuilt when it is stale (a rebake, the first frame) or
       its period is up - the rows the old grass reached copied back from the
       scene, the back grass painted in, its pixels recorded. The prefetch of
       the next frame starts after this, so it copies the new cache. */
    if (loop_ok && g_sg_on && (!sgv || t->clock - g_sg_clock >= g_sg_period || t->clock < g_sg_clock)) {
        int64_t q0 = render_clock_us ? render_clock_us() : 0;
        bool whole = !g_sg_ok || g_sg_scene_ep != g_scene_epoch;
        int top = whole ? 0 : (g_sg_top < 0 ? 0 : g_sg_top);
        if (whole) memcpy(g_sceneg, g_scene, (size_t)TANK_W * TANK_H * sizeof(uint16_t));
        else render_restore_touched(g_sceneg, g_bgmask);   /* only the words the old grass covered, back from the scene (a whole copy of its rows
                                                              cost half of a rebuild's 24 ms on the square) */
        memset(g_bgmask + top * DIRTY_WORDS_PER_ROW, 0, (size_t)(TANK_H - top) * DIRTY_WORDS_PER_ROW * 4);
        uint32_t *keep_t = g_touched; g_touched = NULL;
        ctx_t cg = ctx_full(g_sceneg, TANK_W, dim);
        g_sg_top = TANK_H; g_bg_rec = true; g_veg_arch_pass = 0;
        if (g_fl_odd) { memset(g_fl_odd, 0, (size_t)TANK_H * DIRTY_WORDS_PER_ROW * 4); g_om = g_fl_odd; }   /* (SWIM DEPTH: its odd fronds, held till the next refresh) */
#define SG_PASS(b, layer) do { g_veg_skip = veg_skip_for(t, (b)); \
        g_pk_shade_all = g_pk_lx >= 0 && ((b) == 3 ? tank_decor_above(t, 0, 8) : pkz == DECOR_Z_BACK); \
        draw_veg(&cg, t, (b), VEG_SEED[(b)], (layer), true); g_pk_shade_all = false; } while (0)
        for (int b = 0; b < tank_veg_beds(t); b++) if (bed_z(t, b) == DECOR_Z_BACK) SG_PASS(b, -1);
        for (int b = 0; b < tank_veg_beds(t); b++) if (bed_z(t, b) == DECOR_Z_MIDDLE) SG_PASS(b, 0);
#undef SG_PASS
        g_bg_rec = false; g_touched = keep_t; g_om = NULL;
        if (g_sg_top == TANK_H) g_sg_top = TANK_H - 1;
        g_sg_ok = true; g_sg_scene_ep = g_scene_epoch; g_sg_clock = t->clock; g_sg_ep = (g_sg_ep + 1) & 0x7fffffffu;
        if (render_clock_us) { g_sg_us += render_clock_us() - q0; g_sg_n++; }
    }
    if (gl_forced) g_veg_clock_force = -1;
    /* THE GRASS LOOP's bake: a new look (the scene, a bed, a frond's height past the waiting rule: gl_stale_check) marks the loop stale; once the grass
       has held still a few frames, one band or fine moment a frame is baked - the bands first, the next one due
       first: the scene's band copied in, the back grass of that moment painted over it, its pixels recorded (a
       fine moment's then kept as its difference from its band) */
    if (loop_ok && g_gl_on && !g_gl_bypass && !t->night) {   /* (not with a live piece: the loop waits for the page to close) */
        if (!gl_key_now) gl_key_now = gl_stale_check(t);
        const uint32_t hard = gl_key_now, full = g_gl_full_now;
        if (hard == g_gl_hard_last && ++g_gl_still >= GL_SETTLE && gl_geometry(t) && !g_gl_task) gl_bake_frame(t, dim, pkz);   /* (in slices, under a budget; THE PATCH's boxes first; or the platform's task) */
        g_gl_dim_last = dim; g_gl_pkz_last = pkz;
        g_gl_key_last = full; g_gl_hard_last = hard;
        if (g_gl_replan) { g_gl_replan = false; gl_release(); g_gl_nomem = 0; }   /* (the next frame plans the loop afresh) */
    }
    g_veg_seg_hold = NULL;
    g_pk_lx = -1; g_pk_under_castle = false; g_pk_shadows = false;   /* the light is this frame's: no page is ever lit by it */
    PROF_ADD(6, p0);
#undef DYN_RECT
}

/* ==== THE AQUARIUM's CARDS AND SHELF (moved from render.c's pages, verbatim but for the hooks' names) ==== */
/* the snail's card (2026-09-16, Strato: "tapping the snail show a simple
   card with the upright snail image and how much algae has been grazed so
   far"): a ring on the snail, a centred box in the modal's dress - the
   upright sprite at 2x, SNAIL, the tally. Drawn every frame (no cache: a
   few hundred blended pixels, nothing like the fish card's meters). */
static void snail_card_draw(ctx_t *c, const tank_t *t) {
    ring(c, t->snail_x, t->snail_y, 20, 0x9fd8e2);
    const int W = SNAIL_CARD_W, H = SNAIL_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 235);
    rect_edge(c, X, Y, W, H, 0x9fd8e2); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    const icon_t *ic = &icon_snail_upright;
    blit_icon_scaled(c, X + (W - ic->w * 2) / 2, Y + 10, ic, 2, true);
    draw_text(c, X + (W - text_w("SNAIL", 3)) / 2, Y + 82, 3, 0xffffff, "SNAIL");
    const char *cap = "ALGAE GRAZED SO FAR";
    draw_text(c, X + (W - text_w(cap, 2)) / 2, Y + 112, 2, 0x9fd8e2, cap);
    char n[24];
    if (t->snail_grazed <= 0) snprintf(n, sizeof n, "NOTHING YET");
    else snprintf(n, sizeof n, "%d SPOT%s", (int)t->snail_grazed, t->snail_grazed == 1 ? "" : "S");
    draw_text(c, X + (W - text_w(n, 3)) / 2, Y + 134, 3, 0xffffff, n);
}

/* the urchin's card (2026-10-02), the snail's: a ring on it, the urchin
   at 3x with its spines waving, SEA URCHIN, the grass it has eaten so far
   (the trim's centimeters) and what it is up to - in words, never colour. */
/* the crab's card (0.3.9; Strato 10-23: "we need to add a modal for when the user taps it"), the urchin's way:
   a ring on it, a centred card - the crab facing the glass at 2x (its own grid: the tank's may be a profile),
   STRAWBERRY CRAB, the pellets it has found, what it is doing */
static void crab_collect_box(int *x, int *y, int *w, int *h) {   /* the crab card's COLLECT, where the status line stands */
    *w = 156; *h = MSP_CLOSE_H; *x = (TANK_W - *w) / 2; *y = (TANK_H - CRAB_CARD_H) / 2 + CRAB_CARD_H - *h - 12;
}
bool aquarium_crab_collect_hit(const tank_t *t, float x, float y) {
    if (t->cr_find <= 0) return false;
    int bx, by, bw, bh; crab_collect_box(&bx, &by, &bw, &bh);
    return x >= bx - 8 && x < bx + bw + 8 && y >= by - 8 && y < by + bh + 8;
}
static void crab_card_draw(ctx_t *c, const tank_t *t) {
    ring(c, t->cr_x, CR_FLOOR_Y - t->cr_y - 10, 24, 0x9fd8e2);
    const int W = CRAB_CARD_W, H = CRAB_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 235);
    rect_edge(c, X, Y, W, H, 0x9fd8e2); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    static uint32_t s_key = 0;
    cr_pose_t p; memset(&p, 0, sizeof p); p.salt = 7 + t->cr_variant;   /* the portrait: its roost, breathing; open-clawed while poked */
    cr_pose_roost(&p, 0, 0, t->cr_poke > 0, fast_sin(anim(t) * 0.9f + 4 * TAU) > 0.35f);
    const uint32_t key = cr_hash(&p);
    if (key != s_key) { cr_build(ds()->cr_card_tones, ds()->cr_card_parts, &p); s_key = key; }
    const int S = 2, ox = X + (W - CR_GW * S) / 2, oy = Y + 10;
    for (int gy = 0; gy < CR_GH; gy++)
        for (int gx = 0; gx < CR_GW; gx++) {
            int tone = ds()->cr_card_tones[gy * CR_GW + gx];
            if (tone == CRT_NONE) continue;
            src_t sc = src_color(CR_RGB[tone], 1.0f);
            for (int yy = 0; yy < S; yy++) span(c, ox + gx * S, ox + gx * S + S - 1, oy + gy * S + yy, &sc, 255);
        }
    draw_text(c, X + (W - text_w("STRAWBERRY CRAB", 3)) / 2, Y + 70, 3, 0xffffff, "STRAWBERRY CRAB");
    {   /* two counts side by side: the pellets it found, the sand dollars it dug up (0.3.9) - what they paid, not how many finds (0.3.9.1) */
        const int L = X + W / 4, R = X + 3 * W / 4;
        char n[16];
        draw_text(c, L - text_w("PELLETS", 2) / 2, Y + 104, 2, 0x9fd8e2, "PELLETS");
        if (t->cr_eaten == 0) snprintf(n, sizeof n, "NONE YET"); else snprintf(n, sizeof n, "%u", (unsigned)t->cr_eaten);
        draw_text(c, L - text_w(n, 3) / 2, Y + 126, 3, 0xffffff, n);
        draw_text(c, R - text_w("SAND DOLLARS", 2) / 2, Y + 104, 2, 0x9fd8e2, "SAND DOLLARS");
        if (t->cr_found == 0) snprintf(n, sizeof n, "NONE YET"); else snprintf(n, sizeof n, "%u", SD_CRAB_FINDS_VALUE(t->cr_found));
        draw_text(c, R - text_w(n, 3) / 2, Y + 126, 3, 0xffffff, n);
    }
    const char *now = t->cr_find > 0 ? "FOUND A SAND DOLLAR!"
                    : t->cr_scurry ? "RUNNING FOR COVER"
                    : (t->cr_mode == CR_ROOST && t->cr_hide > 0) ? (t->cr_cave == 2 ? "HIDING IN THE CASTLE" : "HIDING IN THE CAVE")
                    : t->cr_mode == CR_ROOST ? (t->cr_cave == 2 ? "ROOSTING IN THE CASTLE" : t->cr_cave == 9 ? "ROOSTING IN THE CAVE" : "ROOSTING")
                    : t->cr_mode == CR_SHELTER ? (t->cr_cave == 2 ? "OFF TO THE CASTLE" : "OFF TO THE CAVE")
                    : t->cr_mode == CR_DIG ? "PICKING AT THE SAND"
                    : t->cr_mode == CR_FEED ? "GOING FOR FOOD"
                    : t->cr_mode == CR_EAT ? "EATING"
                    : t->cr_mode == CR_CLIMB ? "CLIMBING THE VOLCANO"
                    : t->cr_mode == CR_WALK ? "OUT FOR A WALK" : "RESTING";
    if (t->cr_find > 0) {                         /* COLLECT (0.3.9.4, Strato: a keeper who came by the arrows takes the find from here) */
        int bx, by, bw, bh; crab_collect_box(&bx, &by, &bw, &bh);
        button(c, bx, by, bw, bh, 0x9fd8e2, 0x9fd8e2, "", 2);   /* (MSP_TEAL, lit; MSP_INK lettering - defined further down) */
        const int tw = text_w("COLLECT", 2), lx = bx + (bw - 16 - 8 - tw) / 2;
        blit_icon(c, lx, by + (bh - 16) / 2, WORLD_ICON(icon_shop_sand_dollar_16), 255);
        draw_text(c, lx + 16 + 8, by + (bh - 14) / 2, 2, 0x031015, "COLLECT");
    } else draw_text(c, X + (W - text_w(now, 2)) / 2, Y + 162, 2, 0x9fd8e2, now);
}
static void urchin_card_draw(ctx_t *c, const tank_t *t) {
    ring(c, t->urchin_x, URCHIN_FLOOR_Y + t->urchin_dy - 3, 22, 0x9fd8e2);
    const int W = URCHIN_CARD_W, H = URCHIN_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 235);
    rect_edge(c, X, Y, W, H, 0x9fd8e2); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    ur_pose_t p; urchin_pose(&p, anim(t), tank_urchin_chewing(t));
    const int S = 3, ox = X + W / 2, oy = Y + 10 + 16 * S;               /* the dome's centre at 3x */
    for (int ly = -16; ly <= (int)UR_BASE; ly++)
        for (int lx = -18; lx <= 18; lx++) {
            int tone = urchin_tone((float)lx + 0.5f, (float)ly + 0.5f, &p);
            if (tone == UR_NONE) continue;
            src_t sc = src_color(URCHIN_RGB[tone], 1.0f);
            for (int yy = 0; yy < S; yy++) span(c, ox + lx * S, ox + lx * S + S - 1, oy + ly * S + yy, &sc, 255);
        }
    draw_text(c, X + (W - text_w("SEA URCHIN", 3)) / 2, Y + 90, 3, 0xffffff, "SEA URCHIN");
    const char *cap = "GRASS GRAZED SO FAR";
    draw_text(c, X + (W - text_w(cap, 2)) / 2, Y + 118, 2, 0x9fd8e2, cap);
    char n[24]; int cm = (int)(t->urchin_grazed_px / PX_PER_CM);
    /* CM under a meter, then M with one decimal, KM past a kilometer (0.3.9,
       Strato: "1.4K CM is odd"); the longest line, "999.9 M", fits the card */
    if (cm < 1) snprintf(n, sizeof n, "NOTHING YET");
    else if (cm < 100) snprintf(n, sizeof n, "%d CM", cm);
    else if (cm < 100000) snprintf(n, sizeof n, "%d.%d M", cm / 100, cm % 100 / 10);
    else snprintf(n, sizeof n, "%d.%d KM", cm / 100000, cm % 100000 / 10000);
    draw_text(c, X + (W - text_w(n, 3)) / 2, Y + 140, 3, 0xffffff, n);
    const char *now = tank_urchin_chewing(t) ? "CHEWING"
                    : t->urchin_frond >= 0 ? "OFF TO THE TALL GRASS"
                    : "RESTING";
    draw_text(c, X + (W - text_w(now, 2)) / 2, Y + 170, 2, 0x9fd8e2, now);
}

/* the shrimp school's card (2026-09-29, Strato: "one tap should bring up a
   card to show how much food they've eaten and progress to the next shrimp").
   The pips fill with pellets - filled vs hollow, never colour alone. */
static void shrimp_card_draw(ctx_t *c, const tank_t *t) {
    int n = t->shrimp_n;
    float cx = 0, cy = 0, r = 0;
    for (int i = 0; i < n; i++) { cx += t->shrimp[i].x; cy += t->shrimp[i].y; }
    cx /= n; cy /= n;
    for (int i = 0; i < n; i++) { float d = tank_dist(cx, cy, t->shrimp[i].x, t->shrimp[i].y); if (d > r) r = d; }
    ring(c, cx, cy, fminf(fmaxf(r + 14, 22), 90), 0x9fd8e2);
    const int W = SHRIMP_CARD_W, H = SHRIMP_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 235);
    rect_edge(c, X, Y, W, H, 0x9fd8e2); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    /* a shrimp at 4x, its legs paddling */
    src_t pal[SHRIMP_TONES];
    for (int k = 0; k < SHRIMP_TONES; k++) pal[k] = src_color(SHRIMP_RGB[k], 1.0f);
    const char *const *fr = fmod(t->clock * 2.5, 1.0) < 0.5 ? SHRIMP_IDLE_A : SHRIMP_IDLE_B;
    const int S = 4, sx = X + (W - 18 * S) / 2, sy = Y + 8;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 18; x++) {
            const char *k = fr[y][x] == '.' ? NULL : strchr(SHRIMP_KEYS, fr[y][x]);
            if (!k) continue;
            int ti = (int)(k - SHRIMP_KEYS);
            for (int yy = 0; yy < S; yy++) span(c, sx + x * S, sx + x * S + S - 1, sy + y * S + yy, &pal[ti], SHRIMP_ALPHA[ti]);
        }
    char line[32];
    snprintf(line, sizeof line, "%d SHRIMP", n);
    draw_text(c, X + (W - text_w(line, 3)) / 2, Y + 46, 3, 0xffffff, line);
    snprintf(line, sizeof line, "%d PELLET%s EATEN", (int)t->shrimp_eaten, t->shrimp_eaten == 1 ? "" : "S");
    draw_text(c, X + (W - text_w(line, 2)) / 2, Y + 76, 2, 0x9fd8e2, line);
    const char *cap = "NEXT SHRIMP";
    draw_text(c, X + (W - text_w(cap, 2)) / 2, Y + 104, 2, 0x9fd8e2, cap);
    bool full = n >= SHRIMP_MAX;
    int have = full ? SHRIMP_PER_JOIN : t->shrimp_food;
    const int pitch = 22, px0 = X + (W - (SHRIMP_PER_JOIN - 1) * pitch) / 2, py = Y + 132;
    for (int i = 0; i < SHRIMP_PER_JOIN; i++) {
        float x = px0 + i * pitch;
        if (i < have) {
            fill_ellipse(c, x, py, 6.5f, 6.5f, 0xffbd59, 255);
            fill_ellipse(c, x - 2, py - 2, 2.2f, 2.2f, 0xffe9bd, 255);
        } else ring(c, x, py, 6, 0x5a6a6e);
    }
    if (full) snprintf(line, sizeof line, "THE SCHOOL IS FULL");
    else if (tank_shrimp_refusing(t)) snprintf(line, sizeof line, "TOO MUCH ALGAE TO EAT");
    else if (t->shrimp_food >= SHRIMP_PER_JOIN && t->shrimp_cool > 0) {
        int m = (int)ceilf(t->shrimp_cool / 60.0f);
        snprintf(line, sizeof line, "ARRIVES IN %d MIN", m < 1 ? 1 : m);
    } else {
        int more = SHRIMP_PER_JOIN - t->shrimp_food;
        snprintf(line, sizeof line, "%d MORE PELLET%s", more, more == 1 ? "" : "S");
    }
    draw_text(c, X + (W - text_w(line, 2)) / 2, Y + 158, 2, 0xffffff, line);
}

/* the seahorse's card (0.3.6): a ring on it and a centred card - the
 * seahorse at 2x in its open curl, its days with the keeper, its hitches,
 * the trust it passed on and what it is doing */
static void seahorse_card_draw(ctx_t *c, const tank_t *t) {
    ring(c, t->sh_x + 4 * t->sh_facing, t->sh_y - SH_BODY_UP, 24, 0x9fd8e2);
    const int W = SEAHORSE_CARD_W, H = SEAHORSE_CARD_H, X = (TANK_W - W) / 2, Y = (TANK_H - H) / 2;
    src_t bg = src_color(0x04141a, 1.0f);
    for (int y = Y; y < Y + H; y++) span(c, X, X + W - 1, y, &bg, 235);
    rect_edge(c, X, Y, W, H, 0x9fd8e2); rect_edge(c, X + 1, Y + 1, W - 2, H - 2, 0x1c2f36);
    decor_scratch_t *sc = ds();
    sh_sprite(sc, sc->sh_spr_card, anim(t), SH_FREE, 0, 11.0f, 1.5f, tank_phase(t->clock, 9.0), 0);
    const int S = 2, ox = X + 14, oy = Y + (H - SH_H * S) / 2;
    for (int ly = 0; ly < SH_H; ly++)
        for (int lx = 0; lx < SH_W; lx++) {
            uint8_t b = sc->sh_spr_card[ly * SH_W + lx];
            if (!(b & SHB_ON)) continue;
            src_t s = src_color(SH_RGB[b & 15], 1.0f);
            for (int yy = 0; yy < S; yy++) span(c, ox + lx * S, ox + lx * S + S - 1, oy + ly * S + yy, &s, SH_ALPHA[b & 15]);
        }
    const int TX = X + 14 + SH_W * S + 12;   /* "+N.N TRUST PASSED ON" is 21 chars at scale 2: 252 px, inside the card from here */
    draw_text(c, TX, Y + 18, 3, 0xffffff, "SEAHORSE");
    char line[32];
    float days = tank_seahorse_days(t);
    if (days < 1) snprintf(line, sizeof line, "WITH YOU SINCE TODAY");
    else snprintf(line, sizeof line, "%d DAY%s WITH YOU", (int)days, (int)days == 1 ? "" : "S");
    draw_text(c, TX, Y + 52, 2, 0x9fd8e2, line);
    snprintf(line, sizeof line, "%d HITCH%s", (int)t->sh_hitches, t->sh_hitches == 1 ? "" : "ES");
    draw_text(c, TX, Y + 76, 2, 0xffffff, line);
    snprintf(line, sizeof line, "+%d.%d TRUST PASSED ON", (int)t->sh_trust_passed, (int)(t->sh_trust_passed * 10) % 10);
    draw_text(c, TX, Y + 100, 2, 0xffffff, line);
    snprintf(line, sizeof line, "%d PELLET%s SNAPPED", (int)t->sh_eaten, t->sh_eaten == 1 ? "" : "S");
    draw_text(c, TX, Y + 124, 2, 0xffffff, line);
    char now[32];
    if (t->sh_hitched) snprintf(now, sizeof now, "ON YOUR FINGER");
    else if (t->sh_mode == SH_HITCH) snprintf(now, sizeof now, "SWIMMING TO YOU");
    else if (t->night && t->sh_mode == SH_PERCH) snprintf(now, sizeof now, "ASLEEP");   /* only once it holds on (0.3.7: it said ASLEEP on the finger) */
    else if (t->sh_mode == SH_PERCH && t->sh_akind == SH_ANCHOR_SAND) snprintf(now, sizeof now, "RESTING ON THE SAND");
    else if (t->sh_mode == SH_PERCH) snprintf(now, sizeof now, "HOLDING %s", tank_seahorse_holding(t));
    else snprintf(now, sizeof now, "ADRIFT");
    draw_text(c, TX, Y + 156, 2, 0x9fd8e2, now);
}

/* ---- the critter cards' arrows (0.3.9.3) ---- */
static const int CRITTER_ORDER[] = { RENDER_CARD_SNAIL, RENDER_CARD_SHRIMP, RENDER_CARD_URCHIN, RENDER_CARD_SEAHORSE, RENDER_CARD_CRAB };
#define N_CRITTER_CARDS ((int)(sizeof CRITTER_ORDER / sizeof CRITTER_ORDER[0]))
bool aquarium_critter_here(const tank_t *t, int card) {
    switch (card) {
    case RENDER_CARD_SNAIL:    return (t->sd_unlocks & SD_ITEM_SNAIL) && t->snail_x >= 0;
    case RENDER_CARD_SHRIMP:   return (t->sd_unlocks & SD_ITEM_SHRIMP) && t->shrimp_n > 0;
    case RENDER_CARD_URCHIN:   return (t->sd_unlocks & SD_ITEM_URCHIN) && t->urchin_x >= 0;
    case RENDER_CARD_SEAHORSE: return (t->sd_unlocks & SD_ITEM_SEAHORSE) && t->sh_x >= 0;
    case RENDER_CARD_CRAB:     return (t->sd_unlocks & SD_ITEM_CRAB) && t->cr_x >= 0;
    default:                   return false;
    }
}
static bool crit_card_rect(int card, int *x, int *y, int *w) {   /* every critter card is centred; their heights differ */
    int h;
    switch (card) {
    case RENDER_CARD_SNAIL:    *w = SNAIL_CARD_W; h = SNAIL_CARD_H; break;
    case RENDER_CARD_SHRIMP:   *w = SHRIMP_CARD_W; h = SHRIMP_CARD_H; break;
    case RENDER_CARD_URCHIN:   *w = URCHIN_CARD_W; h = URCHIN_CARD_H; break;
    case RENDER_CARD_SEAHORSE: *w = SEAHORSE_CARD_W; h = SEAHORSE_CARD_H; break;
    case RENDER_CARD_CRAB:     *w = CRAB_CARD_W; h = CRAB_CARD_H; break;
    default: return false;
    }
    *x = (TANK_W - *w) / 2; *y = (TANK_H - h) / 2;
    return true;
}
static int crit_count(const tank_t *t) {
    int n = 0;
    for (int i = 0; i < N_CRITTER_CARDS; i++) n += aquarium_critter_here(t, CRITTER_ORDER[i]);
    return n;
}
int aquarium_critter_step(const tank_t *t, int card, int dir) {
    int at = -1;
    for (int i = 0; i < N_CRITTER_CARDS; i++) if (CRITTER_ORDER[i] == card) at = i;
    if (at < 0) return card;
    for (int k = 1; k < N_CRITTER_CARDS; k++) {
        int c = CRITTER_ORDER[((at + (dir < 0 ? -k : k)) % N_CRITTER_CARDS + N_CRITTER_CARDS) % N_CRITTER_CARDS];
        if (aquarium_critter_here(t, c)) return c;
    }
    return card;
}
int aquarium_critter_nav_hit(int card, float x, float y) {
    int X, Y, W;
    if (!crit_card_rect(card, &X, &Y, &W)) return 0;
    if (y < Y - 12 || y >= Y + RENDER_CRIT_NAV_IN + RENDER_CRIT_NAV_W + 14) return 0;      /* the box plus slop, most of it below and out */
    if (x >= X - 12 && x < X + RENDER_CRIT_NAV_IN + RENDER_CRIT_NAV_W + 12) return -1;
    if (x >= X + W - RENDER_CRIT_NAV_IN - RENDER_CRIT_NAV_W - 12 && x < X + W + 12) return 1;
    return 0;
}
static void crit_nav_draw(ctx_t *c, const tank_t *t, int card) {
    int X, Y, W;
    if (!crit_card_rect(card, &X, &Y, &W)) return;
    const uint32_t rgb = crit_count(t) > 1 ? 0x9fd8e2 : 0x2c4a52;   /* dim with one: nowhere to go (brightness, never hue) */
    crit_nav_box(c, X + RENDER_CRIT_NAV_IN, Y + RENDER_CRIT_NAV_IN, -1, rgb);
    crit_nav_box(c, X + W - RENDER_CRIT_NAV_IN - RENDER_CRIT_NAV_W, Y + RENDER_CRIT_NAV_IN, 1, rgb);
}
/* the card of the critter k of this tank has, with its arrows: false = card is not a critter of the tank's */
bool aquarium_critter_card(ctx_t *c, const tank_t *t, int card) {
    if (!aquarium_critter_here(t, card)) return false;
    switch (card) {
    case RENDER_CARD_SHRIMP:   shrimp_card_draw(c, t); break;
    case RENDER_CARD_URCHIN:   urchin_card_draw(c, t); break;
    case RENDER_CARD_CRAB:     crab_card_draw(c, t); break;
    case RENDER_CARD_SEAHORSE: seahorse_card_draw(c, t); break;
    default:                   snail_card_draw(c, t); break;
    }
    crit_nav_draw(c, t, card);
    return true;
}

/* the shelf's words (0.3.9), the aquarium's part: the pumpkin says when this build no longer sells it (so a SELL is an informed one), a
 * living piece on the shelf says how far it has grown (it keeps growing there). NULL = nothing to say (render.c: the shared words) */
const char *aquarium_shelf_note(const tank_t *t, int item, bool shelved, char *out, size_t n) {
    if (item == 8 && !SD_PUMPKIN_IN_SHOP) return "NOT IN THE SHOP NOW";
    if (!shelved) return NULL;
    if (item == 3) { snprintf(out, n, "GROWING  %d%%", (int)(tank_coral_growth(t) * 100 / CORAL_FULL + 0.5f)); return out; }
    if (item == 4) { snprintf(out, n, "GROWING  %d%%", (int)(tank_cluster_growth(t) * 100 / CLUSTER_FULL + 0.5f)); return out; }
    if (item == 0) return "GROWING";
    return NULL;
}
/* the shelf's little tanks (0.3.9, Strato: "represented inside a small
 * dedicated tank for each living decoration that is shelved"). A living piece
 * on the shelf (the plant, the coral, the cluster) is painted AS IT IS - its
 * color, its look, its growth - by its own painter at full size into a square
 * window of tank coordinates whose scratch is the frame itself (the page
 * paints over it a moment later: no buffer of its own), then box-filtered 3:1
 * into a 32 x 32 thumb (the icon's size) in the decor scratch (PSRAM;
 * internal RAM is spoken for); the window is 72..256 px a side, as tight as
 * the piece. The row shows it as is, the modal doubled (as the icons are). Rebuilt only when its key changes (growth to the percent, color /
 * look, the shop reopened), never per frame. The thumb stands in the row's
 * icon slot and, doubled, in the modal; the castle and the pumpkin, which do
 * not live, sit as their icon on a shelf board. */
#define SHELF_FULL  256                                                /* the widest window: a sword plant at its ceiling (the frame is wider on every glass) */
#define SHELF_WATER 0x0c2e3e
#define SHELF_SAND  0x8c7a4e
static int g_shelf_key[3];                                             /* the plant, the coral, the cluster: 0 = no thumb */
static int shelf_idx(int item) { return item == 0 ? 0 : item == 3 ? 1 : item == 4 ? 2 : -1; }
static int shelf_pct(const tank_t *t, int item) {
    return item == 3 ? (int)(tank_coral_growth(t) * 100 / CORAL_FULL + 0.5f) : item == 4 ? (int)(tank_cluster_growth(t) * 100 / CLUSTER_FULL + 0.5f) : (int)(t->veg_growth[3] * 100 + 0.5f);
}
static int shelf_key(const tank_t *t, int item) {
    uint32_t look = item == 3 ? tank_coral_rgb(t) : item == 4 ? (uint32_t)tank_cluster_scheme(t) : 0;
    return (int)((((look * 2654435761u) >> 12) & 0x7fff00u) + (uint32_t)shelf_pct(t, item) + 1u);
}
/* the window: square, as tight as the piece - its top 10 px over the piece's
 * highest pixel (the bloom and the crown included), 4 px of sand under the
 * floor, at least as wide as the piece and its mound, 72..192 px a side */
static int shelf_window(const tank_t *t, int item, int *top) {
    float ptop; int hw;
    if (item == 0)      { tank_veg_bed(t, 3, NULL, NULL, &ptop, NULL); hw = PLANT_HALF_W + 14; }
    else if (item == 3) { ptop = TANK_BOT - 14 + DECOR_SINK - 92 - (tank_coral_growth(t) > 1 ? 18 : 0); hw = CORAL_HALF_W + 10; }
    else                { ptop = TANK_BOT - 14 + DECOR_SINK - 120 - (tank_cluster_growth(t) > 1 ? 26 : 0); hw = CLUSTER_HALF_W + 10; }
    int s = TANK_BOT + 4 - ((int)ptop - 10);
    if (s < 2 * hw) s = 2 * hw;
    if (s < 72) s = 72;
    if (s > SHELF_FULL) s = SHELF_FULL;
    *top = TANK_BOT + 4 - s;
    return s;
}
static void shelf_build(const tank_t *t, int item, uint16_t *fb, int stride) {
    int k = shelf_idx(item), key = shelf_key(t, item);
    if (k < 0 || g_shelf_key[k] == key) return;
    g_shelf_key[k] = key;
    int cx = (int)tank_decor_x(t, item), top, side = shelf_window(t, item, &top);
    CTX_MADE();
    ctx_t c = { fb, stride, 1.0f, cx - side / 2, top, side, side };   /* the window lands at the frame's top left */
    rect_fill(&c, c.ox, c.oy, side, side, SHELF_WATER);
    rect_fill(&c, c.ox, TANK_BOT - 16, side, c.oy + side - (TANK_BOT - 16), SHELF_SAND);
    int bx0, by0, bx1, by1;
    if (item == 3)      { draw_coral(&c, cx, tank_coral_rgb(t), tank_coral_growth(t), false);
                          draw_coral_crown(&c, cx, tank_coral_rgb(t), tank_coral_growth(t), 0, &bx0, &by0, &bx1, &by1); }
    else if (item == 4) { draw_cluster(&c, cx, tank_cluster_scheme(t), tank_cluster_growth(t), false);
                          draw_cluster_crown(&c, cx, tank_cluster_scheme(t), tank_cluster_growth(t), 0, &bx0, &by0, &bx1, &by1); }
    else                  draw_veg(&c, t, 3, VEG_SEED[3], -1, false);
    uint16_t *th = ds()->shelf_thumb[k];                             /* the box filter: each thumb pixel averages its share of the window */
    for (int y = 0; y < SHELF_THUMB; y++) {
        int sy0 = y * side / SHELF_THUMB, sy1 = (y + 1) * side / SHELF_THUMB; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < SHELF_THUMB; x++) {
            int sx0 = x * side / SHELF_THUMB, sx1 = (x + 1) * side / SHELF_THUMB; if (sx1 <= sx0) sx1 = sx0 + 1;
            int r = 0, g = 0, b = 0, n = (sy1 - sy0) * (sx1 - sx0);
            for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) { uint16_t v = FB_SWAP16(fb[sy * stride + sx]); r += v >> 11; g += (v >> 5) & 63; b += v & 31; }
            th[y * SHELF_THUMB + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}
void aquarium_shelf_prepare(const tank_t *t, uint16_t *fb, int stride) {   /* before the page paints: every shelved living piece has its thumb */
    static const int living[3] = { 0, 3, 4 };
    for (int i = 0; i < 3; i++) if (t->sd_shelved & SD_ITEMS[living[i]].bit) shelf_build(t, living[i], fb, stride);
}
/* the icon slot's little tank (s = 1 on the row (32 px), 2 in the modal (64, as the icons)): the thumb, the glass, a glint - false: this
 * item has none (render.c draws the shelf board and its icon) */
bool aquarium_shelf_thumb(ctx_t *c, int x, int y, const tank_t *t, int item, int s) {
    (void)t;
    const int W = SHELF_THUMB * s, H = SHELF_THUMB * s; int k = shelf_idx(item);
    if (k >= 0 && g_shelf_key[k]) {
        const uint16_t *th = ds()->shelf_thumb[k];
        for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) if (CTX_IN(c, x + xx, y + yy)) CTX_PX(c, x + xx, y + yy) = FB_SWAP16(th[(yy / s) * SHELF_THUMB + xx / s]);
        rect_edge(c, x, y, W, H, MSP_TEAL);
        for (int i = 1; i < 3 * s; i++) px_blend(c, x + 2 * s + i, y + 2 * s, 0xffffff, 110);
        return true;
    }
    return false;
}
void aquarium_shelf_leave(void) { g_shelf_key[0] = g_shelf_key[1] = g_shelf_key[2] = 0; }
