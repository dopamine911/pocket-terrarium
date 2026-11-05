#define _DEFAULT_SOURCE   /* setenv under -std=c11 (glibc) */
/* shot_terrarium.c - pocket-terrarium P3b: the terrarium as render_tank draws it (render.c THE TERRARIUM), staged
 * through the engine - real frames of a running tank, never the art harness (sheet_lizard.c is that):
 *
 *   ./fishsim --terr-shot <prefix>   PPMs of this board's terrarium: <prefix>_day (geckos on every level - basking on
 *                                    the high rock in the lamp's pool, on the shelf, climbing a ramp, on the floor),
 *                                    _ramp (one up a ramp, one down another), _peek (looking out of two hides),
 *                                    _shed (soon, shedding, stuck), _night (the lamp out: asleep up on the rocks and on
 *                                    the floor, one looking out), _card (a lizard's card over the tank), _mister (the
 *                                    mister in hand over the humid box: its spray), _chip (the finger lifted: the tool's
 *                                    chip), _keeper (the keeper page's rows: every stage's portrait) - each staged (goals
 *                                    held, positions
 *                                    set as the director would) and then RUN: the engine ticks and render_tank draws a
 *                                    second of frames with every cache as the board has them
 *   ./fishsim --terr-bench          the frame's cost on this board's glass, estimated on the board by the --bench
 *                                    convention (sheet_lizard.c lizard_board_k: this machine's microseconds x k): three
 *                                    lizards living ten seconds, the first bake, the other light's background bake, a
 *                                    light switch once it is baked and one right after a boot (the bake hurried), a
 *                                    humid hide's moisture step - the mean, the 99th percentile and the worst frame
 *
 * Terrarium-only (main.c dispatches them in the terrarium's bin). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "tank.h"
#include "advisor.h"
#include "render_scene_desert.h"
#include "progression.h"
#include "render.h"
#include "render_lizard.h"
#include "terrain.h"
#include "sim_tmp.h"

double lizard_board_k(void);            /* sheet_lizard.c */

static tank_t s_t;                      /* (tank_t is big: never on the stack) */
static uint16_t s_scene[TANK_W * TANK_H], s_spare[TANK_W * TANK_H], s_fb[TANK_W * TANK_H], s_card[RENDER_CARD_W * RENDER_CARD_H];
static uint8_t  s_vig[TANK_W * TANK_H];
static uint32_t s_dirty[RENDER_DIRTY_WORDS];
static const float DT = 1.0f / 30;

static int64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
static double s_k = 1;                  /* the board's microseconds per this machine's */
static int64_t board_us(void) { return (int64_t)((double)now_us() * s_k); }

