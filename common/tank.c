/* tank.c — reflex layer. See tank.h. Faithful port of the browser prototype's
 * updateFish/targetForGoal/wall handling, with prototype px values scaled by
 * ~0.55 for the 448-wide tank. */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/aquarium/aquarium_priv.h"      /* (phase 3, step 1: the modules split out of this file; step 2 calls them through their ops tables) */
#include "worlds/terrarium/terrarium_priv.h"
#include "pt_util.h"
#include "packs/compat.h"                 /* packs_compatible: tank_set_packs */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.2831853f

const char *const GOAL_NAMES[GOAL_COUNT] = {
    "seek_food", "flee_shadow", "visit_bubbles", "follow_friend",                     /* the fish's (schema v2-v4) */
    "explore", "rest", "dart_play", "inspect_reef",
    "hunt", "bask", "hide", "explore", "investigate", "dig", "rest",                  /* the lizard's (L1: packs/ptpk_write.c LZ_GOAL_NAMES) */
};
const char *const LZ_POSE_NAMES[LZ_POSES] = {
    "stand", "walk", "turn", "flick", "stalk", "lunge", "bask", "dig", "rest", "hidden", "peek", "standoff",
};

/* ---- the event bus (tank_events.h): one listener, synchronous ---- */
const char *const TANK_EVENT_NAMES[TEV_COUNT] = {
    "tap", "feed", "light_on", "light_off", "wipe", "snip", "eat", "spook", "investigate", "bubbles",
    "welcome", "wheel_tick", "confirm", "crab_find", "crab_coin",
};
static tank_event_fn s_ev_fn; static void *s_ev_ud;
void tank_events_set(tank_event_fn fn, void *ud) { s_ev_fn = fn; s_ev_ud = ud; }
void tank_emit(int ev, int fish) { if (s_ev_fn) s_ev_fn(ev, fish, s_ev_ud); }

const char *const STAGE_NAMES[4] = { "fry", "juv", "adult", "elder" };
const char *const TRAINED_NAMES[N_TRAINED_NAMES] = { "mira", "bolt", "kelp", "nori" };

int tank_reflex_overrides = 0;   /* starvation-ignored episodes (diagnostic) */

/* deterministic xorshift32 */
uint32_t xr(tank_t *t) {
    uint32_t x = t->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return t->rng = x;
}
float tank_randf(tank_t *t, float lo, float hi) {
    return lo + (hi - lo) * (float)(xr(t) & 0xffffff) / 16777215.0f;
}

/* identity (0.4): the platform's entropy when it has some; else a splitmix of
 * the seed-derived state, the clock and a count - never t->rng itself */
uint32_t (*tank_entropy)(void) = NULL;
uint32_t tank_new_uid(tank_t *t) {
    for (;;) {
        uint32_t z;
        if (tank_entropy) z = tank_entropy();
        else {
            uint64_t x = ((uint64_t)t->rng << 32) ^ ((uint64_t)++t->uid_n * 0x9E3779B97F4A7C15ull) ^ (uint64_t)(t->clock * 1000.0);
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            z = (uint32_t)(x ^ (x >> 31));
        }
        if (!z) continue;
        bool taken = z == t->uid;
        for (int i = 0; i < N_FISH_MAX && !taken; i++) taken = t->fish[i].uid == z;
        if (!taken) return z;
    }
}
void tank_ensure_ids(tank_t *t) {
    if (!t->uid) t->uid = tank_new_uid(t);
    for (int i = 0; i < t->n_fish; i++) if (!t->fish[i].uid) t->fish[i].uid = tank_new_uid(t);
    for (int i = 0; i < t->n_fish; i++) {
        fish_t *f = &t->fish[i];
        if (!f->parent_uid[0] && f->parent_a >= 0 && f->parent_a < t->n_fish) f->parent_uid[0] = t->fish[f->parent_a].uid;
        if (!f->parent_uid[1] && f->parent_b >= 0 && f->parent_b < t->n_fish) f->parent_uid[1] = t->fish[f->parent_b].uid;
    }
}

float norm_ang(float a) {
    if (!(fabsf(a) < 64.0f)) a = remainderf(a, TAU);   /* never a spin: a huge angle (a bug elsewhere) is folded, inf becomes NaN */
    while (a >  3.14159265f) a -= TAU;
    while (a < -3.14159265f) a += TAU;
    return a;
}

float tank_dist(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

/* ---- the glass (tank.h). The rectangle's is its frame; the bowl's is the
 * circle, with the flat bottom at TANK_BOT. ---- */
#ifdef TANK_ROUND
static float glass_half(float d) {                 /* half the chord, d px off the centre line */
    float r2 = TANK_RAD * TANK_RAD - d * d;
    return r2 > 0 ? sqrtf(r2) : 0;
}
float tank_glass_x0(float y) { return TANK_RAD - glass_half(y - TANK_RAD); }
float tank_glass_x1(float y) { return TANK_RAD + glass_half(y - TANK_RAD); }
float tank_glass_top(float x) { return TANK_RAD - glass_half(x - TANK_RAD); }
void tank_glass_clamp(float *x, float *y, float m) {
    if (*y > TANK_BOT - m) *y = TANK_BOT - m;
    float dx = *x - TANK_RAD, dy = *y - TANK_RAD, d = sqrtf(dx * dx + dy * dy), lim = TANK_RAD - m;
    if (d > lim && d > 0) {
        *x = TANK_RAD + dx * lim / d; *y = TANK_RAD + dy * lim / d;
        if (*y > TANK_BOT - m) {                   /* the corner where the glass meets the floor: along the floor to the glass */
            *y = TANK_BOT - m;
            float h = glass_half(*y - TANK_RAD) - m; if (h < 0) h = 0;
            *x = dx < 0 ? TANK_RAD - h : TANK_RAD + h;
        }
    }
}
#elif defined(TANK_CORNER_R)
/* the watch: the rectangle's frame, and in each corner's R x R square the
 * arc. corner_in says how far a point stands off its corner's centre (0 = it
 * is in no corner's square: the straight glass is the limit there). */
float corner_in(float x, float y, float *cx, float *cy) {
    const float R = TANK_CORNER_R;
    if (x < R) *cx = R; else if (x > TANK_W - R) *cx = TANK_W - R; else return 0;
    if (y < R) *cy = R; else if (y > TANK_H - R) *cy = TANK_H - R; else return 0;
    return sqrtf((x - *cx) * (x - *cx) + (y - *cy) * (y - *cy));
}
static float corner_inset(float v, float len) {    /* how far the glass stands in from the frame, v px along a side of length len */
    const float R = TANK_CORNER_R;
    float d = v < R ? R - v : v > len - R ? v - (len - R) : 0;
    if (d <= 0) return 0;
    return d >= R ? R : R - sqrtf(R * R - d * d);
}
float tank_glass_x0(float y) { return corner_inset(y, TANK_H); }
float tank_glass_x1(float y) { return TANK_W - corner_inset(y, TANK_H); }
float tank_glass_top(float x) { return corner_inset(x, TANK_W); }
void corner_clamp(float *x, float *y, float m) {   /* out of a corner: back along its radius to m inside the arc */
    float cx, cy, d = corner_in(*x, *y, &cx, &cy), lim = TANK_CORNER_R - m;
    if (d > lim && d > 0) { *x = cx + (*x - cx) * lim / d; *y = cy + (*y - cy) * lim / d; }
}
void tank_glass_clamp(float *x, float *y, float m) {
    *x = clampf(*x, m, TANK_W - m); *y = clampf(*y, m, TANK_H - m);
    corner_clamp(x, y, m);
}
#else
float tank_glass_x0(float y) { (void)y; return 0; }
float tank_glass_x1(float y) { (void)y; return TANK_W; }
float tank_glass_top(float x) { (void)x; return 0; }
void tank_glass_clamp(float *x, float *y, float m) {
    *x = clampf(*x, m, TANK_W - m); *y = clampf(*y, m, TANK_H - m);
}
#endif

int tank_nearest_friend(const tank_t *t, int fish_idx, float *dist_out) {
    const fish_t *f = &t->fish[fish_idx];
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < N_FISH_SLOTS; i++) {             /* (visitors are friends too; a fish next door is not here) */
        if (i == fish_idx || !tank_swims(t, i)) continue;
        float d = tank_dist(f->x, f->y, t->fish[i].x, t->fish[i].y);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist_out) *dist_out = bd;
    return best;
}

/* ---- roster: six presets (the prototype's four + two), colors chosen for
 * contrast on AMOLED black. A preset is a look + temperament; bold/social are
 * rolled per tank (starting pair) or inherited (arrivals). ---- */
typedef struct {
    const char *name; uint32_t color, fin, accent;
    float size, curiosity, lazy, turn_rate;
} preset_t;
static const preset_t ROSTER[] = {
    /* name    color     fin       accent    size  cur  lazy  turn */
    { "mira", 0x38dcc7, 0x1d9f98, 0xffd166, 1.08f, 7.5f, 0.2f, 3.2f },   /* (markings orange ffbd59 / cream ffe08a until 0.4's one palette) */
    { "bolt", 0xff725c, 0xb83a43, 0xe8f1f2, 0.94f, 6.0f, 0.2f, 4.2f },
    { "kelp", 0x78d67d, 0x3f8b55, 0xa799ff, 0.86f, 4.0f, 0.1f, 3.2f },
    { "nori", 0xa799ff, 0x6b5ed6, 0x78d67d, 1.00f, 5.0f, 0.7f, 2.4f },
    { "pip",  0xffd166, 0xc98a1e, 0x38dcc7, 0.90f, 6.5f, 0.3f, 3.8f },
    { "sol",  0xf48fb1, 0xb0456f, 0xe8f1f2, 1.04f, 5.5f, 0.4f, 2.9f },
};
#define ROSTER_N ((int)(sizeof ROSTER / sizeof ROSTER[0]))
int tank_roster_count(void) { return ROSTER_N; }
const char *tank_roster_name(int preset) { return preset >= 0 && preset < ROSTER_N ? ROSTER[preset].name : "?"; }
float tank_roster_size(int preset) { return preset >= 0 && preset < ROSTER_N ? ROSTER[preset].size : 1.0f; }
float tank_roster_turn(int preset) { return preset >= 0 && preset < ROSTER_N ? ROSTER[preset].turn_rate : ROSTER[0].turn_rate; }

/* the keeper's palette (setup.c; one for the body and the markings since
 * 0.4): the six roster bodies, blue, silver, and charcoal (Strato: black
 * fish). Until 0.4 the markings had their own eight - orange, cream, white,
 * the stress red, a dark ink among them; silver and charcoal stand in for
 * white and ink, the roster's orange / cream markings became gold / silver. */
const uint32_t LOOK_COLORS[LOOK_N] = { 0x38dcc7, 0xff725c, 0x78d67d, 0xa799ff, 0xffd166, 0xf48fb1, 0x4da3ff, 0xe8f1f2, 0x23262c };
/* the coral's swatches (2026-09-23): the reference art's orange first, then
 * reef colours - pink, magenta, violet, blue, teal, green, gold */
const uint32_t CORAL_PAL[CORAL_N] = { 0xff7a1e, 0xf2698f, 0xd24bb4, 0x8a5be0, 0x3f8ff0, 0x2ec9b4, 0x7fc64a, 0xf2c23a };
/* the reef cluster's three looks (2026-09-24): REEF is the art's - an
 * orange coral, purple tubes, a cyan brain; LAGOON pink / blue / lime;
 * DUSK magenta / teal / gold */
const cluster_scheme_t CLUSTER_SCHEMES[CLUSTER_SCHEME_N] = {
    { "REEF",   0xff6a2a, 0x9b4fe0, 0x5fd8e8 },
    { "LAGOON", 0xf25c8a, 0x3f8ff0, 0xb9f04a },
    { "DUSK",   0xd24bb4, 0x2ec9b4, 0xf2c23a },
};

const char *const SHAPE_NAMES[SHAPE_N]     = { "CLASSIC", "SLENDER", "BULBOUS" };
const char *const FINS_NAMES[FINS_N]       = { "CLASSIC", "VEIL", "SAIL" };
const char *const PATTERN_NAMES[PATTERN_N] = { "BARS", "MASK", "EYE BAND", "SOLID" };
const char *const GENE_NAMES[GENE_N]       = { "SHAPE", "FINS", "PATTERN", "BODY", "MARKINGS" };
int tank_gene_values(int g) { return g == GENE_SHAPE ? SHAPE_N : g == GENE_FINS ? FINS_N : g == GENE_PATTERN ? PATTERN_N : LOOK_N; }

