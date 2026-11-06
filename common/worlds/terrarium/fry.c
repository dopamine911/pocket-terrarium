/* worlds/terrarium/fry.c - how the terrarium's hatchlings come (pocket-terrarium P5, issue #4 phase 3: moved out of
 * progression.c): the care gates, the words behind its HOW? button, and where a hatchling is set down - the humid hide
 * (THE HATCHING). The gate loop, the checklist's phrasing and the arrival itself are progression.c's. */
#include "progression.h"
#include "world_ops.h"
#include <string.h>

/* the keeper's care of the lizards - every one's trust, meals from the keeper's hand (crickets dropped, worms from the dish the
   keeper filled: player_feedings) and sheds come off clean, the lizards' together (fish_t.shed_clean: a stuck one the keeper
   helped off counts) - and from the third on the youngest grown to an adult. No glass and no grass: the egg is laid in the humid
   hide (THE HATCHING) */
#define MEALS(tbl) ((float)((tbl) > c->sale_need ? (tbl) : c->sale_need))
#define GATE(k, h, nd, m) do { g[n].kind = (k); g[n].have = (h); g[n].need = (nd); g[n].met = (m); \
        g[n].frac = g[n].met ? 1.0f : (nd) > 0 ? (h) / (nd) : 0; if (g[n].frac > 1) g[n].frac = 1; n++; } while (0)
int terrarium_care_gates(const tank_t *t, const care_ctx_t *c, care_gate_t g[CARE_GATES_MAX]) {
    const float min_trust = c->min_trust, age = c->age;
    const int last = t->n_fish - 1;
    int n = 0;
    int sheds = 0;
    for (int i = 0; i < t->n_fish; i++) if (!t->fish[i].away) sheds += t->fish[i].shed_clean;
    switch (t->n_fish) {
    case 2:
        GATE(FRY_REQ_TRUST, min_trust, 6.0f, min_trust >= 6.0f);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(12), t->player_feedings >= MEALS(12));
        GATE(FRY_REQ_SHED, (float)sheds, 4, sheds >= 4);
        break;
    case 3:
        GATE(FRY_REQ_GROW, age, progression_stage_age(t, STAGE_ADULT), t->fish[last].stage >= STAGE_ADULT);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(40), t->player_feedings >= MEALS(40));
        GATE(FRY_REQ_SHED, (float)sheds, 8, sheds >= 8);
        break;
    default:
        GATE(FRY_REQ_GROW, age, progression_stage_age(t, STAGE_ADULT), t->fish[last].stage >= STAGE_ADULT);
        GATE(FRY_REQ_FEED, (float)t->player_feedings, MEALS(80), t->player_feedings >= MEALS(80));
        GATE(FRY_REQ_TRUST, min_trust, 7.0f, min_trust >= 7.0f);
        GATE(FRY_REQ_SHED, (float)sheds, 12, sheds >= 12);
        break;
    }
    return n;
}
#undef GATE
#undef MEALS

/* how each gate is moved - the words behind the HOW? button (the terrarium's own: its tools and its lizards) */
static const char *const TIP_TRUST[]  = { "REST A FINGER ON THE GLASS", "AND KEEP IT STILL. EVERY", "LIZARD EARNS TRUST WHILE", "YOUR HAND RESTS THERE.", "3 QUICK TAPS SCARE THEM.", NULL };
static const char *const TIP_FEED[]   = { "TAP THE GLASS TO DROP", "CRICKETS. A FEEDING IS A", "MEAL ONCE A LIZARD CATCHES", "ONE. TAP THE DISH TO FILL", "IT WITH MEALWORMS.", NULL };
static const char *const TIP_HOLD[]   = { "REST A FINGER ON THE GLASS", "FOR A FEW SECONDS. A", "LIZARD THAT TRUSTS YOU", "WALKS OVER AND STAYS.", NULL };
static const char *const TIP_GROW[]   = { "LIZARDS GROW WITH TIME,", "BUT ONLY AT A CLEAN SHED.", "SLOWER WHILE THEY SLEEP.", NULL };
static const char *const TIP_CHANGE[] = { "LIZARD PERSONALITIES WILL", "NATURALLY DRIFT AS THEY", "INTERACT WITH THE WORLD.", NULL };
static const char *const TIP_GLASS[]  = { "DRAG A FINGER ACROSS THE", "GLASS TO WIPE IT CLEAN.", "UNLOCKABLE CRITTERS CAN", "HELP KEEP IT CLEAN.", NULL };   /* Strato's words */
static const char *const TIP_SHED[]   = { "KEEP THE HUMID HIDE DAMP:", "MIST IT WITH THE MISTER.", "A LIZARD THAT SHEDS IN A", "DAMP HIDE SHEDS CLEAN.", NULL };   /* (the terrarium's, P5) */
const char *const *terrarium_fry_tip(int kind) {
    switch (kind) {
    case FRY_REQ_TRUST:  return TIP_TRUST;
    case FRY_REQ_FEED:   return TIP_FEED;
    case FRY_REQ_HOLD:   return TIP_HOLD;
    case FRY_REQ_GROW:   return TIP_GROW;
    case FRY_REQ_CHANGE: return TIP_CHANGE;
    case FRY_REQ_SHED:   return TIP_SHED;
    default:             return TIP_GLASS;   /* (no gate of the terrarium's: the glass and the grass are not its) */
    }
}

/* no nursery to wait for: the egg is laid in the humid hide, always there */
int terrarium_fry_extra(const tank_t *t, fry_req_t *out) { (void)t; (void)out; return 0; }
bool terrarium_nursery_ready(const tank_t *t) { (void)t; return true; }
/* the glass clean (ALGAE_DIRTY): a pair may court; the hide is always there */
bool terrarium_court_ok(const tank_t *t) { return tank_algae_cover(t) <= ALGAE_DIRTY; }

/* THE HATCHING (P5): in the humid hide, where the egg was */
void terrarium_arrival_place(tank_t *t, int slot) { tank_hatch_place(t, slot); }