static void ppm(const char *prefix, const char *what) {
    char path[512]; snprintf(path, sizeof path, "%s_%s.ppm", prefix, what);
    FILE *f = fopen(path, "wb"); if (!f) { printf("terr-shot: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", TANK_W, TANK_H);
    for (int i = 0; i < TANK_W * TANK_H; i++) {
        const uint16_t p = FB_SWAP16(s_fb[i]);
        const unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("terr-shot: %s\n", path);
}

/* the goal each lizard is held to (GOAL_COUNT: the rule stub's) */
static goal_id_t s_force[N_FISH_SLOTS];
static goal_t advisor_force(const tank_t *t, int i, bool request) {
    if (s_force[i] >= GOAL_COUNT) return advisor_rules(t, i, request);
    goal_t g = t->fish[i].goal; g.id = s_force[i]; g.urgency = 5; g.confidence = 1; g.runner_up = GOAL_COUNT;
    return g;
}
static void force(tank_t *t, int i, goal_id_t g) { s_force[i] = g; t->fish[i].goal.id = g; t->fish[i].goal.urgency = 5; t->fish[i].goal_age = 0; t->fish[i].explore_set = false; t->fish[i].hesitate = 0; }

/* a fresh terrarium on a scratch save, n lizards of the given stages and body colours (their morphs: lizard_morph_of) */
static void fresh(uint32_t seed, int n, const int *stage, const uint32_t *body) {
    const char *sav = sim_tmp("pocket-terrarium-terr-shot.sav");
    setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    tank_init(&s_t, seed);
    progression_wake(&s_t, 0);
    s_t.trickle_off = true;
    while (s_t.n_fish < n && tank_add_fish(&s_t, 0, 1) >= 0) {}
    while (s_t.n_fish > n) s_t.n_fish--;
    for (int i = 0; i < s_t.n_fish; i++) {
        fish_t *f = &s_t.fish[i];
        f->stage = (stage_t)stage[i]; tank_set_look(&s_t, i, body[i], 0);
        f->hunger = 3; f->energy = 7; f->stress = 1; f->shed = SHED_NORMAL; f->shed_s = 600;
        s_force[i] = GOAL_COUNT;
    }
    for (int i = n; i < N_FISH_SLOTS; i++) s_force[i] = GOAL_COUNT;
    s_t.light_override = true; s_t.light_on = true; s_t.night = false;
    render_set_scene_cache(s_scene); render_set_scene_spare(s_spare); render_set_vignette_cache(s_vig);
    render_set_dirty_mask(s_dirty); render_set_card_cache(s_card);
}
/* lizard i set down on path s at x, facing `face` (its line's y there; out of any hide) */
static void stand(int i, int s, float x, int face) {
    fish_t *f = &s_t.fish[i];
    const terrain_path_t *p = &tank_terrain(&s_t)->path[s];
    f->hide = -1; f->pose = LZ_POSE_STAND; f->turn_s = f->pause_s = f->walk_s = f->strike_s = 0; f->lane = 0;
    f->facing = (int8_t)face; f->yaw = f->yaw_tail = (float)face; f->heading = face > 0 ? 0 : 3.14159265f;
    f->surf = (int8_t)s; f->x = x; f->y = terrain_y(p, x); f->slope = terrain_tilt(p, x, face);
}
static int path_of(int kind, int nth) {                 /* the nth path of a kind (terrain_t), -1 none */
    const terrain_t *tr = tank_terrain(&s_t);
    for (int s = 0; s < tr->n_paths; s++) if (tr->path[s].kind == kind && nth-- == 0) return s;
    return -1;
}
static float path_mid(int s, float at) { const terrain_path_t *p = &tank_terrain(&s_t)->path[s]; return p->x[0] + (p->x[p->n - 1] - p->x[0]) * at; }
/* the engine and the renderer together for `secs`: every tick a frame */
static void run(float secs) {
    for (int k = 0; k < (int)(secs / DT + 0.5f); k++) {
        tank_tick(&s_t, DT, advisor_force);
        progression_tick(&s_t, DT);
        render_tank(&s_t, s_fb, TANK_W);
    }
}

static const uint32_t TEAL = 0x38dcc7, CORAL = 0xff725c, PINK = 0xf48fb1, BLUE = 0x4da3ff, CHARCOAL = 0x23262c, YELLOW = 0xffd166;

int terr_shot(const char *prefix) {
    if (!PT_TERRARIUM) { printf("terr-shot: the terrarium's bin only\n"); return 1; }
    const terrain_t *tr;
    /* DAY: every stage and four morphs, on every level; blink, tongue and fat are the engine's live draw fields */
    { const int st[4] = { STAGE_ADULT, STAGE_JUV, STAGE_ELDER, STAGE_FRY }; const uint32_t bd[4] = { TEAL, CORAL, CHARCOAL, PINK };
      fresh(7, 4, st, bd); tr = tank_terrain(&s_t);
      stand(0, tr->bask_path, tr->bask_x, -1); force(&s_t, 0, GOAL_LZ_BASK);
      const int shelf = 2; stand(1, shelf, path_mid(shelf, 0.55f), -1); force(&s_t, 1, GOAL_LZ_REST);
      const int r1 = path_of(PATH_RAMP, 0); stand(2, r1, path_mid(r1, 0.55f), -1); force(&s_t, 2, GOAL_LZ_BASK);
      stand(3, 0, TANK_W * 0.62f, 1); force(&s_t, 3, GOAL_LZ_EXPLORE);
      run(2.5f);
      s_t.fish[0].lz_blink = 1; s_t.fish[1].pose = LZ_POSE_FLICK; s_t.fish[1].lz_tongue = 1;
      s_t.fish[2].lz_fat = 1; s_t.fish[3].lz_fat = 0.08f;
      render_tank(&s_t, s_fb, TANK_W); ppm(prefix, "day"); }
    /* RAMP: a pitched walk up ramp 2 and a dig down ramp 3 */
    { const int st[2] = { STAGE_ADULT, STAGE_ELDER }; const uint32_t bd[2] = { YELLOW, BLUE };
      fresh(8, 2, st, bd); tr = tank_terrain(&s_t);
      const int r2 = path_of(PATH_RAMP, 1), r3 = path_of(PATH_RAMP, 2);
      const terrain_path_t *p2 = &tr->path[r2], *p3 = &tr->path[r3];
      const int up2 = p2->y[p2->n - 1] < p2->y[0] ? 1 : -1, down3 = p3->y[p3->n - 1] < p3->y[0] ? -1 : 1;
      stand(0, r2, path_mid(r2, up2 > 0 ? 0.3f : 0.7f), up2); force(&s_t, 0, GOAL_LZ_BASK);
      stand(1, r3, path_mid(r3, 0.5f), down3); force(&s_t, 1, GOAL_LZ_DIG);
      run(1.2f);
      s_t.fish[0].pose = LZ_POSE_WALK; s_t.fish[0].lz_phase = 0.25f;
      s_t.fish[1].pose = LZ_POSE_DIG; s_t.fish[1].lz_phase = 0.55f;
      render_tank(&s_t, s_fb, TANK_W); ppm(prefix, "ramp"); }
    /* PEEK: looking out of the warm cave and the humid box; one standing by on the floor */
    { const int st[3] = { STAGE_ADULT, STAGE_JUV, STAGE_ADULT }; const uint32_t bd[3] = { TEAL, BLUE, CORAL };
      fresh(9, 3, st, bd);
      const habitat_t *h = tank_habitat(&s_t);
      int warm = -1, humid = -1;
      for (int k = 0; k < h->n_hides; k++) { if (h->hides[k].kind == HIDE_WARM) warm = k; if (h->hides[k].kind == HIDE_HUMID) humid = k; }
      if (humid >= 0) s_t.moist[humid] = 1;                              /* the humid box visibly wet behind its peek */
      for (int i = 0; i < 2; i++) {
          const int k = i == 0 ? warm : humid;
          tank_lizard_enter(&s_t, i, k); force(&s_t, i, GOAL_LZ_HIDE);
          s_t.fish[i].shed = i == 1 ? SHED_SOON : SHED_NORMAL;           /* (the juvenile's shed coming: the humid box is its hide) */
          s_t.fish[i].pose = LZ_POSE_PEEK; s_t.fish[i].pose_s = 0.6f; s_t.fish[i].peek_s = 6;
      }
      stand(2, 0, TANK_W * 0.70f, -1); force(&s_t, 2, GOAL_LZ_REST);
      run(1.0f); ppm(prefix, "peek"); }
    /* SHED: soon (dull, blue-grey eyes), shedding (the old skin loose half way back), stuck (rings on the toes and tail) */
    { const int st[3] = { STAGE_ADULT, STAGE_ADULT, STAGE_ADULT }; const uint32_t bd[3] = { TEAL, CORAL, YELLOW };
      fresh(10, 3, st, bd);
      const species_t *sp = tank_species(&s_t);
      const uint8_t sh[3] = { SHED_SOON, SHED_SHEDDING, SHED_STUCK };
      for (int i = 0; i < 3; i++) {
          stand(i, 0, TANK_W * (0.30f + 0.22f * i), i == 1 ? -1 : 1); force(&s_t, i, GOAL_LZ_REST);
          s_t.fish[i].shed = sh[i]; s_t.fish[i].shed_s = sh[i] == SHED_SHEDDING ? sp->shed_window_s * 0.45f : 300;
      }
      run(1.0f);
      for (int i = 0; i < 3; i++) { s_t.fish[i].pose = LZ_POSE_STAND; s_t.fish[i].pose_s = 0; }
      render_tank(&s_t, s_fb, TANK_W); ppm(prefix, "shed"); }
    /* NIGHT: the lamp out - one asleep on the high rock, one on the shelf, one on the floor, one looking out of the cool cave */
    { const int st[4] = { STAGE_ADULT, STAGE_ADULT, STAGE_JUV, STAGE_ADULT }; const uint32_t bd[4] = { TEAL, CHARCOAL, PINK, CORAL };
      fresh(11, 4, st, bd); tr = tank_terrain(&s_t);
      const habitat_t *h = tank_habitat(&s_t);
      int cool = -1; for (int k = 0; k < h->n_hides; k++) if (h->hides[k].kind == HIDE_COOL) cool = k;
      stand(0, tr->bask_path, tr->bask_x + 30, 1); stand(1, 2, path_mid(2, 0.4f), -1); stand(2, 0, TANK_W * 0.45f, 1);
      for (int i = 0; i < 3; i++) force(&s_t, i, GOAL_LZ_REST);
      run(0.5f);
      s_t.light_on = false; run(2.5f);                                          /* (the dark scene: baked in the background, or hurried) */
      tank_lizard_enter(&s_t, 3, cool); force(&s_t, 3, GOAL_LZ_HIDE);
      s_t.fish[3].pose = LZ_POSE_PEEK; s_t.fish[3].pose_s = 0.6f; s_t.fish[3].peek_s = 6;
      run(0.6f); ppm(prefix, "night"); }
    /* CARD: the lit tank with a lizard's card over it (the platform draws it after render_tank) */
    { const int st[2] = { STAGE_ADULT, STAGE_JUV }; const uint32_t bd[2] = { TEAL, CORAL };
      fresh(12, 2, st, bd); tr = tank_terrain(&s_t);
      stand(0, 0, TANK_W * 0.55f, -1); force(&s_t, 0, GOAL_LZ_REST); stand(1, tr->bask_path, tr->bask_x, 1); force(&s_t, 1, GOAL_LZ_BASK);
      run(1.0f); render_stats_card(&s_t, 0, s_fb, TANK_W); ppm(prefix, "card"); }
    /* MISTER: the mister in hand, a stroke over the humid box as the touch port gives it (tank_touch_drag every frame the
       finger is down) - its spray at the finger, the box damp (_mister); the finger lifted, the tool's chip back (the
       platform's, after render_tank: it steps aside while a stroke is on the glass) (_chip) */
    { const int st[2] = { STAGE_ADULT, STAGE_JUV }; const uint32_t bd[2] = { YELLOW, PINK };
      fresh(14, 2, st, bd);
      const habitat_t *h = tank_habitat(&s_t); scene_desert_t d; { const glass_t g = glass_here(); scene_desert_layout(&g, h, &d); }
      int humid = 0; for (int k = 0; k < d.n_hides; k++) if (d.hide[k].kind == HIDE_HUMID) humid = k;
      s_t.moist[humid] = 0.1f;
      stand(0, 0, TANK_W * 0.75f, -1); force(&s_t, 0, GOAL_LZ_EXPLORE); stand(1, 0, TANK_W * 0.30f, 1); force(&s_t, 1, GOAL_LZ_REST);
      run(0.5f);
      tank_set_tool(&s_t, TOOL_MISTER);
      const float mx = d.hide[humid].cx, my = d.hide[humid].base_y - d.hide[humid].height * 0.75f;
      for (int k = 0; k < 28; k++) {                                       /* two strokes across the box (a lift between), the finger still down */
          if (k < 12 || k >= 14) tank_touch_drag(&s_t, mx - 16 + 32 * fabsf(1 - (float)(k % 14) / 6.5f), my - 4 * (k >= 14));
          tank_tick(&s_t, DT, advisor_force); progression_tick(&s_t, DT); render_tank(&s_t, s_fb, TANK_W);
      }
      render_tool_chip(&s_t, -1, s_fb, TANK_W); ppm(prefix, "mister");
      run(0.2f); render_tool_chip(&s_t, -1, s_fb, TANK_W); ppm(prefix, "chip"); }
    /* KEEPER: the keeper page's rows - each lizard's portrait (render.c lizard_still), its stage and badges */
    { const int st[4] = { STAGE_ELDER, STAGE_ADULT, STAGE_JUV, STAGE_FRY }; const uint32_t bd[4] = { CHARCOAL, TEAL, CORAL, PINK };
      fresh(15, 4, st, bd);
      run(0.3f); render_milestones(&s_t, s_fb, TANK_W); ppm(prefix, "keeper"); render_milestones_leave(); }
    return 0;
}

/* ---- --selftest-terr-render: THE TERRARIUM's contract (render.c), through render_tank with the board's caches ----
 *  - the scene shown is the desert as scene_desert_draw paints it, pixel for pixel: the first bake; the other light baked
 *    in the background (no frame bakes more than TS_BAKE_ROWS rows), then the light's switch a swap (no rows that frame),
 *    off and on again; a humid hide's moisture step (its box repainted: the whole scene as a fresh bake would have it);
 *    a switch right after a boot (the bake hurried, TS_HURRY_ROWS a frame at most, the old light on the glass meanwhile);
 *  - the stack: a lizard looking out of a hide, and one walking into its hide, leave every pixel of the hide's front behind
 *    them (from the door's open edge back) as the scene has it - and show (pixels drawn in the door, or out of it); a
 *    HIDDEN one draws nothing; a lizard up on a rock is drawn over the rock (the scene), one on the floor over one up there;
 *  - every pixel the frame drew is in its touched mask (the board's restore puts the scene back under exactly those) */
static uint16_t s_ref[TANK_W * TANK_H];
#define TF(...) do { printf("FAIL: terr-render: "); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)
static int scene_is(bool lit, const char *when) {          /* the scene shown against a fresh bake of the tank's own */
    const habitat_t *h = tank_habitat(&s_t);
    float m[HABITAT_HIDES_MAX];
    for (int i = 0; i < HABITAT_HIDES_MAX; i++) m[i] = i < h->n_hides && h->hides[i].kind == HIDE_HUMID ? (float)scene_desert_moist_step(s_t.moist[i]) / 8.0f : 0;
    const glass_t g = glass_here();
    scene_desert_draw(s_ref, TANK_W, &g, h, lit, m, lit ? 1.0f : 0.45f, s_vig);
    const uint16_t *sc = render_scene_buf(NULL);
    if (!sc) TF("%s: no scene", when);
    int bad = 0, first = -1;
    for (int i = 0; i < TANK_W * TANK_H; i++) if (sc[i] != s_ref[i]) { if (first < 0) first = i; bad++; }
    if (bad) TF("%s: the scene shown differs from a fresh %s bake at %d px (the first at %d,%d)", when, lit ? "lit" : "dark", bad, first % TANK_W, first / TANK_W);
    return 0;
}
static int rows_now(void) { int r; render_terrarium_bake_stats(&r, NULL, NULL); return r; }
/* the hide's front before a lizard at its door (render.c ts_layout's cover: scene_desert_hide_covers) */
static bool cover_at(const scene_desert_t *d, int k, int x, int y) { return scene_desert_hide_covers(d, k, x + 0.5f, y + 0.5f); }
static int stack_hide(const scene_desert_t *d, int k, const char *what, int *drawn) {   /* the frame against the scene inside hide k's box */
    int x0, y0, x1, y1; scene_desert_hide_box(d, k, &x0, &y0, &x1, &y1);
    const uint16_t *sc = render_scene_buf(NULL);
    *drawn = 0;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < TANK_H; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < TANK_W; x++) {
            const int i = y * TANK_W + x;
            if (s_fb[i] == sc[i]) continue;
            if (cover_at(d, k, x, y)) TF("%s: a lizard over the hide's front at %d,%d", what, x, y);
            (*drawn)++;
        }
    return 0;
}
int selftest_terr_render(void) {
    if (!PT_TERRARIUM) { printf("selftest-terr-render: the terrarium's bin only\n"); return 0; }
    static uint32_t touched[RENDER_DIRTY_WORDS];
    const int st[3] = { STAGE_ADULT, STAGE_JUV, STAGE_ELDER }; const uint32_t bd[3] = { TEAL, CORAL, CHARCOAL };
    fresh(31, 3, st, bd);
    const habitat_t *h = tank_habitat(&s_t);
    int humid = -1, warm = -1;
    for (int k = 0; k < h->n_hides; k++) { if (h->hides[k].kind == HIDE_HUMID) humid = k; if (h->hides[k].kind == HIDE_WARM) warm = k; }
    /* 1 the first bake; the background; the switch */
    tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
    if (scene_is(true, "the first bake")) return 1;
    int frames = 0, most = 0;
    for (int r0 = rows_now(); frames < 400; frames++) {
        const int a = rows_now(); tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
        const int d = rows_now() - a; if (d > most) most = d;
        if (rows_now() - r0 >= TANK_H && d == 0) break;
    }
    if (most > 12) TF("the background bake took %d rows in one frame (12 at most)", most);
    const int bg_frames = frames + 1, bg_most = most;
    s_t.light_on = false;
    { const int a = rows_now(); tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
      if (rows_now() != a) TF("the light's switch baked %d rows (a swap bakes none)", rows_now() - a); }
    if (scene_is(false, "the light off")) return 1;
    s_t.light_on = true; tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
    if (scene_is(true, "the light on again")) return 1;
    if (humid >= 0) {
        s_t.moist[humid] = (float)(scene_desert_moist_step(s_t.moist[humid]) >= 7 ? 2 : scene_desert_moist_step(s_t.moist[humid]) + 2) / 8.0f;
        tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
        if (scene_is(true, "a moisture step")) return 1;
        s_t.light_on = false; tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);   /* (the dark one's box at its next showing) */
        if (scene_is(false, "the light off after a moisture step")) return 1;
        s_t.light_on = true; tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
    }
    render_set_scene_cache(s_scene); render_set_scene_spare(s_spare);   /* a boot: the first scene, then the switch at once */
    tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
    s_t.light_on = false; most = 0; frames = 0;
    unsigned ep0; render_scene_buf(&ep0);
    for (;; frames++) {
        const int a = rows_now(); tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
        const int d = rows_now() - a; if (d > most) most = d;
        unsigned ep; render_scene_buf(&ep);
        if (ep != ep0) break;
        if (frames > 100) TF("the light off right after a boot: still lit after 100 frames");
        if (scene_is(true, "the light off, its bake under way (the lit scene still shown)")) return 1;
    }
    if (most > 48) TF("the hurried bake took %d rows in one frame (48 at most)", most);
    if (scene_is(false, "the light off right after a boot")) return 1;
    s_t.light_on = true;
    for (int f = 0; f < 3; f++) { tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W); }
    printf("selftest-terr-render: the scenes as a fresh bake would paint them - the first; the dark one baked in the background in %d frames\n"
           "  (%d rows a frame at most), then the switch a swap (no rows); a moisture step (its box); a switch right after a boot\n"
           "  (%d frames hurried, %d rows a frame at most, the lit scene shown meanwhile)\n", bg_frames, bg_most, frames + 1, most);
    /* 2 the stack */
    scene_desert_t d; { const glass_t g = glass_here(); scene_desert_layout(&g, h, &d); }
    const int k = warm >= 0 ? warm : 0;
    tank_lizard_enter(&s_t, 0, k); force(&s_t, 0, GOAL_LZ_HIDE);                /* a look out of the warm cave */
    s_t.fish[0].pose = LZ_POSE_PEEK; s_t.fish[0].pose_s = 0.7f; s_t.fish[0].peek_s = 6;
    for (int i = 1; i < 3; i++) { tank_lizard_enter(&s_t, i, k); force(&s_t, i, GOAL_LZ_HIDE); s_t.fish[i].pose = LZ_POSE_HIDDEN; s_t.fish[i].peek_s = 60; }
    render_set_touched_mask(touched);
    int drawn_peek = 0;
    for (int f = 0; f < 6; f++) {
        render_tank(&s_t, s_fb, TANK_W);
        if (stack_hide(&d, k, "a look out of the warm cave", &drawn_peek)) return 1;
    }
    if (drawn_peek < 20) TF("a look out of the warm cave: %d px of it shown (none seen through the door)", drawn_peek);
    {   const uint16_t *sc = render_scene_buf(NULL);                      /* every drawn pixel touched */
        for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) {
            const int i = y * TANK_W + x;
            if (s_fb[i] != sc[i] && !(touched[y * ((TANK_W + 31) / 32) + (x >> 5)] >> (x & 31) & 1)) TF("a pixel drawn but not in the touched mask at %d,%d", x, y);
        }
        memcpy(s_ref, s_fb, sizeof s_ref);                                 /* the hidden two draw nothing: the frame without them is the same */
        const int n = s_t.n_fish; s_t.n_fish = 1; render_tank(&s_t, s_fb, TANK_W); s_t.n_fish = n;
        int diff = 0; for (int i = 0; i < TANK_W * TANK_H; i++) diff += s_fb[i] != s_ref[i];
        if (diff) TF("the hidden lizards drew %d px", diff);
    }
    /* walking into its hide from the open side, every frame until it is in */
    tank_lizard_leave(&s_t, 0); force(&s_t, 0, GOAL_LZ_EXPLORE);
    { const scene_hide_t *hd = &d.hide[k]; stand(0, 0, hd->door_x + hd->opens * 90.0f, -hd->opens); }
    force(&s_t, 0, GOAL_LZ_HIDE);
    int walked = 0, drawn_in = 0, seen_behind = 0;
    for (; walked < 400 && s_t.fish[0].hide < 0; walked++) {
        tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W);
        if (s_t.fish[0].hide >= 0) break;
        int dr; if (stack_hide(&d, k, "walking into the warm cave", &dr)) return 1;
        drawn_in += dr; if (dr) seen_behind++;
    }
    if (s_t.fish[0].hide != k) TF("walking into the warm cave: not in it after %d frames", walked);
    /* up on a rock: over the rock; one on the floor over one up there */
    const terrain_t *tr = tank_terrain(&s_t);
    for (int i = 0; i < 3; i++) { tank_lizard_leave(&s_t, i); force(&s_t, i, GOAL_LZ_REST); }
    stand(0, tr->bask_path, tr->bask_x, 1); stand(1, 1, path_mid(1, 0.5f), 1); stand(2, 0, path_mid(1, 0.5f), 1);
    for (int f = 0; f < 30; f++) { tank_tick(&s_t, DT, advisor_force); render_tank(&s_t, s_fb, TANK_W); }
    {   const uint16_t *sc = render_scene_buf(NULL); int on_rock = 0;
        for (int y = (int)(s_t.fish[0].y - 14); y <= (int)(s_t.fish[0].y + 6); y++)
            for (int x = (int)s_t.fish[0].x - 20; x <= (int)s_t.fish[0].x + 20; x++) on_rock += s_fb[y * TANK_W + x] != sc[y * TANK_W + x];
        if (on_rock < 60) TF("a lizard basking on the high rock: %d px of it over the rock", on_rock); }
    printf("selftest-terr-render: a look out of the warm cave (%d px seen, none over its front), into it from the open side in %d frames\n"
           "  (%d of them partly behind its front), the hidden two unseen; every drawn pixel touched; up on the rocks over them\n",
           drawn_peek, walked, seen_behind);
    render_set_touched_mask(NULL);
    printf("selftest-terr-render OK\n");
    return 0;
}

