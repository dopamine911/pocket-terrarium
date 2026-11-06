/* worlds/aquarium/fry.c - how the aquarium's fry come (issue #4 phase 3, moved out of progression.c): the care gates of its
 * population, the words behind its HOW? button, the nursery grass a fry is born in, and where it is set down. The gate
 * loop, the checklist's phrasing and the arrival itself are progression.c's. */
#include "progression.h"
#include "world_ops.h"
#include <string.h>
#include <stdio.h>

/* the MEALS gate after a sale: the table's count, or the meals owed since the fish was sold, whichever is further off */
#define MEALS(tbl) ((float)((tbl) > c->sale_need ? (tbl) : c->sale_need))
#define GATE(k, h, nd, m) do { g[n].kind = (k); g[n].have = (h); g[n].need = (nd); g[n].met = (m); \
        g[n].frac = g[n].met ? 1.0f : (nd) > 0 ? (h) / (nd) : 0; if (g[n].frac > 1) g[n].frac = 1; n++; } while (0)
int aquarium_care_gates(const tank_t *t, const care_ctx_t *c, care_gate_t g[CARE_GATES_MAX]) {
    const float min_trust = c->min_trust, age = c->age;
    const int last = t->n_fish - 1;
    int n = 0;
    switch (t->n_fish) {
    case 2:
        GATE(FRY_REQ_TRUST, min_trust, 6.0f, min_trust >= 6.0f);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(12), t->player_feedings >= MEALS(12));
        GATE(FRY_REQ_HOLD, (float)t->hold_approaches, 1, t->hold_approaches >= 1);
        break;
    case 3:
        /* 2026-09-15 (Strato): MEALS and CHANGE were one "40 meals OR someone
         * drifted 0.11" row - and the parents had always drifted long before,
         * so it was dead weight. Now three rows, and CHANGE is the YOUNGEST
         * fish's accumulated drift pressure (tank.h drift_acc): the same
         * ~26 lit minutes fed and calm, and impossible to soft-lock for a fry
         * born on a trait clamp (inheritance puts them there). */
        GATE(FRY_REQ_GROW, age, progression_stage_age(t, STAGE_JUV), t->fish[last].stage >= STAGE_JUV);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(40), t->player_feedings >= MEALS(40));
        GATE(FRY_REQ_CHANGE, t->fish[last].drift_acc, DRIFT_CHANGE, t->fish[last].drift_acc >= DRIFT_CHANGE);
        break;
    case 4:
        GATE(FRY_REQ_GROW, age, progression_stage_age(t, STAGE_ADULT), t->fish[last].stage >= STAGE_ADULT);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(80), t->player_feedings >= MEALS(80));
        GATE(FRY_REQ_TRUST, min_trust, 7.0f, min_trust >= 7.0f);
        break;
    default:
        GATE(FRY_REQ_GROW, age, progression_stage_age(t, STAGE_ADULT), t->fish[last].stage >= STAGE_ADULT);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(140), t->player_feedings >= MEALS(140));
        GATE(FRY_REQ_TRUST, min_trust, 8.0f, min_trust >= 8.0f);
        break;
    }
    /* and a clean tank (Strato, 2026-09-16: "fish should not be able to
     * breed in a dirty tank. Some algae is OK but if a certain percentage of
     * glass crosses a threshold it will prevent new fries from spawning
     * unless cleaned"): film on no more than ALGAE_DIRTY of the glass. A
     * care gate like the others, so the parents court while only the glass
     * holds them back, and the checklist lists it. The bar reads the other
     * way from the rest - empty at the growth cap, full at the threshold -
     * so it fills as the keeper wipes. Note the staged fry is NOT held by
     * the glass at the light-on that brings it: a night's sleep films ~25%
     * of the glass, so a fry conceived clean would otherwise never land on
     * a morning (do_arrival waits for the nursery only). */
    {
        float cover = tank_algae_cover(t);
        GATE(FRY_REQ_GLASS, cover, ALGAE_DIRTY, cover <= ALGAE_DIRTY);
        if (!g[n - 1].met) {
            float frac = 1.0f - (cover - ALGAE_DIRTY) / ALGAE_DIRTY;
            g[n - 1].frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        }
    }
    return n;
}
#undef GATE
#undef MEALS

