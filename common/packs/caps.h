/* caps.h - capabilities: what a species needs, and what a habitat, the firmware and the model provide (issue #4 phase 2,
 * docs/ptpk.md "Capabilities").
 *
 * A tank runs only when    what the species needs  <=  ( what the habitat provides  AND  what the firmware provides
 *                                                         AND  what the model provides )
 * and this header is the first three's vocabulary: one small fixed bit set (caps_t). The model's part is its goal words, by
 * name (manifest.h) - an open list does not fit a bit set. The world (world.h) decides none of it any more: a world is a
 * preset (which modules, which model, which default packs) and a filter for the shop and the menus (a pack's WORLDS record).
 *
 *   movement modes  SWIM WALK SLITHER   the firmware's modules. Bit 3 is room for the next one; no bit nobody has named is known
 *   areas           WATER LAND BASK SHORE   what the ground is made of; a basking spot is under a lamp; a shore is where water meets land
 *   foods           CRICKET MEALWORM GREENS PELLET   a species' diet (species.h DIET_*), shifted up CAP_FOOD_SHIFT
 *   fittings        LAMP HIDES
 *
 * In a pack file a single capability (a movement mode, an area) is its bit index and a set is the mask (docs/ptpk.md). */
#ifndef CAPS_H
#define CAPS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "world.h"

typedef uint32_t caps_t;
enum {
    CAP_SWIM = 1u << 0, CAP_WALK = 1u << 1, CAP_SLITHER = 1u << 2,
    CAP_WATER = 1u << 4, CAP_LAND = 1u << 5, CAP_BASK = 1u << 6, CAP_SHORE = 1u << 7,
    CAP_CRICKET = 1u << 8, CAP_MEALWORM = 1u << 9, CAP_GREENS = 1u << 10, CAP_PELLET = 1u << 11,
    CAP_LAMP = 1u << 12, CAP_HIDES = 1u << 13
};
#define CAP_FOOD_SHIFT 8                                     /* species.h DIET_x << CAP_FOOD_SHIFT is CAP_x */
#define CAPS_MOVES     (CAP_SWIM | CAP_WALK | CAP_SLITHER)
#define CAPS_AREAS     (CAP_WATER | CAP_LAND | CAP_BASK | CAP_SHORE)
#define CAPS_FOODS     (CAP_CRICKET | CAP_MEALWORM | CAP_GREENS | CAP_PELLET)
#define CAPS_FITTINGS  (CAP_LAMP | CAP_HIDES)
#define CAPS_KNOWN     (CAPS_MOVES | CAPS_AREAS | CAPS_FOODS | CAPS_FITTINGS)
#define CAPS_MEDIA     (CAPS_AREAS | CAPS_FITTINGS)          /* what a species' MEDIA_NEEDS may list: the movement is LOCOMOTION's, the foods DIET's */
#define CAPS_PROVIDED  (CAPS_FITTINGS | CAPS_FOODS)          /* what a habitat's PROVIDES may list (its AREAS the areas) */

/* the bit's number (0..31) of a mask of exactly one bit; -1 for none or several (inline: the encoder reads it without this
   file's .c). caps_name: its name, NULL when no capability carries that number (an unknown bit: bit 3, 14..31) */
static inline int caps_index(caps_t one) {
    if (!one || (one & (one - 1))) return -1;
    int k = 0;
    while (!((one >> k) & 1u)) k++;
    return k;
}
const char *caps_name(unsigned index);
/* the first bit of the set that this firmware does not name, as its number; -1 when it names them all */
int caps_unknown(caps_t set);
/* the names of the bits set, in bit order, ", " between ("BASK, HIDES"); a bit nobody names reads "bit 20". Always terminated, cut to n */
void caps_list(caps_t set, char *out, size_t n);
/* the movement a habitat's areas carry: water carries swimming, land walking and slithering (a habitat provides the room to move) */
caps_t caps_moves_of_areas(caps_t areas);

/* WHAT THIS FIRMWARE PROVIDES - the one place it is said (phase 3 changes this function and nothing around it): the modules and
 * mechanics its engine has. The terrarium's bin walks on land under a lamp, among hides, and feeds crickets and the dish's
 * mealworms; the aquarium's swims in water and drops pellets; the simulator and the tools (PT_ALL_PACKS) carry both worlds'
 * packs, so they report the two together (a cross-world pair is still refused at the tank - by the model: manifest.h) */
#define CAPS_FW_TERRARIUM (CAP_WALK | CAP_LAND | CAP_BASK | CAP_LAMP | CAP_HIDES | CAP_CRICKET | CAP_MEALWORM)
#define CAPS_FW_AQUARIUM  (CAP_SWIM | CAP_WATER | CAP_PELLET)
#if PT_ALL_PACKS
#define CAPS_FW (CAPS_FW_TERRARIUM | CAPS_FW_AQUARIUM)       /* <- the one place: this build's modules */
#elif PT_TERRARIUM
#define CAPS_FW CAPS_FW_TERRARIUM
#else
#define CAPS_FW CAPS_FW_AQUARIUM
#endif
#if PT_ALL_PACKS
/* tests: pretend this is a firmware with exactly these capabilities (on), or the simulator's own again (off) */
void firmware_caps_override(bool on, caps_t caps);
caps_t firmware_caps(void);
#else
static inline caps_t firmware_caps(void) { return CAPS_FW; }
#endif
#endif