/* ---- the cost ---- */
static int cmp64(const void *a, const void *b) { const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
#define BENCH_N 300
static int64_t s_ft[BENCH_N];
static int64_t frame_us(void) {                         /* one tick and one render_tank: the render's board microseconds */
    tank_tick(&s_t, DT, advisor_force); progression_tick(&s_t, DT);
    const int64_t t0 = board_us(); render_tank(&s_t, s_fb, TANK_W); return board_us() - t0;
}
int terr_bench(void) {
    if (!PT_TERRARIUM) { printf("terr-bench: the terrarium's bin only\n"); return 1; }
    s_k = lizard_board_k();
    printf("terr-bench: the terrarium's frame on this glass (%dx%d), in the board's ms (this machine x %.0f, the old urchin's 1.8 ms)\n", TANK_W, TANK_H, s_k);
    render_clock_us = board_us;                         /* the bake's budgets and the stages in the board's microseconds */
    const int st[3] = { STAGE_ADULT, STAGE_JUV, STAGE_ELDER }; const uint32_t bd[3] = { TEAL, CORAL, CHARCOAL };
    fresh(21, 3, st, bd);
    /* the first bake (a boot): whole, in one frame under the platform's loading frame */
    int64_t first = frame_us();
    int rows, slices; int64_t smax;
    render_terrarium_bake_stats(&rows, &slices, &smax);
    printf("  the first scene (hidden by the boot/loading frame), baked whole: %6.1f ms\n", first / 1000.0);
    render_terrarium_bake_stats_reset(); rows = slices = 0; smax = 0;     /* the live-slice evidence excludes that intentional whole bake */
    memset(render_prof_us, 0, sizeof render_prof_us);
    const int b0 = lizard_cache_builds(); int frames_bg = -1; int64_t sum = 0;
    for (int f = 0; f < BENCH_N; f++) {
        s_ft[f] = frame_us(); sum += s_ft[f];
        int r; render_terrarium_bake_stats(&r, NULL, NULL);
        if (frames_bg < 0 && r - rows >= TANK_H) frames_bg = f + 1;
    }
    render_terrarium_bake_stats(NULL, &slices, &smax);
    qsort(s_ft, BENCH_N, sizeof s_ft[0], cmp64);
    printf("  three lizards, %d frames (%.0f s): mean %5.1f ms, 99th pct %5.1f, worst %5.1f; %d body builds (at most 1 a frame)\n",
           BENCH_N, BENCH_N * DT, sum / 1000.0 / BENCH_N, s_ft[BENCH_N * 99 / 100] / 1000.0, s_ft[BENCH_N - 1] / 1000.0, lizard_cache_builds() - b0);
    printf("    by stage (mean ms): scene %.2f (its copy, the bake's slices) | crickets %.2f | lizards %.2f | vignette %.2f | glass %.2f\n",
           render_prof_us[0] / 1000.0 / BENCH_N, render_prof_us[3] / 1000.0 / BENCH_N, render_prof_us[7] / 1000.0 / BENCH_N,
           render_prof_us[5] / 1000.0 / BENCH_N, render_prof_us[6] / 1000.0 / BENCH_N);
    printf("    the other light baked in the background: %d frames (%.1f s), slices up to %.1f ms\n", frames_bg, frames_bg * DT, smax / 1000.0);
    /* a light switch with the other light baked: a swap */
    int64_t worst = 0;
    s_t.light_on = false;
    for (int f = 0; f < 10; f++) { const int64_t u = frame_us(); if (u > worst) worst = u; }
    printf("  the light off (the dark scene baked): the worst of 10 frames %5.1f ms\n", worst / 1000.0);
    worst = 0; s_t.light_on = true;
    for (int f = 0; f < 10; f++) { const int64_t u = frame_us(); if (u > worst) worst = u; }
    printf("  ... and on again:                     the worst of 10 frames %5.1f ms\n", worst / 1000.0);
    /* a humid hide's moisture step */
    const habitat_t *h = tank_habitat(&s_t); int humid = -1;
    for (int k = 0; k < h->n_hides; k++) if (h->hides[k].kind == HIDE_HUMID) humid = k;
    if (humid >= 0) {
        s_t.moist[humid] = fminf(1, (float)(scene_desert_moist_step(s_t.moist[humid]) + 1) / 8.0f + 0.01f);
        worst = 0; for (int f = 0; f < 5; f++) { const int64_t u = frame_us(); if (u > worst) worst = u; }
        printf("  a moisture step (the humid box's repaint): the worst of 5 frames %5.1f ms\n", worst / 1000.0);
    }
    /* a switch right after a boot: the other light not yet baked - the bake hurried, the old light shown meanwhile */
    render_set_scene_cache(s_scene); frame_us();
    s_t.light_on = false; worst = 0; int lag = 0; unsigned ep0; render_scene_buf(&ep0);
    for (int f = 0; f < 60; f++) {
        const int64_t u = frame_us(); if (u > worst) worst = u;
        unsigned ep; render_scene_buf(&ep);
        if (ep == ep0) lag = f + 1;
    }
    printf("  the light off right after a boot (the dark bake hurried): the worst frame %5.1f ms, the dark on the glass after %d frames (%.2f s)\n",
           worst / 1000.0, lag + 1, (lag + 1) * DT);
    render_clock_us = NULL;
    return 0;
}
