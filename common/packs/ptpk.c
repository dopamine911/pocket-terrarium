/* ptpk.c - the PTPK loader (ptpk.h; docs/ptpk.md is the byte-exact contract).
 *
 * Every refusal here is a pack that is not what its file claims; the rules about what a species, a habitat or a manifest may be are
 * the engine's own (species_check, habitat_check, manifest_check) and run after, in the registry's commit. A reader walks the body once:
 * each record is bounds-checked against the body before it is read, a known record is parsed by a reader that cannot run
 * past the record's own bytes (rd_t), and nothing is trusted to be finite, in range or printable until it has been seen to be.
 * The writer it mirrors is ptpk_write.c (the encoder, the hashes, the text rules); the built-in packs' table is builtin.c. */
#include "ptpk.h"
#include "ptpk_write.h"
#include "ptpk_tags.h"
#include "builtin.h"
#include "caps.h"
#include <stdio.h>
#include <string.h>

static uint32_t le(const uint8_t *p, int bytes) {
    uint32_t v = 0;
    for (int k = 0; k < bytes; k++) v |= (uint32_t)p[k] << (8 * k);
    return v;
}

#define BAD(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); return false; } while (0)

bool ptpk_open(const uint8_t *b, size_t n, ptpk_header_t *h, char *why, size_t why_n) {
    if (!b || n < PTPK_HEADER_BYTES) BAD("%u bytes: no room for the 20-byte header", (unsigned)n);
    if (memcmp(b, "PTPK", 4) != 0) BAD("not a PTPK file (no magic)");
    const unsigned fmt = b[4], kind = b[5], flags = le(b + 6, 2);
    if (fmt == 0) BAD("fmt 0");
    if (fmt > PTPK_FMT) BAD("fmt %u is newer than this loader's %d", fmt, PTPK_FMT);
    if (kind != PTPK_KIND_SPECIES && kind != PTPK_KIND_HABITAT && kind != PTPK_KIND_MODEL) BAD("kind %u is no pack kind", kind);
    if (flags) BAD("flags %04x: fmt 1 defines none", flags);
    const uint32_t uid = le(b + 8, 4), len = le(b + 12, 4), crc = le(b + 16, 4);
    if (!uid) BAD("uid 0");
    if (len == 0 || len > PTPK_BODY_MAX) BAD("a body of %u bytes (1..%d allowed)", (unsigned)len, PTPK_BODY_MAX);
    if (n != PTPK_HEADER_BYTES + (size_t)len)
        BAD("%u bytes, but the header's length makes the file %u (%s)", (unsigned)n, (unsigned)(PTPK_HEADER_BYTES + len),
            n < PTPK_HEADER_BYTES + (size_t)len ? "cut short" : "bytes past the end");
    const uint32_t got = ptpk_crc32(b + PTPK_HEADER_BYTES, len);
    if (got != crc) BAD("crc %08x, the body's is %08x", (unsigned)crc, (unsigned)got);
    h->fmt = (uint8_t)fmt; h->kind = (uint8_t)kind; h->flags = (uint16_t)flags;
    h->uid = uid; h->len = len; h->crc = crc; h->body = b + PTPK_HEADER_BYTES;
    return true;
}

/* ---- a reader that cannot leave its record ---- */
enum { E_OK, E_SHORT, E_FLOAT, E_TEXT, E_RANGE, E_GOAL, E_TWICE };
static const char *const E_WORD[] = { "", "is shorter than its layout", "holds a NaN or an infinity",
                                      "holds a text that is not printable ASCII of the right length", "holds a count or an enum out of range",
                                      "holds a goal's word that is not 1..15 of [a-z0-9_], a letter first", "lists a goal twice" };
typedef struct { const uint8_t *p; size_t n, i; int err; } rd_t;

