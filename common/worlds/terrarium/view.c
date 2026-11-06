/* view.c - THE TERRARIUM's descriptor for the renderer (world_view.h): the scene's hooks and the pages' choices. */
#include "world_view.h"
#include "worlds/terrarium/scene.h"
#include "icons.h"
#include "progression.h"

extern const body_ops_t lizard_body;                            /* bodies/lizard/body.c */
static const body_ops_t *const BODIES[] = { &lizard_body };

/* THE TERRARIUM's badges (P5, progression.c MS_NAMES / TMS_NAMES): a lizard's row - a meal from the keeper, the hand, a
   cricket caught, a bask, a clean shed, a dig; the terrarium's row - a quiet night, a stuck shed fixed, CLEAN_RUN_N clean
   sheds in a row, the first adult, a dig together, a wiped glass, a change of heart, then the first sale once earned.
   The fish's other badges (the seagrass, a follow, a feeding, a trim, the bought creatures', a mutation) are never shown
   here: notice.c keeps them silent too */
static const badge_t FISH_BADGES[6] = {
    { MS_FIRST_MEAL_FROM_YOU, &icon_ms_first_meal_terr },  { MS_FIRST_HOLD_APPROACH, &icon_ms_first_hold_approach_terr },
    { MS_FIRST_CATCH, &icon_ms_first_feeding_terr },       { MS_FIRST_BUBBLES, &icon_ms_first_bubbles_terr },   /* crickets; a bask */
    { MS_FIRST_SHED, &icon_ms_first_shed_terr },           { MS_FIRST_DART, &icon_ms_first_dart_terr },         /* the shed skin; a dig */
};
static const badge_t TANK_BADGES[8] = {
    { TMS_FIRST_FULL_NIGHT, &icon_ms_first_quiet_night_terr }, { TMS_SHED_HELPED, &icon_ms_shed_helped_terr },     /* the mister on a gecko */
    { TMS_CLEAN_RUN, &icon_ms_clean_run_terr },             { TMS_FIRST_ADULT, &icon_ms_first_adult_terr },       /* three skins; a big one */
    { TMS_FIRST_PLAY_SESSION, &icon_ms_first_play_session_terr }, { TMS_FIRST_CLEANING, &icon_ms_first_glass_cleaning_terr },
    { TMS_CHANGED_SOMEONE, &icon_ms_tank_changed_someone_terr }, { TMS_FIRST_SALE, &icon_ms_first_sale_terr },   /* (once earned) */
};
/* the tank row shows them all but the first sale's, which only once it is earned (its notice says what it opened) */
static bool tank_badge_shown(const tank_t *t, int i) {
    if (TANK_BADGES[i].bit == TMS_FIRST_SALE) return (t->tank_ms_bits & TANK_BADGES[i].bit) != 0;
    return true;
}
/* the clean-shed gate's own art (the keeper page's fry checklist) */
static const icon_t *req_icon(int kind) { return kind == FRY_REQ_SHED ? &icon_ms_first_shed_terr : NULL; }
/* the TANK row's word: its habitat's name */
static const char *tank_word(const tank_t *t) { return tank_habitat(t)->name; }
/* the shop: its own items (progression.h SD_TERR_FIRST): the moss for the humid hide, the basking bulb, the worm farm, the decorations */
static const icon_t *shop_icon(int item) {
    if (item == SD_TERR_FIRST) return &icon_shop_moss_terr;
    if (item == SD_TERR_FIRST + 1) return &icon_shop_bulb_terr;
    if (item == SD_TERR_FIRST + 2) return &icon_shop_farm_terr;
    if (tank_deco_index(item) >= 0) {                          /* the decorations (P5b) */
        static const icon_t *const SHOP[TERR_DECO_N] = { &icon_shop_saguaro_terr, &icon_shop_skull_terr, &icon_shop_driftwood_terr, &icon_shop_pot_terr };
        return SHOP[tank_deco_index(item)];
    }
    return &icon_shop_moss_terr;
}
static int shop_count(const tank_t *t) { (void)t; return SD_TERR_COUNT; }
static int shop_item(const tank_t *t, int i) { (void)t; return SD_TERR_FIRST + i; }
/* the toolbox: the sponge, the scissors and the mister (the icons at 1x to fit); a fish does not shed, a gecko does */
static const int TOOLS[] = { TOOL_SPONGE, TOOL_SCISSORS, TOOL_MISTER };

