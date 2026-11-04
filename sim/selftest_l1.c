#define _DEFAULT_SOURCE   /* like the other sim selftests (-std=c11, glibc) */
/* selftest_l1.c - THE L1 PARITY (P2a; R4 in PLAN.md): the C encoder advisor_core_encode_l1 against
 * model/gen_traces.py encode_l1_snap, byte for byte, on the fixture model/make_l1_parity.py writes
 * (model/testdata/l1_parity.jsonl: one {"snap": {...}, "line": "..."} a line), then advisor_core_l1_snap on a
 * live terrarium.
 *
 *   ./fishsim --selftest-l1-encode     (make check)
 *
 * The fixture is read with the small JSON reader below (objects, arrays, numbers, strings with \" \\ \/
 * escapes, true / false / null; anything else is malformed): no dependencies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "tank.h"
#include "advisor_core.h"

#define L1F(...) do { printf("FAIL: selftest-l1-encode: "); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

/* ---- the JSON reader: one value (a fixture line) into a node pool ---- */
enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };
#define JN_MAX  128                 /* nodes a line (a fixture line has ~40) */
#define JS_MAX  4096                /* string bytes a line, escapes undone */
#define J_DEPTH 8
typedef struct {
    int         type;
    double      num;                /* J_NUM */
    const char *str;                /* J_STR: the text */
    const char *key;                /* a member of an object: its key */
    int         first, next;        /* J_ARR / J_OBJ: the first child; every node: its next sibling (-1: none) */
    int         count;              /* J_ARR / J_OBJ: children */
} jnode_t;
typedef struct {
    const char *p;
    jnode_t     n[JN_MAX];
    int         nn;
    char        s[JS_MAX];
    int         ns;
} json_t;

static bool j_digit(char c) { return c >= '0' && c <= '9'; }
static void j_ws(json_t *j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++; }
static int j_new(json_t *j, int type) {
    if (j->nn >= JN_MAX) return -1;
    jnode_t *n = &j->n[j->nn];
    memset(n, 0, sizeof *n);
    n->type = type; n->first = n->next = -1;
    return j->nn++;
}
/* the string at j->p (its opening quote) into the pool; NULL if malformed */
static const char *j_str(json_t *j) {
    if (*j->p != '"') return NULL;
    j->p++;
    const char *o = j->s + j->ns;
    for (;;) {
        char c = *j->p++;
        if (c == '"') break;
        if ((unsigned char)c < 0x20) return NULL;                 /* a control character, or the text's end */
        if (c == '\\') {
            c = *j->p++;
            if (c != '"' && c != '\\' && c != '/') return NULL;   /* \n \t \uXXXX ...: no fixture writes them */
        }
        if (j->ns >= JS_MAX - 1) return NULL;
        j->s[j->ns++] = c;
    }
    if (j->ns >= JS_MAX) return NULL;
    j->s[j->ns++] = 0;
    return o;
}
/* a number, JSON's grammar exactly (no +, no leading zeros, no hex / inf / nan), then strtod */
static bool j_num(json_t *j, double *v) {
    const char *q = j->p;
    if (*q == '-') q++;
    if (*q == '0') q++;
    else if (*q >= '1' && *q <= '9') while (j_digit(*q)) q++;
    else return false;
    if (*q == '.') { q++; if (!j_digit(*q)) return false; while (j_digit(*q)) q++; }
    if (*q == 'e' || *q == 'E') {
        q++;
        if (*q == '+' || *q == '-') q++;
        if (!j_digit(*q)) return false;
        while (j_digit(*q)) q++;
    }
    char buf[64];
    size_t len = (size_t)(q - j->p);
    if (len >= sizeof buf) return false;
    memcpy(buf, j->p, len); buf[len] = 0;
    char *end;
    *v = strtod(buf, &end);
    if (*end) return false;
    j->p = q;
    return true;
}
static int j_value(json_t *j, int depth) {
    if (depth > J_DEPTH) return -1;
    j_ws(j);
    char c = *j->p;
    int id;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        char close = obj ? '}' : ']';
        if ((id = j_new(j, obj ? J_OBJ : J_ARR)) < 0) return -1;
        j->p++; j_ws(j);
        if (*j->p == close) { j->p++; return id; }
        int last = -1;
        for (;;) {
            const char *key = NULL;
            if (obj) {
                j_ws(j);
                if (!(key = j_str(j))) return -1;
                j_ws(j);
                if (*j->p != ':') return -1;
                j->p++;
            }
            int ch = j_value(j, depth + 1);
            if (ch < 0) return -1;
            j->n[ch].key = key;
            if (last < 0) j->n[id].first = ch; else j->n[last].next = ch;
            last = ch; j->n[id].count++;
            j_ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == close) { j->p++; return id; }
            return -1;
        }
    }
    if (c == '"') {
        const char *s = j_str(j);
        if (!s || (id = j_new(j, J_STR)) < 0) return -1;
        j->n[id].str = s;
        return id;
    }
    if (strncmp(j->p, "true", 4) == 0)  { j->p += 4; return j_new(j, J_TRUE); }
    if (strncmp(j->p, "false", 5) == 0) { j->p += 5; return j_new(j, J_FALSE); }
    if (strncmp(j->p, "null", 4) == 0)  { j->p += 4; return j_new(j, J_NULL); }
    double v;
    if (!j_num(j, &v) || (id = j_new(j, J_NUM)) < 0) return -1;
    j->n[id].num = v;
    return id;
}
/* the whole text as one value (whitespace around it only): its node, -1 if malformed */
static int j_parse(json_t *j, const char *text) {
    j->p = text; j->nn = 0; j->ns = 0;
    int root = j_value(j, 0);
    if (root < 0) return -1;
    j_ws(j);
    return *j->p ? -1 : root;
}
static int j_get(const json_t *j, int obj, const char *key) {
    for (int c = j->n[obj].first; c >= 0; c = j->n[c].next) if (strcmp(j->n[c].key, key) == 0) return c;
    return -1;
}

