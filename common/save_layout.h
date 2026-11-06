/* save_layout.h - THE SAVE LAYOUT (moved verbatim out of progression.c, issue #4 phase 3): save_t and fish_save_t, and the
 * SAVE LAYOUT LOCK that freezes them. Shared by progression.c (which reads and writes the save) and the worlds' restore hooks
 * (world_ops.h: common/worlds/<world>/restore.c put back what only that world has - the aquarium's pieces and creatures - from
 * the save it is handed). A new field goes at the END of save_t, never anywhere else (the lock below says so, field by field). */
#ifndef SAVE_LAYOUT_H
#define SAVE_LAYOUT_H
#include "tank.h"
#include "shelf.h"        /* SHELF_SAVE_MAX: the save's budget */
#include "passport.h"
#include "progression.h"  /* SAVE_NVS_NS / SAVE_NVS_KEY */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SAVE_MAGIC 0x50544b32u   /* "PTK2" (PTK1 saves are 4-fish, pre-population: start fresh) */

/* the save keeps 644 film cells - the rectangle's 28 x 23 grid, frozen by the
 * layout lock below. A build with another grid (the bowl's 26 x 22) saves its
 * own cells into the front of the same array: the layout never moves. */
#define ALGAE_SAVE 644
_Static_assert(ALGAE_CELLS <= ALGAE_SAVE, "the film grid outgrew the 644 cells the save keeps");

typedef struct {
    uint8_t preset, stage; uint16_t pad;
    float size, trust, bold, sociable, bold0, sociable0, hunger, energy, stress, curiosity;
    float age_s, rest_dx, rest_dy;
    int32_t eaten, eaten_player;
    uint32_t ms_bits;
} fish_save_t;