static uint32_t rd_u(rd_t *r, int bytes) {
    if (r->i + (size_t)bytes > r->n) { if (!r->err) r->err = E_SHORT; return 0; }
    const uint32_t v = le(r->p + r->i, bytes);
    r->i += (size_t)bytes;
    return v;
}
static uint32_t rd_range(rd_t *r, int bytes, uint32_t lo, uint32_t hi) {   /* an unsigned in lo..hi, else E_RANGE */
    const uint32_t v = rd_u(r, bytes);
    if (!r->err && (v < lo || v > hi)) r->err = E_RANGE;
    return v;
}
static float rd_f(rd_t *r) {
    const uint32_t u = rd_u(r, 4);
    if ((u & 0x7f800000u) == 0x7f800000u) { if (!r->err) r->err = E_FLOAT; return 0; }   /* (tested as bits: -ffast-math may fold isfinite) */
    float f; memcpy(&f, &u, 4);
    return f;
}
static void fa(rd_t *r, float *dst, int n) { for (int k = 0; k < n; k++) dst[k] = rd_f(r); }
static bool all_zero(const float *f, int n) {                  /* every float all bits clear (as the encoder's: a -0 is a number) */
    for (int k = 0; k < n; k++) { uint32_t u; memcpy(&u, &f[k], 4); if (u) return false; }
    return true;
}
static void rd_str(rd_t *r, char *dst, size_t cap) {             /* a `str`: u8 n, then n bytes of text; into dst (cap with its NUL) */
    const uint32_t n = rd_u(r, 1);
    if (r->err) return;
    if (r->i + n > r->n) { r->err = E_SHORT; return; }
    if (!ptpk_text_ok(r->p + r->i, n, cap - 1)) { r->err = E_TEXT; return; }
    memcpy(dst, r->p + r->i, n); dst[n] = 0;
    r->i += n;
}
/* the GOALS / MODEL_GOALS value: u8 n (1..8), then n words, each of 1..15 [a-z0-9_] (a letter first), none twice. *n_out and the
   words are the caller's; a word is not matched against any list here (species_check and the manifests decide what it means) */
static void rd_goals(rd_t *r, uint8_t *n_out, char (*goal)[GOAL_NAME_MAX]) {
    const uint32_t n = rd_range(r, 1, 1, SPECIES_GOALS_MAX);
    for (uint32_t k = 0; k < n && !r->err; k++) {
        const uint32_t l = rd_u(r, 1);
        if (r->err) break;
        if (r->i + l > r->n) { r->err = E_SHORT; break; }
        if (!ptpk_goal_ok(r->p + r->i, l)) { r->err = E_GOAL; break; }
        memcpy(goal[k], r->p + r->i, l); goal[k][l] = 0;
        for (uint32_t m = 0; m < k; m++) if (strcmp(goal[m], goal[k]) == 0) { r->err = E_TWICE; break; }   /* a word twice */
        r->i += l;
    }
    *n_out = (uint8_t)n;
}

/* the records seen so far, one bit each: the common ones, then those of the kind of pack being read (the kinds' bits overlap: a file has one kind) */
enum {
    S_NAME = 1u << 0, S_UID_NS = 1u << 1, S_LEGACY = 1u << 2, S_TEST = 1u << 3, S_WORLDS = 1u << 4,
    S_HABITATS = 1u << 5, S_ACTIVITY = 1u << 6, S_HEAT = 1u << 7, S_DRIVES = 1u << 8, S_LIFE = 1u << 9, S_SHED = 1u << 10, S_DIET = 1u << 11,
    S_BODY = 1u << 12, S_MORPHS = 1u << 13, S_PATTERNS = 1u << 14, S_GOALS = 1u << 15, S_LOCO = 1u << 16, S_MEDIA = 1u << 17,
    S_CLIMATE = 1u << 5, S_BASK = 1u << 6, S_HIDES = 1u << 7, S_ROCKS = 1u << 8, S_RAMPS = 1u << 9, S_MOISTURE = 1u << 10, S_DISH = 1u << 11,
    S_AREAS = 1u << 12, S_PROVIDES = 1u << 13,
    S_MODEL_TAG = 1u << 5, S_MODEL_GOALS = 1u << 6
};

/* the record's reader met no error and took every byte of it */
#define DONE(name) do { if (r.err) BAD("%s %s", name, E_WORD[r.err]); if (r.i != r.n) BAD("%s is %u bytes, its layout is %u", name, (unsigned)r.n, (unsigned)r.i); } while (0)
#define ONCE(bit, name) do { if (seen & (bit)) BAD("%s twice", name); seen |= (bit); } while (0)

