/* advisor_core.c — see advisor_core.h. */
#include "advisor_core.h"
#include "word_tok.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static q4_model_t *g_model;
static word_tok_t  g_tok;
static int   g_schema;                /* 2 | 3 | 4 | ADVISOR_SCHEMA_L1 */
static int   g_goal_ids[GOAL_COUNT];  /* token id of each goal word: GOAL_* order (fish), LZ_* order in the first LZ_GOALS (L1) */
static int   g_n_goals = GOAL_COUNT;  /* the candidates in use: GOAL_COUNT (fish), LZ_GOALS (L1) */
static int g_urgency_id = -1;                  /* the literal " urgency" (THE TWO-PASS DECISION) */
bool advisor_core_two_pass = true;
static uint32_t g_rng = 0x9E3779B9u;
static float g_last_p[GOAL_COUNT];
static volatile bool g_damaged;   /* a decision's odds came back not a number: the model is damaged (notice.h) */
static bool not_a_number(float v) { uint32_t u; memcpy(&u, &v, 4); return (u & 0x7f800000u) == 0x7f800000u; }   /* NaN or inf, by the bits (no isfinite: a fast-math build may drop it) */
bool  advisor_core_sample = true;
float advisor_core_temp   = 1.0f;

const char *const L1_STAGE_NAMES[4] = { "hatch", "juv", "adult", "elder" };
const char *const L1_SHED_NAMES[3]  = { "no", "soon", "yes" };

/* ---- schema.md state encoding: must match model/gen_traces.py exactly ---- */
#define SCHEMA_NEAR 70.0f
#define SCHEMA_MID  180.0f
#define SCHEMA_FAR  380.0f

static const char *bucket(float d) {
    if (d < SCHEMA_NEAR) return "near";
    if (d < SCHEMA_MID)  return "mid";
    if (d < SCHEMA_FAR)  return "far";
    return NULL;
}
/* the clock of (ox, oy) seen from (x, y) facing heading: one arithmetic for the fish and the L1 snapshot */
static int clock_at(float x, float y, float heading, float ox, float oy) {
    float rel = atan2f(oy - y, ox - x) - heading;
    while (rel < 0) rel += 6.2831853f;
    int hour = (int)(rel / 6.2831853f * 12.0f + 0.5f) % 12;
    return hour == 0 ? 12 : hour;
}
static int clock_dir(const fish_t *f, float ox, float oy) { return clock_at(f->x, f->y, f->heading, ox, oy); }
static void sighting(const fish_t *f, float ox, float oy, bool present, char *out, size_t n) {
    const char *b = present ? bucket(tank_dist(f->x, f->y, ox, oy)) : NULL;
    if (!b) snprintf(out, n, "none");
    else    snprintf(out, n, "%s %d", b, clock_dir(f, ox, oy));
}
static int drive9(float v) { return v < 0 ? 0 : v > 9 ? 9 : (int)v; }

/* the schema zone 1..6: the 3x2 grid, left to right, top row first */
static int zone_of(const fish_t *f) {
    int col = (int)(f->x / (TANK_W / 3.0f)); if (col > 2) col = 2;
    int row = (int)(f->y / (TANK_BOT / 2.0f)); if (row > 1) row = 1;
    return row * 3 + col + 1;
}
/* wall: the nearest side's distance and its world-frame contact point (*wx, *wy) */
/* (the glass at the fish's own height and column: the frame's edges in the rectangle, the circle in a bowl) */
static float wall_contact(const fish_t *f, float *wx, float *wy) {
    float gx0 = tank_glass_x0(f->y), gx1 = tank_glass_x1(f->y), gy0 = tank_glass_top(f->x);
    float dists[4] = { gx1 - f->x, f->x - gx0, TANK_BOT - f->y, f->y - gy0 };     /* 3,9,6,12 o'clock sides */
    float wxs[4] = { gx1, gx0, f->x, f->x }, wys[4] = { f->y, f->y, TANK_BOT, gy0 };
    int side = 0;
    for (int i = 1; i < 4; i++) if (dists[i] < dists[side]) side = i;
    *wx = wxs[side]; *wy = wys[side];
    return dists[side];
}

