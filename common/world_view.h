/* world_view.h - what the renderer asks of the world it draws (issue #4 phase 3).
 *
 * A world's scene - its backdrop, the things that live on its glass, the animals' stacking, the lights - and the choices its pages
 * make (which badges, which shop pictures, how many tools) are the world's, not render.c's: render.c is the engine (the pixel layer
 * of render_core.h, the pages) and reaches the world through one descriptor, world_view(), never by asking which world it is.
 * A world defines its descriptor in common/worlds/<world>/view.c, the scene behind it beside it (scene.c).
 *
 * Today a bin has ONE world compiled in (sim/Makefile and tankcore/CMakeLists.txt glob common/worlds/<PT_WORLD>/*.c), so world_view()
 * is that world's own definition; the step that builds both worlds into one image replaces it with the pick of a tank's world and
 * nothing else here changes. The animals are dispatched apart, per animal and by its species' body (body.h). */
#ifndef WORLD_VIEW_H
#define WORLD_VIEW_H
#include <stdint.h>
#include "tank.h"

typedef struct world_view_s {
    const char *name;                                           /* "aquarium" / "terrarium" */

    /* ---- the scene ---- */
    void (*render)(const tank_t *t, uint16_t *fb, int stride);  /* render_tank: the whole tank frame - scene, animals, what lives on the glass */
    void (*scene_reset)(uint16_t *scene);                       /* render_set_scene_cache: the scene cache is a new one - the world's own caches start again */
} world_view_t;

const world_view_t *world_view(void);

#endif
