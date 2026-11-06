/* view.c - THE AQUARIUM's descriptor for the renderer (world_view.h): the scene's hooks and the pages' choices. */
#include "world_view.h"
#include "worlds/aquarium/scene.h"
#include "icons.h"
#include "progression.h"

extern const body_ops_t fish_body;                              /* bodies/fish/fish.c */
static const body_ops_t *const BODIES[] = { &fish_body };

/* the badges a world draws its own way (WORLD_ICON: the terrarium's geckos, crickets, hides and rocks, P3b); the
   bought things' badges (school, reef, seahorse, crab) stay the shop's own pictures for as long as the shop sells them */
static const badge_t FISH_BADGES[6] = {
    { MS_FIRST_MEAL_FROM_YOU, &icon_ms_first_meal },  { MS_FIRST_HOLD_APPROACH, &icon_ms_first_hold_approach },
    { MS_FIRST_GRASS, &icon_ms_first_seagrass },      { MS_FIRST_BUBBLES, &icon_ms_first_bubbles },   /* (the third was "first reef" until 0.3.0) */
    { MS_FIRST_FOLLOW, &icon_ms_first_follow },       { MS_FIRST_DART, &icon_ms_first_dart },
};
#define TANK_BADGE_N 12                         /* (11 lost the crab's badge off the row: 0.4's FIRST_MUTATION met 0.3.9's CRAB_FIND at the merge) */
static const badge_t TANK_BADGES[TANK_BADGE_N] = {
    { TMS_FIRST_FEEDING, &icon_ms_first_feeding },    { TMS_FIRST_TRIM, &icon_ms_first_trimming },
    { TMS_FIRST_CLEANING, &icon_ms_first_glass_cleaning }, { TMS_FIRST_FULL_NIGHT, &icon_ms_first_quiet_night },
    { TMS_FIRST_PLAY_SESSION, &icon_ms_first_play_session }, { TMS_CHANGED_SOMEONE, &icon_ms_tank_changed_someone },
    { TMS_FULL_SCHOOL, &icon_ms_full_school },        /* 2026-09-30: only once the shrimp are bought (tank_badge_shown) */
    { TMS_FIRST_REEF, &icon_ms_first_reef },          /* 2026-10-02: only once the reef cluster is bought */
    { TMS_FIRST_MUTATION, &icon_ms_first_mutation },  /* 0.4 (Strato's helix): only once the tank has bred - a trio */
    { TMS_FIRST_SALE, &icon_ms_first_sale },          /* 0.3.5: a fish sold - the shop's fry */
    { TMS_SEAHORSE_FED, &icon_ms_seahorse_fed },      /* 0.3.6: only once the seahorse is bought (Strato: "visible and gray once purchased") */
    { TMS_CRAB_FIND, &icon_ms_crab_find },            /* 0.3.9: only once the crab is bought - its first sand dollar, taken */
};
static bool tank_badge_shown(const tank_t *t, int i) {
    uint32_t bit = TANK_BADGES[i].bit;
    if (bit == TMS_FULL_SCHOOL) return (t->sd_unlocks & SD_ITEM_SHRIMP)  || (t->tank_ms_bits & bit);
    if (bit == TMS_FIRST_REEF)  return (t->sd_unlocks & SD_ITEM_CLUSTER) || (t->tank_ms_bits & bit);
    if (bit == TMS_FIRST_MUTATION) return (t->tank_ms_bits & (TMS_TRIO | bit)) != 0;
    if (bit == TMS_FIRST_SALE)  return (t->tank_ms_bits & bit) != 0;   /* 0.3.5: once earned (its notice says what it opened) */
    if (bit == TMS_SEAHORSE_FED) return (t->sd_unlocks & SD_ITEM_SEAHORSE) || (t->tank_ms_bits & bit);
    if (bit == TMS_CRAB_FIND)   return (t->sd_unlocks & SD_ITEM_CRAB)     || (t->tank_ms_bits & bit);
    return true;
}
/* the TANK row's word */
static const char *tank_word(const tank_t *t) { (void)t; return "TANK"; }
/* the shop: pocket-tank's items, the pumpkin (the last but one) only while this build sells it (or owned) */
static const icon_t *shop_icon(int item) {
    return item == 0 ? &icon_shop_plant : item == 1 ? &icon_shop_snail : item == 2 ? &icon_shop_castle : item == 3 ? &icon_shop_coral : item == 4 ? &icon_shop_cluster : item == 5 ? &icon_shop_shrimp : item == 6 ? &icon_shop_urchin : item == 7 ? &icon_shop_seahorse : item == 8 ? &icon_shop_pumpkin : item == 9 ? &icon_shop_volcano : &icon_shop_crab;
}
static int shop_count(const tank_t *t) { return SD_ITEM_COUNT - !progression_pumpkin_offered(t); }   /* the pumpkin, the last item, only while this build sells it (or owned) */
static int shop_item(const tank_t *t, int i) {                  /* the pumpkin's row (8) is skipped while it is out of the shop */
    if (i >= 8 && !progression_pumpkin_offered(t)) i++;
    return i;
}
/* the toolbox: the sponge and the scissors, as pocket-tank's, their icons at 2x in halves. The fish do not shed: no mister in their
   box (tank_set_tool refuses it too) */
