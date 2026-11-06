/* worlds/aquarium/restore.c - the aquarium's half of a loaded save (issue #4 phase 3, moved out of progression.c load_save): the
 * creatures a purchase brought - the school of shrimp, the urchin, the crab and the seahorse - put back from the frozen save_t
 * (save_layout.h), their tallies and their places. load_save has by then put back everything the worlds share (the beds, the
 * film on the glass, the sand dollars and the unlocks, the pieces' spots) and calls this through world_cur()->restore. */
#include "save_layout.h"
#include "world_ops.h"

void aquarium_restore(tank_t *t, const save_t *s) {
    if (t->sd_unlocks & SD_ITEM_SHRIMP) {             /* the school back in the grass, its count and its progress */
        tank_shrimp_place(t, s->shrimp_n >= SHRIMP_START ? s->shrimp_n : SHRIMP_START);
        t->shrimp_food = s->shrimp_food <= SHRIMP_PER_JOIN ? s->shrimp_food : SHRIMP_PER_JOIN;
        t->shrimp_cool = s->shrimp_cool > 0 && s->shrimp_cool <= SHRIMP_COOLDOWN_S ? s->shrimp_cool : 0;
        t->shrimp_eaten = s->shrimp_eaten > 0 ? s->shrimp_eaten : 0;
    } else { t->shrimp_n = 0; t->shrimp_eaten = 0; }
    if (t->sd_unlocks & SD_ITEM_URCHIN) {             /* back where it was on the floor, its tally */
        tank_urchin_place(t);
        if (s->urchin_x > 0) t->urchin_x = s->urchin_x;
        t->urchin_grazed_px = s->urchin_grazed_px > 0 ? s->urchin_grazed_px : 0;
    } else { t->urchin_x = -1; t->urchin_grazed_px = 0; }
    if (t->sd_unlocks & SD_ITEM_CRAB) {             /* back where it stopped, with its seeds and its finds */
        t->cr_variant = s->cr_variant; t->cr_eaten = s->cr_eaten;
        tank_crab_place(t);
        if (s->cr_x > 0) t->cr_x = s->cr_x;
        t->cr_found = s->cr_found; t->cr_found_paid = s->cr_found_paid > s->cr_found ? s->cr_found : s->cr_found_paid;
        t->cr_find_cool = s->cr_find_wait ? (float)(s->cr_find_wait - 1) : tank_randf(t, CR_FIND_FIRST_LO_S, CR_FIND_FIRST_HI_S);   /* the saved wait (0.3.9.1); an older save: soon */
    } else t->cr_x = -1;
    if (t->sd_unlocks & SD_ITEM_SEAHORSE) {           /* back on a blade, its tally */
        tank_seahorse_place(t);
        t->sh_s = s->sh_s > 0 ? s->sh_s : 0;
        if (s->sh_secs > 1 && s->sh_secs - 1.0 > t->sh_s) t->sh_s = s->sh_secs - 1.0;   /* (the float, if a rollback ran on past these) */
        t->sh_hitches = s->sh_hitches > 0 ? s->sh_hitches : 0;
        t->sh_trust_passed = s->sh_trust_passed > 0 ? s->sh_trust_passed : 0;
        t->sh_eaten = s->sh_eaten > 0 ? s->sh_eaten : 0;
        t->sh_snack_pay = s->sh_snack_pay > 0 ? s->sh_snack_pay : 0;
        t->sd_snacks_paid = s->sd_snacks_paid > 0 ? s->sd_snacks_paid : 0;
        t->sh_snack_cool = s->sh_snack_cool > 0 && s->sh_snack_cool <= SH_SNACK_COOL_S ? s->sh_snack_cool : 0;
    } else { t->sh_x = -1; t->sh_s = 0; t->sh_hitches = 0; t->sh_trust_passed = 0; t->sh_eaten = 0; t->sh_snack_pay = 0; t->sd_snacks_paid = 0; t->sh_snack_cool = 0; }
}
