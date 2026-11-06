/* manifest.c - the model manifest registry and its rules (manifest.h) */
#include "manifest.h"
#include "builtin.h"                       /* PTPK_BUILTIN_MODELS: how many manifests this build ships */
#include "ptpk.h"                          /* packs_init, MANIFEST_SPARE */
#include "ptpk_write.h"                    /* ptpk_tag_ok, ptpk_goal_ok */
#include "version.h"                       /* PT_MODEL_TAG: the model this build runs */
#include <stdio.h>
#include <string.h>

/* THE POOL, as species.c's: this build's built-in manifests and MANIFEST_SPARE slots for the ones found at run time, in .bss.
   s_crc is the body CRC of the file each came from, to tell the same file offered again from another with its uid */
#define MANIFEST_POOL (PTPK_BUILTIN_MODELS + MANIFEST_SPARE)
static model_manifest_t s_pool[MANIFEST_POOL];
static uint32_t         s_crc[MANIFEST_POOL];
static int              s_n;

static size_t slen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

int manifest_count(void) { packs_init(); return s_n; }
const model_manifest_t *manifest_at(int i) { packs_init(); return i >= 0 && i < s_n ? &s_pool[i] : NULL; }
const model_manifest_t *manifest_by_uid(uint32_t uid) {
    packs_init();
    for (int i = 0; uid && i < s_n; i++) if (s_pool[i].uid == uid) return &s_pool[i];
    return NULL;
}
const model_manifest_t *manifest_by_tag(const char *tag) {
    packs_init();
    for (int i = 0; tag && i < s_n; i++) if (strcmp(s_pool[i].tag, tag) == 0) return &s_pool[i];
    return NULL;
}
const model_manifest_t *model_provides(void) { return manifest_by_tag(PT_MODEL_TAG); }

bool manifest_has_goal(const model_manifest_t *m, const char *word) {
    for (int k = 0; m && word && k < m->n_goals && k < SPECIES_GOALS_MAX; k++) if (strcmp(m->goal[k], word) == 0) return true;
    return false;
}
bool manifests_know_goal(const char *word) {
    packs_init();
    for (int i = 0; i < s_n; i++) if (manifest_has_goal(&s_pool[i], word)) return true;
    return false;
}

model_manifest_t *manifest_slot(void) {
    packs_init();
    if (s_n >= MANIFEST_POOL) return NULL;
    memset(&s_pool[s_n], 0, sizeof s_pool[s_n]);
    return &s_pool[s_n];
}

bool manifest_commit(model_manifest_t *m, bool builtin, uint32_t crc, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (m != &s_pool[s_n] || s_n >= MANIFEST_POOL) REFUSE("not the slot the registry gave out");
    if (!m->uid) REFUSE("no uid");
    for (int i = 0; i < s_n; i++) {
        if (s_pool[i].uid == m->uid) REFUSE("uid %08x is %s's already", (unsigned)m->uid, s_pool[i].name);
        if (strcmp(s_pool[i].tag, m->tag) == 0) REFUSE("the model %s has a manifest already (%s's)", m->tag, s_pool[i].name);
    }
    m->builtin = builtin;
    if (!manifest_check(m, why, n)) return false;
    s_crc[s_n] = crc; s_n++;
    return true;
    #undef REFUSE
}

pack_seen_t manifest_seen(uint32_t uid, uint32_t crc, const char **by) {
    packs_init();
    for (int i = 0; i < s_n; i++)
        if (s_pool[i].uid == uid) { if (by) *by = s_pool[i].name; return s_crc[i] == crc ? PACK_SAME : PACK_UID_TAKEN; }
    return PACK_NEW;
}

void manifest_drop_loaded(void) {
    int k = 0;
    for (int i = 0; i < s_n; i++) if (s_pool[i].builtin) { if (k != i) { s_pool[k] = s_pool[i]; s_crc[k] = s_crc[i]; } k++; }
    s_n = k;
}

bool manifest_check(const model_manifest_t *m, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (!m->name[0]) REFUSE("no name");
    const size_t tl = slen(m->tag, MODEL_TAG_MAX);
    if (tl >= MODEL_TAG_MAX || !ptpk_tag_ok((const uint8_t *)m->tag, tl)) REFUSE("the model's tag is not 1..%d characters of [A-Za-z0-9._-]", MODEL_TAG_MAX - 1);
    if (m->n_goals < 1 || m->n_goals > SPECIES_GOALS_MAX) REFUSE("%d goal words, not 1..%d", m->n_goals, SPECIES_GOALS_MAX);
    for (int k = 0; k < m->n_goals; k++) {
        const size_t l = slen(m->goal[k], GOAL_NAME_MAX);
        if (l >= GOAL_NAME_MAX || !ptpk_goal_ok((const uint8_t *)m->goal[k], l)) REFUSE("goal %d is not a word of 1..%d characters of [a-z0-9_], a letter first", k, GOAL_NAME_MAX - 1);
        for (int j = 0; j < k; j++) if (strcmp(m->goal[j], m->goal[k]) == 0) REFUSE("the goal \"%s\" is listed twice", m->goal[k]);
    }
    return true;
    #undef REFUSE
}
