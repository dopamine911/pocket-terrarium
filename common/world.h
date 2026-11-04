/* world.h - the build's WORLD (PLAN.md D11): one reusable core, one bin per world.
 *
 * PT_WORLD is set when the bin is built - never at run time:
 *   sim       make WORLD=terrarium (the default) | make WORLD=aquarium   (sim/Makefile: -DPT_WORLD=...)
 *   firmware  the Kconfig choice POCKET_TANK_WORLD (main/Kconfig.projbuild); sdkconfig.aquarium picks the aquarium
 * It picks, for the whole build:
 *   - the brain: the model and its tokenizer (THE BRAIN below - the one place the lizard model's files go),
 *   - the packs a tank may hold and a new tank's pair (species.h SPECIES_DEFAULT, habitat.h HABITAT_DEFAULT;
 *     tank_set_packs refuses another world's - every pack is compiled into both bins, only this world's are held),
 *   - the goal set (tank.h GOAL_FIRST / GOAL_N: the fish's eight or the lizard's seven),
 *   - the locomotion (tank.c: the swim, or THE WALK on the floor),
 *   - the selftest list (sim/Makefile SELFTESTS_SHARED + SELFTESTS_AQUARIUM / SELFTESTS_TERRARIUM).
 * Values 1 and 2, never 0: an #if on a header that was not included reads 0 and matches neither. */
#ifndef WORLD_H
#define WORLD_H

#define PT_WORLD_AQUARIUM  1          /* pocket-tank's fish, swimming (the fish model, schema v4) */
#define PT_WORLD_TERRARIUM 2          /* the lizards, walking (the lizard model, schema L1) */
#ifndef PT_WORLD
#define PT_WORLD PT_WORLD_TERRARIUM
#endif
#if PT_WORLD != PT_WORLD_AQUARIUM && PT_WORLD != PT_WORLD_TERRARIUM
#error "PT_WORLD is PT_WORLD_AQUARIUM (1) or PT_WORLD_TERRARIUM (2)"
#endif
#define PT_TERRARIUM (PT_WORLD == PT_WORLD_TERRARIUM)   /* 1 in the terrarium's bin, 0 in the aquarium's: `if (PT_TERRARIUM)` keeps both paths compiled */
#define PT_WORLD_NAME (PT_TERRARIUM ? "terrarium" : "aquarium")

/* ---- THE BRAIN, per world: the model and its tokenizer. Paths are from the repo's root: the sim opens "../" + them
 * (it runs in sim/); the firmware embeds PT_FW_TOKENIZER_<WORLD> (from firmware/, a copy kept in the repo, as model/out
 * is not) - firmware/main/CMakeLists.txt reads its world's line from this file - and the model itself goes to the
 * model partition (tools/flash*.sh). The model's identity - tag, length, sha256 - is version.h's PT_MODEL_*.
 *
 * THE LIZARD MODEL GOES HERE (P6): the terrarium's three lines are still the v4 fish model, its TEMPORARY brain - it
 * thinks for the lizards through advisor_core.c's TEMPORARY FISH-MODEL BRIDGE. When the lizard model lands, point them
 * at its files (version.h's identity at its own) and delete the bridge. ---- */
#define PT_MODEL_FILE_AQUARIUM      "model/out/model_q4.bin"
#define PT_TOKENIZER_FILE_AQUARIUM  "model/out/tokenizer.bin"
#define PT_FW_TOKENIZER_AQUARIUM    "main/tokenizer.bin"
#define PT_MODEL_FILE_TERRARIUM     "model/out/model_q4.bin"       /* TEMPORARY: the v4 fish model (P6: the lizard model's) */
#define PT_TOKENIZER_FILE_TERRARIUM "model/out/tokenizer.bin"      /* TEMPORARY: its tokenizer */
#define PT_FW_TOKENIZER_TERRARIUM   "main/tokenizer.bin"           /* TEMPORARY: its tokenizer, embedded */
#if PT_TERRARIUM
#define PT_MODEL_FILE     PT_MODEL_FILE_TERRARIUM
#define PT_TOKENIZER_FILE PT_TOKENIZER_FILE_TERRARIUM
#else
#define PT_MODEL_FILE     PT_MODEL_FILE_AQUARIUM
#define PT_TOKENIZER_FILE PT_TOKENIZER_FILE_AQUARIUM
#endif

#endif
