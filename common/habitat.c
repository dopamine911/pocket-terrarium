/* habitat.c - the habitat registry and its rules (habitat.h) */
#include "habitat.h"
#include "terrain.h"
#include "packs/builtin.h"                 /* PTPK_BUILTIN_HABITATS: how many packs this build ships */
#include "packs/ptpk.h"                    /* packs_init, PACKS_SPARE */
#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const HIDE_KIND_NAMES[HIDE_KINDS] = { "warm", "humid", "cool" };

/* THE POOL, as species.c's: this build's built-in habitats and PACKS_SPARE slots for the ones found at run time */
#define HABITAT_POOL (PTPK_BUILTIN_HABITATS + PACKS_SPARE)
static habitat_t s_pool[HABITAT_POOL];
static uint32_t  s_crc[HABITAT_POOL];
static int       s_n;
static uint32_t  s_gen;

int habitat_count(void) { packs_init(); return s_n; }
const habitat_t *habitat_at(int i) { packs_init(); return i >= 0 && i < s_n ? &s_pool[i] : NULL; }
const habitat_t *habitat_get(uint8_t id) {
    packs_init();
    for (int i = 0; i < s_n; i++) if (s_pool[i].id == id) return &s_pool[i];
    return NULL;
}
const habitat_t *habitat_by_uid(uint32_t uid) {
    packs_init();
    for (int i = 0; uid && i < s_n; i++) if (s_pool[i].uid == uid) return &s_pool[i];
    return NULL;
}
uint32_t habitat_generation(void) { return s_gen; }

habitat_t *habitat_slot(void) {
    packs_init();
    if (s_n >= HABITAT_POOL) return NULL;
    memset(&s_pool[s_n], 0, sizeof s_pool[s_n]);
    return &s_pool[s_n];
}

bool habitat_commit(habitat_t *h, bool builtin, uint8_t legacy_id, uint32_t crc, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (h != &s_pool[s_n] || s_n >= HABITAT_POOL) REFUSE("not the slot the registry gave out");
    if (!h->uid) REFUSE("no uid");
    for (int i = 0; i < s_n; i++) if (s_pool[i].uid == h->uid) REFUSE("uid %08x is %s's already", (unsigned)h->uid, s_pool[i].name);
    if (builtin) {                                         /* the one-byte id old saves hold: a species' bit (1..31) */
        if (legacy_id < 1 || legacy_id > 31) REFUSE("a built-in habitat's legacy id %d is outside 1..31", legacy_id);
        for (int i = 0; i < s_n; i++) if (s_pool[i].id == legacy_id) REFUSE("legacy id %d is %s's already", legacy_id, s_pool[i].name);
        h->id = legacy_id;
    } else {                                               /* a handle for this run: the lowest free of 3..30 (31 is the test desert's) */
        int id = 3;
        for (; id <= 30; id++) { bool used = false; for (int i = 0; i < s_n; i++) used |= s_pool[i].id == id; if (!used) break; }
        if (id > 30) REFUSE("no free id");
        h->id = (uint8_t)id;
    }
    h->builtin = builtin;
    if (!habitat_check(h, why, n)) return false;
    s_crc[s_n] = crc; s_n++; s_gen++;
    species_relink();                                      /* a species may have been waiting for it */
    return true;
    #undef REFUSE
}

pack_seen_t habitat_seen(uint32_t uid, uint32_t crc, const char **by) {
    packs_init();
    for (int i = 0; i < s_n; i++)
        if (s_pool[i].uid == uid) { if (by) *by = s_pool[i].name; return s_crc[i] == crc ? PACK_SAME : PACK_UID_TAKEN; }
    return PACK_NEW;
}

void habitat_drop_loaded(void) {
    int k = 0;
    for (int i = 0; i < s_n; i++) if (s_pool[i].builtin) { if (k != i) { s_pool[k] = s_pool[i]; s_crc[k] = s_crc[i]; } k++; }
    s_n = k; s_gen++;
    species_relink();                                      /* ... and a species that lived in one that is gone loses its bit */
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
    if (!h->name[0]) REFUSE("no name");
    if (h->id < 1 || h->id > 31) REFUSE("id %d is outside 1..31 (species_t.habitats has a bit per id)", h->id);
    /* what it provides (AREAS, PROVIDES): somewhere to be, of kinds that exist (the fields it says it has are held to below) */
    if (!(h->areas & (CAP_WATER | CAP_LAND))) REFUSE("it has neither WATER nor LAND among its areas (%08x)", (unsigned)h->areas);
    if (h->areas & ~CAPS_AREAS) REFUSE("areas %08x lists more than areas", (unsigned)h->areas);
    if (h->provides & ~CAPS_PROVIDED) REFUSE("provides %08x lists more than a lamp, hides and foods", (unsigned)h->provides);
    if ((h->areas & CAP_SHORE) && (h->areas & (CAP_WATER | CAP_LAND)) != (CAP_WATER | CAP_LAND)) REFUSE("a SHORE is where WATER meets LAND, and it has not both");
    const bool land = (h->areas & CAP_LAND) != 0;            /* land's thermoregulation needs a gradient, a lamp and the hides (the terrarium's) */
    if (land ? !(h->floor_warm_c > h->floor_cool_c) : !(h->floor_warm_c >= h->floor_cool_c))
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
    const bool lamp = land || h->bask_r_px != 0;            /* a lamp: land's always, a water habitat's if it has one */
    if (lamp && !(h->bask_r_px > 0)) REFUSE("the lamp's pool has no size (%.0f px)", h->bask_r_px);
    if (h->n_hides < (land ? 1 : 0) || h->n_hides > HABITAT_HIDES_MAX) REFUSE("%d hides, not %d..%d", h->n_hides, land ? 1 : 0, HABITAT_HIDES_MAX);
    /* what it says it provides is what it has (docs/ptpk.md "Capabilities"): a basking spot and a lamp where the lamp has a pool, hides
       where there are hides, mealworms where there is a dish; its crickets and mealworms need land, its pellets water */
    if (((h->areas & CAP_BASK) != 0) != lamp) REFUSE("BASK %s its areas, and its lamp's pool is %.0f px", (h->areas & CAP_BASK) ? "is among" : "is not among", h->bask_r_px);
    if (((h->provides & CAP_LAMP) != 0) != lamp) REFUSE("LAMP %s what it provides, and its lamp's pool is %.0f px", (h->provides & CAP_LAMP) ? "is among" : "is not among", h->bask_r_px);
    if (((h->provides & CAP_HIDES) != 0) != (h->n_hides > 0)) REFUSE("HIDES %s what it provides, and it has %d hides", (h->provides & CAP_HIDES) ? "is among" : "is not among", h->n_hides);
    if (((h->provides & CAP_MEALWORM) != 0) != h->dish) REFUSE("MEALWORM %s what it provides, and it has %s dish", (h->provides & CAP_MEALWORM) ? "is among" : "is not among", h->dish ? "a" : "no");
    if ((h->provides & (CAP_CRICKET | CAP_MEALWORM)) && !land) REFUSE("crickets and mealworms need LAND");
    if ((h->provides & CAP_PELLET) && !(h->areas & CAP_WATER)) REFUSE("pellets need WATER");
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
    if (land && !humid) REFUSE("no humid hide: a lizard needs one to shed (docs/shed.md)");
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
