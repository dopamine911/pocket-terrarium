/* terrain.h - THE TERRAIN (pocket-terrarium P2c): a habitat's walkable ground - the floor, its rocks and the ramps
 * between them (habitat.h rock_t, ramp_t) - laid out on a glass in pixels, for the walk (tank.c THE WALK), the heat
 * field and the places (tank.c), the scene (render_scene_desert.c) and the habitat's rules (habitat_check: every board).
 *
 * THE GLASS (glass_t) is a board's frame and the shape of its glass at run time: this build's (glass_here: tank.h's
 * TANK_*, the same arithmetic as tank_glass_x0 & co.) or any of the four boards' (glass_of: one build lays a habitat out
 * on all of them - its rules, the scene's sheets). A habitat gives its ground in FRACTIONS OF THE GLASS, so it lays out
 * on every board, the round pendant's bowl and the portrait watch too:
 *   up       v  0 the floor's walking line (TERRAIN_FLOOR_DY over the glass's bottom: an animal's middle standing on the
 *               sand, tank.h PLACE_Y) .. 1 the top of the glass:  y = PLACE_Y x (1 - v)
 *   across   u  0 the warm (left) side .. 1 the cool (right) side of the glass AT THAT HEIGHT, TERRAIN_MARGIN in - a rock
 *               high in the bowl spreads as the bowl does; on the floor u is tank_floor_x's (the places' u)
 * THE PATHS: everything walkable is a PATH, a polyline left to right (x-monotonic) of an animal's MIDDLE standing on it -
 * its feet stand TERRAIN_FEET_DY lower, on the rock's top (or the sand): path 0 the floor (PLACE_Y, glass to glass),
 * 1..n_rocks the habitat's rocks (their points mapped as above), then one per ramp - a straight slope from a point on one
 * surface to a point on another (a point along a rock is interpolated between its mapped points, so a ramp's ends lie
 * on its surfaces exactly). Where a ramp lands is a LINK: the two paths share the point, and there alone a walker steps
 * from one onto the other - never a drop, never a hop. A walker on a path is at an x on it: its y is the path's at x.
 * THE ROUTE (terrain_route): the quickest way along the paths and through the links from one (path, x) to another - a
 * Dijkstra over the links' sides (a dozen nodes); a slope costs its length over the walker's pace there (uphill x
 * (1 - TERRAIN_UP_SLOW sin a), downhill x (1 - TERRAIN_DOWN_SLOW sin a): the walk's own pace, tank.c).
 * THE RULES (terrain_check; habitat_check runs them on all four glasses): every path inside the glass with a lizard's
 * height under the glass above it; no slope past TERRAIN_SLOPE_MAX (a steeper one is a step: not walkable); a rock a
 * real rise off the floor (TERRAIN_LIFT_PX: the floor and its band stay open); two paths over one x at least
 * TERRAIN_HEADROOM_PX apart but near a point they share; every path reachable from the floor; the basking spot the
 * highest ground under the lamp, TERRAIN_LAMP_PX of glass over it; no rock or ramp low over a hide (TERRAIN_HIDE_HW,
 * TERRAIN_HIDE_CLEAR_PX). */
#ifndef TERRAIN_H
#define TERRAIN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "habitat.h"

/* ---- the glass ---- */
enum { GLASS_RECT, GLASS_ROUND, GLASS_CORNERS };
typedef struct {
    const char *name;
    int   w, h;                 /* the frame: TANK_W x TANK_H */
    int   kind;                 /* GLASS_*: the rectangle, the bowl (its centre (r, r)), round corners */
    float r;                    /* the bowl's radius, or the corners' */
    int   bot;                  /* TANK_BOT: the glass's bottom under the sand */
} glass_t;
#define GLASS_BOARDS 4
glass_t glass_here(void);                        /* this build's */
glass_t glass_of(int k);                         /* 0 the 1.8's rectangle, 1 the round pendant, 2 the watch, 3 the square */
float   glass_x0(const glass_t *g, float y);     /* the side glass at height y (tank_glass_x0's rule) */
float   glass_x1(const glass_t *g, float y);
float   glass_top(const glass_t *g, float x);    /* the glass above x (tank_glass_top's) */
float   glass_place_y(const glass_t *g);         /* the floor's walking line: tank.h PLACE_Y */
float   glass_floor_x(const glass_t *g, float u);   /* a floor fraction's x: tank_floor_x */

/* ---- the numbers ---- */
#define TERRAIN_FLOOR_DY      24.0f     /* the floor's walking line over the glass's bottom: tank.h PLACE_Y = TANK_BOT - this */
#define TERRAIN_MARGIN        30.0f     /* u 0 and 1 stand this far in from the side glass at their height (tank.h PLACE_MARGIN) */
#define TERRAIN_FEET_DY        8.0f     /* an animal's feet stand this far below its middle: the rock's top (the sand) under it */
#define TERRAIN_SLOPE_MAX     40.0f     /* degrees: the steepest ground a lizard walks; steeper is a step, not walkable */
#define TERRAIN_UP_SLOW        0.5f     /* its pace on a slope: x (1 - this x sin a) uphill ... */
#define TERRAIN_DOWN_SLOW      0.15f    /* ... and x (1 - this x sin a) downhill, a little careful */
#define TERRAIN_LIFT_PX       40.0f     /* a rock stands at least this high over the floor's line, all of it: a raised rock, the floor open */
#define TERRAIN_HEADROOM_PX   32.0f     /* two paths over one x: at least this far apart (a lizard under a ledge fits) ... */
#define TERRAIN_JUNCTION_PX   48.0f     /* ... but within this of a point both pass through (a switchback's corner) */
#define TERRAIN_LINK_IN_PX    48.0f     /* a ramp's ends keep this far inside the glass: every lizard's middle reaches them - the biggest
                                           body drawn, an elder gecko's half length ~47.6 px, is the walk's margin (tank.c walk_span;
                                           species_check refuses a pack whose body is longer) */
