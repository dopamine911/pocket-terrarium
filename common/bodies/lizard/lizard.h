/* render_lizard.h - the lizard body plan's renderer (pocket-terrarium P3a): draw_lizard.
 *
 * One renderer for every PLAN_LIZARD pack (species.h): the pack's lizard_geom_t sets the body (its length, the
 * head's width, the limbs, the eye, how much the tail shows the fat reserve), its morph the colours and its
 * pattern for the stage the markings (BANDED, SPOTTED; anything else is drawn plain). The engine's state picks
 * the pose (tank.h LZ_POSE_*), the turn (fish_t.yaw / yaw_tail), the ground's slope under it (fish_t.slope: the body
 * pitched along it), the walk's phase, the blink, the tongue, the fat reserve and the shed (tank.h SHED_*) - the engine's
 * draw-only fields (fish_t lz_*, tank.c THE LOOK). Seen from the side, a little from above: the feet on the near side
 * stand a little lower than the far ones.
 *
 * render_tank draws every lizard with it in the terrarium (render.c THE TERRARIUM, P3b), over the desert scene
 * (render_scene_desert.h). `fishsim --lizard-sheet <prefix>` (sim/sheet_lizard.c) draws every morph, stage, pose, the
 * walk, the sheds and the scene on all four glasses, and measures the cost.
 *
 * Cost: the body (head, trunk, tail) is a grid of tones built only for a pose not drawn lately and blitted
 * every frame (render.c's crab, cr_build); the legs, the eyes, the tongue and the mouth are drawn live over it.
 * So a walking lizard is one cached body however long it walks; a turn builds a body per step of its yaw, a ramp one
 * per step of its pitch. */
#ifndef RENDER_LIZARD_H
#define RENDER_LIZARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tank.h"                   /* LZ_POSE_*, SHED_*, STAGE_*; species.h's lizard_geom_t, morph_t, pattern_t */

/* where it draws: a frame (or the scene cache) in the frame's byte order (render.h FB_SWAP16), the window of the
 * frame it covers (render.c ctx_t's ox, oy, w, h: everything outside is clipped) and the night's dim (render_tank:
 * 0.45 dark, 1 lit). The light it stands in, as the scene's (render_scene_desert.h scene_light_rgb): glow, the lamp's
 * pool's gain at it (0 none), moon, the lamp out (moonlit). mark, when set, is called for every run of pixels written
 * (frame coordinates, x0..x1 inclusive) - render.c's dirty / touched masks and the drawn box; NULL = nothing marked.
 * cover, when set, is what stands IN FRONT of it (a hide's front, as it goes in, comes out or looks out): one bit a pixel,
 * cover_wpr 32-bit words a row, for the cover_w x cover_h box at (cover_x0, cover_y0) in frame coordinates - a pixel whose
 * bit is set is not drawn (nor marked) */
typedef struct {
    uint16_t *fb; int stride;
    int ox, oy, w, h;
    float dim;
    void (*mark)(void *user, int x0, int x1, int y);
    void *user;
    float glow;
    bool  moon;
    const uint32_t *cover;
    int   cover_x0, cover_y0, cover_w, cover_h, cover_wpr;
} lz_canvas_t;

/* who it is: the pack's body, its morph, the pattern its stage shows, the stage (STAGE_FRY = a hatchling ..
 * STAGE_ELDER) and its own markings' seed (fish_t.uid: no two geckos spotted alike) */
typedef struct {
    const lizard_geom_t *geom;
    const morph_t       *morph;
    const pattern_t     *pattern;
    uint8_t              stage;
    uint32_t             seed;
} lizard_look_t;

/* what it is doing. Everything but kind defaults to 0 (lizard_pose: yaw 1, the tail's yaw with it, fat 0.5) */
typedef struct {
    uint8_t kind;           /* LZ_POSE_* (tank.h): STAND WALK TURN FLICK STALK LUNGE BASK DIG REST HIDDEN PEEK STANDOFF */
    float   yaw, yaw_tail;  /* which way it faces (fish_t.yaw): +1 right .. 0 head-on (its face to the glass) .. -1 left;
                               the tail's a beat behind (fish_t.yaw_tail; pass yaw again if the engine keeps none) */
    float   pitch;          /* the ground's slope under it (fish_t.slope): radians, + rising to the right - the whole lizard
                               turns with the ground about its feet's point, so facing uphill its nose is up (slope x yaw) */
    float   phase;          /* 0..1, wrapping: the walk's cycle (WALK, TURN's shuffle, STALK's creep - the diagonal pairs
                               step together), the dig's scrabble (DIG); LUNGE: how far it has reached, 0..1 */
    float   blink;          /* 0 open .. 1 shut (REST is drawn shut, BASK at least half-lidded) */
    float   tongue;         /* 0 in .. 1 out (a flick: up and back down over ~0.25 s) */
    float   fat;            /* the fat reserve 0..1: the tail's girth (scaled by the pack's geom.tail_fat) */
    uint8_t shed;           /* SHED_* (tank.h): SOON dull and pale with blue-grey eyes; SHEDDING the old skin loose from
                               the nose back (shed_k); STUCK white rings left on the toes and the tail tip */
    float   shed_k;         /* SHEDDING: how far back from the nose the old skin has loosened, 0..1 */
    float   peek;           /* PEEK: how much of it shows out of the hide, from the nose: 0..1 (0.25: the head) */
    float   clock;          /* seconds (the tank's clock): the breath (REST, BASK), the stalk's tail tip, the standoff's
                               tail */
} lizard_pose_t;
lizard_pose_t lizard_pose(int kind, float yaw);

