/* builtin.c - the table of the built-in packs (builtin.h). Registry-free, like ptpk_write.c: tools/ptpk_emit.c links it with
 * the encoder and the const definitions alone. */
#include "builtin.h"
#include "ptpk_write.h"

const builtin_pack_t PTPK_BUILTIN[PTPK_BUILTIN_N] = {
#if PT_ALL_PACKS || PT_TERRARIUM
    { "pt.model.lizard_v1",    MODEL_UID_LIZARD_V1,    NULL, NULL, &MODEL_MANIFEST_LIZARD_V1 },
#endif
#if PT_ALL_PACKS || !PT_TERRARIUM
    { "pt.model.fish_v3m",     MODEL_UID_FISH_V3M,     NULL, NULL, &MODEL_MANIFEST_FISH_V3M },
#endif
#if PT_ALL_PACKS || PT_TERRARIUM
    { "pt.habitat.desert",     HABITAT_UID_DESERT,     NULL, &HABITAT_PACK_DESERT, NULL },
#endif
#if PT_ALL_PACKS || !PT_TERRARIUM
    { "pt.habitat.freshwater", HABITAT_UID_FRESHWATER, NULL, &HABITAT_PACK_FRESHWATER, NULL },
#endif
#if PT_ALL_PACKS
    { "pt.test.desert-warm",   HABITAT_UID_TEST_WARM,  NULL, &HABITAT_PACK_TEST_WARM, NULL },
#endif
#if PT_ALL_PACKS || !PT_TERRARIUM
    { "pt.fish.roster",        SPECIES_UID_FISH,       &SPECIES_PACK_FISH, NULL, NULL },
#endif
#if PT_ALL_PACKS || PT_TERRARIUM
    { "pt.gecko.leopard",      SPECIES_UID_GECKO,      &SPECIES_PACK_GECKO, NULL, NULL },
#endif
#if PT_ALL_PACKS
    { "pt.test.gecko-warm",    SPECIES_UID_TEST_WARM,  &SPECIES_PACK_TEST_WARM, NULL, NULL },
#endif
};

size_t builtin_encode(const builtin_pack_t *b, uint8_t *out, size_t cap, const char **why) {
    const uint32_t own = b->species ? b->species->uid : b->habitat ? b->habitat->uid : b->manifest->uid;
    if (own != b->uid) { if (why) *why = "the table's uid is not the definition's"; return 0; }
    if (b->species) return ptpk_encode_species(b->species, b->uid_ns, out, cap, why);
    if (b->habitat) return ptpk_encode_habitat(b->habitat, b->uid_ns, out, cap, why);
    return ptpk_encode_manifest(b->manifest, b->uid_ns, out, cap, why);
}
