/* render_scene_desert.h - the desert terrarium's static scene (pocket-terrarium P3a, P2c), for the scene cache.
 *
 * The back wall (layered sandstone), the sand, the warm end's warmth and the cool end's cool, the habitat's GROUND
 * (terrain.h: its rocks - sandstone terraces standing on the sand, or shelves on the back wall - and the flagstone ramps
 * between them), the basking lamp's glow, cone and pool over the basking spot up on the high rock, and the habitat's
 * hides on the floor (common/habitat/desert.c: the warm cave, the humid box with its moisture darkening the sand at its
 * door, the cool cave) - all where the engine has them on whichever glass (terrain_lay on a glass given at run time, so
 * one build draws all four glasses for the sheets; glass_here is this build's).
 *
 * Standalone so far: render_tank's bake does not call it yet (P3b). It paints the whole frame (black outside the
 * glass) with the night's dim and an optional vignette LUT (render.c's g_vig), ordered-dithered to RGB565 in the
 * frame's byte order like render.c bake_scene. Rebake it when the light switches or a humid hide's moisture
 * changes its step (scene_desert_moist_step).
 *
 * A LIZARD ON IT (P3b): the engine has its middle at (fish_t.x, fish_t.y) on its path (fish_t.surf) of the same ground
 * (tank_terrain): draw_lizard's feet line is fish_t.y + TERRAIN_FEET_DY - the sand, a rock's top or a ramp's, as the
 * scene draws them - and its body's pitch is fish_t.slope (+ the ground rising to the right). What stands lower on the
 * glass is in front: a lizard on a rock is drawn after the scene, so over the rocks behind it. */
#ifndef RENDER_SCENE_DESERT_H
#define RENDER_SCENE_DESERT_H
#include <stdbool.h>
#include <stdint.h>
#include "habitat.h"
#include "terrain.h"

/* where the scene's things stand on a glass (scene_desert_layout) */
typedef struct {
    int   kind;                 /* HIDE_* */
    float point_x;              /* the habitat's place (tank_hide_point) */
    float cx, base_y;           /* the hide as drawn: the middle of its foot on the sand (kept inside the glass) */
    float half_w, height;
    float door_x, door_y;       /* its door's middle at the sand, where a peeking head comes out ... */
    float door_hw, door_h;
    int   opens;                /* ... facing this way (+1 right, -1 left): toward the open floor */
} scene_hide_t;
#define SCENE_ROCKS_MAX (HABITAT_ROCKS_MAX + HABITAT_RAMPS_MAX)
#define SCENE_BEDS      32
typedef struct {                /* a drawn piece of the ground: a rock (a terrace or a shelf) or a ramp's slab */
    int   path;                 /* its path in the terrain (terrain_t.path: 1.. the rocks, then the ramps) */
    bool  slab;                 /* a ramp's flagstone slab, else a sandstone rock ... */
    bool  shelf;                /* ... a shelf on the back wall (rock_t.shelf), else a terrace standing on the sand */
    float l, r;                 /* its top face's ends: the walking line's, and a rock's lip past either end */
    float foot;                 /* a terrace: where its front face meets the sand; a shelf: its underside */
    float x0, x1, y0, k;        /* its top (the feet's line) from its walking line: y0 + (x - x0) k, x clamped to x0..x1, when ... */
    bool  straight;             /* ... the line is one straight piece (else terrain_y) */
    int   salt;
    /* the drawing's own (scene_desert_layout): a rock's front face in beds - each one's top under the top face (its seam
       sways a little along x), a hard one's bit, its edges past the top face's - and a few cracks (x, from, length, slant) */
    int      n_beds;
    uint32_t hard;
    float    bed_y[SCENE_BEDS + 1], ext_l[SCENE_BEDS], ext_r[SCENE_BEDS];
    float    crack[4][4];
} scene_rock_t;
typedef struct {
    glass_t        glass;
    terrain_t      tr;          /* the ground on this glass (terrain_lay): a lizard's line and feet (above) */
    scene_hide_t   hide[HABITAT_HIDES_MAX];
    int            n_hides;
    scene_rock_t   rock[SCENE_ROCKS_MAX];   /* the ground's pieces as drawn: of two over a pixel, the one whose top stands lower is in front */
    int            n_rocks;
    float          bask_x, bask_y;          /* the basking spot (tank_bask_point): a lizard's middle on it; the lamp is over it */
    float          wall_y;                  /* the sand's back edge, where it meets the wall */
    float          sand_y;                  /* the middle lane's feet on the floor (tank.h PLACE_Y + TERRAIN_FEET_DY) */
} scene_desert_t;
void  scene_desert_layout(const glass_t *b, const habitat_t *h, scene_desert_t *d);
/* the piece of the ground drawn at a pixel - its path (terrain_t.path's index), or 0 for the wall, the sand and the hides
 * (what the scene paints there before the hides): the sheet's check that every rock's and ramp's walking line has its own
 * top under the feet */
int   scene_desert_ground_at(const scene_desert_t *d, float x, float y);
/* a lizard peeking out of hide k: its middle and its feet's line for draw_lizard, and its yaw (out of the door) -
 * half_len its lizard_half_len, peek the pose's (how much of it shows from the nose) */
void  scene_desert_peek_at(const scene_desert_t *d, int k, float half_len, float peek, float *x, float *floor_y, float *yaw);

/* the moisture as the scene shows it, in steps: rebake when a humid hide's step changes */
int   scene_desert_moist_step(float moisture);
/* the scene into fb (stride in pixels, b->w x b->h): lit = the lamp on; moisture[k] for hide k (NULL: the habitat's
 * moist_start); dim the night's (render_tank: 0.45 dark, 1 lit); vig a w x h vignette LUT (render.c) or NULL */
void  scene_desert_draw(uint16_t *fb, int stride, const glass_t *b, const habitat_t *h, bool lit, const float *moisture,
                        float dim, const uint8_t *vig);
#endif