static const int TOOLS[] = { TOOL_SPONGE, TOOL_SCISSORS };

const world_view_t aquarium_view = {
    .name        = "aquarium",
    .bodies      = BODIES, .n_bodies = 1,
    .render      = aquarium_render,
    .scene_alt   = aquarium_scene_alt,
    .critter_here = aquarium_critter_here, .critter_step = aquarium_critter_step, .critter_nav_hit = aquarium_critter_nav_hit,
    .critter_card = (void (*)(ctx_t *, const tank_t *, int))aquarium_critter_card, .crab_collect_hit = aquarium_crab_collect_hit,
    .shelf_prepare = aquarium_shelf_prepare, .shelf_thumb = aquarium_shelf_thumb, .shelf_note = aquarium_shelf_note, .shelf_leave = aquarium_shelf_leave,
    .algae_rgb   = { 0x3f7a45, 0x35663d },                      /* algae's greens */
    .tools       = TOOLS, .n_tools = 2,
    .fish_badges = FISH_BADGES,
    .tank_badges = TANK_BADGES, .n_tank_badges = TANK_BADGE_N, .tank_badge_shown = tank_badge_shown,
    .tank_word   = tank_word,
    .shop_icon   = shop_icon, .shop_count = shop_count, .shop_item = shop_item,
    .text = {
        .reset_title = "RESET TANK?",
        .reset_line = "START OVER WITH TWO FRY",
        .new_arrival = "NEW FRY",
        .worth_fmt = "WORTH %d SAND DOLLARS",
        .keep_two = "KEEP AT LEAST TWO FISH",
        .name_first = "NAME THE NEW FRY FIRST",
        .steps_done = "EVERY STEP IS DONE. A FRY",
        .born_where = "WILL BE BORN IN THE GRASS",
        .needs_met = "WHEN ALL NEEDS ARE MET, A",
        .ready_to_be_born = "NEW FRY IS READY TO BE BORN",
        .so_far_fmt = "%d OF %d FISH SO FAR",
        .the_tank = "THE TANK",
        .the_tank_waits = "THE TANK WAITS FOR IT",
        .animals_safe = "YOUR FISH ARE SAFE",
        .now_version_fmt = "YOUR TANK IS NOW V%s",
        .please_charge = "PLEASE CHARGE THE TANK",
        .newborns_in_shop = "FRY NOW IN THE SHOP",
        .more_in_h_fmt = "NEW FRY IN %d H",
        .more_in_min_fmt = "NEW FRY IN %d MIN",
        .newborn = "FRY",
        .newborns_for_sale = "FRY FOR SALE",
        .tank_full = "THE TANK IS FULL",
        .welcome_newborn = "WELCOME THE NEW FRY",
        .coins = "SAND DOLLARS",
        .in_the_tank = "IN THE TANK",
        .in_tank_short = "IN TANK",
        .charge_fmt = "THE TANK WILL CHARGE AT %d MA.",
        .hold_the_tank = "HOLD THE TANK THE WAY",
        .my_tanks = "MY TANKS",
        .rename_tank = "RENAME THIS TANK",
        .swap_in = "SWAP IN ANOTHER TANK",
        .gone_l1 = "THIS TANK AND ITS FISH",
        .gone_l2 = "ARE GONE FOR GOOD.",
        .filling = "FILLING THE TANK",
        .new_tank = "NEW TANK",
    },
    .art = {
        .hunger = &icon_hunger,
        .shy = &icon_shy,
        .bold = &icon_bold,
        .solo = &icon_solo,
        .social = &icon_social,
        .cautious = &icon_cautious,
        .curious = &icon_curious,
        .foot_my_tanks = &icon_foot_my_tanks,
        .foot_coins = &icon_foot_sand_dollar,
        .req_feed = &icon_ms_first_feeding,
        .req_glass = &icon_ms_first_glass_cleaning,
        .req_hold = &icon_ms_first_hold_approach,
        .req_change = &icon_ms_tank_changed_someone,
        .coin16 = &icon_shop_sand_dollar_16,
        .coin64 = &icon_shop_sand_dollar_64,
    },
    .bowl_title_s = 3,
    .look        = { .bg = { 0x0e3a4c, 0x04121a },              /* deep water, a little lighter at the top */
                     .rim = { 0xe2f6fa, 0x9fd8e2, 0x5f9fae, 0x2c5a66 }, .rim_dark = 0x0a2a36 },
};
const world_view_t *world_view(void) { return &aquarium_view; }