/* the records a species, a habitat and a manifest share (the reader is the record's: a copy). 1: it was one and is read; 0: not one
   of them (for this kind of pack); -1: refused (why). A manifest has a name and a uid and nothing else of them: the rest are
   not its records (REQUIRED: refused, else skipped) */
typedef struct { uint32_t ns_uid; uint8_t worlds, legacy; bool test_only; } common_t;
/* (BAD here answers -1, "refused": the file-wide BAD answers false, which this function's caller would read as "not one of the common records" and go on to
   skip an optional one - a duplicate or a bad value let through; the fuzz in --selftest-ptpk found it) */
#undef BAD
#define BAD(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); return -1; } while (0)
static int common_record(unsigned id, rd_t r, int kind, uint32_t *seen_, common_t *c, char *name, char *why, size_t why_n) {
    uint32_t seen = *seen_;
    if (kind == PTPK_KIND_MODEL && id != T_NAME && id != T_UID_NS) return 0;
    switch (id) {
    case T_NAME:
        ONCE(S_NAME, "NAME");
        if (!ptpk_text_ok(r.p, r.n, PACK_NAME_MAX - 1)) BAD("NAME is not 1..%d printable characters", PACK_NAME_MAX - 1);
        memcpy(name, r.p, r.n); name[r.n] = 0;
        break;
    case T_UID_NS:
        ONCE(S_UID_NS, "UID_NS");
        if (!ptpk_ns_ok(r.p, r.n)) BAD("UID_NS is not 3..47 characters of [a-z0-9._-]");
        c->ns_uid = ptpk_fnv1a(r.p, r.n);
        break;
    case T_WORLDS:
        ONCE(S_WORLDS, "WORLDS");
        c->worlds = (uint8_t)rd_range(&r, 1, 1, 255);
        DONE("WORLDS");
        break;
    case T_LEGACY_ID:
        ONCE(S_LEGACY, "LEGACY_ID");
        c->legacy = (uint8_t)rd_range(&r, 1, 1, kind == PTPK_KIND_HABITAT ? 31 : 255);
        DONE("LEGACY_ID");
        break;
    case T_TEST_ONLY:
        ONCE(S_TEST, "TEST_ONLY");
        rd_range(&r, 1, 1, 1);
        DONE("TEST_ONLY");
        c->test_only = true;
        break;
    default:
        return 0;
    }
    *seen_ = seen;
    return 1;
}
#undef BAD
#define BAD(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); return false; } while (0)

/* the next record of the body: its tag, and a reader on its value */
#define NEXT_RECORD() \
    if (h->len - off < 4) BAD("a truncated record header at byte %u", (unsigned)off); \
    const unsigned tag = le(h->body + off, 2), rlen = le(h->body + off + 2, 2); off += 4; \
    if (rlen > h->len - off) BAD("record %04x runs %u bytes past the end of the body", tag & 0x7fffu, (unsigned)(rlen - (h->len - off))); \
    rd_t r = { h->body + off, rlen, 0, E_OK }; off += rlen; \
    const unsigned id = tag & 0x7fffu

/* the records every pack of a kind cannot do without are all there */
typedef struct { uint32_t bit; const char *name; } must_t;
static bool have_all(uint32_t seen, const must_t *must, size_t n, char *why, size_t why_n) {
    for (size_t k = 0; k < n; k++) if (!(seen & must[k].bit)) BAD("no %s record", must[k].name);
    return true;
}