typedef struct save_s {
    uint32_t magic;
    int64_t  saved_unix;
    float    clock;
    bool     light_override, light_on, arrival_pending;
    uint8_t  n_fish;
    float    feed_spot_x;
    int32_t  player_feedings, hold_approaches;
    uint32_t tank_ms_bits;
    fish_save_t fish[N_FISH_MAX];
    /* ---- upkeep tail (2026-08-30). Fields only ever APPEND here: boot falls
     * back to loading the prefix above from an older PTK2 save; the zeroed
     * tail then reads as clean glass and default vegetation (0 is not a legal
     * growth - the floor is VEG_NUB - so restore treats it as "keep the
     * fresh-tank default"). ---- */
    float    veg_growth[VEG_BEDS];
    uint8_t  algae[ALGAE_SAVE];
    int32_t  trims, cells_cleaned;
    /* per-frond heights (2026-09-04); an older save (no tail, or zeros)
     * seeds every frond from its bed's veg_growth */
    float    veg_h[VEG_BEDS][VEG_FRONDS_MAX];
    /* identity tail (2026-09-13, first-run setup): the keeper's names and
     * colours per fish - an empty name / a zero colour = the preset's - and
     * whether the setup flow still owes the keeper a visit. Older saves read
     * zeros: preset looks, setup done. */
    uint8_t  setup_pending, pad_id[3];
    char     names[N_FISH_MAX][FISH_NAME_MAX + 1];
    uint32_t body[N_FISH_MAX], accent[N_FISH_MAX];
    float    bubble_x;                   /* the keeper's bubble column (0 = the default spot) */
    /* seen-milestones tail (2026-09-13, the milestones page): what the
     * keeper has already looked at, so a badge earned since wears a ring.
     * Older saves read zeros: everything earned shows as new once. */
    uint32_t ms_seen[N_FISH_MAX], tank_ms_seen;
    /* family tail (2026-09-14, the birth flow): each fish's parents and the
     * arrival still owed its welcome, all as slot + 1 so an older save's
     * zeros read as "none". */
    uint8_t  newborn_p1, parent_p1[N_FISH_MAX][2], pad_fam[3];
    /* drift-pressure tail (2026-09-15, the CHANGE gate): older saves read
     * zeros, and the youngest simply starts earning it from this build on */
    float    drift_acc[N_FISH_MAX];
    /* light settings tail (2026-09-15, the settings page): 0 seconds = the
     * default (older saves); auto 0 = MANUAL, the double-tap (the default) */
    uint16_t light_idle_s; uint8_t light_auto, light_manual_off;   /* auto 0 = MANUAL, the default */
    /* sand dollar tail (2026-09-15, the shop): the balance, the lifetime
     * total, the unlocks, the paid ledger, the two chore counters, the snail's
     * spot and the sword plant's leaves. Older saves read zeros: no dollars,
     * nothing bought, nothing paid - and the ledger then pays what the tank
     * already earned on the first tick, once. */
    int32_t  sd_balance, sd_earned;
    uint32_t sd_unlocks;
    uint32_t sd_paid_fish[N_FISH_MAX];
    int32_t  sd_colonies_paid, sd_inches_paid;
    int32_t  algae_colonies;
    float    trim_px;
    float    snail_x, snail_y;           /* 0 = not placed yet */
    float    veg_h3[VEG_FRONDS_MAX];     /* bed 3: zeros = VEG_START when it is bought */
    /* placement tail (2026-09-16, the placement page): the plant's centre x
     * (0 = the default spot) and its layer + 1 (0 = MIDDLE, an older save) */
    float    plant_x;
    uint8_t  plant_z1, pad_place[3];
    /* the snail's tally (2026-09-16, its card): cells grazed clean, lifetime.
     * Older saves read 0 - it starts counting from this build on. */
    int32_t  snail_grazed;
    /* the castle's spot (2026-09-16): its centre x (0 = the default) and its
     * layer + 1 (0 = FRONT, an older save or one that never placed it) */
    float    castle_x;
    uint8_t  castle_z1, pad_castle[3];
    /* the coral (2026-09-23): its centre x (0 = the default), its layer + 1
     * (0 = MIDDLE, a save that never placed it) and its colour (0 = the
     * palette's first). Older saves read zeros: no coral until it is bought. */
    float    coral_x;
    uint8_t  coral_z1, pad_coral[3];
    uint32_t coral_rgb;
    float    coral_growth;               /* CORAL_START..1 (0 = a save from before it grew: full) */
    /* the reef cluster (2026-09-24): its centre x (0 = the default), its
     * layer + 1, its look, its growth (0 = full). Older saves: no cluster. */
    float    cluster_x;
    uint8_t  cluster_z1, cluster_scheme;
    /* the fry for sale (0.3.5): the offer's seed (0 = none drawn yet). In
     * the cluster's padding, zeros in every older save; free on main. */
    uint16_t fry_seed;
    float    cluster_growth;
    /* the shrimp school (2026-09-29): how many (0 = an older save, or none
     * bought), the pellets eaten toward the next, the seconds before another
     * may join. Positions are not saved: a load puts them in the grass. */
    uint8_t  shrimp_n, shrimp_food;
    uint16_t fry_age_min;                /* the fry offer's age, tank minutes (0.3.5, the shrimp's padding) */
    float    shrimp_cool;
    int32_t  shrimp_eaten;               /* pellets eaten, lifetime (its card): in the tail's padding, so still 1664 B */
    /* the release that wrote it (2026-09-29, version.h PT_RELEASE_NUM:
     * 0x000200 = 0.2.0); 0 = a build from before release numbers */
    uint32_t saved_release;
    /* the meals a tank that sold a fish owes before its next fry (2026-10-01,
     * progression_sell_fish): player_feedings must reach this. 0 = no fish
     * was ever sold (every older save; it sat in the struct's tail padding,
     * which every build wrote as zeros - still 1672 B). */
    int32_t  sale_meals_need;
    /* a worn tank's way up (2026-10-02, the watch: tank_screen_*): settings
     * SCREEN, 1 = TURNED. Older saves (and every other board) read 0: NORMAL. */
    uint8_t  screen_turned;
    /* 0.3.2's two settings, in what was screen_turned's padding (every build
     * wrote it as zeros, so older saves read the defaults; still 1688 B, and
     * nothing moves for 0.4's tails after it): orient - bit 0 = ROTATION
     * locked, bit 1 = the locked way up is turned over (tank_orient);
     * autofeed_off - 1 = AUTO FEED off. */
    uint8_t  orient, autofeed_off, pad_screen;
    /* the urchin (2026-10-02): its x on the floor (0 = not placed yet: it
     * starts by the reef bed) and the grass it has eaten, lifetime, in the
     * trim's px (its card). Older saves read zeros. */
    float    urchin_x;
    float    urchin_grazed_px;
    /* the lights-out notice (2026-10-03): 1 = a double-tap has turned the
     * light off once and the keeper was told how. In the tail's padding,
     * which every build wrote as zeros - still 1688 B; older saves read 0
     * and get the notice once. */
    uint8_t  light_tip_seen;
    /* the toolbox's LOCK (0.3.5): 1 = bare fingers never wipe or cut. In
     * light_tip_seen's padding, which every build wrote as zeros - still
     * 1688 B; older saves read 0, unlocked. */
    uint8_t  tools_locked;
    uint8_t  fry_sold, pad_tip;          /* the fry offer's blanks, bit k = fry k bought (0.3.5) */
    /* the genes (0.4, heredity): per fish, [GS_HAS] 1 = written by 0.4+ (an
     * older save reads 0: its fish keep the look they have - every one
     * CLASSIC / CLASSIC / BARS - and the copies they carry are rolled once,
     * at that load), the shown shape / fins / pattern, the five carried
     * copies (GENE_*), the gene that mutated at its birth + 1, and the
     * from_b / from_carried bits. The shown colours stay in body / accent. */
    uint8_t  genes[N_FISH_MAX][12];
    /* identity (0.4, the tank link's layout): the tank's ID, each fish's, and
     * its two parents'. Zero = not saved (a pre-0.4 save): the load hands out
     * fresh IDs and takes the parents' from their slots (tank_ensure_ids) -
     * a parent sold before that first load is a slot no more, so its child's
     * parent_uid stays 0. */
    uint32_t tank_uid, fish_uid[N_FISH_MAX], parent_uid[N_FISH_MAX][2];
    /* who each fish is (0.4, 2026-10-05), so the genes and IDs above find
     * their fish after a rollback: a hash of what a fish is for life - its
     * kind and its birth temper (fish_print: preset, bold0, sociable0, set
     * by tank_make_fish and never changed; a fry takes a kind no living fish
     * has). An OLDER build keeps this tail and writes its own head under it
     * (newer_keep): it may sell a fish (the rest move down a slot) or breed a
     * fry into a slot whose tail is a sold fish's, so the load matches tail
     * to fish by print, not by slot (tail_slot). 0 = not saved (a 0.4 dev
     * save from before the prints, or an empty slot): that slot's tail is
     * taken as its fish's, as it was written. */
    uint32_t fish_print[N_FISH_MAX];
    /* the tank link (0.4, docs/tank-link-0.4.md): bit i = fish i was next door
     * when this was saved. Read back through tail_slot like the genes; a boot
     * that finds one brings it home from bubbles LINK_RESTORE_MS later (the
     * host may still hold it on its last lease). */
    uint8_t  link_away, pad_link[3];
    /* 0.3.6 wrote 176 zero bytes here (1688..1863) and put the seahorse at
     * 1864: the 0.4 tail above is exactly that span, so a 0.3.x save loads
     * here as a tank without genes (as every older save does) and a 0.4
     * save loads on 0.3.x with its tail kept (the rollback below). */
    /* the seahorse (0.3.6): its seconds with the keeper, awake and asleep
     * (its card: days), its hitches and the trust it passed on. Older saves
     * (and main's) read zeros: a fresh count, which is right - they had none. */
    float    sh_s;
    int32_t  sh_hitches;
    float    sh_trust_passed;
    /* its snacks (0.3.6, later the same day): the count, the earning count,
     * the pay ledger, the cooldown. sh_eaten took pad_sh's place (zeros in
     * every 1880 save: a fresh count); the rest grew the save to 1892. */
    int32_t  sh_eaten;
    int32_t  sh_snack_pay, sd_snacks_paid;
    float    sh_snack_cool;
    /* the pumpkin lantern (0.3.6, the seasonal piece): its spot (0 = the
     * default) and its depth + 1 (0 = never placed: BEHIND). Older saves
     * read zeros. */
    float    pumpkin_x;
    uint8_t  pumpkin_z1, pad_pk[3];
    /* the order of the pieces (0.3.6, later the same day): a rank per slot
     * (castle, coral, cluster, pumpkin, plant). Older saves read zeros: the
     * old fixed order. */
    uint8_t  decor_rank[5], pad_rank[3];
    /* the decoration shelf (0.3.9): the owned placeable pieces put away.
     * Older saves read zero (everything in the tank); an older firmware
     * ignores it and the shelved piece stands in the tank at its kept spot. */
    uint32_t sd_shelved;
    /* the bubble volcano (0.3.9): its spot (0 = the default), its depth + 1
     * (0 = never placed: BEHIND), its rank among the pieces (the sixth slot:
     * decor_rank above holds five). Older saves read zeros. */
    float    volcano_x;
    uint8_t  volcano_z1, volcano_rank, pad_vo[2];
    /* LOCKED pieces (0.3.9): the owned placeable pieces a held finger passes
     * by. Older saves read zero (nothing locked); an older firmware ignores it. */
    uint32_t sd_locked;
    /* the strawberry crab (0.3.9): its x on the floor (0 = not placed yet) and
     * its seed pattern, and the pellets it has found (its card). Older saves
     * read zeros: a crab at its first spot, pattern 0, none found. */
    float    cr_x;
    uint8_t  cr_variant, pad_cr;
    uint16_t cr_eaten;
    uint16_t cr_found, cr_found_paid;   /* its sand dollar finds, and the ledger's count of them (0.3.9, in the old padding: sizeof stays 1936) */
    /* the crab's wait for its next find (0.3.9.1): seconds of lived time + 1,
     * so an older save's zero reads "never written" - the first-find window
     * again, a find soon for every keeper who updates. Until then a boot
     * started the wait again at 2-4 h of awake time. */
    uint32_t cr_find_wait;
    /* the world this tank was laid out in (0.3.9.3): its glass and its floor's
     * span, in its own px, and its film grid. A tank that moves to another
     * board (that board's version installed over the 1.8's, which runs on
     * every board) is laid out again in proportion: a castle 80% of the way
     * across stays 80% across. Older saves read zeros (see load_save). */
    uint16_t lay_w, lay_h, lay_fx0, lay_fx1;
    uint8_t  lay_cols, lay_rows, pad_lay[2];
    /* the seahorse's time with the keeper in whole seconds + 1 (0.4; 0 = an older save): sh_s at 1864 is a float,
     * which stops adding a frame at ~12 days - it is still written, for a rollback to read */
    uint32_t sh_secs;
    /* the packs (pocket-terrarium P2a: species.h, habitat.h, docs/shed.md): pack_v 1 = this tail is written (an
     * older save's zeros: a new tank's defaults - the default species in the default habitat, comfortable bodies,
     * a fresh shed clock per fish, the humid hides at the habitat's start); the species' and the habitat's ids
     * (a pair that does not fit, or an id this build lacks: the defaults); per fish SLOT the shed's state, whether
     * it is a growth shed, its clean sheds, the body's heat (C), the shed's seconds (tank.h fish_t.shed_s) and
     * its damp seconds; each of the habitat's hides' moisture, by index; and the prints of the fish this tail
     * was written for (pack_slot): an older build that writes fish_print itself (0.4, this port's P0/P1) may
     * sell or breed under this tail, and its prints are then of its own order, not this tail's */
    uint8_t  pack_v, pack_id, habitat_id, pad_packs;
    uint8_t  shed_state[N_FISH_MAX], shed_growth[N_FISH_MAX];
    uint16_t shed_clean[N_FISH_MAX];
    float    body_c[N_FISH_MAX], shed_s[N_FISH_MAX], shed_damp[N_FISH_MAX];
    float    moist[HABITAT_HIDES_MAX];
    uint32_t pack_print[N_FISH_MAX];
    /* the mealworm dish's worms (pocket-terrarium P2c: tank.h THE CRICKETS; an older save's zero: an empty dish) */
    uint8_t  dish_worms;
    /* the terrarium's progress (pocket-terrarium P5; an older save's zeros - and every aquarium's - read: no clean run
     * yet, the dish's worms the keeper's): clean_run, the sheds off clean in a row tank-wide (TMS_CLEAN_RUN, CLEAN_RUN_N;
     * held at 255); dish_farm 1 = the dish's worms are the MEALWORM FARM's (tank_t.dish_farm) */
    uint8_t  clean_run, dish_farm;
    /* the terrarium's decorations' spots (pocket-terrarium P5b: tank_t.deco_x, x / 2 on the sand; an older save's zeros -
     * and every aquarium's - read: unplaced, each at its own clear spot) */
    uint8_t  deco_x[TERR_DECO_N];
    /* the packs by uid (pocket-terrarium, issue #4 phase 1: docs/ptpk.md "A tank and its packs"): the 32-bit uid of the
     * species pack and of the habitat pack the tank holds - a pack's identity for good, where the one-byte ids above only
     * ever named the built-in ones - and their names as they were saved, for the PACK MISSING card. 0 = not saved (an
     * older save: the one-byte ids above decide). A save is read by uid first. When this build cannot hold the pair the
     * save names, the tank keeps these, the ids above and the whole per-animal tail as loaded and every save writes them
     * back unchanged (progression.c s_want); pack_id / habitat_id above are the built-in packs' ids, 0 for a pack that is
     * not built in, so an older build still reads a save of a built-in pair */
    uint32_t species_uid, habitat_uid;
    char     species_name[PACK_NAME_MAX], habitat_name[PACK_NAME_MAX];
    /* never a field: where this build's fields end and a newer build's tail
     * begins (SAVE_FIELDS_END; new fields go above it) */
    uint8_t  newer_tail[];
} save_t;
enum { GS_HAS, GS_SHAPE, GS_FINS, GS_PATTERN, GS_CARRIED, GS_MUTATED = GS_CARRIED + GENE_N, GS_FROM_B, GS_FROM_CARRIED, GS_N };
_Static_assert(GS_N <= 12, "the genes tail holds 12 bytes a fish");
/* the smallest PTK2 save (pre-upkeep, 2026-08-30): anything shorter is not
 * ours. Every later build wrote sizeof(save_t) of its day - 448, 1112, 1304,
 * 1408, 1440, 1456 ... - and load_save takes ANY such length, so a tail only
 * ever appends and never needs its size recorded here. (Until 2026-09-14 the
 * load walked a fixed list of tail offsets; the seen-masks build's 1440 was
 * 1436 padded to the int64's alignment, no offset matched, and the birth-flow
 * flash replaced a live tank with two fry.) */
