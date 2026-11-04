/* render_scene_desert.h - the desert terrarium's static scene (pocket-terrarium P3a), for the scene cache.
 *
 * The back wall (layered sandstone), the sand, the warm end's warmth and the cool end's cool, the basking lamp's
 * glow and pool, the habitat's hides (common/habitat/desert.c: the warm cave, the humid box with its moisture
 * darkening the sand at its door, the cool cave) and the basking rock - at the habitat's places on whichever
 * glass (tank.h PLACE_*: the same rule as tank_floor_x, on a board given at run time, so one build draws all four
 * glasses for the sheets; scene_board_here is this build's).
 *
 * Standalone so far: render_tank's bake does not call it yet (P3b). It paints the whole frame (black outside the
 * glass) with the night's dim and an optional vignette LUT (render.c's g_vig), ordered-dithered to RGB565 in the
 * frame's byte order like render.c bake_scene. Rebake it when the light switches or a humid hide's moisture
 * changes its step (scene_desert_moist_step). */
#ifndef RENDER_SCENE_DESERT_H
#define RENDER_SCENE_DESERT_H
#include <stdbool.h>
#include <stdint.h>
#include "habitat.h"

/* a board's glass at run time (tank.h's TANK_* for this build) */
enum { SCENE_GLASS_RECT, SCENE_GLASS_ROUND, SCENE_GLASS_CORNERS };
typedef struct {
    const char *name;
    int   w, h;                 /* the frame: TANK_W x TANK_H */
    int   glass;                /* SCENE_GLASS_*: the rectangle, the bowl (centre w/2, h/2), round corners */
    float r;                    /* the bowl's radius, or the corners' */
    int   bot;                  /* TANK_BOT */
} scene_board_t;
#define SCENE_BOARDS 4
scene_board_t scene_board_here(void);                 /* this build's */
scene_board_t scene_board_of(int k);                  /* 0 the 1.8's rectangle, 1 the round pendant, 2 the watch, 3 the square */
float scene_glass_x0(const scene_board_t *b, float y);   /* the side glass at height y (tank_glass_x0's rule) */
float scene_glass_x1(const scene_board_t *b, float y);
float scene_place_y(const scene_board_t *b);          /* tank.h PLACE_Y */
float scene_floor_x(const scene_board_t *b, float u); /* tank_floor_x(u) on board b */

/* where the scene's things stand on a board (scene_desert_layout) */
typedef struct {
    int   kind;                 /* HIDE_* */
    float point_x;              /* the habitat's place (tank_hide_point) */
    float cx, base_y;           /* the hide as drawn: the middle of its foot on the sand (kept inside the glass) */
    float half_w, height;
    float door_x, door_y;       /* its door's middle at the sand, where a peeking head comes out ... */
    float door_hw, door_h;
    int   opens;                /* ... facing this way (+1 right, -1 left): toward the open floor */
} scene_hide_t;
typedef struct {
    scene_hide_t hide[HABITAT_HIDES_MAX];
    int   n_hides;
    float bask_x;               /* the basking spot (tank_bask_point) and the rock under it */
    float rock_x0, rock_x1, rock_top, rock_lift;   /* its flat top: a lizard there stands rock_lift px up (scene_desert_lift) */
    float wall_y;               /* the sand's back edge, where it meets the wall */
    float sand_y;               /* the middle lane's feet (TANK_BOT - 16; tank.h PLACE_Y + LIZARD_FLOOR_DY) */
} scene_desert_t;
void  scene_desert_layout(const scene_board_t *b, const habitat_t *h, scene_desert_t *d);
/* how far up a lizard's feet stand with its middle at x: the rock's top, easing to 0 at its rims (a step up) */
float scene_desert_lift(const scene_desert_t *d, float x);
/* a lizard peeking out of hide k: its middle and its feet's line for draw_lizard, and its yaw (out of the door) -
 * half_len its lizard_half_len, peek the pose's (how much of it shows from the nose) */
void  scene_desert_peek_at(const scene_desert_t *d, int k, float half_len, float peek, float *x, float *floor_y, float *yaw);

/* the moisture as the scene shows it, in steps: rebake when a humid hide's step changes */
int   scene_desert_moist_step(float moisture);
/* the scene into fb (stride in pixels, b->w x b->h): lit = the lamp on; moisture[k] for hide k (NULL: the habitat's
 * moist_start); dim the night's (render_tank: 0.45 dark, 1 lit); vig a w x h vignette LUT (render.c) or NULL */
void  scene_desert_draw(uint16_t *fb, int stride, const scene_board_t *b, const habitat_t *h, bool lit, const float *moisture,
                        float dim, const uint8_t *vig);
#endif