/* heredity's stream: xorshift like rng, its own state (tank_t.gene_rng) */
static uint32_t gene_xr(tank_t *t) {
    uint32_t x = t->gene_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return t->gene_rng = x;
}
static float gene_f(tank_t *t) { return (float)(gene_xr(t) & 0xffffff) / 16777216.0f; }
static int   gene_i(tank_t *t, int n) { return (int)(gene_xr(t) % (uint32_t)n); }
/* a colour's palette index: exact, or the nearest swatch */
static int look_index(const uint32_t *pal, uint32_t rgb) {
    int best = 0; long bd = -1;
    for (int i = 0; i < LOOK_N; i++) {
        long dr = (long)(pal[i] >> 16 & 255) - (rgb >> 16 & 255), dg = (long)(pal[i] >> 8 & 255) - (rgb >> 8 & 255),
             db = (long)(pal[i] & 255) - (rgb & 255), d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

void tank_set_name(tank_t *t, int slot, const char *name) {
    if (slot < 0 || slot >= N_FISH_MAX) return;
    fish_t *f = &t->fish[slot];
    if (!name || !*name) name = tank_roster_name(f->preset);
    int n = 0;
    /* canonical lowercase (2026-09-15): the roster, the director and the
     * trained names are lowercase, the display uppercases at draw, and the
     * wheel used to write capitals into the slots it spun ("FeZ", "LArRY") */
    while (name[n] && n < FISH_NAME_MAX) {
        char c = name[n];
        f->name[n] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        n++;
    }
    f->name[n] = 0;
}
static uint32_t fin_for(uint32_t body) {
    for (int i = 0; i < ROSTER_N; i++) if (ROSTER[i].color == body) return ROSTER[i].fin;
    uint32_t r = body >> 16 & 255, g = body >> 8 & 255, b = body & 255;
    if (r * 299 + g * 587 + b * 114 < 70 * 1000) {       /* a dark body (charcoal): lighter fins, or they vanish in the water */
        r += (255 - r) * 18 / 100; g += (255 - g) * 18 / 100; b += (255 - b) * 18 / 100;
    } else { r = r * 58 / 100; g = g * 58 / 100; b = b * 58 / 100; }   /* the keeper's own colour: the fin at 58% */
    return (r << 16) | (g << 8) | b;
}
uint32_t tank_fin_for(uint32_t body) { return fin_for(body); }
void tank_set_look(tank_t *t, int slot, uint32_t body, uint32_t accent) {
    if (slot < 0 || slot >= N_FISH_MAX) return;
    fish_t *f = &t->fish[slot];
    if (body)   { f->color = body; f->fin = fin_for(body); }
    if (accent) f->accent = accent;
    if (f->accent == f->color)                        /* stripes the body's own colour would vanish */
        for (int i = 0; i < LOOK_N; i++) if (LOOK_ACCENT[i] != f->color) { f->accent = LOOK_ACCENT[i]; break; }
}

float tank_form_turn(const fish_t *f) {
    float k = f->shape == SHAPE_SLENDER ? TURN_SLENDER : f->shape == SHAPE_BULBOUS ? TURN_BULBOUS : 1.0f;
    return f->fins == FINS_VEIL ? k * TURN_VEIL : k;     /* SAIL: as CLASSIC (the sail leans into the turn on the glass) */
}

int tank_gene(const fish_t *f, int g) {
    switch (g) {
    case GENE_SHAPE:   return f->shape;
    case GENE_FINS:    return f->fins;
    case GENE_PATTERN: return f->pattern;
    case GENE_BODY:    return look_index(LOOK_BODY, f->color);
    default:           return look_index(LOOK_ACCENT, f->accent);
    }
}
void tank_set_gene(tank_t *t, int slot, int g, int v) {
    if (slot < 0 || slot >= N_FISH_MAX || v < 0 || v >= tank_gene_values(g)) return;
    fish_t *f = &t->fish[slot];
    switch (g) {
    case GENE_SHAPE:   f->shape = (uint8_t)v; break;
    case GENE_FINS:    f->fins = (uint8_t)v; break;
    case GENE_PATTERN: f->pattern = (uint8_t)v; break;
    case GENE_BODY:    tank_set_look(t, slot, LOOK_BODY[v], 0); break;
    default:           tank_set_look(t, slot, 0, LOOK_ACCENT[v]); break;
    }
}
void tank_roll_carried(tank_t *t, int slot) {
    if (slot < 0 || slot >= N_FISH_MAX) return;
    for (int g = 0; g < GENE_N; g++) t->fish[slot].carried[g] = (uint8_t)gene_i(t, tank_gene_values(g));
}

/* the trail (0.4): how much of the fish streams behind its body - a long
 * low body and long fins trail most, a classic not at all (its springs are
 * stiff: today's fish, to the eye). A young fish wears the plain tail */
float tank_form_trail(const fish_t *f) {
    float k = f->shape == SHAPE_SLENDER ? 0.35f : f->shape == SHAPE_BULBOUS ? 0.1f : 0;
    if (f->stage >= STAGE_ADULT) {
        k += f->fins == FINS_VEIL ? 0.55f : f->fins == FINS_SAIL ? 0.35f : 0;
        if (f->stage == STAGE_ELDER) k += 0.1f;              /* the elder's longer tail */
    }
    return k > 1 ? 1 : k;
}

/* the body's pitch as render.c folds it: the climb or dive, facing either way */
static float fish_pitch(const fish_t *f) { return atan2f(sinf(f->heading), fabsf(cosf(f->heading))); }

/* a spring, x toward `to` at w rad/s, z of critical damping (below 1 it
 * swings a touch past and settles: follow-through) */
static void spring(float *x, float *v, float to, float w, float z, float dt) {
    *v += (w * w * (to - *x) - 2 * z * w * *v) * dt;
    *x += *v * dt;
}

bool tank_trail_off;
void tank_fish_trail(fish_t *f, float dt) {
    float tr = tank_form_trail(f), q = fish_pitch(f);
    if (tank_trail_off) {                               /* as before the trail: rigid, the tail's yaw a tenth of a second behind */
        f->pitch_b = f->pitch_t = q; f->pitch_bv = f->pitch_tv = f->yaw_tv = 0; f->speed_t = f->speed;
        f->yaw_tail += (f->yaw - f->yaw_tail) * fminf(1, fmaxf(0, dt * 10));
        return;
    }
    if (dt > 0.25f || dt <= 0) {                        /* a long gap (a wake, a load): no swing to carry over */
        f->pitch_b = f->pitch_t = q; f->pitch_bv = f->pitch_tv = f->yaw_tv = 0;
        f->yaw_tail = f->yaw; f->speed_t = f->speed;
        return;
    }
    /* the whole body has weight: its pitch follows the heading's a moment
     * late (never the glass's snap); the tail follows the body's, later and
     * looser the longer it is; the tail's yaw likewise */
    float wb = 30 - 10 * tr, wt = 30 - 21 * tr, zt = 0.95f - 0.35f * tr;
    float wy = 20 - 13 * tr, zy = 1.0f - 0.35f * tr;
    int n = (int)(dt * 120) + 1; float h = dt / n;       /* steps of 1/120 s or less: stiff springs stay calm at 12 fps */
    for (int i = 0; i < n; i++) {
        spring(&f->pitch_b, &f->pitch_bv, q, wb, 1.0f, h);
        spring(&f->pitch_t, &f->pitch_tv, f->pitch_b, wt, zt, h);
        spring(&f->yaw_tail, &f->yaw_tv, f->yaw, wy, zy, h);
    }
    if (f->yaw_tail > 1.15f)  { f->yaw_tail = 1.15f;  f->yaw_tv = 0; }   /* past side-on: the tail sweeps a little toward the back glass */
    if (f->yaw_tail < -1.15f) { f->yaw_tail = -1.15f; f->yaw_tv = 0; }
    f->speed_t += (f->speed - f->speed_t) * fminf(1, dt * (4 - 2 * tr));
}

/* the clock's float face (tank.h tank_anim), worked out once a tick: the fish read it several times each, and the S3
 * does a double's fmod in software */
static double s_anim_of = -1; static float s_anim;
float anim(const tank_t *t) { if (t->clock != s_anim_of) { s_anim = tank_anim(t->clock); s_anim_of = t->clock; } return s_anim; }

/* the tail beat (0.4): its phase runs on at the beat's rate a frame at a time (fish_t.beat). Drawn from the clock
 * x the rate, as it was, any change of speed slid the phase by the clock x the change: at an overnight wake's
 * 28,800 s a burst thrashed the tail for a second, and a float clock past 2^20 s froze it */
float tank_fish_beat_rate(const fish_t *f) {
    float form = f->shape == SHAPE_SLENDER ? BEAT_SLENDER : f->shape == SHAPE_BULBOUS ? BEAT_BULBOUS : 1.0f;
    return (8 + f->speed * 0.055f) * form * (f->fins == FINS_VEIL ? BEAT_VEIL : 1.0f);
}
void fish_beat(fish_t *f, float dt) {
    if (dt > 0 && (f->beat += dt * tank_fish_beat_rate(f)) >= 6.2831853f) f->beat = fmodf(f->beat, 6.2831853f);   /* (a lap: fmodf only then) */
}

void tank_fish_face(fish_t *f) {
    f->facing = cosf(f->heading) < 0 ? -1 : 1;
    f->yaw = f->yaw_tail = f->facing; f->behind = 0;
    f->pitch_b = f->pitch_t = fish_pitch(f); f->pitch_bv = f->pitch_tv = f->yaw_tv = 0; f->speed_t = f->speed;
}

void tank_make_fish(tank_t *t, int slot, int preset, float sociable, float bold, stage_t stage) {
    fish_t *f = &t->fish[slot];
    const preset_t *p = &ROSTER[preset];
    f->preset = preset; tank_set_name(t, slot, p->name);
    f->model_name = TRAINED_NAMES[slot % N_TRAINED_NAMES];   /* names carry no signal */
    f->x = tank_randf(t, TANK_FX0 + 90, TANK_FX1 - 90); f->y = tank_randf(t, 80, TANK_BOT - 90);
    f->heading = tank_randf(t, 0, TAU); tank_fish_face(f);
    f->speed = 0; f->target_speed = 0; f->wander = f->x * 0.05f;
    f->base_size = p->size; f->size = p->size; f->turn_rate = p->turn_rate;
    f->hunger = tank_randf(t, 3, 6); f->energy = tank_randf(t, 5, 8);
    f->stress = tank_randf(t, 0, 2); f->curiosity = p->curiosity;
    f->sociable = sociable; f->bold = bold; f->lazy = p->lazy;
    f->bold0 = bold; f->sociable0 = sociable; f->drift_acc = 0;
    f->stage = stage;
    f->starve_flagged = false;
    f->trust = 5.0f;
    f->goal.id = GOAL_W_EXPLORE; f->goal.urgency = 3; f->goal.confidence = 1; f->goal.runner_up = GOAL_COUNT;
    f->goal_age = 0; f->ask_age = 99; f->dart_timer = 0; f->dart_x = f->x; f->dart_y = f->y; f->hesitate = 0;
    f->bored = 0; f->goal_prev = GOAL_COUNT; f->zone_last = -1; f->explore_set = false; f->breather = 0;
    f->gdepth = FISH_DEPTH_FRONT; f->gdepth_want = 0xff; f->gdepth_rest = FISH_DEPTH_BEHIND; f->pdepth = FISH_DEPTH_FRONT; f->pdepth_want = 0xff;
    /* its own order of first visits - from the slot, not the tank's RNG, so a
     * seeded run (the selftests) draws the same numbers it did before */
    for (int z = 0; z < 6; z++) f->zone_seen[z] = -(float)((slot * 37 + z * 13) % 60);
    f->eaten = 0; f->eaten_player = 0; f->at_bubbles = false; f->meal_due = false; f->meal_pay_wait = 0;
    /* its own spot by the reef: bolder fish rest a little further out */
    f->rest_dx = 8 + slot * 24 + bold * 14; f->rest_dy = -slot * 9 - tank_randf(t, 0, 10);   /* a body apart (was 9 px per slot) */
    f->sig = 0xffffffffu; f->ms_bits = 0; f->ms_seen = 0;
    f->color = p->color; f->fin = p->fin; f->accent = p->accent;
    f->parent_a = f->parent_b = -1;
    f->shape = SHAPE_CLASSIC; f->fins = FINS_CLASSIC; f->pattern = PATTERN_BARS;
    for (int g = 0; g < GENE_N; g++) f->carried[g] = (uint8_t)tank_gene(f, g);   /* nothing hidden until rolled or inherited */
    f->mutated = 0; f->from_b = 0; f->from_carried = 0;
    f->uid = 0; f->uid = tank_new_uid(t); f->parent_uid[0] = f->parent_uid[1] = 0;
    f->away = false; f->homeward = false; f->follow_uid = 0; f->door = DOOR_NONE; f->door_edge = 0; f->door_seen = -1e6f; f->door_refused = 0;
    f->door_sent = false; f->deported = false; f->bolt_home = false;   /* (2026-11-02: a slot a fish was GIVEN out of kept its door_sent - the next
                                                                         fish made there hovered at every open door and never asked to cross) */
    tank_lizard_fresh(t, f);
    if (PT_TERRARIUM) walk_floor(t, f);               /* a lizard is made on the floor (the draws above as ever: a seeded run stays itself) */
}
/* a lizard new to this tank (made here, or in from next door): comfortable in its species' band, a fresh shed clock
   for its stage (docs/shed.md) - never 0 C (a visitor is built from its passport, which carries neither) - and out of
   every hide, standing, on the places' line (THE WALK; nothing drawn from the tank's stream: the aquarium's fish are
   made as they always were) */
void tank_lizard_fresh(const tank_t *t, fish_t *f) {
    const species_t *sp = tank_species(t);
    f->body_c = (sp->t_lo + sp->t_hi) * 0.5f; f->heat = species_heat_of(sp, f->body_c);
    f->shed = SHED_NORMAL; f->shed_growth = false; f->shed_clean = 0; f->shed_damp = 0;
    f->shed_s = sp->shed_every_s[f->stage & 3];
    f->hide = -1; f->pose = LZ_POSE_STAND; f->pose_s = 0; f->lane = 0; f->to_x = -1;
    f->surf = 0; f->to_surf = -1; f->explore_surf = 0; f->slope = 0;   /* (on the floor: walk_floor seats it there) */
    f->walk_s = f->pause_s = f->turn_s = f->strike_s = f->peek_s = 0;
    f->prey = -1; f->seen_surf = -1; f->seen_x = 0; f->spook_hide = -1; f->to_hide = -1; f->spook_s = 0; f->shed_helped = false;   /* (THE CRICKETS, THE RAP) */
}

void tank_set_bubble_x(tank_t *t, float x) {
    /* clear of the grass corner's spot (the fish's other landmark) and of the glass;
       grass is fine - the beds cover most of the width and the default spot
       already rises through one */
    float lo = t->reef_x + 50, hi = TANK_FX1 - 30;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    float dx = x - t->bubble_x;
    t->bubble_x = x;
    for (int i = 0; i < MAX_BUBBLE; i++) if (t->bubble[i].column) t->bubble[i].x += dx;   /* the column moves as one */
}

static void place_near_reef(tank_t *t, fish_t *f) {
    f->x = t->reef_x + tank_randf(t, -10, 30); f->y = t->reef_y - 30 + tank_randf(t, -10, 10);
    f->heading = tank_randf(t, -0.6f, 0.6f); tank_fish_face(f);
    if (PT_TERRARIUM) walk_floor(t, f);               /* a hatchling: on the sand under that spot */
}

void tank_new_population(tank_t *t) {
    /* two random presets */
    int a = (int)tank_randf(t, 0, ROSTER_N - 0.001f);
    int b = (int)tank_randf(t, 0, ROSTER_N - 1.001f); if (b >= a) b++;
    /* personalities rolled with a guaranteed contrast so the pair reads as
     * two characters at a glance (docs/progression-next.md, Act 1) */
    float ba, bb, sa, sb;
    do { ba = tank_randf(t, 0.1f, 0.9f); bb = tank_randf(t, 0.1f, 0.9f); } while (fabsf(ba - bb) < 0.45f);
    do { sa = tank_randf(t, 0.1f, 0.9f); sb = tank_randf(t, 0.1f, 0.9f); } while (fabsf(sa - sb) < 0.3f);
    /* both start as FRY: the founding pair grows up on the keeper's watch
     * (progression halved the stage clocks to match - 0.5/3/24 tended hours) */
    tank_make_fish(t, 0, a, sa, ba, STAGE_FRY);
    tank_make_fish(t, 1, b, sb, bb, STAGE_FRY);
    tank_roll_carried(t, 0); tank_roll_carried(t, 1);   /* the founders' secrets */
    t->n_fish = 2;
}

#define TEMPER_OWN 0.33f   /* the chance a fry rolls a trait of its own (tank_add_fish) */

int tank_add_fish(tank_t *t, int parent_a, int parent_b) {
    if (t->n_fish >= N_FISH_MAX || t->n_fish >= ROSTER_N) return -1;
    int used[ROSTER_N] = {0};
    for (int i = 0; i < t->n_fish; i++) used[t->fish[i].preset] = 1;
    int free_n = 0, free_idx[ROSTER_N];
    for (int i = 0; i < ROSTER_N; i++) if (!used[i]) free_idx[free_n++] = i;
    if (!free_n) return -1;
    int preset = free_idx[(int)tank_randf(t, 0, free_n - 0.001f)];
    /* a parent is one of ours, or a visitor (a cross-tank fry, phase 5) */
    #define PARENT_OK(i) (((i) >= 0 && (i) < t->n_fish) || (tank_is_visitor(i) && t->vis_used[(i) - N_FISH_MAX]))
    int ia = PARENT_OK(parent_a) ? parent_a : 0;
    int ib = PARENT_OK(parent_b) ? parent_b : (t->n_fish > 1 ? 1 : 0);
    #undef PARENT_OK
    const fish_t *pa = &t->fish[ia], *pb = &t->fish[ib];
    /* temper (2026-10-29, Strato: variety, not strong biases): each trait from
     * the parents' BIRTH tempers - what drifted in them was their life, not
     * their blood - blended at a ratio of its own +-0.15, and one trait in
     * TEMPER_OWN rolled fresh in the founders' range (its own fish, like a
     * founder). It was the drifted values' average +-0.15: a bold generation
     * bred bold fry, and a family of five squeezed to the middle (spread 0.20
     * -> 0.09 in eight generations; now 0.16, no more fish at the ends than
     * the founders have - sim --selftest-temper). */
    float bold = tank_randf(t, 0, 1) < TEMPER_OWN ? tank_randf(t, 0.1f, 0.9f)
               : clampf(lerpf(pb->bold0, pa->bold0, tank_randf(t, 0, 1)) + tank_randf(t, -0.15f, 0.15f), 0.05f, 0.95f);
    float soc  = tank_randf(t, 0, 1) < TEMPER_OWN ? tank_randf(t, 0.1f, 0.9f)
               : clampf(lerpf(pb->sociable0, pa->sociable0, tank_randf(t, 0, 1)) + tank_randf(t, -0.15f, 0.15f), 0.05f, 0.95f);
    int slot = t->n_fish;
    tank_make_fish(t, slot, preset, soc, bold, STAGE_FRY);
    /* its look is the family's, not the preset's (2026-09-14; heredity 0.4).
       Which parent is "a" stays a coin on the tank's own stream, as it was
       (the genes draw on theirs, so nothing else in a seeded run moves) */
    if (tank_randf(t, 0, 1) < 0.5f) { int x = ia; ia = ib; ib = x; pa = &t->fish[ia]; pb = &t->fish[ib]; }
    fish_t *f = &t->fish[slot];
    int show[GENE_N], keep[GENE_N];
    f->from_b = 0; f->from_carried = 0;
    for (int g = 0; g < GENE_N; g++) {
        /* one copy from each parent: the one it shows, or (GENE_PASS_CARRIED)
           the one it carries; the fry shows one of the two, carries the other */
        bool ca = gene_f(t) < GENE_PASS_CARRIED, cb = gene_f(t) < GENE_PASS_CARRIED;
        int va = ca ? pa->carried[g] : tank_gene(pa, g), vb = cb ? pb->carried[g] : tank_gene(pb, g);
        if (gene_f(t) < 0.5f) { show[g] = va; keep[g] = vb; if (ca) f->from_carried |= 1u << g; }
        else { show[g] = vb; keep[g] = va; f->from_b |= 1u << g; if (cb) f->from_carried |= 1u << g; }
    }
    /* a mutation: one gene shows a value neither parent shows - one neither
       carries either, when one is left */
    f->mutated = 0;
    bool force = t->mutate_next; t->mutate_next = false;
    if (force || gene_f(t) < GENE_MUTATION) {
        int g = gene_i(t, GENE_N), n = tank_gene_values(g), cand[LOOK_N], nc = 0;
        int sa = tank_gene(pa, g), sb = tank_gene(pb, g);
        for (int pass = 0; pass < 2 && !nc; pass++)
            for (int v = 0; v < n; v++)
                if (v != sa && v != sb && (pass || (v != pa->carried[g] && v != pb->carried[g]))
                    && !(g == GENE_ACCENT && LOOK_ACCENT[v] == LOOK_BODY[show[GENE_BODY]])   /* markings that match */
                    && !(g == GENE_BODY && LOOK_BODY[v] == LOOK_ACCENT[show[GENE_ACCENT]]))  /* the body would vanish */
                    cand[nc++] = v;
        if (nc) { show[g] = cand[gene_i(t, nc)]; f->mutated = (uint8_t)(g + 1); f->from_carried &= ~(1u << g); }
    }
    /* the body before the markings: tank_set_look keeps them apart */
    tank_set_look(t, slot, LOOK_BODY[show[GENE_BODY]], LOOK_ACCENT[show[GENE_ACCENT]]);
    f->shape = (uint8_t)show[GENE_SHAPE]; f->fins = (uint8_t)show[GENE_FINS]; f->pattern = (uint8_t)show[GENE_PATTERN];
    for (int g = 0; g < GENE_N; g++) f->carried[g] = (uint8_t)keep[g];
    f->parent_a = (int8_t)(ia < N_FISH_MAX ? ia : -1); f->parent_b = (int8_t)(ib < N_FISH_MAX ? ib : -1);   /* (a visitor parent is no slot of this tank: its ID says who) */
    f->parent_uid[0] = pa->uid; f->parent_uid[1] = pb->uid;   /* who they are, wherever they go (after the coin swap) */
    place_near_reef(t, &t->fish[slot]);
    t->fish[slot].hunger = 4; t->fish[slot].trust = 4;
    t->n_fish++;
    return slot;
}

/* a fry from the shop (0.3.5): no parents - a free roster slot, the look it
 * was offered in, a temperament of its own (nobody's to inherit), and in at
 * the surface under the middle of the glass, as if poured from the bag */
int tank_add_bought_fry(tank_t *t, uint32_t body, uint32_t accent) {
    if (t->n_fish >= N_FISH_MAX || t->n_fish >= ROSTER_N) return -1;
    int used[ROSTER_N] = {0};
    for (int i = 0; i < t->n_fish; i++) used[t->fish[i].preset] = 1;
    int free_n = 0, free_idx[ROSTER_N];
    for (int i = 0; i < ROSTER_N; i++) if (!used[i]) free_idx[free_n++] = i;
    if (!free_n) return -1;
    int preset = free_idx[(int)tank_randf(t, 0, free_n - 0.001f)];
    int slot = t->n_fish;
    tank_make_fish(t, slot, preset, tank_randf(t, 0.1f, 0.9f), tank_randf(t, 0.1f, 0.9f), STAGE_FRY);   /* the founders' range (was 0.25..0.75) */
    tank_set_look(t, slot, body, accent);
    fish_t *f = &t->fish[slot];
    f->parent_a = f->parent_b = -1;
    f->x = TANK_W * 0.5f + tank_randf(t, -30, 30); f->y = tank_glass_top(f->x) + 30;
    f->heading = tank_randf(t, 1.2f, 1.9f); tank_fish_face(f);   /* heading down into the water */
    if (PT_TERRARIUM) walk_floor(t, f);               /* a lizard from the shop: set down on the floor */
    f->hunger = 4; f->trust = 4;
    t->n_fish++;
    return slot;
}

bool tank_remove_fish(tank_t *t, int idx) {
    if (idx < 0 || idx >= t->n_fish) return false;
    for (int i = idx; i < t->n_fish - 1; i++) {
        t->fish[i] = t->fish[i + 1];
        t->sd_paid_fish[i] = t->sd_paid_fish[i + 1];
        /* its spot by the reef is its slot's (tank_make_fish): a slot nearer,
           so the next arrival's spot is free again */
        t->fish[i].rest_dx -= 24; t->fish[i].rest_dy += 9;
    }
    t->n_fish--;
    t->sd_paid_fish[t->n_fish] = 0;
    for (int i = 0; i < t->n_fish; i++) {
        fish_t *f = &t->fish[i];
        if (f->parent_a == idx) f->parent_a = -1; else if (f->parent_a > idx) f->parent_a--;
        if (f->parent_b == idx) f->parent_b = -1; else if (f->parent_b > idx) f->parent_b--;
    }
    t->courting = false; t->court_a = t->court_b = -1; t->court_active = 0;   /* progression picks the pair again */
    t->spawning = false; t->spawn_danced = 0;
    t->stage_fish = -1;
    t->mist_fish = -1; t->mist_s = 0;                 /* (the mister's hold picks again: the slots moved) */
    t->roster_gen++; t->removed_slot = (int8_t)idx;
    return true;
}

void tank_init(tank_t *t, uint32_t seed) {
    t->rng = seed ? seed : 0xC0FFEE;
    t->gene_rng = (t->rng * 2654435761u) ^ 0x5bd1e995u; if (!t->gene_rng) t->gene_rng = 0x5bd1e995u;
    t->uid = 0; t->uid_n = 0;      /* a new tank's ID comes with its first fish (tank_ensure_ids); a load restores it */
    t->n_fish = 0;                 /* progression_boot restores or calls tank_new_population */
    /* the tank link (0.4): no visitors, no open doors, no fish due home - a
       tank on the stack starts with garbage here (2026-10-05: a selftest's
       stack tank grew a "visitor" with a garbage heading and norm_ang spun
       forever, 1 run in 10), and a re-init must not keep the last session's */
    for (int k = 0; k < N_VISIT_MAX; k++) { t->vis_used[k] = false; t->vis_pending[k] = false; t->vis_badge[k] = -1; t->fry_rolled[k] = 0; }
    t->link_badge = -1; t->deport_uid = 0; t->fry_mate = 0; t->fry_ask_at = t->fry_room_at = -1e6f; t->fry_ask_uid = 0;
    for (int h = 0; h < TANK_FRY_HOLDS; h++) { t->fry_hold[h].peer = t->fry_hold[h].parent = 0; t->fry_hold[h].until = 0; }
    t->cap_over = 0;
    for (int i = 0; i < 6; i++) { t->bond[i].vis = t->bond[i].mate = 0; t->bond[i].s = t->bond[i].need = 0; }
    t->fry_partner = 0; t->fry_home_room = -1; t->fry_give_k = -1; t->fry_force = -1; t->fry_asked = t->fry_give_sent = false; t->fry_give_parent = 0;
    t->door_open[0] = t->door_open[1] = false;
    t->link_restore_at = 0;
    for (int i = 0; i < MAX_FOOD; i++) t->food[i].alive = false;
    t->bubble_x = BUBBLE_X_DEFAULT; t->bubble_y = TANK_BOT * 0.5f;   /* matches gen_traces.py; setup may move x */
    for (int i = 0; i < MAX_BUBBLE; i++) {
        bubble_t *b = &t->bubble[i];
        b->column = i < 10;
        b->x = b->column ? t->bubble_x + tank_randf(t, -10, 10) : tank_randf(t, TANK_FX0 + 12, TANK_FX1 - 12);
        b->y = tank_randf(t, 0, TANK_BOT);
        b->vy = tank_randf(t, 14, 30);
        b->wobble = tank_randf(t, 0, TAU);
    }
    t->reef_x   = TANK_FX0 + TANK_FW * 0.15f; t->reef_y   = TANK_BOT * 0.85f;
    t->clock = 0; t->night = false; t->idle_s = 0;
    t->light_idle_s = LIGHT_IDLE_S; t->light_auto = false; t->light_manual_off = false; t->light_tip_seen = false;
    t->orient_lock = false; t->autofeed_off = false;
    t->screen_turned = false;
    t->light_override = false; t->light_on = true;
    t->hold_active = false; t->hold_time = 0; t->hold_approached = false;
    t->tap_count = 0; t->tap_burst_t = 99; t->startled = false;
    t->startle_cooldown = 0;
    t->feed_spot_x = -1; t->player_feedings = 0; t->feed_open = false; t->hold_approaches = 0; t->greet_timer = 0;
    t->courting = false; t->court_a = t->court_b = -1;
    t->court_cool = 30; t->court_active = 0;
    t->spawning = false; t->spawn_danced = 0; t->ui_cover = false;
    t->ravenous = false; t->trickle_off = false;
    t->stage_fish = -1; t->hold_light = false;
    t->drag_active = false; t->drag_has_prev = false; t->drag_dist = 0;
    for (int b = 0; b < VEG_BEDS_MAX; b++) {
        /* a fresh tank's canopy has a natural profile: fronds within +-0.04
           of VEG_START, seeded per slot so it's the same tank every boot */
        for (int i = 0; i < VEG_FRONDS_MAX; i++) {
            uint32_t h = (uint32_t)((b * 17 + i + 1) * 2654435761u);
            t->veg_h[b][i] = VEG_START + ((int)(h >> 8 & 255) - 128) / 128.0f * 0.04f;
        }
    }
    veg_sync(t);
    t->slash_armed = t->slash_engaged = t->slash_cut = false;
    t->slash_h = t->slash_v = 0;
    t->tool = TOOL_HAND; t->tool_idle = 0; t->tools_locked = false;
    for (int i = 0; i < ALGAE_CELLS; i++) t->algae[i] = 0;
    t->algae_acc = 0; t->trims = 0; t->cells_cleaned = 0;
    t->algae_colonies = 0; t->trim_px = 0;
    t->sd_balance = t->sd_earned = 0; t->sd_unlocks = 0; t->sd_shelved = 0; t->sd_locked = 0;
    memset(t->deco_x, 0, sizeof t->deco_x);                  /* (the terrarium's decorations, P5b: none placed) */
    for (int i = 0; i < N_FISH_MAX; i++) t->sd_paid_fish[i] = 0;
    t->sd_colonies_paid = t->sd_inches_paid = 0;
    t->snail_x = -1; t->snail_y = -1; t->snail_heading = 0; t->snail_cell = -1; t->snail_graze = 0;
    t->snail_grazed = 0; t->snail_sleep_acc = 0;
    t->urchin_x = -1; t->urchin_bed = t->urchin_frond = -1; t->urchin_appetite = 0; t->urchin_chew = 0;
    t->urchin_to = -1; t->urchin_rest = 0; t->urchin_grazed_px = 0; t->urchin_front = false; t->urchin_dy = 0;   /* behind, as it always was, until its first move rolls a lane */
    t->cr_x = -1; t->cr_y = 0; t->cr_tilt = 0; t->cr_facing = 1; t->cr_variant = 0; t->cr_mode = CR_REST; t->cr_mode_s = 0; t->cr_poke = 0; t->cr_eaten = 0; t->cr_fidget_kind = CR_FID_NONE; t->cr_fidget = 0; t->cr_next_fidget = 2; t->cr_shuffle_to = -1; t->cr_taps = 0; t->cr_tap_t = -100; t->cr_scurry = false; t->cr_hide = 0; t->cr_back = 0; t->cr_cave = -1; t->cr_food = -1; t->cr_side = 1;
    t->cr_to = -1; t->cr_timer = 0; t->cr_gait = 0; t->cr_climb = 0; t->cr_outing = 0; t->cr_since_climb = 0; t->cr_nibble = 0; t->cr_nibble_i = 0;
    t->cr_startle_cool = 0; t->cr_find = 0; t->cr_find_cool = 0; t->cr_found = 0; t->cr_found_paid = 0; t->cr_was_night = false; t->cr_stood = false; t->cr_lane_front = true; t->cr_next_dig = false;
    t->cr_claim = -1; t->cr_shoo = 0; t->cr_meals = 0; t->cr_full = 0; t->shrimp_fp = -1; t->shrimp_flee = 0; t->shrimp_avoid_x = 0; t->shrimp_avoid_s = 0;
    t->sh_x = -1; t->sh_y = 0; t->sh_bed = t->sh_frond = -1; t->sh_mode = SH_DRIFT; t->sh_facing = 1;
    t->sh_akind = SH_ANCHOR_NONE; t->sh_aitem = -1; t->sh_ax = t->sh_ay = t->sh_ahw = t->sh_aref = 0; t->sh_front = t->sh_front_want = false; t->sh_obed = -1; t->sh_opost = -1; t->sh_orow = false;
    t->sh_hitched = t->sh_credited = false; t->sh_tx = t->sh_ty = 0; t->sh_stay = 0; t->sh_look = 0; t->sh_vx = t->sh_vy = 0;
    t->sh_pose = SH_POSE_BLADE; t->sh_land = 0;
    t->sh_s = 0; t->sh_hitches = 0; t->sh_trust_passed = 0;
    t->sh_eaten = 0; t->sh_snack_pay = 0; t->sh_snack_cool = 0; t->sd_snacks_paid = 0;
    t->sh_snap = -1; t->sh_snap_food = -1; t->sh_turn = 0;
    t->plant_x = 0; t->plant_z = DECOR_Z_BACK;
    t->castle_x = 0; t->castle_z = DECOR_Z_FRONT;
    t->coral_x = 0; t->coral_z = DECOR_Z_FRONT; t->coral_rgb = 0; t->coral_growth = 0; t->coral_acc = 0;
    t->cluster_x = 0; t->cluster_z = DECOR_Z_FRONT; t->cluster_scheme = 0; t->cluster_growth = 0; t->cluster_acc = 0;
    t->tank_ms_bits = 0; t->tank_ms_seen = 0; t->ask_rr = 0; t->advisor_asks = 0;
    t->pack_id = SPECIES_DEFAULT; t->habitat_id = HABITAT_DEFAULT; t->packs_missing = false; t->light_s = 0;
    tank_terrain(t);                                  /* (the habitat's ground laid out here, before anything reads it) */
    {   const habitat_t *h = tank_habitat(t);         /* the humid hides at the habitat's start (the rest hold none) */
        for (int k = 0; k < HABITAT_HIDES_MAX; k++) t->moist[k] = k < h->n_hides && h->hides[k].kind == HIDE_HUMID ? h->moist_start : 0; }
    t->mist_stroke = 0; t->mist_hold = t->mist_done = false; t->mist_x = t->mist_y = t->mist_s = 0; t->mist_fish = -1;
    for (int i = 0; i < MAX_CRICKETS; i++) t->cricket[i].state = CK_NONE;   /* THE CRICKETS: none yet (the two below are the terrarium's), the dish empty */
    t->dish_worms = 0; t->dish_farm = false; t->ck_dropped = t->ck_caught = t->ck_missed = t->ck_lost = t->dish_eaten = 0;
    tank_scatter_food(t, 2);
}

#define STARTLE_COOLDOWN 6.0f   /* calm seconds before the spook wears off */

#define SPAWN_NEAR_PX   20.0f   /* past the courtship loop's reach: a parent this close is circling in the fronds */

#define HOLD_RING_W       0.07f  /* rad/s the places drift (~4 px/s: a fish shifts its spot now and then, not a lap) */

#define HOLD_GATHERED_PX  100.0f  /* a fish this close to a drawing finger has come to it (social warmth, 2026-10-29;
                                   0.3.9.3: the ring's outer reach, 1.2 x (R + wobble) to the side; was 45 under the fingertip) */
#define HOLD_APPROACH_FROM 140.0f /* a hold-approach must START at least this far out - past the ring:
                                 * a fish already at the finger earns nothing (0.3.9.3: was 60 / 30, under the fingertip) */
#define HOLD_APPROACH_AT   HOLD_GATHERED_PX   /* ... and come in this close: onto the ring */
#define TRUST_PER_S        0.012f /* trust a fish earns a second under a resting finger (0.3.6: was 0.02;
                                   * Strato: "a bit slower" so the seahorse's double is worth having) */
#define SEAHORSE_TRUST_X   2.0f   /* ... times this while the seahorse hangs on the finger */

bool tank_at_finger(const tank_t *t, int i) {   /* (hold_time, not hold_active: the tick consumes the
                                                   flag, and the time drops to 0 the frame the finger lifts) */
    if (PT_TERRARIUM) {
        const fish_t *f = &t->fish[i];
        if (!tank_swims(t, i) || !hand_draws(t, f) || f->hide >= 0) return false;
        float x; const int surf = lizard_hand_target(t, i, &x, NULL);
        return f->surf == surf && fabsf(f->x - x) <= LZ_ARRIVE_PX;
    }
    float hx, hy; tank_hand_point(t, &hx, &hy);
    return t->hold_time >= HOLD_ATTRACT_S
        && tank_dist(t->fish[i].x, t->fish[i].y, hx, hy) < HOLD_GATHERED_PX;
}
float tank_tap_d2(const tank_t *t, int i, float x, float y, float px, float py) {
    if (!PT_TERRARIUM) return (px - x) * (px - x) + (py - y) * (py - y);   /* a fish: its middle, as ever */
    const fish_t *f = &t->fish[i];                                      /* a lizard: the nearest point of its body's line */
    const float half = tank_lizard_half_len(t, f), ux = cosf(f->slope), uy = -(f->facing < 0 ? -1.0f : 1.0f) * sinf(f->slope);
    const float u = clampf((px - x) * ux + (py - y) * uy, -half, half), qx = x + ux * u - px, qy = y + uy * u - py;
    return qx * qx + qy * qy;
}

/* ---- hunger economy (2026-09-01) ----
 * The prototype's per-second metabolism (0.15 + 0.12*bold: fed to starving
 * in under a minute) shipped unchanged into a tank the keeper tends for a
 * few minutes at a time - and the trickle that fed the prototype's single
 * fish was scaled per FRAME, so at the device's 25 fps it dropped 2.4x fewer
 * pellets than the 60 fps sim and could never keep up with four fish: the
 * school lived at hunger 9-10, begging under the surface (Strato: "ravenous
 * too often, hovering near the top, breaking the experience").
 * Now: a meal lasts ~5-6 minutes of awake time, and the tank's own trickle is
 * HUNGER-GATED and per-second - it drops a pellet only while somebody is
 * really hungry (a few seconds' wait), so an untended tank cycles between
 * "peckish" and "just fed" on its own and never reaches the ravenous
 * threshold (8.5, progression.c) awake. Ravenous begging is once again the
 * after-sleep / long-absence event it was designed to be (tank_tick_sleep's
 * 0.8/h), and the keeper's pellets are what make a fish FULL - play, bubbles,
 * the reef, a friend: the fed-fish repertoire. The advisor sees only banded
 * hunger, so its training distribution is untouched. */
/* (HUNGER_PER_S, the energy and curiosity rates and the sleep's are tank.h's THE FISH'S DRIVES since P2b: the
 * aquarium's fish pack carries them) */

#define TRICKLE_HUNGER     7.5f     /* the tank feeds itself only past this (someone) */
#define TRICKLE_RATE       0.15f    /* pellets/s while the gate is open (~7 s wait) */

/* ---- upkeep: the vegetation keeps growing, algae films the glass ----
 * Vegetation is a comfort system, not scenery: fish swim slower inside the
 * canopy and their stress (already in the advisor's schema) answers to it -
 * cover calms (a canopy is a place to hide), a tank being smothered (two
 * beds past VEG_SMOTHER) presses hard, a completely scalped tank is a mild
 * unease that lifts as soon as one tuft regrows. Rates are per real second;
 * tank_tick runs them awake, tank_tick_sleep runs them faster (an untended
 * dark tank is where the garden gets away from you). */
#define VEG_GROW_AWAKE_S    162000.0f /* nubs -> the ceiling in ~45 h awake (was 30 h
                                       * until 2026-09-23: a few hours' sleep put
                                       * every bed at the surface, every time) */
#define VEG_GROW_SLEEP_S    72000.0f  /* ~20 h of sleep (was 10 h): one night takes a
                                       * trimmed bed from a third of the glass to
                                       * two thirds, the second reaches the ceilings */
#define VEG_SWORD_GROW      1.25f     /* the sword plant grows a bit faster than the
                                       * grass (~36 h awake / ~16 h asleep) */

#define ALGAE_STEP_AWAKE_S  240.0f    /* one film growth step per 4 min awake */
#define ALGAE_STEP_SLEEP_S  180.0f    /* (120 until 2026-09-23: a 7 h night filmed the
                                       * glass to the cap, so every morning looked the
                                       * same; now a night lands near ALGAE_DIRTY) */
