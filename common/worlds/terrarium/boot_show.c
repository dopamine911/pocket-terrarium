/* worlds/terrarium/boot_show.c - THE BOOT SHOW, the terrarium's side (boot_show.h; issue #4 phase 3). The show is the aquarium's
 * alone (worlds/aquarium/boot_show.c: the water rises, the pieces land, the back grass grows from the loop's bake): the
 * terrarium has no water, no pieces and no grass loop, so there is no show here - boot_show_begin refuses, and its tank comes
 * up as it is (the platform's loading frames: its first scene baked). The platform's calls stay the same in both worlds. */
#include "boot_show.h"
#include "render.h"

bool boot_show_on(void) { return false; }
bool boot_show_ready(void) { return render_scene_buf(NULL); }   /* (no grass loop here: never warming, never n > 0 - the scene alone is the wait) */
void boot_show_end(void) {}
bool boot_show_begin(const tank_t *t, bool fill, void *(*alloc)(size_t), void (*release)(void *)) { (void)t; (void)fill; (void)alloc; (void)release; return false; }
bool boot_show_frame(const tank_t *t, uint16_t *fb, float ts, bool loop_whole, float bake_frac) { (void)t; (void)fb; (void)ts; (void)loop_whole; (void)bake_frac; return false; }
