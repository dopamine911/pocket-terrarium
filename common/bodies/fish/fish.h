/* fish.h - the fish body's drawing (fish.c), as the aquarium's scene and the pages reach it */
#ifndef FISH_BODY_H
#define FISH_BODY_H
#include "render_core.h"

void draw_fish(ctx_t *c, const tank_t *t, const fish_t *f);      /* the tank's fish: its window ends at the sand line */
void draw_fish_core(ctx_t *c, const fish_t *f, float beat, bool asleep, float mark, float floor_y);   /* the body, fins, markings and eye */
void fish_box(int shape, int fins, stage_t stage, float b[4]);    /* the still pose's box of a shape, a fin set and a stage */
void fish_fit(fish_t *f, float x, float y, float size, float room);   /* a page draws a fish in the room a classic fish of its stage takes */

#endif
