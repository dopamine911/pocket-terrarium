/* worlds/aquarium/milestones.c - the aquarium's badges (issue #4 phase 3, moved out of progression.c): the names the cards
 * show, what a fish's tick earns (the dart, the bubbles, the reef, the seagrass), and what the pieces and creatures of the
 * tank earn (the school of shrimp, the seahorse's snack, the crab's find). world.c hangs these on the descriptor. */
#include "progression.h"
#include "world_ops.h"

const char *const MS_NAMES[MS_FISH_COUNT] = {
    "arrived", "first meal from you", "first hold-approach", "first dart", "first bubbles",
    "(inspected)", "(retired)", "(retired)", "first follow",
    "reached juv", "reached adult", "reached elder",
    "first rest in the seagrass",
};
const char *const TMS_NAMES[TMS_COUNT] = {
    "a pair", "a trio", "a quartet", "a quintet", "a sextet",
    "first full night's sleep", "first play session", "the tank changed someone", "first feeding",
    "first trimming", "first glass cleaning", "a school of ten shrimp",
    "first visit to the reef",
    "first mutation",                    /* 0.4 (heredity) */
    "first fish sold",
    "first seahorse feeding",            /* 0.3.6 */
    "first sand dollar dug up",          /* 0.3.9: the crab's first find, taken - every find after it pays double */
};

/* firsts the fish chose to do */
void aquarium_scan_fish(const tank_t *t, const fish_t *f, world_scan_t *o) {
    if (f->goal.id == GOAL_DART_PLAY) {
        o->playing = true;
        if (f->goal_age > 1.0f) o->ms |= MS_FIRST_DART;
    }
    if (f->goal.id == GOAL_VISIT_BUBBLES && tank_dist(f->x, f->y, t->bubble_x, t->bubble_y) < 90) o->ms |= MS_FIRST_BUBBLES;
    if (f->goal.id == GOAL_INSPECT_REEF) {               /* its landmark: the reef cluster once there is one */
        float rx, ry; tank_reef_spot(t, &rx, &ry);
        if (tank_dist(f->x, f->y, rx, ry) < 90) {
            o->ms |= MS_INSPECTED;                        /* no badge (0.3.0): it reveals the curiosity slider */
            if (tank_decor_in(t, 4)) o->reef_visit = true;          /* (a cluster on the shelf is no landmark) */
        }
    }
    /* the seagrass: resting inside a canopy, or tucked into one while
       rattled - the grass as the place to be safe (2026-10-02, in the
       badge slot "first reef" had) */
    if (((f->goal.id == GOAL_REST && f->goal_age > 2.0f) || f->stress >= 5.0f || f->breather > 2.0f) && tank_in_grass(t, f->x, f->y))
        o->ms |= MS_FIRST_GRASS;
}

/* the tank's own: its creatures and what they did */
uint32_t aquarium_scan_tank(const tank_t *t) {
    uint32_t b = 0;
    if ((t->sd_unlocks & SD_ITEM_SHRIMP) && t->shrimp_n >= SHRIMP_MAX) b |= TMS_FULL_SCHOOL;
    if ((t->sd_unlocks & SD_ITEM_SEAHORSE) && t->sh_eaten > 0) b |= TMS_SEAHORSE_FED;   /* its first snack (0.3.6) */
    if ((t->sd_unlocks & SD_ITEM_CRAB) && t->cr_found > 0) b |= TMS_CRAB_FIND;         /* the crab's first find, taken (0.3.9) */
    return b;
}
