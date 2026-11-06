/* scene.h - THE TERRARIUM's scene (scene.c), as view.c's descriptor reaches it */
#ifndef TERRARIUM_SCENE_H
#define TERRARIUM_SCENE_H
#include <stdint.h>
#include "tank.h"

void terrarium_render(const tank_t *t, uint16_t *fb, int stride);   /* render_tank in the terrarium: scene, dish, crickets, decorations, lizards, mister, the film */
void terrarium_scene_reset(uint16_t *scene);                        /* the scene cache is a new one: the two scenes start again */

#endif
