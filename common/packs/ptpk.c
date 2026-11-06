/* ptpk.c - the PTPK loader (ptpk.h; docs/ptpk.md is the byte-exact contract).
 *
 * Every refusal here is a pack that is not what its file claims; the rules about what a species or a habitat may be are
 * the engine's own (species_check, habitat_check) and run after, in the registry's commit. A reader walks the body once:
 * each record is bounds-checked against the body before it is read, a known record is parsed by a reader that cannot run
 * past the record's own bytes (rd_t), and nothing is trusted to be finite, in range or printable until it has been seen to be.
 * The writer it mirrors is ptpk_write.c (the encoder, the hashes, the text rules); the built-in packs' table is builtin.c. */
#include "ptpk.h"
#include "ptpk_write.h"
#include "ptpk_tags.h"
#include "builtin.h"
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
    if (kind == PTPK_KIND_MODEL) BAD("a model manifest: not loaded in this firmware");
    if (kind != PTPK_KIND_SPECIES && kind != PTPK_KIND_HABITAT) BAD("kind %u is no pack kind", kind);
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
enum { E_OK, E_SHORT, E_FLOAT, E_TEXT, E_RANGE };
static const char *const E_WORD[] = { "", "is shorter than its layout", "holds a NaN or an infinity",
                                      "holds a text that is not printable ASCII of the right length", "holds a count or an enum out of range" };
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
static void rd_str(rd_t *r, char *dst, size_t cap) {             /* a `str`: u8 n, then n bytes of text; into dst (cap with its NUL) */
    const uint32_t n = rd_u(r, 1);
    if (r->err) return;
    if (r->i + n > r->n) { r->err = E_SHORT; return; }
    if (!ptpk_text_ok(r->p + r->i, n, cap - 1)) { r->err = E_TEXT; return; }
    memcpy(dst, r->p + r->i, n); dst[n] = 0;
    r->i += n;
}
/* the record ids (docs/ptpk.md) are ptpk_tags.h's, the writer's too; a known id seen twice is refused, by its bit */
#define BIT_C(id) (1u << ((id) - T_NAME))                       /* the common records: 0..4 */
#define BIT_S(id) (1u << (5 + (id) - T_HABITATS))               /* a species': 5..16 */
#define BIT_H(id) (1u << (5 + (id) - T_CLIMATE))                /* a habitat's: 5..11 */

/* the goals' names against the world's list: bit k = the world's k-th word (species.h LZ_GOAL_NAMES, FISH_GOAL_NAMES) */
static bool read_goals(world_t world, const uint8_t *g, size_t len, uint16_t *bits, char *why, size_t why_n) {
    const char *const *names = world == WORLD_TERRARIUM ? LZ_GOAL_NAMES : FISH_GOAL_NAMES;
    const int nn = world == WORLD_TERRARIUM ? LZ_GOALS : FISH_GOALS;
    const char *const wname = world == WORLD_TERRARIUM ? "terrarium" : "aquarium";
    rd_t r = { g, len, 0, E_OK };
    const uint32_t n = rd_range(&r, 1, 1, FISH_GOALS);
    *bits = 0;
    for (uint32_t k = 0; k < n && !r.err; k++) {
        const uint32_t l = rd_range(&r, 1, 1, 15);
        if (r.err) break;
        if (r.i + l > r.n) { r.err = E_SHORT; break; }
        int hit = -1;
        for (int j = 0; j < nn; j++) if (strlen(names[j]) == l && memcmp(names[j], g + r.i, l) == 0) hit = j;
        if (hit < 0) {
            char shown[16];
            for (uint32_t c = 0; c < l; c++) shown[c] = g[r.i + c] >= 0x20 && g[r.i + c] <= 0x7e ? (char)g[r.i + c] : '?';
            shown[l] = 0;
            BAD("goal \"%s\" is not in the %s's goal list", shown, wname);
        }
        if (*bits & (1u << hit)) BAD("goal \"%s\" is listed twice", names[hit]);
        *bits |= (uint16_t)(1u << hit);
        r.i += l;
    }
    if (r.err) BAD("GOALS %s", E_WORD[r.err]);
    if (r.i != r.n) BAD("GOALS is %u bytes, its layout is %u", (unsigned)r.n, (unsigned)r.i);
    return true;
}

/* the record's reader met no error and took every byte of it */
#define DONE(name) do { if (r.err) BAD("%s %s", name, E_WORD[r.err]); if (r.i != r.n) BAD("%s is %u bytes, its layout is %u", name, (unsigned)r.n, (unsigned)r.i); } while (0)
#define ONCE(bit, name) do { if (seen & (bit)) BAD("%s twice", name); seen |= (bit); } while (0)