#define ALGAE_COVER_CAP     0.30f     /* growth stops claiming new cells here */
#define WIPE_RADIUS      20.0f  /* squeegee half-width around the drag path */
#define WIPE_ENGAGE_PX   18.0f  /* stroke travel before a drag starts wiping
                                 * (a rolly fingertip tap stays under this) */
#define SLASH_PX         16.0f  /* sideways travel before a stroke is scissors (a
                                 * rolly fingertip tap stays under this; one
                                 * frond pitch is 12 px, so a flick takes 1-2) */
#define SLASH_RATIO      1.5f   /* ... and it must be this much more h than v */
#define SLASH_TOOL_PX    10.0f  /* the SCISSORS picked from the toolbox: the keeper
                                 * said "cut", so a shorter stroke, merely more
                                 * sideways than not, is enough */
#define SLASH_START_PX   10.0f  /* a slash must START this close to a FROND's tip
                                 * (upward) - not the bed's box: a cleaning scrub
                                 * begun mid-glass over a tall bed used to arm the
                                 * scissors (Strato, 2026-09-04) */
#define SLASH_START_SIDE_PX 16.0f /* ... and this close to a spine SIDEWAYS: the pad
                                 * of a fingertip is ~6 mm = 75 px on this glass, so
                                 * a landing a pitch beside a bed's outer frond has
                                 * that frond under the finger (a stroke begun at
                                 * the glass, heading in, used to arm nothing) */
