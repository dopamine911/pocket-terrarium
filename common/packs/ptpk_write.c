/* ptpk_write.c - the PTPK encoder (ptpk_write.h; docs/ptpk.md is the byte-exact contract) and the format's two hashes. The encoder
 * is ptpk.c's reader read backwards: the same ids (ptpk_tags.h), the same layouts, the same REQUIRED bits, the same limits, one
 * record to one case, in ascending id - and it refuses what the reader would, so a file it writes is a file that reader takes
 * (--selftest-ptpk writes every built-in pack and reads it back, field for field). Registry-free by design: nothing here may
 * reach species.c, habitat.c, manifest.c, terrain or the tank (tools/ptpk_emit.c links this file alone). */
#include "ptpk_write.h"
#include "ptpk_tags.h"
#include <string.h>

/* ---- CRC-32 (zlib's: reflected, 0xedb88320, init and final xor 0xffffffff), a nibble at a time: 64 B of table ---- */
uint32_t ptpk_crc32(const uint8_t *b, size_t n) {
    static const uint32_t T[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu, 0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu };
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) { c ^= b[i]; c = (c >> 4) ^ T[c & 15]; c = (c >> 4) ^ T[c & 15]; }
    return ~c;
}
uint32_t ptpk_fnv1a(const uint8_t *b, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ---- the format's text ---- */
bool ptpk_text_ok(const uint8_t *s, size_t n, size_t max) {   /* 1..max bytes of 0x20..0x7e, no space at either end */
    if (n < 1 || n > max || s[0] == ' ' || s[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++) if (s[i] < 0x20 || s[i] > 0x7e) return false;
    return true;
}
bool ptpk_ns_ok(const uint8_t *s, size_t n) {                  /* UID_NS: [a-z0-9._-], 3..47 bytes */
    if (n < 3 || n > 47) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-')) return false;
    return true;
}
bool ptpk_goal_ok(const uint8_t *s, size_t n) {                /* a goal's word: 1..15 of [a-z0-9_], a lowercase letter first */
    if (n < 1 || n > 15 || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}
bool ptpk_tag_ok(const uint8_t *s, size_t n) {                 /* a model's tag: 1..15 of [A-Za-z0-9._-] */
    if (n < 1 || n > 15) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-')) return false;
    return true;
}

/* ---- a writer that cannot leave its buffer: the first thing it cannot write is its `why`, and nothing after that counts ---- */
typedef struct { uint8_t *p; size_t cap, n; const char *why; } wr_t;

static void no(wr_t *w, const char *why) { if (!w->why) w->why = why; }
static void start(wr_t *w, uint8_t *out, size_t cap) {         /* the body goes behind the header's 20 bytes, up to the format's limit */
    w->p = out + PTPK_HEADER_BYTES; w->n = 0; w->why = NULL;
    w->cap = cap > PTPK_HEADER_BYTES ? cap - PTPK_HEADER_BYTES : 0;
    if (w->cap > PTPK_BODY_MAX) w->cap = PTPK_BODY_MAX;
    if (cap < PTPK_HEADER_BYTES) no(w, "no room for the 20-byte header");
}
/* (the primitives stay out of line: the encoder runs once at boot, so what counts is the image's size, not its speed) */
__attribute__((noinline)) static void put(wr_t *w, uint32_t v, int bytes) {   /* an unsigned, little-endian */
    if (w->n + (size_t)bytes > w->cap) { no(w, "the body is over 2048 bytes, or the buffer is too small"); return; }
    for (int k = 0; k < bytes; k++) w->p[w->n++] = (uint8_t)(v >> (8 * k));
}
static void put_range(wr_t *w, int bytes, uint32_t v, uint32_t hi, const char *why) {   /* an unsigned in 0..hi (an enum or a count) */
    if (v > hi) { no(w, why); return; }
    put(w, v, bytes);
}
__attribute__((noinline)) static void put_f(wr_t *w, float f) {
    uint32_t u; memcpy(&u, &f, 4);
    if ((u & 0x7f800000u) == 0x7f800000u) { no(w, "a float that is a NaN or an infinity"); return; }   /* (as bits, as the reader: -ffast-math may fold isfinite) */
    put(w, u, 4);
}
static void put_fa(wr_t *w, const float *f, int n) { for (int k = 0; k < n; k++) put_f(w, f[k]); }
static bool zero(const float *f, int n) {                      /* every float all bits clear (a -0 is not zero here: it is written) */
    for (int k = 0; k < n; k++) { uint32_t u; memcpy(&u, &f[k], 4); if (u) return false; }
    return true;
}

/* a text of 1..max printable characters; s holds max + 1 bytes at least or ends sooner (a name's array, a string literal) */
static size_t text_len(wr_t *w, const char *s, size_t max, const char *why) {
    size_t n = 0;
    while (n <= max && s[n]) n++;
    if (n > max || !ptpk_text_ok((const uint8_t *)s, n, max)) { no(w, why); return 0; }
    return n;
}
static void put_text(wr_t *w, const char *s, size_t max, const char *why) {      /* a whole-record string: the value is the text */
    const size_t n = text_len(w, s, max, why);
    for (size_t k = 0; k < n; k++) put(w, (uint8_t)s[k], 1);
}
static void put_str(wr_t *w, const char *s, size_t max, const char *why) {       /* a `str`: u8 n, then n bytes */
    const size_t n = text_len(w, s, max, why);
    if (!n) return;
    put(w, (uint32_t)n, 1);
    for (size_t k = 0; k < n; k++) put(w, (uint8_t)s[k], 1);
}
/* a goal's word as a `str` (the length of 1..15 and the grammar are the format's) */
static void put_word(wr_t *w, const char *s) {
    size_t n = 0;
    while (n <= GOAL_NAME_MAX && s[n]) n++;
    if (n >= GOAL_NAME_MAX || !ptpk_goal_ok((const uint8_t *)s, n)) { no(w, "a goal's word is not 1..15 of [a-z0-9_], a letter first"); return; }
    put(w, (uint32_t)n, 1);
    for (size_t k = 0; k < n; k++) put(w, (uint8_t)s[k], 1);
}
/* the GOALS / MODEL_GOALS value: u8 n (1..8), then n words, none twice */
static void put_goals(wr_t *w, int n, const char (*goal)[GOAL_NAME_MAX]) {
    if (n < 1 || n > SPECIES_GOALS_MAX) { no(w, "it has no goal, or more than eight"); return; }
    put(w, (uint32_t)n, 1);
    for (int j = 0; j < n; j++) {
        for (int m = 0; m < j; m++) if (strncmp(goal[m], goal[j], GOAL_NAME_MAX) == 0) no(w, "a goal listed twice");
        put_word(w, goal[j]);
    }
}

/* a record: its tag and a length to be filled in, the value after it, close_rec when the value is in */
static size_t open_rec(wr_t *w, unsigned id, bool required) { put(w, id | (required ? PTPK_REQUIRED : 0u), 2); put(w, 0, 2); return w->n; }
static void close_rec(wr_t *w, size_t at) {
    if (w->why) return;
    const size_t len = w->n - at;
    w->p[at - 2] = (uint8_t)len; w->p[at - 1] = (uint8_t)(len >> 8);
}

/* NAME and UID_NS; the uid is the hash of uid_ns, and the struct's own must be it */
static void ident(wr_t *w, const char *name, const char *uid_ns, uint32_t uid) {
    size_t at = open_rec(w, T_NAME, true);
    put_text(w, name, PACK_NAME_MAX - 1, "the name is not 1..23 printable characters");
    close_rec(w, at);
    size_t n = 0;
    while (n <= 47 && uid_ns[n]) n++;
    at = open_rec(w, T_UID_NS, true);
    if (!ptpk_ns_ok((const uint8_t *)uid_ns, n)) no(w, "uid_ns is not 3..47 characters of [a-z0-9._-]");
    else if (!uid || uid != ptpk_fnv1a((const uint8_t *)uid_ns, n)) no(w, "the uid is not the hash of uid_ns");
    for (size_t k = 0; k < n; k++) put(w, (uint8_t)uid_ns[k], 1);
    close_rec(w, at);
}
/* LEGACY_ID TEST_ONLY WORLDS (a species' and a habitat's), each only when set */
static void extras(wr_t *w, unsigned legacy, unsigned legacy_max, bool test_only, unsigned worlds) {
    if (legacy) {
        const size_t at = open_rec(w, T_LEGACY_ID, false);
        put_range(w, 1, legacy, legacy_max, "the legacy id (its `id`) is out of range");
        close_rec(w, at);
    }
    if (test_only) {
        const size_t at = open_rec(w, T_TEST_ONLY, false);
        put(w, 1, 1);
        close_rec(w, at);
    }
    if (worlds) {
        const size_t at = open_rec(w, T_WORLDS, false);
        put_range(w, 1, worlds, 255, "the worlds mask is past 255");
        close_rec(w, at);
    }
}

/* the header over the body: its size, or 0 and the reason */
static size_t finish(wr_t *w, int kind, uint32_t uid, uint8_t *out, const char **why) {
    if (w->why) { if (why) *why = w->why; return 0; }
    const uint32_t len = (uint32_t)w->n, crc = ptpk_crc32(w->p, w->n);
    memcpy(out, "PTPK", 4);
    out[4] = PTPK_FMT; out[5] = (uint8_t)kind; out[6] = 0; out[7] = 0;
    for (int k = 0; k < 4; k++) { out[8 + k] = (uint8_t)(uid >> (8 * k)); out[12 + k] = (uint8_t)(len >> (8 * k)); out[16 + k] = (uint8_t)(crc >> (8 * k)); }
    return PTPK_HEADER_BYTES + (size_t)len;
}

size_t ptpk_encode_species(const species_t *p, const char *uid_ns, uint8_t *out, size_t cap, const char **why) {
    wr_t w; start(&w, out, cap);
    ident(&w, p->name, uid_ns, p->uid);
    extras(&w, p->id, 255, p->test_only, p->worlds);
    size_t at;
    at = open_rec(&w, T_HABITATS, true);
    if (p->n_habitat_uids < 1 || p->n_habitat_uids > SPECIES_HABITATS_MAX) no(&w, "it lists no habitat, or more than four");
    else {
        put(&w, p->n_habitat_uids, 1);
        for (int j = 0; j < p->n_habitat_uids; j++) {
            bool twice = false;
            for (int m = 0; m < j; m++) twice |= p->habitat_uid[m] == p->habitat_uid[j];
            if (!p->habitat_uid[j] || twice) no(&w, "a habitat uid that is 0 or listed twice");
            put(&w, p->habitat_uid[j], 4);
        }
    }
    close_rec(&w, at);
    at = open_rec(&w, T_ACTIVITY, true);
    put_range(&w, 1, (unsigned)p->schedule, 2, "the schedule is not day, dusk or night");
    put_f(&w, p->settle_s);
    close_rec(&w, at);
    at = open_rec(&w, T_HEAT, true);
    put_f(&w, p->t_cold); put_f(&w, p->t_lo); put_f(&w, p->t_hi); put_f(&w, p->t_hot); put_fa(&w, p->tau_s, 4);
    close_rec(&w, at);
    at = open_rec(&w, T_DRIVES, true);
    put_f(&w, p->hunger_per_s); put_f(&w, p->energy_per_s); put_f(&w, p->energy_rest_per_s); put_f(&w, p->curiosity_per_s);
    put_f(&w, p->hunger_per_cricket);
    put_f(&w, p->sleep_hunger_per_h); put_f(&w, p->sleep_energy_per_h); put_f(&w, p->sleep_stress_per_h);
    close_rec(&w, at);
    at = open_rec(&w, T_LIFE, true);
    put_fa(&w, p->stage_s, 3); put(&w, p->pop_cap, 1);
    close_rec(&w, at);
    if (p->sheds || p->shed_style != SHED_WHOLE || !zero(p->shed_every_s, 4) || !zero(&p->shed_soon_s, 1) || !zero(&p->shed_window_s, 1)) {   /* absent: no shed cycle (the fish) */
        at = open_rec(&w, T_SHED, true);
        put(&w, p->sheds ? 1 : 0, 1); put_range(&w, 1, (unsigned)p->shed_style, 1, "the shed style is neither whole nor patches");
        put_fa(&w, p->shed_every_s, 4); put_f(&w, p->shed_soon_s); put_f(&w, p->shed_window_s);
        close_rec(&w, at);
    }
    at = open_rec(&w, T_DIET, true);
    put_range(&w, 1, p->diet, DIET_CRICKET | DIET_MEALWORM | DIET_GREENS | DIET_PELLET, "a diet bit that is nobody's");
    close_rec(&w, at);
    at = open_rec(&w, T_BODY, true);
    put_range(&w, 1, (unsigned)p->plan, 1, "the body plan is neither lizard nor fish");
    put_f(&w, p->geom.length_px); put_f(&w, p->geom.head_w); put_f(&w, p->geom.limb_len); put_f(&w, p->geom.tail_fat); put_f(&w, p->geom.eye_r);
    put(&w, p->geom.eyelids ? 1 : 0, 1);
    close_rec(&w, at);
    if (p->n_morphs) {
        at = open_rec(&w, T_MORPHS, false);
        if (p->n_morphs > SPECIES_MORPHS_MAX) no(&w, "more than six morphs");
        else {
            put(&w, p->n_morphs, 1);
            for (int j = 0; j < p->n_morphs; j++) {
                put_str(&w, p->morphs[j].name, MORPH_NAME_MAX - 1, "a morph's name is not 1..11 printable characters");
                put_range(&w, 4, p->morphs[j].body, 0xffffff, "a morph's colour is past 0xffffff");
                put_range(&w, 4, p->morphs[j].accent, 0xffffff, "a morph's colour is past 0xffffff");
            }
        }
        close_rec(&w, at);
    }
    if (p->pattern[0].name[0] || p->pattern[1].name[0] || p->pattern[2].name[0] || p->pattern[3].name[0]) {   /* all four stages, or none */
        at = open_rec(&w, T_PATTERNS, false);
        put(&w, 4, 1);
        for (int s = 0; s < 4; s++) put_str(&w, p->pattern[s].name, PATTERN_NAME_MAX - 1, "a stage's pattern is not 1..11 printable characters");
        close_rec(&w, at);
    }
    at = open_rec(&w, T_GOALS, true);
    put_goals(&w, p->n_goals, p->goal);
    close_rec(&w, at);
    /* how it moves: one entry a mode, the walk's numbers on the walk's own (a swim and a slither carry none yet) */
    at = open_rec(&w, T_LOCOMOTION, true);
    if (p->n_loco < 1 || p->n_loco > SPECIES_LOCO_MAX) no(&w, "it moves no way, or in more than three");
    else {
        const float none[9] = { 0 };
        bool walks = false;
        put(&w, p->n_loco, 1);
        for (int j = 0; j < p->n_loco; j++) {
            const int mi = caps_index(p->loco[j].mode), ai = caps_index(p->loco[j].area);
            if (mi < 0 || !(p->loco[j].mode & CAPS_MOVES)) no(&w, "a locomotion entry whose mode is not one movement mode");
            if (ai < 0 || !(p->loco[j].area & CAPS_AREAS)) no(&w, "a locomotion entry whose area is not one area");
            for (int m = 0; m < j; m++) if (p->loco[m].mode == p->loco[j].mode) no(&w, "a movement mode listed twice");
            put(&w, (uint32_t)mi, 1); put(&w, (uint32_t)ai, 1);
            if (p->loco[j].mode == CAP_WALK) {
                walks = true;
                put_fa(&w, p->walk_px_s, 4); put_fa(&w, p->bout_s, 2); put_fa(&w, p->pause_s, 2); put_f(&w, p->turn_s);
            } else put_fa(&w, none, 9);
        }
        if (!walks && (!zero(p->walk_px_s, 4) || !zero(p->bout_s, 2) || !zero(p->pause_s, 2) || !zero(&p->turn_s, 1))) no(&w, "walk numbers, and no walk among its modes");
    }
    close_rec(&w, at);
    at = open_rec(&w, T_MEDIA_NEEDS, true);
    if (p->media_needs & ~CAPS_MEDIA) no(&w, "a need that is not an area or a fitting (movement is LOCOMOTION's, foods are DIET's)");
    if (p->media_wants & ~CAPS_KNOWN) no(&w, "a want that names a capability nobody has");
    put(&w, p->media_needs, 4); put(&w, p->media_wants, 4);
    close_rec(&w, at);
    return finish(&w, PTPK_KIND_SPECIES, p->uid, out, why);
}

size_t ptpk_encode_habitat(const habitat_t *h, const char *uid_ns, uint8_t *out, size_t cap, const char **why) {
    wr_t w; start(&w, out, cap);
    ident(&w, h->name, uid_ns, h->uid);
    extras(&w, h->id, 31, h->test_only, h->worlds);
    size_t at;
    at = open_rec(&w, T_CLIMATE, true);
    put_f(&w, h->floor_warm_c); put_f(&w, h->floor_cool_c); put_f(&w, h->night_drop_c); put_f(&w, h->rise_c);
    close_rec(&w, at);
    at = open_rec(&w, T_BASK, true);
    put(&w, h->bask_surf, 1); put_f(&w, h->bask_u); put_f(&w, h->bask_c); put_f(&w, h->bask_r_px);
    close_rec(&w, at);
    if (h->n_hides) {                                           /* absent: none */
        at = open_rec(&w, T_HIDES, true);
        if (h->n_hides > HABITAT_HIDES_MAX) no(&w, "more than four hides");
        else {
            put(&w, h->n_hides, 1);
            for (int j = 0; j < h->n_hides; j++) {
                put_range(&w, 1, (unsigned)h->hides[j].kind, HIDE_KINDS - 1, "a hide's kind is not warm, humid or cool");
                put_f(&w, h->hides[j].u); put_f(&w, h->hides[j].t_lit_c); put_f(&w, h->hides[j].t_dark_c);
            }
        }
        close_rec(&w, at);
    }
    if (h->n_rocks) {
        at = open_rec(&w, T_ROCKS, true);
        if (h->n_rocks > HABITAT_ROCKS_MAX) no(&w, "more than four rocks");
        else {
            put(&w, h->n_rocks, 1);
            for (int j = 0; j < h->n_rocks; j++) {
                const rock_t *r = &h->rocks[j];
                if (r->n < 2 || r->n > ROCK_PTS_MAX) { no(&w, "a rock of fewer than two or more than four points"); break; }
                put(&w, r->n, 1); put(&w, r->shelf ? 1 : 0, 1);
                for (int i = 0; i < r->n; i++) { put_f(&w, r->u[i]); put_f(&w, r->v[i]); }
            }
        }
        close_rec(&w, at);
    }
    if (h->n_ramps) {
        at = open_rec(&w, T_RAMPS, true);
        if (h->n_ramps > HABITAT_RAMPS_MAX) no(&w, "more than six ramps");
        else {
            put(&w, h->n_ramps, 1);
            for (int j = 0; j < h->n_ramps; j++) { put(&w, h->ramps[j].lo, 1); put(&w, h->ramps[j].hi, 1); put_f(&w, h->ramps[j].u_lo); put_f(&w, h->ramps[j].u_hi); }
        }
        close_rec(&w, at);
    }
    at = open_rec(&w, T_MOISTURE, true);
    put_f(&w, h->moist_start); put_f(&w, h->moist_dry_per_s); put_f(&w, h->humidity);
    close_rec(&w, at);
    if (h->dish) {                                              /* absent: no dish */
        at = open_rec(&w, T_DISH, true);
        put_f(&w, h->dish_u);
        close_rec(&w, at);
    }
    at = open_rec(&w, T_AREAS, true);
    if (h->areas & ~CAPS_AREAS) no(&w, "an area that is nobody's");
    put(&w, h->areas, 4);
    close_rec(&w, at);
    at = open_rec(&w, T_PROVIDES, true);
    if (h->provides & ~CAPS_PROVIDED) no(&w, "a provision that is not a lamp, hides or a food");
    put(&w, h->provides, 4);
    close_rec(&w, at);
    return finish(&w, PTPK_KIND_HABITAT, h->uid, out, why);
}

size_t ptpk_encode_manifest(const model_manifest_t *m, const char *uid_ns, uint8_t *out, size_t cap, const char **why) {
    wr_t w; start(&w, out, cap);
    ident(&w, m->name, uid_ns, m->uid);
    size_t at = open_rec(&w, T_MODEL_TAG, true);
    {
        size_t n = 0;
        while (n <= MODEL_TAG_MAX && m->tag[n]) n++;
        if (n >= MODEL_TAG_MAX || !ptpk_tag_ok((const uint8_t *)m->tag, n)) no(&w, "the model's tag is not 1..15 characters of [A-Za-z0-9._-]");
        for (size_t k = 0; k < n && k < MODEL_TAG_MAX; k++) put(&w, (uint8_t)m->tag[k], 1);
    }
    close_rec(&w, at);
    at = open_rec(&w, T_MODEL_GOALS, true);
    put_goals(&w, m->n_goals, m->goal);
    close_rec(&w, at);
    return finish(&w, PTPK_KIND_MODEL, m->uid, out, why);
}
