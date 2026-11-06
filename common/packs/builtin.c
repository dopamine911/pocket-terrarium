/* builtin.c - the table of the built-in packs (builtin.h). Registry-free, like ptpk_write.c: tools/ptpk_emit.c links it with
 * the encoder and the const definitions alone. */
#include "builtin.h"
#include "ptpk_write.h"

const builtin_pack_t PTPK_BUILTIN[PTPK_BUILTIN_HABITATS + PTPK_BUILTIN_SPECIES] = {
#if PT_ALL_PACKS || PT_TERRARIUM
    { "pt.habitat.desert",     HABITAT_UID_DESERT,     NULL, &HABITAT_PACK_DESERT },
#endif
#if PT_ALL_PACKS || !PT_TERRARIUM
    { "pt.habitat.freshwater", HABITAT_UID_FRESHWATER, NULL, &HABITAT_PACK_FRESHWATER },
#endif
#if PT_ALL_PACKS
    { "pt.test.desert-warm",   HABITAT_UID_TEST_WARM,  NULL, &HABITAT_PACK_TEST_WARM },
#endif
#if PT_ALL_PACKS || !PT_TERRARIUM
    { "pt.fish.roster",        SPECIES_UID_FISH,       &SPECIES_PACK_FISH, NULL },
#endif
#if PT_ALL_PACKS || PT_TERRARIUM
    { "pt.gecko.leopard",      SPECIES_UID_GECKO,      &SPECIES_PACK_GECKO, NULL },
#endif
#if PT_ALL_PACKS
    { "pt.test.gecko-warm",    SPECIES_UID_TEST_WARM,  &SPECIES_PACK_TEST_WARM, NULL },
#endif
};

size_t builtin_encode(const builtin_pack_t *b, uint8_t *out, size_t cap, const char **why) {
    const uint32_t own = b->species ? b->species->uid : b->habitat->uid;
    if (own != b->uid) { if (why) *why = "the table's uid is not the definition's"; return 0; }
    return b->species ? ptpk_encode_species(b->species, b->uid_ns, out, cap, why) : ptpk_encode_habitat(b->habitat, b->uid_ns, out, cap, why);
}
