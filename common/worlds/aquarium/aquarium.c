/* aquarium.c - the aquarium as a world module (mechanics.h mech_aquarium): its creatures' frames and nights, the fright a knock
 * on the glass gives them, and the trust a hitched seahorse passes on. tank.c calls these at its fixed points for every module
 * present, in the order the tick and the night always ran them (issue #4 phase 3; the creatures themselves: critters/*.c) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/aquarium/aquarium_priv.h"
#include "pt_util.h"
#include <math.h>

#define SEAHORSE_TRUST_X   2.0f   /* a resting finger's trust is earned this many times as fast while the seahorse hangs on it (0.3.6: "twice as fast" on its shop row) */

/* tank_tick, after the upkeep: the school's tap timer, then each creature its frame, then the floor critters take up their depth */
static void aq_tick(tank_t *t, float dt) {
    t->shrimp_tap_t += dt;
    snail_tick(t, dt);
    urchin_tick(t, dt);
    crab_tick(t, dt);
    seahorse_tick(t, dt);
    shrimp_tick(t, dt);
    {   /* SWIM DEPTH phase 3c: the floor critters take up their depth against the grass clear of every bed (as the lanes: never a pop) */
        if (t->snail_gdepth != t->snail_gdepth_want && box_clear_beds(t, t->snail_x - 22, t->snail_x + 22, TANK_H)) t->snail_gdepth = t->snail_gdepth_want;
        if (t->urchin_gdepth != t->urchin_gdepth_want && box_clear_beds(t, t->urchin_x - 20, t->urchin_x + 20, TANK_H)) t->urchin_gdepth = t->urchin_gdepth_want;
        if (t->cr_gdepth != t->cr_gdepth_want && box_clear_beds(t, t->cr_x - 26, t->cr_x + 26, TANK_H)) t->cr_gdepth = t->cr_gdepth_want;
    }
}

/* tank_tick_sleep: the school's night, the seahorse's days with the keeper, the crab's hunger and its find - before the slices ... */
static void aq_sleep(tank_t *t, float seconds) {
    shrimp_sleep(t, seconds);
    if (t->sd_unlocks & SD_ITEM_SEAHORSE) { t->sh_s += seconds; t->sh_snack_cool = fmaxf(0, t->sh_snack_cool - seconds); }   /* days with the keeper run through the night */
    if (t->sd_unlocks & SD_ITEM_CRAB) t->cr_full = fmaxf(0, t->cr_full - seconds);   /* hungry again by the morning */
    if (t->sd_unlocks & SD_ITEM_CRAB) {                 /* the crab's next find: its wait runs asleep too; one held up when the tank slept is due again at the wake */
        if (t->cr_find > 0) { t->cr_find = 0; t->cr_find_cool = 0; }
        else t->cr_find_cool = fmaxf(0, t->cr_find_cool - seconds);
    }
}
/* ... and after each slice's growth: the snail and the urchin eat into what grew */
static void aq_sleep_slice(tank_t *t, float dt) {
    snail_sleep(t, dt);
    urchin_sleep(t, dt);
}

/* the keeper's rap or a spooked tank: the shrimp scatter - from the water or from the school alike */
static void aq_scare(tank_t *t, float x, float y, float r) { shrimp_scatter(t, x, y, r); }
/* a knock anywhere on the glass (the triple tap): the crab runs for cover whatever the distance (0.3.9) */
static void aq_knock(tank_t *t, float x) { tank_crab_scare(t, x); }

/* a resting finger: a seahorse hitched to it doubles the trust every animal earns (tank.c touch_tick); what it added over the base rate is its tally */
static float aq_trust_x(const tank_t *t) { return t->sh_hitched ? SEAHORSE_TRUST_X : 1.0f; }
static void aq_trust_credit(tank_t *t, float was, float now, float dt) {
    if (t->sh_hitched) t->sh_trust_passed += fmaxf(0, (now - was) - fminf(10 - was, dt * TRUST_PER_S));
}

const mech_ops_t mech_aquarium = {
    .name = "aquarium", .serves = CAP_WATER,
    .tick = aq_tick, .sleep = aq_sleep, .sleep_slice = aq_sleep_slice, .scare = aq_scare, .knock = aq_knock,
    .trust_x = aq_trust_x, .trust_credit = aq_trust_credit,
};