/* its size: species.h lizard_stage_scale, lizard_half_len (the walk keeps the body drawn inside the glass by it) and the
 * tank's tank_lizard_scale (its stage's x its own build). Its MORPH, picked by its body colour's gene - tank_gene
 * GENE_BODY, a LOOK_COLORS index: teal, green, yellow -> NORMAL; coral -> TANGERINE; pink -> ALBINO; blue, silver ->
 * BLIZZARD; lavender, charcoal -> MACK SNOW (the gecko's five, species/gecko.c; a pack with fewer morphs wraps) - so the
 * keeper's pick and heredity's colour genes carry over and the save keeps it (the body colour is saved); its pattern is
 * its stage's (species_t.pattern). The accent's gene and the fish's forms (shape, fins, pattern) draw nothing on a
 * lizard. lizard_morph_index: the morph's index in sp->morphs (-1: a pack without morphs). lizard_morph_body: the
 * LOOK_COLORS index the setup's MORPH page gives a hatchling for morph m - each morph's own swatch (NORMAL yellow,
 * TANGERINE coral, ALBINO pink, BLIZZARD silver, MACK SNOW lavender), the colour its name and card border wear */
const morph_t *lizard_morph_of(const species_t *sp, const fish_t *f);
int lizard_morph_index(const species_t *sp, const fish_t *f);
int lizard_morph_body(const species_t *sp, int m);

/* where the engine's lizard stands to where it is drawn: (fish_t.x, fish_t.y) is its middle on its path of the ground
 * (fish_t.surf: the floor's band, a rock's top, a ramp - tank_terrain); its feet stand TERRAIN_FEET_DY (terrain.h) below
 * that - draw_lizard's floor_y is fish_t.y + TERRAIN_FEET_DY - and the ground under it slopes fish_t.slope (radians, +
 * rising to the right: lizard_pose_t.pitch). x is the middle of its length - lizard_half_len of it on either side when it
 * faces sideways */

/* the lizard, its middle at x, its feet on floor_y, at scale (lizard_stage_scale x anything the engine adds, ~0.3..1.4):
 * its contact shadow, the legs away from the glass, the body, the near legs, the eyes and lids, the tongue and the
 * mouth. HIDDEN draws nothing */
void draw_lizard(const lz_canvas_t *cv, const lizard_look_t *look, const lizard_pose_t *pose, float x, float floor_y, float scale);
/* the box the last draw_lizard wrote (frame coordinates, inclusive); false = it drew nothing */
bool lizard_last_box(int *x0, int *y0, int *x1, int *y1);
/* how far a lizard standing side-on at scale reaches from its middle (draw_lizard's x): back to its tail's tip, ahead to
 * its snout's with the tongue all the way out - every pixel it draws, in x, lies in x - back .. x + ahead (the pages fit
 * a portrait by it: render.c lizard_still) */
void lizard_reach(const lizard_geom_t *g, float scale, float *back, float *ahead);

/* MEMORY: the cached bodies and the build's scratch live in one block the platform hands over (the firmware: PSRAM,
 * like render_set_decor_scratch); without one it is calloc'd on the first draw (the sim) */
size_t lizard_scratch_bytes(void);
void   lizard_set_scratch(void *buf);
/* THE FRAME'S BUILDS: a new body (a pose, a step of a turn) costs a build, ~6x a cached draw. Called once a frame,
 * at most `builds` of them are made that frame; a lizard over the budget is drawn with its last body a frame more
 * (its legs, eyes and tongue live as ever). Negative (the default): no limit. Returns what was left of the budget it
 * replaces (render.c keeps a tank tick's remainder: the same tick drawn twice builds no more and falls back alike) */
int    lizard_frame(int builds);
/* (tests, the bench) off: a build every draw; how many builds so far; how many bodies were cut to the grid's size
 * (0 at every size the packs use: the sheet checks) */
void   lizard_cache_on(bool on);
int    lizard_cache_builds(void);
int    lizard_cache_clipped(void);
#endif
