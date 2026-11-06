#define _DEFAULT_SOURCE   /* mkdir, setenv under -std=c11 (glibc) */
/* selftest_ptpk.c - issue #4 phase 1: the PTPK pack format (docs/ptpk.md), its encoder (common/packs/ptpk_write.c) and its loader
 * (common/packs/ptpk.c), the registry (species.c, habitat.c), the simulator's folder of packs (packs_dir.c) and a tank whose pack is
 * missing, through the real calls:
 *
 *   ./fishsim --selftest-ptpk
 *
 *   1  known answers: CRC-32 and FNV-1a-32
 *   2  every built-in pack's file - what the encoder makes of its const species_t / habitat_t, the bytes the firmware loads at boot -
 *      is exactly what the contract says (the 20-byte header, the CRC, the uid of its UID_NS and the constant the table holds,
 *      habitats first), is the one the registry holds, and is the file under ../packs/ (`make packs` writes it) when that is there
 *   3  the round trip: every built-in pack encoded, read back (ptpk_read_*) and found in the registry equals its const struct, field
 *      for field (morphs and patterns inline); and the encoder refuses what the format cannot hold, with its reason
 *   4  the fish pack's drives and goals are tank.h's and tank.c's (the swim runs on those constants)
 *   5  every bit flip and every cut of every built-in file is refused, whatever the registry holds, and a flipped or cut
 *      file never changes it
 *   6  hostile bodies behind a good header (the CRC fixed up, so the record parser sees the damage): every bit, byte values,
 *      every cut, stray bytes, garbage, several bytes at once - never a crash, and what loads passes its rules
 *   7  records one at a time: an unknown optional one is skipped, an unknown REQUIRED one, a duplicate, a wrong size, a NaN, a bad
 *      goal, bad text, a bad header, a body of 2049 bytes, a missing record, a species with no habitat, the other world's pack
 *   8  uid collisions, the pool's size, the ids a pack from a file is given
 *   9  a folder of packs: a new pack is available, a corrupt, a short, an empty, a huge and a foreign file are refused, the built-in
 *      pack's own file is dropped quietly
 *  10  a tank whose pack is missing: it pauses, keeps its uids and its tail through a save and a reload, says so, ignores the
 *      hand, and resumes when the pack comes (the folder read again, or the pack added) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include "tank.h"
#include "progression.h"
#include "notice.h"
#include "render.h"
#include "packs/ptpk.h"
#include "packs/ptpk_write.h"
#include "packs/builtin.h"
#include "packs_dir.h"
#include "sim_tmp.h"

static const char *s_name = "selftest-ptpk";
#define PF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static tank_t s_t;                                           /* (tank_t is big: never on the stack) */

