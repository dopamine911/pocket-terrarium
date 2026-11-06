/* compat.h - may this species live in this habitat, on this firmware, under this model? (issue #4 phase 2, docs/ptpk.md "Capabilities")
 *
 *      what the species needs   <=   what the habitat provides  AND  what the firmware provides  AND  what the model provides
 *
 * The species' side adds up (species_needs): the ways it moves and the areas it moves in (LOCOMOTION), the areas and fittings
 * it cannot live without (MEDIA_NEEDS), the foods it eats (DIET); the habitat's (habitat_provides): its areas, its lamp, hides
 * and foods, and the movement its areas carry (water: swimming; land: walking). The firmware's is caps.h firmware_caps; the
 * model's is its goal words (manifest.h model_provides): every goal the species names must be one of them. The species' own
 * list of habitats stays authoritative - it comes first - and the capability check is in addition: it is what lets a habitat
 * of two kinds of ground serve a species of two kinds of movement later. The world decides none of it.
 *
 * tank.h tank_set_packs calls packs_compatible; the shop and the menus call packs_offered. */
#ifndef COMPAT_H
#define COMPAT_H
#include <stdbool.h>
#include <stddef.h>
#include "species.h"
#include "habitat.h"
#include "manifest.h"

caps_t species_moves(const species_t *s);        /* the movement modes of its LOCOMOTION entries */
caps_t species_needs(const species_t *s);        /* everything it cannot live without: its modes and their areas, its media needs, its diet's foods */
caps_t habitat_provides(const habitat_t *h);     /* its areas and provisions, and the movement its areas carry */
bool   world_offers(uint8_t worlds);             /* a pack's WORLDS mask lets this build's shop and menus offer it (0: every world's do) */

/* The whole check, the providers named by the caller (the tests'): the first unmet need is the reason - in this order: the species
 * lists the habitat; the firmware has everything it needs ("LEOPARD GECKO needs WALK; this firmware has none"); the habitat has
 * the rest ("LEOPARD GECKO needs BASK; FRESHWATER has none"); the model has every goal word ("... needs the goal "climb"; the
 * lizard_v1 model has none"). need (optional) gets the same in two or three words for a card, uppercase: BASK, GOAL CLIMB,
 * ANOTHER HABITAT; why (optional) the sentence. Both are "" when the pair is held. model NULL: no model's manifest is loaded */
bool packs_check_with(const species_t *s, const habitat_t *h, caps_t fw, const model_manifest_t *model,
                      char *need, size_t need_n, char *why, size_t why_n);
/* ... with this firmware's capabilities and this build's model (caps.h firmware_caps, manifest.h model_provides) */
bool packs_check(const species_t *s, const habitat_t *h, char *need, size_t need_n, char *why, size_t why_n);
bool packs_compatible(const species_t *s, const habitat_t *h, char *why, size_t why_n);

/* the shop's and the menus' filter: both packs name this build's world (their WORLDS - a filter, not a gate), neither is for the
 * tests, and the pair is held (packs_compatible). A pack from the card is offered where it can run */
bool packs_offered(const species_t *s, const habitat_t *h);
#endif