/* ---- a fixture `snap` -> l1_snap_t: every key required ---- */
static char s_err[160];
static int want_key(const json_t *j, int o, const char *k) {
    int c = j_get(j, o, k);
    if (c < 0) snprintf(s_err, sizeof s_err, "missing key \"%s\"", k);
    return c;
}
static bool want_num(const json_t *j, int o, const char *k, double *v) {
    int c = want_key(j, o, k);
    if (c < 0) return false;
    if (j->n[c].type != J_NUM) { snprintf(s_err, sizeof s_err, "\"%s\" is not a number", k); return false; }
    *v = j->n[c].num;
    return true;
}
static bool want_int(const json_t *j, int o, const char *k, int lo, int hi, int *v) {
    double d;
    if (!want_num(j, o, k, &d)) return false;
    if (d < lo || d > hi || d != (double)(int)d) {
        snprintf(s_err, sizeof s_err, "\"%s\" %g is not a whole number %d..%d", k, d, lo, hi);
        return false;
    }
    *v = (int)d;
    return true;
}
static bool want_word(const json_t *j, int o, const char *k, const char *const *names, int count, int *idx) {
    int c = want_key(j, o, k);
    if (c < 0) return false;
    if (j->n[c].type != J_STR) { snprintf(s_err, sizeof s_err, "\"%s\" is not a word", k); return false; }
    for (int i = 0; i < count; i++) if (strcmp(j->n[c].str, names[i]) == 0) { *idx = i; return true; }
    snprintf(s_err, sizeof s_err, "\"%s\": unknown word \"%s\"", k, j->n[c].str);
    return false;
}
static bool want_bool(const json_t *j, int o, const char *k, bool *b) {
    int c = want_key(j, o, k);
    if (c < 0) return false;
    if (j->n[c].type != J_TRUE && j->n[c].type != J_FALSE) {
        snprintf(s_err, sizeof s_err, "\"%s\" is not true or false", k);
        return false;
    }
    *b = j->n[c].type == J_TRUE;
    return true;
}
/* null: none; [x, y]: a point */
static bool want_point(const json_t *j, int o, const char *k, l1_point_t *p) {
    int c = want_key(j, o, k);
    if (c < 0) return false;
    p->on = false; p->x = p->y = 0;
    if (j->n[c].type == J_NULL) return true;
    int a = j->n[c].first, b = a >= 0 ? j->n[a].next : -1;
    if (j->n[c].type != J_ARR || j->n[c].count != 2 || j->n[a].type != J_NUM || j->n[b].type != J_NUM) {
        snprintf(s_err, sizeof s_err, "\"%s\" is not null or [x, y]", k);
        return false;
    }
    p->on = true; p->x = (float)j->n[a].num; p->y = (float)j->n[b].num;
    return true;
}
static bool snap_of(const json_t *j, int o, l1_snap_t *s) {
    static const struct { const char *k; size_t off; } FLOATS[] = {
        { "x", offsetof(l1_snap_t, x) }, { "y", offsetof(l1_snap_t, y) }, { "heading", offsetof(l1_snap_t, heading) },
        { "body_c", offsetof(l1_snap_t, body_c) },
        { "hunger", offsetof(l1_snap_t, hunger) }, { "energy", offsetof(l1_snap_t, energy) },
        { "stress", offsetof(l1_snap_t, stress) }, { "curiosity", offsetof(l1_snap_t, curiosity) },
        { "trust", offsetof(l1_snap_t, trust) }, { "bored", offsetof(l1_snap_t, bored) },
        { "bold", offsetof(l1_snap_t, bold) }, { "social", offsetof(l1_snap_t, social) },
    };
    memset(s, 0, sizeof *s);
    int pack;
    if (!want_int(j, o, "pack", 0, 255, &pack)) return false;
    s->pack = (uint8_t)pack;
    if (!want_int(j, o, "zone", 1, 6, &s->zone)) return false;
    for (size_t i = 0; i < sizeof FLOATS / sizeof FLOATS[0]; i++) {
        double v;
        if (!want_num(j, o, FLOATS[i].k, &v)) return false;
        *(float *)((char *)s + FLOATS[i].off) = (float)v;
    }
    return want_word(j, o, "stage", L1_STAGE_NAMES, 4, &s->stage) &&
           want_word(j, o, "shed", L1_SHED_NAMES, 3, &s->shed) &&
           want_word(j, o, "last", LZ_GOAL_NAMES, LZ_GOALS, &s->last) &&
           want_word(j, o, "phase", PHASE_NAMES, PHASE_N, &s->phase) &&
           want_point(j, o, "prey", &s->prey) && want_point(j, o, "other", &s->other) &&
           want_point(j, o, "warm", &s->warm) && want_point(j, o, "shelter", &s->shelter) &&
           want_bool(j, o, "in_shelter", &s->in_shelter) && want_point(j, o, "wall", &s->wall);
}

