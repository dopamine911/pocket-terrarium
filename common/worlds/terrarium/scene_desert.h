/* scene_desert.h - the desert terrarium's static scene (pocket-terrarium P3a, P2c; common/worlds/terrarium since issue #4 phase 3), for the scene cache.
 *
 * The back wall (layered sandstone), the sand, the warm end's warmth and the cool end's cool, the habitat's GROUND
 * (terrain.h: its rocks - sandstone terraces standing on the sand, or shelves on the back wall - and the flagstone ramps
 * between them), the basking lamp's glow, cone and pool over the basking spot up on the high rock, and the habitat's
 * hides on the floor (common/habitat/desert.c: the warm cave, the humid box with its moisture darkening the sand at its
 * door, the cool cave) - all where the engine has them on whichever glass (terrain_lay on a glass given at run time, so
 * one build draws all four glasses for the sheets; glass_here is this build's).
 *
 * render_tank (scene.c, P3b) bakes it into the scene cache - one per light, lit and dark, so the light's
 * switch is a swap - in slices over frames (scene_desert_begin + scene_desert_rows), and a humid hide's moisture step
 * repaints only its box (scene_desert_moist_box). It paints the whole frame (black outside the glass) with the night's dim
 * and an optional vignette LUT (render.c's g_vig), ordered-dithered to RGB565 in the frame's byte order like render.c
 * bake_scene.
 *
 * A LIZARD ON IT: the engine has its middle at (fish_t.x, fish_t.y) on its path (fish_t.surf) of the same ground
 * (tank_terrain): draw_lizard's feet line is fish_t.y + TERRAIN_FEET_DY - the sand, a rock's top or a ramp's, as the
 * scene draws them - and its body's pitch is fish_t.slope (+ the ground rising to the right). What stands lower on the
 * glass is in front: a lizard on a rock is drawn after the scene, so over the rocks behind it; one going into a hide, or
 * just out of it, or looking out, is behind the hide's front (scene_desert_hide_part); in the lamp's pool it is lit as the
 * rock under it (scene_desert_lamp_at, scene_light_rgb). */
#ifndef RENDER_SCENE_DESERT_H
#define RENDER_SCENE_DESERT_H
#include <stdbool.h>
#include <stdint.h>
#include "habitat.h"
#include "terrain.h"

/* THE LIGHT on a thing in the scene (lizard.h's palette through lz_canvas_t's glow and moon; the scene's own pixels
 * the same): lit, the lamp's pool's gain g at it (scene_desert_lamp_at) warms it r x (1 + g), g x (1 + 0.8 g),
 * b x (1 + 0.42 g) - and where that passes white, the three scaled down together (scene_glow_fit: the hue kept, so a
 * yellow gecko or a tan rock in the pool stays warm, never bleached to lemon, and a shadow over it darkens to brown); dark,
 * moonlit - x 0.8, then 0.3 of the way to the night's blue */
#define SCENE_GLOW_G    0.80f
#define SCENE_GLOW_B    0.42f
#define SCENE_NIGHT_RGB 0x182238u
#define SCENE_NIGHT_K   0.80f
#define SCENE_NIGHT_MIX 0.30f
static inline void scene_glow_fit(float *r, float *g, float *b) {
    const float m = *r > *g ? (*r > *b ? *r : *b) : (*g > *b ? *g : *b);
    if (m > 255) { const float k = 255 / m; *r *= k; *g *= k; *b *= k; }
}
static inline uint32_t scene_light_rgb(uint32_t rgb, float glow, bool moon) {
    float r = (float)(rgb >> 16 & 255), g = (float)(rgb >> 8 & 255), b = (float)(rgb & 255);
    if (glow > 0) { r *= 1 + glow; g *= 1 + SCENE_GLOW_G * glow; b *= 1 + SCENE_GLOW_B * glow; scene_glow_fit(&r, &g, &b); }
    if (moon) {
        r = r * SCENE_NIGHT_K + ((float)(SCENE_NIGHT_RGB >> 16 & 255) - r * SCENE_NIGHT_K) * SCENE_NIGHT_MIX;
        g = g * SCENE_NIGHT_K + ((float)(SCENE_NIGHT_RGB >> 8 & 255) - g * SCENE_NIGHT_K) * SCENE_NIGHT_MIX;
        b = b * SCENE_NIGHT_K + ((float)(SCENE_NIGHT_RGB & 255) - b * SCENE_NIGHT_K) * SCENE_NIGHT_MIX;
    }
    const int ri = r > 255 ? 255 : (int)r, gi = g > 255 ? 255 : (int)g, bi = b > 255 ? 255 : (int)b;
    return (uint32_t)ri << 16 | (uint32_t)gi << 8 | (uint32_t)bi;
}

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
   half_len its lizard_half_len, peek the pose's (how much of it shows from the nose) */
