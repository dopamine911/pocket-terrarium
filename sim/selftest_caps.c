#define _DEFAULT_SOURCE   /* mkdir, setenv, unsetenv under -std=c11 (glibc) */
/* selftest_caps.c - issue #4 phase 2, "capability matching": what a species needs, what a habitat, the firmware and the model
 * provide, and what the loader and the tank do with a pair that cannot run (docs/ptpk.md "Capabilities"; the contract is
 * packs/caps.h, packs/compat.h, packs/manifest.h).
 *
 *   ./fishsim --selftest-caps     1  every built-in pair: held exactly when the species lists the habitat and the model has its
 *                                    goal words; the other world's listed pairs refused by the model, the tank untouched
 *                                 2  the derived sets (needs, provides, moves) are what the built-in structs say; the firmware's
 *                                    capabilities and the simulator's override of them
 *                                 3  the reasons, verbatim: a need the habitat lacks, the firmware lacks, a goal the model lacks;
 *                                    the card's short need; packs_offered and the WORLDS filter
 *                                 4  a synthetic crossover (walks AND swims) in a habitat that carries both: accepted by a
 *                                    firmware with both modules, refused - the module NAMED - by either module alone, in structs
 *                                    and through files
 *                                 5  capability bits no firmware names, byte by byte in the files: refused where they are needs,
 *                                    skipped where they are only wants or offers; WORLDS; unknown records
 *                                 6  goals: the vocabulary is the manifests', the model leg is the tank's; a manifest loaded
 *                                    gives a word, dropped takes it away; model_provides
 *                                 7  manifests as packs: encoded and read back, every refusal, the pool, and a fuzz
 *                                 8  the unmet-need state: a tank whose pair cannot run is PAUSED like one whose pack is
 *                                    missing (uids kept, defaults drawn, the card says what it needs), and resumes
 *
 * Both worlds' builds run it: the simulator carries both worlds' built-in packs and its firmware is the union of both
 * (caps.h CAPS_FW), so a pair of the other world is listed and loaded, and refused at the tank by the model's goal words.
 * Nothing here pokes a registry or progression's statics: packs go in as files (packs_add), saves as bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <sys/stat.h>
#include "tank.h"
#include "progression.h"
#include "notice.h"
#include "render.h"
#include "version.h"
#include "packs/caps.h"
#include "packs/compat.h"
#include "packs/manifest.h"
#include "packs/ptpk.h"
#include "packs/ptpk_write.h"
#include "packs/builtin.h"
#include "packs_dir.h"
#include "sim_tmp.h"

static const char *s_name = "selftest-caps";
#define PF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static tank_t s_t;                                           /* (tank_t is big: never on the stack) */
static const species_t *s_ws;                                /* this build's default species ... */
static const habitat_t *s_wh;                                /* ... and its habitat */

#define OTHER_WORLD (PT_TERRARIUM ? PT_WORLD_AQUARIUM : PT_WORLD_TERRARIUM)      /* the world's bit in a WORLDS mask: the other one's */
#define OTHER_TAG   (PT_TERRARIUM ? PT_MODEL_TAG_AQUARIUM : PT_MODEL_TAG_TERRARIUM)

/* a goal word of the other world's model that this build's model has not, and the card's short need for it */
static const char *other_goal(void) { return PT_TERRARIUM ? FISH_GOAL_NAMES[2] : LZ_GOAL_NAMES[LZ_INVESTIGATE]; }
static const char *other_goal_need(void) { return PT_TERRARIUM ? "GOAL VISIT BUBBLES" : "GOAL INVESTIGATE"; }

static uint32_t fnv(const char *s) { return ptpk_fnv1a((const uint8_t *)s, strlen(s)); }
static caps_t lowest(caps_t set) { return set & (~set + 1u); }
static void upper_goal(char *out, size_t n, const char *word) {       /* the card's short need for a goal: GOAL, the word upper case, underscores spaces */
    snprintf(out, n, "GOAL ");
    size_t at = strlen(out);
    for (; *word && at + 1 < n; word++, at++) out[at] = *word == '_' ? ' ' : (*word >= 'a' && *word <= 'z') ? (char)(*word - 'a' + 'A') : *word;
    out[at] = 0;
}
static const char *first_missing_goal(const species_t *p, const model_manifest_t *m) {
    for (int k = 0; k < p->n_goals && k < SPECIES_GOALS_MAX; k++) if (!manifest_has_goal(m, p->goal[k])) return p->goal[k];
    return NULL;
}

/* ================================================================ files as bytes, edited in place (selftest_ptpk.c's, not shared: its statics are its own) */
typedef struct { uint8_t b[PTPK_FILE_MAX + 4096]; size_t n; } blob_t;
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static void blob_copy(blob_t *k, const uint8_t *b, size_t n) { memcpy(k->b, b, n); k->n = n; }
static bool rec_find(const blob_t *k, unsigned id, size_t *at, size_t *vlen) {       /* the record's tag offset and its value's length */
    for (size_t o = PTPK_HEADER_BYTES; o + 4 <= k->n; ) {
        const unsigned tag = rd16(k->b + o), l = rd16(k->b + o + 2);
        if (o + 4 + l > k->n) return false;
        if ((tag & 0x7fffu) == id) { *at = o; *vlen = l; return true; }
        o += 4 + l;
    }
    return false;
}
static void blob_fix(blob_t *k) {                    /* the header from the body: len, the uid of UID_NS, the crc */
    size_t at, l;
    wr32(k->b + 12, (uint32_t)(k->n - PTPK_HEADER_BYTES));
    if (rec_find(k, 2, &at, &l)) wr32(k->b + 8, ptpk_fnv1a(k->b + at + 4, l));
    wr32(k->b + 16, ptpk_crc32(k->b + PTPK_HEADER_BYTES, k->n - PTPK_HEADER_BYTES));
}
static void rec_append(blob_t *k, unsigned tag, const void *v, size_t l) {
    wr16(k->b + k->n, tag); wr16(k->b + k->n + 2, (uint32_t)l);
    if (l) memcpy(k->b + k->n + 4, v, l);
    k->n += 4 + l;
}
static void rec_delete(blob_t *k, unsigned id) {
    size_t at, l;
    if (!rec_find(k, id, &at, &l)) return;
    memmove(k->b + at, k->b + at + 4 + l, k->n - (at + 4 + l));
    k->n -= 4 + l;
}
static void rec_set(blob_t *k, unsigned id, const void *v, size_t l) {      /* the value replaced (the tag's REQUIRED bit kept), the length may change */
    size_t at, ol;
    if (!rec_find(k, id, &at, &ol)) return;
    const unsigned tag = rd16(k->b + at);
    memmove(k->b + at + 4 + l, k->b + at + 4 + ol, k->n - (at + 4 + ol));
    wr16(k->b + at, tag); wr16(k->b + at + 2, (uint32_t)l);
    if (l) memcpy(k->b + at + 4, v, l);
    k->n = k->n - ol + l;
}
static bool rec_dup(blob_t *k, unsigned id) {         /* the record once more at the end, as it is */
    size_t at, l;
    if (!rec_find(k, id, &at, &l)) return false;
    uint8_t v[256];
    if (l > sizeof v) return false;
    const unsigned tag = rd16(k->b + at);
    memcpy(v, k->b + at + 4, l);
    rec_append(k, tag, v, l);
    return true;
}
static bool rec_u32_get(const blob_t *k, unsigned id, size_t off, uint32_t *v) {
    size_t at, l;
    if (!rec_find(k, id, &at, &l) || off + 4 > l) return false;
    *v = rd32(k->b + at + 4 + off);
    return true;
}
static bool rec_u32_set(blob_t *k, unsigned id, size_t off, uint32_t v) {
    size_t at, l;
    if (!rec_find(k, id, &at, &l) || off + 4 > l) return false;
    wr32(k->b + at + 4 + off, v);
    return true;
}
static bool rec_u8_set(blob_t *k, unsigned id, size_t off, uint8_t v) {
    size_t at, l;
    if (!rec_find(k, id, &at, &l) || off + 1 > l) return false;
    k->b[at + 4 + off] = v;
    return true;
}
/* a new pack made of a built-in's file: UID_NS and NAME get new last characters (the length stays); the header is made good */
static void blob_variant(blob_t *k, const uint8_t *b, size_t n, const char *ns_tail, const char *name_tail) {
    blob_copy(k, b, n);
    size_t at, l;
    if (rec_find(k, 2, &at, &l)) memcpy(k->b + at + 4 + l - strlen(ns_tail), ns_tail, strlen(ns_tail));
    if (rec_find(k, 1, &at, &l)) memcpy(k->b + at + 4 + l - strlen(name_tail), name_tail, strlen(name_tail));
    blob_fix(k);
}
static void set_worlds(blob_t *k, unsigned v) {
    const uint8_t b = (uint8_t)v;
    size_t at, l;
    if (rec_find(k, 0x10, &at, &l)) rec_set(k, 0x10, &b, 1); else rec_append(k, 0x10, &b, 1);
    blob_fix(k);
}

/* the built-in packs as files: what the firmware loads at boot, encoded from the const structs (builtin_encode) once, in the table's order */
typedef struct { const builtin_pack_t *def; uint8_t blob[PTPK_FILE_MAX]; size_t len; } enc_t;
static enc_t s_enc[PTPK_BUILTIN_N];
static int enc_all(void) {
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {
        const char *why = "";
        s_enc[i].def = &PTPK_BUILTIN[i];
        s_enc[i].len = builtin_encode(&PTPK_BUILTIN[i], s_enc[i].blob, sizeof s_enc[i].blob, &why);
        if (!s_enc[i].len) PF("built-in pack %s cannot be encoded: %s", PTPK_BUILTIN[i].uid_ns, why);
    }
    return 0;
}
static const enc_t *enc_with_uid(uint32_t uid) {
    for (int i = 0; i < PTPK_BUILTIN_N; i++) if (rd32(s_enc[i].blob + 8) == uid) return &s_enc[i];
    return NULL;
}
/* offered to the loader the way a file read from a folder is: from an exact-size heap copy (a read past its end is a bug a checker sees) */
static packs_result_t offer(const uint8_t *b, size_t n, packs_info_t *info, char *why, size_t why_n) {
    uint8_t *c = malloc(n ? n : 1);
    memcpy(c, b, n);
    const packs_result_t r = packs_add(c, n, PACKS_FILE, info, why, why_n);
    free(c);
    return r;
}
static packs_result_t offer_blob(const blob_t *k, packs_info_t *info, char *why, size_t why_n) { return offer(k->b, k->n, info, why, why_n); }

static uint32_t s_rng = 0x3c6ef372u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* the registries' own state, to see a refusal changed nothing */
typedef struct { int ns, nh, nm; uint32_t gen; } reg_t;
static reg_t reg_now(void) { reg_t r = { species_count(), habitat_count(), manifest_count(), habitat_generation() }; return r; }
static bool reg_same(reg_t a, reg_t b) { return a.ns == b.ns && a.nh == b.nh && a.nm == b.nm && a.gen == b.gen; }
static bool reg_counts(reg_t a, reg_t b) { return a.ns == b.ns && a.nh == b.nh && a.nm == b.nm; }

/* what loaded is a well-formed pack that passes its own rules */
static int check_loaded(const packs_info_t *info, const char *what) {
    char why[200];
    if (info->kind == PTPK_KIND_SPECIES) {
        const species_t *p = species_by_uid(info->uid);
        if (!p) PF("%s: loaded but not found by uid (%08x)", what, (unsigned)info->uid);
        if (!species_check(p, why, sizeof why)) PF("%s: a species that fails its rules was loaded: %s", what, why);
        if (strnlen(p->name, PACK_NAME_MAX) >= PACK_NAME_MAX || !p->habitats) PF("%s: a species with a bad shape was loaded", what);
    } else if (info->kind == PTPK_KIND_HABITAT) {
        const habitat_t *h = habitat_by_uid(info->uid);
        if (!h) PF("%s: loaded but not found by uid (%08x)", what, (unsigned)info->uid);
        if (!habitat_check(h, why, sizeof why)) PF("%s: a habitat that fails its rules was loaded: %s", what, why);
    } else {
        const model_manifest_t *m = manifest_by_uid(info->uid);
        if (!m) PF("%s: loaded but not found by uid (%08x)", what, (unsigned)info->uid);
        if (!manifest_check(m, why, sizeof why)) PF("%s: a manifest that fails its rules was loaded: %s", what, why);
        if (strnlen(m->name, PACK_NAME_MAX) >= PACK_NAME_MAX || strnlen(m->tag, MODEL_TAG_MAX) >= MODEL_TAG_MAX) PF("%s: a manifest with an unterminated text was loaded", what);
    }
    return 0;
}
static int s_cases;                                          /* the cases expect() has run */
/* the file offered: the result wanted; a refusal says `word` and changes nothing; what loads is well formed (left loaded: the caller drops it) */
static int expect(const blob_t *k, packs_result_t want, const char *word, const char *what, packs_info_t *out) {
    char why[300] = ""; packs_info_t info;
    memset(&info, 0, sizeof info);
    const reg_t before = reg_now();
    s_cases++;
    const packs_result_t r = offer_blob(k, &info, why, sizeof why);
    if (r != want) { if (r == PACKS_ADDED) packs_drop_loaded(); PF("%s: %s (%s)", what, r == PACKS_ADDED ? "it loaded" : r == PACKS_SAME ? "it was called the same" : "it was refused", why); }
    if (r == PACKS_REFUSED) {
        if (word && !strstr(why, word)) PF("%s: refused, but for \"%s\" (want one with \"%s\")", what, why, word);
        if (!reg_same(before, reg_now())) PF("%s: refused, and a registry moved", what);
    } else if (r == PACKS_ADDED && check_loaded(&info, what)) { packs_drop_loaded(); return 1; }
    if (out) *out = info;
    return 0;
}
#define RF(k, word, what) do { if (expect(&(k), PACKS_REFUSED, word, what, NULL)) return 1; } while (0)
#define RA(k, info, what) do { if (expect(&(k), PACKS_ADDED, NULL, what, &(info))) return 1; } while (0)