#define SLASH_REACH_PX    8.0f  /* the fingertip's reach past its REPORTED point: a
                                 * stroke cuts the frond up to this far beyond where
                                 * it lands and where it lifts. The touch point is
                                 * the pad's centre; the outer frond of a bed stands
                                 * 24 px from the glass and the centre stops short of
                                 * it, so "the last blade never trips" (Strato,
                                 * 2026-09-14). Two thirds of the 12 px pitch: a flick
                                 * still takes only the fronds it visibly covers. */
#define SLASH_WALL_PX       48.0f /* a frond this close to a side wall is a WALL frond ... */
#define SLASH_WALL_SIDE_PX  96.0f /* ... and a stroke that starts on no frond of its bed,
                                 * but this close to it on its OPEN side (no higher
                                 * than its tip), is aimed at it: armed, and once it
                                 * is in among the wall fronds, heading for the wall,
                                 * it runs on into the wall. No finger can stand on a
                                 * frond 19 px from the glass, or come at it from the
                                 * wall: the pad meets the bezel and its center stops
                                 * 2-4 mm short. With its neighbors mown, a bed's
                                 * outer frond stood where no stroke could begin or
                                 * end (Strato's log, 2026-10-01: 73 tries at one,
                                 * starting 29..106 px from the wall, lifting at
                                 * 39..66). A stroke that starts ON a frond of that
                                 * bed keeps the precise reach above. */

static int popcount32u(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

/* canopy geometry: one source of truth for render, the trim hit test and the
 * in-canopy slowdown. Bed 0 is the reef bed (milestone lushness widens its
 * base, as ever); growth adds fronds (out, wider) and segments (up, toward
 * the surface). Height IS growth (2026-09-04): every bed's tallest frond
 * stands g of the way from the floor to the surface - at growth 1 it
 * touches the tank ceiling, whichever bed it is - so "85% grown" and "85%
 * of the tank's height" are the same thing for the comfort band below. At
 * VEG_NUB the fronds are ~5 segment green stubble. */
static int veg_segs(float h) { return 2 + (int)(h * (VEG_SEGS_FULL - 2)); }
/* frond pitch per bed: grass at 12 px; the sword plant's broad leaves at 14 */
static int veg_pitch(int b) { return b == 3 ? 14 : 12; }
void veg_bed_base(const tank_t *t, int b, float *bx0, int *n) {
    int lush = popcount32u(t->tank_ms_bits); if (lush > 4) lush = 4;
    int base_n;
    if (b == 0)      { *bx0 = t->reef_x - 24 - lush * 6; base_n = 5 + lush; }
#ifdef TANK_ROUND                                          /* the bowl's floor is 358 px, not 448: the two right-hand beds keep
                                                              their place against the right glass, the reef bed gives up two fronds */
    else if (b == 1) { *bx0 = TANK_FX1 - 72; base_n = 4; }
    else if (b == 2) { *bx0 = TANK_FX1 - 170; base_n = 2; }
#elif defined(TANK_WATCH)                                  /* the watch's portrait floor is 322 px: bed 1 against the right glass, bed 2
                                                              five fronds beside it, the reef bed four fronds fewer - open sand between */
    else if (b == 1) { *bx0 = TANK_FX1 - 72; base_n = 4; }
    else if (b == 2) { *bx0 = TANK_FX1 - 146; base_n = -1; }
#elif defined(TANK_SQUARE)                                 /* the square's floor is 460 px, 12 more than the rectangle's: the two right-hand
                                                              beds keep their place against the right glass (the rectangle's 0.84 x 480
                                                              left bed 1 a frond short of the wall) */
    else if (b == 1) { *bx0 = TANK_FX1 - 72; base_n = 4; }
    else if (b == 2) { *bx0 = TANK_FX1 - 170; base_n = 2; }
#else
    else if (b == 1) { *bx0 = TANK_W * 0.84f; base_n = 4; }
    else if (b == 2) { *bx0 = TANK_W * 0.62f; base_n = 2; }
#endif
    else             { *bx0 = tank_decor_x(t, 0) - PLANT_HALF_W; *n = 4; return; }   /* the sword plant
                                                              (2026-09-15): four broad leaves, by default on
                                                              the open floor between the reef bed's widest
                                                              reach (~199) and bed 2 (272) - the keeper's
                                                              to move (tank_decor_set) */
    /* the frond count is fixed per bed now (it used to widen with growth):
       with fronds cut one at a time, a bed's outer fronds can't be allowed
       to vanish because its MEAN height dropped */
    int nn = base_n + 6;
#ifdef TANK_ROUND
    if (b == 0) nn -= 2;
#elif defined(TANK_WATCH)
    if (b == 0) nn -= 4;
#endif
    if (nn > VEG_FRONDS_MAX) nn = VEG_FRONDS_MAX;
    while (nn > 1 && *bx0 + nn * 12 > TANK_FX1 - 8) nn--;   /* beds stop at the glass */
    *n = nn;
}
int tank_veg_beds(const tank_t *t) { return (t->sd_unlocks & SD_ITEM_PLANT) && !(t->sd_shelved & SD_ITEM_PLANT) ? VEG_BEDS_MAX : VEG_BEDS; }
static int veg_grow_beds(const tank_t *t) { return (t->sd_unlocks & SD_ITEM_PLANT) ? VEG_BEDS_MAX : VEG_BEDS; }   /* a plant on the shelf grows on (0.3.9) */
veg_kind_t tank_veg_kind(const tank_t *t, int b) { (void)t; return b == 3 ? VEG_KIND_SWORD : VEG_KIND_GRASS; }
void tank_veg_bed(const tank_t *t, int b, float *x0, float *x1, float *top_y, int *fronds) {
    float bx0; int n;
    veg_bed_base(t, b, &bx0, &n);
    float hmax = 0;
    for (int i = 0; i < n; i++) if (t->veg_h[b][i] > hmax) hmax = t->veg_h[b][i];
    if (x0) *x0 = bx0 - 6;
    if (x1) *x1 = bx0 + n * veg_pitch(b) + 6;
    if (top_y) *top_y = TANK_BOT - 16 - veg_segs(hmax) * VEG_SEG_PX - 4;
    if (fronds) *fronds = n;
}
int tank_nursery_bed(const tank_t *t) {
    int best = -1;
    for (int b = 0; b < tank_veg_beds(t); b++)
        if (t->veg_growth[b] >= VEG_NURSERY && (best < 0 || t->veg_growth[b] > t->veg_growth[best])) best = b;
    return best;
}
/* where the pair circles: low inside the nursery bed, a flat loop that weaves
 * the fronds (body centre ~14 px off the floor line) */
void court_site(const tank_t *t, float *cx, float *cy, float *rx) {
    int b = tank_nursery_bed(t);
    if (b < 0) { *cx = t->reef_x + 26; *cy = TANK_BOT - 78; *rx = 26; return; }   /* legacy spot */
    float x0, x1; tank_veg_bed(t, b, &x0, &x1, NULL, NULL);
    *cx = (x0 + x1) * 0.5f; *cy = TANK_BOT - 16 - 14;
    float half = (x1 - x0) * 0.5f - 8; *rx = half < 16 ? 16 : half > 30 ? 30 : half;
}
void tank_court_puff(tank_t *t, int n) {
    float sx, sy, sr; court_site(t, &sx, &sy, &sr);
    for (int i = 0; i < MAX_BUBBLE && n > 0; i++) {
        if (t->bubble[i].column) continue;
        t->bubble[i].x = sx + tank_randf(t, -10, 10);
        t->bubble[i].y = sy - 6;
        n--;
    }
}

int tank_veg_frond(const tank_t *t, int b, int i, float *x) {
    float bx0; int n;
    veg_bed_base(t, b, &bx0, &n);
    if (x) *x = bx0 + i * veg_pitch(b);
    return veg_segs(t->veg_h[b][i]);
}
/* veg_growth[b] is the bed's mean frond height - the comfort band, the save
 * and the firmware log read it; recomputed after anything moves a frond */
void veg_sync(tank_t *t) {
    for (int b = 0; b < VEG_BEDS_MAX; b++) {
        float bx0; int n; veg_bed_base(t, b, &bx0, &n);
        float sum = 0;
        for (int i = 0; i < n; i++) sum += t->veg_h[b][i];
        t->veg_growth[b] = sum / n;
    }
}
/* a frond slot's own number in 0..1, one per salt. A full mix: the plain
 * multiply this replaced stepped ~0.05 per frond, so every grown bed was a
 * smooth slope and not a ragged skyline (2026-10-05) */
static float veg_slot_rand(int b, int i, uint32_t salt) {
    uint32_t h = (uint32_t)(b * VEG_FRONDS_MAX + i) * 0x9E3779B9u ^ salt;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return (float)(h >> 8) / 16777215.0f;
}
/* frond i of bed b grows toward its own ceiling: a hash of the slot spread
 * over VEG_CAP_LO..VEG_CAP_HI (a different hash than the fresh tank's start
 * profile, so a tall start does not mean a tall ceiling) */
float tank_veg_cap(int b, int i) {
    float cap = VEG_CAP_LO + veg_slot_rand(b, i, 0x5EA62A55u) * (VEG_CAP_HI - VEG_CAP_LO);
#if defined(TANK_ROUND) || defined(TANK_CORNER_R)          /* the bowl closes in over the outer fronds: a tip stops 12 px under the glass
                                                              (the reef bed taken at its widest, the sword plant wherever it stands);
                                                              the watch's and the square's upper corners do the same to the beds by its walls */
    if (b < 3) {
#ifdef TANK_WATCH
        float x = (b == 0 ? TANK_FX0 + TANK_FW * 0.15f - 48 : b == 1 ? TANK_FX1 - 72 : TANK_FX1 - 146) + i * 12;
#else                                                      /* (the bowl's and the square's bed spots) */
        float x = (b == 0 ? TANK_FX0 + TANK_FW * 0.15f - 48 : b == 1 ? TANK_FX1 - 72 : TANK_FX1 - 170) + i * 12;
#endif
        float room = (TANK_BOT - 16 - tank_glass_top(x) - VEG_GLASS_GAP) / (VEG_SEGS_FULL * VEG_SEG_PX);
        if (cap > room) cap = room;
    }
#endif
    return cap;
}
/* a bed's mean ceiling: what "full" means for it (the smother band) */
float veg_cap_mean(const tank_t *t, int b) {
    float bx0; int n; veg_bed_base(t, b, &bx0, &n);
    float sum = 0;
    for (int i = 0; i < n; i++) sum += tank_veg_cap(b, i);
    return sum / n;
}
static void veg_grow(tank_t *t, float dg) {
    for (int b = 0; b < veg_grow_beds(t); b++) {
        float dgb = tank_veg_kind(t, b) == VEG_KIND_SWORD ? dg * VEG_SWORD_GROW : dg;
        for (int i = 0; i < VEG_FRONDS_MAX; i++) {
            float h = t->veg_h[b][i], cap = tank_veg_cap(b, i);
            if (h >= cap) continue;                   /* at (or staged above) its ceiling */
            float room = cap - h;                     /* the last stretch comes in slowly */
            float ease = room < VEG_CAP_TAPER ? fmaxf(0.3f, room / VEG_CAP_TAPER) : 1.0f;
            float pace = 1.0f + (veg_slot_rand(b, i, 0xF20D5u) * 2 - 1) * VEG_PACE_SPREAD;   /* its own pace: a flat cut regrows ragged */
            t->veg_h[b][i] = fminf(cap, h + dgb * pace * ease);
        }
    }
    veg_sync(t);
}
void tank_veg_sync(tank_t *t) { veg_sync(t); }
void tank_veg_set(tank_t *t, int b, float g) {
    for (int i = 0; i < VEG_FRONDS_MAX; i++) t->veg_h[b][i] = g;
    veg_sync(t);
}

/* is (x,y) inside bed b's canopy? */
/* is (x,y) within `side` px of some frond of bed b sideways, and no more than
 * `up` px above its tip? The slash arms only from here. */
static bool veg_near_frond(const tank_t *t, int b, float x, float y, float side, float up) {
    float x0, x1; int n;
    tank_veg_bed(t, b, &x0, &x1, NULL, &n);
    if (x < x0 - side || x > x1 + side) return false;
    for (int i = 0; i < n; i++) {
        float fx; int segs = tank_veg_frond(t, b, i, &fx);
        float tip = TANK_BOT - 16 - segs * VEG_SEG_PX;
        if (fabsf(x - fx) <= side && y >= tip - up) return true;
    }
    return false;
}
/* does (x,y) lie on the OPEN side of one of bed b's wall fronds, within
 * SLASH_WALL_SIDE_PX of it and no more than `up` px above its tip? -1 = a
 * frond by the left wall, +1 = by the right, 0 = no. */
static int veg_wall_frond(const tank_t *t, int b, float x, float y, float up) {
    int n; tank_veg_bed(t, b, NULL, NULL, NULL, &n);
    for (int i = 0; i < n; i++) {
        float fx; int segs = tank_veg_frond(t, b, i, &fx);
        if (y < TANK_BOT - 16 - segs * VEG_SEG_PX - up) continue;
        float d = x - fx;
        if (fx <= TANK_FX0 + SLASH_WALL_PX && d > 0 && d <= SLASH_WALL_SIDE_PX) return -1;
        if (fx >= TANK_FX1 - SLASH_WALL_PX && d < 0 && -d <= SLASH_WALL_SIDE_PX) return 1;
    }
    return 0;
}
bool veg_inside(const tank_t *t, int b, float x, float y) {
    float x0, x1, ty;
    tank_veg_bed(t, b, &x0, &x1, &ty, NULL);
    return x >= x0 && x <= x1 && y >= ty;
}
bool tank_in_grass(const tank_t *t, float x, float y) {
    for (int b = 0; b < tank_veg_beds(t); b++)
        if (tank_veg_kind(t, b) == VEG_KIND_GRASS && t->veg_growth[b] >= VEG_BARE && veg_inside(t, b, x, y)) return true;
    return false;
}
void tank_reef_spot(const tank_t *t, float *x, float *y) {
    if (tank_decor_in(t, 4)) {                     /* the cluster stands ~75 px off the floor (more with its bloom): the
                                                      orbit, REEF_ORBIT_UP above this, rides round its crown */
        *x = tank_decor_x(t, 4); *y = TANK_BOT - 50;
    } else { *x = t->reef_x; *y = t->reef_y; }
}

/* the scissors: cut every frond whose spine the stroke segment (x0,y0)-(x1,y1)
 * crosses, to the height where it crosses (only ever DOWN, never below nubs).
 * The segment reaches SLASH_REACH_PX past its far end - the finger is a pad,
 * not a point, and the frond just ahead of the reported point at lift is
 * under it (mid-stroke the next segment crosses that frond anyway, at the
 * same height); `landing` (the retroactive first segment of a stroke) reaches
 * the same distance back past its start, for the frond under the finger as
 * it touched down. A frond in the reach is cut at the height of that end.
 * Returns the number of fronds cut. */
static int veg_cut(tank_t *t, float x0, float y0, float x1, float y1, bool landing, int wall) {
    int cuts = 0;
    float lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
    if (x1 >= x0) hi += SLASH_REACH_PX; else lo -= SLASH_REACH_PX;
    if (landing) { if (x1 >= x0) lo -= SLASH_REACH_PX; else hi += SLASH_REACH_PX; }
    if (wall < 0 && x1 < x0 && x1 <= TANK_FX0 + SLASH_WALL_PX) lo = TANK_FX0;                  /* aimed at a wall frond (SLASH_WALL_SIDE_PX) */
    if (wall > 0 && x1 > x0 && x1 >= TANK_FX1 - SLASH_WALL_PX) hi = TANK_FX1;
    float dx = x1 - x0;
    for (int b = 0; b < tank_veg_beds(t); b++) {
        float bx0; int n; veg_bed_base(t, b, &bx0, &n);
        for (int i = 0; i < n; i++) {
            float fx = bx0 + i * veg_pitch(b);
            if (fx < lo || fx > hi) continue;
            float u = fabsf(dx) > 0.001f ? clampf((fx - x0) / dx, 0, 1) : 0;
            float cy = y0 + (y1 - y0) * u;
            float hf = (TANK_BOT - 16 - cy) / ((VEG_SEGS_FULL - 1) * VEG_SEG_PX);
            if (hf < VEG_NUB) hf = VEG_NUB;
            if (hf >= t->veg_h[b][i]) continue;    /* the stroke passed above the tip */
            t->trim_px += (t->veg_h[b][i] - hf) * (VEG_SEGS_FULL - 1) * VEG_PAY_PX;   /* the inches (sand dollars) */
            t->veg_h[b][i] = hf;
            cuts++;
            int puffs = 2;                         /* cut leaves drift up */
            for (int k = 0; k < MAX_BUBBLE && puffs > 0; k++) {
                if (t->bubble[k].column) continue;
                t->bubble[k].x = fx + tank_randf(t, -4, 4);
                t->bubble[k].y = cy - tank_randf(t, 0, 6);
                puffs--;
            }
        }
    }
    if (cuts) veg_sync(t);
    return cuts;
}

/* a film cell that is on the glass: every one in the rectangle; in the bowl,
 * the ones whose centre the circle holds (the grid is the square around it) */
static bool algae_ok(int cx, int cy) {
#ifdef TANK_ROUND
    if (cx < 0 || cx >= ALGAE_COLS || cy < 0 || cy >= ALGAE_ROWS) return false;
    float dx = cx * ALGAE_CELL + ALGAE_CELL * 0.5f - TANK_RAD, dy = cy * ALGAE_CELL + ALGAE_CELL * 0.5f - TANK_RAD;
    return dx * dx + dy * dy <= (TANK_RAD - 6) * (TANK_RAD - 6);
#elif defined(TANK_CORNER_R)                               /* the watch: not the cells its round corners hide (the last column and
                                                              row hang off the frame: their centres are still on the glass) */
    float px = cx * ALGAE_CELL + ALGAE_CELL * 0.5f, py = cy * ALGAE_CELL + ALGAE_CELL * 0.5f, ccx, ccy;
    if (px > TANK_W - 1) px = TANK_W - 1;
    if (py > TANK_H - 1) py = TANK_H - 1;
    return corner_in(px, py, &ccx, &ccy) <= TANK_CORNER_R - 4;
#else
    (void)cx; (void)cy; return true;
#endif
}
int tank_algae_cells(void) {
    static int n;
    if (!n) for (int i = 0; i < ALGAE_CELLS; i++) n += algae_ok(i % ALGAE_COLS, i / ALGAE_COLS);
    return n;
}
float tank_algae_cover(const tank_t *t) {
    int covered = 0;
    for (int i = 0; i < ALGAE_CELLS; i++) covered += t->algae[i] > 0;
    return (float)covered / tank_algae_cells();
}