#define TERRAIN_HEAD_PX       24.0f     /* a walking line keeps this far under the glass above it (a lizard's head) */
#define TERRAIN_LAMP_PX      100.0f     /* the basking spot keeps this much glass over it: the lamp and its cone */
#define TERRAIN_HIDE_HW       36.0f     /* a hide's width either side of its point (the widest, the warm cave, is 35 px) ... */
#define TERRAIN_HIDE_CLEAR_PX 44.0f     /* ... over which no rock or ramp runs lower than this over the floor's line (a hide shows) */

/* ---- the laid-out ground ---- */
enum { PATH_FLOOR, PATH_ROCK, PATH_RAMP };
#define TERRAIN_PATHS_MAX (1 + HABITAT_ROCKS_MAX + HABITAT_RAMPS_MAX)
#define TERRAIN_LINKS_MAX (2 * HABITAT_RAMPS_MAX)
typedef struct {
    uint8_t kind;                           /* PATH_* */
    uint8_t n;                              /* its points, 2..ROCK_PTS_MAX */
    float   x[ROCK_PTS_MAX], y[ROCK_PTS_MAX];   /* the walking line, left to right: an animal's middle on it */
    float   cost_r[ROCK_PTS_MAX - 1], cost_l[ROCK_PTS_MAX - 1];   /* each segment's seconds per px across, walked rightward / leftward at
                                                                   a pace of 1 px/s (its length over the slope's pace: terrain_route) */
} terrain_path_t;
typedef struct { uint8_t a, b; float x, y; } terrain_link_t;   /* paths a and b meet at (x, y): a ramp's end where it lands */
typedef struct {
    glass_t        glass;
    terrain_path_t path[TERRAIN_PATHS_MAX];  /* 0 the floor, 1..n_rocks the rocks (habitat_t.rocks), then the ramps in order */
    uint8_t        n_paths, n_rocks;
    terrain_link_t link[TERRAIN_LINKS_MAX];
    uint8_t        n_links;
    int8_t         bask_path;               /* the basking spot: on this path (habitat_t.bask_surf) ... */
    float          bask_x, bask_y;          /* ... here (an animal's middle on it) */
} terrain_t;
void  terrain_lay(const habitat_t *h, const glass_t *g, terrain_t *out);   /* (the habitat's data passed habitat_check's own rules) */
/* a surface's point: 0 the floor at floor fraction u, k rock k at u along it (between its mapped points) */
void  terrain_point(const terrain_t *tr, const habitat_t *h, int surf, float u, float *x, float *y);
float terrain_y(const terrain_path_t *p, float x);          /* the walking line at x (an end's beyond it) */
float terrain_tilt(const terrain_path_t *p, float x, int dir);   /* the line's angle at x, radians, + rising to the right (at a
                                                                   point: the segment toward dir, +1 right / -1 left) */
float terrain_pace(float climb);                            /* the pace's share walking a slope of `climb` radians (+ uphill) */
int   terrain_at(const terrain_t *tr, float x, float y);    /* the path over x whose line is nearest y (the floor if none nearer) */
bool  terrain_on(const terrain_path_t *p, float x, float y, float tol);   /* (x, y) on p's line, within tol px */
/* THE ROUTE from (from, fx) to (to, tx): false = no way there. hop: walk along `from` to x, then step onto path via there
 * (-1: x is the destination itself); entry: where the route comes onto `to` (fx if from is to); cost: its seconds' worth
 * at a pace of 1 px/s */
typedef struct { float x; int via; float entry, cost; } terrain_hop_t;
bool  terrain_route(const terrain_t *tr, int from, float fx, int to, float tx, terrain_hop_t *hop);
/* THE AIR at (x, y) by the habitat (tank.c's heat field, the habitat's rules): lit, the floor's gradient by the floor's
 * fraction at x (warm end to cool end), rise_c x the height's fraction v at y (the lamp's heat rising), and the lamp's pool
 * round the basking spot - bask_c in its middle, blending to the air's own at bask_r_px; dark, the gradient night_drop_c
 * cooler, no rise, no lamp. _open_c: the lit air without the lamp's pool (what the lamp warms its spot over) */
float terrain_air_c(const terrain_t *tr, const habitat_t *h, float x, float y, bool lit);
/* ... with the lamp's pool lamp_x as wide (pocket-terrarium P5: the shop's BASKING BULB, tank.c) */
float terrain_air_lamp_c(const terrain_t *tr, const habitat_t *h, float x, float y, bool lit, float lamp_x);
float terrain_air_open_c(const terrain_t *tr, const habitat_t *h, float x, float y);
/* the rules on glass g (habitat_check calls it for every board; the habitat's own data rules first): false with the first
 * broken one in why */
bool  terrain_check(const habitat_t *h, const glass_t *g, char *why, size_t n);
#endif