/* ================================================================ the dump: a pack as text, its fields as bits */
static char s_out[24576]; static size_t s_len;
static void emit(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int n = vsnprintf(s_out + s_len, sizeof s_out - s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += (size_t)n;
    if (s_len >= sizeof s_out) s_len = sizeof s_out - 1;
}
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
#define F(p, f)     emit("  " #f " %08x\n", bits((p)->f))
#define I(p, f)     emit("  " #f " %d\n", (int)(p)->f)
#define FA(p, f, n) for (int k_ = 0; k_ < (n); k_++) emit("  " #f "[%d] %08x\n", k_, bits((p)->f[k_]))
/* every field a file carries, and (runtime) the registry's own: `builtin` and the habitats' bit mask, which a const struct leaves
   to the loader. An unused morph or habitat slot is dumped too: a const struct and a loaded pack are both zero there */
static void dump_species(const species_t *p, bool runtime) {
    emit("species %d\n", p->id);
    emit("  name %s\n", p->name);
    I(p, id); I(p, test_only); I(p, world); emit("  uid %08x\n", (unsigned)p->uid);
    I(p, n_habitat_uids);
    for (int k = 0; k < SPECIES_HABITATS_MAX; k++) emit("  habitat_uid[%d] %08x\n", k, (unsigned)p->habitat_uid[k]);
    if (runtime) { I(p, builtin); emit("  habitats %08x\n", (unsigned)p->habitats); }
    I(p, schedule); F(p, settle_s);
    F(p, t_cold); F(p, t_lo); F(p, t_hi); F(p, t_hot); FA(p, tau_s, 4);
    F(p, hunger_per_s); F(p, energy_per_s); F(p, energy_rest_per_s); F(p, curiosity_per_s); F(p, hunger_per_cricket);
    F(p, sleep_hunger_per_h); F(p, sleep_energy_per_h); F(p, sleep_stress_per_h);
    FA(p, walk_px_s, 4); FA(p, bout_s, 2); FA(p, pause_s, 2); F(p, turn_s);
    FA(p, stage_s, 3); I(p, pop_cap); I(p, sheds); FA(p, shed_every_s, 4); F(p, shed_soon_s); F(p, shed_window_s);
    I(p, shed_style); I(p, diet); I(p, plan);
    F(&p->geom, length_px); F(&p->geom, head_w); F(&p->geom, limb_len); F(&p->geom, tail_fat); F(&p->geom, eye_r); I(&p->geom, eyelids);
    I(p, n_morphs);
    for (int i = 0; i < SPECIES_MORPHS_MAX; i++) emit("  morph[%d] %s %06x %06x\n", i, p->morphs[i].name, (unsigned)p->morphs[i].body, (unsigned)p->morphs[i].accent);
    for (int s = 0; s < 4; s++) emit("  pattern[%d] %s\n", s, p->pattern[s].name);
    emit("  goals %04x\n", (unsigned)p->goals);
}
static void dump_habitat(const habitat_t *h, bool runtime) {
    emit("habitat %d\n", h->id);
    emit("  name %s\n", h->name);
    I(h, id); I(h, world); I(h, test_only); emit("  uid %08x\n", (unsigned)h->uid);
    if (runtime) I(h, builtin);
    F(h, floor_warm_c); F(h, floor_cool_c); F(h, night_drop_c); F(h, rise_c);
    I(h, bask_surf); F(h, bask_u); F(h, bask_c); F(h, bask_r_px);
    I(h, n_hides);
    for (int k = 0; k < HABITAT_HIDES_MAX; k++)
        emit("  hide[%d] kind %d u %08x t_lit_c %08x t_dark_c %08x\n", k, (int)h->hides[k].kind, bits(h->hides[k].u), bits(h->hides[k].t_lit_c), bits(h->hides[k].t_dark_c));
    I(h, n_rocks);
    for (int k = 0; k < HABITAT_ROCKS_MAX; k++) {
        const rock_t *r = &h->rocks[k];
        emit("  rock[%d] n %d shelf %d", k, r->n, (int)r->shelf);
        for (int i = 0; i < ROCK_PTS_MAX; i++) emit(" u%d %08x v%d %08x", i, bits(r->u[i]), i, bits(r->v[i]));
        emit("\n");
    }
    I(h, n_ramps);
    for (int k = 0; k < HABITAT_RAMPS_MAX; k++)
        emit("  ramp[%d] lo %d hi %d u_lo %08x u_hi %08x\n", k, h->ramps[k].lo, h->ramps[k].hi, bits(h->ramps[k].u_lo), bits(h->ramps[k].u_hi));
    F(h, moist_start); F(h, moist_dry_per_s); F(h, humidity);
    I(h, dish); F(h, dish_u);
}

/* ================================================================ files as bytes, edited in place */
typedef struct { uint8_t b[PTPK_FILE_MAX + 4096]; size_t n; } blob_t;
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static void blob_copy(blob_t *k, const uint8_t *b, size_t n) { memcpy(k->b, b, n); k->n = n; }
static bool rec_find(const blob_t *k, unsigned id, size_t *at, size_t *vlen) {       /* the record's tag offset and its value's length */
    for (size_t o = PTPK_HEADER_BYTES; o + 4 <= k->n; ) {
        const unsigned tag = rd16(k->b + o), l = rd16(k->b + o + 2);
        if (o + 4 + l > k->n) return false;                        /* (a record that runs past the bytes: nothing of it to find - the loader's refusal to see) */
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
/* a new pack made of a built-in's file: UID_NS and NAME get new last characters (the length stays); the header is made good */
static void blob_variant(blob_t *k, const uint8_t *b, size_t n, const char *ns_tail, const char *name_tail) {
    blob_copy(k, b, n);
    size_t at, l;
    if (rec_find(k, 2, &at, &l)) memcpy(k->b + at + 4 + l - strlen(ns_tail), ns_tail, strlen(ns_tail));
    if (rec_find(k, 1, &at, &l)) memcpy(k->b + at + 4 + l - strlen(name_tail), name_tail, strlen(name_tail));
    blob_fix(k);
}
/* the built-in packs as files: what the firmware loads at boot, encoded from the const structs (builtin_encode) once for the whole test, in the table's order */
#define N_BUILTIN (PTPK_BUILTIN_HABITATS + PTPK_BUILTIN_SPECIES)
typedef struct { const builtin_pack_t *def; uint8_t blob[PTPK_FILE_MAX]; size_t len; } enc_t;
static enc_t s_enc[N_BUILTIN];
static int enc_all(void) {
    for (int i = 0; i < N_BUILTIN; i++) {
        const char *why = "";
        s_enc[i].def = &PTPK_BUILTIN[i];
        s_enc[i].len = builtin_encode(&PTPK_BUILTIN[i], s_enc[i].blob, sizeof s_enc[i].blob, &why);
        if (!s_enc[i].len) PF("built-in pack %s cannot be encoded: %s", PTPK_BUILTIN[i].uid_ns, why);
    }
    return 0;
}
static const enc_t *builtin_with_uid(uint32_t uid) {
    for (int i = 0; i < N_BUILTIN; i++) if (rd32(s_enc[i].blob + 8) == uid) return &s_enc[i];
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

static uint32_t s_rng = 0x2545f491u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* the registry's own state, to see a refusal changed nothing */
typedef struct { int ns, nh; uint32_t gen; } reg_t;
static reg_t reg_now(void) { reg_t r = { species_count(), habitat_count(), habitat_generation() }; return r; }
static bool reg_same(reg_t a, reg_t b) { return a.ns == b.ns && a.nh == b.nh && a.gen == b.gen; }

/* ================================================================ 1: known answers */
static int t_known(void) {
    if (ptpk_crc32((const uint8_t *)"123456789", 9) != 0xcbf43926u) PF("CRC-32 of 123456789 is %08x, want cbf43926", (unsigned)ptpk_crc32((const uint8_t *)"123456789", 9));
    if (ptpk_crc32((const uint8_t *)"", 0) != 0) PF("CRC-32 of nothing is not 0");
    if (ptpk_fnv1a((const uint8_t *)"", 0) != 0x811c9dc5u || ptpk_fnv1a((const uint8_t *)"a", 1) != 0xe40c292cu || ptpk_fnv1a((const uint8_t *)"foobar", 6) != 0xbf9cf968u)
        PF("FNV-1a-32: '' %08x 'a' %08x 'foobar' %08x", (unsigned)ptpk_fnv1a((const uint8_t *)"", 0), (unsigned)ptpk_fnv1a((const uint8_t *)"a", 1), (unsigned)ptpk_fnv1a((const uint8_t *)"foobar", 6));
    printf("selftest-ptpk: CRC-32 (123456789 -> cbf43926) and FNV-1a-32 (a -> e40c292c, foobar -> bf9cf968) give the known answers\n");
    return 0;
}

/* ================================================================ 2: the built-in files */
static int t_builtin(void) {
    char why[200];
    int on_disk = 0;
    for (int i = 0; i < N_BUILTIN; i++) {
        const enc_t *e = &s_enc[i];
        const uint8_t *b = e->blob; const size_t n = e->len;
        const char *ns = e->def->uid_ns;
        const uint32_t own = e->def->species ? e->def->species->uid : e->def->habitat->uid;
        blob_t k; blob_copy(&k, b, n);
        size_t at, l;
        if (!rec_find(&k, 2, &at, &l)) PF("built-in pack %d has no UID_NS", i);
        if (l != strlen(ns) || memcmp(k.b + at + 4, ns, l)) PF("%s: the file's UID_NS record is not the table's name", ns);
        ptpk_header_t h;
        if (!ptpk_open(b, n, &h, why, sizeof why)) PF("%s: %s", ns, why);
        if (memcmp(b, "PTPK", 4) || b[4] != 1 || rd16(b + 6) != 0 || n != 20 + rd32(b + 12) || rd32(b + 12) == 0 || rd32(b + 12) > 2048)
            PF("%s: the header is not the contract's (fmt %d, flags %04x, %zu bytes for a body of %u)", ns, b[4], (unsigned)rd16(b + 6), n, (unsigned)rd32(b + 12));
        if (rd32(b + 16) != ptpk_crc32(b + 20, n - 20)) PF("%s: crc %08x, the body's is %08x", ns, (unsigned)rd32(b + 16), (unsigned)ptpk_crc32(b + 20, n - 20));
        if (rd32(b + 8) != ptpk_fnv1a(k.b + at + 4, l) || !rd32(b + 8)) PF("%s: uid %08x is not the hash of its name", ns, (unsigned)rd32(b + 8));
        if (e->def->uid != rd32(b + 8) || own != rd32(b + 8))     /* (the name's hash, as the file has it, is the uid constant: in the table and in the struct) */
            PF("%s: the uid constant (the table's %08x, the struct's %08x) is not the hash of its name, %08x", ns, (unsigned)e->def->uid, (unsigned)own, (unsigned)rd32(b + 8));
        const bool hab = i < PTPK_BUILTIN_HABITATS;
        if (b[5] != (hab ? PTPK_KIND_HABITAT : PTPK_KIND_SPECIES) || hab != (e->def->habitat != NULL)) PF("%s: kind %d: the habitats come first (a species lists them by uid)", ns, b[5]);
        packs_info_t info;
        if (packs_add(b, n, PACKS_BUILTIN, &info, why, sizeof why) != PACKS_SAME) PF("%s: the registry does not hold this file (%s)", ns, why);
        (void)info;
        /* the file under packs/ (`make packs` writes it; the simulator runs from sim/) is this one, when it is there */
        char path[300]; snprintf(path, sizeof path, "../packs/%s.ptpk", ns);
        FILE *f = fopen(path, "rb");
        if (f) {
            static uint8_t disk[PTPK_FILE_MAX + 8];
            const size_t got = fread(disk, 1, sizeof disk, f); fclose(f);
            if (got != n || memcmp(disk, b, n)) PF("%s is not what the encoder makes of %s's const struct (`make packs` writes it)", path, ns);
            on_disk++;
        }
    }
    if (species_count() < PTPK_BUILTIN_SPECIES || habitat_count() < PTPK_BUILTIN_HABITATS) PF("the registry holds %d species and %d habitats, the build ships %d and %d", species_count(), habitat_count(), PTPK_BUILTIN_SPECIES, PTPK_BUILTIN_HABITATS);
    for (int i = 0; i < species_count(); i++) {
        const species_t *p = species_at(i);
        if (i < PTPK_BUILTIN_SPECIES && !p->builtin) PF("%s is not marked built in", p->name);
        if (species_by_uid(p->uid) != p || species_get(p->id) != p) PF("%s: the lookups by uid and id do not find it", p->name);
    }
    printf("selftest-ptpk: %d built-in packs (%d habitats, then %d species), each encoded from its const struct: 20-byte headers, CRCs and uids exact (every uid constant is its name's hash), "
           "each the one the registry holds; %d of them equal the file in packs/ (`make packs` writes those)\n", N_BUILTIN, PTPK_BUILTIN_HABITATS, PTPK_BUILTIN_SPECIES, on_disk);
    return 0;
}

/* ================================================================ 3: encoded, read back, equal field for field */
static const char *species_text(const species_t *p, bool runtime) { s_len = 0; s_out[0] = 0; dump_species(p, runtime); return s_out; }
static const char *habitat_text(const habitat_t *h, bool runtime) { s_len = 0; s_out[0] = 0; dump_habitat(h, runtime); return s_out; }
static int lines_of(const char *s) { int n = 0; for (; *s; s++) n += *s == '\n'; return n; }
static int dumps_differ(const char *pack, const char *what, const char *want, const char *got) {   /* 0: the same text; else the FAIL, with the first line that differs */
    if (strcmp(want, got) == 0) return 0;
    const char *a = want, *b = got; int line = 1;
    while (*a && *b) {
        const char *ea = strchr(a, '\n'), *eb = strchr(b, '\n');
        const size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
        if (la != lb || memcmp(a, b, la)) { printf("FAIL: %s: %s %s differs from its const struct at line %d: the struct says \"%.*s\", this \"%.*s\"\n", s_name, pack, what, line, (int)la, a, (int)lb, b); return 1; }
        a += la + (ea ? 1 : 0); b += lb + (eb ? 1 : 0); line++;
    }
    printf("FAIL: %s: %s %s and its const struct differ in length (%zu and %zu bytes)\n", s_name, pack, what, strlen(want), strlen(got));
    return 1;
}
static int t_roundtrip(void) {
    char why[200];
    static char want[8192];
    int lines = 0;
    for (int i = 0; i < N_BUILTIN; i++) {                          /* every built-in: the struct -> the file -> ptpk_read_* -> the same struct, and the registry's copy */
        const enc_t *e = &s_enc[i]; const builtin_pack_t *d = e->def;
        ptpk_header_t h; uint8_t legacy = 0;
        if (!ptpk_open(e->blob, e->len, &h, why, sizeof why)) PF("%s: %s", d->uid_ns, why);
        if (d->species) {
            static species_t got;
            if (!ptpk_read_species(&h, &got, &legacy, why, sizeof why)) PF("%s: the encoder's file is not read back: %s", d->uid_ns, why);
            got.id = legacy;                                       /* (what species_commit gives a built-in pack) */
            snprintf(want, sizeof want, "%s", species_text(d->species, false));
            if (dumps_differ(d->uid_ns, "read back,", want, species_text(&got, false))) return 1;
            const species_t *r = species_by_uid(d->species->uid);
            if (!r) PF("%s: it is not in the registry", d->uid_ns);
            if (dumps_differ(d->uid_ns, "as the registry holds it,", want, species_text(r, false))) return 1;
            uint32_t mask = 0;                                     /* the runtime's own fields: built in, and the habitats it lists as bits of their ids */
            for (int k = 0; k < d->species->n_habitat_uids; k++) { const habitat_t *hb = habitat_by_uid(d->species->habitat_uid[k]); if (hb) mask |= 1u << hb->id; }
            if (!r->builtin || !mask || r->habitats != mask) PF("%s: the registry has it built in %d with habitats %08x; the habitats it lists, loaded, are %08x", d->uid_ns, r->builtin, (unsigned)r->habitats, (unsigned)mask);
        } else {
            static habitat_t got;
            if (!ptpk_read_habitat(&h, &got, &legacy, why, sizeof why)) PF("%s: the encoder's file is not read back: %s", d->uid_ns, why);
            got.id = legacy;
            snprintf(want, sizeof want, "%s", habitat_text(d->habitat, false));
            if (dumps_differ(d->uid_ns, "read back,", want, habitat_text(&got, false))) return 1;
            const habitat_t *r = habitat_by_uid(d->habitat->uid);
            if (!r) PF("%s: it is not in the registry", d->uid_ns);
            if (dumps_differ(d->uid_ns, "as the registry holds it,", want, habitat_text(r, false))) return 1;
            if (!r->builtin) PF("%s: the registry does not have it built in", d->uid_ns);
        }
        lines += lines_of(want);
    }
    /* what the encoder will not write: each refused with its reason, and no file */
    static uint8_t buf[PTPK_FILE_MAX];
    const builtin_pack_t *sd = &PTPK_BUILTIN[PTPK_BUILTIN_HABITATS], *hd = &PTPK_BUILTIN[0];       /* the first species and the first habitat */
    species_t p; habitat_t hb;
    const char *w;
    int cases = 0;
    const uint32_t NAN_BITS = 0x7fc00000u, INF_BITS = 0xff800000u;
#define NOT_S(mod, word, what) do { p = *sd->species; mod; w = ""; cases++;                                                                        \
        if (ptpk_encode_species(&p, sd->uid_ns, buf, sizeof buf, &w) != 0) PF("the encoder wrote " what);                                          \
        if (!strstr(w, word)) PF(what ": refused, but for \"%s\" (want one with \"%s\")", w, word); } while (0)
#define NOT_H(mod, word, what) do { hb = *hd->habitat; mod; w = ""; cases++;                                                                       \
        if (ptpk_encode_habitat(&hb, hd->uid_ns, buf, sizeof buf, &w) != 0) PF("the encoder wrote " what);                                         \
        if (!strstr(w, word)) PF(what ": refused, but for \"%s\" (want one with \"%s\")", w, word); } while (0)
    NOT_S(memcpy(p.morphs[0].name, "ABCDEFGHIJKL", 12), "morph's name", "a morph name of 12 characters");
    NOT_S(p.n_morphs = 7, "six morphs", "seven morphs");
    NOT_S(p.morphs[0].body = 0x1000000, "colour", "a colour past 0xffffff");
    NOT_S(memcpy(&p.t_cold, &NAN_BITS, 4), "NaN", "a NaN");
    NOT_S(memcpy(&p.settle_s, &INF_BITS, 4), "infinity", "an infinity");
    NOT_S(p.uid ^= 1, "hash of uid_ns", "a uid that is not the name's hash");
    NOT_S(p.goals |= 1u << 15, "no word for", "a goal the world's list has no word for");
    NOT_S(p.goals = 0, "no goal", "no goals");
    NOT_S(p.n_habitat_uids = 0, "no habitat", "no habitat");
    NOT_S((p.n_habitat_uids = 2, p.habitat_uid[1] = p.habitat_uid[0]), "twice", "a habitat listed twice");
    NOT_S(memset(p.name, 'A', sizeof p.name), "name", "a name with no end");
    NOT_S(p.name[0] = ' ', "name", "a name that starts with a space");
    NOT_S(p.schedule = (sched_t)3, "schedule", "schedule 3");
    NOT_S(p.plan = (body_plan_t)2, "body plan", "body plan 2");
    NOT_S(p.diet = 16, "diet", "a diet bit that is nobody's");
    NOT_S(p.pattern[2].name[0] = 0, "pattern", "three of the four stages' patterns");
    NOT_S(p.world = (world_t)5, "world", "world 5");
    NOT_H(hb.n_hides = 5, "four hides", "five hides");
    NOT_H(hb.hides[0].kind = (hide_kind_t)3, "hide's kind", "a hide of kind 3");
    NOT_H(hb.n_rocks = 5, "four rocks", "five rocks");
    NOT_H(hb.rocks[1].n = 5, "rock of", "a rock of five points");
    NOT_H(hb.n_ramps = 7, "six ramps", "seven ramps");
    NOT_H(memcpy(&hb.floor_warm_c, &NAN_BITS, 4), "NaN", "a NaN in a habitat");
    NOT_H(hb.id = 32, "legacy id", "legacy id 32");
    NOT_H(hb.uid ^= 1, "hash of uid_ns", "a habitat uid that is not the name's hash");
    NOT_H(hb.world = (world_t)3, "world", "a habitat of world 3");
    p = *sd->species; w = "";                                      /* the name's own limits, the buffer's, and a pack with no legacy id */
    if (ptpk_encode_species(&p, "Pt.Bad", buf, sizeof buf, &w) != 0 || !strstr(w, "uid_ns")) PF("uid_ns \"Pt.Bad\" (an uppercase letter) was written or refused for \"%s\"", w);
    if (ptpk_encode_species(&p, "pt.fish.rosters", buf, sizeof buf, &w) != 0 || !strstr(w, "hash")) PF("a uid_ns that is not the uid's name was written or refused for \"%s\"", w);
    if (ptpk_encode_species(&p, sd->uid_ns, buf, s_enc[PTPK_BUILTIN_HABITATS].len - 1, &w) != 0 || !strstr(w, "too small")) PF("a buffer one byte short for the pack got a file (or the reason \"%s\")", w);
    if (ptpk_encode_species(&p, sd->uid_ns, buf, PTPK_HEADER_BYTES - 1, &w) != 0 || !strstr(w, "20-byte header")) PF("a buffer without room for the header got a file (or the reason \"%s\")", w);
    if (ptpk_encode_species(&p, sd->uid_ns, buf, 0, &w) != 0) PF("a buffer of nothing got a file");
    cases += 5;
    p.id = 0;
    {
        const size_t n = ptpk_encode_species(&p, sd->uid_ns, buf, sizeof buf, &w);
        blob_t k; blob_copy(&k, buf, n); size_t at, l;
        ptpk_header_t h; uint8_t legacy = 7; static species_t got;
        if (!n || rec_find(&k, 4, &at, &l)) PF("a pack with no legacy id: %zu bytes, a LEGACY_ID record there: %d", n, n ? rec_find(&k, 4, &at, &l) : 0);
        if (!ptpk_open(buf, n, &h, why, sizeof why) || !ptpk_read_species(&h, &got, &legacy, why, sizeof why) || legacy != 0) PF("a pack with no legacy id is not read back as one (%s, legacy %d)", why, legacy);
    }
    printf("selftest-ptpk: %d built-in packs encoded, read back (ptpk_read_*) and found in the registry equal to their const structs, field for field - names, morphs and patterns inline, "
           "floats as bits, the habitats by uid, the legacy id (%d dump lines); the encoder refuses %d things the format cannot hold - a long text, a NaN, a stale uid, an unlisted goal, "
           "too many of a list, a buffer too small - each with its reason\n", N_BUILTIN, lines, cases);
    return 0;
}

/* ================================================================ 4: the fish pack and the swim */
static int t_fish_numbers(void) {
    const species_t *p = species_get(SPECIES_FISH);
    if (!p) PF("no fish pack");
    if (p->hunger_per_s != HUNGER_PER_S || p->energy_per_s != FISH_ENERGY_PER_S || p->energy_rest_per_s != FISH_ENERGY_REST_PER_S || p->curiosity_per_s != FISH_CURIOSITY_PER_S
        || p->hunger_per_cricket != FISH_HUNGER_PER_PELLET || p->sleep_hunger_per_h != SLEEP_HUNGER_PER_H || p->sleep_energy_per_h != SLEEP_ENERGY_PER_H || p->sleep_stress_per_h != SLEEP_STRESS_PER_H)
        PF("the fish pack's drives are not tank.h's THE FISH'S DRIVES (hunger %g/%g, energy %g/%g, rest %g/%g, curiosity %g/%g, pellet %g/%g)", p->hunger_per_s, HUNGER_PER_S, p->energy_per_s,
           FISH_ENERGY_PER_S, p->energy_rest_per_s, FISH_ENERGY_REST_PER_S, p->curiosity_per_s, FISH_CURIOSITY_PER_S, p->hunger_per_cricket, FISH_HUNGER_PER_PELLET);
    if (p->pop_cap != POP_CAP) PF("the fish pack's pop_cap %d is not POP_CAP %d", p->pop_cap, POP_CAP);
    if (p->goals != FISH_SCHEMA_GOALS) PF("the fish pack's goals %03x are not the fish's eight", p->goals);
    for (int k = 0; k < FISH_GOALS; k++) if (strcmp(FISH_GOAL_NAMES[k], GOAL_NAMES[GOAL_FISH_FIRST + k]) != 0) PF("goal %d: species.c says %s, tank.c %s", k, FISH_GOAL_NAMES[k], GOAL_NAMES[GOAL_FISH_FIRST + k]);
    for (int k = 0; k < LZ_GOALS; k++) if (strcmp(LZ_GOAL_NAMES[k], GOAL_NAMES[GOAL_LZ_FIRST + k]) != 0) PF("lizard goal %d: species.c says %s, tank.c %s", k, LZ_GOAL_NAMES[k], GOAL_NAMES[GOAL_LZ_FIRST + k]);
    printf("selftest-ptpk: the fish pack's drives, its cap (%d) and its eight goals are tank.h's and tank.c's; the lizard's seven are tank.c's too\n", POP_CAP);
    return 0;
}

/* ================================================================ 5: every flip and every cut of every built-in file */
static int t_flips(void) {
    const reg_t before = reg_now();
    char why[200];
    long flips = 0, cuts = 0;
    for (int i = 0; i < N_BUILTIN; i++) {
        const uint8_t *b = s_enc[i].blob; const size_t n = s_enc[i].len;
        uint8_t *c = malloc(n);
        for (size_t bit = 0; bit < n * 8; bit++) {
            memcpy(c, b, n); c[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
            uint8_t *e = malloc(n); memcpy(e, c, n);                                           /* (exact size: a read past the end is seen) */
            const packs_result_t r = packs_add(e, n, PACKS_FILE, NULL, why, sizeof why);
            free(e);
            if (r != PACKS_REFUSED) { free(c); PF("pack %d with bit %zu flipped was not refused (%d)", i, bit, (int)r); }
            flips++;
        }
        free(c);
        for (size_t m = 0; m < n; m++) {                                                       /* every cut: nothing, then every prefix */
            if (offer(b, m, NULL, why, sizeof why) != PACKS_REFUSED) PF("pack %d cut to %zu of %zu bytes was not refused", i, m, n);
            cuts++;
        }
        uint8_t *g = malloc(n + 33); memcpy(g, b, n); memset(g + n, 0x5a, 33);
        for (size_t extra = 1; extra <= 33; extra += 8) if (offer(g, n + extra, NULL, why, sizeof why) != PACKS_REFUSED) { free(g); PF("pack %d with %zu stray bytes was not refused", i, extra); }
        free(g);
    }
    if (offer(NULL, 0, NULL, why, sizeof why) != PACKS_REFUSED) PF("no bytes at all were not refused");
    if (!reg_same(before, reg_now())) PF("a refused file changed the registry (%d/%d species, %d/%d habitats)", species_count(), before.ns, habitat_count(), before.nh);
    printf("selftest-ptpk: %ld bit flips and %ld cuts of the built-in files (and stray bytes after them): every one refused, the registry never moved\n", flips, cuts);
    return 0;
}

/* ================================================================ 6: hostile bodies behind a good header */
static int fuzz_check_loaded(const packs_info_t *info, const char *what) {      /* what loaded passes its rules and is well formed */
    char why[200];
    if (info->kind == PTPK_KIND_SPECIES) {
        const species_t *p = species_by_uid(info->uid);
        if (!p) PF("%s: loaded but not found by uid (kind %d, uid %08x, id %d, \"%s\")", what, info->kind, (unsigned)info->uid, info->id, info->name);
        if (!species_check(p, why, sizeof why)) PF("%s: a species that fails its rules was loaded: %s", what, why);
        if (strnlen(p->name, PACK_NAME_MAX) >= PACK_NAME_MAX || p->n_morphs > SPECIES_MORPHS_MAX || p->n_habitat_uids > SPECIES_HABITATS_MAX || !p->habitats) PF("%s: a species with a bad shape was loaded", what);
        for (int s = 0; s < 4; s++) if (strnlen(p->pattern[s].name, PATTERN_NAME_MAX) >= PATTERN_NAME_MAX) PF("%s: an unterminated pattern name", what);
        for (int m = 0; m < p->n_morphs; m++) if (strnlen(p->morphs[m].name, MORPH_NAME_MAX) >= MORPH_NAME_MAX) PF("%s: an unterminated morph name", what);
    } else {
        const habitat_t *h = habitat_by_uid(info->uid);
        if (!h) PF("%s: loaded but not found by uid (kind %d, uid %08x, id %d, \"%s\")", what, info->kind, (unsigned)info->uid, info->id, info->name);
        if (!habitat_check(h, why, sizeof why)) PF("%s: a habitat that fails its rules was loaded: %s", what, why);
        if (strnlen(h->name, PACK_NAME_MAX) >= PACK_NAME_MAX || h->n_hides > HABITAT_HIDES_MAX || h->n_rocks > HABITAT_ROCKS_MAX || h->n_ramps > HABITAT_RAMPS_MAX) PF("%s: a habitat with a bad shape was loaded", what);
    }
    return 0;
}
typedef struct { long refused, loaded; } tally_t;
static int fuzz_offer(const blob_t *m, tally_t *t, const char *what, reg_t base) {
    packs_info_t info; char why[200];
    switch (offer_blob(m, &info, why, sizeof why)) {
    case PACKS_ADDED:
        if (fuzz_check_loaded(&info, what)) return 1;
        t->loaded++;
        packs_drop_loaded();
        break;
    case PACKS_SAME: PF("%s: a file that is not the one loaded was called the same", what);
    default:
        t->refused++;
        break;
    }
    if (species_count() != base.ns || habitat_count() != base.nh) PF("%s: the registry holds %d species and %d habitats, it held %d and %d", what, species_count(), habitat_count(), base.ns, base.nh);
    return 0;
}
static int t_body_fuzz(void) {
    tally_t tot = { 0, 0 };
    long tried = 0;
    const reg_t base = reg_now();
    for (int i = 0; i < N_BUILTIN; i++) {
        blob_t v; blob_variant(&v, s_enc[i].blob, s_enc[i].len, "v0", "V0");                    /* (a pack of a new uid: the dedupe by uid does not hide the parser) */
        char why[200]; packs_info_t info;
        if (offer_blob(&v, &info, why, sizeof why) != PACKS_ADDED) PF("the variant of built-in pack %d does not load: %s", i, why);
        packs_drop_loaded();
        const size_t blen = v.n - PTPK_HEADER_BYTES;
        blob_t m;
        tally_t t = { 0, 0 };
        for (size_t bit = 0; bit < blen * 8; bit++) {                                           /* every bit of the body */
            m = v; m.b[PTPK_HEADER_BYTES + (bit >> 3)] ^= (uint8_t)(1u << (bit & 7)); blob_fix(&m);
            if (fuzz_offer(&m, &t, "a bit flip", base)) return 1;
            tried++;
        }
        for (size_t p = 0; p < blen; p++) for (int val = 0; val < 2; val++) {                  /* every byte set to 0 and to 255 */
            m = v; m.b[PTPK_HEADER_BYTES + p] = val ? 0xff : 0x00; blob_fix(&m);
            if (fuzz_offer(&m, &t, "a byte set", base)) return 1;
            tried++;
        }
        for (size_t cut = 1; cut < blen; cut++) {                                               /* every cut of the body */
            m = v; m.n = PTPK_HEADER_BYTES + cut; blob_fix(&m);
            if (fuzz_offer(&m, &t, "a body cut", base)) return 1;
            tried++;
        }
        for (size_t extra = 1; extra <= 9; extra++) {                                           /* stray bytes: a record header that is not one, or an empty one */
            m = v; memset(m.b + m.n, 0, extra); m.n += extra; blob_fix(&m);
            if (fuzz_offer(&m, &t, "stray bytes", base)) return 1;
            memset(m.b + v.n, 0xa5, extra); blob_fix(&m);
            if (fuzz_offer(&m, &t, "stray bytes", base)) return 1;
            tried += 2;
        }
        for (int it = 0; it < 1500; it++) {                                                     /* several bytes at once */
            m = v;
            const int hits = 1 + (int)(rnd() % 4);
            for (int h = 0; h < hits; h++) m.b[PTPK_HEADER_BYTES + rnd() % blen] = (uint8_t)rnd();
            blob_fix(&m);
            if (fuzz_offer(&m, &t, "several bytes", base)) return 1;
            tried++;
        }
        tot.refused += t.refused; tot.loaded += t.loaded;
    }
    for (int it = 0; it < 6000; it++) {                                                         /* garbage behind a good header */
        blob_t m; memcpy(m.b, s_enc[0].blob, PTPK_HEADER_BYTES);
        const size_t l = 1 + rnd() % 600;
        for (size_t k = 0; k < l; k++) m.b[PTPK_HEADER_BYTES + k] = (uint8_t)rnd();
        m.n = PTPK_HEADER_BYTES + l; blob_fix(&m);
        if (fuzz_offer(&m, &tot, "garbage", base)) return 1;
        tried++;
    }
    for (int it = 0; it < 3000; it++) {                                                         /* records that are all headers: tags and lengths from the dice */
        blob_t m; memcpy(m.b, s_enc[PTPK_BUILTIN_HABITATS].blob, PTPK_HEADER_BYTES);              /* (a species' header) */
        size_t o = PTPK_HEADER_BYTES;
        while (o + 4 < 500) {
            const unsigned tags[] = { 1, 2, 3, 4, 5, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10a, 0x10b, 0x10c, 0x201, 0x203, 0x999 };
            unsigned tag = tags[rnd() % (sizeof tags / sizeof tags[0])] | (rnd() & 1 ? 0x8000u : 0), l = rnd() % 48;
            wr16(m.b + o, tag); wr16(m.b + o + 2, l);
            for (unsigned k = 0; k < l; k++) m.b[o + 4 + k] = (uint8_t)(rnd() % 5 ? rnd() % 0x80 : rnd());
            o += 4 + l;
        }
        m.n = o; blob_fix(&m);
        if (fuzz_offer(&m, &tot, "tags and lengths", base)) return 1;
        tried++;
    }
    if (species_count() != base.ns || habitat_count() != base.nh) PF("the registry moved over the fuzz (%d/%d species, %d/%d habitats)", species_count(), base.ns, habitat_count(), base.nh);
    printf("selftest-ptpk: %ld hostile bodies behind good headers (every bit, byte values, every cut, stray bytes, garbage, tags and lengths from the dice): %ld refused, %ld loaded - and each of those passed species_check / habitat_check\n",
           tried, tot.refused, tot.loaded);
    return 0;
}

/* ================================================================ 7: records one at a time */
static const species_t *s_ws;       /* the world's default species ... */
static const habitat_t *s_wh;       /* ... and its habitat */
static int s_cases;                                          /* the cases expect() has run */
static int expect(const blob_t *k, packs_result_t want, const char *word, const char *what) {
    char why[300] = ""; packs_info_t info;
    const reg_t before = reg_now();
    s_cases++;
    const packs_result_t r = offer_blob(k, &info, why, sizeof why);
    if (r != want) PF("%s: %s (%s)", what, r == PACKS_ADDED ? "it loaded" : r == PACKS_SAME ? "it was called the same" : "it was refused", why);
    if (r == PACKS_REFUSED) {
        if (word && !strstr(why, word)) PF("%s: refused, but for \"%s\" (want one with \"%s\")", what, why, word);
        if (!reg_same(before, reg_now())) PF("%s: refused, and the registry moved", what);
    } else if (r == PACKS_ADDED && fuzz_check_loaded(&info, what)) return 1;
    return 0;
}
#define EXPECT(k, want, word, what) do { if (expect(&(k), want, word, what)) return 1; } while (0)
#define EXPECT_ADDED(k, what) do { if (expect(&(k), PACKS_ADDED, NULL, what)) return 1; packs_drop_loaded(); } while (0)

static int t_records(void) {
    blob_t G, D, k;
    const enc_t *gb = builtin_with_uid(species_get(SPECIES_GECKO)->uid), *db = builtin_with_uid(habitat_get(HABITAT_DESERT)->uid);
    if (!gb || !db) PF("the gecko or the desert is not built in");
    blob_variant(&G, gb->blob, gb->len, "r0", "R0");
    blob_variant(&D, db->blob, db->len, "r0", "R0");
    packs_info_t info; char why[300];
    char base[8192];
    if (offer_blob(&G, &info, why, sizeof why) != PACKS_ADDED) PF("the gecko variant does not load: %s", why);
    s_len = 0; dump_species(species_by_uid(info.uid), true); memcpy(base, s_out, s_len + 1);
    packs_drop_loaded();
    s_cases = 0;
    /* unknown records: an optional one is skipped (the pack loads as it did), a REQUIRED one refuses the pack */
    k = G; rec_append(&k, 0x0555, "abc", 3); blob_fix(&k);
    if (expect(&k, PACKS_ADDED, NULL, "an unknown optional record")) return 1;
    s_len = 0; dump_species(species_by_uid(rd32(k.b + 8)), true); packs_drop_loaded();
    if (strcmp(base, s_out) != 0) PF("a pack with an unknown optional record loaded differently");
    k = G; rec_append(&k, 0x0000, NULL, 0); blob_fix(&k); EXPECT_ADDED(k, "an empty unknown optional record (id 0)");
    k = G; rec_append(&k, 0x0555, "abc", 3); rec_append(&k, 0x0555, "abc", 3); blob_fix(&k); EXPECT_ADDED(k, "an unknown optional record twice (skipped twice)");
    k = G; rec_append(&k, 0x8555, "abc", 3); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "REQUIRED", "an unknown REQUIRED record");
    k = G; rec_append(&k, 0x8203, "\x01", 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "REQUIRED", "a habitat's record, REQUIRED, in a species");
    k = G; rec_append(&k, 0x0203, "\x00", 1); blob_fix(&k); EXPECT_ADDED(k, "a habitat's record, optional, in a species (skipped)");
    /* records that are not what they say */
    size_t at, l;
    k = G; if (!rec_find(&k, 0x103, &at, &l)) PF("no HEAT"); { uint8_t rec[4 + 64]; memcpy(rec, k.b + at, 4 + l); rec_append(&k, rd16(rec), rec + 4, l); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "twice", "HEAT twice"); }
    k = G; { uint8_t v[33] = { 0 }; rec_find(&k, 0x103, &at, &l); memcpy(v, k.b + at + 4, l); rec_set(&k, 0x103, v, l + 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "bytes", "HEAT one byte too long"); }
    k = G; { uint8_t v[31]; rec_find(&k, 0x103, &at, &l); memcpy(v, k.b + at + 4, l - 1); rec_set(&k, 0x103, v, l - 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "shorter", "HEAT one byte short"); }
    k = G; rec_find(&k, 0x103, &at, &l); wr32(k.b + at + 4, 0x7fc00000u); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "NaN", "a NaN in HEAT");
    k = G; rec_find(&k, 0x103, &at, &l); wr32(k.b + at + 8, 0xff800000u); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "infinity", "an infinity in HEAT");
    k = G; rec_find(&k, 0x103, &at, &l); wr32(k.b + at + 4, bits(40.0f)); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "heat band", "t_cold over t_lo (the rules refuse it, with their words)");
    k = G; rec_find(&k, 0x101, &at, &l); wr16(k.b + at + 2, 0x7000); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "past the end", "a record that runs past the body");
    k = G; rec_find(&k, 0x10c, &at, &l); memcpy(k.b + at + 4 + 3, "k", 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "goal", "a goal that is nobody's (hunk)");
    k = G; { const uint8_t v[] = { 1, 9, 's', 'e', 'e', 'k', '_', 'f', 'o', 'o', 'd' }; rec_set(&k, 0x10c, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "terrarium's goal list", "the fish's goal in a lizard"); }
    k = G; { const uint8_t v[] = { 2, 4, 'h', 'u', 'n', 't', 4, 'h', 'u', 'n', 't' }; rec_set(&k, 0x10c, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "twice", "a goal listed twice"); }
    k = G; { const uint8_t v[] = { 0 }; rec_set(&k, 0x10c, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "no goals"); }
    k = G; { const uint8_t v[] = { 1, 4, 'h', 'u', 'n' }; rec_set(&k, 0x10c, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "shorter", "a goal cut short"); }
    k = G; { const uint8_t v[] = { 1, 4, 'h', 'u', 'n', 't', 0 }; rec_set(&k, 0x10c, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "bytes", "a byte after the goals"); }
    /* text and enums */
    k = G; rec_find(&k, 1, &at, &l); k.b[at + 4] = 0x07; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "NAME", "a control character in the name");
    k = G; rec_find(&k, 1, &at, &l); k.b[at + 4] = ' '; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "NAME", "a name that starts with a space");
    k = G; { uint8_t v[24]; memset(v, 'A', 24); rec_set(&k, 1, v, 24); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "NAME", "a name of 24 characters"); }
    k = G; { uint8_t v[23]; memset(v, 'A', 23); rec_set(&k, 1, v, 23); blob_fix(&k); EXPECT_ADDED(k, "a name of 23 characters"); }
    k = G; rec_set(&k, 1, "", 0); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "NAME", "a name of nothing");
    k = G; rec_find(&k, 2, &at, &l); k.b[at + 4] = 'P'; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "UID_NS", "an uppercase letter in UID_NS");
    k = G; rec_set(&k, 2, "pt", 2); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "UID_NS", "a UID_NS of two characters");
    k = G; { uint8_t w = 0; rec_set(&k, 3, &w, 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "world 0"); w = 3; rec_set(&k, 3, &w, 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "world 3"); }
    k = G; { uint8_t v = 0; rec_set(&k, 4, &v, 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "legacy id 0"); }
    k = G; { uint8_t v = 0x10; rec_set(&k, 0x108, &v, 1); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "a diet bit that is nobody's"); }
    k = G; rec_find(&k, 0x102, &at, &l); k.b[at + 4] = 3; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "schedule 3");
    k = G; rec_find(&k, 0x102, &at, &l); k.b[at + 4] = 1; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "SCHED_DAY", "a dusk schedule (the rules: only the day runs)");
    k = G; rec_find(&k, 0x109, &at, &l); k.b[at + 4] = 2; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "body plan 2");
    k = G; rec_find(&k, 0x10a, &at, &l); k.b[at + 4] = 7; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "seven morphs");
    k = G; rec_find(&k, 0x10b, &at, &l); k.b[at + 4] = 3; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "three patterns");
    k = G; rec_find(&k, 0x10a, &at, &l); wr32(k.b + at + 4 + 1 + 1 + 6, 0x1000000u); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "a colour past 0xffffff");
    k = G; rec_find(&k, 0x10a, &at, &l); k.b[at + 4 + 1 + 1] = 0x7f; blob_fix(&k); EXPECT(k, PACKS_REFUSED, "MORPHS", "a morph's name that is not printable");
    k = G; { uint8_t v[1 + 5 * 24]; v[0] = 5; for (int m = 0; m < 5; m++) { v[1 + m * 24] = 12; memset(v + 2 + m * 24, 'M', 12); memset(v + 14 + m * 24, 0, 11); } rec_set(&k, 0x10a, v, 1 + 5 * 24); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "MORPHS", "a morph name of 12 characters"); }
    /* habitats: the lists and their limits, an absent record */
    k = D; { uint8_t v[1 + 7 * 10] = { 7 }; rec_set(&k, 0x205, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "seven ramps"); }
    k = D; { uint8_t v[1 + 13] = { 1, 3 }; rec_set(&k, 0x203, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "a hide of kind 3"); }
    k = D; { uint8_t v[1 + 2 + 5 * 8] = { 1, 5, 0 }; rec_set(&k, 0x204, v, sizeof v); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "a rock of five points"); }
    k = D; rec_delete(&k, 0x207); blob_fix(&k);
    if (expect(&k, PACKS_ADDED, NULL, "a desert without its DISH record")) return 1;
    if (habitat_by_uid(rd32(k.b + 8))->dish) PF("a desert without a DISH record has a dish");
    packs_drop_loaded();
    k = D; rec_delete(&k, 0x205); rec_delete(&k, 0x204); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "basking spot", "a desert without rocks or ramps (the basking spot is on rock 3)");
    k = D; rec_find(&k, 0x201, &at, &l); wr32(k.b + at + 4, bits(20.0f)); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "warm end", "a desert whose warm end is cooler (the rules)");
    /* the records a pack cannot do without */
    static const struct { unsigned id; const char *name; } MUST[] = { { 1, "NAME" }, { 2, "UID_NS" }, { 3, "WORLD" }, { 0x101, "HABITATS" }, { 0x102, "ACTIVITY" }, { 0x103, "HEAT" },
        { 0x104, "DRIVES" }, { 0x106, "LIFE" }, { 0x108, "DIET" }, { 0x109, "BODY" }, { 0x10c, "GOALS" } };
    for (size_t m = 0; m < sizeof MUST / sizeof MUST[0]; m++) {
        char need[16], what[64]; snprintf(need, sizeof need, "no %s", MUST[m].name); snprintf(what, sizeof what, "a species without %s", MUST[m].name);
        k = G; rec_delete(&k, MUST[m].id); blob_fix(&k); EXPECT(k, PACKS_REFUSED, need, what);
    }
    k = D; rec_delete(&k, 0x201); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "no CLIMATE", "a habitat without CLIMATE");
    k = D; rec_delete(&k, 0x202); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "no BASK", "a habitat without BASK");
    k = D; rec_delete(&k, 0x206); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "no MOISTURE", "a habitat without MOISTURE");
    k = G; rec_delete(&k, 0x105); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "walk_px_s", "a terrarium pack without WALK (the rules: it walks)");
    k = G; rec_delete(&k, 0x107); blob_fix(&k);
    if (expect(&k, PACKS_ADDED, NULL, "a gecko without SHED")) return 1;
    if (species_by_uid(rd32(k.b + 8))->sheds) PF("a pack without SHED sheds");
    packs_drop_loaded();
    /* the header */
    k = G; k.b[4] = 2; EXPECT(k, PACKS_REFUSED, "newer", "fmt 2");
    k = G; k.b[4] = 0; EXPECT(k, PACKS_REFUSED, "fmt 0", "fmt 0");
    k = G; k.b[5] = 3; EXPECT(k, PACKS_REFUSED, "manifest", "a model manifest");
    k = G; k.b[5] = 9; EXPECT(k, PACKS_REFUSED, "no pack kind", "kind 9");
    k = G; k.b[5] = 2; EXPECT(k, PACKS_REFUSED, "", "a species' file called a habitat");
    k = G; wr16(k.b + 6, 1); EXPECT(k, PACKS_REFUSED, "flags", "a flag set");
    k = G; wr32(k.b + 8, 0); EXPECT(k, PACKS_REFUSED, "uid 0", "uid 0");
    k = G; wr32(k.b + 8, rd32(k.b + 8) ^ 1); EXPECT(k, PACKS_REFUSED, "not the name's", "a uid that is not its name's");
    k = G; wr32(k.b + 12, rd32(k.b + 12) + 1); EXPECT(k, PACKS_REFUSED, "header's length", "a length that is one too long");
    k = G; k.b[0] = 'X'; EXPECT(k, PACKS_REFUSED, "magic", "no magic");
    /* the body's size: 2048 bytes loads, 2049 does not */
    k = G; { static uint8_t pad[2100]; const size_t room = 2048 - (k.n - PTPK_HEADER_BYTES) - 4; rec_append(&k, 0x0556, pad, room); blob_fix(&k);
             if (k.n - PTPK_HEADER_BYTES != 2048) PF("the test's own sums: a body of %zu", k.n - PTPK_HEADER_BYTES);
             EXPECT_ADDED(k, "a body of 2048 bytes");
             rec_append(&k, 0x0557, NULL, 0); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "a body of 2052", "a body of 2052 bytes"); }
    k = G; { static uint8_t pad[2100]; const size_t room = 2049 - (k.n - PTPK_HEADER_BYTES) - 4; rec_append(&k, 0x0556, pad, room); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "a body of 2049", "a body of 2049 bytes"); }
    /* a species that lives nowhere that is loaded */
    k = G; { uint8_t v[5] = { 1, 0x78, 0x56, 0x34, 0x12 }; rec_set(&k, 0x101, v, 5); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "none is loaded", "a species whose habitat is not loaded"); }
    k = G; { uint8_t v[9] = { 2, 1, 0, 0, 0, 1, 0, 0, 0 }; rec_set(&k, 0x101, v, 9); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "a habitat listed twice"); }
    k = G; { uint8_t v[5] = { 1, 0, 0, 0, 0 }; rec_set(&k, 0x101, v, 5); blob_fix(&k); EXPECT(k, PACKS_REFUSED, "range", "habitat uid 0"); }
    /* the other world's pack: refused by a release image, kept by the simulator */
    {
        const bool other_terr = !PT_TERRARIUM;                           /* the world this build is not: a lizard in the aquarium's bin, a fish in the terrarium's */
        const enc_t *ob = builtin_with_uid(species_get(other_terr ? SPECIES_GECKO : SPECIES_FISH)->uid);
        blob_variant(&k, ob->blob, ob->len, "r1", "R1");
        packs_world_filter(true);
        const int r = expect(&k, PACKS_REFUSED, "firmware", "another world's pack, in a release image's filter");
        packs_world_filter(PT_ALL_PACKS ? false : true);
        if (r) return 1;
        EXPECT_ADDED(k, "another world's pack, in the simulator");
    }
    printf("selftest-ptpk: %d single-record cases: an unknown optional record skipped (the pack loads as it did), an unknown REQUIRED one refused; a duplicate, a wrong size, a NaN, "
           "a bad goal, bad text, a bad header, bodies of 2048 / 2049 bytes, every record a pack cannot do without, a species with no habitat, another world's pack - each its own refusal\n", s_cases);
    return 0;
}

