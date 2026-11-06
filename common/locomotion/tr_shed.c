/* tr_shed.c - THE SHED and the mister
 * (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/terrarium/terrarium_priv.h"
#include "pt_util.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool terr_in_damp_hide(const tank_t *t, int i) {
    int k = t->fish[i].hide;                                  /* in one (THE WALK: it went in), not passing its footprint */
    return k >= 0 && k < tank_habitat(t)->n_hides && tank_habitat(t)->hides[k].kind == HIDE_HUMID && t->moist[k] >= SHED_DAMP;
}
bool terr_shed_growth(tank_t *t, int i) {
    fish_t *f = &t->fish[i];
    if (!tank_species(t)->sheds) return false;        /* (a pack that does not shed: progression puts the age's stage on at once) */
    if (f->shed == SHED_CLEAN) return false;          /* a shed just off: progression starts the next once it is finished */
    if (f->shed == SHED_NORMAL) { f->shed = SHED_SOON; f->shed_s = tank_species(t)->shed_soon_s; f->shed_damp = 0; }   /* at once, whatever the timer said */
    f->shed_growth = true;                            /* (soon, shedding or stuck: the shed under way is the growth shed) */
    return true;
}
/* one animal's shed through dt tended seconds: the timers, and the damp time while it is in a humid hide that is still
 * damp - the first damp_until seconds of dt (the hide dries as dt goes; < 0: not damp, or not in one). Off clean = CLEAN,
 * the moment the soak is done; the rest of dt waits for progression to finish the shed (the timer restarts there) */
static void shed_step(const species_t *p, fish_t *f, float dt, float damp_until) {
    float at = 0;                                     /* how much of dt is lived */
    for (int guard = 0; at < dt && guard < 8; guard++) {
        float left = dt - at;
        switch (f->shed) {
        case SHED_NORMAL: case SHED_SOON:
            if (f->shed_s > left) { f->shed_s -= left; at = dt; break; }
            at += fmaxf(0, f->shed_s);
            if (f->shed == SHED_NORMAL) { f->shed = SHED_SOON; f->shed_s = p->shed_soon_s; }
            else { f->shed = SHED_SHEDDING; f->shed_s = p->shed_window_s; f->shed_damp = 0; }
            break;
        case SHED_SHEDDING: case SHED_STUCK: {
            bool stuck = f->shed == SHED_STUCK;
            float step = stuck ? left : fminf(left, fmaxf(0, f->shed_s));
            float damp = fmaxf(0, fminf(at + step, damp_until) - at);
            if (damp > 0 && f->shed_damp + damp >= SHED_SOAK_S) {   /* enough damp time: off clean */
                float need = SHED_SOAK_S - f->shed_damp;
                f->shed_s += stuck ? need : -need;
                f->shed_damp = SHED_SOAK_S; f->shed = SHED_CLEAN; at = dt; f->shed_helped = stuck;   /* (a stuck one's stay: helped off) */
                break;
            }
            f->shed_damp += damp;
            if (stuck) { f->shed_s += step; at = dt; break; }
            f->shed_s -= step; at += step;
            if (f->shed_s <= 0) { f->shed = SHED_STUCK; f->shed_s = 0; f->shed_damp = 0; }   /* the window ran out: it sticks */
            break; }
        default: at = dt; break;                      /* CLEAN: a moment (progression) */
        }
    }
}
void terr_shed_clock(tank_t *t, float dt) {
    const species_t *p = tank_species(t);
    if (dt <= 0 || !p->sheds) return;                 /* (the aquarium's fish do not shed, and its water keeps no hide damp) */
    const habitat_t *h = tank_habitat(t);
    const float dry = h->moist_dry_per_s * ((t->sd_unlocks & SD_ITEM_MOSS) ? SD_MOSS_DRY_X : 1.0f);   /* (the shop's moss, P5: slower) */
    float damp_until[HABITAT_HIDES_MAX];              /* the part of dt each humid hide stays damp (-1: none) */
    for (int k = 0; k < h->n_hides; k++) {
        float m0 = t->moist[k];
        damp_until[k] = h->hides[k].kind == HIDE_HUMID && m0 >= SHED_DAMP ? (m0 - SHED_DAMP) / dry : -1;
        if (h->hides[k].kind == HIDE_HUMID) t->moist[k] = fmaxf(0, m0 - dry * dt);
    }
    for (int i = 0; i < t->n_fish; i++) {
        fish_t *f = &t->fish[i];
        if (f->away) continue;                        /* (next door: its shed waits with its age) */
        int k = f->hide;                              /* the hide it is IN (THE WALK), not one it passes */
        shed_step(p, f, dt, k >= 0 && k < h->n_hides ? damp_until[k] : -1);
    }
}
void terr_mist_pass(tank_t *t, int k) {
    if (k >= 0 && k < HABITAT_HIDES_MAX && tank_species(t)->sheds) t->moist[k] = fminf(1.0f, t->moist[k] + MIST_PASS);
}
bool terr_mist_soak(tank_t *t, int i) {
    fish_t *f = &t->fish[i];
    f->stress = clampf(f->stress + 1.0f, 0, 10);     /* the spray: they dislike it, soaked or not */
    if (f->shed != SHED_STUCK) return false;
    f->shed = SHED_CLEAN; f->shed_helped = true;      /* the soak: it comes off at once (progression finishes it) - helped off */
    if (f->trust >= 6.0f) f->trust = fminf(10, f->trust + 0.2f);   /* helped by a hand it knows */
    return true;
}