bool ptpk_read_species(const ptpk_header_t *h, species_t *p, uint8_t *legacy_id, char *why, size_t why_n) {
    memset(p, 0, sizeof *p);
    *legacy_id = 0;
    if (h->kind != PTPK_KIND_SPECIES) BAD("kind %u is not a species", h->kind);
    uint32_t seen = 0;
    common_t c = { 0, 0, 0, false };
    for (size_t off = 0; off < h->len; ) {
        NEXT_RECORD();
        const int k = common_record(id, r, PTPK_KIND_SPECIES, &seen, &c, p->name, why, why_n);
        if (k < 0) return false;
        if (k > 0) continue;
        switch (id) {
        case T_HABITATS: {
            ONCE(S_HABITATS, "HABITATS");
            const uint32_t n = rd_range(&r, 1, 1, SPECIES_HABITATS_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                p->habitat_uid[j] = rd_u(&r, 4);
                if (!r.err && !p->habitat_uid[j]) r.err = E_RANGE;
                for (uint32_t m = 0; m < j && !r.err; m++) if (p->habitat_uid[m] == p->habitat_uid[j]) r.err = E_RANGE;
            }
            DONE("HABITATS");
            p->n_habitat_uids = (uint8_t)n;
            break; }
        case T_ACTIVITY:
            ONCE(S_ACTIVITY, "ACTIVITY");
            p->schedule = (sched_t)rd_range(&r, 1, 0, 2); p->settle_s = rd_f(&r);
            DONE("ACTIVITY");
            break;
        case T_HEAT:
            ONCE(S_HEAT, "HEAT");
            p->t_cold = rd_f(&r); p->t_lo = rd_f(&r); p->t_hi = rd_f(&r); p->t_hot = rd_f(&r); fa(&r, p->tau_s, 4);
            DONE("HEAT");
            break;
        case T_DRIVES:
            ONCE(S_DRIVES, "DRIVES");
            p->hunger_per_s = rd_f(&r); p->energy_per_s = rd_f(&r); p->energy_rest_per_s = rd_f(&r); p->curiosity_per_s = rd_f(&r);
            p->hunger_per_cricket = rd_f(&r);
            p->sleep_hunger_per_h = rd_f(&r); p->sleep_energy_per_h = rd_f(&r); p->sleep_stress_per_h = rd_f(&r);
            DONE("DRIVES");
            break;
        case T_LIFE:
            ONCE(S_LIFE, "LIFE");
            fa(&r, p->stage_s, 3); p->pop_cap = (uint8_t)rd_range(&r, 1, 0, 255);
            DONE("LIFE");
            break;
        case T_SHED:
            ONCE(S_SHED, "SHED");
            p->sheds = rd_range(&r, 1, 0, 1) != 0; p->shed_style = (shed_style_t)rd_range(&r, 1, 0, 1);
            fa(&r, p->shed_every_s, 4); p->shed_soon_s = rd_f(&r); p->shed_window_s = rd_f(&r);
            DONE("SHED");
            break;
        case T_DIET:
            ONCE(S_DIET, "DIET");
            p->diet = (uint8_t)rd_range(&r, 1, 0, DIET_CRICKET | DIET_MEALWORM | DIET_GREENS | DIET_PELLET);
            DONE("DIET");
            break;
        case T_BODY:
            ONCE(S_BODY, "BODY");
            p->plan = (body_plan_t)rd_range(&r, 1, 0, 1);
            p->geom.length_px = rd_f(&r); p->geom.head_w = rd_f(&r); p->geom.limb_len = rd_f(&r); p->geom.tail_fat = rd_f(&r); p->geom.eye_r = rd_f(&r);
            p->geom.eyelids = rd_range(&r, 1, 0, 1) != 0;
            DONE("BODY");
            break;
        case T_MORPHS: {
            ONCE(S_MORPHS, "MORPHS");
            const uint32_t n = rd_range(&r, 1, 1, SPECIES_MORPHS_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                rd_str(&r, p->morphs[j].name, MORPH_NAME_MAX);
                p->morphs[j].body = rd_range(&r, 4, 0, 0xffffff); p->morphs[j].accent = rd_range(&r, 4, 0, 0xffffff);
            }
            DONE("MORPHS");
            p->n_morphs = (uint8_t)n;
            break; }
        case T_PATTERNS:
            ONCE(S_PATTERNS, "PATTERNS");
            rd_range(&r, 1, 4, 4);
            for (int s = 0; s < 4 && !r.err; s++) rd_str(&r, p->pattern[s].name, PATTERN_NAME_MAX);
            DONE("PATTERNS");
            break;
        case T_GOALS:
            ONCE(S_GOALS, "GOALS");
            rd_goals(&r, &p->n_goals, p->goal);
            DONE("GOALS");
            break;
        case T_LOCOMOTION: {
            ONCE(S_LOCO, "LOCOMOTION");
            const uint32_t n = rd_range(&r, 1, 1, SPECIES_LOCO_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                const uint32_t mi = rd_u(&r, 1), ai = rd_u(&r, 1);
                float num[9];                                    /* the pace by stage (4), the bout (2), the pause (2), the turn */
                fa(&r, num, 9);
                if (r.err) break;
                const caps_t m = mi < 32 ? 1u << mi : 0, a = ai < 32 ? 1u << ai : 0;
                if (!(m & CAPS_KNOWN)) BAD("LOCOMOTION moves by capability bit %u, which this firmware does not know", (unsigned)mi);
                if (!(m & CAPS_MOVES)) BAD("LOCOMOTION moves by %s, which is not a movement mode", caps_name(mi));
                if (!(a & CAPS_KNOWN)) BAD("LOCOMOTION moves in capability bit %u, which this firmware does not know", (unsigned)ai);
                if (!(a & CAPS_AREAS)) BAD("LOCOMOTION moves in %s, which is not an area", caps_name(ai));
                for (uint32_t q = 0; q < j; q++) if (p->loco[q].mode == m) BAD("LOCOMOTION lists %s twice", caps_name(mi));
                if (m == CAP_WALK) { memcpy(p->walk_px_s, num, sizeof p->walk_px_s); p->bout_s[0] = num[4]; p->bout_s[1] = num[5]; p->pause_s[0] = num[6]; p->pause_s[1] = num[7]; p->turn_s = num[8]; }
                else if (!all_zero(num, 9)) BAD("LOCOMOTION: the %s entry carries numbers, and only a walk's are read yet", caps_name(mi));
                p->loco[j].mode = m; p->loco[j].area = a;
            }
            DONE("LOCOMOTION");
            p->n_loco = (uint8_t)n;
            break; }
        case T_MEDIA_NEEDS: {
            ONCE(S_MEDIA, "MEDIA_NEEDS");
            const caps_t needs = rd_u(&r, 4), wants = rd_u(&r, 4);
            DONE("MEDIA_NEEDS");
            const int odd = caps_unknown(needs);                 /* a need this firmware cannot name is one it can never meet */
            if (odd >= 0) BAD("MEDIA_NEEDS needs capability bit %d, which this firmware does not know", odd);
            if (needs & ~CAPS_MEDIA) {
                char l[80]; caps_list(needs & ~CAPS_MEDIA, l, sizeof l);
                BAD("MEDIA_NEEDS lists %s: movement belongs in LOCOMOTION and foods in DIET", l);
            }
            p->media_needs = needs;
            p->media_wants = wants & CAPS_KNOWN;                 /* what it only makes use of: a bit this firmware does not know is skipped */
            break; }
        default:
            if (tag & 0x8000u) BAD("record %04x is REQUIRED and this loader does not know it", id);
            break;                                      /* an optional one it does not know: skipped */
        }
    }
    static const must_t MUST[] = {
        { S_NAME, "NAME" }, { S_UID_NS, "UID_NS" }, { S_HABITATS, "HABITATS" },
        { S_ACTIVITY, "ACTIVITY" }, { S_HEAT, "HEAT" }, { S_DRIVES, "DRIVES" }, { S_LIFE, "LIFE" },
        { S_DIET, "DIET" }, { S_BODY, "BODY" }, { S_GOALS, "GOALS" }, { S_LOCO, "LOCOMOTION" }, { S_MEDIA, "MEDIA_NEEDS" } };
    if (!have_all(seen, MUST, sizeof MUST / sizeof MUST[0], why, why_n)) return false;
    p->worlds = c.worlds;
    p->test_only = c.test_only;
    *legacy_id = c.legacy;
    if (c.ns_uid != h->uid) BAD("the header's uid %08x is not the name's (%08x)", (unsigned)h->uid, (unsigned)c.ns_uid);
    p->uid = h->uid;
    return true;
}