/* ================================================================ 8: uids, the pool, the ids a file's pack is given */
static int t_collision_pool(void) {
    packs_drop_loaded();
    blob_t A, B, k;
    const enc_t *gb = builtin_with_uid(species_get(SPECIES_GECKO)->uid);
    blob_variant(&A, gb->blob, gb->len, "o1wu", "O1WU");                     /* pt.gecko.leoo1wu and pt.gecko.leo1tfa: two names, one FNV-1a-32 (0ee0ff1a) */
    blob_variant(&B, gb->blob, gb->len, "1tfa", "1TFA");
    if (rd32(A.b + 8) != 0x0ee0ff1au || rd32(B.b + 8) != 0x0ee0ff1au || A.n != B.n || !memcmp(A.b + 20, B.b + 20, A.n - 20)) PF("the test's colliding names do not collide (%08x, %08x)", (unsigned)rd32(A.b + 8), (unsigned)rd32(B.b + 8));
    packs_info_t info; char why[300];
    if (offer_blob(&A, &info, why, sizeof why) != PACKS_ADDED) PF("the first of two colliding names does not load: %s", why);
    const reg_t one = reg_now();
    if (offer_blob(&A, &info, why, sizeof why) != PACKS_SAME) PF("the same file twice was not called the same");
    if (offer_blob(&B, NULL, why, sizeof why) != PACKS_REFUSED || !strstr(why, "already")) PF("two different names with one uid: the second was not refused for it (%s)", why);
    if (!reg_same(one, reg_now())) PF("a collision changed the registry");
    const species_t *a = species_by_uid(rd32(A.b + 8));
    if (a->builtin || a->id < 3 || a->id > 249 || a->id == SPECIES_GECKO) PF("a pack from a file was given id %d (built in %d): the LEGACY_ID record in its file must not count", a->id, a->builtin);
    packs_drop_loaded();
    if (species_count() != PTPK_BUILTIN_SPECIES) PF("the dropped pack is still in the registry");
    /* the pool: PACKS_SPARE slots of each kind past the built-in ones, then no room */
    int ids[PACKS_SPARE + 2];
    for (int i = 0; i < PACKS_SPARE + 1; i++) {
        char tail[4], name[4]; snprintf(tail, sizeof tail, "%02x", i + 16); snprintf(name, sizeof name, "%02X", i + 16);
        blob_variant(&k, gb->blob, gb->len, tail, name);
        const packs_result_t r = offer_blob(&k, &info, why, sizeof why);
        if (i < PACKS_SPARE) {
            if (r != PACKS_ADDED) PF("species %d of the spare %d was refused: %s", i, PACKS_SPARE, why);
            ids[i] = info.id;
            for (int j = 0; j < i; j++) if (ids[j] == ids[i]) PF("two species from files share the id %d", ids[i]);
        } else if (r != PACKS_REFUSED || !strstr(why, "no room")) PF("the species past the pool was not refused for room (%d: %s)", (int)r, why);
    }
    if (species_count() != PTPK_BUILTIN_SPECIES + PACKS_SPARE) PF("%d species held, the pool is %d + %d", species_count(), PTPK_BUILTIN_SPECIES, PACKS_SPARE);
    packs_drop_loaded();
    const enc_t *db = builtin_with_uid(habitat_get(HABITAT_DESERT)->uid);
    for (int i = 0; i < PACKS_SPARE + 1; i++) {
        char tail[4], name[4]; snprintf(tail, sizeof tail, "%02x", i + 16); snprintf(name, sizeof name, "%02X", i + 16);
        blob_variant(&k, db->blob, db->len, tail, name);
        const packs_result_t r = offer_blob(&k, &info, why, sizeof why);
        if (i < PACKS_SPARE) { if (r != PACKS_ADDED) PF("habitat %d of the spare %d was refused: %s", i, PACKS_SPARE, why); if (info.id < 3 || info.id > 30) PF("a habitat from a file was given id %d (3..30)", info.id); }
        else if (r != PACKS_REFUSED || !strstr(why, "no room")) PF("the habitat past the pool was not refused for room (%d: %s)", (int)r, why);
    }
    packs_drop_loaded();
    if (habitat_count() != PTPK_BUILTIN_HABITATS || species_count() != PTPK_BUILTIN_SPECIES) PF("the pools did not come back to the built-in packs (%d, %d)", species_count(), habitat_count());
    /* a species and the habitat it lists: the mask follows the habitat in and out */
    blob_variant(&k, db->blob, db->len, "h1", "H1");
    if (offer_blob(&k, &info, why, sizeof why) != PACKS_ADDED) PF("a habitat from a file: %s", why);
    const uint32_t huid = info.uid; const habitat_t *h1 = habitat_by_uid(huid);
    blob_variant(&A, gb->blob, gb->len, "s1", "S1");
    { uint8_t v[9] = { 2 }; wr32(v + 1, habitat_get(HABITAT_DESERT)->uid); wr32(v + 5, huid); rec_set(&A, 0x101, v, 9); blob_fix(&A); }
    if (offer_blob(&A, &info, why, sizeof why) != PACKS_ADDED) PF("a species that lists a habitat from a file: %s", why);
    const species_t *s1 = species_by_uid(info.uid);
    if (!((s1->habitats >> h1->id) & 1u) || !((s1->habitats >> HABITAT_DESERT) & 1u) || !habitat_fits(h1, s1)) PF("the species' habitats mask %08x does not hold the habitat %d it lists", (unsigned)s1->habitats, h1->id);
    packs_drop_loaded();
    printf("selftest-ptpk: two names with one uid (0ee0ff1a): the second refused, the same file twice dropped quietly; ids 3..249 / 3..30 given to files (a LEGACY_ID record is not theirs); the pool takes %d + %d species and %d + %d habitats and then says no room; the species' mask follows its habitats\n",
           PTPK_BUILTIN_SPECIES, PACKS_SPARE, PTPK_BUILTIN_HABITATS, PACKS_SPARE);
    return 0;
}

