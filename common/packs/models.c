/* models.c - the built-in model manifests (manifest.h) and the goal words the engine reads (species.h). Registry-free, like
 * ptpk_write.c and builtin.c: tools/ptpk_emit.c links it with the encoder and the table alone.
 *
 * THESE STRUCTS ARE THE MANIFESTS' SOURCE (docs/ptpk.md "Model manifests"): packs_load_builtin encodes each into the PTPK file
 * the loader takes at boot and tools/ptpk_emit.c writes them to packs/pt.model.lizard_v1.ptpk and packs/pt.model.fish_v3m.ptpk
 * (`make packs`). A model's tag is the one version.h names, so the manifest of the model a build runs is found by it
 * (manifest.c model_provides); the goal words are the words of the model's candidates, in its order - the lizard model's seven
 * (model/schema-lizard.md "Goals") and the fish model's eight (schema v2-v4). `.uid` is the FNV-1a-32 of the uid string beside
 * it in packs/builtin.c (--selftest-ptpk holds it to the hash). */
#include "manifest.h"
#include "version.h"                       /* PT_MODEL_TAG_TERRARIUM, PT_MODEL_TAG_AQUARIUM */

const char *const LZ_GOAL_NAMES[LZ_GOALS]     = { LZ_GOAL_WORDS };
const char *const FISH_GOAL_NAMES[FISH_GOALS] = { FISH_GOAL_WORDS };

_Static_assert(sizeof PT_MODEL_TAG_TERRARIUM <= MODEL_TAG_MAX && sizeof PT_MODEL_TAG_AQUARIUM <= MODEL_TAG_MAX, "a model's tag fits its manifest");

#if PT_ALL_PACKS || PT_TERRARIUM
const model_manifest_t MODEL_MANIFEST_LIZARD_V1 = {
    .name = "LIZARD V1", .uid = MODEL_UID_LIZARD_V1, .tag = PT_MODEL_TAG_TERRARIUM,
    .n_goals = LZ_GOALS, .goal = { LZ_GOAL_WORDS },
};
#endif
#if PT_ALL_PACKS || !PT_TERRARIUM
const model_manifest_t MODEL_MANIFEST_FISH_V3M = {
    .name = "FISH V3M", .uid = MODEL_UID_FISH_V3M, .tag = PT_MODEL_TAG_AQUARIUM,
    .n_goals = FISH_GOALS, .goal = { FISH_GOAL_WORDS },
};
#endif
