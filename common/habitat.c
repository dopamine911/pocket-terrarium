/* habitat.c - the habitat registry and its rules (habitat.h) */
#include "habitat.h"
#include <stdio.h>

const char *const HIDE_KIND_NAMES[HIDE_KINDS] = { "warm", "humid", "cool" };

static const habitat_t *const HABITATS[] = { &HABITAT_PACK_DESERT };
#define N_HABITATS ((int)(sizeof HABITATS / sizeof HABITATS[0]))

int habitat_count(void) { return N_HABITATS; }
const habitat_t *habitat_at(int i) { return i >= 0 && i < N_HABITATS ? HABITATS[i] : NULL; }
const habitat_t *habitat_get(uint8_t id) {
    for (int i = 0; i < N_HABITATS; i++) if (HABITATS[i]->id == id) return HABITATS[i];
    return NULL;
}

bool habitat_fits(const habitat_t *h, const species_t *s) {
    return h && s && h->id >= 1 && h->id <= 31 && ((s->habitats >> h->id) & 1u) && s->world == h->world;
}

/* two hides this far apart along the floor: two HIDE_R (tank.h) on the narrowest floor, the watch's ~280 px */
#define HIDE_GAP_U 0.2f

bool habitat_check(const habitat_t *h, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (!h->name || !h->name[0]) REFUSE("no name");
    if (h->id < 1 || h->id > 31) REFUSE("id %d is outside 1..31 (species_t.habitats has a bit per id)", h->id);
    if (h->world != WORLD_AQUARIUM && h->world != WORLD_TERRARIUM) REFUSE("world %d is no world", (int)h->world);
    if (!(h->floor_warm_c > h->floor_cool_c)) REFUSE("the floor's warm end (%.1f C) is not warmer than its cool end (%.1f C)", h->floor_warm_c, h->floor_cool_c);
    if (!(h->night_drop_c >= 0)) REFUSE("the night warms the floor (drop %.1f C)", h->night_drop_c);
    if (!(h->bask_u >= 0 && h->bask_u <= 1)) REFUSE("the basking spot at %.2f is off the floor", h->bask_u);
    if (!(h->bask_r_px > 0)) REFUSE("the lamp's pool has no size (%.0f px)", h->bask_r_px);
    float under = h->floor_warm_c + (h->floor_cool_c - h->floor_warm_c) * h->bask_u;
    if (!(h->bask_c > under)) REFUSE("the lamp does not warm its spot (%.1f C over a %.1f C floor)", h->bask_c, under);
    if (h->n_hides < 1 || h->n_hides > HABITAT_HIDES_MAX) REFUSE("%d hides, not 1..%d", h->n_hides, HABITAT_HIDES_MAX);
    int humid = 0;
    for (int k = 0; k < h->n_hides; k++) {
        const hide_t *d = &h->hides[k];
        if (d->kind >= HIDE_KINDS) REFUSE("hide %d is of no kind (%d)", k, (int)d->kind);
        if (!(d->u >= 0 && d->u <= 1)) REFUSE("hide %d at %.2f is off the floor", k, d->u);
        if (!(d->t_dark_c <= d->t_lit_c)) REFUSE("hide %d is warmer dark (%.1f C) than lit (%.1f C)", k, d->t_dark_c, d->t_lit_c);
        for (int j = 0; j < k; j++) {
            float gap = d->u - h->hides[j].u; if (gap < 0) gap = -gap;
            if (gap < HIDE_GAP_U) REFUSE("hides %d and %d are %.2f of the floor apart, under %.2f", j, k, gap, HIDE_GAP_U);
        }
        humid += d->kind == HIDE_HUMID;
    }
    if (!humid) REFUSE("no humid hide: a lizard needs one to shed (docs/shed.md)");
    if (!(h->moist_start >= 0 && h->moist_start <= 1)) REFUSE("a new humid hide's moisture %.2f is outside 0..1", h->moist_start);
    if (!(h->moist_dry_per_s > 0)) REFUSE("a humid hide never dries (%g a second)", h->moist_dry_per_s);
    if (!(h->humidity >= 0 && h->humidity <= 1)) REFUSE("humidity %.2f is outside 0..1", h->humidity);
    return true;
    #undef REFUSE
}