/* ================================================================ 1: every built-in pair */
static int t_pairs(void) {
    char why[300], why2[300], need[40];
    const model_manifest_t *model = model_provides();
    if (!model || strcmp(model->tag, PT_MODEL_TAG) != 0) PF("no manifest for this build's model %s", PT_MODEL_TAG);
    const int ns = species_count(), nh = habitat_count();
    int pairs = 0, listed = 0, held = 0, by_model = 0, unlisted = 0;
    for (int i = 0; i < ns; i++) for (int j = 0; j < nh; j++) {
        const species_t *p = species_at(i); const habitat_t *h = habitat_at(j);
        pairs++;
        const bool lists = ((p->habitats >> h->id) & 1u) != 0;
        const char *lacks = first_missing_goal(p, model);
        const bool want = lists && !lacks;
        const bool ok = packs_check(p, h, need, sizeof need, why, sizeof why);
        if (ok != want) PF("%s in the %s: packs_check says %d (%s), the species lists it %d and the model %s", p->name, h->name, ok, why, lists, lacks ? "lacks a goal" : "has every goal");
        if (packs_compatible(p, h, why2, sizeof why2) != ok || strcmp(why2, why) != 0) PF("%s in the %s: packs_compatible and packs_check disagree", p->name, h->name);
        tank_init(&s_t, 3);
        const uint8_t h0 = s_t.habitat_id, p0 = s_t.pack_id; const bool m0 = s_t.packs_missing;
        const bool took = tank_set_packs(&s_t, h->id, p->id, why2, sizeof why2);
        if (took != ok) PF("%s in the %s: a tank says %d, packs_compatible %d (%s)", p->name, h->name, took, ok, why2);
        if (ok) {
            if (why[0] || need[0] || why2[0]) PF("%s in the %s is held and gave a reason \"%s\" / need \"%s\"", p->name, h->name, why, need);
            if (s_t.habitat_id != h->id || s_t.pack_id != p->id || s_t.packs_missing) PF("a tank took %s in the %s and does not hold it", p->name, h->name);
            if (tank_species(&s_t) != p || tank_habitat(&s_t) != h) PF("a tank took %s in the %s and reads another pair", p->name, h->name);
            if (!world_offers(p->worlds)) PF("%s is held in the %s build and its WORLDS %02x leave this world out", p->name, PT_WORLD_NAME, p->worlds);
            listed++; held++;
            continue;
        }
        if (s_t.habitat_id != h0 || s_t.pack_id != p0 || s_t.packs_missing != m0) PF("a refusal of %s in the %s moved the tank (%d/%d -> %d/%d)", p->name, h->name, h0, p0, s_t.habitat_id, s_t.pack_id);
        if (strcmp(why2, why) != 0 || !why[0] || !need[0]) PF("%s in the %s: the tank's reason \"%s\" is not packs_check's \"%s\" (need \"%s\")", p->name, h->name, why2, why, need);
        if (!lists) {
            if (!strstr(why, "does not live in") || !strstr(why, p->name) || !strstr(why, h->name) || strcmp(need, "ANOTHER HABITAT") != 0)
                PF("%s in the %s is not listed, and was refused as \"%s\" / \"%s\"", p->name, h->name, why, need);
            unlisted++;
        } else {                                               /* listed: the other world's - its goal words are not in this build's model */
            char word[24]; snprintf(word, sizeof word, "\"%s\"", lacks);
            if (!strstr(why, "needs the goal") || !strstr(why, word) || !strstr(why, model->tag)) PF("%s in the %s is listed and refused, but not for a goal of the model: %s", p->name, h->name, why);
            char up[24]; upper_goal(up, sizeof up, lacks);
            if (strcmp(need, up) != 0) PF("%s in the %s: the short need is \"%s\", not \"%s\"", p->name, h->name, need, up);
            if (world_offers(p->worlds)) PF("%s is refused for a goal of another model and its WORLDS %02x say it is this world's", p->name, p->worlds);
            listed++; by_model++;
        }
    }
    /* the species' own bits are what its listed uids find: an independent count of the listed pairs */
    int listed2 = 0;
    for (int i = 0; i < ns; i++) for (int m = 0; m < species_at(i)->n_habitat_uids; m++) listed2 += habitat_by_uid(species_at(i)->habitat_uid[m]) != NULL;
    if (listed != listed2) PF("%d listed pairs by the masks, %d by the species' uids", listed, listed2);
    if (pairs != ns * nh || pairs != listed + unlisted || held < 1 || by_model < 1 || unlisted < 1) PF("the pair classes do not add up: %d pairs, %d held, %d refused by the model, %d unlisted", pairs, held, by_model, unlisted);
    tank_init(&s_t, 3);
    if (s_t.habitat_id != HABITAT_DEFAULT || s_t.pack_id != SPECIES_DEFAULT || s_t.packs_missing) PF("a new tank holds habitat %d / species %d, not the defaults", s_t.habitat_id, s_t.pack_id);
    if (!packs_compatible(tank_species(&s_t), tank_habitat(&s_t), why, sizeof why)) PF("the defaults are not a pair: %s", why);
    /* A16's test pack lives in the desert and in the warm desert (a terrarium's); the aquarium's model has not its goals */
    if (PT_TERRARIUM) {
        if (!tank_set_packs(&s_t, HABITAT_DESERT, SPECIES_TEST_WARM, why, sizeof why) || !tank_set_packs(&s_t, HABITAT_TEST_WARM, SPECIES_TEST_WARM, why, sizeof why)) PF("the test pack is refused: %s", why);
    } else if (tank_set_packs(&s_t, HABITAT_DESERT, SPECIES_TEST_WARM, why, sizeof why) || !strstr(why, "needs the goal")) {
        PF("the aquarium's tank took the test pack, or refused it for another reason: %s", why);
    }
    tank_init(&s_t, 3);
    printf("selftest-caps: %d built-in pairs (%d species x %d habitats): %d are listed by their species, %d of those held in the %s build (packs_compatible, and a tank holds each), "
           "%d listed of the other world refused naming a goal word the %s model lacks (the tank, its pair and its missing flag untouched), %d unlisted refused 'does not live in' (ANOTHER HABITAT); "
           "a new tank holds the defaults; the test pack %s\n",
           pairs, ns, nh, listed, held, PT_WORLD_NAME, by_model, PT_MODEL_TAG, unlisted, PT_TERRARIUM ? "lives in the desert and the warm desert" : "is refused by the model");
    return 0;
}

/* ================================================================ 2: the derived sets */
static int t_derived(void) {
    char l[160];
    const species_t *gk = species_get(SPECIES_GECKO), *fish = species_get(SPECIES_FISH), *tw = species_get(SPECIES_TEST_WARM);
    const habitat_t *desert = habitat_get(HABITAT_DESERT), *fresh = habitat_get(HABITAT_FRESHWATER), *twh = habitat_get(HABITAT_TEST_WARM);
    if (!gk || !fish || !tw || !desert || !fresh || !twh) PF("a built-in pack is not loaded (the simulator carries them all)");
    if (species_needs(gk) != (CAP_WALK | CAP_LAND | CAP_BASK | CAP_HIDES | CAP_CRICKET | CAP_MEALWORM)) { caps_list(species_needs(gk), l, sizeof l); PF("the gecko needs %s", l); }
    if (species_needs(tw) != species_needs(gk)) PF("the test pack's needs are not the gecko's");
    if (species_needs(fish) != (CAP_SWIM | CAP_WATER | CAP_PELLET)) { caps_list(species_needs(fish), l, sizeof l); PF("the fish needs %s", l); }
    if (habitat_provides(desert) != (CAP_LAND | CAP_BASK | CAP_LAMP | CAP_HIDES | CAP_CRICKET | CAP_MEALWORM | CAP_WALK | CAP_SLITHER)) { caps_list(habitat_provides(desert), l, sizeof l); PF("the desert provides %s", l); }
    if (habitat_provides(twh) != habitat_provides(desert)) PF("the warm desert does not provide what the desert does");
    if (habitat_provides(fresh) != (CAP_WATER | CAP_PELLET | CAP_SWIM)) { caps_list(habitat_provides(fresh), l, sizeof l); PF("the freshwater provides %s", l); }
    if (species_moves(gk) != CAP_WALK || species_moves(tw) != CAP_WALK || species_moves(fish) != CAP_SWIM) PF("the species' modes are %08x / %08x / %08x", (unsigned)species_moves(gk), (unsigned)species_moves(tw), (unsigned)species_moves(fish));
    if (caps_moves_of_areas(CAP_LAND | CAP_BASK) != (CAP_WALK | CAP_SLITHER) || caps_moves_of_areas(CAP_WATER) != CAP_SWIM || caps_moves_of_areas(CAP_LAND | CAP_WATER | CAP_SHORE) != (CAP_WALK | CAP_SLITHER | CAP_SWIM))
        PF("the movement the areas carry is wrong");
    /* the names, the order and the index of every capability, and the bits nobody names */
    const char *const NM[14] = { "SWIM", "WALK", "SLITHER", NULL, "WATER", "LAND", "BASK", "SHORE", "CRICKET", "MEALWORM", "GREENS", "PELLET", "LAMP", "HIDES" };
    for (unsigned k = 0; k < 32; k++) {
        const char *n = caps_name(k);
        if (k < 14 ? (NM[k] ? !n || strcmp(n, NM[k]) != 0 : n != NULL) : n != NULL) PF("caps_name(%u) is %s", k, n ? n : "NULL");
        if (caps_index(1u << k) != (int)k) PF("caps_index(1 << %u) is %d", k, caps_index(1u << k));
        if ((((1u << k) & CAPS_KNOWN) != 0) != (k < 14 && k != 3)) PF("bit %u is %s known", k, ((1u << k) & CAPS_KNOWN) ? "" : "not");
    }
    if (caps_index(0) != -1 || caps_index(CAP_WALK | CAP_SWIM) != -1 || caps_unknown(CAPS_KNOWN) != -1 || caps_unknown(CAPS_KNOWN | (1u << 3)) != 3 || caps_unknown(1u << 20) != 20) PF("caps_index / caps_unknown are wrong");
    caps_list(CAP_BASK | CAP_HIDES | (1u << 20) | CAP_SWIM, l, sizeof l);
    if (strcmp(l, "SWIM, BASK, HIDES, bit 20") != 0) PF("caps_list reads \"%s\"", l);
    { char tiny[6]; caps_list(CAPS_KNOWN, tiny, sizeof tiny); if (strlen(tiny) > 5) PF("caps_list overran its buffer"); caps_list(0, l, sizeof l); if (l[0]) PF("caps_list of nothing is \"%s\"", l); }
    /* every built-in's declared capabilities agree with the fields they were derived from */
    int checked = 0;
    for (int i = 0; i < species_count(); i++) {
        const species_t *p = species_at(i);
        const bool walks = (species_moves(p) & CAP_WALK) != 0;
        bool any = false;
        for (int k = 0; k < 4; k++) any |= p->walk_px_s[k] != 0;
        any |= p->bout_s[0] != 0 || p->bout_s[1] != 0 || p->pause_s[0] != 0 || p->pause_s[1] != 0 || p->turn_s != 0;
        if (walks != any) PF("%s: a WALK entry %d, walk numbers %d", p->name, walks, any);
        if ((species_needs(p) & CAPS_FOODS) != ((caps_t)p->diet << CAP_FOOD_SHIFT)) PF("%s: the foods it needs (%08x) are not its diet (%02x) shifted", p->name, (unsigned)(species_needs(p) & CAPS_FOODS), p->diet);
        if (p->media_needs & ~CAPS_MEDIA) PF("%s: media_needs %08x lists more than areas and fittings", p->name, (unsigned)p->media_needs);
        if (p->media_wants & ~CAPS_KNOWN) PF("%s: media_wants %08x lists more than known capabilities", p->name, (unsigned)p->media_wants);
        if (p->sheds && !(p->media_needs & CAP_HIDES)) PF("%s sheds and does not need HIDES", p->name);
        if (p->n_loco < 1 || p->n_loco > SPECIES_LOCO_MAX) PF("%s: %d ways of moving", p->name, p->n_loco);
        for (int k = 0; k < p->n_loco; k++) {
            if (caps_index(p->loco[k].mode) < 0 || !(p->loco[k].mode & CAPS_MOVES) || caps_index(p->loco[k].area) < 0 || !(p->loco[k].area & CAPS_AREAS)) PF("%s: way of moving %d is not a mode in an area", p->name, k);
            if (!(species_needs(p) & p->loco[k].area) || !(species_needs(p) & p->loco[k].mode)) PF("%s: way of moving %d is not among what it needs", p->name, k);
        }
        if ((species_needs(p) & CAP_SHORE) && (species_needs(p) & (CAP_WATER | CAP_LAND)) != (CAP_WATER | CAP_LAND)) PF("%s needs a SHORE and not both areas", p->name);
        if (p->n_goals < 1) PF("%s names no goal", p->name);
        for (int k = 0; k < p->n_goals; k++) if (!manifests_know_goal(p->goal[k])) PF("%s names the goal \"%s\" and no manifest lists it", p->name, p->goal[k]);
        checked++;
    }
    for (int j = 0; j < habitat_count(); j++) {
        const habitat_t *h = habitat_at(j);
        if (((h->areas & CAP_BASK) != 0) != (h->bask_r_px > 0) || ((h->provides & CAP_LAMP) != 0) != (h->bask_r_px > 0)) PF("%s: BASK / LAMP disagree with the lamp's pool (%.0f px)", h->name, h->bask_r_px);
        if (((h->provides & CAP_HIDES) != 0) != (h->n_hides > 0)) PF("%s: HIDES disagree with its %d hides", h->name, h->n_hides);
        if (((h->provides & CAP_MEALWORM) != 0) != h->dish) PF("%s: MEALWORM disagrees with its dish", h->name);
        if ((h->areas & ~CAPS_AREAS) || (h->provides & ~CAPS_PROVIDED)) PF("%s lists more than areas / provisions (%08x / %08x)", h->name, (unsigned)h->areas, (unsigned)h->provides);
        if ((h->provides & (CAP_CRICKET | CAP_MEALWORM)) && !(h->areas & CAP_LAND)) PF("%s: crickets without land", h->name);
        if ((h->provides & CAP_PELLET) && !(h->areas & CAP_WATER)) PF("%s: pellets without water", h->name);
        if ((habitat_provides(h) & CAPS_MOVES) != caps_moves_of_areas(h->areas)) PF("%s: the movement it provides is not its areas'", h->name);
        if (!(habitat_provides(h) & h->areas) || (habitat_provides(h) & h->provides) != h->provides) PF("%s: habitat_provides forgot its own", h->name);
        checked++;
    }
    /* the firmware's capabilities: the union in the simulator, and the override's two directions */
    const caps_t fw = firmware_caps();
    if (fw != (CAPS_FW_TERRARIUM | CAPS_FW_AQUARIUM) || fw != CAPS_FW) PF("the simulator's firmware provides %08x, not both worlds' %08x", (unsigned)fw, (unsigned)(CAPS_FW_TERRARIUM | CAPS_FW_AQUARIUM));
    if ((CAPS_FW_TERRARIUM & CAPS_FW_AQUARIUM) != 0) PF("the two worlds' firmware share a capability");
    firmware_caps_override(true, CAP_WALK | CAP_LAND);
    if (firmware_caps() != (CAP_WALK | CAP_LAND)) PF("the override on provides %08x, not WALK | LAND", (unsigned)firmware_caps());
    firmware_caps_override(true, 0);
    if (firmware_caps() != 0) PF("the override on with nothing provides %08x", (unsigned)firmware_caps());
    firmware_caps_override(false, CAP_SWIM);
    if (firmware_caps() != fw) PF("the override off provides %08x, not the simulator's %08x", (unsigned)firmware_caps(), (unsigned)fw);
    printf("selftest-caps: derived sets: the gecko needs WALK LAND BASK HIDES CRICKET MEALWORM and the fish SWIM WATER PELLET; the desert provides LAND BASK LAMP HIDES CRICKET MEALWORM and the movement its land carries (WALK SLITHER), "
           "the freshwater WATER PELLET SWIM; %d built-ins agree with the fields their capabilities come from (walk numbers <=> a WALK entry, diet <=> foods, lamp <=> bask_r_px, hides <=> n_hides, MEALWORM <=> dish, sheds => HIDES); "
           "the names of all 32 bits; the firmware provides both worlds' union in the simulator, and the override sets it exactly and gives it back\n", checked);
    return 0;
}

