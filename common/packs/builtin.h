/* builtin.h - the built-in packs (docs/ptpk.md "Built-in packs"): the table of them, and their uids.
 *
 * A built-in pack's source of truth is a const species_t / habitat_t / model_manifest_t in C - common/species/gecko.c and
 * fish.c, common/habitat/desert.c and freshwater.c, common/packs/models.c, edited by hand. Nothing reads those structs at run
 * time: packs_load_builtin (ptpk.c) encodes each of this table's packs into a PTPK file (ptpk_write.c) and offers it to
 * packs_add, the entry a file from a folder or the card goes through; tools/ptpk_emit.c writes the same files to
 * packs/<uid_ns>.ptpk (`make packs`).
 *
 * What a build carries: PT_ALL_PACKS (the simulator, the tools) every pack; a release image its world's, test-only packs
 * left out - the firmware links only its world's const defs (firmware/components/tankcore/CMakeLists.txt) and the table
 * names only what is linked. */
#ifndef BUILTIN_H
#define BUILTIN_H
#include <stddef.h>
#include <stdint.h>
#include "world.h"
#include "species.h"
#include "habitat.h"
#include "manifest.h"

/* the packs' uids, as the constants a species lists its habitats by (habitat_uid[]): each the FNV-1a-32 of the uid_ns string
 * beside it (docs/ptpk.md "uid"); --selftest-ptpk holds every one to its hash (the models' are manifest.h's) */
#define HABITAT_UID_DESERT     0x4eddd683u    /* pt.habitat.desert */
#define HABITAT_UID_FRESHWATER 0x8e5bc411u    /* pt.habitat.freshwater */
#define HABITAT_UID_TEST_WARM  0x5d6c36fau    /* pt.test.desert-warm */
#define SPECIES_UID_GECKO      0xc292e03fu    /* pt.gecko.leopard */
#define SPECIES_UID_FISH       0xddfd3d94u    /* pt.fish.roster */
#define SPECIES_UID_TEST_WARM  0x7c14a23eu    /* pt.test.gecko-warm */

/* the const definitions (common/species and common/habitat; the test packs' only where PT_ALL_PACKS) */
extern const species_t SPECIES_PACK_GECKO, SPECIES_PACK_TEST_WARM, SPECIES_PACK_FISH;
extern const habitat_t HABITAT_PACK_DESERT, HABITAT_PACK_FRESHWATER, HABITAT_PACK_TEST_WARM;

#if PT_ALL_PACKS
#define PTPK_BUILTIN_MODELS 2
#define PTPK_BUILTIN_HABITATS 3
#define PTPK_BUILTIN_SPECIES 3
#else
#define PTPK_BUILTIN_MODELS 1
#define PTPK_BUILTIN_HABITATS 1
#define PTPK_BUILTIN_SPECIES 1
#endif

/* one built-in pack: the name its uid is the hash of, that uid, and its definition - a species, a habitat or a model
   manifest, the others NULL */
typedef struct {
    const char             *uid_ns;
    uint32_t                uid;
    const species_t        *species;
    const habitat_t        *habitat;
    const model_manifest_t *manifest;
} builtin_pack_t;

/* manifests first, then habitats, then species: a species lists its habitats by uid and names goals that the manifests list,
   so they are loaded before it */
#define PTPK_BUILTIN_N (PTPK_BUILTIN_MODELS + PTPK_BUILTIN_HABITATS + PTPK_BUILTIN_SPECIES)
extern const builtin_pack_t PTPK_BUILTIN[PTPK_BUILTIN_N];

/* the pack as a file into out, as ptpk_encode_species / ptpk_encode_habitat / ptpk_encode_manifest (0 and a reason in *why:
 * the definition cannot be written, or the table's uid is not its own) */
size_t builtin_encode(const builtin_pack_t *b, uint8_t *out, size_t cap, const char **why);
#endif
