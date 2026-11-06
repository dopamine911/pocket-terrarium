/* critters/snail.c - the aquarium's snail (SD_ITEM_SNAIL), a creature of the aquarium module (aquarium.c)
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

/* ---- the snail (2026-09-15, the shop's algae control) ----
 * A rule-based creature on the GLASS: it crawls toward the nearest film cell
 * and grazes it thin, cell by cell; with the glass clean it ambles along the
 * edge. The model never sees it (schema v4 is frozen) - it reaches the fish
 * the way the film does, through cover and calm. Its grazing is not the
 * keeper's chore: cells_cleaned and the colonies never count it. */
#define SNAIL_PX_S        4.0f     /* crawl toward a patch (6 was "a bit fast" - Strato) */
#define SNAIL_AMBLE_PX_S  3.0f     /* nothing to eat: the edge walk */
#define SNAIL_CRAB_GAP    34.0f    /* the crab this near ahead on the sand: it turns back (0.3.9) */

#define SNAIL_GRAZE_PER_S 45.0f    /* film units per second on a cell (a fresh 90 cell in 2 s) */
#if defined(TANK_ROUND)
#define SNAIL_SLEEP_FRAC_H 0.26f   /* the bowl films faster (a night alone 0.24, not 0.175): its own
                                    * share keeps the week under ALGAE_DIRTY there too (0.3.6) */
#else
#define SNAIL_SLEEP_FRAC_H 0.20f   /* the night shift (tank_tick_sleep): this share of the film's cells
                                    * an hour - the more film, the less it crawls between meals - so a
                                    * night ends cleaner than it would and a long absence settles under
                                    * the cap instead of on it, and there is still film to wipe (tuned
                                    * in selftest-sleep; 20 cells an hour flat was a spotless glass).
                                    * 0.12 until 0.3.6 (a night 0.12, a week 0.18 of the glass); now a
                                    * night ~0.10 and a week ~0.12, under ALGAE_DIRTY: a snail's tank
                                    * comes back from days away fit to breed, still filmed (Strato) */
