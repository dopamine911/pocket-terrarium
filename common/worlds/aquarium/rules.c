/* worlds/aquarium/rules.c - the fish's reflex rules (prototype chooseCompanionGoal), the aquarium's advisor when the model is
 * not asked (issue #4 phase 3: moved out of advisor.c; world.c hangs it on the descriptor). */
#include "advisor.h"
#include "world_ops.h"

goal_t aquarium_rules(const tank_t *t, int fish_idx, bool request) {
    const fish_t *f = &t->fish[fish_idx];
    if (!request) return f->goal;            /* rules are synchronous: polls no-op */
    float food_d;
    int food_i = tank_nearest_food(t, f, &food_d);
    float friend_d;
    tank_nearest_friend(t, fish_idx, &friend_d);
    float r = advisor_frand(t, fish_idx);

    goal_t g = f->goal;                      /* default: keep current */
    g.confidence = 1.0f; g.runner_up = GOAL_COUNT;   /* rules are never torn */
    if (f->goal_age < 2.5f && f->goal.id != GOAL_FLEE_SHADOW) return g;

    if (f->energy < 2.5f || f->stress > 7.5f ||
               (f->lazy > 0.55f && f->hunger < 5 && r < 0.35f) ||
               (t->night && f->hunger < 6 && r < 0.5f)) {
        g.id = GOAL_REST; g.urgency = f->energy < 1.5f ? 7 : 4;
    } else if (f->hunger > 6.3f && food_i >= 0) {
        g.id = GOAL_SEEK_FOOD; g.urgency = f->hunger > 8.5f ? 9 : 6;
    } else if (f->sociable > 0.62f && friend_d > 60 && r < 0.36f) {
        g.id = GOAL_FOLLOW_FRIEND; g.urgency = 4;
    } else if (f->curiosity > 6 && r < 0.32f) {
        g.id = (r < 0.16f) ? GOAL_VISIT_BUBBLES : GOAL_INSPECT_REEF; g.urgency = 3;
    } else if (f->bold > 0.75f && f->energy > 6 && r < 0.2f) {
        g.id = GOAL_DART_PLAY; g.urgency = 5;
    } else if (f->goal_age > 5.2f + r * 4.8f) {
        g.id = GOAL_EXPLORE; g.urgency = 3;
    }
    return g;
}