/* how each gate is moved - the words behind the HOW? button */
static const char *const TIP_TRUST[]  = { "REST A FINGER ON THE GLASS", "AND KEEP IT STILL. EVERY", "FISH EARNS TRUST WHILE IT", "RESTS THERE. 3 QUICK TAPS", "SCARE THEM AND COST TRUST.", NULL };
static const char *const TIP_FEED[]   = { "TAP THE WATER AT THE TOP", "OF THE TANK TO DROP FOOD.", "A FEEDING COUNTS AS A MEAL", "ONCE A FISH EATS FROM IT.", NULL };
static const char *const TIP_HOLD[]   = { "REST A FINGER ON THE GLASS", "FOR A FEW SECONDS. A FISH", "THAT TRUSTS YOU SWIMS OVER", "AND STAYS. FEED FIRST: A", "HUNGRY FISH WON'T COME.", NULL };
static const char *const TIP_GROW[]   = { "FISH GROW WITH TIME,", "SLOWER WHEN THE TANK IS", "IN SLEEP MODE.", NULL };
static const char *const TIP_CHANGE[] = { "FISH PERSONALITIES WILL", "NATURALLY DRIFT AS THEY", "INTERACT WITH THE WORLD.", NULL };   /* Strato: intentionally vague */
static const char *const TIP_GRASS[]  = { "GRASS REGROWS ON ITS OWN,", "FASTEST WHILE THE TANK", "SLEEPS.", NULL };
static const char *const TIP_GLASS[]  = { "DRAG A FINGER ACROSS THE", "GLASS TO WIPE IT CLEAN.", "UNLOCKABLE CRITTERS CAN", "HELP KEEP IT CLEAN.", NULL };   /* Strato's words */
const char *const *aquarium_fry_tip(int kind) {
    switch (kind) {
    case FRY_REQ_TRUST:  return TIP_TRUST;
    case FRY_REQ_FEED:   return TIP_FEED;
    case FRY_REQ_HOLD:   return TIP_HOLD;
    case FRY_REQ_GROW:   return TIP_GROW;
    case FRY_REQ_CHANGE: return TIP_CHANGE;
    case FRY_REQ_GLASS:  return TIP_GLASS;
    default:             return TIP_GRASS;
    }
}

/* and the nursery: every fry needs grass to be born in */
int aquarium_fry_extra(const tank_t *t, fry_req_t *r) {
    memset(r, 0, sizeof *r);
    float best = 0;
    for (int b = 0; b < VEG_BEDS; b++) if (t->veg_growth[b] > best) best = t->veg_growth[b];
    r->kind = FRY_REQ_GRASS; r->met = tank_nursery_bed(t) >= 0;
    r->frac = r->met ? 1.0f : best / VEG_NURSERY;
    snprintf(r->title, sizeof r->title, "GRASS");
    snprintf(r->words, sizeof r->words, "ONE GRASS BED MUST GROW");
    snprintf(r->words2, sizeof r->words2, "TALL ENOUGH TO HIDE IN");
    if (r->met) snprintf(r->progress, sizeof r->progress, "A NURSERY BED IS READY");
    else snprintf(r->progress, sizeof r->progress, "TALLEST BED %d%% THERE", (int)(r->frac * 100 + 0.5f));
    return 1;
}

/* no grass to be born in: no arrival and no courtship */
bool aquarium_nursery_ready(const tank_t *t) { return tank_nursery_bed(t) >= 0; }

/* the glass clean (ALGAE_DIRTY) and a nursery bed: the pair may court */
bool aquarium_court_ok(const tank_t *t) { return tank_algae_cover(t) <= ALGAE_DIRTY && tank_nursery_bed(t) >= 0; }

/* born in the grass it was courted in */
void aquarium_arrival_place(tank_t *t, int slot) {
    int nb = tank_nursery_bed(t);
    if (nb >= 0) {
        float x0, x1; tank_veg_bed(t, nb, &x0, &x1, NULL, NULL);
        t->fish[slot].x = (x0 + x1) * 0.5f; t->fish[slot].y = TANK_BOT - 16 - 18;
    }
}
