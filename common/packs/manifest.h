/* manifest.h - model manifests: what a model provides (issue #4 phase 2, docs/ptpk.md "Model manifests").
 *
 * A species names its goals by word, and a model has a list of words it can answer with (its candidates, in its own order):
 * the model's MANIFEST, a PTPK file of kind 3 - its tag (common/version.h PT_MODEL_TAG: lizard_v1, v3m) and its goal words.
 * The built-in manifests are C data (models.c) encoded at boot and loaded through the one loader like a species or a habitat
 * (packs_add); a manifest from a folder or the card loads into a spare slot - the registry below, a fixed pool, no malloc.
 *
 * Two questions, two functions: species_check asks whether any manifest this build carries lists the word
 * (manifests_know_goal: a release image carries its own model's, the simulator both worlds'), and the pair check
 * (compat.h packs_compatible) whether the manifest of the model this build RUNS does (model_provides). A tank runs a species
 * only if that model has every goal word it names. */
#ifndef MANIFEST_H
#define MANIFEST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "species.h"

#define MODEL_TAG_MAX 16                /* a model's tag: 1..15 characters of [A-Za-z0-9._-] */
typedef struct {
    char     name[PACK_NAME_MAX];       /* "LIZARD V1" */
    bool     builtin;                   /* shipped in the firmware */
    uint32_t uid;                       /* FNV-1a-32 of its uid string (pt.model.lizard_v1), as a species' */
    char     tag[MODEL_TAG_MAX];        /* the model's tag as version.h names it */
    uint8_t  n_goals;                   /* its goal words, in its candidate order (1..SPECIES_GOALS_MAX) */
    char     goal[SPECIES_GOALS_MAX][GOAL_NAME_MAX];
} model_manifest_t;

/* THE BUILT-IN MANIFESTS (models.c): the two models this repository trains. A release image links its own world's alone */
#define MODEL_UID_LIZARD_V1 0xb551e3d0u    /* pt.model.lizard_v1 (tag lizard_v1: the terrarium's model) */
#define MODEL_UID_FISH_V3M  0x4bc519a3u    /* pt.model.fish_v3m (tag v3m: the aquarium's model) */
extern const model_manifest_t MODEL_MANIFEST_LIZARD_V1, MODEL_MANIFEST_FISH_V3M;

/* THE REGISTRY, as species.h's: the manifests loaded, built-in ones first (packs_init) */
const model_manifest_t *manifest_by_tag(const char *tag);          /* NULL: none loaded for that model */
const model_manifest_t *manifest_by_uid(uint32_t uid);
int  manifest_count(void);
const model_manifest_t *manifest_at(int i);
/* the manifest of the model THIS BUILD RUNS (common/version.h PT_MODEL_TAG); NULL if it is not loaded (every pair is then refused) */
const model_manifest_t *model_provides(void);
bool manifest_has_goal(const model_manifest_t *m, const char *word);
/* some manifest loaded lists the word: a goal no model can perform is no pack's to name */
bool manifests_know_goal(const char *word);
/* the loader's, as species_slot / species_commit / species_seen / species_drop_loaded; a manifest's tag is unique */
model_manifest_t *manifest_slot(void);
bool manifest_commit(model_manifest_t *slot, bool builtin, uint32_t crc, char *why, size_t n);
pack_seen_t manifest_seen(uint32_t uid, uint32_t crc, const char **by);
void manifest_drop_loaded(void);
/* a manifest's own rules: a name, a tag of 1..15 characters of [A-Za-z0-9._-], 1..8 goal words, each of [a-z][a-z0-9_]* (1..15
   characters), none twice (ptpk_write.h ptpk_tag_ok, ptpk_goal_ok). false with the first broken rule in why */
bool manifest_check(const model_manifest_t *m, char *why, size_t n);
#endif
