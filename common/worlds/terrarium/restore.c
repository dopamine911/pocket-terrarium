/* worlds/terrarium/restore.c - the terrarium's half of a loaded save (issue #4 phase 3): none. What it keeps - the film on the
 * glass, the sand dollars and their unlocks, the beds and the pieces' spots, the decorations' spots, the dish, the clean run -
 * is put back by load_save for both worlds (their fields are in every save); the aquarium's creatures are the aquarium's alone
 * (worlds/aquarium/restore.c), and a terrarium's tank_init defaults are what its save held of them. */
#include "save_layout.h"
#include "world_ops.h"

void terrarium_restore(tank_t *t, const save_t *s) { (void)t; (void)s; }