/* one film step: thicken a covered cell, or claim a fresh one (preferring
 * cells next to existing film, then the glass edges - the way a real tank
 * fouls from the corners in) until the dapple cap is reached */
void tank_grow_algae(tank_t *t, int steps) {
    for (int s = 0; s < steps; s++) {
        int covered = 0;
        for (int i = 0; i < ALGAE_CELLS; i++) covered += t->algae[i] > 0;
        bool claim = covered < (int)(tank_algae_cells() * ALGAE_COVER_CAP);
        for (int try = 0; try < 8; try++) {
            int cx = (int)tank_randf(t, 0, ALGAE_COLS - 0.001f);
            int cy = (int)tank_randf(t, 0, ALGAE_ROWS - 0.001f);
            uint8_t *cell = &t->algae[cy * ALGAE_COLS + cx];
            if (!algae_ok(cx, cy)) continue;
            if (*cell) { *cell = (uint8_t)(*cell > 210 ? 255 : *cell + 45); break; }
            if (!claim) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1 && !near; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx >= 0 && nx < ALGAE_COLS && ny >= 0 && ny < ALGAE_ROWS)
                        near = t->algae[ny * ALGAE_COLS + nx] > 0;
                }
            bool edge = cx == 0 || cy == 0 || cx == ALGAE_COLS - 1 || cy == ALGAE_ROWS - 1
                     || !algae_ok(cx - 1, cy) || !algae_ok(cx + 1, cy) || !algae_ok(cx, cy - 1) || !algae_ok(cx, cy + 1);
            float p = near ? 1.0f : edge ? 0.5f : 0.10f;
            if (tank_randf(t, 0, 1) < p) { *cell = 90; break; }
        }
    }
}

/* label the film's connected patches (8-neighbour): lab[i] = 1.. per cell
 * with film, 0 without; returns the patch count. A 28 x 23 grid, an
 * iterative flood fill - a few thousand steps, once per wiping frame. */
static int algae_label(const uint8_t *algae, uint8_t *lab) {
    int n = 0;
    int16_t stack[ALGAE_CELLS];
    for (int i = 0; i < ALGAE_CELLS; i++) lab[i] = 0;
    for (int i = 0; i < ALGAE_CELLS; i++) {
        if (!algae[i] || lab[i]) continue;
        if (n >= 255) break;                               /* the label is a byte; the cap is 30% cover anyway */
        n++; int sp = 0; stack[sp++] = (int16_t)i; lab[i] = (uint8_t)n;
        while (sp) {
            int c = stack[--sp], cx = c % ALGAE_COLS, cy = c / ALGAE_COLS;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || nx >= ALGAE_COLS || ny < 0 || ny >= ALGAE_ROWS) continue;
                    int j = ny * ALGAE_COLS + nx;
                    if (algae[j] && !lab[j]) { lab[j] = (uint8_t)n; stack[sp++] = (int16_t)j; }
                }
        }
    }
    return n;
}
/* wipe the film in a WIPE_RADIUS band around the segment (x0,y0)-(x1,y1).
 * A patch whose last cell goes under the keeper's stroke is a COLONY removed
 * (tank_t.algae_colonies, the sand dollars' chore count, 2026-09-15). */
static void wipe_algae(tank_t *t, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    uint8_t lab[ALGAE_CELLS]; int patches = -1;             /* labelled lazily: only a stroke that clears something pays the fill */
    int wiped = 0;
    for (int cy = 0; cy < ALGAE_ROWS; cy++)
        for (int cx = 0; cx < ALGAE_COLS; cx++) {
            uint8_t *cell = &t->algae[cy * ALGAE_COLS + cx];
            if (!*cell) continue;
            float px = cx * ALGAE_CELL + ALGAE_CELL * 0.5f;
            float py = cy * ALGAE_CELL + ALGAE_CELL * 0.5f;
            float u = len2 > 1 ? clampf(((px - x0) * dx + (py - y0) * dy) / len2, 0, 1) : 0;
            if (tank_dist(px, py, x0 + dx * u, y0 + dy * u) < WIPE_RADIUS) {
                if (patches < 0) patches = algae_label(t->algae, lab);
                *cell = 0;
                t->cells_cleaned++; wiped++;
            }
        }
    if (wiped && patches > 0) {
        bool alive[256] = { false };
        for (int i = 0; i < ALGAE_CELLS; i++) if (t->algae[i] && lab[i]) alive[lab[i]] = true;
        for (int p = 1; p <= patches; p++) {
            bool touched = false;                          /* only a patch this stroke reached can have gone */
            for (int i = 0; i < ALGAE_CELLS && !touched; i++) touched = lab[i] == p && !t->algae[i];
            if (touched && !alive[p]) t->algae_colonies++;
        }
    }
}

void tank_plant_place(tank_t *t) {
    tank_veg_set(t, 3, VEG_START);                          /* a young plant; it grows from here */
}
void tank_castle_place(tank_t *t) {
    t->castle_x = 0; t->castle_z = DECOR_Z_FRONT;          /* the default spot, the fish swim through */
}
void tank_coral_place(tank_t *t) {
    t->coral_x = 0; t->coral_z = DECOR_Z_FRONT;            /* the default spot, in front of the reef bed's grass; the colour stays the keeper's */
    t->coral_growth = CORAL_START;                          /* young: it grows from here */
}
uint32_t tank_coral_rgb(const tank_t *t) { return t->coral_rgb ? t->coral_rgb : CORAL_PAL[0]; }
float    tank_coral_growth(const tank_t *t) { return t->coral_growth <= 0 ? CORAL_FULL : t->coral_growth > CORAL_FULL ? CORAL_FULL : t->coral_growth; }
static void coral_grow(tank_t *t, float seconds) {          /* the same pace awake and asleep */
    if (!(t->sd_unlocks & SD_ITEM_CORAL) || t->coral_growth <= 0 || t->coral_growth >= CORAL_FULL) return;
    t->coral_growth = fminf(CORAL_FULL, t->coral_growth + seconds / CORAL_GROW_S);
}
void tank_cluster_place(tank_t *t) {
    t->cluster_x = 0; t->cluster_z = DECOR_Z_FRONT;         /* the default spot; the look stays the keeper's */
    t->cluster_growth = CLUSTER_START + 1e-4f;              /* > 0: growing (0 in a save = full, tank_cluster_growth) */
}
void tank_pumpkin_place(tank_t *t) {
    t->pumpkin_x = 0; t->pumpkin_z = DECOR_Z_BACK;          /* the default spot, behind the grass: the glow through the blades */
}
void tank_volcano_place(tank_t *t) {
    t->volcano_x = 0; t->volcano_z = DECOR_Z_BACK;          /* the default spot, a backdrop behind the grass (0.3.9) */
}
float tank_cluster_growth(const tank_t *t) { return t->cluster_growth <= 0 ? CLUSTER_FULL : t->cluster_growth > CLUSTER_FULL ? CLUSTER_FULL : t->cluster_growth; }
int   tank_cluster_scheme(const tank_t *t) { return t->cluster_scheme < CLUSTER_SCHEME_N ? t->cluster_scheme : 0; }
void  tank_cluster_set_scheme(tank_t *t, int i) { t->cluster_scheme = (uint8_t)(i < 0 ? 0 : i >= CLUSTER_SCHEME_N ? CLUSTER_SCHEME_N - 1 : i); }
static void cluster_grow(tank_t *t, float seconds) {
    if (!(t->sd_unlocks & SD_ITEM_CLUSTER) || t->cluster_growth <= 0 || t->cluster_growth >= CLUSTER_FULL) return;
    t->cluster_growth = fminf(CLUSTER_FULL, t->cluster_growth + seconds / CLUSTER_GROW_S);
}
void     tank_coral_set_rgb(tank_t *t, uint32_t rgb) { t->coral_rgb = rgb & 0xffffff; }

int tank_deco_index(int item) { return PT_TERRARIUM && item >= TERR_DECO_ITEM0 && item < TERR_DECO_ITEM0 + TERR_DECO_N ? item - TERR_DECO_ITEM0 : -1; }

/* the decor's spot and layer (see tank.h): item 0 is the plant, item 2 the castle, item 3 the coral, item 4 the cluster, item 8 the pumpkin, item 9 the volcano */
bool  tank_decor_placeable(int item) { return item == 0 || item == 2 || item == 3 || item == 4 || item == 8 || item == 9 || tank_deco_index(item) >= 0; }
float tank_decor_half_w(int item) { return item == 0 ? PLANT_HALF_W : item == 2 ? CASTLE_HALF_W : item == 3 ? CORAL_HALF_W : item == 4 ? CLUSTER_HALF_W : item == 8 ? PUMPKIN_HALF_W : item == 9 ? VOLCANO_HALF_W
                                        : tank_deco_index(item) >= 0 ? DECO_HW[tank_deco_index(item)] : 0; }
/* every piece BEHIND or IN FRONT - the plant too since 0.3.9.3 (Strato: "we can let the weaving go ... either grass is
   in front or behind"): its AMONG, the fronds woven with the fish, is gone; a saved AMONG loads BEHIND */
/* ... except the sword plant since 0.4 (Strato 11-02, on the watch: BEHIND THE SEAGRASS never drew behind it - the plant
   was always over the grass): ONE depth, the back lane it is baked in with the grass loop (no frame cost), drawn over the
   seagrass. A saved IN FRONT loads BEHIND. Its order among the pieces (the ORDER arrows) stays. Behind the grass for real:
   0.4.1+ (the back passes draw bed 3 last - draw it first) */
int   tank_decor_z_count(int item) { return item == 0 || tank_deco_index(item) >= 0 ? 1 : 2; }   /* (a decoration: one depth, behind the lizards) */
int   tank_decor_z_at(int item, int i) { return item == 0 || tank_deco_index(item) >= 0 || i <= 0 ? DECOR_Z_BACK : DECOR_Z_FRONT; }
int   tank_decor_z_index(int item, int z) { return item != 0 && tank_deco_index(item) < 0 && z == DECOR_Z_FRONT ? 1 : 0; }
/* the seagrass stays BEHIND the fish, where the keeper cannot move it (Strato 0.3.9.3: a held page for the beds would bring
   back the edits the decoration LOCK just quieted, "to support something no one has asked for"); the sword plant is a piece */
int   tank_bed_z(const tank_t *t, int b) { return b == 3 && t->plant_z == DECOR_Z_FRONT ? DECOR_Z_FRONT : DECOR_Z_BACK; }
float tank_decor_x(const tank_t *t, int item) {
    { const int d = tank_deco_index(item);                     /* a decoration: its saved spot, or its own; either slid clear (a save
                                                                  from another glass keeps the rule too) */
      if (d >= 0) return tank_deco_slide(t, item, t->deco_x[d] ? t->deco_x[d] * 2.0f : tank_floor_x(DECO_U[d])); }
    if (item == 2) return t->castle_x > 0 ? t->castle_x : CASTLE_X_DEFAULT;
    if (item == 3) return t->coral_x > 0 ? t->coral_x : CORAL_X_DEFAULT;
    if (item == 4) return t->cluster_x > 0 ? t->cluster_x : CLUSTER_X_DEFAULT;
    if (item == 8) return t->pumpkin_x > 0 ? t->pumpkin_x : PUMPKIN_X_DEFAULT;
    if (item == 9) return t->volcano_x > 0 ? t->volcano_x : VOLCANO_X_DEFAULT;
    if (item != 0) return 0;
    return t->plant_x > 0 ? t->plant_x : PLANT_X_DEFAULT;
}
int tank_decor_z(const tank_t *t, int item) { return item == 0 ? t->plant_z : item == 2 ? t->castle_z : item == 3 ? t->coral_z : item == 4 ? t->cluster_z : item == 8 ? t->pumpkin_z : item == 9 ? t->volcano_z
                                                     : tank_deco_index(item) >= 0 ? DECOR_Z_BACK : DECOR_Z_MIDDLE; }
float tank_decor_fy(const tank_t *t, int item) { return tank_deco_index(item) >= 0 ? TERR_DECO_FY : (float)DECOR_FY(tank_decor_z(t, item)); }   /* the floor line it stands on: by its depth (0.3.9) */
float tank_decor_height(int item) { return item == 2 ? 146 : item == 3 ? 92 : item == 4 ? 120 : item == 8 ? 82 : item == 9 ? 200
                                           : tank_deco_index(item) >= 0 ? DECO_H[tank_deco_index(item)] : 0; }   /* its highest pixel over that line */
static float decor_top(const tank_t *t, int item) {         /* the piece's highest pixel */
    if (item == 0) { float top; tank_veg_bed(t, 3, NULL, NULL, &top, NULL); return top; }
    return tank_decor_fy(t, item) - tank_decor_height(item);
}
int tank_decor_hit(const tank_t *t, float x, float y) {
    static const int8_t DRAW[TERR_DECO_N] = TERR_DECO_DRAW;
    for (int k = TERR_DECO_N - 1; PT_TERRARIUM && k >= 0; k--) {    /* a decoration (the last drawn first): its own box, above its feet - the
                                                                       hand on the sand under it stays the hand, and so does a hold on a
                                                                       lizard in front of it (the hand hold is the trust's, P2c) */
        const int d = DRAW[k], item = TERR_DECO_ITEM0 + d;
        if (!tank_decor_in(t, item) || tank_decor_locked(t, item)) continue;
        const float cx = tank_decor_x(t, item), half = DECO_HW[d] + 6;
        if (x >= cx - half && x <= cx + half && y >= TERR_DECO_FY - DECO_H[d] - 6 && y <= TERR_DECO_FY - 2) {
            for (int i = 0; i < t->n_fish; i++)
                if (tank_swims(t, i) && tank_tap_d2(t, i, t->fish[i].x, t->fish[i].y, x, y) < 16.0f * 16.0f) return -1;
            return item;
        }
    }
    if (PT_TERRARIUM) return -1;                                /* no desert hit: the hand, never an invisible aquarium piece from old bits/default spots */
    static const int order[6] = { 3, 0, 8, 2, 4, 9 };         /* the coral, the plant, the pumpkin, the castle, the cluster, the volcano */
    for (int k = 0; k < 6; k++) {
        int item = order[k];
        if (!tank_decor_in(t, item)) continue;                    /* a piece on the shelf is not under the finger (0.3.9) */
        if (tank_decor_locked(t, item)) continue;                 /* a LOCKED piece is passed by: the hold reaches whatever is behind it, or the water (0.3.9) */
        float cx = tank_decor_x(t, item), half = tank_decor_half_w(item) + 8;
        if (x >= cx - half && x <= cx + half && y >= decor_top(t, item) - 8 && y <= TANK_BOT) return item;
    }
    return -1;
}
/* a poof: a burst of bubbles where a fish was (or comes back from) - 0.4's
 * tank link, the same code here so the merge forward meets itself */
void tank_poof(tank_t *t, float x, float y) {
    int n = 0;
    for (int i = 0; i < MAX_BUBBLE && n < 12; i++) {
        bubble_t *b = &t->bubble[i];
        if (b->column) continue;
        b->x = x + tank_randf(t, -12, 12); b->y = y + tank_randf(t, -10, 10);
        b->vy = tank_randf(t, 35, 70); b->wobble = tank_randf(t, 0, 6.28f);
        n++;
    }
}
/* the shelf's plume (0.3.9): the free bubbles across the piece's footprint,
 * from its top to the floor, rising fast - the piece pops away into them */