/* ================================================================ 3: the reasons, verbatim */
static int t_reasons(void) {
    char why[300], need[40], want[300], l[120], up[40];
    const species_t *gk = species_get(SPECIES_GECKO);
    const habitat_t *fresh = habitat_get(HABITAT_FRESHWATER), *desert = habitat_get(HABITAT_DESERT);
    const model_manifest_t *model = model_provides();
    /* a species that lists a habitat that has nothing it needs */
    species_t g = *gk; g.habitats |= 1u << HABITAT_FRESHWATER;
    if (packs_check(&g, fresh, need, sizeof need, why, sizeof why)) PF("a gecko lives in the freshwater");
    const caps_t lack = species_needs(&g) & ~habitat_provides(fresh);
    if (lack != (CAP_WALK | CAP_LAND | CAP_BASK | CAP_HIDES | CAP_CRICKET | CAP_MEALWORM)) PF("the freshwater lacks %08x for the gecko", (unsigned)lack);
    caps_list(lack, l, sizeof l);
    snprintf(want, sizeof want, "%s needs %s; %s has none", g.name, l, fresh->name);
    if (strcmp(why, want) != 0) PF("the reason reads \"%s\", want \"%s\"", why, want);
    char freshwater_why[300]; snprintf(freshwater_why, sizeof freshwater_why, "%s", why);
    if (!strstr(why, "LEOPARD GECKO") || !strstr(why, "FRESHWATER") || !strstr(why, "has none") || !strstr(why, "BASK")) PF("the reason lacks a name: %s", why);
    if (strcmp(need, caps_name((unsigned)caps_index(lowest(lack)))) != 0 || strcmp(need, "WALK") != 0) PF("the short need is \"%s\", the lowest lacking bit's name is WALK", need);
    /* the desert with the lamp taken out: BASK alone */
    habitat_t d = *desert; d.areas &= ~CAP_BASK; d.provides &= ~CAP_LAMP;
    if (packs_check(gk, &d, need, sizeof need, why, sizeof why) || strcmp(why, "LEOPARD GECKO needs BASK; DESERT has none") != 0 || strcmp(need, "BASK") != 0) PF("a desert without a lamp: \"%s\" / \"%s\"", why, need);
    if (packs_compatible(gk, &d, why, sizeof why) || strcmp(why, "LEOPARD GECKO needs BASK; DESERT has none") != 0) PF("packs_compatible: \"%s\"", why);
    /* ... on a firmware without the walk module: the firmware leg comes first, and names it */
    if (packs_check_with(gk, &d, CAPS_FW_TERRARIUM & ~CAP_WALK, model, need, sizeof need, why, sizeof why) ||
        strcmp(why, "LEOPARD GECKO needs WALK; this firmware has none") != 0 || strcmp(need, "WALK") != 0) PF("a firmware without WALK: \"%s\" / \"%s\"", why, need);
    if (packs_check_with(gk, desert, CAPS_FW_TERRARIUM & ~(CAP_BASK | CAP_HIDES), model, need, sizeof need, why, sizeof why) ||
        strcmp(why, "LEOPARD GECKO needs BASK, HIDES; this firmware has none") != 0 || strcmp(need, "BASK") != 0) PF("a firmware without BASK and HIDES: \"%s\" / \"%s\"", why, need);
    /* not listed: another habitat */
    if (packs_check(gk, fresh, need, sizeof need, why, sizeof why) || strcmp(why, "LEOPARD GECKO does not live in FRESHWATER") != 0 || strcmp(need, "ANOTHER HABITAT") != 0) PF("a gecko in the freshwater: \"%s\" / \"%s\"", why, need);
    if (packs_check(NULL, desert, need, sizeof need, why, sizeof why) || packs_check(gk, NULL, need, sizeof need, why, sizeof why) || !strstr(why, "no such pack")) PF("a missing pack is not refused by name: %s", why);
    /* a goal the model lacks: a word of no model, and a word of the other world's */
    species_t w = *s_ws;
    snprintf(w.goal[0], GOAL_NAME_MAX, "climb");
    if (packs_check(&w, s_wh, need, sizeof need, why, sizeof why)) PF("%s lives where its model has no goal \"climb\"", w.name);
    snprintf(want, sizeof want, "%s needs the goal \"climb\"; the %s model has none", w.name, PT_MODEL_TAG);
    if (strcmp(why, want) != 0 || strcmp(need, "GOAL CLIMB") != 0) PF("the goal's reason reads \"%s\" / \"%s\", want \"%s\" / GOAL CLIMB", why, need, want);
    w = *s_ws; snprintf(w.goal[0], GOAL_NAME_MAX, "%s", other_goal());
    if (packs_compatible(&w, s_wh, why, sizeof why) || !strstr(why, other_goal()) || !strstr(why, PT_MODEL_TAG) || !strstr(why, "needs the goal")) PF("a goal of the other world's model: \"%s\"", why);
    packs_check(&w, s_wh, need, sizeof need, NULL, 0);
    if (strcmp(need, other_goal_need()) != 0) PF("the short need for \"%s\" is \"%s\", not \"%s\"", other_goal(), need, other_goal_need());
    w = *s_ws; snprintf(w.goal[1], GOAL_NAME_MAX, "go_up_high");       /* the FIRST goal that is missing, its underscores spaces */
    packs_check(&w, s_wh, need, sizeof need, why, sizeof why);
    if (strcmp(need, "GOAL GO UP HIGH") != 0 || !strstr(why, "\"go_up_high\"")) PF("the second goal missing: \"%s\" / \"%s\"", why, need);
    w = *s_ws; snprintf(w.goal[0], GOAL_NAME_MAX, "a_b_c"); snprintf(w.goal[1], GOAL_NAME_MAX, "x9");
    packs_check(&w, s_wh, need, sizeof need, why, sizeof why);
    if (strcmp(need, "GOAL A B C") != 0) PF("two goals missing: the short need is \"%s\", the first's", need);
    /* no manifest at all, and another model's */
    if (packs_check_with(s_ws, s_wh, firmware_caps(), NULL, need, sizeof need, why, sizeof why) || !strstr(why, "no model's goal list")) PF("no model loaded: \"%s\"", why);
    upper_goal(up, sizeof up, s_ws->goal[0]);
    if (strcmp(need, up) != 0) PF("no model loaded: the short need is \"%s\", not \"%s\"", need, up);
    const model_manifest_t *other = manifest_by_tag(OTHER_TAG);
    if (!other) PF("the other world's manifest %s is not loaded (the simulator carries both)", OTHER_TAG);
    const char *lacks = first_missing_goal(s_ws, other);
    if (!lacks) PF("%s has every goal of the %s model", s_ws->name, OTHER_TAG);
    if (packs_check_with(s_ws, s_wh, firmware_caps(), other, need, sizeof need, why, sizeof why) || !strstr(why, OTHER_TAG) || !strstr(why, lacks)) PF("another model: \"%s\"", why);
    if (!packs_check_with(s_ws, s_wh, firmware_caps(), model, need, sizeof need, why, sizeof why) || need[0] || why[0]) PF("the world's own pair is refused: \"%s\"", why);
    /* the buffers: a short one is cut and terminated, never overrun */
    {
        struct { char need[4]; char g1[8]; char why[12]; char g2[8]; } sb;      /* (guards on either side of each short buffer) */
        memset(&sb, 0x5a, sizeof sb);
        packs_check(&g, fresh, sb.need, sizeof sb.need, sb.why, sizeof sb.why);
        packs_check(&g, fresh, need, sizeof need, why, sizeof why);
        packs_check(&g, fresh, NULL, 0, NULL, 0);
        for (size_t k = 0; k < sizeof sb.g1; k++) if (sb.g1[k] != 0x5a || sb.g2[k] != 0x5a) PF("a short buffer was overrun");
        if (strlen(sb.need) != 3 || strncmp(sb.need, need, 3) != 0 || strlen(sb.why) != 11 || strncmp(sb.why, why, 11) != 0) PF("a short buffer: need \"%s\", reason \"%s\"", sb.need, sb.why);
    }
    /* THE FILTER: packs_offered = both packs' WORLDS + not test + held */
    int offered = 0;
    for (int i = 0; i < species_count(); i++) for (int j = 0; j < habitat_count(); j++) offered += packs_offered(species_at(i), habitat_at(j));
    if (offered != 1 || !packs_offered(s_ws, s_wh)) PF("%d built-in pairs are offered; only this build's own, non-test pair should be", offered);
    if (packs_offered(species_get(SPECIES_TEST_WARM), desert) || packs_offered(species_get(SPECIES_TEST_WARM), habitat_get(HABITAT_TEST_WARM)) || packs_offered(gk, habitat_get(HABITAT_TEST_WARM))) PF("a test pack is offered");
    const species_t *os = species_get(PT_TERRARIUM ? SPECIES_FISH : SPECIES_GECKO); const habitat_t *oh = habitat_get(PT_TERRARIUM ? HABITAT_FRESHWATER : HABITAT_DESERT);
    if (packs_offered(os, oh) || packs_offered(NULL, oh) || packs_offered(os, NULL)) PF("the other world's pair is offered");
    species_t ws = *s_ws; habitat_t wh = *s_wh;
    ws.worlds = 0;               if (!packs_offered(&ws, s_wh)) PF("a species of every world is not offered");
    ws.worlds = OTHER_WORLD;     if (packs_offered(&ws, s_wh) || !packs_compatible(&ws, s_wh, why, sizeof why)) PF("a species of the other world alone is offered (or not held: %s)", why);
    ws.worlds = PT_WORLD;        if (!packs_offered(&ws, s_wh)) PF("a species of this world is not offered");
    ws.worlds = 3;               if (!packs_offered(&ws, s_wh)) PF("a species of both worlds is not offered");
    ws.worlds = 0x80;            if (packs_offered(&ws, s_wh)) PF("a species of a world nobody knows is offered");
    ws = *s_ws; ws.test_only = true; if (packs_offered(&ws, s_wh)) PF("a test species is offered");
    wh.worlds = 0;               if (!packs_offered(s_ws, &wh)) PF("a habitat of every world is not offered");
    wh.worlds = OTHER_WORLD;     if (packs_offered(s_ws, &wh) || !packs_compatible(s_ws, &wh, why, sizeof why)) PF("a habitat of the other world alone is offered (or not held: %s)", why);
    wh.worlds = 3;               if (!packs_offered(s_ws, &wh)) PF("a habitat of both worlds is not offered");
    wh = *s_wh; wh.test_only = true; if (packs_offered(s_ws, &wh)) PF("a test habitat is offered");
    if (!world_offers(0) || !world_offers(PT_WORLD) || !world_offers(3) || world_offers(OTHER_WORLD) || world_offers(0x80)) PF("world_offers is wrong");
    printf("selftest-caps: reasons verbatim: a gecko in the freshwater (\"%s\", short need WALK, the lowest lacking bit); the desert without its lamp 'LEOPARD GECKO needs BASK; DESERT has none' (need BASK); "
           "a firmware without WALK 'needs WALK; this firmware has none'; not listed 'does not live in' (ANOTHER HABITAT); a goal of no model, of the other world's, the second of two, none loaded, another model's "
           "(GOAL CLIMB, %s, GOAL GO UP HIGH ...); short buffers cut and never overrun; packs_offered: 1 of %d built-in pairs (this world's own, not a test), the WORLDS filter in both directions for species and habitat\n",
           freshwater_why, other_goal_need(), species_count() * habitat_count());
    return 0;
}

/* ================================================================ 4: the crossover */
#define X_NS "pt.test.crossover"
#define P_NS "pt.test.paludarium"
/* a gecko that walks AND swims, and a desert with water and a shore: the pair whose capabilities need both firmware modules. hid is the
   habitat's registry id for the struct-level checks (0: for the file, which carries none). The goals are this build's default species',
   so the model leg passes in either world */
static void cross_make(species_t *x, habitat_t *p, uint8_t hid) {
    *p = *habitat_get(HABITAT_DESERT);
    snprintf(p->name, sizeof p->name, "PALUDARIUM");
    p->uid = fnv(P_NS); p->id = hid; p->builtin = false; p->worlds = 0; p->test_only = false;
    p->areas |= CAP_WATER | CAP_SHORE;
    *x = *species_get(SPECIES_GECKO);
    snprintf(x->name, sizeof x->name, "CROSSOVER");
    x->uid = fnv(X_NS); x->id = 0; x->builtin = false; x->worlds = 0; x->test_only = false;
    x->n_loco = 2;
    x->loco[0].mode = CAP_WALK; x->loco[0].area = CAP_LAND;
    x->loco[1].mode = CAP_SWIM; x->loco[1].area = CAP_WATER;
    x->loco[2].mode = 0; x->loco[2].area = 0;
    x->media_needs = CAP_LAND | CAP_BASK | CAP_HIDES | CAP_WATER;       /* (no SHORE: no firmware provides one yet - t_crossover says so) */
    x->media_wants = 0;
    x->n_habitat_uids = 1; memset(x->habitat_uid, 0, sizeof x->habitat_uid); x->habitat_uid[0] = p->uid;
    x->habitats = hid ? 1u << hid : 0;
    x->n_goals = s_ws->n_goals; memcpy(x->goal, s_ws->goal, sizeof x->goal);
}
static int cross_encode(blob_t *H, blob_t *S) {
    species_t x; habitat_t p;
    cross_make(&x, &p, 0);
    const char *why = "";
    H->n = ptpk_encode_habitat(&p, P_NS, H->b, sizeof H->b, &why);
    if (!H->n) PF("the paludarium cannot be encoded: %s", why);
    S->n = ptpk_encode_species(&x, X_NS, S->b, sizeof S->b, &why);
    if (!S->n) PF("the crossover species cannot be encoded: %s", why);
    return 0;
}
static int t_crossover(void) {
    char why[300], need[40], l[120];
    species_t x; habitat_t p;
    cross_make(&x, &p, 20);
    if (!habitat_check(&p, why, sizeof why)) PF("the paludarium is not a habitat: %s", why);
    if (!species_check(&x, why, sizeof why)) PF("the crossover species is not a species (in the simulator's firmware): %s", why);
    const caps_t needs = CAP_WALK | CAP_SWIM | CAP_LAND | CAP_WATER | CAP_BASK | CAP_HIDES | CAP_CRICKET | CAP_MEALWORM;
    if (species_needs(&x) != needs || species_moves(&x) != (CAP_WALK | CAP_SWIM)) PF("the crossover needs %08x, want %08x", (unsigned)species_needs(&x), (unsigned)needs);
    if ((species_needs(&x) & ~habitat_provides(&p)) != 0) { caps_list(species_needs(&x) & ~habitat_provides(&p), l, sizeof l); PF("the paludarium lacks %s", l); }
    const model_manifest_t *model = model_provides();
    const caps_t both = CAPS_FW_TERRARIUM | CAPS_FW_AQUARIUM;
    if (!packs_check_with(&x, &p, both, model, need, sizeof need, why, sizeof why) || need[0] || why[0]) PF("a firmware with both modules refuses the crossover: %s", why);
    if (packs_check_with(&x, &p, CAPS_FW_TERRARIUM, model, need, sizeof need, why, sizeof why) || !strstr(why, "SWIM") || !strstr(why, "this firmware has none") || strcmp(need, "SWIM") != 0 || !strstr(why, "WATER"))
        PF("the terrarium's firmware alone: \"%s\" / \"%s\"", why, need);
    if (strcmp(why, "CROSSOVER needs SWIM, WATER; this firmware has none") != 0) PF("the terrarium's firmware alone reads \"%s\"", why);
    if (packs_check_with(&x, &p, CAPS_FW_AQUARIUM, model, need, sizeof need, why, sizeof why) || !strstr(why, "WALK") || !strstr(why, "this firmware has none") || strcmp(need, "WALK") != 0)
        PF("the aquarium's firmware alone: \"%s\" / \"%s\"", why, need);
    /* the same species in a habitat that has one of the two grounds: the habitat is named */
    species_t x2 = x; x2.habitats |= 1u << HABITAT_DESERT;
    if (packs_check_with(&x2, habitat_get(HABITAT_DESERT), both, model, need, sizeof need, why, sizeof why) || strcmp(why, "CROSSOVER needs SWIM, WATER; DESERT has none") != 0 || strcmp(need, "SWIM") != 0)
        PF("the crossover in the desert: \"%s\" / \"%s\"", why, need);
    x2 = x; x2.habitats |= 1u << HABITAT_FRESHWATER;
    if (packs_check_with(&x2, habitat_get(HABITAT_FRESHWATER), both, model, need, sizeof need, why, sizeof why) || !strstr(why, "WALK") || !strstr(why, "FRESHWATER has none") || strcmp(need, "WALK") != 0)
        PF("the crossover in the freshwater: \"%s\" / \"%s\"", why, need);
    /* what no firmware provides yet: a SHORE (the habitat offers one; a species that needs it is refused by both modules together, naming it) */
    x2 = x; x2.media_needs |= CAP_SHORE;
    if (packs_check_with(&x2, &p, both, model, need, sizeof need, why, sizeof why) || strcmp(why, "CROSSOVER needs SHORE; this firmware has none") != 0 || strcmp(need, "SHORE") != 0)
        PF("a crossover that needs a SHORE: \"%s\" / \"%s\"", why, need);
    /* through FILES: the habitat then the species, on three firmwares */
    static blob_t H, S;
    if (cross_encode(&H, &S)) return 1;
    const struct { const char *what; caps_t fw; const char *word, *word2; } FW[] = {
        { "both modules", both, NULL, NULL },
        { "the terrarium's alone", CAPS_FW_TERRARIUM, "SWIM", "no such module" },
        { "the aquarium's alone", CAPS_FW_AQUARIUM, "WALK", "no such module" } };
    for (size_t f = 0; f < sizeof FW / sizeof FW[0]; f++) {
        packs_drop_loaded();
        const reg_t base = reg_now();
        packs_info_t info;
        firmware_caps_override(true, FW[f].fw);
        char ws[300] = "";
        if (offer_blob(&H, &info, ws, sizeof ws) != PACKS_ADDED) { firmware_caps_override(false, 0); PF("the paludarium file is refused on %s (a habitat has no needs): %s", FW[f].what, ws); }
        if (habitat_count() != base.nh + 1 || !habitat_by_uid(fnv(P_NS))) { firmware_caps_override(false, 0); PF("the paludarium is not in the registry on %s", FW[f].what); }
        const reg_t with_h = reg_now();
        const packs_result_t r = offer_blob(&S, &info, ws, sizeof ws);
        if (!FW[f].word) {
            if (r != PACKS_ADDED) { firmware_caps_override(false, 0); PF("the crossover species is refused with both modules: %s", ws); }
            const species_t *xs = species_by_uid(fnv(X_NS)); const habitat_t *ph = habitat_by_uid(fnv(P_NS));
            if (!xs || !ph || xs->builtin || xs->id < 3 || ph->id < 3) { firmware_caps_override(false, 0); PF("the loaded crossover has no registry entry or a built-in id"); }
            if (check_loaded(&info, "the crossover")) { firmware_caps_override(false, 0); return 1; }
            if (!packs_compatible(xs, ph, ws, sizeof ws)) { firmware_caps_override(false, 0); PF("the loaded pair is not compatible with both modules: %s", ws); }
            if (!packs_offered(xs, ph)) { firmware_caps_override(false, 0); PF("a crossover with both modules is not offered"); }
            tank_init(&s_t, 7);
            if (!tank_set_packs(&s_t, ph->id, xs->id, ws, sizeof ws) || tank_species(&s_t) != xs || tank_habitat(&s_t) != ph) { firmware_caps_override(false, 0); PF("a tank refused the crossover: %s", ws); }
            for (int n = 0; n < 150; n++) tank_tick(&s_t, 1.0f / 30, NULL);
            bool finite = s_t.clock > 4.0;
            for (int i = 0; i < s_t.n_fish; i++) finite = finite && isfinite(s_t.fish[i].x) && isfinite(s_t.fish[i].y);
            if (!finite) { firmware_caps_override(false, 0); PF("a tank on the crossover does not tick (clock %.2f)", s_t.clock); }
            firmware_caps_override(true, CAPS_FW_TERRARIUM);          /* the firmware loses a module: the loaded pair is no longer held, and says which */
            if (packs_compatible(xs, ph, ws, sizeof ws) || !strstr(ws, "SWIM") || packs_offered(xs, ph)) { firmware_caps_override(false, 0); PF("the loaded crossover without SWIM: \"%s\"", ws); }
        } else {
            if (r != PACKS_REFUSED) { firmware_caps_override(false, 0); PF("the crossover species was not refused on %s", FW[f].what); }
            if (!strstr(ws, FW[f].word) || !strstr(ws, FW[f].word2)) { firmware_caps_override(false, 0); PF("on %s the species file is refused for \"%s\" (want %s and \"%s\")", FW[f].what, ws, FW[f].word, FW[f].word2); }
            if (!reg_same(with_h, reg_now()) || species_by_uid(fnv(X_NS))) { firmware_caps_override(false, 0); PF("on %s a refused species moved the registry", FW[f].what); }
        }
        firmware_caps_override(false, 0);
        packs_drop_loaded();
        if (!reg_counts(base, reg_now()) || species_by_uid(fnv(X_NS)) || habitat_by_uid(fnv(P_NS))) PF("the crossover test left packs behind");
    }
    tank_init(&s_t, 7);
    printf("selftest-caps: the crossover (a gecko that walks on LAND and swims in WATER, needing LAND BASK HIDES WATER; the desert with WATER and a SHORE): accepted with both modules; "
           "the terrarium's firmware alone refuses it 'CROSSOVER needs SWIM, WATER; this firmware has none', the aquarium's names WALK; the desert and the freshwater each named; a species that needs a SHORE is refused even by both modules together (no firmware has one yet); "
           "through files on three firmwares: both modules load it and a tank on it ticks (and loses it again without SWIM); the terrarium's alone refuses the species at load naming SWIM, the aquarium's naming WALK, "
           "the habitat file loading on all three, the registry unchanged by each refusal\n");
    return 0;
}