/* the records a species and a habitat share (the reader is the record's: a copy). 1: it was one and is read; 0: not one
   of them; -1: refused (why) */
typedef struct { uint32_t ns_uid; int world; uint8_t legacy; bool test_only; } common_t;
/* (BAD here answers -1, "refused": the file-wide BAD answers false, which this function's caller would read as "not one of the common records" and go on to
   skip an optional one - a duplicate or a bad value let through; the fuzz in --selftest-ptpk found it) */
#undef BAD
#define BAD(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); return -1; } while (0)
static int common_record(unsigned id, rd_t r, int kind, uint32_t *seen_, common_t *c, char *name, char *why, size_t why_n) {
    uint32_t seen = *seen_;
    switch (id) {
    case T_NAME:
        ONCE(BIT_C(T_NAME), "NAME");
        if (!ptpk_text_ok(r.p, r.n, PACK_NAME_MAX - 1)) BAD("NAME is not 1..%d printable characters", PACK_NAME_MAX - 1);
        memcpy(name, r.p, r.n); name[r.n] = 0;
        break;
    case T_UID_NS:
        ONCE(BIT_C(T_UID_NS), "UID_NS");
        if (!ptpk_ns_ok(r.p, r.n)) BAD("UID_NS is not 3..47 characters of [a-z0-9._-]");
        c->ns_uid = ptpk_fnv1a(r.p, r.n);
        break;
    case T_WORLD:
        ONCE(BIT_C(T_WORLD), "WORLD");
        c->world = (int)rd_range(&r, 1, 1, 2);
        DONE("WORLD");
        break;
    case T_LEGACY_ID:
        ONCE(BIT_C(T_LEGACY_ID), "LEGACY_ID");
        c->legacy = (uint8_t)rd_range(&r, 1, 1, kind == PTPK_KIND_HABITAT ? 31 : 255);
        DONE("LEGACY_ID");
        break;
    case T_TEST_ONLY:
        ONCE(BIT_C(T_TEST_ONLY), "TEST_ONLY");
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

bool ptpk_read_species(const ptpk_header_t *h, species_t *p, uint8_t *legacy_id, char *why, size_t why_n) {
    memset(p, 0, sizeof *p);
    *legacy_id = 0;
    if (h->kind != PTPK_KIND_SPECIES) BAD("kind %u is not a species", h->kind);
    uint32_t seen = 0;
    common_t c = { 0, 0, 0, false };
    const uint8_t *goals_p = NULL; size_t goals_n = 0;
    for (size_t off = 0; off < h->len; ) {
        NEXT_RECORD();
        const int k = common_record(id, r, PTPK_KIND_SPECIES, &seen, &c, p->name, why, why_n);
        if (k < 0) return false;
        if (k > 0) continue;
        switch (id) {
        case T_HABITATS: {
            ONCE(BIT_S(T_HABITATS), "HABITATS");
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
            ONCE(BIT_S(T_ACTIVITY), "ACTIVITY");
            p->schedule = (sched_t)rd_range(&r, 1, 0, 2); p->settle_s = rd_f(&r);
            DONE("ACTIVITY");
            break;
        case T_HEAT:
            ONCE(BIT_S(T_HEAT), "HEAT");
            p->t_cold = rd_f(&r); p->t_lo = rd_f(&r); p->t_hi = rd_f(&r); p->t_hot = rd_f(&r); fa(&r, p->tau_s, 4);
            DONE("HEAT");
            break;
        case T_DRIVES:
            ONCE(BIT_S(T_DRIVES), "DRIVES");
            p->hunger_per_s = rd_f(&r); p->energy_per_s = rd_f(&r); p->energy_rest_per_s = rd_f(&r); p->curiosity_per_s = rd_f(&r);
            p->hunger_per_cricket = rd_f(&r);
            p->sleep_hunger_per_h = rd_f(&r); p->sleep_energy_per_h = rd_f(&r); p->sleep_stress_per_h = rd_f(&r);
            DONE("DRIVES");
            break;
        case T_WALK:
            ONCE(BIT_S(T_WALK), "WALK");
            fa(&r, p->walk_px_s, 4); fa(&r, p->bout_s, 2); fa(&r, p->pause_s, 2); p->turn_s = rd_f(&r);
            DONE("WALK");
            break;
        case T_LIFE:
            ONCE(BIT_S(T_LIFE), "LIFE");
            fa(&r, p->stage_s, 3); p->pop_cap = (uint8_t)rd_range(&r, 1, 0, 255);
            DONE("LIFE");
            break;
        case T_SHED:
            ONCE(BIT_S(T_SHED), "SHED");
            p->sheds = rd_range(&r, 1, 0, 1) != 0; p->shed_style = (shed_style_t)rd_range(&r, 1, 0, 1);
            fa(&r, p->shed_every_s, 4); p->shed_soon_s = rd_f(&r); p->shed_window_s = rd_f(&r);
            DONE("SHED");
            break;
        case T_DIET:
            ONCE(BIT_S(T_DIET), "DIET");
            p->diet = (uint8_t)rd_range(&r, 1, 0, DIET_CRICKET | DIET_MEALWORM | DIET_GREENS | DIET_PELLET);
            DONE("DIET");
            break;
        case T_BODY:
            ONCE(BIT_S(T_BODY), "BODY");
            p->plan = (body_plan_t)rd_range(&r, 1, 0, 1);
            p->geom.length_px = rd_f(&r); p->geom.head_w = rd_f(&r); p->geom.limb_len = rd_f(&r); p->geom.tail_fat = rd_f(&r); p->geom.eye_r = rd_f(&r);
            p->geom.eyelids = rd_range(&r, 1, 0, 1) != 0;
            DONE("BODY");
            break;
        case T_MORPHS: {
            ONCE(BIT_S(T_MORPHS), "MORPHS");
            const uint32_t n = rd_range(&r, 1, 1, SPECIES_MORPHS_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                rd_str(&r, p->morphs[j].name, MORPH_NAME_MAX);
                p->morphs[j].body = rd_range(&r, 4, 0, 0xffffff); p->morphs[j].accent = rd_range(&r, 4, 0, 0xffffff);
            }
            DONE("MORPHS");
            p->n_morphs = (uint8_t)n;
            break; }
        case T_PATTERNS:
            ONCE(BIT_S(T_PATTERNS), "PATTERNS");
            rd_range(&r, 1, 4, 4);
            for (int s = 0; s < 4 && !r.err; s++) rd_str(&r, p->pattern[s].name, PATTERN_NAME_MAX);
            DONE("PATTERNS");
            break;
        case T_GOALS:
            ONCE(BIT_S(T_GOALS), "GOALS");
            goals_p = r.p; goals_n = r.n;               /* (read once the world is known: the records come in any order) */
            break;
        default:
            if (tag & 0x8000u) BAD("record %04x is REQUIRED and this loader does not know it", id);
            break;                                      /* an optional one it does not know: skipped */
        }
    }
    static const struct { uint32_t bit; const char *name; } MUST[] = {
        { BIT_C(T_NAME), "NAME" }, { BIT_C(T_UID_NS), "UID_NS" }, { BIT_C(T_WORLD), "WORLD" }, { BIT_S(T_HABITATS), "HABITATS" },
        { BIT_S(T_ACTIVITY), "ACTIVITY" }, { BIT_S(T_HEAT), "HEAT" }, { BIT_S(T_DRIVES), "DRIVES" }, { BIT_S(T_LIFE), "LIFE" },
        { BIT_S(T_DIET), "DIET" }, { BIT_S(T_BODY), "BODY" }, { BIT_S(T_GOALS), "GOALS" } };
    for (size_t k = 0; k < sizeof MUST / sizeof MUST[0]; k++) if (!(seen & MUST[k].bit)) BAD("no %s record", MUST[k].name);
    p->world = c.world == 1 ? WORLD_AQUARIUM : WORLD_TERRARIUM;
    p->test_only = c.test_only;
    *legacy_id = c.legacy;
    if (!read_goals(p->world, goals_p, goals_n, &p->goals, why, why_n)) return false;
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
            ONCE(BIT_H(T_CLIMATE), "CLIMATE");
            p->floor_warm_c = rd_f(&r); p->floor_cool_c = rd_f(&r); p->night_drop_c = rd_f(&r); p->rise_c = rd_f(&r);
            DONE("CLIMATE");
            break;
        case T_BASK:
            ONCE(BIT_H(T_BASK), "BASK");
            p->bask_surf = (uint8_t)rd_range(&r, 1, 0, 255); p->bask_u = rd_f(&r); p->bask_c = rd_f(&r); p->bask_r_px = rd_f(&r);
            DONE("BASK");
            break;
        case T_HIDES: {
            ONCE(BIT_H(T_HIDES), "HIDES");
            const uint32_t n = rd_range(&r, 1, 0, HABITAT_HIDES_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                p->hides[j].kind = (hide_kind_t)rd_range(&r, 1, 0, HIDE_KINDS - 1);
                p->hides[j].u = rd_f(&r); p->hides[j].t_lit_c = rd_f(&r); p->hides[j].t_dark_c = rd_f(&r);
            }
            DONE("HIDES");
            p->n_hides = (uint8_t)n;
            break; }
        case T_ROCKS: {
            ONCE(BIT_H(T_ROCKS), "ROCKS");
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
            ONCE(BIT_H(T_RAMPS), "RAMPS");
            const uint32_t n = rd_range(&r, 1, 0, HABITAT_RAMPS_MAX);
            for (uint32_t j = 0; j < n && !r.err; j++) {
                p->ramps[j].lo = (uint8_t)rd_range(&r, 1, 0, 255); p->ramps[j].hi = (uint8_t)rd_range(&r, 1, 0, 255);
                p->ramps[j].u_lo = rd_f(&r); p->ramps[j].u_hi = rd_f(&r);
            }
            DONE("RAMPS");
            p->n_ramps = (uint8_t)n;
            break; }
        case T_MOISTURE:
            ONCE(BIT_H(T_MOISTURE), "MOISTURE");
            p->moist_start = rd_f(&r); p->moist_dry_per_s = rd_f(&r); p->humidity = rd_f(&r);
            DONE("MOISTURE");
            break;
        case T_DISH:
            ONCE(BIT_H(T_DISH), "DISH");
            p->dish_u = rd_f(&r);
            DONE("DISH");
            p->dish = true;
            break;
        default:
            if (tag & 0x8000u) BAD("record %04x is REQUIRED and this loader does not know it", id);
            break;                                      /* an optional one it does not know: skipped */
        }
    }
    static const struct { uint32_t bit; const char *name; } MUST[] = {
        { BIT_C(T_NAME), "NAME" }, { BIT_C(T_UID_NS), "UID_NS" }, { BIT_C(T_WORLD), "WORLD" },
        { BIT_H(T_CLIMATE), "CLIMATE" }, { BIT_H(T_BASK), "BASK" }, { BIT_H(T_MOISTURE), "MOISTURE" } };
    for (size_t k = 0; k < sizeof MUST / sizeof MUST[0]; k++) if (!(seen & MUST[k].bit)) BAD("no %s record", MUST[k].name);
    p->world = c.world == 1 ? WORLD_AQUARIUM : WORLD_TERRARIUM;
    p->test_only = c.test_only;
    *legacy_id = c.legacy;
    if (c.ns_uid != h->uid) BAD("the header's uid %08x is not the name's (%08x)", (unsigned)h->uid, (unsigned)c.ns_uid);
    p->uid = h->uid;
    return true;
}

/* ---- the registry's entry ---- */
static bool s_one_world = !PT_ALL_PACKS;   /* a release image takes its own world's packs alone */
void packs_world_filter(bool on) { s_one_world = on; }
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
    if (h.kind == PTPK_KIND_SPECIES) {
        switch (species_seen(h.uid, h.crc, &by)) {
        case PACK_SAME: { const species_t *s = species_by_uid(h.uid); say(info, h.kind, s->uid, s->id, s->name); return PACKS_SAME; }
        case PACK_UID_TAKEN: NO("uid %08x is %s's already (this is another file)", (unsigned)h.uid, by);
        default: break;
        }
        species_t *p = species_slot();
        if (!p) NO("no room: the species pool is full");
        if (!ptpk_read_species(&h, p, &legacy, why, why_n)) return PACKS_REFUSED;
        if (s_one_world && p->world != PT_WORLD_ID)
            NO("%s is a %s pack and this is the %s's firmware", p->name, p->world == WORLD_TERRARIUM ? "terrarium" : "aquarium", PT_WORLD_NAME);
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
    if (s_one_world && p->world != PT_WORLD_ID)
        NO("%s is a %s pack and this is the %s's firmware", p->name, p->world == WORLD_TERRARIUM ? "terrarium" : "aquarium", PT_WORLD_NAME);
    if (!habitat_commit(p, builtin, builtin ? legacy : 0, h.crc, why, why_n)) return PACKS_REFUSED;
    say(info, h.kind, p->uid, p->id, p->name);
    return PACKS_ADDED;
}

bool g_packs_ready;
/* the built-in packs through the one entry (docs/ptpk.md "The loader"): each is encoded from its const definition (builtin.c) into
   a buffer on this stack - the firmware's main task, packs_init being the first thing app_main does, so no small task ever
   comes here first - and offered to packs_add as the bytes of a file, exactly as a folder's or the card's would be: the same
   validation, species_check / habitat_check, uid dedupe, and the LEGACY_ID kept for these alone (PACKS_BUILTIN) */
void packs_load_builtin(void) {
    g_packs_ready = true;                 /* first: the registry's own calls come back to packs_init while this runs */
    uint8_t file[PTPK_FILE_MAX];
    char why[160];
    for (int i = 0; i < PTPK_BUILTIN_HABITATS + PTPK_BUILTIN_SPECIES; i++) {    /* habitats first: a species lists them */
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
}
