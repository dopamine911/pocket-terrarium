/* view.c - THE TERRARIUM's descriptor for the renderer (world_view.h): the scene's hooks and the pages' choices. */
#include "world_view.h"
#include "worlds/terrarium/scene.h"

const world_view_t terrarium_view = {
    .name        = "terrarium",
    .render      = terrarium_render,
    .scene_reset = terrarium_scene_reset,
};
const world_view_t *world_view(void) { return &terrarium_view; }