bool ptpk_read_habitat(const ptpk_header_t *h, habitat_t *p, uint8_t *legacy_id, char *why, size_t why_n) {
    memset(p, 0, sizeof *p);
    *legacy_id = 0;
    if (h->kind != PTPK_KIND_HABITAT) BAD("kind %u is not a habitat", h->kind);
    uint32_t seen = 0;
    common_t c = { 0, 0, 0, false };
    for (size_t off = 0; off < h->len; ) {
        NEXT_RECORD();
        const int k = common_record(id, r, PTPK_KIND_HABITAT, &seen, &c, p->name, why, why_n);
        if (k < 0) return false;
        if (k > 0) continue;
        switch (id) {
        case T_CLIMATE:
            ONCE(S_CLIMATE, "CLIMATE");
            p->floor_warm_c = rd_f(&r); p->floor_cool_c = rd_f(&r); p->night_drop_c = rd_f(&r); p->rise_c = rd_f(&r);
            DONE("CLIMATE");
            break;
        case T_BASK:
            ONCE(S_BASK, "BASK");
            p->bask_surf = (uint8_t)rd_range(&r, 1, 0, 255); p->bask_u = rd_f(&r); p->bask_c = rd_f(&r); p->bask_r_px = rd_f(&r);
            DONE("BASK");
            break;
        case T_HIDES: {
            ONCE(S_HIDES, "HIDES");
            const uint32_t n = rd_range(&r, 1, 0, HABITAT_HIDES_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                p->hides[j].kind = (hide_kind_t)rd_range(&r, 1, 0, HIDE_KINDS - 1);
                p->hides[j].u = rd_f(&r); p->hides[j].t_lit_c = rd_f(&r); p->hides[j].t_dark_c = rd_f(&r);
            }
            DONE("HIDES");
            p->n_hides = (uint8_t)n;
            break; }
        case T_ROCKS: {
            ONCE(S_ROCKS, "ROCKS");
            const uint32_t n = rd_range(&r, 1, 0, HABITAT_ROCKS_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                rock_t *rk = &p->rocks[j];
                rk->n = (uint8_t)rd_range(&r, 1, 2, ROCK_PTS_MAX);
                rk->shelf = rd_range(&r, 1, 0, 1) != 0;
                for (int i = 0; i < rk->n && !r.err; i++) { rk->u[i] = rd_f(&r); rk->v[i] = rd_f(&r); }
            }
            DONE("ROCKS");
            p->n_rocks = (uint8_t)n;
            break; }
        case T_RAMPS: {
            ONCE(S_RAMPS, "RAMPS");
            const uint32_t n = rd_range(&r, 1, 0, HABITAT_RAMPS_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                p->ramps[j].lo = (uint8_t)rd_range(&r, 1, 0, 255); p->ramps[j].hi = (uint8_t)rd_range(&r, 1, 0, 255);
                p->ramps[j].u_lo = rd_f(&r); p->ramps[j].u_hi = rd_f(&r);
            }
            DONE("RAMPS");
            p->n_ramps = (uint8_t)n;
            break; }
        case T_MOISTURE:
            ONCE(S_MOISTURE, "MOISTURE");
            p->moist_start = rd_f(&r); p->moist_dry_per_s = rd_f(&r); p->humidity = rd_f(&r);
            DONE("MOISTURE");
            break;
        case T_DISH:
            ONCE(S_DISH, "DISH");
            p->dish_u = rd_f(&r);
            DONE("DISH");
            p->dish = true;
            break;
        case T_AREAS: {
            ONCE(S_AREAS, "AREAS");
            const caps_t a = rd_u(&r, 4);
            DONE("AREAS");
            const caps_t known = a & CAPS_KNOWN;                 /* what a habitat offers that this firmware cannot name is of no use to it: skipped */
            if (known & ~CAPS_AREAS) { char l[80]; caps_list(known & ~CAPS_AREAS, l, sizeof l); BAD("AREAS lists %s, which is not an area", l); }
            p->areas = known;
            break; }
        case T_PROVIDES: {
            ONCE(S_PROVIDES, "PROVIDES");
            const caps_t v = rd_u(&r, 4);
            DONE("PROVIDES");
            const caps_t known = v & CAPS_KNOWN;
            if (known & ~CAPS_PROVIDED) { char l[80]; caps_list(known & ~CAPS_PROVIDED, l, sizeof l); BAD("PROVIDES lists %s, which is not a lamp, hides or a food", l); }
            p->provides = known;
            break; }
        default:
            if (tag & 0x8000u) BAD("record %04x is REQUIRED and this loader does not know it", id);
            break;                                      /* an optional one it does not know: skipped */
        }
    }
    static const must_t MUST[] = {
        { S_NAME, "NAME" }, { S_UID_NS, "UID_NS" },
        { S_CLIMATE, "CLIMATE" }, { S_BASK, "BASK" }, { S_MOISTURE, "MOISTURE" }, { S_AREAS, "AREAS" }, { S_PROVIDES, "PROVIDES" } };
    if (!have_all(seen, MUST, sizeof MUST / sizeof MUST[0], why, why_n)) return false;
    p->worlds = c.worlds;
    p->test_only = c.test_only;
    *legacy_id = c.legacy;
    if (c.ns_uid != h->uid) BAD("the header's uid %08x is not the name's (%08x)", (unsigned)h->uid, (unsigned)c.ns_uid);
    p->uid = h->uid;
    return true;
}