void tank_decor_poof(tank_t *t, int item) {
    if (!tank_decor_placeable(item) || tank_deco_index(item) >= 0) return;   /* (a terrarium's decoration: no water to bubble - it just goes) */
    float cx = tank_decor_x(t, item), hw = tank_decor_half_w(item) * 0.8f, top = decor_top(t, item);
    int n = 0;
    for (int i = 0; i < MAX_BUBBLE && n < 16; i++) {
        bubble_t *b = &t->bubble[i];
        if (b->column) continue;
        b->x = cx + tank_randf(t, -hw, hw); b->y = tank_randf(t, top, TANK_BOT - 12);
        b->vy = tank_randf(t, 40, 80); b->wobble = tank_randf(t, 0, 6.28f);
        n++;
    }
}
/* ---- the order of the pieces (2026-10-17; tank.h) ---- */
static const int DECOR_SLOT_ITEM[DECOR_SLOTS] = { 2, 3, 4, 8, 0, 9 };              /* the tie order: castle, coral, cluster, pumpkin, plant, volcano */
static const uint32_t DECOR_SLOT_BIT[DECOR_SLOTS] = { SD_ITEM_CASTLE, SD_ITEM_CORAL, SD_ITEM_CLUSTER, SD_ITEM_PUMPKIN, SD_ITEM_PLANT, SD_ITEM_VOLCANO };
int  tank_decor_slot(int item) { for (int s = 0; s < DECOR_SLOTS; s++) if (DECOR_SLOT_ITEM[s] == item) return s; return -1; }
int  tank_decor_slot_item(int slot) { return slot >= 0 && slot < DECOR_SLOTS ? DECOR_SLOT_ITEM[slot] : -1; }
static uint32_t decor_bit(int item) {                           /* a piece's sd_unlocks bit: its slot's, or a decoration's */
    const int s = tank_decor_slot(item), d = tank_deco_index(item);
    return s >= 0 ? DECOR_SLOT_BIT[s] : d >= 0 ? DECO_BIT[d] : 0;
}
bool tank_decor_owned(const tank_t *t, int item) { const uint32_t b = decor_bit(item); return b && (t->sd_unlocks & b); }
bool tank_decor_in(const tank_t *t, int item)    { const uint32_t b = decor_bit(item); return b && (t->sd_unlocks & b) && !(t->sd_shelved & b); }
bool tank_decor_locked(const tank_t *t, int item) { const uint32_t b = decor_bit(item); return b && (t->sd_unlocks & b) && (t->sd_locked & b) != 0; }
static bool decor_before(const tank_t *t, int ia, int ib) {       /* a is painted before (under) b at one depth */
    int sa = tank_decor_slot(ia), sb = tank_decor_slot(ib);
    return t->decor_rank[sa] != t->decor_rank[sb] ? t->decor_rank[sa] < t->decor_rank[sb] : sa < sb;
}
int  tank_decor_rank(const tank_t *t, int item) { int s = tank_decor_slot(item); return s < 0 ? 0 : t->decor_rank[s]; }
int  tank_decor_order(const tank_t *t, int items[DECOR_SLOTS]) {
    int n = 0;
    for (int s = 0; s < DECOR_SLOTS; s++) if ((t->sd_unlocks & DECOR_SLOT_BIT[s]) && !(t->sd_shelved & DECOR_SLOT_BIT[s])) items[n++] = DECOR_SLOT_ITEM[s];   /* the shelf is not in the water (0.3.9) */
    for (int i = 1; i < n; i++) {                                 /* an insertion sort of five */
        int v = items[i], j = i;
        while (j > 0 && decor_before(t, v, items[j - 1])) { items[j] = items[j - 1]; j--; }
        items[j] = v;
    }
    return n;
}
bool tank_decor_above(const tank_t *t, int a, int b) {
    int za = tank_decor_z(t, a), zb = tank_decor_z(t, b);
    return za != zb ? za > zb : decor_before(t, b, a);
}
static void decor_normalize(tank_t *t) {                          /* ranks 1..n in the current order (ties resolved) */
    int items[DECOR_SLOTS], n = tank_decor_order(t, items);
    for (int i = 0; i < n; i++) t->decor_rank[tank_decor_slot(items[i])] = (uint8_t)(i + 1);
}
void tank_decor_raise(tank_t *t, int item) {
    int s = tank_decor_slot(item); if (s < 0) return;
    decor_normalize(t);
    int top = 0; for (int k = 0; k < DECOR_SLOTS; k++) if (t->decor_rank[k] > top) top = t->decor_rank[k];
    t->decor_rank[s] = (uint8_t)(top + 1);
}
int  tank_decor_neighbor(const tank_t *t, int item, int dir) {
    if (tank_decor_slot(item) < 0 || !tank_decor_in(t, item) || tank_decor_z(t, item) == DECOR_Z_MIDDLE) return -1;      /* the woven plant has no stack; nor has the shelf, nor a decoration */
    int items[DECOR_SLOTS], n = tank_decor_order(t, items), at = -1;
    for (int i = 0; i < n; i++) if (items[i] == item) at = i;
    for (int i = at + dir; i >= 0 && i < n; i += dir) if (tank_decor_z(t, items[i]) == tank_decor_z(t, item)) return items[i];
    return -1;
}
bool tank_decor_shift(tank_t *t, int item, int dir) {
    int other = tank_decor_neighbor(t, item, dir);
    if (other < 0) return false;
    decor_normalize(t);
    uint8_t *a = &t->decor_rank[tank_decor_slot(item)], *b = &t->decor_rank[tank_decor_slot(other)], v = *a; *a = *b; *b = v;
    return true;
}
void tank_decor_reset(tank_t *t, int item) {
    { int s = tank_decor_slot(item); if (s >= 0) t->decor_rank[s] = 0; }
    if (item == 0) { t->plant_x = 0; t->plant_z = DECOR_Z_BACK; tank_veg_set(t, 3, VEG_START); }
    if (item == 2) { t->castle_x = 0; t->castle_z = DECOR_Z_FRONT; }
    if (item == 3) { t->coral_x = 0; t->coral_z = DECOR_Z_FRONT; t->coral_rgb = 0; t->coral_growth = 0; t->coral_acc = 0; }
    if (item == 4) { t->cluster_x = 0; t->cluster_z = DECOR_Z_FRONT; t->cluster_scheme = 0; t->cluster_growth = 0; t->cluster_acc = 0; }
    if (item == 8) tank_pumpkin_place(t);
    if (item == 9) tank_volcano_place(t);
    if (tank_deco_index(item) >= 0) t->deco_x[tank_deco_index(item)] = 0;   /* (a decoration: its own clear spot again) */
}
void tank_decor_set(tank_t *t, int item, float x, int z) {
    if (!tank_decor_placeable(item)) return;
    { const int d = tank_deco_index(item);                     /* a decoration: slid to the nearest clear spot, kept to 2 px - a clear one */
      if (d >= 0) {
          int v = (int)lroundf(tank_deco_slide(t, item, x) * 0.5f);
          if (!tank_deco_clear(t, item, v * 2.0f)) v += tank_deco_clear(t, item, (v - 1) * 2.0f) ? -1 : 1;
          t->deco_x[d] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v); (void)z; return;
      } }
    float half = tank_decor_half_w(item), lo = TANK_FX0 + DECOR_MARGIN + half, hi = TANK_FX1 - DECOR_MARGIN - half;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    if (z < 0) z = 0;
    if (z >= DECOR_Z_N) z = DECOR_Z_N - 1;
    uint8_t z2 = (uint8_t)(z == DECOR_Z_BACK ? DECOR_Z_BACK : DECOR_Z_FRONT);   /* no AMONG for these */
    if (item == 2) { t->castle_x = x; t->castle_z = z2; return; }
    if (item == 3) { t->coral_x = x; t->coral_z = z2; return; }
    if (item == 4) { t->cluster_x = x; t->cluster_z = z2; return; }
    if (item == 8) { t->pumpkin_x = x; t->pumpkin_z = z2; return; }
    if (item == 9) { t->volcano_x = x; t->volcano_z = z2; return; }
    (void)z; t->plant_x = x; t->plant_z = DECOR_Z_BACK;   /* one depth (0.4): an old AMONG or IN FRONT loads BEHIND */
}

void tank_touch_hold(tank_t *t, float x, float y) {
    if (t->packs_missing) return;                /* the packs are missing: a paused tank takes no hand (tank.h packs_missing) */
    tank_handled(t);
    if (t->tool == TOOL_MISTER) { t->mist_hold = true; t->mist_x = x; t->mist_y = y; return; }   /* the mister held on a lizard: the soak (touch_tick) */
    if (t->tool != TOOL_HAND) return;            /* a tool in hand: the glass takes its strokes and nothing else (2026-10-04) */
    t->hold_active = true; t->hold_x = x; t->hold_y = y;
}

void tank_touch_drag(tank_t *t, float x, float y) {
    if (t->packs_missing) return;
    tank_handled(t);
    t->drag_active = true;
    if (!t->drag_has_prev) {
        /* stroke start: a slash must BEGIN on a plant - within SLASH_START_PX
         * of an actual frond (beside its spine, no higher than its tip). A
         * cleaning scrub that starts mid-glass, even straight above a tall
         * bed, and then works down through the grass arms nothing: it keeps
         * wiping algae and the fronds stand. */
        t->slash_armed = t->tool == TOOL_SCISSORS;   /* the scissors: any stroke (below) */
        for (int b = 0; b < tank_veg_beds(t) && !t->slash_armed; b++)
            t->slash_armed = veg_near_frond(t, b, x, y, SLASH_START_SIDE_PX, SLASH_START_PX);
        /* ... or beside a wall frond, on no frond of that frond's own bed
         * (SLASH_WALL_SIDE_PX; another bed's canopy under the finger does
         * not make the stroke any less aimed at the wall) */
        t->slash_wall = 0;
        for (int b = 0; b < tank_veg_beds(t) && !t->slash_wall; b++)
            if (!veg_near_frond(t, b, x, y, SLASH_START_SIDE_PX, SLASH_START_PX))
                t->slash_wall = (int8_t)veg_wall_frond(t, b, x, y, SLASH_START_PX);
        if (t->slash_wall) t->slash_armed = true;
        if (t->tool == TOOL_SPONGE || t->tool == TOOL_MISTER || (t->tool == TOOL_HAND && t->tools_locked)) { t->slash_armed = false; t->slash_wall = 0; }   /* the sponge and the mister never cut, nor does a locked bare finger */
        t->slash_engaged = t->slash_cut = false; t->wipe_sounded = false;
        t->slash_x0 = x; t->slash_y0 = y; t->slash_h = t->slash_v = 0;
    }
    if (t->tool == TOOL_MISTER) {                     /* a stroke or a tap over a humid hide: one pass each (docs/shed.md), once a stroke */
        const habitat_t *h = tank_habitat(t);
        for (int k = 0; k < h->n_hides; k++) {
            if (h->hides[k].kind != HIDE_HUMID || (t->mist_stroke >> k & 1)) continue;
            float hx, hy; tank_hide_point(t, k, &hx, &hy);
            if (tank_dist(x, y, hx, hy) < MIST_HIDE_R) { tank_mist_pass(t, k); t->mist_stroke |= (uint8_t)(1u << k); }
        }
    }
    if (t->drag_has_prev) {
        float sdx = x - t->drag_px, sdy = y - t->drag_py;
        t->drag_dist += tank_dist(x, y, t->drag_px, t->drag_py);
        bool wipes = t->tool == TOOL_SPONGE || (t->tool == TOOL_HAND && !t->tools_locked);   /* the scissors never wipe, nor does a locked bare finger */
        if (wipes && t->drag_dist >= WIPE_ENGAGE_PX) {   /* a real stroke, not a tap */
            wipe_algae(t, t->drag_px, t->drag_py, x, y);
            if (!t->wipe_sounded) { t->wipe_sounded = true; tank_emit(TEV_WIPE, -1); }
        }
        /* the slash (2026-09-04, per frond): an armed stroke becomes scissors
         * once it has travelled SLASH_PX sideways, mostly sideways - deliberate
         * work, so a tap or a missed poke at a fish never shears the garden.
         * From then on every sideways segment cuts the fronds it crosses at
         * the height it crosses them (the travel before engagement is cut
         * retroactively as one straight segment from the stroke start), so
         * a flick takes one or two fronds at the finger's height and a sweep
         * along the floor mows the bed to nubs. */
        if (t->slash_armed) {
            t->slash_h += fabsf(sdx); t->slash_v += fabsf(sdy);
            int cuts = 0;
            if (!t->slash_engaged) {
                if (t->tool == TOOL_SCISSORS ? t->slash_h >= SLASH_TOOL_PX && t->slash_h > t->slash_v
                                             : t->slash_h >= SLASH_PX && t->slash_h > SLASH_RATIO * t->slash_v) {
                    t->slash_engaged = true;
                    cuts += veg_cut(t, t->slash_x0, t->slash_y0, x, y, true, t->slash_wall);
                }
            } else if (fabsf(sdx) >= fabsf(sdy))   /* only the sideways segments cut */
                cuts += veg_cut(t, t->drag_px, t->drag_py, x, y, false, t->slash_wall);
            if (cuts) tank_emit(TEV_SNIP, -1);
            if (cuts && !t->slash_cut) { t->slash_cut = true; t->trims++; }
        }
    }
    t->drag_px = x; t->drag_py = y; t->drag_has_prev = true;
    t->tool_idle = 0;
}

void tank_set_tool(tank_t *t, int tool) {
    const bool mister = tool == TOOL_MISTER && tank_species(t)->sheds;   /* (the aquarium's fish do not shed: no mister in its box) */
    t->tool = (uint8_t)(tool == TOOL_SPONGE || tool == TOOL_SCISSORS || mister ? tool : TOOL_HAND);
    t->tool_idle = 0;
    t->mist_hold = t->mist_done = false; t->mist_fish = -1; t->mist_s = 0;
}

void tank_lock_tools(tank_t *t, bool lock) { t->tools_locked = lock; }
bool tank_stroking(const tank_t *t) { return t->drag_has_prev && t->drag_dist >= WIPE_ENGAGE_PX; }

void tank_feed(tank_t *t, float x, int n) {
    if (t->packs_missing) return;
    tank_handled(t);
    if (PT_TERRARIUM) {                          /* THE CRICKETS (tank.h): n from the hand at x - or, over the dish, the dish filled */
        float dx = 0, dy;
        if (tank_dish_here(t)) tank_dish_point(t, &dx, &dy);
        if (tank_dish_here(t) && fabsf(x - dx) <= DISH_FEED_PX) {
            if (t->dish_worms < DISH_WORMS) { t->dish_worms = DISH_WORMS; t->dish_farm = false; tank_emit(TEV_FEED, -1); }   /* (the keeper's dish now: P5) */
        } else {
            int dropped = 0;
            for (int i = ck_free(t); i >= 0 && n > 0; i = ck_free(t), n--) {   /* (the draws a pellet's were: its jitter, its height) */
                const float cx = ck_drop_x(t, x + tank_randf(t, -14, 14));
                ck_drop_at(t, i, cx, tank_glass_top(cx) + tank_randf(t, 6, 18), true);
                dropped++;
            }
            if (dropped) tank_emit(TEV_FEED, -1);
        }
        t->feed_spot_x = t->feed_spot_x < 0 ? x : t->feed_spot_x + (x - t->feed_spot_x) * 0.3f;
        t->feed_open = true;                     /* a meal once a lizard eats from it */
        return;
    }
    x = clampf(x, TANK_FX0 + 25, TANK_FX1 - 25);
    int dropped = 0;
    for (int i = 0; i < MAX_FOOD && n > 0; i++) {
        if (t->food[i].alive) continue;
        t->food[i].alive = true; t->food[i].from_player = true;
        t->food[i].x = clampf(x + tank_randf(t, -14, 14), TANK_FX0 + 20, TANK_FX1 - 20);
        t->food[i].y = tank_glass_top(t->food[i].x) + tank_randf(t, 6, 18);
        t->food[i].age = 0; t->food[i].floor_s = 0; t->food[i].nibbled = 0;
        n--; dropped++;
    }
    if (dropped) tank_emit(TEV_FEED, -1);        /* the plink is for pellets, not for the tap (a full tank drops none) */
    t->feed_spot_x = t->feed_spot_x < 0 ? x : t->feed_spot_x + (x - t->feed_spot_x) * 0.3f;
    t->feed_open = true;                         /* a meal once somebody eats from it */
}

/* the triple tap: fish near it bolt (stress, a little trust lost) and the
 * shrimp scatter - from the water or from the school alike */
/* a visitor spooked by the keeper (the triple tap, or a tap while the
   spook lasts) high-tails it home: its door, at a flight's speed */
static void spook_visitors(tank_t *t, float x, float y) {
    for (int i = N_FISH_MAX; i < N_FISH_SLOTS; i++)
        if (tank_swims(t, i) && tank_dist(t->fish[i].x, t->fish[i].y, x, y) < STARTLE_RADIUS * 1.6f) {
            fish_t *f = &t->fish[i];
            f->homeward = f->bolt_home = true; f->hesitate = 0;
        }
}
void tank_startle(tank_t *t, float x, float y) {
    tank_emit(TEV_SPOOK, -1);
    t->startled = true; t->startle_x = x; t->startle_y = y; t->startle_cooldown = STARTLE_COOLDOWN;
    for (int i = 0; i < N_FISH_SLOTS; i++) {
        if (!tank_swims(t, i)) continue;
        fish_t *f = &t->fish[i];
        if (tank_dist(f->x, f->y, x, y) < STARTLE_RADIUS) {
            f->stress = fminf(10, f->stress + 2.5f);
            f->trust = fmaxf(0, f->trust - 0.4f);
        }
        if (PT_TERRARIUM && tank_dist(f->x, f->y, x, y) < STARTLE_RADIUS * 1.6f) lizard_spook(t, i);   /* THE RAP: a dash for cover */
    }
    if (PT_TERRARIUM) ck_scare(t, x, y, STARTLE_RADIUS);
    spook_visitors(t, x, y);
    shrimp_scatter(t, x, y, STARTLE_RADIUS);
    tank_crab_scare(t, x);                       /* the crab feels the knock anywhere on the glass: it runs for cover (0.3.9) */
}

void tank_touch_tap(tank_t *t, float x, float y) {
    if (t->packs_missing) return;
    tank_handled(t);
    if (t->tool != TOOL_HAND) return;            /* a tool in hand: no feed, no light, no startle - DONE first (2026-10-04) */
    if (y - tank_glass_top(x) < FEED_ZONE_Y) { tank_feed(t, x, 3); return; }     /* surface tap = feed (the bowl's surface is its top glass) */
    if (t->tap_burst_t > TAP_WINDOW) t->tap_count = 0;
    t->tap_count++; t->tap_burst_t = 0; t->tap_x = x; t->tap_y = y;
    tank_emit(TEV_TAP, -1);
    if (t->startled) {                              /* chasing: keep them spooked */
        t->startle_x = x; t->startle_y = y; t->startle_cooldown = STARTLE_COOLDOWN;
        for (int i = 0; i < N_FISH_SLOTS; i++) if (tank_swims(t, i)) {
            t->fish[i].stress = fminf(10, t->fish[i].stress + 0.6f);
            if (PT_TERRARIUM && tank_dist(t->fish[i].x, t->fish[i].y, x, y) < STARTLE_RADIUS * 1.6f) lizard_spook(t, i);   /* (kept in its cover) */
        }
        if (PT_TERRARIUM) ck_scare(t, x, y, STARTLE_RADIUS);
        spook_visitors(t, x, y);
        shrimp_scatter(t, x, y, STARTLE_RADIUS);
    } else if (t->tap_count >= 3) tank_startle(t, x, y);   /* aggressive: engage */
}

