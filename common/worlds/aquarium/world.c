/* worlds/aquarium/world.c - THE AQUARIUM's descriptor (issue #4 phase 3): pocket-tank's fish, swimming. The engine files
 * (progression.c, setup.c, notice.c, shelf.c, boot_show.c, advisor.c) ask it through world_cur() (world_ops.h); what it
 * points at lives beside it: milestones.c, shop.c, fry.c, rules.c, setup.c. */
#include "world_ops.h"
#include "setup_world.h"

goal_t   aquarium_rules(const tank_t *t, int animal, bool request);
uint32_t aquarium_unlock_bits(void);
bool     aquarium_can_buy(int item);
void     aquarium_item_bought(tank_t *t, const sd_item_t *it, int item);
int      aquarium_earn_lines(char (*lines)[30], int max);
int      aquarium_care_gates(const tank_t *t, const care_ctx_t *c, care_gate_t g[CARE_GATES_MAX]);
const char *const *aquarium_fry_tip(int kind);
int      aquarium_fry_extra(const tank_t *t, fry_req_t *out);
bool     aquarium_nursery_ready(const tank_t *t);
bool     aquarium_court_ok(const tank_t *t);
void     aquarium_arrival_place(tank_t *t, int slot);
void     aquarium_scan_fish(const tank_t *t, const fish_t *f, world_scan_t *out);
uint32_t aquarium_scan_tank(const tank_t *t);
void     aquarium_badges_sync(const tank_t *t);
bool     aquarium_badges_moved(const tank_t *t);
void     aquarium_restore(tank_t *t, const struct save_s *s);
bool     aquarium_dish_here(const tank_t *t);

static void aquarium_light_on(tank_t *t) { (void)t; }                       /* (no farm: the terrarium's) */
static void aquarium_slept(tank_t *t, float seconds) { (void)t; (void)seconds; }

/* never announced: they belong to the birth of a tank or a fish (MS_INSPECTED: no badge since 0.3.0) */
#define AQ_MS_SILENT  (MS_ARRIVED | MS_INSPECTED | MS_RETIRED_6 | MS_RETIRED_7 | MS_CROSSBRED)
#define AQ_TMS_SILENT (TMS_PAIR)

static const world_t AQUARIUM = {
    .id = PT_WORLD_AQUARIUM, .name = "aquarium",
    .rules = aquarium_rules, .goal_friend = GOAL_FOLLOW_FRIEND,
    .unlock_bits = aquarium_unlock_bits, .can_buy = aquarium_can_buy, .item_bought = aquarium_item_bought, .earn_lines = aquarium_earn_lines,
    .arrival = ARRIVAL_COURT,
    .care_gates = aquarium_care_gates, .fry_tip = aquarium_fry_tip, .fry_extra = aquarium_fry_extra,
    .nursery_ready = aquarium_nursery_ready, .court_ok = aquarium_court_ok, .arrival_place = aquarium_arrival_place,
    .light_on = aquarium_light_on, .slept = aquarium_slept, .restore = aquarium_restore, .dish_here = aquarium_dish_here,
    .scan_fish = aquarium_scan_fish, .scan_tank = aquarium_scan_tank,
    .badges_sync = aquarium_badges_sync, .badges_moved = aquarium_badges_moved,
    .ms_silent = AQ_MS_SILENT, .tms_silent = AQ_TMS_SILENT,
    .words = {
        .stage_fry = "A FRY",
        .trust_words = "ALL FISH MUST HAVE TRUST", .trust_done = "EVERY FISH DOES",
        .feed_words = "FEED THE FISH AT LEAST",
        .hold_words2 = "UNTIL A FISH SWIMS TO IT",
        .grow_words = "THE YOUNGEST FISH MUST", .shed_words = NULL,
        .shelf_first = "MY TANK", .shelf_nth = "TANK %u", .shelf_nth_scan = "TANK %u%c",
    },
    .setup = &aquarium_setup,
};

const world_t *world_cur(void) { return &AQUARIUM; }
