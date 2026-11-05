/* habitat.c - the habitat registry and its rules (habitat.h) */
#include "habitat.h"
#include "terrain.h"
#include <math.h>
#include <stdio.h>

const char *const HIDE_KIND_NAMES[HIDE_KINDS] = { "warm", "humid", "cool" };

#if PT_ALL_PACKS
static const habitat_t *const HABITATS[] = { &HABITAT_PACK_DESERT, &HABITAT_PACK_FRESHWATER };
#elif PT_TERRARIUM
static const habitat_t *const HABITATS[] = { &HABITAT_PACK_DESERT };
#else
static const habitat_t *const HABITATS[] = { &HABITAT_PACK_FRESHWATER };
#endif
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

/* u on surface s (0 the floor, k rock k): within its ends */
static bool on_surface(const habitat_t *h, int s, float u) {
    if (s == 0) return u >= 0 && u <= 1;
    const rock_t *r = &h->rocks[s - 1];
    return r->n >= 2 && u >= r->u[0] && u <= r->u[r->n - 1];
}

bool habitat_check(const habitat_t *h, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (!h->name || !h->name[0]) REFUSE("no name");
    if (h->id < 1 || h->id > 31) REFUSE("id %d is outside 1..31 (species_t.habitats has a bit per id)", h->id);
    if (h->world != WORLD_AQUARIUM && h->world != WORLD_TERRARIUM) REFUSE("world %d is no world", (int)h->world);
    const bool terr = h->world == WORLD_TERRARIUM;           /* a terrarium's thermoregulation needs a gradient, a lamp and the hides */
    if (terr ? !(h->floor_warm_c > h->floor_cool_c) : !(h->floor_warm_c >= h->floor_cool_c))
        REFUSE("the floor's warm end (%.1f C) is not warmer than its cool end (%.1f C)", h->floor_warm_c, h->floor_cool_c);
    if (!(h->night_drop_c >= 0)) REFUSE("the night warms the floor (drop %.1f C)", h->night_drop_c);
    if (!(h->rise_c >= 0)) REFUSE("the air cools toward the lamp (rise %.1f C)", h->rise_c);
    /* the ground (P2c, terrain.h): rocks and ramps well formed here, walkable on every board below */
    if (h->n_rocks > HABITAT_ROCKS_MAX || h->n_ramps > HABITAT_RAMPS_MAX)
        REFUSE("%d rocks and %d ramps, past %d and %d", h->n_rocks, h->n_ramps, HABITAT_ROCKS_MAX, HABITAT_RAMPS_MAX);
    for (int k = 0; k < h->n_rocks; k++) {
        const rock_t *r = &h->rocks[k];
        if (r->n < 2 || r->n > ROCK_PTS_MAX) REFUSE("rock %d has %d points, not 2..%d", k + 1, r->n, ROCK_PTS_MAX);
        for (int i = 0; i < r->n; i++) {
            if (!(r->u[i] >= 0 && r->u[i] <= 1 && r->v[i] > 0 && r->v[i] < 1)) REFUSE("rock %d's point %d (%.2f, %.2f) is off the glass", k + 1, i, r->u[i], r->v[i]);
            if (i && !(r->u[i] > r->u[i - 1])) REFUSE("rock %d runs back on itself at its point %d (u %.2f after %.2f)", k + 1, i, r->u[i], r->u[i - 1]);
        }
    }
    for (int k = 0; k < h->n_ramps; k++) {
        const ramp_t *m = &h->ramps[k];
        if (m->lo > h->n_rocks || m->hi > h->n_rocks || m->lo == m->hi) REFUSE("ramp %d joins surfaces %d and %d (two of 0..%d)", k + 1, m->lo, m->hi, h->n_rocks);
        if (!on_surface(h, m->lo, m->u_lo) || !on_surface(h, m->hi, m->u_hi))
            REFUSE("ramp %d ends off its surfaces (%.2f on %d, %.2f on %d)", k + 1, m->u_lo, m->lo, m->u_hi, m->hi);
    }
    if (h->bask_surf > h->n_rocks || !on_surface(h, h->bask_surf, h->bask_u))
        REFUSE("the basking spot at %.2f is off its surface (%d)", h->bask_u, h->bask_surf);
    const bool lamp = terr || h->bask_r_px != 0;            /* a lamp: the terrarium's always, an aquarium's if it has one */
    if (lamp && !(h->bask_r_px > 0)) REFUSE("the lamp's pool has no size (%.0f px)", h->bask_r_px);
    if (h->n_hides < (terr ? 1 : 0) || h->n_hides > HABITAT_HIDES_MAX) REFUSE("%d hides, not %d..%d", h->n_hides, terr ? 1 : 0, HABITAT_HIDES_MAX);
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
    if (terr && !humid) REFUSE("no humid hide: a lizard needs one to shed (docs/shed.md)");
    if (humid) {                                             /* (a habitat without a humid hide has no moisture to keep) */
        if (!(h->moist_start >= 0 && h->moist_start <= 1)) REFUSE("a new humid hide's moisture %.2f is outside 0..1", h->moist_start);
        if (!(h->moist_dry_per_s > 0)) REFUSE("a humid hide never dries (%g a second)", h->moist_dry_per_s);
    }
    if (!(h->humidity >= 0 && h->humidity <= 1)) REFUSE("humidity %.2f is outside 0..1", h->humidity);
    if (h->dish && !(h->dish_u >= 0 && h->dish_u <= 1)) REFUSE("the mealworm dish at %.2f is off the floor", h->dish_u);
    /* on every board: the ground walkable (terrain.h THE RULES), and the lamp warming its spot over the air there */
    for (int b = 0; b < GLASS_BOARDS; b++) {
        const glass_t g = glass_of(b);
        if (!terrain_check(h, &g, why, n)) return false;
        if (lamp) {
            terrain_t tr; terrain_lay(h, &g, &tr);
            const float under = terrain_air_open_c(&tr, h, tr.bask_x, tr.bask_y);
            if (!(h->bask_c > under)) REFUSE("on the %s: the lamp does not warm its spot (%.1f C over %.1f C air)", g.name, h->bask_c, under);
        }
        if (h->dish) {                                       /* the dish: on the sand, clear of every hide, room over it for a lizard's head */
            terrain_t tr; terrain_lay(h, &g, &tr);
            const float dx = glass_floor_x(&g, h->dish_u), py = glass_place_y(&g);
            for (int k = 0; k < h->n_hides; k++) {
                const float gap = fabsf(glass_floor_x(&g, h->hides[k].u) - dx);
                if (gap < TERRAIN_HIDE_HW + HABITAT_DISH_HW) REFUSE("on the %s: the mealworm dish is %.0f px from hide %d (under %.0f)", g.name, gap, k, TERRAIN_HIDE_HW + HABITAT_DISH_HW);
            }
            for (int s = 1; s < tr.n_paths; s++) {
                const terrain_path_t *p = &tr.path[s];
                for (float x = dx - HABITAT_DISH_HW; x <= dx + HABITAT_DISH_HW; x += 2)
                    if (x >= p->x[0] && x <= p->x[p->n - 1] && py - terrain_y(p, x) < TERRAIN_HEADROOM_PX)
                        REFUSE("on the %s: path %d runs %.0f px over the mealworm dish (under %.0f)", g.name, s, py - terrain_y(p, x), TERRAIN_HEADROOM_PX);
            }
        }
    }
    return true;
    #undef REFUSE
}
