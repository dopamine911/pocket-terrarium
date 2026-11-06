/* worlds/aquarium/shop.c - the aquarium's shop (issue #4 phase 3, moved out of progression.c): what a purchase does in the
 * water - the plant, the snail, the castle, the coral, the reef cluster, the school of shrimp, the urchin, the seahorse, the
 * pumpkin, the volcano, the crab - and HOW TO EARN's lines. The catalogue itself (progression.h SD_ITEMS, the ids and the
 * prices) is the engine's: one numbering for every world, frozen. */
#include "progression.h"
#include "world_ops.h"
#include <stdio.h>

/* every bit a save's sd_unlocks may keep here: the fish's eleven */
uint32_t aquarium_unlock_bits(void) { return (1u << SD_ITEM_COUNT) - 1; }

/* the terrarium's items are its bin's shop alone */
bool aquarium_can_buy(int item) { return item < SD_TERR_FIRST; }

void aquarium_item_bought(tank_t *t, const sd_item_t *it, int item) {
    (void)item;
    if (it->bit == SD_ITEM_PLANT) tank_plant_place(t);
    if (it->bit == SD_ITEM_SNAIL) tank_snail_place(t);
    if (it->bit == SD_ITEM_CASTLE) tank_castle_place(t);
    if (it->bit == SD_ITEM_CORAL) tank_coral_place(t);
    if (it->bit == SD_ITEM_CLUSTER) tank_cluster_place(t);
    if (it->bit == SD_ITEM_SHRIMP) { tank_shrimp_place(t, SHRIMP_START); t->shrimp_food = 0; t->shrimp_cool = 0; }
    if (it->bit == SD_ITEM_URCHIN) tank_urchin_place(t);
    if (it->bit == SD_ITEM_SEAHORSE) { tank_seahorse_place(t); t->sh_s = 0; t->sh_hitches = 0; t->sh_trust_passed = 0; }
    if (it->bit == SD_ITEM_PUMPKIN) tank_pumpkin_place(t);
    if (it->bit == SD_ITEM_VOLCANO) tank_volcano_place(t);
    if (it->bit == SD_ITEM_CRAB) { t->cr_variant = (uint8_t)(tank_randf(t, 0, 255.999f)); tank_crab_place(t); }   /* its seed pattern is drawn once, at the sale */
}

/* <= 25 chars each: the modal is 336 px wide at scale 2 */
int aquarium_earn_lines(char (*lines)[30], int max) {
    int n = 0;
    (void)max;
    snprintf(lines[n++], 30, "+%d  EACH FISH YOU FEED", SD_MEAL);
    snprintf(lines[n++], 30, "+%d/%d/%d  A FISH GROWS UP", SD_STAGE_JUV, SD_STAGE_ADULT, SD_STAGE_ELDER);
    snprintf(lines[n++], 30, "+%d  A NEW FRY IS BORN", SD_BIRTH);
    snprintf(lines[n++], 30, "+%d  A FISH FULLY TRUSTS", SD_TRUST);
    snprintf(lines[n++], 30, "+%d  %d ALGAE COLONIES", SD_CHORE, SD_CHORE_EVERY);
    snprintf(lines[n++], 30, "+%d  %d CM OF GRASS CUT", SD_CHORE, SD_TRIM_CM);
    snprintf(lines[n++], 30, "+%d  THE SEAHORSE SNACKS", SD_SNACK);
    snprintf(lines[n++], 30, "+%d/%d  THE CRAB DIGS ONE UP", SD_CRAB_FIND, SD_CRAB_FIND_AGAIN);   /* the first, then every one after (the badge's reward) */
    return n;
}

/* freshwater has no dish: an aquarium's worms are always none */
bool aquarium_dish_here(const tank_t *t) { (void)t; return false; }