const world_view_t terrarium_view = {
    .name        = "terrarium",
    .bodies      = BODIES, .n_bodies = 1,
    .render      = terrarium_render,
    .scene_reset = terrarium_scene_reset,
    .algae_rgb   = { 0x9c8f7c, 0x857864 },                      /* a dusty smudge */
    .tools       = TOOLS, .n_tools = 3,
    .fish_badges = FISH_BADGES,
    .tank_badges = TANK_BADGES, .n_tank_badges = 8, .tank_badge_shown = tank_badge_shown,
    .req_icon    = req_icon, .tank_word = tank_word,
    .shop_icon   = shop_icon, .shop_count = shop_count, .shop_item = shop_item,
    .text = {
        .reset_title = "RESET TERRARIUM?",
        .reset_line = "START OVER: TWO HATCHLINGS",
        .new_arrival = "NEW EGG",
        .worth_fmt = "WORTH %d DESERT ROSES",
        .keep_two = "KEEP AT LEAST TWO LIZARDS",
        .name_first = "NAME THE HATCHLING FIRST",
        .steps_done = "EVERY STEP IS DONE. AN EGG",
        .born_where = "HATCHES IN THE HUMID HIDE",
        .needs_met = "WHEN ALL NEEDS ARE MET, AN",
        .ready_to_be_born = "EGG HATCHES IN THE HIDE",
        .so_far_fmt = "%d OF %d LIZARDS SO FAR",
        .the_tank = "THE TERRARIUM",
        .the_tank_waits = "THE TERRARIUM WAITS FOR IT",
        .animals_safe = "YOUR LIZARDS ARE SAFE",
        .now_version_fmt = "THIS IS NOW V%s",
        .please_charge = "PLEASE CHARGE THE TERRARIUM",
        .newborns_in_shop = "HATCHLINGS NOW IN THE SHOP",
        .more_in_h_fmt = "MORE IN %d H",
        .more_in_min_fmt = "MORE IN %d MIN",
        .newborn = "HATCHLING",
        .newborns_for_sale = "HATCHLINGS",
        .tank_full = "THE TERRARIUM IS FULL",
        .welcome_newborn = "WELCOME THE HATCHLING",
        .coins = "DESERT ROSES",
        .in_the_tank = "IN THE TERRARIUM",
        .in_tank_short = "OWNED",
        .charge_fmt = "IT WILL CHARGE AT %d MA.",
        .hold_the_tank = "HOLD THE TERRARIUM THE WAY",
        .my_tanks = "MY TERRARIUMS",
        .rename_tank = "RENAME THIS TERRARIUM",
        .swap_in = "SWAP IN ANOTHER TERRARIUM",
        .gone_l1 = "THIS TERRARIUM AND ITS",
        .gone_l2 = "LIZARDS ARE GONE FOR GOOD.",
        .filling = "SETTING UP THE TERRARIUM",
        .new_tank = "NEW ONE",
    },
    .art = {
        .hunger = &icon_hunger_terr,
        .shy = &icon_shy_terr,
        .bold = &icon_bold_terr,
        .solo = &icon_solo_terr,
        .social = &icon_social_terr,
        .cautious = &icon_cautious_terr,
        .curious = &icon_curious_terr,
        .foot_my_tanks = &icon_foot_my_tanks_terr,
        .foot_coins = &icon_foot_sand_dollar_terr,
        .req_feed = &icon_ms_first_feeding_terr,
        .req_glass = &icon_ms_first_glass_cleaning_terr,
        .req_hold = &icon_ms_first_hold_approach_terr,
        .req_change = &icon_ms_tank_changed_someone_terr,
        .coin16 = &icon_shop_sand_dollar_16_terr,
        .coin64 = &icon_shop_sand_dollar_64_terr,
    },
    .bowl_title_s = 2,                                          /* MY TERRARIUMS: in the round glass's narrow cap, beside the padlock, a size down */
    .look        = { .bg = { 0x4a3020, 0x140c07 },              /* a desert dusk; its bubbles are sand pebbles */
                     .rim = { 0xffe6b8, 0xe9b98c, 0xb07e52, 0x5e3e28 }, .rim_dark = 0x2a180c },
};
const world_view_t *world_view(void) { return &terrarium_view; }