void  scene_desert_peek_at(const scene_desert_t *d, int k, float half_len, float peek, float *x, float *floor_y, float *yaw);
/* what of hide k is at a pixel, as the scene paints it: none, its FRONT (its wall, its lid and moss, its sill) or its
   DOOR's opening (the dark inside) - a lizard behind the hide's front shows only through the door; and the box every
   pixel of it lies in (frame coordinates, inclusive) */
enum { SCENE_HIDE_NONE, SCENE_HIDE_FRONT, SCENE_HIDE_DOOR };
int   scene_desert_hide_part(const scene_desert_t *d, int k, float x, float y);
void  scene_desert_hide_box(const scene_desert_t *d, int k, int *x0, int *y0, int *x1, int *y1);
/* hide k's front stands IN FRONT of a lizard at its door at (x, y) (scene.c's cover, as it goes in, comes out or looks
   out): a pixel of the front on the closed side of the door's edge at that row - the door's outline there (the cave's
   arch, the box's round hole), its widest on a row the outline does not reach; the front on the open side and the door's
   dark stand behind it */
bool  scene_desert_hide_covers(const scene_desert_t *d, int k, float x, float y);
/* the lamp's pool's gain on a thing standing at (x, y) (0 outside it; the scene's own pool on the rock) */
float scene_desert_lamp_at(const scene_desert_t *d, const habitat_t *h, float x, float y);

/* the moisture as the scene shows it, in steps: a humid hide's step changes only its box - the hide and the sand at its
   door (scene_desert_moist_box; false for a hide that is not humid) */
int   scene_desert_moist_step(float moisture);
bool  scene_desert_moist_box(const scene_desert_t *d, int k, int *x0, int *y0, int *x1, int *y1);
/* the scene into fb (stride in pixels, b->w x b->h): lit = the lamp on; moisture[k] for hide k (NULL: the habitat's
   moist_start); dim the night's (render_tank: 0.45 dark, 1 lit); vig a w x h vignette LUT (render.c) or NULL */
void  scene_desert_draw(uint16_t *fb, int stride, const glass_t *b, const habitat_t *h, bool lit, const float *moisture,
                        float dim, const uint8_t *vig);
/* ... IN SLICES: scene_desert_begin works out what a bake paints from (the layout, the pebbles, the plants, each column's
   own) for these inputs, into the module's one context (the glass is copied; h and vig must outlive the bake); then
   scene_desert_rows paints rows y0..y1, columns x0..x1 (inclusive, clipped to the glass's frame) into fb - any order,
   any buffer, until the next begin. scene_desert_draw is begin and every row */
void  scene_desert_begin(const glass_t *b, const habitat_t *h, bool lit, const float *moisture, float dim, const uint8_t *vig);
void  scene_desert_rows(uint16_t *fb, int stride, int y0, int y1, int x0, int x1);
unsigned scene_desert_begun(void);     /* how many begins so far: slices across frames check the context is still theirs */
#endif
