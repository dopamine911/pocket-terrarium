/* worlds/terrarium/rules.c - the lizard's reflex rules (P2b: model/schema-lizard.md "Behaviour the teacher must express", as
 * plain rules), the terrarium's advisor when the model is not asked (issue #4 phase 3: moved out of advisor.c; world.c hangs
 * it on the descriptor). */
#include "advisor.h"
#include "world_ops.h"

/* the lizard's rules (the schema's nine, in its order of precedence; each a law here where the model's are tendencies):
 * starving with prey about it hunts - not too cold to strike; overheating hides (the walk picks the cool hide); its
 * sleep phase hides (the warm one, or the humid one in a shed); a shed coming or on hides (the humid one) unless hungry;
 * cold basks; rattled hides; spent rests (or basks, cold too); hungry hunts; the settle phase finishes business and
 * goes in; a cool one basks now and then, and a bask goes on until it is warm; bored moves on to something new;
 * curious explores, or investigates the other if it is sociable; then its temper - the lazy rest, the bold dig - and
 * a long-held pastime gives way to a wander. A fresh decision stands 2.5 s, but never over overheating, a shed or the
 * night. */
goal_t terrarium_rules(const tank_t *t, int i, bool request) {
    const fish_t *f = &t->fish[i];
    if (!request) return f->goal;
    goal_t g = f->goal;
    g.confidence = 1.0f; g.runner_up = GOAL_COUNT;
    const float r = advisor_frand(t, i);
    const int phase = tank_activity_phase(t);
    float prey_d; const bool prey = tank_food_sight(t, i, NULL, NULL, &prey_d);   /* (its prey: a cricket about, the dish - tank.h THE CRICKETS) */
    float other_d; const bool other = tank_nearest_friend(t, i, &other_d) >= 0;
    const bool shed = f->shed == SHED_SOON || f->shed == SHED_SHEDDING || f->shed == SHED_STUCK;
    const bool starving = f->hunger >= 8.5f && prey;
    const goal_id_t cur = f->goal.id;
    const bool urgent = f->heat >= HEAT_HOT || starving || (shed && cur != GOAL_LZ_HIDE) || (phase == PHASE_SLEEP && cur != GOAL_LZ_HIDE);
    if (f->goal_age < 2.5f && !urgent) return g;
#define SET(goal, u) do { g.id = (goal); g.urgency = (u); return g; } while (0)
    if (starving && f->heat >= 2) SET(GOAL_LZ_HUNT, 9);
    if (f->heat >= HEAT_HOT) SET(GOAL_LZ_HIDE, 8);
    if (cur == GOAL_LZ_BASK && f->heat >= 6.5f && phase == PHASE_ACTIVE) SET(GOAL_LZ_EXPLORE, 3);   /* warm: off the spot before it overheats */
    if (phase == PHASE_SLEEP) SET(GOAL_LZ_HIDE, 3);
    if (shed && f->hunger < 7) SET(GOAL_LZ_HIDE, 6);
    if (f->heat < 3) SET(GOAL_LZ_BASK, 7);
    if (f->stress > 7) SET(GOAL_LZ_HIDE, 6);
    if (f->energy < 2) SET(f->heat < 4 ? GOAL_LZ_BASK : GOAL_LZ_REST, 5);
    if (f->hunger > 6 && prey) SET(GOAL_LZ_HUNT, f->hunger > 8 ? 8 : 6);
    if (phase == PHASE_SETTLE) SET(GOAL_LZ_HIDE, 3);
    if (cur == GOAL_LZ_BASK && f->heat < 6.5f && f->bored < 7) SET(GOAL_LZ_BASK, 4);   /* warming up: until it is warm */
    if (f->heat < 4.5f && r < 0.5f) SET(GOAL_LZ_BASK, 4);
    if (f->bored > 7) {                                   /* stale: something new */
        const goal_id_t a = r < 0.7f ? GOAL_LZ_EXPLORE : GOAL_LZ_DIG, b = a == GOAL_LZ_EXPLORE ? GOAL_LZ_DIG : GOAL_LZ_EXPLORE;
        SET(a != cur ? a : b, 4);
    }
    if (f->curiosity > 6 && r < 0.5f) SET(other && f->sociable > 0.6f && r < 0.25f ? GOAL_LZ_INVESTIGATE : GOAL_LZ_EXPLORE, 3);
    if (f->lazy > 0.55f && f->hunger < 5 && r < 0.3f) SET(GOAL_LZ_REST, 3);
    if (f->bold > 0.7f && f->energy > 6 && r < 0.15f) SET(GOAL_LZ_DIG, 4);
    if (cur == GOAL_LZ_HIDE && !shed && f->heat < 7 && phase == PHASE_ACTIVE && f->goal_age > 20 + r * 20) SET(GOAL_LZ_EXPLORE, 3);   /* out again by day */
    if (f->goal_age > 8.0f + r * 10.0f)
        SET(r < 0.45f ? GOAL_LZ_EXPLORE : r < 0.6f && other ? GOAL_LZ_INVESTIGATE : r < 0.8f ? GOAL_LZ_REST : GOAL_LZ_DIG, 3);
#undef SET
    if (!goal_in_world(g.id)) g.id = GOAL_LZ_EXPLORE;    /* (a goal of the other world: never this one's) */
    return g;
}