static json_t s_json;
static char   s_text[8192];
static tank_t s_tank;

int selftest_l1_encode(void) {
    /* 1. the fixture, byte for byte */
    const char *path = "../model/testdata/l1_parity.jsonl";
    FILE *fp = fopen(path, "r");
    if (!fp) { path = "model/testdata/l1_parity.jsonl"; fp = fopen(path, "r"); }
    if (!fp) L1F("no fixture (../model/testdata/l1_parity.jsonl or model/testdata/l1_parity.jsonl)");
#define L1F_FP(...) do { fclose(fp); L1F(__VA_ARGS__); } while (0)
    int lineno = 0, total = 0, ok = 0, bad = 0, n_gecko = 0, n_warm = 0;
    bool heat_seen[10] = { false }, stage_seen[4] = { false }, shed_seen[3] = { false };
    bool last_seen[LZ_GOALS] = { false }, phase_seen[PHASE_N] = { false };
    while (fgets(s_text, sizeof s_text, fp)) {
        lineno++;
        size_t len = strlen(s_text);
        if (len == sizeof s_text - 1 && s_text[len - 1] != '\n') L1F_FP("%s line %d: longer than %d bytes", path, lineno, (int)sizeof s_text - 2);
        const char *q = s_text;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (!*q) continue;                                          /* a blank line (the file's end) */
        int root = j_parse(&s_json, s_text);
        if (root < 0 || s_json.n[root].type != J_OBJ) L1F_FP("%s line %d: not a JSON object", path, lineno);
        int sn = j_get(&s_json, root, "snap"), ln = j_get(&s_json, root, "line");
        if (sn < 0 || s_json.n[sn].type != J_OBJ) L1F_FP("%s line %d: no \"snap\" object", path, lineno);
        if (ln < 0 || s_json.n[ln].type != J_STR) L1F_FP("%s line %d: no \"line\" string", path, lineno);
        l1_snap_t s;
        if (!snap_of(&s_json, sn, &s)) L1F_FP("%s line %d: snap: %s", path, lineno, s_err);
        char got[512];
        advisor_core_encode_l1(&s, got, sizeof got);
        const char *want = s_json.n[ln].str;
        total++;
        if (strcmp(got, want) != 0) {
            if (bad < 5) printf("  mismatch, line %d:\n    want: %s\n    got:  %s\n", lineno, want, got);
            bad++;
            continue;
        }
        ok++;
        if (s.pack == SPECIES_GECKO) n_gecko++;
        else if (s.pack == SPECIES_TEST_WARM) n_warm++;
        const char *h = strstr(got, " heat ");
        if (h && h[6] >= '0' && h[6] <= '9') heat_seen[h[6] - '0'] = true;
        stage_seen[s.stage] = shed_seen[s.shed] = last_seen[s.last] = phase_seen[s.phase] = true;   /* in range: want_word */
    }
    fclose(fp);
#undef L1F_FP
    if (total < 300) L1F("%s: %d lines, the fixture has at least 300", path, total);
    if (bad) L1F("%d/%d lines differ from the Python encoder (%s; the first %d above)", bad, total, path, bad < 5 ? bad : 5);
    printf("selftest-l1-encode: %d/%d lines reproduced (pack %d: %d, pack %d: %d)\n",
           ok, total, SPECIES_GECKO, n_gecko, SPECIES_TEST_WARM, n_warm);

    /* 2. what the fixture covers: both packs, every heat digit, every word */
    if (!n_gecko || !n_warm) L1F("the fixture misses a pack (pack %d: %d, pack %d: %d)", SPECIES_GECKO, n_gecko, SPECIES_TEST_WARM, n_warm);
    for (int d = 0; d < 10; d++) if (!heat_seen[d]) L1F("no fixture line has heat %d", d);
    for (int i = 0; i < 4; i++) if (!stage_seen[i]) L1F("no fixture line has stage %s", L1_STAGE_NAMES[i]);
    for (int i = 0; i < 3; i++) if (!shed_seen[i]) L1F("no fixture line has shed %s", L1_SHED_NAMES[i]);
    for (int i = 0; i < LZ_GOALS; i++) if (!last_seen[i]) L1F("no fixture line has last %s", LZ_GOAL_NAMES[i]);
    for (int i = 0; i < PHASE_N; i++) if (!phase_seen[i]) L1F("no fixture line has phase %s", PHASE_NAMES[i]);
    printf("selftest-l1-encode: coverage: both packs, heat 0-9, every stage, shed, last and phase word\n");

    /* 3. a live terrarium: lizard 0 inside the humid hide, stuck in its shed, on the goal that plays bask */
    tank_t *t = &s_tank;
    tank_init(t, 7);
    tank_new_population(t);
    if (t->n_fish < 1) L1F("live: a new population has no lizard");
    fish_t *f = &t->fish[0];
    float hx, hy, bx, by;
    int humid = tank_humid_hide(t);
    if (humid < 0) L1F("live: the habitat has no humid hide");
    tank_hide_point(t, humid, &hx, &hy);
    tank_bask_point(t, &bx, &by);
    f->x = hx; f->y = hy;
    f->shed = SHED_STUCK;
    f->goal.id = GOAL_VISIT_BUBBLES;
    l1_snap_t s;
    advisor_core_l1_snap(t, 0, &s);
    if (!s.in_shelter) L1F("live: at the humid hide's point (%.1f, %.1f) but not in_shelter", hx, hy);
    if (!s.shelter.on || s.shelter.x != hx || s.shelter.y != hy)
        L1F("live: shelter (%.1f, %.1f) is not the humid hide's point (%.1f, %.1f)", s.shelter.x, s.shelter.y, hx, hy);
    if (s.shed != 2) L1F("live: stuck in its shed but shed %d, want 2 (yes)", s.shed);
    if (s.last != LZ_BASK) L1F("live: goal visit_bubbles but last %d, want %d (bask)", s.last, LZ_BASK);
    if (!s.warm.on || s.warm.x != bx || s.warm.y != by)
        L1F("live: warm (%.1f, %.1f) is not the basking spot (%.1f, %.1f)", s.warm.x, s.warm.y, bx, by);
    if (s.pack != t->pack_id) L1F("live: pack %d, the tank's is %d", s.pack, t->pack_id);
    if (t->night) L1F("live: a new terrarium starts with the light off");
    if (s.phase != PHASE_ACTIVE) L1F("live: phase %d with the light on, want %d (active)", s.phase, PHASE_ACTIVE);
    printf("selftest-l1-encode: live snapshot: in the humid hide, shed yes, last bask, warm = the basking spot, pack %d, phase active\n", s.pack);

    char line[512];
    advisor_core_encode_l1(&s, line, sizeof line);
    if (!strstr(line, " shelter near 12 ")) L1F("live line has no ` shelter near 12 `: %s", line);
    if (!strstr(line, " last bask "))       L1F("live line has no ` last bask `: %s", line);
    if (!strstr(line, " shed yes "))        L1F("live line has no ` shed yes `: %s", line);
    printf("selftest-l1-encode: live line: %s\n", line);
    return 0;
}