/* ================================================================ 5: capability bits no firmware names, byte by byte */
static int t_bits(void) {
    static blob_t GV, FV, DV, K;
    const enc_t *ge = enc_with_uid(SPECIES_UID_GECKO), *fe = enc_with_uid(SPECIES_UID_FISH), *de = enc_with_uid(HABITAT_UID_DESERT);
    if (!ge || !fe || !de) PF("a built-in file is missing");
    blob_variant(&GV, ge->blob, ge->len, "c1", "C1");        /* (packs of new uids: the dedupe by uid does not hide the parser) */
    blob_variant(&FV, fe->blob, fe->len, "c1", "C1");
    blob_variant(&DV, de->blob, de->len, "c1", "C1");
    const species_t *gk = species_get(SPECIES_GECKO);
    const habitat_t *desert = habitat_get(HABITAT_DESERT);
    packs_info_t info;
    const int cases0 = s_cases;
    const reg_t base_reg = reg_now();
    RA(GV, info, "the gecko variant"); packs_drop_loaded();
    RA(FV, info, "the fish variant"); packs_drop_loaded();
    RA(DV, info, "the desert variant"); packs_drop_loaded();
    uint32_t needs0 = 0, wants0 = 0;
    if (!rec_u32_get(&GV, 0x111, 0, &needs0) || !rec_u32_get(&GV, 0x111, 4, &wants0) || needs0 != gk->media_needs) PF("the gecko's MEDIA_NEEDS record is not as its struct says");

    /* MEDIA_NEEDS: a need this firmware cannot name is one it can never meet */
    const struct { uint32_t bit; const char *word; const char *what; } NEEDS[] = {
        { 1u << 20, "capability bit 20", "needs with bit 20" }, { 1u << 3, "capability bit 3", "needs with bit 3 (the room bit)" }, { 1u << 31, "capability bit 31", "needs with bit 31" },
        { 1u << 14, "capability bit 14", "needs with bit 14" },
        { CAP_WALK, "LOCOMOTION", "needs with a movement bit" }, { CAP_SWIM, "LOCOMOTION", "needs with SWIM" }, { CAP_CRICKET, "DIET", "needs with a food bit" } };
    for (size_t k = 0; k < sizeof NEEDS / sizeof NEEDS[0]; k++) {
        blob_copy(&K, GV.b, GV.n); rec_u32_set(&K, 0x111, 0, needs0 | NEEDS[k].bit); blob_fix(&K);
        RF(K, NEEDS[k].word, NEEDS[k].what);
    }
    blob_copy(&K, GV.b, GV.n); { uint8_t v[7] = { 0 }; rec_set(&K, 0x111, v, 7); blob_fix(&K); } RF(K, "MEDIA_NEEDS", "MEDIA_NEEDS of 7 bytes");
    blob_copy(&K, GV.b, GV.n); { uint8_t v[9] = { 0 }; rec_set(&K, 0x111, v, 9); blob_fix(&K); } RF(K, "MEDIA_NEEDS", "MEDIA_NEEDS of 9 bytes");
    blob_copy(&K, GV.b, GV.n); rec_delete(&K, 0x111); blob_fix(&K); RF(K, "no MEDIA_NEEDS", "no MEDIA_NEEDS");
    blob_copy(&K, GV.b, GV.n); rec_delete(&K, 0x110); blob_fix(&K); RF(K, "no LOCOMOTION", "no LOCOMOTION");
    /* ... and a want is only a want: a bit it does not know is skipped, the known ones stay */
    blob_copy(&K, GV.b, GV.n); rec_u32_set(&K, 0x111, 4, wants0 | CAP_LAMP | (1u << 20) | (1u << 3) | (1u << 31)); blob_fix(&K);
    RA(K, info, "wants with unknown bits");
    { const species_t *q = species_by_uid(info.uid);
      if (q->media_wants != ((wants0 & CAPS_KNOWN) | CAP_LAMP) || q->media_needs != gk->media_needs || (q->media_wants & ~CAPS_KNOWN)) PF("a want with unknown bits loaded as wants %08x (want LAMP), needs %08x", (unsigned)q->media_wants, (unsigned)q->media_needs); }
    packs_drop_loaded();
    blob_copy(&K, GV.b, GV.n); rec_u32_set(&K, 0x111, 4, CAP_PELLET | CAP_WALK | CAP_SHORE | (1u << 25)); blob_fix(&K);
    RA(K, info, "wants of every kind");
    if (species_by_uid(info.uid)->media_wants != (CAP_PELLET | CAP_WALK | CAP_SHORE)) PF("wants of every kind came back as %08x", (unsigned)species_by_uid(info.uid)->media_wants);
    packs_drop_loaded();

    /* a habitat's AREAS and PROVIDES: what it offers that this firmware cannot use is dropped; a known bit of the wrong kind is refused */
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, desert->areas | (1u << 25) | (1u << 3) | (1u << 31)); blob_fix(&K);
    RA(K, info, "areas with unknown bits");
    if (habitat_by_uid(info.uid)->areas != desert->areas || habitat_by_uid(info.uid)->provides != desert->provides) PF("areas with unknown bits came back as %08x", (unsigned)habitat_by_uid(info.uid)->areas);
    packs_drop_loaded();
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x211, 0, desert->provides | (1u << 25) | (1u << 14)); blob_fix(&K);
    RA(K, info, "provides with unknown bits");
    if (habitat_by_uid(info.uid)->provides != desert->provides || habitat_by_uid(info.uid)->areas != desert->areas) PF("provides with unknown bits came back as %08x", (unsigned)habitat_by_uid(info.uid)->provides);
    packs_drop_loaded();
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, desert->areas | (1u << 25)); rec_u32_set(&K, 0x211, 0, desert->provides | (1u << 25)); blob_fix(&K);
    RA(K, info, "both with bit 25");
    if (habitat_by_uid(info.uid)->areas != desert->areas || habitat_by_uid(info.uid)->provides != desert->provides) PF("both with bit 25 came back as %08x / %08x", (unsigned)habitat_by_uid(info.uid)->areas, (unsigned)habitat_by_uid(info.uid)->provides);
    packs_drop_loaded();
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, desert->areas | CAP_CRICKET); blob_fix(&K); RF(K, "CRICKET", "AREAS naming a food");
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, desert->areas | CAP_SWIM); blob_fix(&K); RF(K, "SWIM", "AREAS naming a movement mode");
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, desert->areas | CAP_LAMP); blob_fix(&K); RF(K, "LAMP", "AREAS naming a fitting");
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x211, 0, desert->provides | CAP_WATER); blob_fix(&K); RF(K, "WATER", "PROVIDES naming an area");
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x211, 0, desert->provides | CAP_WALK); blob_fix(&K); RF(K, "WALK", "PROVIDES naming a movement mode");
    blob_copy(&K, DV.b, DV.n); { uint8_t v[3] = { 0 }; rec_set(&K, 0x210, v, 3); blob_fix(&K); } RF(K, "AREAS", "AREAS of 3 bytes");
    blob_copy(&K, DV.b, DV.n); rec_delete(&K, 0x210); blob_fix(&K); RF(K, "no AREAS", "no AREAS");
    blob_copy(&K, DV.b, DV.n); rec_delete(&K, 0x211); blob_fix(&K); RF(K, "no PROVIDES", "no PROVIDES");
    /* what loaded as the desert's is a habitat that can only be refused by its own rules: BASK without a lamp ... (habitat_check, not the reader) */
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, desert->areas & ~CAP_BASK); blob_fix(&K); RF(K, "BASK", "AREAS without BASK for a lamp's pool");
    blob_copy(&K, DV.b, DV.n); rec_u32_set(&K, 0x210, 0, 0); blob_fix(&K); RF(K, "neither WATER nor LAND", "AREAS of nothing");

    /* LOCOMOTION: entries of 38 bytes - mode, area, nine floats. The gecko's walk, the fish's swim */
    size_t at, vl;
    if (!rec_find(&GV, 0x110, &at, &vl) || vl != 39 || GV.b[at + 4] != 1 || GV.b[at + 5] != 1 || GV.b[at + 6] != 5) PF("the gecko's LOCOMOTION is %zu bytes, not one 38-byte entry (WALK on LAND)", vl);
    if (!rec_find(&FV, 0x110, &at, &vl) || vl != 39 || FV.b[at + 4] != 1 || FV.b[at + 5] != 0 || FV.b[at + 6] != 4) PF("the fish's LOCOMOTION is %zu bytes, not one 38-byte entry (SWIM on WATER)", vl);
    const struct { unsigned off; uint8_t v; const char *word; const char *what; } LOCO[] = {
        { 1, 3, "capability bit 3", "mode index 3 (the room bit)" }, { 1, 31, "capability bit 31", "mode index 31" }, { 1, 200, "capability bit 200", "mode index 200" },
        { 1, 4, "not a movement mode", "mode WATER" }, { 1, 8, "CRICKET", "mode CRICKET" },
        { 2, 3, "capability bit 3", "area index 3" }, { 2, 9, "MEALWORM", "area index 9 (MEALWORM)" }, { 2, 31, "capability bit 31", "area index 31" },
        { 2, 200, "capability bit 200", "area index 200" }, { 2, 1, "WALK", "area WALK" }, { 2, 13, "HIDES", "area HIDES" } };
    for (size_t k = 0; k < sizeof LOCO / sizeof LOCO[0]; k++) {
        blob_copy(&K, GV.b, GV.n); rec_u8_set(&K, 0x110, LOCO[k].off, LOCO[k].v); blob_fix(&K);
        RF(K, LOCO[k].word, LOCO[k].what);
    }
    /* ... the swim carries no number yet, a mode appears once, the count and the layout hold */
    blob_copy(&K, FV.b, FV.n); rec_u32_set(&K, 0x110, 3, 0x3f800000u); blob_fix(&K); RF(K, "carries numbers", "a SWIM entry with a pace");
    blob_copy(&K, FV.b, FV.n); rec_u32_set(&K, 0x110, 3 + 8 * 4, 0x80000000u); blob_fix(&K); RF(K, "carries numbers", "a SWIM entry with a -0 turn");
    blob_copy(&K, FV.b, FV.n); rec_u32_set(&K, 0x110, 3 + 4 * 4, 0x3f800000u); blob_fix(&K); RF(K, "carries numbers", "a SWIM entry with a bout");
    { uint8_t v[77]; rec_find(&GV, 0x110, &at, &vl); v[0] = 2; memcpy(v + 1, GV.b + at + 5, 38); memcpy(v + 39, GV.b + at + 5, 38);
      blob_copy(&K, GV.b, GV.n); rec_set(&K, 0x110, v, sizeof v); blob_fix(&K); RF(K, "twice", "WALK twice"); }
    { uint8_t v[77]; rec_find(&FV, 0x110, &at, &vl); v[0] = 2; memcpy(v + 1, FV.b + at + 5, 38); memcpy(v + 39, FV.b + at + 5, 38);
      blob_copy(&K, FV.b, FV.n); rec_set(&K, 0x110, v, sizeof v); blob_fix(&K); RF(K, "twice", "SWIM twice"); }
    blob_copy(&K, GV.b, GV.n); rec_u8_set(&K, 0x110, 0, 0); blob_fix(&K); RF(K, "LOCOMOTION", "no entries");
    blob_copy(&K, GV.b, GV.n); rec_u8_set(&K, 0x110, 0, 4); blob_fix(&K); RF(K, "LOCOMOTION", "four entries");
    blob_copy(&K, GV.b, GV.n); { uint8_t v[20] = { 1, 1, 5 }; rec_set(&K, 0x110, v, sizeof v); blob_fix(&K); } RF(K, "LOCOMOTION", "a cut entry");
    blob_copy(&K, GV.b, GV.n); rec_u32_set(&K, 0x110, 3, 0x7f800000u); blob_fix(&K); RF(K, "LOCOMOTION", "an infinite pace");
    /* a WALK whose numbers are zero: the reader takes the record, the species' own rules refuse the pack */
    blob_copy(&K, GV.b, GV.n); rec_find(&K, 0x110, &at, &vl); memset(K.b + at + 4 + 3, 0, 36); blob_fix(&K);
    { ptpk_header_t h; species_t q; uint8_t legacy; char why[200];
      if (!ptpk_open(K.b, K.n, &h, why, sizeof why) || !ptpk_read_species(&h, &q, &legacy, why, sizeof why)) PF("the reader refuses a WALK with zero numbers: %s", why);
      q.habitats = 1u << HABITAT_DESERT;
      if (species_check(&q, why, sizeof why) || !strstr(why, "walk_px_s")) PF("species_check lets a walk without a pace through, or for another reason: %s", why); }
    RF(K, "walk_px_s", "a WALK with zero numbers");
    /* a swimming gecko with walk numbers and no walk entry: the reader refuses it (a swim carries none) */
    blob_copy(&K, GV.b, GV.n); rec_u8_set(&K, 0x110, 1, 0); rec_u8_set(&K, 0x110, 2, 4); blob_fix(&K); RF(K, "carries numbers", "a SWIM entry with a walk's numbers");

    /* WORLDS: a filter, not a gate */
    blob_copy(&K, GV.b, GV.n); set_worlds(&K, 0); RF(K, "WORLDS", "WORLDS 0");
    blob_copy(&K, DV.b, DV.n); set_worlds(&K, 0); RF(K, "WORLDS", "a habitat's WORLDS 0");
    static const unsigned WM[] = { 1, 2, 3, 255 };
    for (size_t k = 0; k < sizeof WM / sizeof WM[0]; k++) {
        blob_copy(&K, GV.b, GV.n); set_worlds(&K, WM[k]);
        RA(K, info, "a species with WORLDS");
        if (species_by_uid(info.uid)->worlds != WM[k]) PF("a species with WORLDS %u loaded with worlds %u", WM[k], species_by_uid(info.uid)->worlds);
        packs_drop_loaded();
        blob_copy(&K, DV.b, DV.n); set_worlds(&K, WM[k]);
        RA(K, info, "a habitat with WORLDS");
        if (habitat_by_uid(info.uid)->worlds != WM[k]) PF("a habitat with WORLDS %u loaded with worlds %u", WM[k], habitat_by_uid(info.uid)->worlds);
        packs_drop_loaded();
    }
    { const blob_t *fo = PT_TERRARIUM ? &FV : &GV;                              /* the other world's own pack, as its file says (WORLDS: its world alone) */
      blob_copy(&K, fo->b, fo->n); }
    RA(K, info, "the other world's pack");                                      /* (a filter: the pack of the other world loads) */
    if (world_offers(species_by_uid(info.uid)->worlds)) PF("the other world's pack says it is this world's (WORLDS %02x)", species_by_uid(info.uid)->worlds);
    packs_drop_loaded();
    blob_copy(&K, GV.b, GV.n); { const uint8_t v = 2; rec_append(&K, 0x10, &v, 1); blob_fix(&K); } RF(K, "WORLDS twice", "WORLDS twice");
    blob_copy(&K, GV.b, GV.n); { const uint8_t v[2] = { 2, 0 }; rec_set(&K, 0x10, v, 2); blob_fix(&K); } RF(K, "WORLDS", "WORLDS of 2 bytes");

    /* records no loader knows: REQUIRED refuses the pack, an optional one is skipped - the retired ones (WORLD 0003, WALK 0105) among them */
    static const unsigned UNKNOWN_REQ[] = { 0x8555, 0x8003, 0x8105 }, UNKNOWN_OPT[] = { 0x0555, 0x0003, 0x0105 };
    const uint8_t two[2] = { 1, 0 };
    for (size_t k = 0; k < 3; k++) {
        blob_copy(&K, GV.b, GV.n); rec_append(&K, UNKNOWN_REQ[k], two, 2); blob_fix(&K); RF(K, "REQUIRED", "an unknown REQUIRED record in a species");
        blob_copy(&K, DV.b, DV.n); rec_append(&K, UNKNOWN_REQ[k], two, 2); blob_fix(&K); RF(K, "REQUIRED", "an unknown REQUIRED record in a habitat");
        blob_copy(&K, GV.b, GV.n); rec_append(&K, UNKNOWN_OPT[k], two, 2); blob_fix(&K);
        RA(K, info, "an unknown optional record in a species");
        if (species_needs(species_by_uid(info.uid)) != species_needs(gk) || species_by_uid(info.uid)->n_goals != gk->n_goals) PF("an unknown optional record changed a species");
        packs_drop_loaded();
        blob_copy(&K, DV.b, DV.n); rec_append(&K, UNKNOWN_OPT[k], two, 2); blob_fix(&K);
        RA(K, info, "an unknown optional record in a habitat");
        if (habitat_provides(habitat_by_uid(info.uid)) != habitat_provides(desert)) PF("an unknown optional record changed a habitat");
        packs_drop_loaded();
    }
    /* the other kinds' records are unknown ones (a manifest's tag in a species, a species' LOCOMOTION in a habitat) */
    blob_copy(&K, GV.b, GV.n); rec_append(&K, 0x8301, "abc", 3); blob_fix(&K); RF(K, "REQUIRED", "a manifest's MODEL_TAG in a species");
    blob_copy(&K, DV.b, DV.n); rec_append(&K, 0x8110, "abc", 3); blob_fix(&K); RF(K, "REQUIRED", "a species' LOCOMOTION in a habitat");
    blob_copy(&K, DV.b, DV.n); rec_append(&K, 0x0111, "abc", 3); blob_fix(&K);
    RA(K, info, "a species' MEDIA_NEEDS, optionally, in a habitat"); packs_drop_loaded();
    if (!reg_counts(base_reg, reg_now())) PF("the registries are not as they were");
    printf("selftest-caps: unknown capabilities and records at load, byte by byte (%d files): MEDIA_NEEDS needs with bit 20, 3 (the room bit), 31, 14 refused naming the bit, with a movement or a food bit refused (LOCOMOTION / DIET), "
           "a want with bits 20, 3, 31 loads and drops them keeping the known ones; AREAS and PROVIDES with bits 25, 3, 31, 14 load with them dropped (equal to the desert's), naming a food, a mode, a fitting or an area of the wrong kind refused; "
           "LOCOMOTION mode index 3, 31, 200, WATER, CRICKET and area index 3, 9, 31, 200, WALK, HIDES refused naming the capability, a SWIM entry with a number (a pace, a bout, a -0 turn) 'carries numbers', a mode twice, 0 / 4 entries, a cut entry, "
           "a WALK of zero numbers read by the reader and refused by species_check (walk_px_s); WORLDS 0 and 2 bytes and twice refused, 1 / 2 / 3 / 255 load and are kept (the other world's pack loads: a filter); "
           "unknown REQUIRED records (8555, the retired WORLD 8003 and WALK 8105, another kind's) refused, the optional ones skipped, in a species and a habitat\n", s_cases - cases0);
    return 0;
}

