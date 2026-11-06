/* tr_mech.c - the terrarium as a world module (mechanics.h): what it adds to a tank besides its walkers and its crickets -
 * the shed and the mister, and the decorations it sells
 * (issue #4 phase 3) */
#include "tank.h"
#include "tank_priv.h"
#include "worlds/terrarium/terrarium_priv.h"

const mech_ops_t mech_terrarium = {
    .name = "terrarium", .serves = CAP_LAND,
    .shed_clock = terr_shed_clock, .shed_growth = terr_shed_growth, .in_damp_hide = terr_in_damp_hide,
    .mist_pass = terr_mist_pass, .mist_soak = terr_mist_soak,
    .deco = terr_deco, .deco_clear = terr_deco_clear, .deco_slide = terr_deco_slide, .deco_home = terr_deco_home,
    .deco_hit = terr_deco_hit,
};