void advisor_core_encode(const tank_t *t, int idx, char *out, size_t n) {
    if (g_schema == ADVISOR_SCHEMA_L1) {
        l1_snap_t s;
        advisor_core_l1_snap(t, idx, &s);
        advisor_core_encode_l1(&s, out, n);
        return;
    }
    const fish_t *f = &t->fish[idx];
    int zone = zone_of(f);

    char food_s[24], shadow_s[24], friend_s[40], bubble_s[24], reef_s[24], wall_s[24], frs[24];
    float fd; int fi = tank_nearest_food(t, f, &fd);
    sighting(f, fi >= 0 ? t->food[fi].x : 0, fi >= 0 ? t->food[fi].y : 0, fi >= 0, food_s, sizeof food_s);
    /* the shadow was removed from the game (2026-09-13); the field stays in
     * the frozen schema and always reads `none` (the model was trained on
     * plenty of shadow-none states) */
    snprintf(shadow_s, sizeof shadow_s, "none");
    int fr = tank_nearest_friend(t, idx, NULL);
    sighting(f, fr >= 0 ? t->fish[fr].x : 0, fr >= 0 ? t->fish[fr].y : 0, fr >= 0, frs, sizeof frs);
    if (strcmp(frs, "none") == 0) snprintf(friend_s, sizeof friend_s, "none");
    else if (g_schema >= 3) snprintf(friend_s, sizeof friend_s, "%s", frs);            /* v3: no names */
    else snprintf(friend_s, sizeof friend_s, "%s %s", t->fish[fr].model_name, frs);
    sighting(f, t->bubble_x, t->bubble_y, true, bubble_s, sizeof bubble_s);
    { float rx, ry; tank_reef_spot(t, &rx, &ry);   /* the cluster once the keeper owns one, else the grass corner's spot */
      sighting(f, rx, ry, true, reef_s, sizeof reef_s); }

    /* wall: nearest side, world-frame contact point, heading-relative clock */
    float wx, wy, wd = wall_contact(f, &wx, &wy);
    const char *wb = bucket(wd);
    if (wb && strcmp(wb, "far") != 0)
        snprintf(wall_s, sizeof wall_s, "%s %d", wb, clock_dir(f, wx, wy));
    else snprintf(wall_s, sizeof wall_s, "clear");

    int bold9 = (int)(f->bold * 9.0f + 0.5f), soc9 = (int)(f->sociable * 9.0f + 0.5f);
    if (g_schema >= 4)
        snprintf(out, n,
            "zone %d hunger %d energy %d stress %d curiosity %d bold %d social %d stage %s trust %d bored %d "
            "food %s friend %s bubble %s reef %s wall %s last %s time %s",
            zone, drive9(f->hunger), drive9(f->energy), drive9(f->stress), drive9(f->curiosity),
            bold9, soc9, STAGE_NAMES[f->stage], drive9(f->trust), drive9(f->bored),
            food_s, friend_s, bubble_s, reef_s, wall_s,
            GOAL_NAMES[f->goal.id], t->night ? "night" : "day");
    else if (g_schema >= 3)
        snprintf(out, n,
            "zone %d hunger %d energy %d stress %d curiosity %d bold %d social %d stage %s trust %d "
            "food %s shadow %s friend %s bubble %s reef %s wall %s last %s time %s",
            zone, drive9(f->hunger), drive9(f->energy), drive9(f->stress), drive9(f->curiosity),
            bold9, soc9, STAGE_NAMES[f->stage], drive9(f->trust),
            food_s, shadow_s, friend_s, bubble_s, reef_s, wall_s,
            GOAL_NAMES[f->goal.id], t->night ? "night" : "day");
    else
        snprintf(out, n,
            "fish %s zone %d hunger %d energy %d stress %d curiosity %d bold %d social %d stage %s "
            "food %s shadow %s friend %s bubble %s reef %s wall %s last %s time %s",
            f->model_name, zone,
            drive9(f->hunger), drive9(f->energy), drive9(f->stress), drive9(f->curiosity),
            bold9, soc9, STAGE_NAMES[f->stage],
            food_s, shadow_s, friend_s, bubble_s, reef_s, wall_s,
            GOAL_NAMES[f->goal.id], t->night ? "night" : "day");
}