/* ================================================================ 6: goals, and the model's manifest */
static int t_goals(void) {
    char why[300], need[40];
    species_t g = *species_get(SPECIES_GECKO);                 /* (a copy: the rules read the struct, the registry's masks stay) */
    const struct { const char *what; const char *word; } BAD[] = { { "a goal no manifest has", "climb" }, { "a goal twice", "twice" }, { "a goal in capitals", "not a word" },
        { "a goal that starts with a digit", "not a word" }, { "a goal of 16 characters", "not a word" }, { "no goals", "goals, not 1" }, { "nine goals", "goals, not 1" } };
    for (size_t k = 0; k < sizeof BAD / sizeof BAD[0]; k++) {
        g = *species_get(SPECIES_GECKO);
        switch (k) {
        case 0: snprintf(g.goal[g.n_goals++], GOAL_NAME_MAX, "climb"); break;
        case 1: snprintf(g.goal[g.n_goals], GOAL_NAME_MAX, "%s", g.goal[0]); g.n_goals++; break;
        case 2: snprintf(g.goal[0], GOAL_NAME_MAX, "Hunt"); break;
        case 3: snprintf(g.goal[0], GOAL_NAME_MAX, "9lives"); break;
        case 4: memcpy(g.goal[0], "abcdefghijklmnop", GOAL_NAME_MAX); break;
        case 5: g.n_goals = 0; break;
        default: g.n_goals = SPECIES_GOALS_MAX + 1; break;
        }
        if (species_check(&g, why, sizeof why) || !strstr(why, BAD[k].word)) PF("%s: species_check says \"%s\"", BAD[k].what, why);
    }
    /* the word of the OTHER world's model loads (the simulator carries both manifests: a vocabulary, not a gate) and is refused at the tank */
    static blob_t K, M;
    species_t o = *s_ws;
    snprintf(o.name, sizeof o.name, "OTHER GOAL"); o.uid = fnv("pt.test.goal-other"); o.id = 0; o.builtin = false; o.worlds = 0; o.test_only = false;
    snprintf(o.goal[0], GOAL_NAME_MAX, "%s", other_goal());
    const char *ewhy = "";
    K.n = ptpk_encode_species(&o, "pt.test.goal-other", K.b, sizeof K.b, &ewhy);
    if (!K.n) PF("the species of the other world's goal cannot be encoded: %s", ewhy);
    packs_info_t info;
    const reg_t base = reg_now();
    RA(K, info, "a species with a goal of the other world's model");
    const species_t *q = species_by_uid(o.uid);
    if (packs_compatible(q, s_wh, why, sizeof why) || !strstr(why, other_goal()) || !strstr(why, PT_MODEL_TAG) || !strstr(why, "needs the goal")) PF("a goal of the other world's model: \"%s\"", why);
    packs_check(q, s_wh, need, sizeof need, NULL, 0);
    if (strcmp(need, other_goal_need()) != 0 || packs_offered(q, s_wh)) PF("the other world's goal: short need \"%s\", offered %d", need, packs_offered(q, s_wh));
    tank_init(&s_t, 9);
    if (tank_set_packs(&s_t, s_wh->id, q->id, why, sizeof why) || !strstr(why, other_goal()) || !strstr(why, PT_MODEL_TAG) || s_t.pack_id != SPECIES_DEFAULT || s_t.habitat_id != HABITAT_DEFAULT) PF("a tank took a species whose goal the model lacks: %s", why);
    packs_drop_loaded();
    if (!reg_counts(base, reg_now())) PF("the registry is not as it was");
    /* a manifest from a file gives the vocabulary a word; the pair is still refused by this build's model; dropping it takes the word and the species back */
    model_manifest_t m; memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "TEST CLIMB"); snprintf(m.tag, sizeof m.tag, "test_climb"); m.uid = fnv("pt.model.test-climb");
    m.n_goals = 3; snprintf(m.goal[0], GOAL_NAME_MAX, "climb"); snprintf(m.goal[1], GOAL_NAME_MAX, "hunt"); snprintf(m.goal[2], GOAL_NAME_MAX, "bask");
    M.n = ptpk_encode_manifest(&m, "pt.model.test-climb", M.b, sizeof M.b, &ewhy);
    if (!M.n) PF("the synthetic manifest cannot be encoded: %s", ewhy);
    species_t c = *s_ws;
    snprintf(c.name, sizeof c.name, "CLIMBER"); c.uid = fnv("pt.test.climber"); c.id = 0; c.builtin = false; c.worlds = 0; c.test_only = false;
    snprintf(c.goal[0], GOAL_NAME_MAX, "climb");
    K.n = ptpk_encode_species(&c, "pt.test.climber", K.b, sizeof K.b, &ewhy);
    if (!K.n) PF("the climber cannot be encoded: %s", ewhy);
    if (manifests_know_goal("climb") || manifest_by_tag("test_climb")) PF("a manifest with the word climb is loaded already");
    RF(K, "climb", "a climber before any manifest has the word");
    c.habitats = s_ws->habitats;
    if (species_check(&c, why, sizeof why) || !strstr(why, "climb")) PF("species_check lets a goal no manifest has through: %s", why);
    RA(M, info, "the test manifest");
    if (info.kind != PTPK_KIND_MODEL || !manifests_know_goal("climb") || !manifest_by_tag("test_climb") || manifest_count() != base.nm + 1) PF("the manifest did not go into a spare slot");
    if (!species_check(&c, why, sizeof why)) PF("the vocabulary has the word and species_check refuses it: %s", why);
    RA(K, info, "a climber with the manifest loaded");
    q = species_by_uid(c.uid);
    if (packs_compatible(q, s_wh, why, sizeof why) || !strstr(why, "climb") || !strstr(why, PT_MODEL_TAG) || !strstr(why, "has none")) PF("a climber in this build: \"%s\"", why);
    packs_check(q, s_wh, need, sizeof need, NULL, 0);
    if (strcmp(need, "GOAL CLIMB") != 0 || packs_offered(q, s_wh)) PF("a climber: short need \"%s\", offered %d", need, packs_offered(q, s_wh));
    if (packs_check_with(q, s_wh, firmware_caps(), manifest_by_tag("test_climb"), need, sizeof need, why, sizeof why) == (first_missing_goal(q, manifest_by_tag("test_climb")) != NULL)) PF("the test model's own check is wrong: %s", why);
    tank_init(&s_t, 9);
    if (tank_set_packs(&s_t, s_wh->id, q->id, why, sizeof why) || !strstr(why, "climb") || s_t.pack_id != SPECIES_DEFAULT) PF("a tank took a climber: %s", why);
    packs_drop_loaded();                                       /* (the species goes first, the manifest with it: nothing stays that names the word) */
    if (manifests_know_goal("climb") || manifest_by_tag("test_climb") || species_by_uid(c.uid) || !reg_counts(base, reg_now())) PF("dropping the manifest left it, its word or the climber behind");
    if (species_check(&c, why, sizeof why) || !strstr(why, "climb")) PF("with the manifest gone species_check takes climb again: %s", why);
    RF(K, "climb", "a climber with the manifest dropped");
    /* what the model provides: this build's manifest, its tag, its goal words, its uid */
    const model_manifest_t *mp = model_provides();
    if (!mp || strcmp(mp->tag, PT_MODEL_TAG) != 0) PF("model_provides has the tag \"%s\", not %s", mp ? mp->tag : "(none)", PT_MODEL_TAG);
    if (mp->n_goals != GOAL_N) PF("the %s manifest lists %d goals, the tank's world has %d", mp->tag, mp->n_goals, GOAL_N);
    for (int k = 0; k < GOAL_N; k++) if (strcmp(mp->goal[k], GOAL_NAMES[GOAL_FIRST + k]) != 0) PF("goal %d of the %s manifest is \"%s\", the tank's is \"%s\"", k, mp->tag, mp->goal[k], GOAL_NAMES[GOAL_FIRST + k]);
    const model_manifest_t *lz = manifest_by_tag(PT_MODEL_TAG_TERRARIUM), *fm = manifest_by_tag(PT_MODEL_TAG_AQUARIUM);
    if (!lz || !fm) PF("a built-in manifest is not loaded (the simulator carries both)");
    if (lz->n_goals != LZ_GOALS || fm->n_goals != FISH_GOALS) PF("the manifests list %d and %d goals", lz->n_goals, fm->n_goals);
    for (int k = 0; k < LZ_GOALS; k++) if (strcmp(lz->goal[k], LZ_GOAL_NAMES[k]) != 0 || strcmp(lz->goal[k], GOAL_NAMES[GOAL_LZ_FIRST + k]) != 0) PF("lizard goal %d is \"%s\"", k, lz->goal[k]);
    for (int k = 0; k < FISH_GOALS; k++) if (strcmp(fm->goal[k], FISH_GOAL_NAMES[k]) != 0 || strcmp(fm->goal[k], GOAL_NAMES[GOAL_FISH_FIRST + k]) != 0) PF("fish goal %d is \"%s\"", k, fm->goal[k]);
    if (strcmp(lz->tag, "lizard_v1") != 0 || strcmp(fm->tag, "v3m") != 0) PF("the manifests' tags are %s and %s, want lizard_v1 and v3m", lz->tag, fm->tag);
    if (strcmp(PT_MODEL_TAG_TERRARIUM, "lizard_v1") != 0 || strcmp(PT_MODEL_TAG_AQUARIUM, "v3m") != 0) PF("version.h names the models %s and %s", PT_MODEL_TAG_TERRARIUM, PT_MODEL_TAG_AQUARIUM);
    if (lz->uid != fnv("pt.model.lizard_v1") || lz->uid != MODEL_UID_LIZARD_V1 || fm->uid != fnv("pt.model.fish_v3m") || fm->uid != MODEL_UID_FISH_V3M) PF("the manifests' uids are %08x and %08x", (unsigned)lz->uid, (unsigned)fm->uid);
    if (manifest_by_uid(lz->uid) != lz || manifest_by_uid(fm->uid) != fm || !lz->builtin || !fm->builtin || manifest_by_uid(0) || manifest_by_tag(NULL) || manifest_by_tag("nope")) PF("the manifest lookups are wrong");
    if (strcmp(FISH_GOAL_NAMES[2], "visit_bubbles") != 0 || strcmp(LZ_GOAL_NAMES[LZ_INVESTIGATE], "investigate") != 0) PF("the goal words the tests rely on have changed");
    for (int k = 0; k < lz->n_goals; k++) if (manifest_has_goal(fm, lz->goal[k]) && strcmp(lz->goal[k], "explore") != 0 && strcmp(lz->goal[k], "rest") != 0) PF("the worlds' goal words overlap in \"%s\"", lz->goal[k]);
    printf("selftest-caps: goals: species_check refuses a goal no manifest lists (climb), twice, in capitals, a digit first, 16 long, none, nine; a goal of the other world's model (%s) LOADS in the simulator and is refused at the tank naming the word and the %s model "
           "(%s); a manifest file (tag test_climb) loaded into a spare slot gives a species the word climb - and packs_compatible still refuses it naming climb and %s (GOAL CLIMB), packs_offered false; "
           "dropping it drops the word and the climber; model_provides is %s with the tank's %d goal words in order; the manifests' tags (lizard_v1, v3m), uids (FNV-1a of pt.model.lizard_v1 / pt.model.fish_v3m) and words are the engine's\n",
           other_goal(), PT_MODEL_TAG, other_goal_need(), PT_MODEL_TAG, PT_MODEL_TAG, GOAL_N);
    return 0;
}

