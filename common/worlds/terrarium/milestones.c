/* worlds/terrarium/milestones.c - the terrarium's badges (pocket-terrarium P5, issue #4 phase 3: moved out of progression.c):
 * the names the cards show, and what a lizard's tick earns. A lizard's row is MEAL_FROM_YOU, HOLD_APPROACH, CATCH, BUBBLES
 * (the bask), SHED, DART (the dig); GRASS and FOLLOW are no badge here. Its catch and its shed are set where they happen
 * (tank.c lizard_catch, shed_done). world.c hangs these on the descriptor. */
#include "progression.h"
#include "world_ops.h"

/* each <= 28 chars (a notice line) */
const char *const MS_NAMES[MS_FISH_COUNT] = {
    "arrived", "first meal from you", "came to your hand", "first dig", "first bask",
    "(inspected)", "(retired)", "(retired)", "(investigated)",
    "reached juv", "reached adult", "reached elder",
    "(hid)", "(crossbred)",
    "first cricket caught", "first clean shed",
};
const char *const TMS_NAMES[TMS_COUNT] = {
    "a pair", "a trio", "a quartet", "a quintet", "a sextet",
    "first quiet night", "first dig together", "a personality changed", "first feeding",
    "first trimming", "first glass cleaning", "a school of ten shrimp",
    "first visit to the reef",
    "first new morph",
    "first lizard sold",
    "first seahorse feeding",
    "first sand dollar dug up",
    "first stuck shed fixed", "three clean sheds in a row", "first adult",
};

/* a dig is the dart's bit - digging, not on its way to the spot - and a bask on the spot the bubbles'; an investigation the
   curiosity's and the follow's (no badge there: the card's sliders) */
void terrarium_scan_fish(const tank_t *t, const fish_t *f, world_scan_t *o) {
    if (f->pose == LZ_POSE_DIG) { o->playing = true; o->ms |= MS_FIRST_DART; }
    float bx, by; tank_bask_point(t, &bx, &by);
    if (f->goal.id == GOAL_LZ_BASK && tank_dist(f->x, f->y, bx, by) < 20) o->ms |= MS_FIRST_BUBBLES;   /* on the spot (up on its rock) */
    if (f->goal.id == GOAL_LZ_INVESTIGATE && f->goal_age > 2.0f) o->ms |= MS_INSPECTED;
    if (f->stage >= STAGE_ADULT) o->tms |= TMS_FIRST_ADULT;   /* (P5: grown up at a clean growth shed) */
}

/* the tank's creatures earn no badge of their own here */
uint32_t terrarium_scan_tank(const tank_t *t) { (void)t; return 0; }