/* ================================================================ 9: a folder of packs */
static void put_file(const char *path, const void *b, size_t n) { FILE *f = fopen(path, "wb"); if (f) { if (n) fwrite(b, 1, n, f); fclose(f); } }
static int t_folder(void) {
    packs_drop_loaded();
    char dir[600]; snprintf(dir, sizeof dir, "%s", sim_tmp("packs-folder"));
    mkdir(dir, 0700);
    char path[700];
    blob_t H, S, bad, cut;
    const enc_t *sb = builtin_with_uid(s_ws->uid), *hb = builtin_with_uid(s_wh->uid);
    blob_variant(&H, hb->blob, hb->len, "f1", "F1");
    blob_variant(&S, sb->blob, sb->len, "f1", "F1");
    { uint8_t v[9] = { 2 }; wr32(v + 1, s_wh->uid); wr32(v + 5, rd32(H.b + 8)); rec_set(&S, 0x101, v, 9); blob_fix(&S); }      /* lives in the world's habitat and in the new one */
    blob_variant(&bad, sb->blob, sb->len, "f2", "F2"); bad.b[PTPK_HEADER_BYTES + 40] ^= 0x10;                             /* a bit flipped, the CRC not fixed */
    blob_variant(&cut, sb->blob, sb->len, "f3", "F3"); cut.n -= 10;                                                       /* a file cut short */
    snprintf(path, sizeof path, "%s/b-species.ptpk", dir);   put_file(path, S.b, S.n);
    snprintf(path, sizeof path, "%s/a-habitat.ptpk", dir);   put_file(path, H.b, H.n);                                    /* (named to come first: the order does not matter, the habitats are read first) */
    snprintf(path, sizeof path, "%s/c-corrupt.ptpk", dir);   put_file(path, bad.b, bad.n);
    snprintf(path, sizeof path, "%s/d-short.ptpk", dir);     put_file(path, cut.b, cut.n);
    snprintf(path, sizeof path, "%s/e-empty.ptpk", dir);     put_file(path, "", 0);
    { static uint8_t huge[5000]; memset(huge, 0xaa, sizeof huge); snprintf(path, sizeof path, "%s/f-huge.ptpk", dir); put_file(path, huge, sizeof huge); }
    snprintf(path, sizeof path, "%s/g-builtin.ptpk", dir);   put_file(path, sb->blob, sb->len);                          /* the built-in pack's own file */
    snprintf(path, sizeof path, "%s/h-notapack.ptpk", dir);  put_file(path, "hello, this is not a pack", 25);
    snprintf(path, sizeof path, "%s/ignored.txt", dir);      put_file(path, H.b, H.n);
    const reg_t before = reg_now();
    printf("selftest-ptpk: the folder (the loader's own words follow):\n");
    const int added = sim_packs_scan(dir, true);
    if (added != 2) PF("the folder added %d packs, want the new habitat and the new species", added);
    const habitat_t *h = habitat_by_uid(rd32(H.b + 8)); const species_t *s = species_by_uid(rd32(S.b + 8));
    if (!h || !s) PF("the folder's new packs are not in the registry");
    if (h->builtin || s->builtin || h->id < 3 || s->id < 3) PF("a pack from the folder carries a built-in id (%d, %d)", h->id, s->id);
    if (species_count() != before.ns + 1 || habitat_count() != before.nh + 1) PF("the folder left %d species and %d habitats (had %d and %d, and two new packs)", species_count(), habitat_count(), before.ns, before.nh);
    if (!((s->habitats >> h->id) & 1u) || !habitat_fits(h, s) || !habitat_fits(s_wh, s)) PF("the folder's species and habitat do not fit (mask %08x)", (unsigned)s->habitats);
    tank_init(&s_t, 5);
    if (!tank_set_packs(&s_t, h->id, s->id) || tank_species(&s_t) != s || tank_habitat(&s_t) != h) PF("a tank refused the pair the folder brought");
    tank_tick(&s_t, 0.1f, NULL);
    if (sim_packs_scan(dir, false) != 0 || species_count() != before.ns + 1) PF("a second scan of the folder added packs");
    packs_drop_loaded();
    tank_init(&s_t, 5);
    if (tank_species(&s_t)->id != SPECIES_DEFAULT) PF("a tank still holds a pack that was dropped");
    printf("selftest-ptpk: the folder: the new habitat and the new species loaded (ids %d and %d, available to a tank); the corrupt, the short, the empty, the huge and the foreign files refused, "
           "the built-in pack's own file dropped quietly, the .txt ignored; a second scan changes nothing\n", h->id, s->id);
    return 0;
}

