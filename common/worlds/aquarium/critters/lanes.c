/* critters/lanes.c - THE LANES the floor critters walk (tank.h): the snail, the urchin and the crab share them
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

float lane_drop(const tank_t *t, bool front, float x, float half, float floor_y, float now, float dt) {
    float to = (front && !tank_floor_clear(t, x, half)) ? (float)DECOR_FY_FRONT - floor_y : 0;
    now += (to - now) * fminf(1.0f, 6.0f * dt);
    return fabsf(now - to) < 0.05f ? to : now;
}
bool tank_floor_clear(const tank_t *t, float x, float half) {
    static const int P[5] = { 2, 3, 4, 8, 9 };
    for (int k = 0; k < 5; k++) {
        if (!tank_decor_in(t, P[k]) || tank_decor_z(t, P[k]) != DECOR_Z_FRONT) continue;
        if (fabsf(tank_decor_x(t, P[k]) - x) < half + tank_decor_half_w(P[k]) + 2) return false;
    }
    return true;
}