/* ================================================================ 7: manifests as packs */
static size_t goals_value(uint8_t *o, int count_field, const char *const *w, int nw) {   /* GOALS / MODEL_GOALS: u8 n, then n x (u8 len, bytes) */
    size_t n = 0;
    o[n++] = (uint8_t)count_field;
    for (int k = 0; k < nw; k++) { const size_t l = strlen(w[k]); o[n++] = (uint8_t)l; memcpy(o + n, w[k], l); n += l; }
    return n;
}
/* a built-in manifest's file under a new UID_NS / NAME tail and a new tag: the dedupe by uid and the tag rule do not hide the parser */
static void man_variant(blob_t *k, const enc_t *e, const char *ns_tail, const char *name_tail, const char *tag) {
    blob_variant(k, e->blob, e->len, ns_tail, name_tail);
    rec_set(k, 0x301, tag, strlen(tag));
    blob_fix(k);
}
typedef struct { long refused, loaded; } tally_t;
static int man_offer(const blob_t *m, tally_t *t, const char *what, reg_t base) {
    packs_info_t info; char why[200];
    switch (offer_blob(m, &info, why, sizeof why)) {
    case PACKS_ADDED:
        if (check_loaded(&info, what)) return 1;
        t->loaded++;
        packs_drop_loaded();
        break;
    case PACKS_SAME: PF("%s: a file that is not the one loaded was called the same", what);
    default:
        t->refused++;
        break;
    }
    if (!reg_counts(base, reg_now())) PF("%s: the registries hold %d/%d/%d (species, habitats, manifests), they held %d/%d/%d", what, species_count(), habitat_count(), manifest_count(), base.ns, base.nh, base.nm);
    return 0;
}
static int t_manifests(void) {
    char why[300];
    packs_drop_loaded();
    const reg_t base0 = reg_now();
    if (base0.nm != PTPK_BUILTIN_MODELS) PF("%d manifests loaded at the start, the build ships %d", base0.nm, PTPK_BUILTIN_MODELS);
    /* 1. every built-in manifest: encoded, read back field for field, the registry's own, the file in packs/ */
    int n_models = 0, on_disk = 0;
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {
        const builtin_pack_t *b = &PTPK_BUILTIN[i];
        if (!b->manifest) continue;
        n_models++;
        const enc_t *e = &s_enc[i];
        ptpk_header_t h; model_manifest_t r;
        if (!ptpk_open(e->blob, e->len, &h, why, sizeof why)) PF("%s: %s", b->uid_ns, why);
        if (h.kind != PTPK_KIND_MODEL) PF("%s: kind %d, not a model manifest", b->uid_ns, h.kind);
        if (!ptpk_read_manifest(&h, &r, why, sizeof why)) PF("%s: the reader refuses what the encoder wrote: %s", b->uid_ns, why);
        if (strcmp(r.name, b->manifest->name) != 0 || strcmp(r.tag, b->manifest->tag) != 0 || r.uid != b->manifest->uid || r.uid != b->uid || r.uid != fnv(b->uid_ns) || r.n_goals != b->manifest->n_goals)
            PF("%s: read back as %s / %s / %08x / %d goals", b->uid_ns, r.name, r.tag, (unsigned)r.uid, r.n_goals);
        for (int k = 0; k < r.n_goals; k++) if (strcmp(r.goal[k], b->manifest->goal[k]) != 0) PF("%s: goal %d read back as \"%s\"", b->uid_ns, k, r.goal[k]);
        if (!manifest_check(&r, why, sizeof why)) PF("%s: read back and refused by manifest_check: %s", b->uid_ns, why);
        const model_manifest_t *reg = manifest_by_uid(b->uid);
        if (!reg || !reg->builtin || strcmp(reg->name, r.name) != 0 || strcmp(reg->tag, r.tag) != 0 || reg->n_goals != r.n_goals || manifest_by_tag(r.tag) != reg) PF("%s: the registry does not hold this manifest", b->uid_ns);
        for (int k = 0; k < r.n_goals; k++) if (strcmp(reg->goal[k], r.goal[k]) != 0) PF("%s: the registry's goal %d is \"%s\"", b->uid_ns, k, reg->goal[k]);
        packs_info_t info;
        if (packs_add(e->blob, e->len, PACKS_BUILTIN, &info, why, sizeof why) != PACKS_SAME || info.kind != PTPK_KIND_MODEL || info.uid != b->uid) PF("%s: the registry does not say it holds this file (%s)", b->uid_ns, why);
        char path[300]; snprintf(path, sizeof path, "../packs/%s.ptpk", b->uid_ns);
        FILE *f = fopen(path, "rb");
        if (f) {
            static uint8_t disk[PTPK_FILE_MAX + 8];
            const size_t got = fread(disk, 1, sizeof disk, f); fclose(f);
            if (got != e->len || memcmp(disk, e->blob, e->len)) PF("%s is not what the encoder makes of %s's const struct (`make packs` writes it)", path, b->uid_ns);
            on_disk++;
        }
    }
    if (n_models != PTPK_BUILTIN_MODELS) PF("the table lists %d manifests, the build ships %d", n_models, PTPK_BUILTIN_MODELS);

    /* 2. the encoder refuses what the reader would */
    {
        static uint8_t out[PTPK_FILE_MAX];
        const char *ns = PT_TERRARIUM ? "pt.model.lizard_v1" : "pt.model.fish_v3m";
        model_manifest_t m;
        #define ENC_NO(stmt, word, what) do { m = *model_provides(); stmt; const char *ew = NULL; \
            if (ptpk_encode_manifest(&m, ns, out, sizeof out, &ew)) PF("the encoder wrote a manifest with %s", what); \
            if (!ew || !strstr(ew, word)) PF("a manifest with %s: the encoder says \"%s\", want one with \"%s\"", what, ew ? ew : "(nothing)", word); } while (0)
        ENC_NO(memcpy(m.tag, "bad tag", 8), "tag", "a space in the tag");
        ENC_NO(m.tag[0] = 0, "tag", "no tag");
        ENC_NO(memcpy(m.tag, "abcdefghijklmnop", 16), "tag", "a tag of 16 characters");
        ENC_NO(memcpy(m.tag, "a/b", 4), "tag", "a slash in the tag");
        ENC_NO(m.n_goals = 0, "goal", "no goals");
        ENC_NO(m.n_goals = 9, "goal", "nine goals");
        ENC_NO(snprintf(m.goal[1], GOAL_NAME_MAX, "%s", m.goal[0]), "twice", "a goal twice");
        ENC_NO(snprintf(m.goal[0], GOAL_NAME_MAX, "Bad"), "goal's word", "a goal in capitals");
        ENC_NO(memcpy(m.goal[0], "abcdefghijklmnop", GOAL_NAME_MAX), "goal's word", "a goal of 16 characters");
        ENC_NO(memset(m.name, 'A', sizeof m.name), "name", "a name of 24 characters");
        ENC_NO(m.name[0] = 0, "name", "no name");
        ENC_NO(m.uid ^= 1, "uid", "a uid that is not its name's hash");
        ENC_NO(m.uid = 0, "uid", "no uid");
        #undef ENC_NO
    }

    /* 3. the loader: a variant of this build's manifest, record by record */
    static blob_t MV, K, K2;
    const enc_t *A = enc_with_uid(PT_TERRARIUM ? MODEL_UID_LIZARD_V1 : MODEL_UID_FISH_V3M);
    if (!A) PF("this build's manifest file is missing");
    man_variant(&MV, A, "a1", "A1", "mtest1");
    packs_info_t info;
    const int cases0 = s_cases;
    RA(MV, info, "the manifest variant");
    {
        const model_manifest_t *a = model_provides(), *q = manifest_by_tag("mtest1");
        if (!q || q->builtin || q->uid != info.uid || info.kind != PTPK_KIND_MODEL || info.id != 0 || q->n_goals != a->n_goals || manifest_count() != base0.nm + 1) PF("the manifest variant is not in a spare slot as itself");
        for (int k = 0; k < a->n_goals; k++) if (strcmp(q->goal[k], a->goal[k]) != 0) PF("the variant's goal %d is \"%s\"", k, q->goal[k]);
    }
    const reg_t with_mv = reg_now();
    if (offer_blob(&MV, &info, why, sizeof why) != PACKS_SAME || reg_now().nm != with_mv.nm) PF("the same manifest file twice was not PACKS_SAME (%s)", why);
    /* two names with one uid: the second is another file */
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 1, "ANOTHER NAME", 12); blob_fix(&K); RF(K, "already", "two names with one uid");
    /* the same tag under another uid */
    man_variant(&K, A, "a2", "A2", "mtest1"); RF(K, "has a manifest already", "the same tag under another uid");
    man_variant(&K, A, "a3", "A3", PT_MODEL_TAG); RF(K, "has a manifest already", "a second manifest for the model this build runs");
    packs_drop_loaded();
    if (!reg_counts(base0, reg_now()) || manifest_by_tag("mtest1")) PF("a manifest stayed after packs_drop_loaded");
    /* missing, duplicate and bad records, one at a time */
    struct { unsigned id; const char *word; const char *what; } MISSING[] = {
        { 1, "no NAME", "no NAME" }, { 2, "no UID_NS", "no UID_NS" }, { 0x301, "no MODEL_TAG", "no MODEL_TAG" }, { 0x302, "no MODEL_GOALS", "no MODEL_GOALS" } };
    for (size_t k = 0; k < sizeof MISSING / sizeof MISSING[0]; k++) { blob_copy(&K, MV.b, MV.n); rec_delete(&K, MISSING[k].id); blob_fix(&K); RF(K, MISSING[k].word, MISSING[k].what); }
    struct { unsigned id; const char *word; const char *what; } TWICE[] = {
        { 1, "NAME twice", "NAME twice" }, { 2, "UID_NS twice", "UID_NS twice" }, { 0x301, "MODEL_TAG twice", "MODEL_TAG twice" }, { 0x302, "MODEL_GOALS twice", "MODEL_GOALS twice" } };
    for (size_t k = 0; k < sizeof TWICE / sizeof TWICE[0]; k++) { blob_copy(&K, MV.b, MV.n); if (!rec_dup(&K, TWICE[k].id)) PF("no such record to double"); blob_fix(&K); RF(K, TWICE[k].word, TWICE[k].what); }
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 1, "AAAAAAAAAAAAAAAAAAAAAAAA", 24); blob_fix(&K); RF(K, "NAME", "a name of 24 characters");
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 1, " A", 2); blob_fix(&K); RF(K, "NAME", "a name with a space at its start");
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 2, "Bad NS", 6); blob_fix(&K); RF(K, "UID_NS", "a UID_NS of the wrong characters");
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 2, "ab", 2); blob_fix(&K); RF(K, "UID_NS", "a UID_NS of 2 characters");
    const struct { const char *tag; const char *what; } TAGS[] = { { "bad tag", "a space in the tag" }, { "a/b", "a slash in the tag" }, { "abcdefghijklmnop", "a tag of 16 characters" }, { "", "an empty tag" },
        { "tag\x01x", "a control character in the tag" }, { "t\xc3\xa9", "a non-ASCII tag" } };
    for (size_t k = 0; k < sizeof TAGS / sizeof TAGS[0]; k++) { blob_copy(&K, MV.b, MV.n); rec_set(&K, 0x301, TAGS[k].tag, strlen(TAGS[k].tag)); blob_fix(&K); RF(K, "MODEL_TAG", TAGS[k].what); }
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 0x301, "abcdefghijklmno", 15); blob_fix(&K);
    RA(K, info, "a tag of 15 characters"); if (strcmp(manifest_by_uid(info.uid)->tag, "abcdefghijklmno") != 0) PF("a tag of 15 characters changed"); packs_drop_loaded();
    blob_copy(&K, MV.b, MV.n); rec_set(&K, 0x301, "A.b_c-9", 7); blob_fix(&K);
    RA(K, info, "a tag of every allowed kind"); packs_drop_loaded();
    {
        uint8_t v[200]; size_t n;
        static const char *const G9[] = { "g1", "g2", "g3", "g4", "g5", "g6", "g7", "g8", "g9" };
        static const char *const G8[] = { "g1", "g2", "g3", "g4", "g5", "g6", "g7", "g8" };
        static const char *const TW[] = { "hunt", "hunt" };
        static const char *const UP[] = { "Hunt" };
        static const char *const DG[] = { "9lives" };
        static const char *const LONG[] = { "abcdefghijklmnop" };
        static const char *const OK1[] = { "climb" };
        const struct { const char *const *w; int count, nw; const char *what; bool ok; } GL[] = {
            { TW, 2, 2, "goals twice", false }, { G9, 9, 9, "a 9-goal list", false }, { UP, 1, 1, "a goal in capitals", false }, { DG, 1, 1, "a goal with a digit first", false },
            { LONG, 1, 1, "a goal of 16 characters", false }, { OK1, 0, 1, "a count of 0 over a goal", false }, { OK1, 2, 1, "a count of 2 over one goal", false },
            { G8, 8, 8, "an 8-goal list", true }, { OK1, 1, 1, "one goal", true } };
        for (size_t k = 0; k < sizeof GL / sizeof GL[0]; k++) {
            n = goals_value(v, GL[k].count, GL[k].w, GL[k].nw);
            blob_copy(&K, MV.b, MV.n); rec_set(&K, 0x302, v, n); blob_fix(&K);
            if (GL[k].ok) { RA(K, info, GL[k].what); if (manifest_by_uid(info.uid)->n_goals != GL[k].count) PF("%s: %d goals loaded", GL[k].what, manifest_by_uid(info.uid)->n_goals); packs_drop_loaded(); }
            else RF(K, "MODEL_GOALS", GL[k].what);
        }
        n = goals_value(v, 1, OK1, 1); v[n++] = 0;
        blob_copy(&K, MV.b, MV.n); rec_set(&K, 0x302, v, n); blob_fix(&K); RF(K, "MODEL_GOALS", "a stray byte after the goals");
    }
    /* records of the other kinds: REQUIRED refuses the manifest, an optional one is skipped (a manifest has a name, a uid, a tag and goals, nothing else) */
    const uint8_t one = 1;
    const struct { unsigned id; const char *what; } FOREIGN[] = { { 0x0101, "a species' HABITATS" }, { 0x0110, "LOCOMOTION" }, { 0x0210, "AREAS" }, { 0x0004, "LEGACY_ID" }, { 0x0005, "TEST_ONLY" }, { 0x0010, "WORLDS" }, { 0x0555, "an unknown record" }, { 0x0003, "the retired WORLD" } };
    for (size_t k = 0; k < sizeof FOREIGN / sizeof FOREIGN[0]; k++) {
        blob_copy(&K, MV.b, MV.n); rec_append(&K, FOREIGN[k].id | 0x8000u, &one, 1); blob_fix(&K); RF(K, "REQUIRED", FOREIGN[k].what);
        blob_copy(&K, MV.b, MV.n); rec_append(&K, FOREIGN[k].id, &one, 1); blob_fix(&K);
        RA(K, info, FOREIGN[k].what);
        { const model_manifest_t *q = manifest_by_uid(info.uid), *a = model_provides();
          if (strcmp(q->tag, "mtest1") != 0 || q->n_goals != a->n_goals) PF("%s: skipping the record changed the manifest", FOREIGN[k].what); }
        packs_drop_loaded();
    }
    /* a uid that is not the hash of UID_NS; a cut record; a record past the body */
    blob_copy(&K, MV.b, MV.n); wr32(K.b + 8, rd32(K.b + 8) ^ 0x100u); RF(K, "not the name's", "a uid that is not UID_NS's hash");
    blob_copy(&K, MV.b, MV.n); K.b[K.n - 1] ^= 1; RF(K, "crc", "a body bit flipped, the CRC not fixed");
    blob_copy(&K, MV.b, MV.n); K.b[5] = PTPK_KIND_SPECIES; RF(K, "REQUIRED", "a manifest's body under a species' header");
    blob_copy(&K, MV.b, MV.n); K.b[5] = PTPK_KIND_HABITAT; RF(K, "REQUIRED", "a manifest's body under a habitat's header");
    blob_copy(&K, A->blob, A->len); K.b[5] = PTPK_KIND_SPECIES; RF(K, "REQUIRED", "the built-in manifest as a species");
    /* the pool: MANIFEST_SPARE spare slots, then no room */
    {
        static const char *const TAILS[] = { "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7" };
        static const char *const NAMES[] = { "P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7" };
        static const char *const TAGS2[] = { "pool0", "pool1", "pool2", "pool3", "pool4", "pool5", "pool6", "pool7" };
        if (MANIFEST_SPARE > 7) PF("the pool test knows 7 spare slots at most");
        for (int k = 0; k < MANIFEST_SPARE; k++) { man_variant(&K, A, TAILS[k], NAMES[k], TAGS2[k]); RA(K, info, "a manifest into a spare slot"); }
        if (manifest_count() != PTPK_BUILTIN_MODELS + MANIFEST_SPARE) PF("the pool holds %d manifests, want %d", manifest_count(), PTPK_BUILTIN_MODELS + MANIFEST_SPARE);
        man_variant(&K2, A, TAILS[MANIFEST_SPARE], NAMES[MANIFEST_SPARE], TAGS2[MANIFEST_SPARE]); RF(K2, "no room", "a manifest with the pool full");
        packs_drop_loaded();
        if (manifest_count() != PTPK_BUILTIN_MODELS) PF("the pool holds %d manifests after packs_drop_loaded", manifest_count());
        man_variant(&K2, A, TAILS[0], NAMES[0], TAGS2[0]); RA(K2, info, "a manifest after the drop"); packs_drop_loaded();
    }
    const int refusals = s_cases - cases0;

    /* 4. FUZZ: every bit flip and every cut of every built-in manifest's file is refused, the registries unchanged */
    const reg_t base = reg_now();
    long flips = 0, cuts = 0;
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {
        if (!s_enc[i].def->manifest) continue;
        const blob_t *src = NULL;
        static blob_t F;
        blob_copy(&F, s_enc[i].blob, s_enc[i].len); src = &F;
        blob_t *m = &K;
        for (size_t bit = 0; bit < src->n * 8; bit++) {
            blob_copy(m, src->b, src->n); m->b[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
            tally_t t = { 0, 0 };
            if (man_offer(m, &t, "a bit flip of a built-in manifest's file", base)) return 1;
            if (t.loaded || t.refused != 1) PF("a bit flip (bit %zu) of %s was %s", bit, s_enc[i].def->uid_ns, t.loaded ? "loaded" : "not refused");
            flips++;
        }
        for (size_t cut = 0; cut < src->n; cut++) {
            blob_copy(m, src->b, cut);
            tally_t t = { 0, 0 };
            if (man_offer(m, &t, "a cut of a built-in manifest's file", base)) return 1;
            if (t.loaded || t.refused != 1) PF("a cut at %zu of %s was %s", cut, s_enc[i].def->uid_ns, t.loaded ? "loaded" : "not refused");
            cuts++;
        }
    }
    /* ... then hostile bodies behind a good header (the CRC fixed up): the parser's own work */
    tally_t tot = { 0, 0 };
    long tried = 0;
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {
        if (!s_enc[i].def->manifest) continue;
        man_variant(&MV, &s_enc[i], "v0", "V0", "fuzz0");        /* (a new uid and a new tag: nothing is deduped away) */
        { tally_t t = { 0, 0 }; if (man_offer(&MV, &t, "the fuzz's variant", base)) return 1; if (t.loaded != 1) PF("the fuzz's variant of %s does not load", s_enc[i].def->uid_ns); }
        const size_t blen = MV.n - PTPK_HEADER_BYTES;
        blob_t *m = &K;
        for (size_t bit = 0; bit < blen * 8; bit++) {                                           /* every bit of the body */
            blob_copy(m, MV.b, MV.n); m->b[PTPK_HEADER_BYTES + (bit >> 3)] ^= (uint8_t)(1u << (bit & 7)); blob_fix(m);
            if (man_offer(m, &tot, "a bit flip", base)) return 1;
            tried++;
        }
        for (size_t p = 0; p < blen; p++) for (int val = 0; val < 2; val++) {                  /* every byte set to 0 and to 255 */
            blob_copy(m, MV.b, MV.n); m->b[PTPK_HEADER_BYTES + p] = val ? 0xff : 0x00; blob_fix(m);
            if (man_offer(m, &tot, "a byte set", base)) return 1;
            tried++;
        }
        for (size_t cut = 1; cut < blen; cut++) {                                               /* every cut of the body */
            blob_copy(m, MV.b, PTPK_HEADER_BYTES + cut); blob_fix(m);
            if (man_offer(m, &tot, "a body cut", base)) return 1;
            tried++;
        }
        for (size_t extra = 1; extra <= 9; extra++) {                                           /* stray bytes: a record header that is not one, or an empty one */
            blob_copy(m, MV.b, MV.n); memset(m->b + m->n, 0, extra); m->n += extra; blob_fix(m);
            if (man_offer(m, &tot, "stray bytes", base)) return 1;
            memset(m->b + MV.n, 0xa5, extra); blob_fix(m);
            if (man_offer(m, &tot, "stray bytes", base)) return 1;
            tried += 2;
        }
        for (int it = 0; it < 1500; it++) {                                                     /* several bytes at once */
            blob_copy(m, MV.b, MV.n);
            const int hits = 1 + (int)(rnd() % 4);
            for (int h = 0; h < hits; h++) m->b[PTPK_HEADER_BYTES + rnd() % blen] = (uint8_t)rnd();
            blob_fix(m);
            if (man_offer(m, &tot, "several bytes", base)) return 1;
            tried++;
        }
    }
    for (int it = 0; it < 3000; it++) {                                                         /* garbage behind a manifest's header */
        blob_t *m = &K;
        for (int i = 0; i < PTPK_BUILTIN_N; i++) if (s_enc[i].def->manifest) { memcpy(m->b, s_enc[i].blob, PTPK_HEADER_BYTES); break; }
        const size_t l = 1 + rnd() % 300;
        for (size_t k = 0; k < l; k++) m->b[PTPK_HEADER_BYTES + k] = (uint8_t)rnd();
        m->n = PTPK_HEADER_BYTES + l; blob_fix(m);
        if (man_offer(m, &tot, "garbage", base)) return 1;
        tried++;
    }
    for (int it = 0; it < 3000; it++) {                                                         /* records that are all headers: tags and lengths from the dice */
        blob_t *m = &K;
        for (int i = 0; i < PTPK_BUILTIN_N; i++) if (s_enc[i].def->manifest) { memcpy(m->b, s_enc[i].blob, PTPK_HEADER_BYTES); break; }
        size_t o = PTPK_HEADER_BYTES;
        while (o + 4 < 300) {
            static const unsigned tags[] = { 1, 2, 3, 4, 5, 0x10, 0x101, 0x110, 0x111, 0x210, 0x301, 0x302, 0x301, 0x302, 0x555 };
            const unsigned tag = tags[rnd() % (sizeof tags / sizeof tags[0])] | (rnd() & 1 ? 0x8000u : 0), l = rnd() % 40;
            wr16(m->b + o, tag); wr16(m->b + o + 2, l);
            for (unsigned k = 0; k < l; k++) m->b[o + 4 + k] = (uint8_t)(rnd() % 3 ? 'a' + rnd() % 26 : rnd() % 9);
            o += 4 + l;
        }
        m->n = o; blob_fix(m);
        if (man_offer(m, &tot, "tags and lengths", base)) return 1;
        tried++;
    }
    if (!reg_counts(base, reg_now()) || manifest_count() != PTPK_BUILTIN_MODELS) PF("the registries moved over the fuzz (%d manifests)", manifest_count());
    printf("selftest-caps: manifests as packs: %d built-in manifests encode, read back equal field for field (name, tag, goals, uid = FNV-1a of the uid_ns), are the registry's own, %d equal the file in packs/; "
           "the encoder refuses a bad tag (space, slash, none, 16 long), no or nine goals, a goal twice or in capitals or 16 long, a long or empty name, a uid that is not its name's hash; "
           "the loader: %d cases refused or loaded as they must (a missing or doubled NAME / UID_NS / MODEL_TAG / MODEL_GOALS, a tag of the wrong characters or length, goals twice / nine / in capitals / a count that lies, "
           "another kind's records REQUIRED refused and optional skipped, a uid that is not UID_NS's, the same tag under another uid 'has a manifest already', the same file twice PACKS_SAME, two names with one uid 'already', "
           "the pool full after %d spare slots 'no room'); fuzz: %ld bit flips and %ld cuts of the built-in manifest files all refused, then %ld hostile bodies behind good headers (every bit, every byte as 0 and 255, every cut, stray bytes, "
           "multi-byte hits, garbage, tags and lengths from the dice): %ld refused, %ld loaded and each of those passed manifest_check - the registries back to %d species, %d habitats, %d manifests after each\n",
           n_models, on_disk, refusals, MANIFEST_SPARE, flips, cuts, tried, tot.refused, tot.loaded, base.ns, base.nh, base.nm);
    return 0;
}