void hold_approach(tank_t *t, int i) {
    t->fish[i].ms_bits |= MS_FIRST_HOLD_APPROACH;
    if (!t->hold_approached) { t->hold_approached = true; t->hold_approaches++; tank_emit(TEV_INVESTIGATE, i); }
}

/* per-frame bookkeeping for the touch state machine */
static void touch_tick(tank_t *t, float dt) {
    t->tap_burst_t += dt;
    t->shrimp_tap_t += dt;
    /* two taps then a pause: the light (MANUAL, the default; in settings'
       AUTO the idle rule owns it - 2026-09-15: the old always-on override
       rode in the save and froze a tank in permanent day) */
    if (!t->startled && t->tap_count == 2 && t->tap_burst_t > TAP_WINDOW) {
        if (!t->light_auto) {
            t->light_manual_off = !t->light_manual_off;
            if (t->light_manual_off) t->light_tip_seen = true;   /* the first time: the notice says what happened (2026-10-03) */
        }
        t->tap_count = 0;
    }
    if (t->tap_count >= 3 && t->tap_burst_t > TAP_WINDOW) t->tap_count = 0;
    if (t->startled) {
        t->startle_cooldown -= dt;
        if (t->startle_cooldown <= 0) { t->startled = false; t->tap_count = 0; }
    }
    if (t->hold_active) {                           /* calm presence earns trust */
        float was = t->hold_time;
        t->hold_time += dt;
        bool draw_begins = was < HOLD_ATTRACT_S && t->hold_time >= HOLD_ATTRACT_S;
        if (draw_begins) {                          /* the ring starts on the side the company comes from, and either way */
            float sx = 0, sy = 0;
            for (int i = 0; i < t->n_fish; i++) { sx += t->fish[i].x - t->hold_x; sy += t->fish[i].y - t->hold_y; }
            t->hold_ph = atan2f(sy, sx); t->hold_dir = tank_randf(t, 0, 1) < 0.5f ? -1 : 1; t->hold_turn_s = tank_randf(t, 12, 26);
        } else if (t->hold_time > HOLD_ATTRACT_S) {
            t->hold_ph = norm_ang(t->hold_ph + t->hold_dir * HOLD_RING_W * dt);
            if ((t->hold_turn_s -= dt) <= 0) { t->hold_dir = (int8_t)-t->hold_dir; t->hold_turn_s = tank_randf(t, 12, 26); }   /* a while one way, then back */
        }
        float hx, hy; tank_hand_point(t, &hx, &hy);    /* (the finger; a terrarium's hand on the ground nearest it: THE HAND) */
        for (int i = 0; i < t->n_fish; i++) {
            fish_t *f = &t->fish[i];
            if (f->away) continue;
            float d = tank_dist(f->x, f->y, hx, hy);
            if (d < HOLD_RADIUS) {
                /* 0.3.6: the base rate is slower than it was (0.02/s, 2026-09-04
                 * .. 0.3.5) and a hitched seahorse doubles it - "twice as fast"
                 * on its shop row. What it adds over the base is its tally. */
                float rate = TRUST_PER_S * (t->sh_hitched ? SEAHORSE_TRUST_X : 1.0f);
                float was_t = f->trust;
                f->trust = fminf(10, f->trust + dt * rate);
                if (t->sh_hitched) t->sh_trust_passed += fmaxf(0, (f->trust - was_t) - fminf(10 - was_t, dt * TRUST_PER_S));
            }
            /* a hold-approach = a fish that was out in the tank when the
             * settled hold began its draw and then came all the way in.
             * Per FISH (2026-09-15: it was gated on the per-hold flag below,
             * so only the first arrival - always the fastest, highest-trust
             * pair - could ever get it; the other two came in hold after
             * hold and were never credited), and only for a fish that
             * actually travelled (same day: a fish already sitting under the
             * finger used to be credited at 1.5 s without moving). The
             * tank's hold_approaches counter still counts the hold once. */
            if (!PT_TERRARIUM) {
                if (draw_begins) f->hold_far = d >= HOLD_APPROACH_FROM;
                if (f->hold_far && d < HOLD_APPROACH_AT) hold_approach(t, i);
            }   /* a lizard: its actual hand-directed walk and arrival below, not the aquarium ring's distances */
        }
    } else {
        t->hold_time = 0; t->hold_approached = false;
        for (int i = 0; i < t->n_fish; i++) t->fish[i].hold_far = false;
    }
    if (t->greet_timer > 0) t->greet_timer -= dt;
    /* courtship episodes: while progression says an arrival is close, the
     * pair circles the reef for a few seconds every minute or so - frequent
     * enough to notice across a couple of check-ins, rare enough to feel
     * like something glimpsed rather than an indicator */
    if (t->spawning && t->court_a >= 0 && t->court_b >= 0) {
        /* the spawning (2026-09-24): no episode clock - the pair circles in
         * the grass until the fry comes, lit or dark. The clock that counts
         * is the time BOTH spend in the fronds (a parent still swimming down,
         * or begging, holds it); the bubbles mark their arrival. */
        float sx, sy, sr; court_site(t, &sx, &sy, &sr);
        const fish_t *a = &t->fish[t->court_a], *b = &t->fish[t->court_b];
        if (tank_dist(a->x, a->y, sx, sy) < sr + SPAWN_NEAR_PX && tank_dist(b->x, b->y, sx, sy) < sr + SPAWN_NEAR_PX) {
            if (t->spawn_danced == 0) tank_court_puff(t, 2);
            t->spawn_danced += dt;
        }
        t->court_active = 0;
    } else if (t->courting && !t->night) {
        if (t->court_active > 0) t->court_active -= dt;
        else if ((t->court_cool -= dt) <= 0) {
            t->court_active = tank_randf(t, 6, 9);
            t->court_cool = tank_randf(t, 40, 90);
            tank_court_puff(t, 2);                  /* a flirt of bubbles, from the grass */
        }
    } else t->court_active = 0;
    /* the mister held on a lizard (docs/shed.md "The mister"): SOAK_HOLD_S on the same one soaks it - a stuck
     * shed comes off, anything else is a spray it dislikes - once a hold. The lizard under the finger at the
     * hold's start is the one; it is let go if it gets MIST_KEEP_R away (it may walk off) */
    if (t->mist_hold && t->tool == TOOL_MISTER) {
        if (t->mist_fish < 0 && !t->mist_done) {
            float bd = MIST_PICK_R;
            for (int i = 0; i < N_FISH_SLOTS; i++) {
                if (!tank_swims(t, i)) continue;
                float d = tank_dist(t->fish[i].x, t->fish[i].y, t->mist_x, t->mist_y);
                if (d < bd) { bd = d; t->mist_fish = (int8_t)i; }
            }
            t->mist_s = 0;
        }
        if (t->mist_fish >= 0) {
            const fish_t *f = &t->fish[t->mist_fish];
            if (!tank_swims(t, t->mist_fish) || tank_dist(f->x, f->y, t->mist_x, t->mist_y) > MIST_KEEP_R) { t->mist_fish = -1; t->mist_s = 0; }
            else if (!t->mist_done && (t->mist_s += dt) >= SOAK_HOLD_S) { t->mist_done = true; tank_mist_soak(t, t->mist_fish); }
        }
    } else { t->mist_fish = -1; t->mist_s = 0; t->mist_done = false; }
    t->mist_hold = false;                          /* (the platform says it again every frame the finger stays) */
    /* a tool nobody has used for a while goes back in the box */
    if (t->tool && !t->drag_active && (t->tool_idle += dt) > TOOL_IDLE_S) t->tool = TOOL_HAND;
    /* a lifted finger ends the wipe/slash stroke (platform re-asserts while down) */
    if (!t->drag_active) {
        t->drag_has_prev = false; t->drag_dist = 0;
        t->slash_armed = t->slash_engaged = t->slash_cut = false;
        t->slash_h = t->slash_v = 0;
        t->mist_stroke = 0;                        /* the next stroke may mist again */
    }
    t->drag_active = false;
}

void tank_handled(tank_t *t) { t->idle_s = 0; }

void tank_toggle_light(tank_t *t) {
    /* the first toggle takes over from the idle rule AND flips the light
       (the on/off cue comes from tank_tick, where the flip lands) */
    if (!t->light_override) { t->light_override = true; t->light_on = t->night; }
    else t->light_on = !t->light_on;
}

void tank_light_auto(tank_t *t) { t->light_override = false; }

/* the worn tank's way up (tank.h) */
bool tank_screen_turned(const tank_t *t) { return TANK_WORN && t->screen_turned; }
void tank_screen_set(tank_t *t, bool turned) { t->screen_turned = TANK_WORN && turned; }
int tank_orient(tank_t *t, int live_q) {
    if (!t->orient_lock) t->orient_q = (uint8_t)TANK_QUARTER(live_q);
    return t->orient_q;
}
void tank_quarter_from_upright(int q, float *x, float *y) {
    float ux = *x, uy = *y;
    switch (TANK_QUARTER(q)) {
    case 1: *x = uy; *y = TANK_W - 1 - ux; break;             /* turned clockwise: the glass's left edge is the picture's floor */
    case 2: *x = TANK_W - 1 - ux; *y = TANK_H - 1 - uy; break;
    case 3: *x = TANK_H - 1 - uy; *y = ux; break;
    default: break;
    }
}
void tank_quarter_to_upright(int q, float *x, float *y) {
    float tx = *x, ty = *y;
    switch (TANK_QUARTER(q)) {
    case 1: *x = TANK_W - 1 - ty; *y = tx; break;
    case 2: *x = TANK_W - 1 - tx; *y = TANK_H - 1 - ty; break;
    case 3: *x = ty; *y = TANK_H - 1 - tx; break;
    default: break;
    }
}
void tank_orient_lock(tank_t *t, bool lock) { t->orient_lock = lock; }

const int LIGHT_IDLE_CHOICES[LIGHT_IDLE_N] = { 5, 15, 30, 60, 180, 300, 600, 1800 };
int tank_light_choice(const tank_t *t) {
    if (!t->light_auto) return 0;
    int best = 0;
    for (int i = 1; i < LIGHT_IDLE_N; i++)
        if (abs(LIGHT_IDLE_CHOICES[i] - t->light_idle_s) < abs(LIGHT_IDLE_CHOICES[best] - t->light_idle_s)) best = i;
    return best + 1;
}
void tank_light_choice_set(tank_t *t, int choice) {
    if (choice < 0) choice = 0;
    if (choice > LIGHT_IDLE_N) choice = LIGHT_IDLE_N;
    t->light_auto = choice > 0;
    if (choice > 0) t->light_idle_s = LIGHT_IDLE_CHOICES[choice - 1];
    t->light_manual_off = false;
}

void tank_scatter_food(tank_t *t, int n) {
    if (PT_TERRARIUM) {                          /* AUTO CRICKETS (tank.h THE CRICKETS): the tank's own, at a random x (a pellet's draws) */
        for (int i = ck_free(t); i >= 0 && n > 0; i = ck_free(t), n--) {
            const float x = ck_drop_x(t, tank_randf(t, TANK_FX0 + 25, TANK_FX1 - 25));
            ck_drop_at(t, i, x, tank_glass_top(x) + tank_randf(t, 6, 20), false);
        }
        return;
    }
    for (int i = 0; i < MAX_FOOD && n > 0; i++) {
        if (t->food[i].alive) continue;
        t->food[i].alive = true; t->food[i].from_player = false;
        t->food[i].x = tank_randf(t, TANK_FX0 + 25, TANK_FX1 - 25);
        t->food[i].y = tank_glass_top(t->food[i].x) + tank_randf(t, 6, 20);
        t->food[i].age = 0; t->food[i].floor_s = 0; t->food[i].nibbled = 0;
        n--;
    }
}

/* ---- the packs' world (pocket-terrarium P2a: tank.h "the packs' world") ---- */
const species_t *tank_species(const tank_t *t) {
    const species_t *p = species_get(t->pack_id);
    return p ? p : species_get(SPECIES_DEFAULT);
}
const habitat_t *tank_habitat(const tank_t *t) {
    const habitat_t *h = habitat_get(t->habitat_id);
    return h ? h : habitat_get(HABITAT_DEFAULT);
}
/* a new pair (tank.h): both known, the species lists the habitat, and everything the species needs is provided by the habitat, this
   firmware and the model (packs/compat.h) - else false, the tank as it was, and why says why ("LEOPARD GECKO needs BASK; FRESHWATER
   has none"). The world no longer decides: a pair of another world's is refused for what it needs (the other world's model has not
   the goals, its firmware not the modules), as any pair is */
bool tank_set_packs(tank_t *t, uint8_t habitat_id, uint8_t pack_id, char *why, size_t why_n) {
    const habitat_t *h = habitat_get(habitat_id);
    const species_t *s = species_get(pack_id);
    if (!h || !s) {
        if (why && why_n) snprintf(why, why_n, "no %s with id %d", s ? "habitat" : "species", s ? habitat_id : pack_id);
        return false;
    }
    if (!packs_compatible(s, h, why, why_n)) return false;
    t->habitat_id = habitat_id; t->pack_id = pack_id; t->packs_missing = false;
    tank_terrain(t);                                    /* (its ground laid out now, on the tank's own task) */
    return true;
}
/* THE GROUND: each habitat's terrain on this board, laid out the first time it is asked for (tank_init and tank_set_packs
   ask at once, so the tick's task lays it before the advisor's or a renderer's reads it) and kept: a habitat does not
   change while the registry stands (habitat_generation: a pack added or dropped lays everything out again) */
#define TERRAIN_CACHE 2                  /* (a bin lives in its world's habitat; the packs test stages the other world's too) */
const terrain_t *tank_terrain(const tank_t *t) {
    static terrain_t laid[TERRAIN_CACHE];
    static const habitat_t *of[TERRAIN_CACHE];
    static uint32_t gen[TERRAIN_CACHE];
    const habitat_t *h = tank_habitat(t);
    const uint32_t now = habitat_generation();
    int k = 0;
    for (; k < TERRAIN_CACHE && of[k]; k++) { if (gen[k] != now) break; if (of[k] == h) return &laid[k]; }   /* (a slot laid before the registry changed is the one to lay again) */
    if (k == TERRAIN_CACHE) k = TERRAIN_CACHE - 1;        /* (more habitats than that in one bin: the last slot is re-laid) */
    const glass_t g = glass_here();
    of[k] = NULL; terrain_lay(h, &g, &laid[k]); of[k] = h; gen[k] = now;
    return &laid[k];
}
float tank_floor_x(float u) { const glass_t g = glass_here(); return glass_floor_x(&g, u); }
void tank_hide_point(const tank_t *t, int k, float *x, float *y) {
    const habitat_t *h = tank_habitat(t);
    *x = tank_floor_x(k >= 0 && k < h->n_hides ? h->hides[k].u : 0.5f); *y = PLACE_Y;
}
void tank_bask_point(const tank_t *t, float *x, float *y) { const terrain_t *tr = tank_terrain(t); *x = tr->bask_x; *y = tr->bask_y; }
int tank_hide_at(const tank_t *t, float x, float y) {
    const habitat_t *h = tank_habitat(t);
    for (int k = 0; k < h->n_hides; k++) {
        float hx, hy; tank_hide_point(t, k, &hx, &hy);
        if (tank_dist(x, y, hx, hy) < HIDE_R) return k;
    }
    return -1;
}
int tank_nearest_hide(const tank_t *t, float x, float y, float *dist) {
    const habitat_t *h = tank_habitat(t);
    int best = -1; float bd = 1e9f;
    for (int k = 0; k < h->n_hides; k++) {
        float hx, hy; tank_hide_point(t, k, &hx, &hy);
        float d = tank_dist(x, y, hx, hy);
        if (d < bd) { bd = d; best = k; }
    }
    if (dist) *dist = bd;
    return best;
}
int tank_humid_hide(const tank_t *t) {
    const habitat_t *h = tank_habitat(t);
    for (int k = 0; k < h->n_hides; k++) if (h->hides[k].kind == HIDE_HUMID) return k;
    return -1;
}
/* the lamp's pool as the tank has it: the habitat's, SD_BULB_R_X as wide with the shop's BASKING BULB (pocket-terrarium P5) */
static float lamp_x(const tank_t *t) { return (t->sd_unlocks & SD_ITEM_BULB) ? SD_BULB_R_X : 1.0f; }
float tank_env_c_lit(const tank_t *t, float x, float y, bool lit) {
    const habitat_t *h = tank_habitat(t);
    int k = tank_hide_at(t, x, y);
    if (k >= 0) return lit ? h->hides[k].t_lit_c : h->hides[k].t_dark_c;
    return terrain_air_lamp_c(tank_terrain(t), h, x, y, lit, lamp_x(t));
}
float tank_animal_env_c(const tank_t *t, const fish_t *f, bool lit) {
    const habitat_t *h = tank_habitat(t);
    if (f->hide >= 0 && f->hide < h->n_hides) return lit ? h->hides[f->hide].t_lit_c : h->hides[f->hide].t_dark_c;   /* in it: its air */
    return terrain_air_lamp_c(tank_terrain(t), h, f->x, f->y, lit, lamp_x(t));   /* out: the air where it stands (a footprint walked past too) */
}
float tank_env_c(const tank_t *t, float x, float y) { return tank_env_c_lit(t, x, y, !t->night); }
int tank_activity_phase(const tank_t *t) { return species_phase(tank_species(t), !t->night, t->light_s); }
/* the body's heat toward env over dt seconds (exact, so dt may be a whole night) at its pack's tau for its stage */
void body_follow(const tank_t *t, fish_t *f, float env, float dt) {
    const species_t *p = tank_species(t);
    f->body_c = env + (f->body_c - env) * expf(-dt / p->tau_s[f->stage & 3]);
    f->heat = species_heat_of(p, f->body_c);
}
/* a stuck shed's floor under the stress: STUCK_STRESS_PER_S a tended second stuck, up to STUCK_STRESS_MAX
 * (docs/shed.md "Stuck shed"; a floor, not a push: the reflex's stress settles ~0.18 a second) */
float stuck_floor(const fish_t *f) {
    return f->shed == SHED_STUCK ? fminf(STUCK_STRESS_MAX, STUCK_STRESS_PER_S * f->shed_s) : 0;
}

