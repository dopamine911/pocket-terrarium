/* worlds/terrarium/shop.c - the terrarium's shop (pocket-terrarium P5, issue #4 phase 3: moved out of progression.c): what its
 * purchases do - the moss soaks the humid hides, the farm fills the dish at once, a decoration goes to its home on the sand -
 * and HOW TO EARN's lines. The catalogue itself (progression.h SD_TERR_ITEMS, the ids and the prices) is the engine's: one
 * numbering for every world, frozen. */
#include "progression.h"
#include "world_ops.h"
#include <stdio.h>

/* every bit a save's sd_unlocks may keep here: the fish's eleven, and the terrarium's own */
uint32_t terrarium_unlock_bits(void) {
    uint32_t b = (1u << SD_ITEM_COUNT) - 1;
    for (int k = 0; k < SD_TERR_COUNT; k++) b |= SD_TERR_ITEMS[k].bit;
    return b;
}

bool terrarium_can_buy(int item) { (void)item; return true; }

void terrarium_item_bought(tank_t *t, const sd_item_t *it, int item) {
    if (it->bit == SD_ITEM_MOSS) {                         /* fresh moss goes in wet: the humid hides soaked */
        const habitat_t *h = tank_habitat(t);
        for (int k = 0; k < h->n_hides && k < HABITAT_HIDES_MAX; k++) if (h->hides[k].kind == HIDE_HUMID) t->moist[k] = 1.0f;
    }
    if (it->bit == SD_ITEM_FARM) tank_dish_farm(t);        /* its first worms at once */
    if (tank_deco_index(item) >= 0) tank_decor_set(t, item, tank_deco_home(t, item), DECOR_Z_BACK);   /* a decoration: its home on the sand, kept (the placement page moves it) */
}

/* the terrarium's (P5): its meals, stages, sheds, hatchlings, trust and the glass - no grass, seahorse or crab.
   <= 25 chars each: the modal is 336 px wide at scale 2 */
int terrarium_earn_lines(char (*lines)[30], int max) {
    int n = 0;
    (void)max;
    snprintf(lines[n++], 30, "+%d  EACH LIZARD YOU FEED", SD_MEAL);
    snprintf(lines[n++], 30, "+%d/%d/%d  A LIZARD GROWS", SD_STAGE_JUV, SD_STAGE_ADULT, SD_STAGE_ELDER);
    snprintf(lines[n++], 30, "+%d  A CLEAN SHED", SD_SHED_CLEAN);
    snprintf(lines[n++], 30, "+%d  AN EGG HATCHES", SD_BIRTH);
    snprintf(lines[n++], 30, "+%d  A LIZARD TRUSTS YOU", SD_TRUST);
    snprintf(lines[n++], 30, "+%d  %d GLASS SMUDGES", SD_CHORE, SD_CHORE_EVERY);
    return n;
}

/* the habitat's mealworm dish (tank.h THE CRICKETS): the farm fills it, the keeper's hand too */
bool terrarium_dish_here(const tank_t *t) { return tank_dish_here(t); }