/* ================================================================ 10: a tank whose pack is missing */
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
#define SAVE_BYTES 2160                    /* sizeof(save_t) today (progression.c): the packs by uid at 2104 .. 2159 */
static int t_missing(void) {
    packs_drop_loaded();
    const char *sav = sim_tmp("ptpk-missing.sav"); setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    char dir[600]; snprintf(dir, sizeof dir, "%s", sim_tmp("packs-missing")); mkdir(dir, 0700);
    setenv("POCKET_PACKS_DIR", dir, 1);
    blob_t S; char why[300]; packs_info_t info; char path[700];
    const enc_t *sb = builtin_with_uid(s_ws->uid);
    blob_variant(&S, sb->blob, sb->len, "m1", "M1");                       /* the world's own species under another name: it lives where the original does */
    const uint32_t suid = rd32(S.b + 8);
    char sname[PACK_NAME_MAX]; { size_t at = 0, l = 0; rec_find(&S, 1, &at, &l); snprintf(sname, sizeof sname, "%.*s", (int)l, (const char *)S.b + at + 4); }
    if (offer_blob(&S, &info, why, sizeof why) != PACKS_ADDED) PF("the species of the missing pack does not load: %s", why);
    const uint8_t sid = info.id;
    /* a tank on it, in its own state, saved */
    tank_init(&s_t, 24); progression_wake(&s_t, 0);
    if (!tank_set_packs(&s_t, s_wh->id, sid)) PF("a tank refused the pack from a file");
    for (int i = 0; i < s_t.n_fish; i++) {
        fish_t *f = &s_t.fish[i];
        f->hunger = 3.25f + i; f->energy = 6.5f - i; f->stress = 1.5f + i; f->curiosity = 4.75f; f->body_c = (s_ws->t_lo + s_ws->t_hi) * 0.5f - 0.75f * i;
        f->heat = species_heat_of(s_ws, f->body_c);
        if (s_ws->sheds) { f->shed = i ? SHED_SOON : SHED_SHEDDING; f->shed_s = 123.5f + i; f->shed_damp = 1.25f * i; f->shed_clean = (uint16_t)(3 + i); }
    }
    const int humid = tank_humid_hide(&s_t);
    if (humid >= 0) s_t.moist[humid] = 0.3125f;
    progression_save(&s_t);
    static uint8_t a[4096], b[4096], c[4096];
    if (file_read(a, sizeof a) != SAVE_BYTES) PF("the save is %ld bytes, want %d", file_read(a, sizeof a), SAVE_BYTES);
    if (rd32(a + 2104) != suid || rd32(a + 2108) != s_wh->uid || strncmp((const char *)a + 2112, sname, PACK_NAME_MAX) != 0 || a[1957] != 0)
        PF("the save does not carry the pack's uid %08x / name %s (uid %08x, habitat %08x, one-byte id %d)", (unsigned)suid, sname, (unsigned)rd32(a + 2104), (unsigned)rd32(a + 2108), a[1957]);
    if (a[1958] != (s_wh->builtin ? s_wh->id : 0)) PF("the habitat's one-byte id in the save is %d", a[1958]);
    /* the pack goes (a folder without it): the tank loads PAUSED, with its pair kept */
    packs_drop_loaded();
    look_t was[N_FISH_MAX], now[N_FISH_MAX];
    tank_init(&s_t, 25); progression_wake(&s_t, 0);
    if (!s_t.packs_missing) PF("a save of a pack that is not here did not leave the tank waiting for it");
    if (s_t.pack_id != SPECIES_DEFAULT || s_t.habitat_id != HABITAT_DEFAULT) PF("the stand-in for the missing pack is %d / %d, not the defaults", s_t.pack_id, s_t.habitat_id);
    if (strcmp(progression_packs_missing_name(&s_t), sname) != 0) PF("the card would name \"%s\", the pack is %s", progression_packs_missing_name(&s_t), sname);
    for (int i = 0; i < s_t.n_fish; i++) {
        const fish_t *f = &s_t.fish[i];
        if (f->hunger != 3.25f + i || f->energy != 6.5f - i || f->body_c != (s_ws->t_lo + s_ws->t_hi) * 0.5f - 0.75f * i || (s_ws->sheds && (f->shed_s != 123.5f + i || f->shed_clean != 3 + i)))
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
    if (s_t.clock != clock0 || tank_food_count(&s_t) != food0 || tank_crickets_about(&s_t) != crickets0 || s_t.player_feedings != fed0) PF("a paused tank's clock or food moved (clock %.2f -> %.2f, food %d -> %d)", clock0, s_t.clock, food0, tank_food_count(&s_t));
    if (!s_t.packs_missing) PF("a tank stopped waiting for a pack that never came");
    /* it says so: PACK MISSING, up until a tap, and again after a while */
    notice_sync(&s_t);
    for (int n = 0; n < 120 && !notice_current(); n++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current() || notice_current()->kind != NOTICE_PACK_MISSING) PF("no PACK MISSING card for a tank waiting for its pack");
    if (notice_take_cue() < 0) PF("the PACK MISSING card has no cue");
    static uint16_t fb[TANK_W * TANK_H];
    render_tank(&s_t, fb, TANK_W);
    memset(fb, 0, sizeof fb);
    render_notice(&s_t, fb, TANK_W, NOTICE_PACK_MISSING, -1, 0, 1.0f);
    long lit = 0; for (size_t i = 0; i < sizeof fb / sizeof fb[0]; i++) lit += fb[i] != 0;
    if (lit < 2000) PF("the PACK MISSING card drew %ld pixels", lit);
    for (int n = 0; n < (int)(NOTICE_UP_S * 60) + 120; n++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current()) PF("the PACK MISSING card left by itself (it waits for a tap)");
    if (!notice_dismiss() || notice_current()) {                      /* (inside its hold a tap is spent: it has been up long enough) */
        PF("a tap did not close the PACK MISSING card"); }
    for (int n = 0; n < (int)(NOTICE_PACKS_AGAIN_S * 60) + 600 && !notice_current(); n++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current() || notice_current()->kind != NOTICE_PACK_MISSING) PF("the card did not come back while the tank still waits");
    /* every save while it waits writes the pair and the tail back as they were, and a reload still waits */
    progression_save(&s_t);
    if (file_read(b, sizeof b) != SAVE_BYTES) PF("the save while the pack is missing is %ld bytes", file_read(b, sizeof b));
    if (memcmp(a + 1956, b + 1956, SAVE_BYTES - 1956) != 0) {
        int k = 1956; while (a[k] == b[k]) k++;
        PF("the pair or the tail changed while the pack was missing (byte %d: %02x -> %02x)", k, a[k], b[k]); }
    tank_init(&s_t, 26); progression_wake(&s_t, 0);
    if (!s_t.packs_missing || strcmp(progression_packs_missing_name(&s_t), sname) != 0) PF("after a save and a reload the tank no longer waits for %s", sname);
    for (int n = 0; n < 20000; n++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }        /* a heartbeat's worth (SAVE_HEARTBEAT_S 600 s), paused: it saves too */
    if (file_read(c, sizeof c) != SAVE_BYTES || memcmp(a + 1956, c + 1956, SAVE_BYTES - 1956) != 0) PF("the heartbeat's save of a tank waiting for its pack changed the pair or the tail");
    /* the pack comes: the folder is read again while the tank waits, the next tick resumes it */
    snprintf(path, sizeof path, "%s/m1.ptpk", dir); put_file(path, S.b, S.n);
    sim_packs_poll(true, 0.5f);
    if (s_t.packs_missing == false) PF("resumed before the pack was found");
    sim_packs_poll(true, 2.0f);                                       /* (the scan comes every 2 s while it waits) */
    if (species_by_uid(suid) == NULL) PF("the folder poll did not find the pack");
    progression_tick(&s_t, 1.2f);
    if (s_t.packs_missing || tank_species(&s_t)->uid != suid || tank_habitat(&s_t)->uid != s_wh->uid) PF("the tank did not resume on its pack (missing %d, species %08x)", s_t.packs_missing, (unsigned)tank_species(&s_t)->uid);
    for (int i = 0; i < s_t.n_fish; i++) {
        const fish_t *f = &s_t.fish[i];
        if (f->hunger != 3.25f + i || f->heat != species_heat_of(tank_species(&s_t), f->body_c) || (s_ws->sheds && (f->shed_s != 123.5f + i || f->shed_clean != 3 + i)))
            PF("animal %d resumed as something else (hunger %.2f, heat %.2f)", i, f->hunger, f->heat);
    }
    if (humid >= 0 && s_t.moist[humid] != 0.3125f) PF("the humid hide's moisture %.4f did not come back", s_t.moist[humid]);
    notice_tick(&s_t, 1.0f / 60, false);
    if (notice_current() || notice_pending()) PF("the PACK MISSING card stayed after the pack came");
    const double age0 = progression_age_s(&s_t, 0); const double clock1 = s_t.clock;
    for (int n = 0; n < 900; n++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (!(s_t.clock > clock1 + 29) || !(progression_age_s(&s_t, 0) > age0 + 29)) PF("a resumed tank does not live (clock %.1f -> %.1f, age %.1f -> %.1f)", clock1, s_t.clock, age0, progression_age_s(&s_t, 0));
    progression_save(&s_t);
    if (file_read(b, sizeof b) != SAVE_BYTES || rd32(b + 2104) != suid || rd32(b + 2108) != s_wh->uid) PF("the resumed tank saved another pair than the pack it waited for");
    /* a reload with the pack there is the ordinary load; and a pack that never comes is reset away */
    tank_init(&s_t, 27); progression_wake(&s_t, 0);
    if (s_t.packs_missing || tank_species(&s_t)->uid != suid) PF("a reload with the pack present did not hold it");
    packs_drop_loaded();
    tank_init(&s_t, 28); progression_wake(&s_t, 0);
    if (!s_t.packs_missing) PF("the pack was dropped again and the tank did not wait");
    progression_reset(&s_t, 29);
    if (s_t.packs_missing || progression_packs_missing_name(&s_t)[0] || s_t.pack_id != SPECIES_DEFAULT) PF("a reset left the tank waiting for its old pack");
    tank_tick(&s_t, 1.0f / 30, NULL);
    if (!(s_t.clock > 0)) PF("a reset tank does not tick");
    unsetenv("POCKET_PACKS_DIR"); setenv("POCKET_PACKS_DIR", sim_tmp("packs"), 1);
    printf("selftest-ptpk: a tank whose pack (%s, uid %08x) is not loaded: it waits - the defaults drawn, animals / clock / food unmoved over 133 s of frames, a night and the hand; PACK MISSING %s up until a tap and "
           "again after %.0f s; its uid, name and per-animal tail written back unchanged by every save and reload; it resumes when the folder gives the pack (the card goes, the animals as saved, living again); a reset ends the wait\n",
           sname, (unsigned)suid, sname, NOTICE_PACKS_AGAIN_S);
    return 0;
}

/* ================================================================ */
int selftest_ptpk(void) {
    packs_drop_loaded();                                           /* (what an earlier test left: nothing) */
    s_ws = species_get(SPECIES_DEFAULT); s_wh = habitat_get(HABITAT_DEFAULT);
    if (!s_ws || !s_wh) PF("the world's default pack is not loaded");
    if (enc_all() || t_known() || t_builtin() || t_roundtrip() || t_fish_numbers() || t_flips() || t_body_fuzz() || t_records() || t_collision_pool() || t_folder() || t_missing()) return 1;
    printf("selftest-ptpk OK\n");
    return 0;
}