#endif
#define SNAIL_MARGIN      30.0f    /* it keeps inside the visible window: the panel's corners are
                                    * rounded and the bezel curve hides the outer ~24 px (Strato,
                                    * 2026-09-15: "stuck in the bottom left corner, I can barely
                                    * see it" - the film seeds from the corners and it went there) */
#define SNAIL_REACH       16.0f    /* it grazes a cell from this close (one cell): a corner cell
                                    * under the bezel is eaten from the visible edge */
/* where the snail may stand: SNAIL_MARGIN inside the rectangle's frame; on the
 * bowl, whose glass hides nothing, 16 px inside the circle */
static void snail_clamp(float *x, float *y, float ymax) {
#if defined(TANK_ROUND) || defined(TANK_CORNER_R)
    tank_glass_clamp(x, y, 16);
    if (*y > ymax) *y = ymax;
#else
    *x = clampf(*x, SNAIL_MARGIN, TANK_W - SNAIL_MARGIN); *y = clampf(*y, SNAIL_MARGIN, ymax);
#endif
}
static int snail_nearest_cell(const tank_t *t) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < ALGAE_CELLS; i++) {
        if (!t->algae[i]) continue;
        float cx = (i % ALGAE_COLS) * ALGAE_CELL + ALGAE_CELL * 0.5f, cy = (i / ALGAE_COLS) * ALGAE_CELL + ALGAE_CELL * 0.5f;
        float d = tank_dist(t->snail_x, t->snail_y, cx, cy);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
bool tank_snail_upright(const tank_t *t) { return t->snail_cell < 0 && t->snail_y >= SNAIL_FLOOR_Y - 1; }
#define SNAIL_TAP_RADIUS 48.0f     /* generous round the 32 px sprite: a fingertip on this 322 ppi
                                    * panel covers ~60 px, and the snail is small and low on the glass
                                    * (Strato, 2026-09-16: "challenging to tap the little guy" at 30;
                                    * the fish take 38 and are tested first) */
bool tank_snail_hit(const tank_t *t, float x, float y) {
    if (!(t->sd_unlocks & SD_ITEM_SNAIL) || t->snail_x < 0) return false;
    return tank_dist(t->snail_x, t->snail_y, x, y) <= SNAIL_TAP_RADIUS;
}
void snail_tick(tank_t *t, float dt) {
    if (!(t->sd_unlocks & SD_ITEM_SNAIL)) return;
    if (t->snail_x < 0) tank_snail_place(t);
    if (t->snail_cell < 0 || !t->algae[t->snail_cell]) { t->snail_cell = (int16_t)snail_nearest_cell(t); t->snail_graze = 0; }
    if (t->snail_cell >= 0) {
        float cx = (t->snail_cell % ALGAE_COLS) * ALGAE_CELL + ALGAE_CELL * 0.5f;
        float cy = (t->snail_cell / ALGAE_COLS) * ALGAE_CELL + ALGAE_CELL * 0.5f;
        float tx = cx, ty = cy; snail_clamp(&tx, &ty, TANK_BOT - SNAIL_MARGIN);
        float d = tank_dist(t->snail_x, t->snail_y, tx, ty);      /* to the nearest point it may stand on */
        if (d > 3 && tank_dist(t->snail_x, t->snail_y, cx, cy) > SNAIL_REACH) {
            t->snail_heading = atan2f(ty - t->snail_y, tx - t->snail_x);
            float step = fminf(d, SNAIL_PX_S * dt);
            t->snail_x += cosf(t->snail_heading) * step; t->snail_y += sinf(t->snail_heading) * step;
        } else {                                            /* grazing: the film thins under it */
            t->snail_graze += dt;
            int v = t->algae[t->snail_cell] - (int)(SNAIL_GRAZE_PER_S * dt + 0.5f);
            t->algae[t->snail_cell] = (uint8_t)(v < 0 ? 0 : v);
            if (v <= 0) t->snail_grazed++;                  /* a cell eaten clean: its card's tally */
        }
    } else if (t->snail_y < SNAIL_FLOOR_Y - 1) {            /* clean glass: down to the floor, flat on the glass, head down.
                                                               A hair off straight down keeps the sideways facing it had
                                                               (cos's sign) for the floor walk it lands in. */
        t->snail_heading = 1.5708f + (cosf(t->snail_heading) < 0 ? 0.001f : -0.001f);
        t->snail_front = true;                              /* off the glass it stands where it was: in front of everything */
        t->snail_y = fminf(SNAIL_FLOOR_Y, t->snail_y + SNAIL_PX_S * dt);
    } else {                                                /* the floor walk, upright: along the bottom, a rest now and
                                                               then, a turn at each end (the facing lives in snail_heading) */
        t->snail_dy = lane_drop(t, t->snail_front, t->snail_x, 14, SNAIL_FLOOR_Y + SNAIL_SOLE_DY_T, t->snail_dy, dt);
        t->snail_y = SNAIL_FLOOR_Y + t->snail_dy;
        bool resting = fmod(t->clock, 24.0) < 5.0;
        if (!resting) {
            float dir = cosf(t->snail_heading) < 0 ? -1.0f : 1.0f, cx;
            if (crab_floor_x(t, &cx) && (cx - t->snail_x) * dir > 0 && fabsf(cx - t->snail_x) < SNAIL_CRAB_GAP) {   /* the crab just ahead: it turns back (Strato: they should notice each other) */
                t->snail_heading = dir > 0 ? 3.14159f : 0; dir = -dir;
            }
            t->snail_x += dir * SNAIL_AMBLE_PX_S * dt;
            bool turned = false;
            if (t->snail_x <= TANK_FX0 + SNAIL_MARGIN) { t->snail_x = TANK_FX0 + SNAIL_MARGIN; t->snail_heading = 0; turned = true; }
            if (t->snail_x >= TANK_FX1 - SNAIL_MARGIN) { t->snail_x = TANK_FX1 - SNAIL_MARGIN; t->snail_heading = 3.14159f; turned = true; }
            /* at each end it picks its lane for the walk back: in front of the IN FRONT pieces, or behind
               them (from the clock's bits, not the tank's RNG: a seeded run is the same run) */
            if (turned && tank_floor_clear(t, t->snail_x, 21)) t->snail_front = ((((uint32_t)(uint64_t)(t->clock * 997.0) * 2654435761u) >> 20) % 10) < 6;   /* 60/40 in front (LANE_FRONT_ODDS), and only in the clear - never a pop (10-24) */
            if (turned) t->snail_gdepth_want = critter_depth_roll(t, t->snail_x, 0x3c1u);   /* SWIM DEPTH: and its depth against the grass for the walk */
        }
    }
    snail_clamp(&t->snail_x, &t->snail_y, SNAIL_FLOOR_Y + t->snail_dy);
}
/* asleep: the snail keeps working, coarsely - the nearest cells go, one by
 * one. tank_tick_sleep hands it the night a slice at a time, AFTER the
 * slice's film has grown, so it eats the night's film as it comes; the
 * part-cell carries over, so a nap's few minutes count too (2026-10-02: it
 * used to run once, before the night's film, on the film the keeper had
 * already left - from a clean glass a snail's tank woke exactly as dirty as
 * one without, and (int) of a short nap was no cells at all). */
void snail_sleep(tank_t *t, float seconds) {
    if (!(t->sd_unlocks & SD_ITEM_SNAIL)) return;
    if (t->snail_x < 0) tank_snail_place(t);
    int covered = 0;
    for (int i = 0; i < ALGAE_CELLS; i++) covered += t->algae[i] > 0;
    t->snail_sleep_acc += covered * (1.0f - expf(-SNAIL_SLEEP_FRAC_H * seconds / 3600.0f));
    int cells = (int)t->snail_sleep_acc;
    t->snail_sleep_acc -= cells;
    for (int k = 0; k < cells; k++) {
        int c = snail_nearest_cell(t);
        if (c < 0) break;
        t->algae[c] = 0; t->snail_grazed++;
        t->snail_x = (c % ALGAE_COLS) * ALGAE_CELL + ALGAE_CELL * 0.5f;
        t->snail_y = (c / ALGAE_COLS) * ALGAE_CELL + ALGAE_CELL * 0.5f;
        snail_clamp(&t->snail_x, &t->snail_y, TANK_BOT - SNAIL_MARGIN);
    }
    t->snail_cell = -1;
}
void tank_snail_place(tank_t *t) {
    t->snail_x = TANK_FX0 + SNAIL_MARGIN + 10; t->snail_y = SNAIL_FLOOR_Y;   /* on the floor, bottom left, facing right */
    t->snail_heading = 0; t->snail_cell = -1; t->snail_graze = 0; t->snail_front = true; t->snail_dy = 0;
}
