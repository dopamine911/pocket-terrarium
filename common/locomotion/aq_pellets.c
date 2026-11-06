/* aq_pellets.c - the aquarium's food: the pellets
 * (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/aquarium/aquarium_priv.h"
#include "pt_util.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tank_nearest_food(const tank_t *t, const fish_t *f, float *dist_out) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < MAX_FOOD; i++) {
        if (!t->food[i].alive) continue;
        float d = tank_dist(f->x, f->y, t->food[i].x, t->food[i].y);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist_out) *dist_out = bd;
    return best;
}
