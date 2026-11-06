/* advisor.c — the rule-based advisor stub: the tank's world's reflex rules (the fish's, the lizard's: common/worlds/<world>/rules.c,
 * world_ops.h) behind one entry, and the stateless draw they roll with. */
#include "advisor.h"
#include "world_ops.h"

/* stateless PRNG: advisor must not mutate the tank */
float advisor_frand(const tank_t *t, int fish_idx) {
    uint32_t x = t->rng ^ (0x9E3779B9u * (uint32_t)(fish_idx + 1))
                        ^ (uint32_t)(uint64_t)(t->clock * 977.0);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (float)(x & 0xffffff) / 16777215.0f;
}

/* the reflex of the tank's world (world_ops.h: common/worlds/<world>/rules.c) */
goal_t advisor_rules(const tank_t *t, int fish_idx, bool request) {
    return world_cur()->rules(t, fish_idx, request);
}
