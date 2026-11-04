/* habitat.h - the habitat packs: the terrarium a species lives in, apart from the species itself.
 *
 * Everything about the enclosure the engine would otherwise hardcode - the floor's temperature profile, the
 * basking spot, the hides, the humid hide's moisture - is a compiled-in `const habitat_t`
 * (common/habitat/<name>.c), the same pattern as the species packs (species.h). A terrarium stores both ids
 * (tank_t.habitat_id, tank_t.pack_id; saved), and holds only a pair that fits: the species lists the habitat
 * (species_t.habitats) - tank_set_terrarium refuses anything else. --selftest-packs checks every habitat
 * (habitat_check) and that every habitat's lit floor reaches each fitting species' comfort band somewhere.
 *
 * Positions are fractions of the floor's span (tank.h PLACE_*: 0 = the glass at the warm, left end, 1 = the
 * cool, right end), so a habitat lays out on every board; temperatures are degrees C; times are tended
 * seconds (awake in full, asleep at SLEEP_GROWTH_FRAC). One habitat for now: the desert. */
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

typedef struct {
    const char *name;                   /* "DESERT" */
    uint8_t     id;                     /* stored in the save; never reused; 1..31 (species_t.habitats is a bit per id) */
    world_t     world;                  /* the build it belongs to: only a species of the same world lives in it */
    /* the floor while lit: from its warm end to its cool end, straight across; dark, all of it night_drop_c cooler */
    float       floor_warm_c, floor_cool_c, night_drop_c;
    /* the basking spot: where along the floor, its middle while the lamp is lit, and the lamp's pool round it
       (bask_c in the middle, falling to the floor's own at bask_r_px; dark, the lamp is out) */
    float       bask_u, bask_c, bask_r_px;
    hide_t      hides[HABITAT_HIDES_MAX];
    uint8_t     n_hides;
    /* a humid hide's moisture (docs/shed.md "Moisture"): in a new terrarium, and how much it dries a tended second */
    float       moist_start, moist_dry_per_s;
    float       humidity;               /* the air's baseline 0..1 (the desert ~0.2): data - the shed reads the humid hide's moisture only */
} habitat_t;

#define HABITAT_DESERT  1               /* common/habitat/desert.c */
#define HABITAT_DEFAULT HABITAT_DESERT
extern const habitat_t HABITAT_PACK_DESERT;

const habitat_t *habitat_get(uint8_t id);      /* NULL: no such habitat */
int  habitat_count(void);
const habitat_t *habitat_at(int i);
/* a habitat's own rules (--selftest-packs): an ordered, positive layout, at least one humid hide, a lamp that
 * warms its spot, moisture and humidity in 0..1; false with the first broken rule in why */
bool habitat_check(const habitat_t *h, char *why, size_t n);
/* the species may live in the habitat: it lists the habitat's id (species_t.habitats) and they share a world */
bool habitat_fits(const habitat_t *h, const species_t *s);
#endif
