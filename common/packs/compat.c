/* compat.c - what a species needs, what a habitat provides, and the pair check (compat.h) */
#include "compat.h"
#include <stdio.h>
#include <string.h>

caps_t species_moves(const species_t *s) {
    caps_t m = 0;
    for (int k = 0; k < s->n_loco && k < SPECIES_LOCO_MAX; k++) m |= s->loco[k].mode;
    return m;
}

caps_t species_needs(const species_t *s) {
    caps_t n = s->media_needs | ((caps_t)(s->diet & (DIET_CRICKET | DIET_MEALWORM | DIET_GREENS | DIET_PELLET)) << CAP_FOOD_SHIFT);
    for (int k = 0; k < s->n_loco && k < SPECIES_LOCO_MAX; k++) n |= s->loco[k].mode | s->loco[k].area;
    return n;
}

caps_t habitat_provides(const habitat_t *h) { return h->areas | h->provides | caps_moves_of_areas(h->areas); }

bool world_offers(uint8_t worlds) { return worlds == 0 || (worlds & PT_WORLD) != 0; }

/* what the card says in a few words: the first capability's name; a goal's word, upper case, its underscores spaces */
static void short_cap(char *need, size_t n, caps_t set) {
    if (!need || !n) return;
    caps_list(set & (~set + 1u), need, n);                  /* the lowest bit alone */
}
static void short_goal(char *need, size_t n, const char *word) {
    if (!need || !n) return;
    size_t at = (size_t)snprintf(need, n, "GOAL ");
    for (; at + 1 < n && *word; word++, at++) need[at] = *word == '_' ? ' ' : (*word >= 'a' && *word <= 'z') ? (char)(*word - 'a' + 'A') : *word;
    need[at < n ? at : n - 1] = 0;
}

bool packs_check_with(const species_t *s, const habitat_t *h, caps_t fw, const model_manifest_t *model,
                      char *need, size_t need_n, char *why, size_t why_n) {
    #define NO(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); return false; } while (0)
    if (need && need_n) need[0] = 0;
    if (why && why_n) why[0] = 0;
    if (!s || !h) NO("no such pack");
    if (!(h->id >= 1 && h->id <= 31 && ((s->habitats >> h->id) & 1u))) {
        if (need && need_n) snprintf(need, need_n, "ANOTHER HABITAT");
        NO("%s does not live in %s", s->name, h->name);
    }
    const caps_t want = species_needs(s), have = habitat_provides(h);
    char list[120];
    const caps_t no_fw = want & ~fw;                         /* what no habitat could give it: the firmware has no such module or mechanic */
    if (no_fw) {
        short_cap(need, need_n, no_fw);
        caps_list(no_fw, list, sizeof list);
        NO("%s needs %s; this firmware has none", s->name, list);
    }
    const caps_t no_hab = want & ~have;
    if (no_hab) {
        short_cap(need, need_n, no_hab);
        caps_list(no_hab, list, sizeof list);
        NO("%s needs %s; %s has none", s->name, list, h->name);
    }
    for (int k = 0; k < s->n_goals && k < SPECIES_GOALS_MAX; k++) {
        if (manifest_has_goal(model, s->goal[k])) continue;
        short_goal(need, need_n, s->goal[k]);
        if (model) NO("%s needs the goal \"%s\"; the %s model has none", s->name, s->goal[k], model->tag);
        NO("%s needs the goal \"%s\"; no model's goal list is loaded for this build", s->name, s->goal[k]);
    }
    return true;
    #undef NO
}

bool packs_check(const species_t *s, const habitat_t *h, char *need, size_t need_n, char *why, size_t why_n) {
    return packs_check_with(s, h, firmware_caps(), model_provides(), need, need_n, why, why_n);
}

bool packs_compatible(const species_t *s, const habitat_t *h, char *why, size_t why_n) {
    return packs_check(s, h, NULL, 0, why, why_n);
}

bool packs_offered(const species_t *s, const habitat_t *h) {
    return s && h && !s->test_only && !h->test_only && world_offers(s->worlds) && world_offers(h->worlds) && packs_compatible(s, h, NULL, 0);
}
