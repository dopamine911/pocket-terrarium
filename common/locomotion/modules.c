/* modules.c - the modules this build has, and the lookups that pick one (issue #4 phase 3).
 *
 * THE ONE PLACE a build's modules are listed. The engine (tank.c) names no world: it asks for the movement module of an
 * animal's species (tank_loco, locomotion.h), the food module of a species' diet (tank_food, mechanics.h) and calls every
 * world module at its fixed points (mech_n / mech_at). What a build HAS is what its Makefile / CMake globs compile -
 * common/worlds/<world>/, common/bodies/<body>/ for the build's world, today one - and these two flags say the same to the
 * lists below. THE STEP THAT COMPILES BOTH WORLDS IN ONE BUILD globs both directories and makes both flags 1: nothing else
 * changes, because every choice below is made from a species' capabilities (caps.h), never from the build.
 *
 * (This file is compiled in every build, like the rest of common/locomotion/.) */
#include "locomotion.h"
#include "mechanics.h"
#include "packs/compat.h"
#include "world.h"

#define HAVE_AQUARIUM (PT_WORLD == PT_WORLD_AQUARIUM)
#define HAVE_TERRARIUM (PT_WORLD == PT_WORLD_TERRARIUM)

#if HAVE_AQUARIUM
extern const loco_ops_t loco_swim;
extern const food_ops_t food_pellets;
extern const mech_ops_t mech_aquarium;
#endif
#if HAVE_TERRARIUM
extern const loco_ops_t loco_walk;
extern const food_ops_t food_crickets;
extern const mech_ops_t mech_terrarium;
#endif

static const loco_ops_t *const LOCOS[] = {
#if HAVE_AQUARIUM
    &loco_swim,
#endif
#if HAVE_TERRARIUM
    &loco_walk,
#endif
};
static const food_ops_t *const FOODS[] = {
#if HAVE_AQUARIUM
    &food_pellets,
#endif
#if HAVE_TERRARIUM
    &food_crickets,
#endif
};
static const mech_ops_t *const MECHS[] = {
#if HAVE_AQUARIUM
    &mech_aquarium,
#endif
#if HAVE_TERRARIUM
    &mech_terrarium,
#endif
};
#define N_OF(a) ((int)(sizeof (a) / sizeof (a)[0]))

/* the module that moves a species the way it says (its LOCOMOTION entries: packs/compat.h species_moves); one of a kind
   no module serves is held by the build's first - what the engine always did for a pack of another world in a bin of this */
const loco_ops_t *loco_of(const species_t *sp) {
    const caps_t moves = species_moves(sp);
    for (int k = 0; k < N_OF(LOCOS); k++) if (LOCOS[k]->mode & moves) return LOCOS[k];
    return LOCOS[0];
}
const loco_ops_t *tank_loco(const tank_t *t, int i) {
    (void)i;                           /* one species a tank: the format keeps room for one on each animal (docs/ptpk.md) */
    return loco_of(tank_species(t));
}

/* the module that feeds a species' diet (species.h DIET_*) */
const food_ops_t *food_of(const species_t *sp) {
    for (int k = 0; k < N_OF(FOODS); k++) if (FOODS[k]->diet & sp->diet) return FOODS[k];
    return FOODS[0];
}
const food_ops_t *tank_food(const tank_t *t) { return food_of(tank_species(t)); }
int food_n(void) { return N_OF(FOODS); }
const food_ops_t *food_at(int i) { return FOODS[i]; }

int mech_n(void) { return N_OF(MECHS); }
const mech_ops_t *mech_at(int i) { return MECHS[i]; }
/* the tank's habitat provides an area the module serves (packs/compat.h habitat_provides) */
bool mech_serves(const mech_ops_t *m, const tank_t *t) { return (m->serves & habitat_provides(tank_habitat(t))) != 0; }