bool ptpk_read_manifest(const ptpk_header_t *h, model_manifest_t *m, char *why, size_t why_n) {
    memset(m, 0, sizeof *m);
    if (h->kind != PTPK_KIND_MODEL) BAD("kind %u is not a model manifest", h->kind);
    uint32_t seen = 0;
    common_t c = { 0, 0, 0, false };
    for (size_t off = 0; off < h->len; ) {
        NEXT_RECORD();
        const int k = common_record(id, r, PTPK_KIND_MODEL, &seen, &c, m->name, why, why_n);
        if (k < 0) return false;
        if (k > 0) continue;
        switch (id) {
        case T_MODEL_TAG:
            ONCE(S_MODEL_TAG, "MODEL_TAG");
            if (!ptpk_tag_ok(r.p, r.n)) BAD("MODEL_TAG is not 1..%d characters of [A-Za-z0-9._-]", MODEL_TAG_MAX - 1);
            memcpy(m->tag, r.p, r.n); m->tag[r.n] = 0;
            break;
        case T_MODEL_GOALS:
            ONCE(S_MODEL_GOALS, "MODEL_GOALS");
            rd_goals(&r, &m->n_goals, m->goal);
            DONE("MODEL_GOALS");
            break;
        default:
            if (tag & 0x8000u) BAD("record %04x is REQUIRED and this loader does not know it", id);
            break;
        }
    }
    static const must_t MUST[] = {
        { S_NAME, "NAME" }, { S_UID_NS, "UID_NS" }, { S_MODEL_TAG, "MODEL_TAG" }, { S_MODEL_GOALS, "MODEL_GOALS" } };
    if (!have_all(seen, MUST, sizeof MUST / sizeof MUST[0], why, why_n)) return false;
    if (c.ns_uid != h->uid) BAD("the header's uid %08x is not the name's (%08x)", (unsigned)h->uid, (unsigned)c.ns_uid);
    m->uid = h->uid;
    return true;
}

