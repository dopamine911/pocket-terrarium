/* scene.h - THE AQUARIUM's scene (scene.c), as view.c's descriptor reaches it */
#ifndef AQUARIUM_SCENE_H
#define AQUARIUM_SCENE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "render_core.h"
#include "tank.h"

void aquarium_render(const tank_t *t, uint16_t *fb, int stride);   /* render_tank in the aquarium */
const uint16_t *aquarium_scene_alt(unsigned *epoch);               /* the grass cache's scene (and its epoch), while it is the current one; NULL otherwise */

/* the critters' cards, their arrows and the crab's COLLECT (scene.c: they draw with the scene's art) */
bool aquarium_critter_here(const tank_t *t, int card);
int  aquarium_critter_step(const tank_t *t, int card, int dir);
int  aquarium_critter_nav_hit(int card, float x, float y);
bool aquarium_critter_card(ctx_t *c, const tank_t *t, int card);
bool aquarium_crab_collect_hit(const tank_t *t, float x, float y);
/* the shop's shelf: the living pieces' little tanks */
void aquarium_shelf_prepare(const tank_t *t, uint16_t *fb, int stride);
bool aquarium_shelf_thumb(ctx_t *c, int x, int y, const tank_t *t, int item, int s);
const char *aquarium_shelf_note(const tank_t *t, int item, bool shelved, char *out, size_t n);
void aquarium_shelf_leave(void);

#endif