/* ================================================================ 8: the unmet-need state */
typedef struct { float x, y, hunger, energy, stress, curiosity, body_c, heat, shed_s, age; uint8_t shed, goal, pose, pad; } look_t;
static void looks(look_t *out) {
    for (int i = 0; i < s_t.n_fish; i++) {
        const fish_t *f = &s_t.fish[i];
        memset(&out[i], 0, sizeof out[i]);
        out[i].x = f->x; out[i].y = f->y; out[i].hunger = f->hunger; out[i].energy = f->energy; out[i].stress = f->stress; out[i].curiosity = f->curiosity;
        out[i].body_c = f->body_c; out[i].heat = f->heat; out[i].shed_s = f->shed_s; out[i].age = progression_age_s(&s_t, i);
        out[i].shed = f->shed; out[i].goal = (uint8_t)f->goal.id; out[i].pose = (uint8_t)f->pose;
    }
}
static long file_read(uint8_t *buf, size_t max) {
    FILE *f = fopen(getenv("POCKET_TANK_SAVE"), "rb"); if (!f) return -1;
    const size_t n = fread(buf, 1, max, f); fclose(f);
    return (long)n;
}
static void put_file(const char *path, const void *b, size_t n) { FILE *f = fopen(path, "wb"); if (f) { if (n) fwrite(b, 1, n, f); fclose(f); } }
#define SAVE_BYTES 2160                    /* sizeof(save_t) today (progression.c): the packs by uid at 2104 (species), 2108 (habitat), their names at 2112 and 2136 */

static uint16_t s_fb[TANK_W * TANK_H], s_ref[TANK_W * TANK_H];
/* the card (NOTICE_PACK_MISSING) the paused tank draws, into a cleared frame: every pixel of it inside the modal's rectangle, and its three lines exactly the
   words the contract gives - PACK NEEDS <NEED> (a bare PACK NEEDS over the need when it would not fit the modal), the pack's name, the world line - drawn by
   the same font at the same places */
static int card_check(const char *label, const char *need, const char *name) {
    const int X = PAGE_X + MSP_MODAL_X, Y = PAGE_Y + MSP_MODAL_Y;
    if (X < 0 || Y < 0 || X + MSP_MODAL_W > TANK_W || Y + MSP_MODAL_H > TANK_H) PF("%s: the modal (%d,%d %dx%d) is not inside the %dx%d frame", label, X, Y, MSP_MODAL_W, MSP_MODAL_H, TANK_W, TANK_H);
    if (strcmp(progression_packs_need(&s_t), need) != 0 || strcmp(progression_packs_missing_name(&s_t), name) != 0)
        PF("%s: the tank says need \"%s\" / name \"%s\", want \"%s\" / \"%s\"", label, progression_packs_need(&s_t), progression_packs_missing_name(&s_t), need, name);
    char title[64], caption[64];
    const char *nm = name[0] ? name : "A PACK THIS TANK USES";
    if (need[0]) {
        snprintf(title, sizeof title, "PACK NEEDS %s", need);
        if (render_text_w(title, 3) > MSP_MODAL_W - 12) { snprintf(title, sizeof title, "PACK NEEDS"); snprintf(caption, sizeof caption, "%s", need); }
        else snprintf(caption, sizeof caption, "%s", nm);
    } else { snprintf(title, sizeof title, "PACK MISSING"); snprintf(caption, sizeof caption, "%s", nm); }
    const char *world = WORLD_STR("THE TANK WAITS FOR IT", "THE TERRARIUM WAITS FOR IT");
    memset(s_fb, 0, sizeof s_fb);
    render_notice(&s_t, s_fb, TANK_W, NOTICE_PACK_MISSING, -1, 0, 1.0f);
    long inside = 0, outside = 0;
    for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) {
        if (!s_fb[y * TANK_W + x]) continue;
        if (x >= X && x < X + MSP_MODAL_W && y >= Y && y < Y + MSP_MODAL_H) inside++; else outside++;
    }
    if (inside < 2000) PF("%s: the card drew %ld pixels", label, inside);
    if (outside) PF("%s: the card drew %ld pixels outside its %dx%d modal (title \"%s\")", label, outside, MSP_MODAL_W, MSP_MODAL_H, title);
    const struct { const char *s; int scale; int y; uint32_t rgb; } LINE[3] = {
        { title, 3, 70, 0xffffff }, { caption, 2, 104, 0x9fd8e2 }, { world, 2, 126, 0x9fd8e2 } };
    const uint16_t bg = s_fb[(Y + 60) * TANK_W + X + 8];          /* the modal's fill: between its icon and its edge, where no line is */
    if (!bg) PF("%s: the modal has no fill", label);
    for (int k = 0; k < 3; k++) {
        memset(s_ref, 0, sizeof s_ref);
        const int w = render_text_w(LINE[k].s, LINE[k].scale);
        if (w > MSP_MODAL_W - 12) PF("%s: the line \"%s\" is %d px wide, the modal has %d", label, LINE[k].s, w, MSP_MODAL_W - 12);
        render_text(s_ref, TANK_W, MSP_MODAL_X + (MSP_MODAL_W - w) / 2, MSP_MODAL_Y + LINE[k].y, LINE[k].scale, LINE[k].rgb, LINE[k].s);
        long lit = 0;
        for (int y = Y + LINE[k].y - 3; y < Y + LINE[k].y + 7 * LINE[k].scale + 3; y++) for (int x = X + 6; x < X + MSP_MODAL_W - 6; x++) {
            const uint16_t want = s_ref[y * TANK_W + x] ? s_ref[y * TANK_W + x] : bg;     /* the text's pixels, the fill everywhere else */
            if (s_ref[y * TANK_W + x]) lit++;
            if (s_fb[y * TANK_W + x] != want) PF("%s: the card's line %d differs from \"%s\" at scale %d (frame %d, %d)", label, k + 1, LINE[k].s, LINE[k].scale, x, y);
        }
        if (lit < 20) PF("%s: \"%s\" drew %ld pixels", label, LINE[k].s, lit);
    }
    return 0;
}

/* a save of a tank that holds a pair both of whose packs are loaded (or a uid nobody has), made from a plain tank's by poking the bytes of the pair, as
   sim/main.c's saves test does (the tank cannot hold such a pair itself: tank_set_packs refuses it). Loaded, it is PAUSED with the defaults drawn, and the
   card says what the pair lacks */