/* ---- the registry's entry ---- */
static void say(packs_info_t *info, int kind, uint32_t uid, uint8_t id, const char *name) {
    if (!info) return;
    info->kind = kind; info->uid = uid; info->id = id;
    snprintf(info->name, sizeof info->name, "%s", name);
}

/* a refusal here is PACKS_REFUSED: BAD above returns false, which as a packs_result_t is PACKS_ADDED (the fuzz in --selftest-ptpk found it) */
#define NO(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); return PACKS_REFUSED; } while (0)
packs_result_t packs_add(const uint8_t *blob, size_t n, packs_origin_t origin, packs_info_t *info, char *why, size_t why_n) {
    packs_init();
    ptpk_header_t h;
    if (!ptpk_open(blob, n, &h, why, why_n)) return PACKS_REFUSED;
    const bool builtin = origin == PACKS_BUILTIN;
    const char *by = "";
    uint8_t legacy = 0;
    if (h.kind == PTPK_KIND_MODEL) {
        switch (manifest_seen(h.uid, h.crc, &by)) {
        case PACK_SAME: { const model_manifest_t *m = manifest_by_uid(h.uid); say(info, h.kind, m->uid, 0, m->name); return PACKS_SAME; }
        case PACK_UID_TAKEN: NO("uid %08x is %s's already (this is another file)", (unsigned)h.uid, by);
        default: break;
        }
        model_manifest_t *m = manifest_slot();
        if (!m) NO("no room: the manifest pool is full");
        if (!ptpk_read_manifest(&h, m, why, why_n)) return PACKS_REFUSED;
        if (!manifest_commit(m, builtin, h.crc, why, why_n)) return PACKS_REFUSED;
        say(info, h.kind, m->uid, 0, m->name);
        return PACKS_ADDED;
    }
    if (h.kind == PTPK_KIND_SPECIES) {
        switch (species_seen(h.uid, h.crc, &by)) {
        case PACK_SAME: { const species_t *s = species_by_uid(h.uid); say(info, h.kind, s->uid, s->id, s->name); return PACKS_SAME; }
        case PACK_UID_TAKEN: NO("uid %08x is %s's already (this is another file)", (unsigned)h.uid, by);
        default: break;
        }
        species_t *p = species_slot();
        if (!p) NO("no room: the species pool is full");
        if (!ptpk_read_species(&h, p, &legacy, why, why_n)) return PACKS_REFUSED;
        if (!species_commit(p, builtin, builtin ? legacy : 0, h.crc, why, why_n)) return PACKS_REFUSED;
        say(info, h.kind, p->uid, p->id, p->name);
        return PACKS_ADDED;
    }
    switch (habitat_seen(h.uid, h.crc, &by)) {
    case PACK_SAME: { const habitat_t *q = habitat_by_uid(h.uid); say(info, h.kind, q->uid, q->id, q->name); return PACKS_SAME; }
    case PACK_UID_TAKEN: NO("uid %08x is %s's already (this is another file)", (unsigned)h.uid, by);
    default: break;
    }
    habitat_t *p = habitat_slot();
    if (!p) NO("no room: the habitat pool is full");
    if (!ptpk_read_habitat(&h, p, &legacy, why, why_n)) return PACKS_REFUSED;
    if (!habitat_commit(p, builtin, builtin ? legacy : 0, h.crc, why, why_n)) return PACKS_REFUSED;
    say(info, h.kind, p->uid, p->id, p->name);
    return PACKS_ADDED;
}

