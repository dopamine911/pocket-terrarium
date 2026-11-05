/* habitat.h - the habitat packs: the terrarium a species lives in, apart from the species itself.
 *
 * Everything about the enclosure the engine would otherwise hardcode - the floor's temperature profile and the air's
 * rise toward the lamp, the ground (the floor, its rocks and the ramps between them), the basking spot, the hides, the
 * humid hide's moisture - is a compiled-in `const habitat_t` (common/habitat/<name>.c), the same pattern as the species
 * packs (species.h). A terrarium stores both ids (tank_t.habitat_id, tank_t.pack_id; saved), and holds only a pair that
 * fits: the species lists the habitat (species_t.habitats) - tank_set_packs refuses anything else. --selftest-packs
 * checks every habitat (habitat_check, its ground on all four boards: terrain.h THE RULES) and that every habitat's lit
 * ground reaches each fitting species' comfort band somewhere.
 *
 * Positions are fractions of the glass, so a habitat lays out on every board (terrain.h): along the floor, u from the
 * warm, left end (0) to the cool, right end (1) of its span (tank.h PLACE_*); a rock's points (u, v) across the glass at
 * their height and up it. Temperatures are degrees C; times are tended seconds (awake in full, asleep at
 * SLEEP_GROWTH_FRAC). Two habitats: the terrarium's desert, and the aquarium's freshwater (P2b: pocket-tank's water as a
 * pack - a heater's even 25 C, no lamp, no hides, no rocks). */
#ifndef HABITAT_H
#define HABITAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "species.h"

typedef enum { HIDE_WARM, HIDE_HUMID, HIDE_COOL, HIDE_KINDS } hide_kind_t;
extern const char *const HIDE_KIND_NAMES[HIDE_KINDS];      /* warm humid cool */
typedef struct {
    hide_kind_t kind;
    float       u;                      /* along the floor, 0 (warm end) .. 1 (cool end) */
    float       t_lit_c, t_dark_c;      /* inside it, the light on / off */
} hide_t;
#define HABITAT_HIDES_MAX 4
/* THE GROUND (P2c, terrain.h): the floor (always: surface 0), and ROCKS - raised surfaces, surface 1.. in order - each a
 * walking line left to right through up to ROCK_PTS_MAX points (u across the glass at that height, 0..1 with u rising;
 * v up it, 0 the floor's line .. 1 the glass's top) - joined by RAMPS: a straight slope from a point on one surface (lo,
 * at u_lo along it - the floor's fraction, or a rock's u between its points) to a point on another (hi, u_hi). A walker
 * changes surface only at a ramp's ends; none of it steeper than TERRAIN_SLOPE_MAX on any board (terrain.h THE RULES) */
#define HABITAT_ROCKS_MAX 4
#define HABITAT_RAMPS_MAX 6
#define ROCK_PTS_MAX      4
typedef struct {
    uint8_t n;                          /* its points, 2..ROCK_PTS_MAX, left to right */
    float   u[ROCK_PTS_MAX], v[ROCK_PTS_MAX];
    bool    shelf;                      /* as drawn (P3): a shelf on the back wall, its underside showing - else a terrace standing on the sand */
} rock_t;
typedef struct {
    uint8_t lo, hi;                     /* the surfaces it joins: 0 the floor, k rock k (rocks[k - 1]) */
    float   u_lo, u_hi;                 /* where on each */
} ramp_t;

typedef struct {
    const char *name;                   /* "DESERT" */
    uint8_t     id;                     /* stored in the save; never reused; 1..31 (species_t.habitats is a bit per id) */
    world_t     world;                  /* the build it belongs to: only a species of the same world lives in it */
    bool        test_only;              /* compiled in for the tests (A16's warm desert), never offered to a keeper */
    /* the air while lit: the floor from its warm end to its cool end, straight across (an aquarium's may be even), and
       rise_c warmer at the glass's top than on the floor under it - the lamp's heat rising, straight up with v; dark, the
       lamp out, all of it night_drop_c cooler than the lit floor and no rise */
    float       floor_warm_c, floor_cool_c, night_drop_c, rise_c;
    /* the basking spot: on surface bask_surf (0 the floor, k rock k) at bask_u along it - the highest ground under the
       lamp (terrain.h THE RULES) - its middle while the lamp is lit, and the lamp's pool round it (bask_c in the middle,
       falling to the air's own at bask_r_px; dark, the lamp is out). bask_r_px 0: no lamp (an aquarium) */
    uint8_t     bask_surf;
    float       bask_u, bask_c, bask_r_px;
    hide_t      hides[HABITAT_HIDES_MAX];   /* on the floor: a hide is entered from the floor */
    uint8_t     n_hides;
    rock_t      rocks[HABITAT_ROCKS_MAX];
    uint8_t     n_rocks;
    ramp_t      ramps[HABITAT_RAMPS_MAX];
    uint8_t     n_ramps;
    /* a humid hide's moisture (docs/shed.md "Moisture"): in a new terrarium, and how much it dries a tended second */
    float       moist_start, moist_dry_per_s;
    float       humidity;               /* the air's baseline 0..1 (the desert ~0.2): data - the shed reads the humid hide's moisture only */
    /* the mealworm dish (P2c, tank.h THE CRICKETS): a fixed place on the floor at dish_u (the places' u), the keeper's feed
       gesture over it fills it; false: none (an aquarium's) */
    bool        dish;
    float       dish_u;
} habitat_t;

#define HABITAT_DESERT     1            /* common/habitat/desert.c (the terrarium) */
#define HABITAT_FRESHWATER 2            /* common/habitat/freshwater.c (the aquarium) */
#define HABITAT_TEST_WARM  31           /* common/habitat/desert.c: A16's - the desert 6 C warmer throughout, for the test pack */
#if PT_TERRARIUM
#define HABITAT_DEFAULT HABITAT_DESERT
#else
#define HABITAT_DEFAULT HABITAT_FRESHWATER
#endif
/* the dish's half-width on the sand (the rules keep it clear of the hides; render.c draws it this wide) */
#define HABITAT_DISH_HW 14.0f
extern const habitat_t HABITAT_PACK_DESERT, HABITAT_PACK_FRESHWATER, HABITAT_PACK_TEST_WARM;

const habitat_t *habitat_get(uint8_t id);      /* NULL: no such habitat */
int  habitat_count(void);
const habitat_t *habitat_at(int i);
/* a habitat's own rules (--selftest-packs), its world's: a terrarium's - an ordered, positive layout, a warm end warmer
 * than its cool end, a lamp that warms its spot, at least one humid hide; an aquarium's - the floor may be even, a lamp
 * and hides are its own business (none in freshwater); every one's moisture and humidity in 0..1, and its ground well
 * formed and walkable on all four boards (terrain.h THE RULES). false with the first broken rule in why */
bool habitat_check(const habitat_t *h, char *why, size_t n);
/* the species may live in the habitat: it lists the habitat's id (species_t.habitats) and they share a world */
bool habitat_fits(const habitat_t *h, const species_t *s);
#endif