#define SAVE_CORE_SIZE   offsetof(save_t, veg_growth)
/* the one field ever added MID-struct: bubble_x (2026-09-14) went in ahead of
 * the seen masks, and the browser installer's first builds (public 09-13 ..
 * 09-14) had already written saves without it - 1432 bytes, the masks
 * starting where bubble_x now sits. load_save slides them into place, so a
 * tank kept since the first install updates clean (its badges stay seen and
 * the bubble column stays put). Never add a field mid-struct again. */
#define SAVE_PRE_BUBBLE_SIZE 1432
_Static_assert(offsetof(save_t, ms_seen) == offsetof(save_t, bubble_x) + sizeof(float),
               "the pre-bubble migration expects the seen masks right after bubble_x");
_Static_assert(offsetof(save_t, bubble_x) + sizeof(((save_t *)0)->ms_seen) + sizeof(uint32_t) == SAVE_PRE_BUBBLE_SIZE,
               "the pre-bubble migration expects the 1432-byte layout's masks to end at 1432");

/* ---- SAVE LAYOUT LOCK (2026-09-29): a new field goes at the END of save_t
 * with its own assert here; never edit an existing line here. ----
 * Every tank on a keeper's board is read back through these offsets after a
 * browser update, so they are frozen: a failing assert means the change just
 * moved (scrambled) every saved tank - put the field at the end instead. The
 * array bounds are in here too (N_FISH_MAX, VEG_BEDS, VEG_FRONDS_MAX,
 * ALGAE_CELLS, FISH_NAME_MAX): growing one shifts everything after it, so a
 * bigger bound needs a new tail, not a bigger array. The numbers hold on both
 * builds: no pointers or longs, int64_t aligns to 8 and bool is 1 byte on the
 * device's xtensa gcc and on the 64-bit sim alike (both compilers measured
 * the same 1656 bytes, 2026-09-29). The sizes older builds wrote, all still
 * loadable: 448, 1112, 1304, 1408, 1432 (pre-bubble), 1440, 1456, 1480, 1608,
 * 1616, 1624, 1640, 1656 - sim/testdata/saves holds them. */