bool g_packs_ready;
/* the built-in packs through the one entry (docs/ptpk.md "The loader"): each is encoded from its const definition (builtin.c) into
   a buffer on this stack - the firmware's main task, packs_init being the first thing app_main does, so no small task ever
   comes here first - and offered to packs_add as the bytes of a file, exactly as a folder's or the card's would be: the same
   validation, species_check / habitat_check / manifest_check, uid dedupe, and the LEGACY_ID kept for these alone (PACKS_BUILTIN) */
void packs_load_builtin(void) {
    g_packs_ready = true;                 /* first: the registry's own calls come back to packs_init while this runs */
    uint8_t file[PTPK_FILE_MAX];
    char why[160];
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {    /* the manifests and the habitats first: a species lists habitats and names goals they list */
        const builtin_pack_t *b = &PTPK_BUILTIN[i];
        const char *bad = "";
        const size_t n = builtin_encode(b, file, sizeof file, &bad);
        if (!n) { fprintf(stderr, "packs: the built-in pack %s cannot be encoded: %s\n", b->uid_ns, bad); continue; }   /* (a build error: --selftest-ptpk fails on it) */
        if (packs_add(file, n, PACKS_BUILTIN, NULL, why, sizeof why) == PACKS_REFUSED)
            fprintf(stderr, "packs: the built-in pack %s was refused: %s\n", b->uid_ns, why);
    }
}

void packs_drop_loaded(void) {
    packs_init();
    habitat_drop_loaded();
    species_drop_loaded();
    manifest_drop_loaded();
}