/* see tank.h: dark-screen physiology only, rates per real hour of sleep - the fish's SLEEP_*_PER_H (tank.h THE FISH'S
 * DRIVES), a lizard's its pack's (species_t.sleep_*_per_h, P2b) */
#define SLEEP_SLICE_S      300.0f  /* the night's growth and grazing interleave at this grain */
void tank_tick_sleep(tank_t *t, float seconds) {
    if (seconds <= 0 || t->packs_missing) return;       /* (a tank whose packs are missing lives no night: tank.h packs_missing) */
    float h = seconds / 3600.0f;
    t->clock += seconds;
    const species_t *sp = tank_species(t);                  /* its pack's sleep (the fish pack's are THE FISH'S DRIVES: the same numbers) */
    for (int i = 0; i < t->n_fish; i++) {
        fish_t *f = &t->fish[i];
        if (f->away) continue;                              /* (living its night next door) */
        f->hunger = clampf(f->hunger + sp->sleep_hunger_per_h * h, 0, 10);   /* (a shed's slower hunger is its appetite awake: update_lizard) */
        f->energy = clampf(f->energy + sp->sleep_energy_per_h * h, 0, 10);
        f->stress = clampf(f->stress - sp->sleep_stress_per_h * h, 0, 10);
        f->stress = fmaxf(f->stress, stuck_floor(f));
        body_follow(t, f, tank_animal_env_c(t, f, false), seconds);   /* the body follows the dark terrarium where it sleeps (its hide's, in one) */
        f->speed = 0; f->target_speed = 0; f->bored = 0; f->breather = 0;  /* a night's sleep is a fresh start */
        f->goal_age += seconds; f->ask_age += seconds;  /* wake re-asks the advisor at once */
    }
    t->light_s += seconds;                              /* (the phase: the light as it was, for that much longer) */
    t->idle_s = 0;                                      /* the wake press is handling: lights up */
    for (int i = 0; i < MAX_FOOD; i++)                  /* overnight pellets go stale */
        if (t->food[i].alive && (t->food[i].age += seconds) > 45) t->food[i].alive = false;
    for (int i = 0; i < MAX_CRICKETS; i++) {            /* ... and the crickets live out their time (a night: gone); the dish's worms stay */
        cricket_t *c = &t->cricket[i];
        if (c->state != CK_NONE && (c->age += seconds) > CK_LIFE_S) { c->state = CK_NONE; t->ck_lost++; }
    }
    coral_grow(t, seconds);
    cluster_grow(t, seconds);
    shrimp_sleep(t, seconds);
    if (t->sd_unlocks & SD_ITEM_SEAHORSE) { t->sh_s += seconds; t->sh_snack_cool = fmaxf(0, t->sh_snack_cool - seconds); }   /* days with the keeper run through the night */
    if (t->sd_unlocks & SD_ITEM_CRAB) t->cr_full = fmaxf(0, t->cr_full - seconds);   /* hungry again by the morning */
    if (t->sd_unlocks & SD_ITEM_CRAB) {                 /* the crab's next find: its wait runs asleep too; one held up when the tank slept is due again at the wake */
        if (t->cr_find > 0) { t->cr_find = 0; t->cr_find_cool = 0; }
        else t->cr_find_cool = fmaxf(0, t->cr_find_cool - seconds);
    }
    /* the garden grows fastest in a dark, untended tank: waking to a taller
     * canopy and film on the glass is the morning chore. The night is lived
     * a SLEEP_SLICE_S at a time - the grass and the film grow, then the
     * snail and the urchin eat into what grew (2026-10-02: one pass for the
     * whole night let the snail eat only the film from before it, and the
     * night's growth all landed after) - the same whether the span comes in
     * one call (a boot after a power-off) or many (the naps). */
    for (float left = seconds; left > 0; left -= SLEEP_SLICE_S) {
        float dt = left < SLEEP_SLICE_S ? left : SLEEP_SLICE_S;
        veg_grow(t, dt / VEG_GROW_SLEEP_S);
        /* film steps go through the same accumulator the awake tick uses:
         * (int)(30 / 120) is 0 - the truncation that had quietly stopped every
         * bit of algae from forming overnight (2026-09-04). Nothing is banked
         * past the slice: the awake tick once paid a long sleep's overflow out
         * a step per frame, film refilling under the keeper's wipe (2026-09-29). */
        t->algae_acc += dt;
        int steps = (int)(t->algae_acc / ALGAE_STEP_SLEEP_S);
        t->algae_acc -= steps * ALGAE_STEP_SLEEP_S;
        tank_grow_algae(t, steps);
        snail_sleep(t, dt);
        urchin_sleep(t, dt);
    }
}

/* the schema's 3 x 2 zone grid (advisor_core.c encodes the same), 0..5 */
int zone_of(float x, float y) {
    int col = (int)(x / (TANK_W / 3.0f)); if (col > 2) col = 2; if (col < 0) col = 0;
    int row = (int)(y / (TANK_BOT / 2.0f)); if (row > 1) row = 1; if (row < 0) row = 0;
    return row * 3 + col;
}

/* Coarse state signature (the browser prototype's "blunt" gate): banded
 * drives plus bucketed sightings. Excludes clock bearings and exact drive
 * digits, which only steer - a change here is a reason to re-decide. */
static int band3(float v) { return v < 3.5f ? 0 : v < 7 ? 1 : 2; }
static uint32_t state_signature(const tank_t *t, int idx) {
    const fish_t *f = &t->fish[idx];
    /* 2026-09-11 (the battery pass): the device's LLM core was saturated by
     * this gate, not by the idle ceiling - measured in the sim at 25 fps,
     * 2 fish: food distance flipped 6.3x per fish-minute (every bucket a
     * sinking pellet crossed), the (since removed) roaming shadow's distance
     * 5.8x, the wall flag 4x, against 0.4 for hunger. So the twitchy fields
     * are now events:
     *  food   - only for a fish that could want it (not full): none / in the
     *           tank / within reach. A pellet appearing, or coming close, is a
     *           reason to re-decide; its every metre of sinking is not.
     *  wall   - dropped: it is steering, and the state line still says
     *           near/clear whenever a decision is made for another reason.
     * The model sees the exact distances in its state line either way; this
     * only decides WHEN it is asked. */
    float fd = 1e9f; const bool seen = tank_food_sight(t, idx, NULL, NULL, &fd);   /* (the aquarium: the nearest pellet; a lizard: its prey) */
    int food = (!seen || band3(f->hunger) == 0) ? 0 : fd < 70 ? 2 : 1;
    uint32_t sig = (uint32_t)band3(f->hunger) | (uint32_t)band3(f->energy) << 2 | (uint32_t)band3(f->stress) << 4
                 | (uint32_t)food << 6
                 | (uint32_t)t->night << 10 | (uint32_t)band3(f->curiosity) << 11
                 | (uint32_t)band3(f->bored) << 13;      /* a fish going stale is asked again (2026-09-14) */
    if (PT_TERRARIUM) {   /* a lizard is asked again as its heat crosses a band (cold, cool, comfortable, warm, hot), a shed
                             comes or goes, the light's phase turns (P2b: the L1 line's heat, shed and phase) */
        const int hb = f->heat < 3 ? 0 : f->heat < 5 ? 1 : f->heat < 7 ? 2 : f->heat < HEAT_HOT ? 3 : 4;
        const int sh = f->shed == SHED_SOON ? 1 : f->shed == SHED_SHEDDING || f->shed == SHED_STUCK ? 2 : 0;
        sig |= (uint32_t)hb << 16 | (uint32_t)sh << 19 | (uint32_t)tank_activity_phase(t) << 21;
    }
    return sig;
}

void tank_tick(tank_t *t, float dt, advisor_fn advise) {
    if (t->packs_missing) { t->hold_active = false; return; }   /* paused: nothing moves, nothing is asked, no time passes (tank.h packs_missing) */
    t->clock += dt;
    /* the light: on while the device is handled, off LIGHT_IDLE_S after the
     * last touch or movement (tank_handled) - a tank left on the desk goes
     * dark and the fish sleep. A setup page or a prompt holds it on; the
     * director's / sim's manual override wins over both. */
    t->idle_s += dt;
    bool was_night = t->night;
    t->night = t->light_override ? !t->light_on
             : t->hold_light ? false
             : !t->light_auto ? t->light_manual_off              /* MANUAL (default): the double-tap's state */
             : t->idle_s > (float)t->light_idle_s;                      /* AUTO: the idle rule */
    if (t->night != was_night) { tank_emit(t->night ? TEV_LIGHT_OFF : TEV_LIGHT_ON, -1); t->light_s = 0; }
    else t->light_s += dt;                                   /* (the pack's phase: how long the light has been as it is) */

    touch_tick(t, dt);

    /* food sinks, settles, decays */
    int live_food = 0;
    for (int i = 0; i < MAX_FOOD; i++) {
        food_t *p = &t->food[i];
        if (!p->alive) continue;
        live_food++;
        p->age += dt;
        if (p->y < FOOD_FLOOR_Y) {
            p->y += FOOD_SINK_PX_S * dt;
            p->x += sinf(p->age * 1.5f) * dt * 2;
        } else p->floor_s += dt;                        /* resting on the floor (FOOD_FLOOR_S) */
        if (p->floor_s > 0 ? p->floor_s > FOOD_FLOOR_S : p->age > FOOD_LIFE_S) p->alive = false;
    }
    if (PT_TERRARIUM) live_food += ck_tick(t, dt);           /* THE CRICKETS: they fall, sit, hop, fly, hide (tank.h) */
    /* the tank's own trickle keeps fish alive when nobody is home (never
     * ruined by absence); the keeper's pellets are what progression counts.
     * Hunger-gated (see the hunger economy notes above): a pellet only when
     * somebody is really hungry, at a per-SECOND rate, so it's the same tank
     * at 25 fps and 60 fps and it never over-feeds a school that isn't asking.
     * While the tank is ravenous-begging it holds off, so the keeper's first
     * pellets are the event that ends the wait (progression re-arms it) -
     * but once the keeper HAS fed this episode it helps again: a quick fish
     * can gobble every pellet, and the slow one shouldn't beg out the whole
     * give-up valve for it. */
    float hungriest = 0;
    for (int i = 0; i < N_FISH_SLOTS; i++)
        if (tank_swims(t, i) && t->fish[i].hunger > hungriest) hungriest = t->fish[i].hunger;
    bool hold = (t->ravenous && !t->ravenous_fed) || t->trickle_off || t->autofeed_off;   /* (AUTO FEED off: the keeper's alone, 0.3.2) */
    if (PT_TERRARIUM && tank_activity_phase(t) != PHASE_ACTIVE) hold = true;   /* (P2b: nobody feeds lizards after lights-out - settle or sleep;
                                                                                 P2c: AUTO CRICKETS, in the active phase only. The aquarium's as ever) */
    if (!hold && live_food < 2 && hungriest >= TRICKLE_HUNGER &&
        tank_randf(t, 0, 1) < TRICKLE_RATE * dt)
        tank_scatter_food(t, 1);

    /* upkeep: the garden gets away from an idle keeper, slowly */
    veg_grow(t, dt / VEG_GROW_AWAKE_S);
    t->coral_acc += dt;                                     /* by the minute: see tank_t.coral_acc */
    if (t->coral_acc >= 60) { coral_grow(t, t->coral_acc); t->coral_acc = 0; }
    t->cluster_acc += dt;
    if (t->cluster_acc >= 60) { cluster_grow(t, t->cluster_acc); t->cluster_acc = 0; }
    t->algae_acc += dt;
    if (t->algae_acc >= ALGAE_STEP_AWAKE_S) {
        t->algae_acc = 0;                                   /* one step per 4 min, never a backlog */
        tank_grow_algae(t, 1);
    }
    snail_tick(t, dt);
    urchin_tick(t, dt);
    crab_tick(t, dt);
    seahorse_tick(t, dt);
    shrimp_tick(t, dt);
    {   /* SWIM DEPTH phase 3c: the floor critters take up their depth against the grass clear of every bed (as the lanes: never a pop) */
        if (t->snail_gdepth != t->snail_gdepth_want && box_clear_beds(t, t->snail_x - 22, t->snail_x + 22, TANK_H)) t->snail_gdepth = t->snail_gdepth_want;
        if (t->urchin_gdepth != t->urchin_gdepth_want && box_clear_beds(t, t->urchin_x - 20, t->urchin_x + 20, TANK_H)) t->urchin_gdepth = t->urchin_gdepth_want;
        if (t->cr_gdepth != t->cr_gdepth_want && box_clear_beds(t, t->cr_x - 26, t->cr_x + 26, TANK_H)) t->cr_gdepth = t->cr_gdepth_want;
    }

    /* bubbles rise */
    for (int i = 0; i < MAX_BUBBLE; i++) {
        bubble_t *b = &t->bubble[i];
        b->wobble += dt * 2.5f;
        b->y -= b->vy * dt;
        b->x += sinf(b->wobble) * dt * (b->column ? 9 : 4);
        if (b->y < -6) {
            b->y = TANK_BOT + tank_randf(t, 4, 24);
            b->x = b->column ? t->bubble_x + tank_randf(t, -10, 10)
                             : tank_randf(t, TANK_FX0 + 12, TANK_FX1 - 12);
        }
    }

    /* advisor: polled every frame (async decisions land the moment they're
     * ready). A (re)decision is REQUESTED need-based, not on a fixed cadence:
     * the coarse signature changed and ADVISOR_MIN_INTERVAL passed, or the
     * idle ceiling hit, or something urgent (starving with food in view).
     * Effective cadence therefore scales with how much is
     * happening, not with how many fish live here. The start index rotates
     * so no slot is structurally favoured when several fish ask at once. */
    if (advise && t->n_fish > 0) {
        for (int k = 0; k < N_FISH_SLOTS; k++) {
            int i = (t->ask_rr + k) % N_FISH_SLOTS;           /* every swimmer, visitors too (their host decides for them) */
            if (!tank_swims(t, i)) continue;
            fish_t *f = &t->fish[i];
            uint32_t sig = state_signature(t, i);
            /* prototype's urgent path: starving with food in view gets
             * asked NOW instead of waiting its turn */
            bool urgent = f->hunger > 8.0f && f->goal.id != GOAL_W_FOOD &&
                          f->goal_age > 0.8f && tank_food_sight(t, i, NULL, NULL, NULL);
            bool changed = sig != f->sig && f->ask_age > ADVISOR_MIN_INTERVAL;
            bool idle = f->ask_age > ADVISOR_IDLE_CEILING;
            bool want = changed || idle || (urgent && f->ask_age > 0.5f);
            goal_t g = advise(t, i, want);
            if (want) {            /* async advisors queue it; rules answer now */
                f->sig = sig; f->ask_age = 0; t->advisor_asks++;
                t->ask_rr = (i + 1) % N_FISH_SLOTS;
            }
            if (g.id < GOAL_COUNT && goal_in_world(g.id) && g.id != f->goal.id) {   /* (only its world's goals: GOAL_FIRST..) */
                /* a genuinely new pastime relieves boredom; bouncing back to
                 * the one just left (bubbles -> friend -> bubbles) does not */
                if (g.id != f->goal_prev) f->bored = clampf(f->bored - BORED_NEW_GOAL, 0, 10);
                f->goal_prev = f->goal.id;
                if (g.id == GOAL_W_EXPLORE || g.id == GOAL_LZ_INVESTIGATE) f->explore_set = false;   /* pick a fresh destination (a lizard's
                                                                                                         investigate alone: a fresh place) */
                if (f->door == DOOR_HEADING || f->door == DOOR_ASKED) f->door = DOOR_NONE;   /* the model changed its mind: no visit (one already passing goes on) */
                f->goal = g;
                f->goal_age = 0;
                if (!PT_TERRARIUM) {                           /* SWIM DEPTH: how it goes about it (a lizard walks the floor's lanes) */
                    f->gdepth_want = depth_pick(t, f, g.id);
                    f->pdepth_want = pdepth_pick(t, f, g.id);
                }
                /* visible deliberation, scaled by how torn the advisor was */
                bool calm = g.id != GOAL_FLEE_SHADOW && g.urgency < 8 && g.confidence < 0.6f;
                f->hesitate = calm ? (0.6f - g.confidence) * 2.5f : 0;
            } else if (g.id == f->goal.id) {
                f->goal.urgency = g.urgency;
                f->goal.confidence = g.confidence; f->goal.runner_up = g.runner_up;
            }
        }
    }

    /* Starvation instrument (was a TEMPORARY survival-reflex override; retired
     * 2026-08-21 after the retrained model chose seek_food on 92% of starving
     * states with defensible misses - per Strato: the LLM owns decisions).
     * Counts moments a starving fish ignores available food for >4s. Pure
     * diagnostic, never changes a goal. */
    for (int i = 0; i < N_FISH_SLOTS; i++) {
        if (!tank_swims(t, i)) continue;
        fish_t *f = &t->fish[i];
        if (f->hunger > 8.5f && f->goal.id != GOAL_W_FOOD &&
            f->goal.id != GOAL_FLEE_SHADOW && f->goal_age > 4.0f &&
            !f->starve_flagged && tank_food_sight(t, i, NULL, NULL, NULL)) {
            f->starve_flagged = true;          /* count once per episode */
            tank_reflex_overrides++;
        }
        if (f->goal.id == GOAL_W_FOOD || f->hunger < 8.0f) f->starve_flagged = false;
    }

    /* the bodies: the aquarium's fish swim, the terrarium's lizards walk (world.h; both kept, each world uses its own) */
    for (int i = 0; i < N_FISH_SLOTS; i++) if (tank_swims(t, i)) { if (PT_TERRARIUM) update_lizard(t, i, dt); else update_fish(t, i, dt); }
    for (int k = 0; k < N_VISIT_MAX; k++) if (t->vis_used[k]) t->vis_age[k] += dt;   /* a visitor grows here as it would at home */

    /* consume the hold AFTER the fish have seen it (the platform re-asserts
     * every frame the finger stays down). Clearing it at the top of the tick
     * was the old bug that kept the drift-to-finger reflex from ever firing. */
    t->hold_active = false;
}
