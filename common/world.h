/* world.h - the build's WORLD (PLAN.md D11): one reusable core, one bin per world.
 *
 * PT_WORLD is set when the bin is built - never at run time:
 *   sim       make WORLD=terrarium (the default) | make WORLD=aquarium   (sim/Makefile: -DPT_WORLD=...)
 *   firmware  the Kconfig choice POCKET_TANK_WORLD (main/Kconfig.projbuild); sdkconfig.aquarium picks the aquarium
 * It picks, for the whole build:
 *   - the brain: the model and its tokenizer (THE BRAIN below - the one place the lizard model's files go),
 *   - the packs a bin LINKS and a new tank's pair (species.h SPECIES_DEFAULT, habitat.h HABITAT_DEFAULT; a release bin carries
 *     only its world's; the simulator defines PT_ALL_PACKS to carry both). Which pack a tank may HOLD is no longer the world's
 *     to say: it is the pair's capabilities (packs/compat.h, issue #4 phase 2) - the world is a preset (modules, model,
 *     default packs) and a filter for the shop and the menus (a pack's WORLDS mask: PT_WORLD_* are its bits),
 *   - the goal set (tank.h GOAL_FIRST / GOAL_N: the fish's eight or the lizard's seven),
 *   - the locomotion (tank.c: the swim, or THE WALK on the habitat's ground - terrain.h),
 *   - the renderer and art sources (the aquarium's live water/grass stack or the terrarium's desert/lizards),
 *   - the selftest list (sim/Makefile SELFTESTS_SHARED + SELFTESTS_AQUARIUM / SELFTESTS_TERRARIUM).
 * Values 1 and 2, never 0: an #if on a header that was not included reads 0 and matches neither. */
#ifndef WORLD_H
#define WORLD_H

#define PT_WORLD_AQUARIUM  1          /* pocket-tank's fish, swimming (the fish model, schema v4). Also its bit (1) in a pack's WORLDS mask */
#define PT_WORLD_TERRARIUM 2          /* the lizards, walking (the lizard model, schema L1). Also its bit (2) in a pack's WORLDS mask */
#ifndef PT_WORLD
#define PT_WORLD PT_WORLD_TERRARIUM
#endif
#if PT_WORLD != PT_WORLD_AQUARIUM && PT_WORLD != PT_WORLD_TERRARIUM
#error "PT_WORLD is PT_WORLD_AQUARIUM (1) or PT_WORLD_TERRARIUM (2)"
#endif
/* THE ONE PLACE the build's world is picked (issue #4 phase 3): everything below that is a constant before the C compiler runs
 * - the model's and the tokenizer's file names, its identity in version.h - is the world's own PT_<THING>_<KEY> constant,
 * chosen with PT_WORLD_KEY through PT_PICK(). The step that builds both worlds into one image turns this block into the
 * default world's pick and nothing around it changes: the engine's per-world decisions run through world_cur() (below). */
#if PT_WORLD == PT_WORLD_AQUARIUM
#define PT_WORLD_KEY  AQUARIUM
#define PT_WORLD_NAME "aquarium"
#else
#define PT_WORLD_KEY  TERRARIUM
#define PT_WORLD_NAME "terrarium"
#endif
#define PT_CAT_(a, b)   a##b
#define PT_CAT(a, b)    PT_CAT_(a, b)
#define PT_PICK(prefix) PT_CAT(prefix, PT_WORLD_KEY)   /* PT_PICK(PT_MODEL_TAG_): this world's constant */
#define PT_TERRARIUM (PT_WORLD == PT_WORLD_TERRARIUM)   /* 1 in the terrarium's bin, 0 in the aquarium's (still read by the slices not yet through world_cur()) */
/* the words a keeper reads, per world (P5): WORLD_STR("TWO FRY", "TWO HATCHLINGS") is the aquarium's in its bin and the
 * terrarium's in its own - a constant choice, folded away like icons.h WORLD_ICON, so the aquarium's text is pocket-tank's.
 * Whole phrases, never spliced words: each world's line is written (and fitted to its box) on its own. The terrarium says
 * LIZARD for fish, HATCHLING for fry, TERRARIUM for tank, SMUDGES for algae, PLANTS for grass, CRICKETS for pellets and
 * DESERT ROSES for sand dollars. */