_Static_assert(SAVE_MAGIC == 0x50544b32u, "SAVE LAYOUT LOCK: a new magic = every saved tank starts fresh");
_Static_assert(sizeof SAVE_NVS_NS == 5 && sizeof SAVE_NVS_KEY == 5, "SAVE LAYOUT LOCK: the NVS namespace / key are \"terr\" / \"save\"");
#define SAVE_AT(f, off) _Static_assert(offsetof(save_t, f) == (off), "SAVE LAYOUT LOCK: save_t." #f " moved")
#define FISH_AT(f, off) _Static_assert(offsetof(fish_save_t, f) == (off), "SAVE LAYOUT LOCK: fish_save_t." #f " moved")
FISH_AT(preset, 0); FISH_AT(stage, 1); FISH_AT(pad, 2); FISH_AT(size, 4); FISH_AT(trust, 8);
FISH_AT(bold, 12); FISH_AT(sociable, 16); FISH_AT(bold0, 20); FISH_AT(sociable0, 24);
FISH_AT(hunger, 28); FISH_AT(energy, 32); FISH_AT(stress, 36); FISH_AT(curiosity, 40);
FISH_AT(age_s, 44); FISH_AT(rest_dx, 48); FISH_AT(rest_dy, 52);
FISH_AT(eaten, 56); FISH_AT(eaten_player, 60); FISH_AT(ms_bits, 64);
_Static_assert(sizeof(fish_save_t) == 68, "SAVE LAYOUT LOCK: fish_save_t is frozen (a new per-fish field = a new save_t tail array)");
SAVE_AT(magic, 0); SAVE_AT(saved_unix, 8); SAVE_AT(clock, 16);
SAVE_AT(light_override, 20); SAVE_AT(light_on, 21); SAVE_AT(arrival_pending, 22); SAVE_AT(n_fish, 23);
SAVE_AT(feed_spot_x, 24); SAVE_AT(player_feedings, 28); SAVE_AT(hold_approaches, 32); SAVE_AT(tank_ms_bits, 36);
SAVE_AT(fish, 40);                                             /* 6 x 68 */
SAVE_AT(veg_growth, 448); SAVE_AT(algae, 460); SAVE_AT(trims, 1104); SAVE_AT(cells_cleaned, 1108);   /* upkeep, 08-30 */
SAVE_AT(veg_h, 1112);                                          /* fronds, 09-04 */
SAVE_AT(setup_pending, 1304); SAVE_AT(pad_id, 1305); SAVE_AT(names, 1308);                            /* identity, 09-13 */
SAVE_AT(body, 1356); SAVE_AT(accent, 1380); SAVE_AT(bubble_x, 1404);
SAVE_AT(ms_seen, 1408); SAVE_AT(tank_ms_seen, 1432);           /* seen masks, 09-13 */
SAVE_AT(newborn_p1, 1436); SAVE_AT(parent_p1, 1437); SAVE_AT(pad_fam, 1449);                          /* family, 09-14 */
SAVE_AT(drift_acc, 1452);                                      /* drift, 09-15 */
SAVE_AT(light_idle_s, 1476); SAVE_AT(light_auto, 1478); SAVE_AT(light_manual_off, 1479);              /* light, 09-15 */
SAVE_AT(sd_balance, 1480); SAVE_AT(sd_earned, 1484); SAVE_AT(sd_unlocks, 1488); SAVE_AT(sd_paid_fish, 1492);   /* shop, 09-15 */
SAVE_AT(sd_colonies_paid, 1516); SAVE_AT(sd_inches_paid, 1520); SAVE_AT(algae_colonies, 1524);
SAVE_AT(trim_px, 1528); SAVE_AT(snail_x, 1532); SAVE_AT(snail_y, 1536); SAVE_AT(veg_h3, 1540);
SAVE_AT(plant_x, 1604); SAVE_AT(plant_z1, 1608); SAVE_AT(pad_place, 1609);                            /* placement, 09-16 */
SAVE_AT(snail_grazed, 1612);
SAVE_AT(castle_x, 1616); SAVE_AT(castle_z1, 1620); SAVE_AT(pad_castle, 1621);                         /* castle, 09-16 */
SAVE_AT(coral_x, 1624); SAVE_AT(coral_z1, 1628); SAVE_AT(pad_coral, 1629); SAVE_AT(coral_rgb, 1632);  /* coral, 09-23 */
SAVE_AT(coral_growth, 1636);
SAVE_AT(cluster_x, 1640); SAVE_AT(cluster_z1, 1644); SAVE_AT(cluster_scheme, 1645);                   /* reef cluster, 09-24 */
SAVE_AT(fry_seed, 1646); SAVE_AT(cluster_growth, 1648);                                                 /* (1646: pad_cluster until 0.3.5's fry seed) */
/* (the next field: SAVE_AT(its_name, 1652 or its type's alignment past it);) */
_Static_assert(sizeof(save_t) >= 1656, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(shrimp_n, 1652); SAVE_AT(shrimp_food, 1653); SAVE_AT(fry_age_min, 1654); SAVE_AT(shrimp_cool, 1656);   /* shrimp, 09-29 (1654: pad_shrimp until 0.3.5's fry clock) */
SAVE_AT(shrimp_eaten, 1660);
_Static_assert(sizeof(save_t) >= 1664, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(saved_release, 1664);                                                                          /* release stamp, 09-29 (0.2.0) */
_Static_assert(sizeof(save_t) >= 1672, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(sale_meals_need, 1668);                                                                        /* a fish sold, 10-01 */
_Static_assert(sizeof(save_t) >= 1672, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(screen_turned, 1672); SAVE_AT(orient, 1673); SAVE_AT(autofeed_off, 1674); SAVE_AT(pad_screen, 1675);                  /* the watch's way up, 10-02 */
_Static_assert(sizeof(save_t) >= 1680, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(urchin_x, 1676); SAVE_AT(urchin_grazed_px, 1680);                                              /* the urchin, 10-02 */
_Static_assert(sizeof(save_t) >= 1688, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(light_tip_seen, 1684); SAVE_AT(tools_locked, 1685); SAVE_AT(fry_sold, 1686); SAVE_AT(pad_tip, 1687);   /* the lights-out notice, 10-03; the toolbox's LOCK and the fry's blanks, 0.3.5 */
_Static_assert(sizeof(save_t) >= 1688, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(genes, 1688);                                                                                  /* the genes, 0.4 */
_Static_assert(sizeof(save_t) >= 1760, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(tank_uid, 1760); SAVE_AT(fish_uid, 1764); SAVE_AT(parent_uid, 1788);                          /* identity, 0.4 (the tank link) */
_Static_assert(sizeof(save_t) >= 1840, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1836 of fields, padded to 8 */
/* (the next field: SAVE_AT(its_name, 1836 or its type's alignment past it);) */
SAVE_AT(fish_print, 1836);                                                                             /* who each fish is, 0.4 (rollbacks) */
_Static_assert(sizeof(save_t) >= 1864, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1860 of fields, padded to 8 */
SAVE_AT(link_away, 1860); SAVE_AT(pad_link, 1861);                                                   /* fish next door, 0.4 (the tank link) */
_Static_assert(sizeof(save_t) >= 1864, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(sh_s, 1864); SAVE_AT(sh_hitches, 1868); SAVE_AT(sh_trust_passed, 1872); SAVE_AT(sh_eaten, 1876);  /* the seahorse, 0.3.6 (1876: pad_sh until its snacks) */
_Static_assert(sizeof(save_t) >= 1880, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(sh_snack_pay, 1880); SAVE_AT(sd_snacks_paid, 1884); SAVE_AT(sh_snack_cool, 1888);               /* its snacks, 0.3.6 */
_Static_assert(sizeof(save_t) >= 1896, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1892 of fields, padded to 8 */
SAVE_AT(pumpkin_x, 1892); SAVE_AT(pumpkin_z1, 1896); SAVE_AT(pad_pk, 1897);                            /* the pumpkin lantern, 0.3.6 */
_Static_assert(sizeof(save_t) >= 1904, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1900 of fields, padded to 8 */
SAVE_AT(decor_rank, 1900); SAVE_AT(pad_rank, 1905);                                                    /* the pieces' order, 0.3.6 */
_Static_assert(sizeof(save_t) >= 1912, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1908 of fields, padded to 8 */
SAVE_AT(sd_shelved, 1908);                                                                             /* the decoration shelf, 0.3.9 (main's 0.4 tail must reserve 1908) */
_Static_assert(sizeof(save_t) >= 1912, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1908 of fields, padded to 8 */
SAVE_AT(volcano_x, 1912); SAVE_AT(volcano_z1, 1916); SAVE_AT(volcano_rank, 1917); SAVE_AT(pad_vo, 1918);  /* the bubble volcano, 0.3.9 */
_Static_assert(sizeof(save_t) >= 1920, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(sd_locked, 1920);                                                                              /* the LOCK on a piece, 0.3.9 (main's 0.4 tail must reserve 1908..1935: the shelf, the volcano, the lock, the crab) */
_Static_assert(sizeof(save_t) >= 1928, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1924 of fields, padded to 8 */
SAVE_AT(cr_x, 1924); SAVE_AT(cr_variant, 1928); SAVE_AT(pad_cr, 1929); SAVE_AT(cr_eaten, 1930);         /* the strawberry crab, 0.3.9 */
SAVE_AT(cr_found, 1932); SAVE_AT(cr_found_paid, 1934);                                                 /* its finds, 0.3.9 (the padding of the same day) */
_Static_assert(sizeof(save_t) >= 1936, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1936 of fields */
SAVE_AT(cr_find_wait, 1936);                                                                           /* the wait for its next find, 0.3.9.1 (main's 0.4 tail must reserve 1936..1939) */
_Static_assert(sizeof(save_t) >= 1944, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1940 of fields, padded to 8 */
SAVE_AT(lay_w, 1940); SAVE_AT(lay_h, 1942); SAVE_AT(lay_fx0, 1944); SAVE_AT(lay_fx1, 1946);            /* the world it was laid out in, 0.3.9.3 (main's 0.4 tail must reserve 1940..1951) */
SAVE_AT(lay_cols, 1948); SAVE_AT(lay_rows, 1949); SAVE_AT(pad_lay, 1950);                              /* (main's 0.4 tail must reserve 1940..1951) */
_Static_assert(sizeof(save_t) >= 1952, "SAVE LAYOUT LOCK: save_t only ever grows");
SAVE_AT(sh_secs, 1952);                                                                                 /* the seahorse's days, precise, 0.4 */
_Static_assert(sizeof(save_t) >= 1960, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 1956 of fields, padded to 8 */
/* (the next field: SAVE_AT(its_name, 1956 or its type's alignment past it);) */
SAVE_AT(pack_v, 1956); SAVE_AT(pack_id, 1957); SAVE_AT(habitat_id, 1958); SAVE_AT(pad_packs, 1959);   /* the packs, pocket-terrarium P2a */
SAVE_AT(shed_state, 1960); SAVE_AT(shed_growth, 1966); SAVE_AT(shed_clean, 1972);
SAVE_AT(body_c, 1984); SAVE_AT(shed_s, 2008); SAVE_AT(shed_damp, 2032); SAVE_AT(moist, 2056);       /* (moist: HABITAT_HIDES_MAX is a bound of the lock too) */
SAVE_AT(pack_print, 2072);                                                                             /* (its own prints: pack_slot) */
_Static_assert(sizeof(save_t) >= 2096, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 2096 of fields */
SAVE_AT(dish_worms, 2096);                                                                             /* the mealworm dish, pocket-terrarium P2c */
_Static_assert(sizeof(save_t) >= 2104, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 2097 of fields, padded to 8 */
SAVE_AT(clean_run, 2097); SAVE_AT(dish_farm, 2098);                                                   /* the terrarium's progress, pocket-terrarium P5 */
_Static_assert(sizeof(save_t) >= 2104, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 2099 of fields, padded to 8 */
SAVE_AT(deco_x, 2099);                                                                                 /* the terrarium's decorations, pocket-terrarium P5b */
_Static_assert(sizeof(save_t) >= 2104, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 2103 of fields, padded to 8 */
SAVE_AT(species_uid, 2104); SAVE_AT(habitat_uid, 2108); SAVE_AT(species_name, 2112); SAVE_AT(habitat_name, 2136);   /* the packs by uid, pocket-terrarium pack structure phase 1 */
_Static_assert(sizeof(save_t) >= 2160, "SAVE LAYOUT LOCK: save_t only ever grows");   /* 2160 of fields */
/* (the next field: SAVE_AT(its_name, 2160 or its type's alignment past it);) */
/* A ROLLBACK keeps the newer build's tail (2026-10-05): a save longer than
 * this build's fields loads its head, and every save writes the bytes past
 * SAVE_FIELDS_END back as they were - so the update back up finds its tail
 * (0.4's genes and IDs, or whatever comes next) where it left it, under a
 * head the older build kept living. Builds before this one (0.3.2 and
 * older) load the head but drop the tail. A tail is written by the newer
 * build and read back under an OLDER build's head: anything in it that
 * belongs to a fish slot must find its fish by save_t.fish_print, and
 * anything else must still make sense if the older build changed the head. */
#define SAVE_FIELDS_END offsetof(save_t, newer_tail)
_Static_assert(SAVE_FIELDS_END >= 1952 && SAVE_FIELDS_END <= sizeof(save_t), "SAVE LAYOUT LOCK: newer_tail is the end of save_t's fields");
#define SAVE_KEEP_MAX   8192         /* a longer blob than this is not a save of ours: its head loads, no tail is kept */
/* NVS budget: the save is one blob in the nvs partition (0x9000, 0x6000 =
 * 6 pages of 4096 B; tools/make_installer.py pins the row). A page is 126
 * entries of 32 B, and NVS keeps one page free for its garbage collection:
 * 5 x 126 = 630 entries usable. A blob costs an entry per 32 B, a header per
 * chunk (a chunk stays on one page) and an index entry, and nvs_set_blob
 * writes the new copy before it erases the old - on top of the director's
 * parked "bk" copy, three at once at worst. At 4000 B (a page's worth) that
 * is 3 x ~128 = ~390 entries, plus ~10 for the settings, "bat"/"hist" and
 * the batlog's "bed": under 2/3 of the 630, so GC always has room. Past it,
 * saves can start failing for space, and any nvs_flash_init error makes
 * main.c ERASE the partition. 2160 B today (pocket-terrarium: P2a's packs tail at 1956..2095, P2c's mealworm dish at
 * 2096 and P5's clean run and farm at 2097..2098 past pocket-tank 0.4's 1956; 2099 of fields padded to 8; the packs' uids
 * and names at 2104..2159, issue #4 phase 1; ~69 entries a copy): 1840 B of headroom
 * under the 4000. A rolled-back tank writes the newer build's length instead
 * (SAVE_FIELDS_END).
 * MY TANKS holds the save in a slot of SHELF_SAVE_MAX (3840, shelf.h), under the 4000 the NVS math allows: that is the
 * budget. A longer save cannot go on the shelf (a rolled-back tank's newer save: shelf_swap refuses it). */
#define SAVE_NVS_BUDGET SHELF_SAVE_MAX
_Static_assert(SAVE_NVS_BUDGET <= 4000, "the shelf's slot outgrew the NVS budget - see the math above");
_Static_assert(sizeof(save_t) <= SAVE_NVS_BUDGET, "the save outgrew its budget (the NVS math above, a MY TANKS slot)");
_Static_assert(SAVE_KEEP_MAX >= SAVE_NVS_BUDGET, "a save of ours is always kept whole");

#endif
