/* worlds/terrarium/world.c - THE TERRARIUM's descriptor (pocket-terrarium, issue #4 phase 3): the lizards, walking. The
 * engine files (progression.c, setup.c, notice.c, shelf.c, boot_show.c, advisor.c) ask it through world_cur() (world_ops.h);
 * what it points at lives beside it: milestones.c, shop.c, fry.c, rules.c, setup.c. */
#include "world_ops.h"
#include "setup_world.h"

goal_t   terrarium_rules(const tank_t *t, int animal, bool request);
uint32_t terrarium_unlock_bits(void);
bool     terrarium_can_buy(int item);
void     terrarium_item_bought(tank_t *t, const sd_item_t *it, int item);
int      terrarium_earn_lines(char (*lines)[30], int max);
int      terrarium_care_gates(const tank_t *t, const care_ctx_t *c, care_gate_t g[CARE_GATES_MAX]);
const char *const *terrarium_fry_tip(int kind);
int      terrarium_fry_extra(const tank_t *t, fry_req_t *out);
bool     terrarium_nursery_ready(const tank_t *t);
bool     terrarium_court_ok(const tank_t *t);
void     terrarium_arrival_place(tank_t *t, int slot);
void     terrarium_scan_fish(const tank_t *t, const fish_t *f, world_scan_t *out);
uint32_t terrarium_scan_tank(const tank_t *t);
void     terrarium_badges_sync(const tank_t *t);
bool     terrarium_badges_moved(const tank_t *t);
void     terrarium_restore(tank_t *t, const struct save_s *s);
bool     terrarium_dish_here(const tank_t *t);

/* the MEALWORM FARM's morning (P5): at a light-on, and after a sleep this long (owned or not, tank.c decides) */
#define FARM_NIGHT_S 3600.0f
static void terrarium_light_on(tank_t *t) { if (t->sd_unlocks & SD_ITEM_FARM) tank_dish_farm(t); }
static void terrarium_slept(tank_t *t, float seconds) { if (seconds >= FARM_NIGHT_S) tank_dish_farm(t); }

/* never announced: they belong to the birth of a tank or a fish - and the fish's follow and seagrass, which are no badge here
   (tank.h: FOLLOW still reveals the card's slider); and the tank bits that are no badge on its row (P5): the first feeding
   (the first meal from you says it), the trimming, the fish's bought creatures, a mutation (the birth page names a new morph) */
#define TERR_MS_SILENT  (MS_ARRIVED | MS_INSPECTED | MS_RETIRED_6 | MS_RETIRED_7 | MS_CROSSBRED | MS_FIRST_FOLLOW | MS_FIRST_GRASS)
#define TERR_TMS_SILENT (TMS_PAIR | TMS_FIRST_FEEDING | TMS_FIRST_TRIM | TMS_FIRST_MUTATION | TMS_FULL_SCHOOL | TMS_FIRST_REEF | TMS_SEAHORSE_FED | TMS_CRAB_FIND)

static const world_t TERRARIUM = {
    .id = PT_WORLD_TERRARIUM, .name = "terrarium",
    .rules = terrarium_rules, .goal_friend = GOAL_LZ_INVESTIGATE,
    .unlock_bits = terrarium_unlock_bits, .can_buy = terrarium_can_buy, .item_bought = terrarium_item_bought, .earn_lines = terrarium_earn_lines,
    .arrival = ARRIVAL_HATCH,
    .care_gates = terrarium_care_gates, .fry_tip = terrarium_fry_tip, .fry_extra = terrarium_fry_extra,
    .nursery_ready = terrarium_nursery_ready, .court_ok = terrarium_court_ok, .arrival_place = terrarium_arrival_place,
    .light_on = terrarium_light_on, .slept = terrarium_slept, .restore = terrarium_restore, .dish_here = terrarium_dish_here,
    .scan_fish = terrarium_scan_fish, .scan_tank = terrarium_scan_tank,
    .badges_sync = terrarium_badges_sync, .badges_moved = terrarium_badges_moved,
    .ms_silent = TERR_MS_SILENT, .tms_silent = TERR_TMS_SILENT,
    .words = {
        .stage_fry = "A HATCHLING",
        .trust_words = "EVERY LIZARD NEEDS TRUST", .trust_done = "EVERY LIZARD DOES",
        .feed_words = "FEED THE LIZARDS AT LEAST",
        .hold_words2 = "UNTIL A LIZARD WALKS TO IT",
        .grow_words = "THE YOUNGEST LIZARD MUST", .shed_words = "THE LIZARDS MUST SHED",
        .shelf_first = "MY DESERT", .shelf_nth = "DESERT %u", .shelf_nth_scan = "DESERT %u%c",
    },
    .setup = &terrarium_setup,
};

const world_t *world_cur(void) { return &TERRARIUM; }
