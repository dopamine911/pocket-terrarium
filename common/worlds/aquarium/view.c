/* view.c - THE AQUARIUM's descriptor for the renderer (world_view.h): the scene's hooks and the pages' choices. */
#include "world_view.h"
#include "worlds/aquarium/scene.h"

const world_view_t aquarium_view = {
    .name        = "aquarium",
    .render      = aquarium_render,
    .scene_alt   = aquarium_scene_alt,
    .critter_here = aquarium_critter_here, .critter_step = aquarium_critter_step, .critter_nav_hit = aquarium_critter_nav_hit,
    .critter_card = (void (*)(ctx_t *, const tank_t *, int))aquarium_critter_card, .crab_collect_hit = aquarium_crab_collect_hit,
    .shelf_prepare = aquarium_shelf_prepare, .shelf_thumb = aquarium_shelf_thumb, .shelf_note = aquarium_shelf_note, .shelf_leave = aquarium_shelf_leave,
};
const world_view_t *world_view(void) { return &aquarium_view; }