typedef struct { const char *label; uint32_t suid, huid; const char *sname, *hname; const char *need; } scen_t;
static int scenario(const scen_t *sc, const char *dir) {
    static uint8_t base[4096], got[4096];
    char why[300], want_need[40];
    const char *sav = getenv("POCKET_TANK_SAVE");
    remove(sav);
    tank_init(&s_t, 31); progression_wake(&s_t, 0);
    progression_save(&s_t);
    if (file_read(base, sizeof base) != SAVE_BYTES || base[1956] != 1) PF("%s: the plain tank's save is %ld bytes, pack_v %d", sc->label, file_read(base, sizeof base), base[1956]);
    base[1957] = 0; base[1958] = 0;                                        /* (a pack that is not built in has no one-byte id in a save) */
    wr32(base + 2104, sc->suid); wr32(base + 2108, sc->huid);
    memset(base + 2112, 0, 48);
    snprintf((char *)base + 2112, PACK_NAME_MAX, "%s", sc->sname); snprintf((char *)base + 2136, PACK_NAME_MAX, "%s", sc->hname);
    put_file(sav, base, SAVE_BYTES);
    const species_t *sp = species_by_uid(sc->suid); const habitat_t *hb = habitat_by_uid(sc->huid);
    want_need[0] = 0;
    if (sp && hb && packs_check(sp, hb, want_need, sizeof want_need, why, sizeof why)) PF("%s: the pair is held, it should lack something", sc->label);
    if (strcmp(want_need, sc->need) != 0) PF("%s: packs_check says the pair lacks \"%s\" (%s), want \"%s\"", sc->label, want_need, why, sc->need);
    tank_init(&s_t, 32); progression_wake(&s_t, 0);
    if (!s_t.packs_missing) PF("%s: a save of a pair that cannot run did not leave the tank waiting", sc->label);
    if (s_t.pack_id != SPECIES_DEFAULT || s_t.habitat_id != HABITAT_DEFAULT) PF("%s: the stand-in for the pair is %d / %d, not the defaults", sc->label, s_t.pack_id, s_t.habitat_id);
    if (card_check(sc->label, sc->need, sc->sname)) return 1;
    /* it stays paused: nothing will satisfy it, a rescan changes nothing, the save keeps the pair and the tail */
    const double clock0 = s_t.clock;
    for (int n = 0; n < 300; n++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (sim_packs_scan(dir, false) != 0) PF("%s: a rescan of the folder added packs", sc->label);
    sim_packs_poll(true, 3.0f);
    for (int n = 0; n < 90; n++) progression_tick(&s_t, 1.0f / 30);
    if (!s_t.packs_missing || s_t.clock != clock0 || strcmp(progression_packs_need(&s_t), sc->need) != 0 || strcmp(progression_packs_missing_name(&s_t), sc->sname) != 0)
        PF("%s: after a rescan and 13 s the tank is %s, need \"%s\"", sc->label, s_t.packs_missing ? "paused" : "running", progression_packs_need(&s_t));
    if (card_check(sc->label, sc->need, sc->sname)) return 1;
    progression_save(&s_t);
    if (file_read(got, sizeof got) != SAVE_BYTES || memcmp(base + 1956, got + 1956, SAVE_BYTES - 1956) != 0) {
        int k = 1956; while (k < SAVE_BYTES && base[k] == got[k]) k++;
        PF("%s: the save of the waiting tank is not the one it loaded (byte %d: %02x -> %02x)", sc->label, k, base[k], got[k]);
    }
    tank_init(&s_t, 33); progression_wake(&s_t, 0);
    if (!s_t.packs_missing || strcmp(progression_packs_need(&s_t), sc->need) != 0) PF("%s: after a save and a reload the tank does not wait for it", sc->label);
    return 0;
}

static int t_unmet(void) {
    char why[300], want_need[40];
    packs_drop_loaded();
    const char *sav = sim_tmp("caps-unmet.sav"); setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    char dir[600]; snprintf(dir, sizeof dir, "%s", sim_tmp("caps-packs")); mkdir(dir, 0700);
    setenv("POCKET_PACKS_DIR", dir, 1);
    const reg_t base = reg_now();
    static blob_t B;
    char path[700];
    /* the folder: the crossover (habitat, species), and what the scenarios need */
    {
        species_t x; habitat_t p;
        cross_make(&x, &p, 0);
        const char *ew = "";
        #define PUT_H(file, hab, ns) do { habitat_t hh = (hab); hh.id = 0; hh.worlds = 0; hh.test_only = false; hh.builtin = false; hh.uid = fnv(ns); \
            B.n = ptpk_encode_habitat(&hh, ns, B.b, sizeof B.b, &ew); if (!B.n) PF("%s cannot be encoded: %s", ns, ew); snprintf(path, sizeof path, "%s/%s", dir, file); put_file(path, B.b, B.n); } while (0)
        #define PUT_S(file, sp, ns, nm) do { species_t ss = (sp); snprintf(ss.name, sizeof ss.name, "%s", nm); ss.id = 0; ss.worlds = 0; ss.test_only = false; ss.builtin = false; ss.uid = fnv(ns); \
            B.n = ptpk_encode_species(&ss, ns, B.b, sizeof B.b, &ew); if (!B.n) PF("%s cannot be encoded: %s", ns, ew); snprintf(path, sizeof path, "%s/%s", dir, file); put_file(path, B.b, B.n); } while (0)
        PUT_H("b-paludarium.ptpk", p, P_NS);
        habitat_t nodish = *habitat_get(HABITAT_DESERT); snprintf(nodish.name, sizeof nodish.name, "NO DISH"); nodish.provides &= ~CAP_MEALWORM; nodish.dish = false;
        PUT_H("b-nodish.ptpk", nodish, "pt.test.no-dish");
        habitat_t nocrick = *habitat_get(HABITAT_DESERT); snprintf(nocrick.name, sizeof nocrick.name, "NO CRICKETS"); nocrick.provides &= ~CAP_CRICKET;
        PUT_H("b-nocrickets.ptpk", nocrick, "pt.test.no-crickets");
        PUT_S("c-crossover.ptpk", x, X_NS, "CROSSOVER");
        species_t g = *species_get(SPECIES_GECKO);                       /* a gecko that lists four habitats: the desert, two of its variants, the freshwater */
        g.n_habitat_uids = 4; g.habitat_uid[0] = HABITAT_UID_DESERT; g.habitat_uid[1] = fnv("pt.test.no-dish"); g.habitat_uid[2] = fnv("pt.test.no-crickets"); g.habitat_uid[3] = HABITAT_UID_FRESHWATER;
        PUT_S("c-gecko4.ptpk", g, "pt.test.gecko4", "GECKO OF FOUR");
        species_t go = *s_ws; snprintf(go.goal[0], GOAL_NAME_MAX, "%s", other_goal());
        PUT_S("c-othergoal.ptpk", go, "pt.test.other-goal", "OTHER GOAL");
        species_t gl = *s_ws; snprintf(gl.goal[0], GOAL_NAME_MAX, "investigate_far");
        PUT_S("c-longgoal.ptpk", gl, "pt.test.long-goal", "LONG GOAL");
        model_manifest_t mm; memset(&mm, 0, sizeof mm);
        snprintf(mm.name, sizeof mm.name, "TEST LONG"); snprintf(mm.tag, sizeof mm.tag, "testlong"); mm.uid = fnv("pt.model.test-long");
        mm.n_goals = 2; snprintf(mm.goal[0], GOAL_NAME_MAX, "investigate_far"); snprintf(mm.goal[1], GOAL_NAME_MAX, "climb");
        B.n = ptpk_encode_manifest(&mm, "pt.model.test-long", B.b, sizeof B.b, &ew);
        if (!B.n) PF("the manifest cannot be encoded: %s", ew);
        snprintf(path, sizeof path, "%s/a-manifest.ptpk", dir); put_file(path, B.b, B.n);
        #undef PUT_H
        #undef PUT_S
    }
    const uint32_t xuid = fnv(X_NS), puid = fnv(P_NS);
    firmware_caps_override(true, CAPS_FW_TERRARIUM | CAPS_FW_AQUARIUM);          /* a firmware with both modules */
    const int added = sim_packs_scan(dir, false);
    if (added != 8) { firmware_caps_override(false, 0); PF("the folder added %d packs, want 8 (a manifest, three habitats, four species)", added); }
    const species_t *xs = species_by_uid(xuid); const habitat_t *ph = habitat_by_uid(puid);
    if (!xs || !ph || !manifest_by_tag("testlong")) { firmware_caps_override(false, 0); PF("the folder's packs are not in the registry"); }
    if (sim_packs_scan(dir, false) != 0) { firmware_caps_override(false, 0); PF("a second scan of the folder added packs"); }

    /* A tank on the crossover, in its own state, saved */
    tank_init(&s_t, 24); progression_wake(&s_t, 0);
    if (!tank_set_packs(&s_t, ph->id, xs->id, why, sizeof why)) { firmware_caps_override(false, 0); PF("a tank refused the crossover from the folder: %s", why); }
    for (int i = 0; i < s_t.n_fish; i++) {
        fish_t *f = &s_t.fish[i];
        f->hunger = 3.25f + i; f->energy = 6.5f - i; f->stress = 1.5f + i; f->curiosity = 4.75f; f->body_c = (xs->t_lo + xs->t_hi) * 0.5f - 0.75f * i;
        f->heat = species_heat_of(xs, f->body_c);
        if (xs->sheds) { f->shed = i ? SHED_SOON : SHED_SHEDDING; f->shed_s = 123.5f + i; f->shed_damp = 1.25f * i; f->shed_clean = (uint16_t)(3 + i); }
    }
    const int humid = tank_humid_hide(&s_t);
    if (humid >= 0) s_t.moist[humid] = 0.3125f;
    progression_save(&s_t);
    static uint8_t a[4096], b[4096], c[4096];
    if (file_read(a, sizeof a) != SAVE_BYTES) { firmware_caps_override(false, 0); PF("the save is %ld bytes, want %d", file_read(a, sizeof a), SAVE_BYTES); }
    if (a[1956] != 1 || a[1957] != 0 || a[1958] != 0 || rd32(a + 2104) != xuid || rd32(a + 2108) != puid || strncmp((const char *)a + 2112, "CROSSOVER", PACK_NAME_MAX) != 0 || strncmp((const char *)a + 2136, "PALUDARIUM", PACK_NAME_MAX) != 0) {
        firmware_caps_override(false, 0); PF("the save does not carry the pair: pack_v %d, ids %d/%d, uids %08x/%08x, names \"%.24s\" / \"%.24s\"", a[1956], a[1957], a[1958], (unsigned)rd32(a + 2104), (unsigned)rd32(a + 2108), (const char *)a + 2112, (const char *)a + 2136);
    }
    /* the firmware loses SWIM: the pair is no longer held, and the tank loads PAUSED with its pair kept */
    firmware_caps_override(true, CAPS_FW_TERRARIUM);
    if (!packs_check(xs, ph, want_need, sizeof want_need, why, sizeof why) && strcmp(want_need, "SWIM") != 0) PF("without SWIM the crossover lacks \"%s\" (%s), not SWIM", want_need, why);
    if (packs_compatible(xs, ph, why, sizeof why)) PF("the crossover is held without SWIM");
    look_t was[N_FISH_MAX], now[N_FISH_MAX];
    tank_init(&s_t, 25); progression_wake(&s_t, 0);
    if (!s_t.packs_missing) PF("a save of a pair that needs SWIM did not leave the tank waiting for a firmware that has it");
    if (s_t.pack_id != SPECIES_DEFAULT || s_t.habitat_id != HABITAT_DEFAULT) PF("the stand-in for the pair is %d / %d, not the defaults", s_t.pack_id, s_t.habitat_id);
    if (strcmp(progression_packs_need(&s_t), "SWIM") != 0 || strcmp(progression_packs_missing_name(&s_t), "CROSSOVER") != 0) PF("the card would say need \"%s\" for \"%s\"", progression_packs_need(&s_t), progression_packs_missing_name(&s_t));
    if (species_by_uid(xuid) != xs || habitat_by_uid(puid) != ph) PF("the packs were dropped by the firmware losing a module");
    for (int i = 0; i < s_t.n_fish; i++) {
        const fish_t *f = &s_t.fish[i];
        if (f->hunger != 3.25f + i || f->energy != 6.5f - i || f->body_c != (xs->t_lo + xs->t_hi) * 0.5f - 0.75f * i || (xs->sheds && (f->shed_s != 123.5f + i || f->shed_clean != 3 + i)))
            PF("animal %d did not come back as saved (hunger %.2f, energy %.2f, %.2f C)", i, f->hunger, f->energy, f->body_c);
    }
    /* paused: no time passes, nothing is asked, nothing grows, the hand does nothing, a night is not lived */
    looks(was);
    const double clock0 = s_t.clock; const int food0 = tank_food_count(&s_t), crickets0 = tank_crickets_about(&s_t), fed0 = s_t.player_feedings;
    for (int n = 0; n < 4000; n++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    tank_touch_tap(&s_t, TANK_W / 2, 10); tank_feed(&s_t, TANK_W / 2, 3); tank_touch_hold(&s_t, 100, 100); tank_touch_drag(&s_t, 120, 120);
    tank_tick(&s_t, 1.0f / 30, NULL);
    tank_tick_sleep(&s_t, 7200); progression_slept(&s_t, 7200);
    looks(now);
    if (memcmp(was, now, sizeof(look_t) * (size_t)s_t.n_fish) != 0) PF("a paused tank's animals moved, aged or changed");
    if (s_t.clock != clock0 || tank_food_count(&s_t) != food0 || tank_crickets_about(&s_t) != crickets0 || s_t.player_feedings != fed0) PF("a paused tank's clock or food moved (clock %.2f -> %.2f)", clock0, s_t.clock);
    if (!s_t.packs_missing || strcmp(progression_packs_need(&s_t), "SWIM") != 0) PF("the tank stopped waiting for SWIM, or its need changed to \"%s\" over 133 s", progression_packs_need(&s_t));
    /* it says so: PACK NEEDS SWIM, up until a tap, and again after a while */
    notice_sync(&s_t);
    for (int n = 0; n < 120 && !notice_current(); n++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current() || notice_current()->kind != NOTICE_PACK_MISSING) PF("no PACK NEEDS card for a tank waiting for SWIM");
    if (notice_take_cue() < 0) PF("the PACK NEEDS card has no cue");
    if (card_check("the crossover without SWIM", "SWIM", "CROSSOVER")) return 1;
    for (int n = 0; n < (int)(NOTICE_UP_S * 60) + 120; n++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current()) PF("the PACK NEEDS card left by itself (it waits for a tap)");
    if (!notice_dismiss() || notice_current()) PF("a tap did not close the PACK NEEDS card");
    for (int n = 0; n < (int)(NOTICE_PACKS_AGAIN_S * 60) + 600 && !notice_current(); n++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current() || notice_current()->kind != NOTICE_PACK_MISSING) PF("the card did not come back while the tank still waits");
    /* every save while it waits writes the pair and the tail back as they were, and a reload still waits */
    progression_save(&s_t);
    if (file_read(b, sizeof b) != SAVE_BYTES) PF("the save while the pair cannot run is %ld bytes", file_read(b, sizeof b));
    if (memcmp(a + 1956, b + 1956, SAVE_BYTES - 1956) != 0) { int k = 1956; while (a[k] == b[k]) k++; PF("the pair or the tail changed while the tank waited for SWIM (byte %d: %02x -> %02x)", k, a[k], b[k]); }
    tank_init(&s_t, 26); progression_wake(&s_t, 0);
    if (!s_t.packs_missing || strcmp(progression_packs_need(&s_t), "SWIM") != 0 || strcmp(progression_packs_missing_name(&s_t), "CROSSOVER") != 0) PF("after a save and a reload the tank no longer waits for SWIM");
    for (int n = 0; n < 20000; n++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }        /* a heartbeat's worth (SAVE_HEARTBEAT_S 600 s), paused: it saves too */
    if (file_read(c, sizeof c) != SAVE_BYTES || memcmp(a + 1956, c + 1956, SAVE_BYTES - 1956) != 0) PF("the heartbeat's save of a tank waiting for SWIM changed the pair or the tail");
    /* the firmware gets SWIM back: the next look (every second) resumes the tank */
    firmware_caps_override(false, 0);
    float waited = 0;
    while (s_t.packs_missing && waited < 5.0f) { progression_tick(&s_t, 1.0f / 30); waited += 1.0f / 30; }
    if (s_t.packs_missing || waited > 2.0f) PF("the tank did not resume within two seconds of the firmware having SWIM (%.1f s, missing %d)", waited, s_t.packs_missing);
    if (tank_species(&s_t)->uid != xuid || tank_habitat(&s_t)->uid != puid || progression_packs_need(&s_t)[0] || progression_packs_missing_name(&s_t)[0]) PF("the tank resumed on another pair, or still says a need (\"%s\")", progression_packs_need(&s_t));
    for (int i = 0; i < s_t.n_fish; i++) {
        const fish_t *f = &s_t.fish[i];
        if (f->hunger != 3.25f + i || f->heat != species_heat_of(tank_species(&s_t), f->body_c) || (xs->sheds && (f->shed_s != 123.5f + i || f->shed_clean != 3 + i)))
            PF("animal %d resumed as something else (hunger %.2f, heat %.2f)", i, f->hunger, f->heat);
    }
    if (humid >= 0 && s_t.moist[humid] != 0.3125f) PF("the humid hide's moisture %.4f did not come back", s_t.moist[humid]);
    notice_tick(&s_t, 1.0f / 60, false);
    if (notice_current() || notice_pending()) PF("the PACK NEEDS card stayed after the firmware had SWIM");
    const double age0 = progression_age_s(&s_t, 0); const double clock1 = s_t.clock;
    for (int n = 0; n < 900; n++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (!(s_t.clock > clock1 + 29) || !(progression_age_s(&s_t, 0) > age0 + 29)) PF("a resumed tank does not live (clock %.1f -> %.1f, age %.1f -> %.1f)", clock1, s_t.clock, age0, progression_age_s(&s_t, 0));
    progression_save(&s_t);
    if (file_read(b, sizeof b) != SAVE_BYTES || rd32(b + 2104) != xuid || rd32(b + 2108) != puid) PF("the resumed tank saved another pair than the one it waited for");
    tank_init(&s_t, 27); progression_wake(&s_t, 0);
    if (s_t.packs_missing || tank_species(&s_t)->uid != xuid || tank_habitat(&s_t)->uid != puid) PF("a reload with the module present did not hold the crossover");

    /* the other needs a card can say: each from a real save (the pair's bytes poked), the card's words exact and inside the modal */
    const uint32_t gecko4 = fnv("pt.test.gecko4");
    const scen_t SC[] = {
        { "MEALWORM (no dish)",       gecko4, fnv("pt.test.no-dish"),     "GECKO OF FOUR", "NO DISH",     "MEALWORM" },
        { "CRICKET (no crickets)",    gecko4, fnv("pt.test.no-crickets"), "GECKO OF FOUR", "NO CRICKETS", "CRICKET" },
        { "WALK (a gecko in water)",  gecko4, HABITAT_UID_FRESHWATER,     "GECKO OF FOUR", "FRESHWATER",  "WALK" },
        { "a goal of the other model", fnv("pt.test.other-goal"), s_wh->uid, "OTHER GOAL", s_wh->name,    PT_TERRARIUM ? "GOAL VISIT BUBBLES" : "GOAL INVESTIGATE" },
        { "the longest goal",         fnv("pt.test.long-goal"),  s_wh->uid, "LONG GOAL",  s_wh->name,     "GOAL INVESTIGATE FAR" },
        { "another habitat",          s_ws->uid, PT_TERRARIUM ? HABITAT_UID_FRESHWATER : HABITAT_UID_DESERT, s_ws->name, PT_TERRARIUM ? "FRESHWATER" : "DESERT", "ANOTHER HABITAT" },
        { "a pack that is not here",  0x0badf00du, s_wh->uid,            "GHOST PACK",   s_wh->name,     "" } };
    for (size_t k = 0; k < sizeof SC / sizeof SC[0]; k++) if (scenario(&SC[k], dir)) return 1;
    /* the notice and the tank are put back as a reset leaves them */
    progression_reset(&s_t, 29);
    notice_sync(&s_t);
    packs_drop_loaded();
    tank_init(&s_t, 28); progression_wake(&s_t, 0);
    if (s_t.packs_missing || !reg_counts(base, reg_now())) PF("the test left a tank waiting, or packs loaded");
    unsetenv("POCKET_PACKS_DIR"); setenv("POCKET_PACKS_DIR", sim_tmp("packs"), 1);
    printf("selftest-caps: a tank whose pair cannot run (the crossover when the firmware loses SWIM) waits like one whose pack is missing: the defaults drawn, animals / clock / food unmoved over 133 s of frames, a night and the hand; "
           "progression says the need SWIM and the pack CROSSOVER, and the card is PACK NEEDS SWIM - its three lines the contract's words in the font's own pixels, nothing outside the %d x %d modal - up until a tap and again after %.0f s; "
           "its uids, ids, names and per-animal tail written back unchanged by every save, a reload and a heartbeat; with SWIM back it resumes in %.1f s (the card goes, the animals as saved, living again, the same pair saved); "
           "%d saves of pairs this build cannot run, made by poking the bytes: %s - each paused, its card exact and inside the modal, staying paused over a rescan and 13 s, its pair and tail kept\n",
           MSP_MODAL_W, MSP_MODAL_H, NOTICE_PACKS_AGAIN_S, waited, (int)(sizeof SC / sizeof SC[0]), "MEALWORM, CRICKET, WALK, GOAL VISIT BUBBLES / GOAL INVESTIGATE, GOAL INVESTIGATE FAR, ANOTHER HABITAT, and a pack that is not here (PACK MISSING)");
    return 0;
}

/* ================================================================ */
int selftest_caps(void) {
    packs_drop_loaded();                                           /* (what an earlier test left: nothing) */
    firmware_caps_override(false, 0);
    s_ws = species_get(SPECIES_DEFAULT); s_wh = habitat_get(HABITAT_DEFAULT);
    if (!s_ws || !s_wh) PF("the world's default pack is not loaded");
    if (enc_all() || t_pairs() || t_derived() || t_reasons() || t_crossover() || t_bits() || t_goals() || t_manifests() || t_unmet()) { firmware_caps_override(false, 0); packs_drop_loaded(); return 1; }
    printf("selftest-caps OK\n");
    return 0;
}