/* ---- schema L1 (model/schema-lizard.md): must match model/gen_traces.py encode_l1_snap exactly ---- */
static float pt_dist(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}
static void l1_sight(const l1_snap_t *s, l1_point_t p, char *out, size_t n) {
    const char *b = p.on ? bucket(pt_dist(s->x, s->y, p.x, p.y)) : NULL;
    if (!b) snprintf(out, n, "none");
    else    snprintf(out, n, "%s %d", b, clock_at(s->x, s->y, s->heading, p.x, p.y));
}
static int trait9(float v) { return (int)(v * 9.0f + 0.5f); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void advisor_core_encode_l1(const l1_snap_t *s, char *out, size_t n) {
    const species_t *sp = species_get(s->pack);
    if (!sp) sp = species_get(SPECIES_DEFAULT);
    int heat = sp ? drive9(species_heat_of(sp, s->body_c)) : 0;
    char prey_s[24], other_s[24], warm_s[24], shelter_s[24], wall_s[24];
    l1_sight(s, s->prey, prey_s, sizeof prey_s);
    l1_sight(s, s->other, other_s, sizeof other_s);
    l1_sight(s, s->warm, warm_s, sizeof warm_s);
    if (s->in_shelter) snprintf(shelter_s, sizeof shelter_s, "near 12");   /* inside one: whatever the point */
    else l1_sight(s, s->shelter, shelter_s, sizeof shelter_s);
    snprintf(wall_s, sizeof wall_s, "clear");
    if (s->wall.on) {                                                       /* shown only within mid */
        const char *wb = bucket(pt_dist(s->x, s->y, s->wall.x, s->wall.y));
        if (wb && strcmp(wb, "far") != 0)
            snprintf(wall_s, sizeof wall_s, "%s %d", wb, clock_at(s->x, s->y, s->heading, s->wall.x, s->wall.y));
    }
    int last = s->last >= 0 && s->last < LZ_GOALS ? s->last : LZ_EXPLORE;  /* out of range: never an index past the table */
    snprintf(out, n,
        "zone %d hunger %d energy %d stress %d curiosity %d heat %d bold %d social %d stage %s trust %d bored %d "
        "shed %s prey %s other %s warm %s shelter %s wall %s last %s phase %s",
        s->zone, drive9(s->hunger), drive9(s->energy), drive9(s->stress), drive9(s->curiosity), heat,
        trait9(s->bold), trait9(s->social), L1_STAGE_NAMES[clampi(s->stage, 0, 3)], drive9(s->trust), drive9(s->bored),
        L1_SHED_NAMES[clampi(s->shed, 0, 2)], prey_s, other_s, warm_s, shelter_s, wall_s,
        LZ_GOAL_NAMES[last], PHASE_NAMES[clampi(s->phase, 0, PHASE_N - 1)]);
}

int advisor_core_l1_goal_of(goal_id_t g) {
    switch (g) {
    case GOAL_SEEK_FOOD:     return LZ_HUNT;
    case GOAL_VISIT_BUBBLES: return LZ_BASK;
    case GOAL_INSPECT_REEF:  return LZ_HIDE;
    case GOAL_FOLLOW_FRIEND: return LZ_INVESTIGATE;
    case GOAL_EXPLORE:       return LZ_EXPLORE;
    case GOAL_REST:          return LZ_REST;
    case GOAL_DART_PLAY:     return LZ_DIG;
    case GOAL_FLEE_SHADOW:   return LZ_HIDE;
    default:                 return LZ_EXPLORE;
    }
}
goal_id_t advisor_core_fish_goal_of(int lz) {
    switch (lz) {
    case LZ_HUNT:        return GOAL_SEEK_FOOD;
    case LZ_BASK:        return GOAL_VISIT_BUBBLES;
    case LZ_HIDE:        return GOAL_INSPECT_REEF;
    case LZ_EXPLORE:     return GOAL_EXPLORE;
    case LZ_INVESTIGATE: return GOAL_FOLLOW_FRIEND;
    case LZ_DIG:         return GOAL_DART_PLAY;
    case LZ_REST:        return GOAL_REST;
    default:             return GOAL_COUNT;
    }
}

void advisor_core_l1_snap(const tank_t *t, int idx, l1_snap_t *s) {
    const fish_t *f = &t->fish[idx];
    memset(s, 0, sizeof *s);
    s->pack = t->pack_id;
    s->zone = zone_of(f);
    s->x = f->x; s->y = f->y; s->heading = f->heading;
    s->body_c = f->body_c;
    s->hunger = f->hunger; s->energy = f->energy; s->stress = f->stress; s->curiosity = f->curiosity;
    s->trust = f->trust; s->bored = f->bored;
    s->bold = f->bold; s->social = f->sociable;
    s->stage = (int)f->stage;
    s->shed = f->shed == SHED_SOON ? 1 : (f->shed == SHED_SHEDDING || f->shed == SHED_STUCK) ? 2 : 0;
    s->last = advisor_core_l1_goal_of(f->goal.id);
    s->phase = tank_activity_phase(t);
    int fi = tank_nearest_food(t, f, NULL);
    if (fi >= 0) { s->prey.on = true; s->prey.x = t->food[fi].x; s->prey.y = t->food[fi].y; }
    int fr = tank_nearest_friend(t, idx, NULL);
    if (fr >= 0) { s->other.on = true; s->other.x = t->fish[fr].x; s->other.y = t->fish[fr].y; }
    s->warm.on = true; tank_bask_point(t, &s->warm.x, &s->warm.y);
    int h = tank_nearest_hide(t, f->x, f->y, NULL);
    if (h >= 0) { s->shelter.on = true; tank_hide_point(t, h, &s->shelter.x, &s->shelter.y); }
    s->in_shelter = tank_hide_at(t, f->x, f->y) >= 0;
    s->wall.on = true; wall_contact(f, &s->wall.x, &s->wall.y);   /* the encoder decides near/mid vs clear */
}

bool advisor_core_init(const uint8_t *model_bin, size_t model_len,
                       const uint8_t *tok_bin, size_t tok_len,
                       void *(*alloc)(size_t), uint32_t seed) {
    g_model = q4_model_open(model_bin, model_len, alloc);
    if (!g_model) return false;
    if (word_tok_init(&g_tok, tok_bin, tok_len, q4_model_config(g_model)->vocab_size) != 0) return false;
    g_schema = 2;
    for (int i = 0; i < GOAL_COUNT; i++) g_goal_ids[i] = -1;
    for (int id = 0; id < g_tok.vocab_size; id++) {
        const char *p = word_tok_piece(&g_tok, id);
        if (strcmp(p, " trust") == 0 && g_schema < 3) g_schema = 3;
        if (strcmp(p, " bored") == 0 && g_schema < 4) g_schema = 4;      /* v4: no shadow, + bored */
        if (strcmp(p, " heat") == 0) g_schema = ADVISOR_SCHEMA_L1;        /* the lizard's (it has trust and bored too) */
        if (strcmp(p, " urgency") == 0) g_urgency_id = id;
    }
    /* the goal words: the lizard's seven (LZ_* order) for L1, the fish's GOAL_NAMES otherwise */
    bool l1 = g_schema == ADVISOR_SCHEMA_L1;
    g_n_goals = l1 ? LZ_GOALS : GOAL_COUNT;
    for (int id = 0; id < g_tok.vocab_size; id++) {
        const char *p = word_tok_piece(&g_tok, id);
        for (int g = 0; g < g_n_goals; g++)
            if (p[0] == ' ' && strcmp(p + 1, l1 ? LZ_GOAL_NAMES[g] : GOAL_NAMES[g]) == 0) g_goal_ids[g] = id;
    }
    for (int g = 0; g < g_n_goals; g++) if (g_goal_ids[g] < 0) return false;   /* not our vocab */
    if (seed) g_rng = seed;
    return true;
}
int advisor_core_schema(void) { return g_model ? g_schema : 0; }
q4_model_t *advisor_core_model(void) { return g_model; }
const q4_config_t *advisor_core_config(void) { return g_model ? q4_model_config(g_model) : NULL; }
const float *advisor_core_last_probs(void) { return g_last_p; }
bool advisor_core_damaged(void) { return g_damaged; }

goal_t advisor_core_infer(const char *state, int *tokens_out) {
    goal_t g = { GOAL_COUNT, 5, 1.0f, GOAL_COUNT };   /* GOAL_COUNT = "no change" */
    if (!g_model) return g;
    char prompt[400];
    snprintf(prompt, sizeof prompt, "%s ->", state);
    int toks[64]; int n = word_tok_encode(&g_tok, prompt, toks, 60);
    int out[8];
    float temp = advisor_core_sample ? advisor_core_temp : 0;
    /* max_out 3 = goal + " urgency" + digit: everything the parser reads;
       each extra token would cost a full weight pass */
    /* THE TWO-PASS DECISION: the goal and the literal " urgency" that follows it go as one
       batched pass, the digit read from it (q4_model.c); the old three passes otherwise */
    bool two = advisor_core_two_pass && g_urgency_id >= 0;
    bool l1 = g_schema == ADVISOR_SCHEMA_L1;
    float p_lz[LZ_GOALS];   /* L1: the seven lizard goals' odds, LZ_* order (g_last_p stays in the fish's order) */
    int nout = q4_model_decide_follow(g_model, toks, n, g_goal_ids, g_n_goals, temp, &g_rng,
                                      l1 ? p_lz : g_last_p, out, 3, two ? &g_urgency_id : NULL, two ? 1 : 0);
    if (tokens_out) *tokens_out = n + nout;
    /* first token is the goal (one of the candidates); then "urgency <d>" */
    int k = -1;
    if (nout >= 1)
        for (int i = 0; i < g_n_goals; i++) if (out[0] == g_goal_ids[i]) { k = i; break; }
    if (l1) {
        /* THE TEMPORARY GOAL BRIDGE: the lizard's answer as the fish goal the reflex performs; its odds into
           the fish's order, 0 for the goal no lizard goal plays (flee_shadow). nout 0: no odds came back */
        if (nout >= 1) {
            for (int i = 0; i < LZ_GOALS; i++) if (not_a_number(p_lz[i])) { g_damaged = true; break; }
            for (int i = 0; i < GOAL_COUNT; i++) g_last_p[i] = 0;
            for (int i = 0; i < LZ_GOALS; i++) g_last_p[advisor_core_fish_goal_of(i)] = p_lz[i];
        }
        if (k >= 0) g.id = advisor_core_fish_goal_of(k);
    } else {
        if (k >= 0) g.id = (goal_id_t)k;
        for (int i = 0; i < GOAL_COUNT; i++) if (not_a_number(g_last_p[i])) { g_damaged = true; break; }
    }
    if (g.id < GOAL_COUNT) {
        g.confidence = g_last_p[g.id];
        int ru = -1;
        for (int i = 0; i < GOAL_COUNT; i++) {
            if (l1 && advisor_core_fish_goal_of(advisor_core_l1_goal_of((goal_id_t)i)) != (goal_id_t)i) continue;   /* no lizard goal plays it */
            if (i != (int)g.id && (ru < 0 || g_last_p[i] > g_last_p[ru])) ru = i;
        }
        g.runner_up = ru >= 0 ? (goal_id_t)ru : GOAL_COUNT;
    }
    for (int i = 0; i + 1 < nout; i++) {
        const char *p = word_tok_piece(&g_tok, out[i]);
        if (strcmp(p, " urgency") == 0) {
            const char *d = word_tok_piece(&g_tok, out[i + 1]);
            if (d[0] == ' ' && d[1] >= '0' && d[1] <= '9' && d[2] == 0) g.urgency = (float)(d[1] - '0');
            break;
        }
    }
    return g;
}
