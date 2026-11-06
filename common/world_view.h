/* world_view.h - what the renderer asks of the world it draws (issue #4 phase 3).
 *
 * A world's scene - its backdrop, the things that live on its glass, the animals' stacking, the lights - and the choices its pages
 * make (which badges, which shop pictures, how many tools) are the world's, not render.c's: render.c is the engine (the pixel layer
 * of render_core.h, the pages) and reaches the world through one descriptor, world_view(), never by asking which world it is.
 * A world defines its descriptor in common/worlds/<world>/view.c, the scene behind it beside it (scene.c).
 *
 * Today a bin has ONE world compiled in (sim/Makefile and tankcore/CMakeLists.txt glob the directory common/worlds/<PT_WORLD>), so world_view()
 * is that world's own definition; the step that builds both worlds into one image replaces it with the pick of a tank's world and
 * nothing else here changes. The animals are dispatched apart, per animal and by its species' body (body.h). */
#ifndef WORLD_VIEW_H
#define WORLD_VIEW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "render_core.h"
#include "body.h"
#include "tank.h"

/* a badge of the keeper page: the bit that earns it (MS_* of an animal's, TMS_* of the tank's) and its art */
typedef struct { uint32_t bit; const icon_t *icon; } badge_t;
/* the colours of the loading screen: the water or the dusk behind it (top, bottom) and its ring of bubbles or pebbles (the bright one,
   its trail), shaded toward rim_dark */
typedef struct { uint32_t bg[2]; uint32_t rim[4]; uint32_t rim_dark; } world_look_t;

typedef struct world_view_s {
    const char *name;                                           /* "aquarium" / "terrarium" */
    const body_ops_t *const *bodies; int n_bodies;              /* the body types this world draws (body.h): body_of picks the one of a species' plan, the first when none is */

    /* ---- the scene ---- */
    void (*render)(const tank_t *t, uint16_t *fb, int stride);  /* render_tank: the whole tank frame - scene, animals, what lives on the glass */
    void (*scene_reset)(uint16_t *scene);                       /* render_set_scene_cache: the scene cache is a new one - the world's own caches start again (NULL: none) */
    const uint16_t *(*scene_alt)(unsigned *epoch);              /* render_scene_buf: the world's own cache of the scene when it is the current one (the aquarium's grass cache), with its epoch; NULL: none */

    /* ---- the glass and the tools ---- */
    uint32_t algae_rgb[2];                                      /* the film on the glass: the two greens (a terrarium's: dusty smudges) of a blob, by its hash's bit 2 */
    const int *tools; int n_tools;                              /* the toolbox's tools (TOOL_*), left to right: the box is cut in as many equal parts; two draw their icons at 2x, three at 1x */

    /* ---- the keeper page ---- */
    const badge_t *fish_badges;                                 /* an animal's six */
    const badge_t *tank_badges; int n_tank_badges;              /* the tank's row ... */
    bool (*tank_badge_shown)(const tank_t *t, int i);           /* ... which of them show (NULL: all) */
    const icon_t *(*req_icon)(int kind);                        /* the art of a fry gate the shared pages do not draw (NULL: none) */
    const char *(*tank_word)(const tank_t *t);                  /* the TANK row's title: "TANK", a terrarium's habitat's name */

    /* ---- the shop ---- */
    const icon_t *(*shop_icon)(int item);                       /* an item's picture */
    int  (*shop_count)(const tank_t *t);                        /* the items its shop lists now (the FRY row apart) */
    int  (*shop_item)(const tank_t *t, int i);                  /* the catalogue number of the i-th */

    /* ---- the looks ---- */
    int bowl_title_s;                                           /* MY TANKS's title scale on the round glass (the terrarium's is longer: a size down) */
    world_look_t look;                                          /* the loading screen */

    /* ---- the critters' cards (render_stats_card with a RENDER_CARD_* index, render.h): the critters a world has; NULL hooks: it has none ---- */
    bool (*critter_here)(const tank_t *t, int card);            /* that critter is in the tank (its card would draw) */
    int  (*critter_step)(const tank_t *t, int card, int dir);   /* the card the arrows step to */
    int  (*critter_nav_hit)(int card, float x, float y);        /* -1 the left arrow, +1 the right, 0 neither */
    void (*critter_card)(ctx_t *c, const tank_t *t, int card);  /* draws the card of a critter that is here, with its arrows */
    bool (*crab_collect_hit)(const tank_t *t, float x, float y);/* the crab card's COLLECT */

    /* ---- the shop's shelf (render_shop): living pieces shelved in a little tank of their own ---- */
    void (*shelf_prepare)(const tank_t *t, uint16_t *fb, int stride);   /* before the page paints: every shelved living piece has its thumb (the frame is their scratch) */
    bool (*shelf_thumb)(ctx_t *c, int x, int y, const tank_t *t, int item, int s);   /* an item's thumb in its slot: false = it has none (the board and its icon) */
    const char *(*shelf_note)(const tank_t *t, int item, bool shelved, char *out, size_t n);   /* a piece's own line in its modal; NULL: the shared ones */
    void (*shelf_leave)(void);                                  /* the shop is left: the thumbs are stale */
} world_view_t;

const world_view_t *world_view(void);

#endif