#define WORLD_STR(aquarium, terrarium) (PT_TERRARIUM ? (terrarium) : (aquarium))

/* THE WORLD MODULE (issue #4 phase 3, world_ops.h): the engine's per-world decisions - the shop's effects, the fry's gates,
 * the badges, the first-run pages, the words - are one descriptor per world, defined in common/worlds/<world>/world.c.
 * world_cur() is the descriptor of the world the tank in hand lives in; engine files ask it and never ask which world it is.
 * (A bin builds one world today, so it is that world's. The macros in this header stay only for what must be a constant
 * before the C compiler runs: the model's and the tokenizer's file names, the packs a bin links, the firmware shell.) */
typedef struct world_s world_t;
const world_t *world_cur(void);
/* Test/tool builds may carry both worlds' const packs to validate the boundary. Release bins leave this 0 and link
 * only their own pack sources (firmware/components/tankcore/CMakeLists.txt). */
#ifndef PT_ALL_PACKS
#define PT_ALL_PACKS 0
#endif
/* THE CARD LIBRARY (issue #4 phase 4, docs/card-library.md): 1 = the build carries the microSD card library - the card port
 * (common/card.h), the pack store in flash (packs/pstore.h), the library (packs/library.h), the PACKS page under ADVANCED
 * (packs_page.h) and the pool's spare slots they need (packs/ptpk.h PACKS_SPARE). 0 = none of it is compiled, the pools keep
 * their size and no page is drawn: a board that cannot use a card has no dead UI. The simulator builds it by default (its
 * card is a folder: POCKET_CARD_DIR; `make CARD=0` builds without) and the firmware from the Kconfig option
 * CONFIG_POCKET_TANK_CARD (default n; compile-only, UNVERIFIED on a 1.8 until someone tries it - docs/card-library.md). */
#ifndef PT_CARD
#define PT_CARD 0
#endif

/* ---- THE BRAIN, per world: the model and its tokenizer. Paths are from the repo's root: the sim opens "../" + them
 * (it runs in sim/); the firmware embeds PT_FW_TOKENIZER_<WORLD> (from firmware/, a copy kept in the repo) -
 * firmware/main/CMakeLists.txt reads its world's line from this file - and the model itself goes to the model partition
 * (tools/flash*.sh, which ask tools/model_trailer.py --print-model for their build's world). The model's identity - tag,
 * length, sha256 - is version.h's PT_MODEL_*_<WORLD>. The aquarium thinks with the v4 fish model, the terrarium with
 * the lizard model (schema L1, model/schema-lizard.md; docs/retrain-lizard.md, lizard_v1 = model_q4_l1_focus2.bin);
 * advisor_core_init refuses the other world's. ---- */
#define PT_MODEL_FILE_AQUARIUM      "model/out/model_q4.bin"
#define PT_TOKENIZER_FILE_AQUARIUM  "model/out/tokenizer.bin"
#define PT_FW_TOKENIZER_AQUARIUM    "main/tokenizer.bin"
#define PT_MODEL_FILE_TERRARIUM     "model/out/model_q4_lizard.bin"
#define PT_TOKENIZER_FILE_TERRARIUM "model/out/tokenizer_lizard.bin"
#define PT_FW_TOKENIZER_TERRARIUM   "main/tokenizer_lizard.bin"
#define PT_MODEL_FILE     PT_PICK(PT_MODEL_FILE_)
#define PT_TOKENIZER_FILE PT_PICK(PT_TOKENIZER_FILE_)

#endif
