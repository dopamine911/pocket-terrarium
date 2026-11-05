#define _DEFAULT_SOURCE   /* setenv under -std=c11 (glibc) */
/* selftest_walk.c - pocket-terrarium P2b, P2c: THE WALK (tank.h) on the ground (terrain.h: the floor, the rocks, the
 * ramps), the terrarium's locomotion, through the sim (tank_tick, progression_tick, the advisors) - never by reading a
 * constant back:
 *
 *   ./fishsim --selftest-walk        the lizards stay on the ground: a lit day, a feeding, a startle, a night and a
 *                                    morning on the rule stub, every lizard every frame on its path's walking line (on
 *                                    the floor in the floor's band) within the path's ends, its body inside the glass at
 *                                    its height (each board: make ROUND=1 / WATCH=1 / SQUARE=1 check); it changes path
 *                                    only where two meet (a ramp's end: never through rock, never a drop or a float); it
 *                                    goes into a hide only from the floor, at its mouth; it climbs the ramps up and down;
 *                                    its body takes the ground's slope; a turn about is on the spot and swings it head-on;
 *                                    a walk stops and goes; each stage walks at its pack's pace on the level, cold slower,
 *                                    up a ramp slower and down it a little careful, by the slope
 *   ./fishsim --selftest-goals       each of the seven goals reaches its place on the ground: bask up the ramps to the
 *                                    high rock's top and stays; hide down from the high rock into the hide its need picks
 *                                    (warm, cool when hot, humid with a shed soon, on or stuck - and a shed that comes
 *                                    moves it there, and comes off clean in it); hunt a cricket (held still) - crept up
 *                                    on, frozen at, lunged at - on the sand, from the high rock, up on a ledge, and one
 *                                    from the keeper's hand, which lands on the first top under it; explore a stale zone,
 *                                    the rocks' top row too; investigate the other (up to it, up on a ledge, or a
 *                                    stand-off when territorial) or a place; every place from every path; dig (down on
 *                                    the sand) and rest in place, on a rock too; the night
 *   ./fishsim --selftest-crickets    the terrarium's live prey (tank.h THE CRICKETS): the feed gesture's crickets from the
 *                                    hand (over the dish: the dish filled); a drop every 3 px of the glass lands on the
 *                                    first top under it, never in rock or through the glass (each board); frightened off
 *                                    every rock and ramp, onto the first top under it; the hop, the flight from a lizard,
 *                                    cover and out of sight, gone uneaten, a night; AUTO CRICKETS lit only; a strike's
 *                                    misses by the cold, the stage, a cricket mid-hop or frightened; a starving lizard on
 *                                    the rule stub catches one on the move in a bound; a catch is one meal; the dish
 *   ./fishsim --selftest-gestures    THE HAND (on the ground nearest the finger: the trusting come, the wary stay, trust for
 *                                    both; over the high rock, up it), THE RAP (a fish's stress and trust, the dash for
 *                                    cover, in while the fright lasts, out), a tap on a lizard along its body, the tools
 *   ./fishsim --selftest-terr-pay    the terrarium's sand dollars: keeper meals (a cricket from the hand, a worm from the
 *                                    dish the keeper filled; once a lizard per SD_MEAL_GAP_S; the tank's own, nothing) and
 *                                    clean sheds (never stuck: SD_SHED_CLEAN; a stuck one cleared: nothing; growth: + its stage)
 *   ./fishsim --selftest-terrarium   a free-running terrarium, the rule stub its brain and nobody at the glass but the
 *                                    tank's own AUTO CRICKETS: a lit day, a night, a morning. The heat in band, the growth
 *                                    sheds off clean (the humid hide's own start, never misted), the night in hides or
 *                                    lying still; the time above the floor, the zone rows and the crickets caught a
 *                                    lizard-hour (A15) printed
 *
 * Terrarium-only (sim/Makefile SELFTESTS_TERRARIUM). The tank is a fresh one on a scratch save. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tank.h"
#include "advisor.h"
#include "progression.h"
#include "advisor_core.h"
#include "pt_util.h"
#include "sim_tmp.h"

static const char *s_name = "";
#define WF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)
static tank_t s_t;                           /* (tank_t is big: never on the stack) */
static const float DT = 1.0f / 30;
static int s_ck = -1, s_ck_surf; static float s_ck_x;   /* the cricket --selftest-goals holds still (ck_pin, below) */

/* a fresh terrarium on a scratch save (progression booted: the founding pair, hatchlings), n lizards, no food */
static void fresh(tank_t *t, uint32_t seed, int n, bool trickle) {
    const char *sav = sim_tmp("pocket-terrarium-selftest-walk.sav");
    setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    tank_init(t, seed);
    progression_wake(t, 0);
    t->trickle_off = !trickle;
    tank_clear_food(t);                          /* (tank_init's two crickets) */
    s_ck = -1;
    while (t->n_fish < n && tank_add_fish(t, 0, 1) >= 0) {}
    t->light_override = true; t->light_on = true; t->night = false;
}
static void lights(tank_t *t, bool on) { t->light_override = true; t->light_on = on; }
static const char *const PATH_KIND[3] = { "the floor", "a rock", "a ramp" };

/* the ground, as the keeper sees it: on its path (fish_t.surf), within the path's ends, its middle on the path's walking
 * line (the floor's: in its band), its body as drawn (tank_lizard_half_len either side) inside the glass at its height, no NaN */
static int on_ground(const tank_t *t, int i, const char *when, double at) {
    const fish_t *f = &t->fish[i];
    const terrain_t *tr = tank_terrain(t);
    if (f->x != f->x || f->y != f->y) WF("%s %s at %.1f s: NaN (%f, %f)", f->name, when, at, f->x, f->y);
    if (f->surf < 0 || f->surf >= tr->n_paths) WF("%s %s at %.1f s: on no path (%d of %d)", f->name, when, at, f->surf, tr->n_paths);
    const terrain_path_t *p = &tr->path[f->surf];
    if (f->x < p->x[0] - 0.01f || f->x > p->x[p->n - 1] + 0.01f)
        WF("%s %s at %.1f s: past the end of %s (path %d): x %.2f, it runs %.1f..%.1f", f->name, when, at, PATH_KIND[p->kind], f->surf, f->x, p->x[0], p->x[p->n - 1]);
    const float d = f->y - terrain_y(p, f->x);
    if (f->surf == 0 ? d < LZ_LANE_BACK - 0.01f || d > LZ_LANE_FRONT + 0.01f : fabsf(d) > 0.01f)
        WF("%s %s at %.1f s: %s %s (path %d) - %.2f px off its line (y %.2f)", f->name, when, at, d < 0 ? "floating over" : "sunk into", PATH_KIND[p->kind], f->surf, d, f->y);
    const float half = tank_lizard_half_len(t, f);
    if (f->x - half < tank_glass_x0(f->y) - 0.01f || f->x + half > tank_glass_x1(f->y) + 0.01f)
        WF("%s %s at %.1f s: through the glass - its body %.1f..%.1f, the glass %.1f..%.1f at y %.1f", f->name, when, at,
           f->x - half, f->x + half, tank_glass_x0(f->y), tank_glass_x1(f->y), f->y);
    return 0;
}
/* a change of path happens where the two meet: last frame it stood on a point both pass through (a ramp's end; two links at
 * one point - a switchback's corner - are crossed in a frame) - never through rock, never a drop */
static int stepped_ok(const tank_t *t, int i, int ps, float px, float py, const char *when, double at) {
    const fish_t *f = &t->fish[i];
    if (f->surf == ps) return 0;
    const terrain_t *tr = tank_terrain(t);
    for (int k = 0; k < tr->n_links; k++) {
        const terrain_link_t *l = &tr->link[k];
        if (fabsf(l->x - px) > 0.05f || fabsf(l->y - py) > 0.05f) continue;
        if (terrain_on(&tr->path[ps], l->x, l->y, 0.05f) && terrain_on(&tr->path[f->surf], l->x, l->y, 0.05f)) return 0;
    }
    WF("%s %s at %.1f s: from path %d at (%.2f, %.2f) onto path %d at (%.2f, %.2f) - not where they meet", f->name, when, at, ps, px, py, f->surf, f->x, f->y);
}

/* the goal each lizard is held to (GOAL_COUNT: the rule stub's) */
static goal_id_t s_force[N_FISH_SLOTS];
static goal_t advisor_force(const tank_t *t, int i, bool request) {
    if (s_force[i] >= GOAL_COUNT) return advisor_rules(t, i, request);
    goal_t g = t->fish[i].goal; g.id = s_force[i]; g.urgency = 5; g.confidence = 1; g.runner_up = GOAL_COUNT;
    return g;
}
static void force(tank_t *t, int i, goal_id_t g) { s_force[i] = g; t->fish[i].goal.id = g; t->fish[i].goal.urgency = 5; t->fish[i].goal_age = 0; t->fish[i].explore_set = false; t->fish[i].hesitate = 0; }
static void hold_drives(tank_t *t) { for (int i = 0; i < t->n_fish; i++) { t->fish[i].hunger = fminf(t->fish[i].hunger, 3); t->fish[i].energy = fmaxf(t->fish[i].energy, 6); } }
/* lizard i set down on path s at x (its line's y there; out of any hide) */
static void stand(tank_t *t, int i, int s, float x) {
    fish_t *f = &t->fish[i];
    const terrain_path_t *p = &tank_terrain(t)->path[s];
    f->hide = -1; f->pose = LZ_POSE_STAND; f->turn_s = f->pause_s = f->walk_s = f->strike_s = 0; f->lane = 0;
    f->surf = (int8_t)s; f->x = x; f->y = terrain_y(p, x); f->slope = terrain_tilt(p, x, f->facing);
}
/* a cricket held sitting on path s at x, its feet on the path's top: neither hopping nor fleeing nor hiding - the walk's
   reaching it is what --selftest-goals tests (the crickets' own life is --selftest-crickets) - until it is eaten */
static void ck_pin(tank_t *t) {
    if (s_ck < 0) return;
    cricket_t *c = &t->cricket[s_ck];
    if (c->state == CK_NONE) { s_ck = -1; return; }          /* (eaten) */
    c->state = CK_SIT; c->surf = (int8_t)s_ck_surf; c->x = s_ck_x; c->timer = 1e9f; c->alert = 0;
    c->y = terrain_y(&tank_terrain(t)->path[s_ck_surf], s_ck_x) + TERRAIN_FEET_DY;
}
static int ck_hold(tank_t *t, int i, int s, float x) { s_ck = i; s_ck_surf = s; s_ck_x = x; ck_pin(t); return i; }
static int ck_put(tank_t *t, int s, float x) { const int i = tank_cricket_drop(t, x, false); return i < 0 ? -1 : ck_hold(t, i, s, x); }

static int ck_first_top(const terrain_t *tr, float x) {
    int top = -1;
    float y = 1e9f;
    for (int s = 0; s < tr->n_paths; s++) {
        const terrain_path_t *p = &tr->path[s];
        if (x < p->x[0] || x > p->x[p->n - 1]) continue;
        const float py = terrain_y(p, x) + TERRAIN_FEET_DY;
        if (py < y) { y = py; top = s; }
    }
    return top;
}

static void ck_stage(tank_t *t, int i, int surf, float x, int state, bool from_player) {
    const terrain_path_t *p = &tank_terrain(t)->path[surf];
    t->cricket[i] = (cricket_t){
        .x = x, .y = terrain_y(p, x) + TERRAIN_FEET_DY, .timer = 100,
        .surf = (int8_t)surf, .state = (uint8_t)state, .facing = 1, .from_player = from_player
    };
}

/* One strike already in its lunge, the prey at its snout: 1 a miss, 0 a catch, -1 neither (one or the other, exactly, is
 * what a strike is). The factors a miss's odds take (tank.h LZ_MISS_*): SK_COLD a lizard at its pack's t_cold (heat 0),
 * SK_HATCH a hatchling (else an adult), SK_HOP a cricket in the air, SK_ALERT a frightened one; none: a comfortable adult
 * at a cricket sitting unaware. */
enum { SK_COLD = 1, SK_HATCH = 2, SK_HOP = 4, SK_ALERT = 8 };
static int strike_once(tank_t *t, int sk) {
    fish_t *f = &t->fish[0];
    const species_t *sp = tank_species(t);
    tank_clear_food(t);
    f->stage = sk & SK_HATCH ? STAGE_FRY : STAGE_ADULT;
    f->body_c = sk & SK_COLD ? sp->t_cold : 0.5f * (sp->t_lo + sp->t_hi);
    f->heat = species_heat_of(sp, f->body_c);
    f->hunger = 5; f->energy = 10; f->stress = 0; f->facing = 1; f->yaw = 1;
    stand(t, 0, 0, tank_floor_x(0.45f));
    force(t, 0, GOAL_LZ_HUNT);
    const float x = f->x + tank_lizard_half_len(t, f);
    int ck = tank_cricket_drop(t, x, false);
    if (ck < 0) return -1;
    ck_stage(t, ck, 0, x, sk & SK_HOP ? CK_HOP : CK_SIT, false);
    cricket_t *c = &t->cricket[ck];
    if (sk & SK_HOP) { c->hop_s = 100; c->x0 = c->x1 = c->x; c->hop_h = 0; c->timer = 1; }   /* (mid-hop, at its snout: a hop just begun) */
    if (sk & SK_ALERT) c->alert = CK_ALERT_S;
    f->prey = (int8_t)ck; f->pose = LZ_POSE_LUNGE; f->strike_s = 1.0f;   /* under way; the production cap is private to tank.c */
    const uint32_t caught = t->ck_caught, missed = t->ck_missed;
    tank_tick(t, 0.01f, advisor_force);
    if (t->ck_missed == missed + 1 && t->ck_caught == caught) return 1;
    if (t->ck_caught == caught + 1 && t->ck_missed == missed) return 0;
    return -1;
}

/* ================================================================ --selftest-walk */
int selftest_walk(void) {
    s_name = "selftest-walk";
    tank_t *t = &s_t;
    fresh(t, 301, 3, false);
    const species_t *sp = tank_species(t);
    const terrain_t *tr = tank_terrain(t);
    int turns = 0, turn_bad = 0, walk_frames = 0, stops = 0, ups = 0, downs = 0, entries = 0, slope_bad = 0;
    long frames_kind[3] = { 0 }, frames_bask_rock = 0;
    float turn_drift = 0, min_headon = 1, slope_err = 0;
    float tx[N_FISH_MAX] = { 0 }; bool turning[N_FISH_MAX] = { false }; float head_min[N_FISH_MAX];
    uint8_t was_pose[N_FISH_MAX]; int8_t was_surf[N_FISH_MAX], was_hide[N_FISH_MAX]; float was_x[N_FISH_MAX], was_y[N_FISH_MAX], on_s[N_FISH_MAX];
    for (int i = 0; i < t->n_fish; i++) { was_pose[i] = t->fish[i].pose; was_surf[i] = t->fish[i].surf; was_hide[i] = t->fish[i].hide; was_x[i] = t->fish[i].x; was_y[i] = t->fish[i].y; on_s[i] = 0; }
    /* 1. a day, a meal, a startle, a night, a morning - every frame on the ground */
    const int day = (int)(600 / DT), night = (int)((sp->settle_s + 300) / DT), morning = (int)(120 / DT);
    for (int k = 0; k < day + night + morning; k++) {
        if (k == day) lights(t, false);
        if (k == day + night) lights(t, true);
        if (k == (int)(60 / DT)) { for (int i = 0; i < t->n_fish; i++) t->fish[i].hunger = 7.5f; tank_feed(t, tank_floor_x(0.3f), 2); tank_feed(t, tank_floor_x(0.8f), 2); }
        if (k == (int)(200 / DT)) for (int n = 0; n < 4; n++) tank_touch_tap(t, t->fish[0].x, t->fish[0].y - 20);   /* a rap: the startle */
        tank_tick(t, DT, advisor_rules); progression_tick(t, DT);
        const char *when = k < day ? "by day" : k < day + night ? "in the dark" : "in the morning";
        for (int i = 0; i < t->n_fish; i++) {
            const fish_t *f = &t->fish[i];
            if (on_ground(t, i, when, k * DT) || stepped_ok(t, i, was_surf[i], was_x[i], was_y[i], when, k * DT)) return 1;
            const terrain_path_t *p = &tr->path[f->surf];
            /* up and down the ramps: off the floor onto a ramp's foot, back down onto it */
            if (f->surf != was_surf[i]) { ups += was_surf[i] == 0; downs += f->surf == 0; on_s[i] = 0; } else on_s[i] += DT;
            frames_kind[p->kind]++; frames_bask_rock += f->surf == tr->bask_path;
            /* into a hide only from the floor, at it (within its footprint: by the glass a hide's point is past a big lizard's reach) */
            if (f->hide >= 0 && was_hide[i] < 0) {
                float hx, hy; tank_hide_point(t, f->hide, &hx, &hy);
                if (f->surf != 0 || fabsf(was_x[i] - hx) > HIDE_R || was_surf[i] != 0)
                    WF("%s went into the %s hide from path %d at x %.1f (its point %.1f)", f->name, HIDE_KIND_NAMES[tank_habitat(t)->hides[f->hide].kind], was_surf[i], was_x[i], hx);
                entries++;
            }
            /* its body takes the ground's slope: settled on a path half a second, it is the ground's under it */
            if (on_s[i] > 0.5f && f->turn_s <= 0) {
                const float e = fabsf(f->slope - terrain_tilt(p, f->x, f->facing));
                slope_err = fmaxf(slope_err, e); slope_bad += e > 0.01f;
            }
            /* a turn about: from the frame its facing flips, it does not move until the turn is done, and it swings head-on */
            if (f->pose == LZ_POSE_TURN && !turning[i]) { turning[i] = true; tx[i] = f->x; head_min[i] = fabsf(f->yaw); turns++; }
            if (turning[i]) {
                head_min[i] = fminf(head_min[i], fabsf(f->yaw));
                if (f->pose != LZ_POSE_TURN) {
                    turning[i] = false;
                    if (head_min[i] > 0.2f) turn_bad++;
                    min_headon = fminf(min_headon, head_min[i]);
                } else turn_drift = fmaxf(turn_drift, fabsf(f->x - tx[i]));
            }
            /* stop-and-go: a walk stops on its way (a stop with the walk's point still ahead) */
            if (f->pose == LZ_POSE_WALK) walk_frames++;
            if (was_pose[i] == LZ_POSE_WALK && (f->pose == LZ_POSE_STAND || f->pose == LZ_POSE_FLICK) && (f->to_surf != f->surf || fabsf(f->to_x - f->x) > LZ_ARRIVE_PX + 1)) stops++;
            was_pose[i] = f->pose; was_surf[i] = f->surf; was_hide[i] = f->hide; was_x[i] = f->x; was_y[i] = f->y;
        }
    }
    const float walk_min = walk_frames * DT / 60;
    const long all = frames_kind[0] + frames_kind[1] + frames_kind[2];
    if (turns < 6 || turn_bad || turn_drift > 0.01f) WF("%d turns about: %d never swung head-on (|yaw| min %.2f), it moved %.2f px turning", turns, turn_bad, min_headon, turn_drift);
    if (walk_min < 2 || stops / walk_min < 4) WF("%.1f lizard-minutes walking with %d stops on the way: no stop-and-go", walk_min, stops);
    if (ups < 2 || downs < 2 || !frames_kind[PATH_RAMP] || !frames_bask_rock) WF("%d climbs up off the floor and %d down onto it, %ld frames on ramps, %ld on the basking rock: the lizards never used the rocks", ups, downs, frames_kind[PATH_RAMP], frames_bask_rock);
    if (slope_bad) WF("%d frames its body's slope was not the ground's (off by %.3f rad at most)", slope_bad, slope_err);
    if (!entries) WF("no lizard went into a hide in a day and a night");
    printf("selftest-walk: %d lizards, %.0f min lit + %.0f min dark + 2 min lit (a meal, a startle): every frame on the ground's paths (the floor's band on the floor), every body inside the glass\n",
           t->n_fish, 600 / 60.0f, (sp->settle_s + 300) / 60);
    printf("selftest-walk: %d climbs off the floor, %d back down onto it, every change of path where two meet; the frames on the floor / rocks / ramps %.0f / %.0f / %.0f %% (the basking rock %.0f %%); %d hides entered, each from the floor at its mouth; the body's slope the ground's (%.4f rad at most off)\n",
           ups, downs, 100.0 * frames_kind[0] / all, 100.0 * frames_kind[1] / all, 100.0 * frames_kind[2] / all, 100.0 * frames_bask_rock / all, entries, slope_err);
    printf("selftest-walk: %d turns about, each on the spot (%.3f px moved at most) and through head-on; %.1f lizard-minutes walking, %.1f stops a minute on the way\n",
           turns, turn_drift, walk_min, stops / walk_min);

    /* 2. its pace on the level: each stage at its pack's walk_px_s (explore's slow walk, urgency 5: x0.8 x1.05), cold slower */
    float pace[4];
    for (int st = 0; st < 4; st++) {
        fresh(t, 310 + st, 1, false);
        if (st) progression_set_age(t, 0, progression_stage_age(t, (stage_t)st) + 1);
        fish_t *f = &t->fish[0];
        if ((int)f->stage != st) WF("staged as stage %d it is %d", st, f->stage);
        for (int cold = 0; cold < (st == 2 ? 2 : 1); cold++) {
            stand(t, 0, 0, tank_floor_x(0.1f)); f->goal.id = GOAL_LZ_EXPLORE; f->goal.urgency = 5; f->explore_set = false;
            for (int z = 0; z < 6; z++) f->zone_seen[z] = 0;
            const float body = cold ? sp->t_cold - 2 : (sp->t_lo + sp->t_hi) / 2;
            float top = 0;
            for (int k = 0; k < (int)(20 / DT); k++) {
                f->body_c = body; f->energy = 9; f->hunger = 2;
                tank_tick(t, DT, NULL);
                if (f->pose == LZ_POSE_WALK && f->surf == 0) top = fmaxf(top, f->speed);   /* (on the level: the floor) */
            }
            const float want = sp->walk_px_s[st] * 0.8f * (0.8f + 5 * 0.05f) * (cold ? HEAT_SLOW_MIN : 1);
            if (fabsf(top - want) > 0.01f * want) WF("stage %d%s explores the floor at %.2f px/s, its pack says %.2f (%.1f x 0.8 x 1.05%s)", st, cold ? " (cold)" : "", top, want, sp->walk_px_s[st], cold ? " x 0.5" : "");
            if (!cold) pace[st] = top;
        }
    }
    printf("selftest-walk: exploring at urgency 5 a hatchling walks %.1f px/s on the level, a juvenile %.1f, an adult %.1f, an elder %.1f (the pack's %.0f / %.0f / %.0f / %.0f x 0.84); cold, half\n",
           pace[0], pace[1], pace[2], pace[3], sp->walk_px_s[0], sp->walk_px_s[1], sp->walk_px_s[2], sp->walk_px_s[3]);

    /* 3. the slope's pace: an adult held to bask walks up every ramp to the high rock - slower uphill by the slope - and held
          to the warm hide walks back down them - a little careful - each ramp at the level's pace x terrain_pace */
    fresh(t, 320, 2, false);
    progression_set_age(t, 0, progression_stage_age(t, STAGE_ADULT) + 1);
    for (int i = 0; i < N_FISH_SLOTS; i++) s_force[i] = GOAL_LZ_REST;
    stand(t, 1, 0, tank_floor_x(0.02f));
    fish_t *f = &t->fish[0];
    stand(t, 0, 0, tank_floor_x(0.98f));
    float level = 0, top_up[TERRAIN_PATHS_MAX] = { 0 }, top_down[TERRAIN_PATHS_MAX] = { 0 };
    for (int leg = 0; leg < 2; leg++) {
        force(t, 0, leg ? GOAL_LZ_HIDE : GOAL_LZ_BASK);
        bool done = false;
        for (float s = 0; s < 240 && !done; s += DT) {
            f->body_c = (sp->t_lo + sp->t_hi) / 2; f->energy = 9; f->hunger = 2; f->shed = SHED_NORMAL; f->shed_s = 1e5f;
            tank_tick(t, DT, advisor_force);
            const terrain_path_t *p = &tr->path[f->surf];
            if (f->pose == LZ_POSE_WALK) {
                const float tilt = terrain_tilt(p, f->x, f->facing) * f->facing;
                if (p->kind != PATH_RAMP) level = fmaxf(level, f->speed);
                else if (tilt > 0) top_up[f->surf] = fmaxf(top_up[f->surf], f->speed);
                else top_down[f->surf] = fmaxf(top_down[f->surf], f->speed);
            }
            done = leg ? f->hide >= 0 : f->surf == tr->bask_path && fabsf(f->x - tr->bask_x) <= LZ_ARRIVE_PX && f->speed == 0;
        }
        if (!done) WF("the slope's pace: held to %s it never got there (on path %d at x %.0f)", leg ? "the warm hide" : "bask", f->surf, f->x);
    }
    printf("selftest-walk: an adult up to the high rock and down to the warm hide, on the level %.1f px/s; ramps", level);
    for (int k = 1 + tr->n_rocks; k < tr->n_paths; k++) {
        const terrain_path_t *p = &tr->path[k];
        const float a = fabsf(terrain_tilt(p, p->x[0], 1)), up = level * terrain_pace(a), down = level * terrain_pace(-a);
        if (fabsf(top_up[k] - up) > 0.01f * up || fabsf(top_down[k] - down) > 0.01f * down)
            WF("ramp %d (%.0f deg): up at %.2f px/s, down at %.2f, want %.2f and %.2f (the level's %.2f x the slope's pace)", k - tr->n_rocks, a * 57.29578f, top_up[k], top_down[k], up, down, level);
        printf(" %d (%.0f deg) up %.1f down %.1f%s", k - tr->n_rocks, a * 57.29578f, top_up[k], top_down[k], k + 1 < tr->n_paths ? "," : "");
    }
    printf(" px/s\n");
    printf("selftest-walk OK\n");
    return 0;
}

/* ================================================================ --selftest-goals */
/* run until pred(t) holds or limit s pass: the seconds it took, -1 = never */
static float until(tank_t *t, float limit, bool (*pred)(const tank_t *), bool drives) {
    for (float s = 0; s < limit; s += DT) {
        tank_tick(t, DT, advisor_force); progression_tick(t, DT);
        if (drives) hold_drives(t);
        if (pred(t)) return s + DT;
    }
    return -1;
}
static float s_px; static int s_k, s_surf;
static bool at_spot(const tank_t *t) { return t->fish[0].surf == s_surf && fabsf(t->fish[0].x - s_px) <= LZ_ARRIVE_PX && t->fish[0].speed == 0; }
static bool near_spot(const tank_t *t) { return t->fish[0].surf == s_surf && fabsf(t->fish[0].x - s_px) <= LZ_ARRIVE_PX; }   /* (an explorer walks on to its next point) */
static bool in_hide_k(const tank_t *t) { return t->fish[0].hide == s_k; }
static bool ate(const tank_t *t) { return t->fish[0].eaten > s_k; }
static const char *kind_of(const tank_t *t, int k) { return k >= 0 && k < tank_habitat(t)->n_hides ? HIDE_KIND_NAMES[tank_habitat(t)->hides[k].kind] : "none"; }
static int hide_of_kind(const tank_t *t, hide_kind_t kind) {
    const habitat_t *h = tank_habitat(t);
    for (int k = 0; k < h->n_hides; k++) if (h->hides[k].kind == kind) return k;
    return -1;
}
/* lizard 0 alone (the other parked far off on the floor, never in its way), on path s at x, held to goal g */
static void stage_on(tank_t *t, uint32_t seed, int s, float x, goal_id_t g) {
    fresh(t, seed, 2, false);
    for (int i = 0; i < N_FISH_SLOTS; i++) s_force[i] = GOAL_LZ_REST;
    fish_t *f = &t->fish[0];
    stand(t, 0, s, x);
    f->body_c = (tank_species(t)->t_lo + tank_species(t)->t_hi) / 2;
    f->hunger = 2; f->energy = 8; f->stress = 0; f->shed = SHED_NORMAL; f->shed_s = 1e5f;
    stand(t, 1, 0, tank_floor_x(x < tank_floor_x(0.5f) ? 0.98f : 0.02f)); t->fish[1].shed_s = 1e5f;
    force(t, 0, g); force(t, 1, GOAL_LZ_REST);
}
static void stage_one(tank_t *t, uint32_t seed, float u, goal_id_t g) { stage_on(t, seed, 0, tank_floor_x(u), g); }
/* the ground's paths it walked, in order, as text (floor, rock 1, ramp 2 ...) */
static void route_word(const tank_t *t, int s, char *out, size_t n) {
    const terrain_t *tr = tank_terrain(t);
    if (s == 0) snprintf(out, n, "floor");
    else if (s <= tr->n_rocks) snprintf(out, n, "rock %d", s);
    else snprintf(out, n, "ramp %d", s - tr->n_rocks);
}
static void route_add(const tank_t *t, char *route, size_t n, int *last) {
    const int s = t->fish[0].surf;
    if (s == *last) return;
    char w[16]; route_word(t, s, w, sizeof w);
    size_t at = strlen(route);
    snprintf(route + at, n - at, "%s%s", at ? " > " : "", w);
    *last = s;
}
int selftest_goals(void) {
    s_name = "selftest-goals";
    tank_t *t = &s_t;
    fresh(t, 400, 2, false);
    const species_t *sp = tank_species(t);
    const terrain_t *tr = tank_terrain(t);
    const habitat_t *hb = tank_habitat(t);
    float bx, by; tank_bask_point(t, &bx, &by);
    if (tr->bask_path == 0 || by >= PLACE_Y - TERRAIN_LIFT_PX) WF("the basking spot is on path %d at y %.0f: not up on a rock", tr->bask_path, by);

    /* bask: from the cool end of the floor up the ramps to the high rock's top, flat on it, and there it stays as it warms */
    char route[160] = ""; int last = -1;
    stage_one(t, 401, 0.95f, GOAL_LZ_BASK); s_px = bx; s_surf = tr->bask_path;
    float got = -1, high = 1e9f;
    for (float s = 0; s < 180 && got < 0; s += DT) {
        tank_tick(t, DT, advisor_force); progression_tick(t, DT); hold_drives(t);
        route_add(t, route, sizeof route, &last); high = fminf(high, t->fish[0].y);
        if (at_spot(t)) got = s + DT;
    }
    if (got < 0) WF("bask: never reached the basking spot on the high rock (path %d at x %.1f; the spot: path %d at %.1f) - %s", t->fish[0].surf, t->fish[0].x, s_surf, bx, route);
    float c0 = t->fish[0].body_c; int moved = 0;
    for (int k = 0; k < (int)(30 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); moved += t->fish[0].surf != s_surf || fabsf(t->fish[0].x - bx) > LZ_ARRIVE_PX; }
    if (moved || t->fish[0].pose != LZ_POSE_BASK || !(t->fish[0].body_c > c0 + 0.5f))
        WF("bask: on the spot it left it %d frames, pose %s, %.1f -> %.1f C", moved, LZ_POSE_NAMES[t->fish[0].pose], c0, t->fish[0].body_c);
    printf("selftest-goals: bask - from the cool end up to the basking spot on the high rock (y %.0f, %.0f px over the floor) in %.1f s: %s; flat on it 30 s, %.1f -> %.1f C\n",
           by, PLACE_Y - by, got, route, c0, t->fish[0].body_c);

    /* hide: down from the high rock into the hide its need picks - warm comfortable, cool hot, humid with a shed soon, on or stuck */
    static const struct { const char *need; hide_kind_t kind; int shed; float body; } NEED[] = {
        { "comfortable", HIDE_WARM, SHED_NORMAL, 0 }, { "hot (heat 9)", HIDE_COOL, SHED_NORMAL, 1 }, { "a shed soon", HIDE_HUMID, SHED_SOON, 0 },
        { "shedding", HIDE_HUMID, SHED_SHEDDING, 0 }, { "stuck", HIDE_HUMID, SHED_STUCK, 0 },
    };
    for (size_t n = 0; n < sizeof NEED / sizeof NEED[0]; n++) {
        const int k = hide_of_kind(t, NEED[n].kind);
        float hx, hy; tank_hide_point(t, k, &hx, &hy);
        stage_on(t, 410 + (uint32_t)n, tr->bask_path, bx, GOAL_LZ_HIDE);
        fish_t *f = &t->fish[0];
        f->shed = (uint8_t)NEED[n].shed; f->shed_s = 1e5f;
        if (NEED[n].body) f->body_c = sp->t_hot + 1;
        s_k = k; route[0] = 0; last = -1;
        got = -1;
        for (float s = 0; s < 180 && got < 0; s += DT) {    /* (hot: held hot - it is the need, not the trip, that is tested) */
            tank_tick(t, DT, advisor_force); hold_drives(t); route_add(t, route, sizeof route, &last);
            if (NEED[n].body && f->hide < 0) { f->body_c = sp->t_hot + 1; f->heat = species_heat_of(sp, f->body_c); }
            if (f->hide >= 0) got = s + DT;
        }
        if (got < 0 || f->hide != k) WF("hide, %s: in the %s hide after %.1f s, want the %s one (%s)", NEED[n].need, kind_of(t, f->hide), got, kind_of(t, k), route);
        l1_snap_t s; advisor_core_l1_snap(t, 0, &s);
        if (!s.in_shelter || s.shelter.x != hx) WF("hide, %s: in it, the L1 snapshot says in_shelter %d at %.1f", NEED[n].need, s.in_shelter, s.shelter.x);
        bool peeked = false, hidden = false;
        for (int q = 0; q < (int)(60 / DT); q++) {
            tank_tick(t, DT, advisor_force); hold_drives(t);
            if (f->hide != k) WF("hide, %s: it came out of the %s hide on its own", NEED[n].need, kind_of(t, k));
            peeked |= f->pose == LZ_POSE_PEEK; hidden |= f->pose == LZ_POSE_HIDDEN;
        }
        if (!peeked || !hidden) WF("hide, %s: a minute inside, hidden %d, peeking %d (want both)", NEED[n].need, hidden, peeked);
        printf("selftest-goals: hide, %s - down from the high rock into the %s hide in %.1f s (%s); a minute in it, hidden and peeking out; in_shelter on the L1 line\n",
               NEED[n].need, kind_of(t, k), got, route);
    }
    /* ... a shed that comes moves it from the warm hide to the humid one, where it comes off clean (the tended clock) */
    {
        const int warm = hide_of_kind(t, HIDE_WARM), humid = hide_of_kind(t, HIDE_HUMID);
        stage_one(t, 420, 0.5f, GOAL_LZ_HIDE);
        fish_t *f = &t->fish[0];
        tank_lizard_enter(t, 0, warm);
        for (int q = 0; q < (int)(5 / DT); q++) { tank_tick(t, DT, advisor_force); progression_tick(t, DT); hold_drives(t); }
        if (f->hide != warm) WF("hide: comfortable in the warm hide, it left it (in %s)", kind_of(t, f->hide));
        f->shed = SHED_SHEDDING; f->shed_s = sp->shed_window_s; f->shed_damp = 0; t->moist[humid] = 0.9f;
        s_k = humid;
        float moved_s = until(t, 120, in_hide_k, true);
        if (moved_s < 0) WF("hide: shedding in the warm hide, it never went to the humid one (in %s)", kind_of(t, f->hide));
        uint16_t c = f->shed_clean; float clean_s = -1;
        for (float s = 0; s < SHED_SOAK_S + 10 && clean_s < 0; s += DT) { tank_tick(t, DT, advisor_force); progression_tick(t, DT); hold_drives(t); if (f->shed_clean > c) clean_s = s + DT; }
        if (clean_s < 0 || fabsf(clean_s - SHED_SOAK_S) > 1) WF("hide: shedding in the damp humid hide, off clean after %.1f s (want %.0f)", clean_s, SHED_SOAK_S);
        printf("selftest-goals: hide - a shed come on in the warm hide sends it to the humid one (%.1f s), off clean %.0f s in\n", moved_s, clean_s);
    }

    /* hunt: a cricket (sitting still - the walk's reaching it is what is tested here; the crickets' own life is
       --selftest-crickets): to it, creeping the last stretch, a freeze, a lunge, caught (a lunge may miss: it tries again) -
       on the sand from the floor and down from the high rock, up on the ledge with the most room from the floor, and one
       from the keeper's hand, which lands on the first top under it */
    {
        int ledge = 1; for (int k = 2; k <= tr->n_rocks; k++) if (tr->path[k].x[1] - tr->path[k].x[0] > tr->path[ledge].x[1] - tr->path[ledge].x[0] && k != tr->bask_path) ledge = k;
        const float lx = 0.5f * (tr->path[ledge].x[0] + tr->path[ledge].x[tr->path[ledge].n - 1]);
        static const char *const WHAT[4] = { "on the sand", "on the sand, from the high rock", "up on the ledge", "from the hand" };
        for (int c4 = 0; c4 < 4; c4++) {
            if (c4 == 1) stage_on(t, 432, tr->bask_path, bx, GOAL_LZ_HUNT); else stage_one(t, 430 + (c4 == 3) + 3 * (c4 == 2), c4 == 3 ? 0.2f : 0.15f, GOAL_LZ_HUNT);
            fish_t *f = &t->fish[0];
            f->hunger = 8;
            int ck; float cx;
            if (c4 < 3) { cx = c4 == 2 ? lx : tank_floor_x(0.55f); ck = ck_put(t, c4 == 2 ? ledge : 0, cx); }
            else {                                            /* from the hand: it falls to the first top under it */
                tank_feed(t, lx, 1); ck = -1; for (int i = 0; i < MAX_CRICKETS; i++) if (t->cricket[i].state != CK_NONE) ck = i;
                if (ck < 0) WF("hunt: the feed gesture dropped no cricket");
                cx = t->cricket[ck].x;
                int top = -1; float ty = 1e9f;                /* (the highest ground over its x: what it must land on) */
                for (int s = 0; s < tr->n_paths; s++) { const terrain_path_t *p = &tr->path[s]; if (cx >= p->x[0] && cx <= p->x[p->n - 1] && terrain_y(p, cx) < ty) { ty = terrain_y(p, cx); top = s; } }
                for (int k = 0; k < (int)(3 / DT) && t->cricket[ck].state == CK_FALL; k++) tank_tick(t, DT, advisor_force);
                const cricket_t *c = &t->cricket[ck];
                if (c->state == CK_FALL || c->surf != top || fabsf(c->y - (terrain_y(&tr->path[top], c->x) + TERRAIN_FEET_DY)) > 0.01f)
                    WF("hunt: a cricket dropped at x %.0f landed on path %d (state %s, feet %.1f px off its top), the first top under it is path %d", cx, c->surf, CK_STATE_NAMES[c->state],
                       c->y - (terrain_y(&tr->path[c->surf >= 0 ? c->surf : 0], c->x) + TERRAIN_FEET_DY), top);
                ck = ck_hold(t, ck, c->surf, c->x); cx = c->x;   /* (sitting still from here, where it landed) */
            }
            s_k = f->eaten; route[0] = 0; last = -1;
            bool froze = false, lunged = false, crept = false;
            const uint32_t miss0 = t->ck_missed;
            got = -1;
            for (float s = 0; s < 180 && got < 0; s += DT) {
                tank_tick(t, DT, advisor_force); f->hunger = fmaxf(f->hunger, 7); ck_pin(t);
                route_add(t, route, sizeof route, &last);
                froze |= f->pose == LZ_POSE_STALK; lunged |= f->pose == LZ_POSE_LUNGE;
                crept |= f->pose == LZ_POSE_WALK && f->speed > 0 && f->speed < 0.6f * tank_species(t)->walk_px_s[f->stage & 3];
                if (ate(t)) got = s + DT;
            }
            if (got < 0 || !froze || !lunged || !crept) WF("hunt, %s: eaten after %.1f s, crept %d, froze %d, lunged %d (%s)", WHAT[c4], got, crept, froze, lunged, route);
            printf("selftest-goals: hunt - a cricket %s (x %.0f): crept up, a freeze, a lunge, caught in %.1f s (%u missed first): %s\n", WHAT[c4], cx, got, (unsigned)(t->ck_missed - miss0), route);
        }
    }
    fish_t *f = &t->fish[0];

    /* explore: to the zone it has not seen for longest - a floor zone (the top row fresh), and the top row's left zone up on
       the rocks */
    for (int top = 0; top < 2; top++) {
        stage_one(t, 440 + top, 0.1f, GOAL_LZ_EXPLORE);
        f = &t->fish[0];
        for (int z = 0; z < 6; z++) f->zone_seen[z] = t->clock;
        if (top) f->zone_seen[0] = t->clock - 120;                                /* the top row's left zone the stalest */
        else { f->zone_seen[4] = t->clock - 60; f->zone_seen[5] = t->clock - 120; }   /* the floor's middle stale, its right end stalest */
        const int start_col = (int)(f->x / (TANK_W / 3.0f));
        tank_tick(t, DT, advisor_force);
        const float ex = f->explore_x, ey = f->explore_y; const int want_col = (int)(ex / (TANK_W / 3.0f)), want_row = ey < TANK_BOT / 2.0f ? 0 : 1;
        s_px = ex; s_surf = f->explore_surf; route[0] = 0; last = -1;
        got = -1;
        for (float s = 0; s < 300 && got < 0; s += DT) { tank_tick(t, DT, advisor_force); progression_tick(t, DT); hold_drives(t); route_add(t, route, sizeof route, &last); if (near_spot(t)) got = s + DT; }   /* (explore's slow walk with long stops: up three ramps) */
        if (start_col != 0 || !f->explore_set || got < 0 || (top ? want_row != 0 || want_col != 0 || s_surf == 0 : want_row != 1 || want_col < 1))
            WF("explore%s: from column %d it chose (%.0f, %.0f) on path %d (row %d column %d; want %s) and reached it after %.1f s", top ? " up" : "",
               start_col, ex, ey, s_surf, want_row, want_col, top ? "the top row's left zone, up on a rock" : "a stale floor column, 1 or 2", got);
        printf("selftest-goals: explore - from the zone it had just seen to stale zone row %d column %d (%.0f, %.0f) in %.1f s%s%s\n", want_row, want_col, ex, ey, got, top ? ": " : "", top ? route : "");
    }

    /* investigate: the other lizard - up to it (social), or a stand-off (territorial) - on the floor, and up on a ledge; with
       nobody, a place */
    for (int low = 0; low < 2; low++) {
        stage_one(t, 450 + low, 0.1f, GOAL_LZ_INVESTIGATE);
        f = &t->fish[0];
        fish_t *o = &t->fish[1]; o->x = tank_floor_x(0.75f);
        f->sociable = low ? 0.1f : 0.8f;
        float closest = 1e9f; got = -1;
        for (float s = 0; s < 90; s += DT) {
            tank_tick(t, DT, advisor_force); hold_drives(t); stand(t, 1, 0, tank_floor_x(0.75f));
            float gap = fabsf(o->x - f->x);
            closest = fminf(closest, gap);
            if (got < 0 && f->speed == 0 && gap <= (low ? LZ_STANDOFF_PX : LZ_CONTACT_PX) + 12 && f->facing == (o->x > f->x ? 1 : -1)) got = s + DT;
        }
        if (got < 0) WF("investigate (social %.1f): never came to the other, %.0f px at closest", f->sociable, closest);
        if (low && (closest < LZ_STANDOFF_PX - 2 || f->pose != LZ_POSE_STANDOFF)) WF("investigate (territorial): it came %.0f px close, pose %s (a stand-off is %.0f off)", closest, LZ_POSE_NAMES[f->pose], LZ_STANDOFF_PX);
        if (!low && closest > LZ_CONTACT_PX + 12) WF("investigate (social): no closer than %.0f px", closest);
        printf("selftest-goals: investigate, social %.1f - facing the other at %.0f px in %.1f s (%s)\n", f->sociable, closest, got, low ? "a stand-off, never closer" : "nose to it");
    }
    {   /* ... the other up on the ledge with the most room: up to it there */
        int ledge = 1; for (int k = 2; k <= tr->n_rocks; k++) if (tr->path[k].x[1] - tr->path[k].x[0] > tr->path[ledge].x[1] - tr->path[ledge].x[0] && k != tr->bask_path) ledge = k;
        const float ox = 0.5f * (tr->path[ledge].x[0] + tr->path[ledge].x[tr->path[ledge].n - 1]);
        stage_one(t, 452, 0.1f, GOAL_LZ_INVESTIGATE);
        f = &t->fish[0]; f->sociable = 0.8f;
        route[0] = 0; last = -1; got = -1;
        for (float s = 0; s < 180 && got < 0; s += DT) {
            stand(t, 1, ledge, ox);
            tank_tick(t, DT, advisor_force); hold_drives(t); route_add(t, route, sizeof route, &last);
            if (f->surf == ledge && f->speed == 0 && fabsf(ox - f->x) <= LZ_CONTACT_PX + 12 && f->facing == (ox > f->x ? 1 : -1)) got = s + DT;
        }
        if (got < 0) WF("investigate: the other up on rock %d at x %.0f - it never came to it (path %d at x %.0f: %s)", ledge, ox, f->surf, f->x, route);
        printf("selftest-goals: investigate - the other up on rock %d: up to it, nose to it, in %.1f s (%s)\n", ledge, got, route);
    }
    fresh(t, 455, 1, false); t->n_fish = 1;                                   /* (alone: the second founder gone) */
    s_force[0] = GOAL_LZ_INVESTIGATE; force(t, 0, GOAL_LZ_INVESTIGATE);
    f = &t->fish[0]; stand(t, 0, 0, tank_floor_x(0.45f));
    for (int k = 0; k < (int)(90 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); }
    {   bool at_place = fabsf(bx - f->x) <= LZ_ARRIVE_PX + 1 && f->surf == tr->bask_path; float px, py;
        for (int k = 0; k < hb->n_hides; k++) { tank_hide_point(t, k, &px, &py); at_place |= f->surf == 0 && fabsf(px - f->x) <= LZ_ARRIVE_PX + 1; }
        if (!at_place || f->pose != LZ_POSE_FLICK) WF("investigate alone: at x %.0f on path %d (pose %s), no place", f->x, f->surf, LZ_POSE_NAMES[f->pose]);
        printf("selftest-goals: investigate, alone - to a place (x %.0f on path %d), tongue-flicking it\n", f->x, f->surf);
    }

    /* every place from every path: set down at either end and the middle of each path, bask reaches the spot and the hide
       goal each hide (held hot for the cool one, shedding for the humid one) */
    {
        int trips = 0; float slowest = 0;
        for (int s = 0; s < tr->n_paths; s++)
            for (int at = 0; at < 3; at++) {
                const terrain_path_t *p = &tr->path[s];
                const float x = at == 0 ? p->x[0] : at == 1 ? 0.5f * (p->x[0] + p->x[p->n - 1]) : p->x[p->n - 1];
                for (int place = -1; place < hb->n_hides; place++) {
                    stage_on(t, 460 + (uint32_t)(s * 16 + at * 4 + place + 1), s, x, place < 0 ? GOAL_LZ_BASK : GOAL_LZ_HIDE);
                    f = &t->fish[0];
                    const hide_kind_t kind = place < 0 ? HIDE_WARM : hb->hides[place].kind;
                    if (place >= 0 && kind == HIDE_HUMID) { f->shed = SHED_SHEDDING; f->shed_s = 1e5f; }
                    got = -1;
                    for (float q = 0; q < 240 && got < 0; q += DT) {
                        if (place >= 0 && kind == HIDE_COOL && f->hide < 0) { f->body_c = sp->t_hot + 1; f->heat = species_heat_of(sp, f->body_c); }
                        tank_tick(t, DT, advisor_force); hold_drives(t);
                        if (on_ground(t, 0, "on a trip", q)) return 1;
                        if (place < 0 ? f->surf == tr->bask_path && fabsf(f->x - bx) <= LZ_ARRIVE_PX && f->speed == 0 : f->hide == place) got = q + DT;
                    }
                    if (got < 0) WF("from path %d at x %.0f it never reached %s%s (on path %d at x %.0f, in hide %d)", s, x, place < 0 ? "the basking spot" : "the hide ", place < 0 ? "" : HIDE_KIND_NAMES[kind], f->surf, f->x, f->hide);
                    trips++; slowest = fmaxf(slowest, got);
                }
            }
        printf("selftest-goals: every place from every path - %d trips from either end and the middle of each of the %d paths to the basking spot and each hide, the slowest %.0f s\n",
               trips, tr->n_paths, slowest);
    }

    /* dig and rest: where it stands (out of a hide first, for a dig; a rest in a hide stays in) - up on a rock, a rest stays
       there, a dig goes down to the sand */
    for (int g = 0; g < 2; g++) {
        stage_one(t, 470 + g, 0.4f, g ? GOAL_LZ_REST : GOAL_LZ_DIG);
        f = &t->fish[0]; const float x0 = f->x; float drift = 0;
        for (int k = 0; k < (int)(20 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); drift = fmaxf(drift, fabsf(f->x - x0)); }
        const uint8_t want = g ? LZ_POSE_REST : LZ_POSE_DIG;
        if (drift > 1 || f->pose != want) WF("%s: moved %.1f px in 20 s, pose %s", g ? "rest" : "dig", drift, LZ_POSE_NAMES[f->pose]);
        const int humid = hide_of_kind(t, HIDE_HUMID);
        tank_lizard_enter(t, 0, humid);
        for (int k = 0; k < (int)(5 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); }
        if (g ? f->hide != humid : (f->hide >= 0 || f->pose != LZ_POSE_DIG)) WF("%s in a hide: hide %d, pose %s", g ? "rest" : "dig", f->hide, LZ_POSE_NAMES[f->pose]);
        stage_on(t, 472 + g, tr->bask_path, bx, g ? GOAL_LZ_REST : GOAL_LZ_DIG);
        f = &t->fish[0];
        float up_got = -1; drift = 0;
        for (float s = 0; s < 120 && up_got < 0; s += DT) {
            tank_tick(t, DT, advisor_force); hold_drives(t); drift = fmaxf(drift, fabsf(f->x - bx) + (f->surf != tr->bask_path) * 1000);
            if (g ? s > 20 : f->surf == 0 && f->pose == LZ_POSE_DIG) up_got = s + DT;
        }
        if (g ? drift > 1 || f->pose != LZ_POSE_REST : up_got < 0) WF("%s on the high rock: path %d at x %.0f, pose %s", g ? "rest" : "dig", f->surf, f->x, LZ_POSE_NAMES[f->pose]);
        if (g) printf("selftest-goals: rest, lying still - in place (%.2f px in 20 s), on the high rock too; in a hide it stays in\n", drift);
        else printf("selftest-goals: dig, scrabbling - in place on the sand; in a hide it comes out and digs at the mouth; on the high rock it goes down to the sand (%.1f s) and digs\n", up_got);
    }

    /* the night: in the sleep phase an active goal is lived as rest; on the rule stub every lizard is soon in a hide - one up
       on the high rock too */
    stage_one(t, 480, 0.3f, GOAL_LZ_EXPLORE);
    f = &t->fish[0];
    stand(t, 1, tr->bask_path, bx);
    lights(t, false);
    for (int k = 0; k < (int)((sp->settle_s + 5) / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); stand(t, 1, tr->bask_path, bx); }
    if (tank_activity_phase(t) != PHASE_SLEEP) WF("night: phase %s after %.0f s dark", PHASE_NAMES[tank_activity_phase(t)], sp->settle_s + 5);
    const float x0 = f->x; float drift = 0;
    for (int k = 0; k < (int)(20 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); drift = fmaxf(drift, fabsf(f->x - x0)); }
    if (drift > 1 || f->pose != LZ_POSE_REST) WF("night: exploring in the sleep phase it moved %.1f px, pose %s (want it lying still)", drift, LZ_POSE_NAMES[f->pose]);
    for (int i = 0; i < N_FISH_SLOTS; i++) s_force[i] = GOAL_COUNT;   /* the rule stub */
    float all_in = -1;
    for (float s = 0; s < 240 && all_in < 0; s += DT) {
        tank_tick(t, DT, advisor_force); hold_drives(t);
        bool in = true; for (int i = 0; i < t->n_fish; i++) in &= t->fish[i].hide >= 0;
        if (in) all_in = s + DT;
    }
    if (all_in < 0) WF("night: on the rule stub not every lizard went into a hide in 4 min (one from the high rock)");
    printf("selftest-goals: night - exploring in the sleep phase it lies still (%.2f px in 20 s); on the rule stub all in hides in %.1f s, one down from the high rock\n", drift, all_in);
    printf("selftest-goals OK\n");
    return 0;
}

/* ================================================================ --selftest-crickets */
/* the first top under (x, y) but path `skip`'s: the ground nearest at or below y at x (-1: none) - where something falling
   straight down from there lands */
static int ck_top_below(const terrain_t *tr, float x, float y, int skip) {
    int top = -1; float by = 1e9f;
    for (int s = 0; s < tr->n_paths; s++) {
        const terrain_path_t *p = &tr->path[s];
        if (s == skip || x < p->x[0] || x > p->x[p->n - 1]) continue;
        const float g = terrain_y(p, x) + TERRAIN_FEET_DY;
        if (g >= y - 0.01f && g < by) { by = g; top = s; }
    }
    return top;
}
/* cricket ck on its ground: on a path, within its ends, its feet on the path's top (mid-hop: over it), inside the glass at
   its feet (CK_GLASS_M in) - never in rock, never through the glass */
static int ck_on_ground(const tank_t *t, int ck, const char *when) {
    const cricket_t *c = &t->cricket[ck];
    const terrain_t *tr = tank_terrain(t);
    if (c->state == CK_NONE || c->state == CK_FALL) WF("%s: the cricket is %s (x %.1f, y %.1f)", when, CK_STATE_NAMES[c->state], c->x, c->y);
    if (c->surf < 0 || c->surf >= tr->n_paths) WF("%s: on no path (%d)", when, c->surf);
    const terrain_path_t *p = &tr->path[c->surf];
    if (c->x < p->x[0] - 0.01f || c->x > p->x[p->n - 1] + 0.01f)
        WF("%s: past the ends of %s %d - x %.1f, it runs %.1f..%.1f", when, PATH_KIND[p->kind], c->surf, c->x, p->x[0], p->x[p->n - 1]);
    const float top = terrain_y(p, c->x) + TERRAIN_FEET_DY;
    if (c->state == CK_HOP ? c->y > top + 0.02f : fabsf(c->y - top) > 0.02f)
        WF("%s: its feet %.2f px off the top of %s %d (%s)", when, c->y - top, PATH_KIND[p->kind], c->surf, CK_STATE_NAMES[c->state]);
    if (c->x < tank_glass_x0(top) + CK_GLASS_M - 0.01f || c->x > tank_glass_x1(top) - CK_GLASS_M + 0.01f)
        WF("%s: through the glass - x %.1f, the glass %.1f..%.1f at its feet (y %.1f)", when, c->x, tank_glass_x0(top), tank_glass_x1(top), top);
    return 0;
}
/* lizard i a comfortable adult on the floor at u, facing right, rested (the hunts below set their own hunger) */
static fish_t *ck_hunter(tank_t *t, int i, float u) {
    fish_t *f = &t->fish[i];
    const species_t *sp = tank_species(t);
    f->stage = STAGE_ADULT; f->body_c = 0.5f * (sp->t_lo + sp->t_hi); f->heat = species_heat_of(sp, f->body_c);
    f->energy = 9; f->stress = 0; f->shed = SHED_NORMAL; f->shed_s = 1e5f; f->facing = 1; f->yaw = 1; f->prey = -1;
    stand(t, i, 0, tank_floor_x(u));
    return f;
}
/* a hunt held to its goal (advisor_force) for a keeper's cricket, wherever the hand's drop lands it: on most of the glass the
   first top under the hand is the high rock or the ramp over the shelf (the drops' sweep below), so the hunt is a climb */
#define CK_CATCH_S 300.0f
/* one hunt on a fresh tank (seed): a starving adult, the rule stub its brain, on the sand at the warm end; a cricket sitting at
   the cool end, about to move. The seconds to its catch, or -1 (a failure printed) past `limit` or without the hunt's every
   part: the hunt goal, the cricket its prey, a stalk, a lunge, the cricket moving meanwhile */
#define CK_HUNTS        8
#define CK_HUNT_LIMIT_S 150.0f
static float ck_moving_hunt(tank_t *t, uint32_t seed, float limit, float *moved, unsigned *missed) {
    fresh(t, seed, 1, false);
    t->n_fish = 1;
    fish_t *f = ck_hunter(t, 0, 0.16f); f->hunger = 9;
    force(t, 0, GOAL_COUNT);                          /* (the rule stub decides) */
    const float cu = tank_floor_x(0.78f);
    const int ck = tank_cricket_drop(t, cu, false);
    ck_stage(t, ck, 0, cu, CK_SIT, false);
    t->cricket[ck].timer = 0.1f;
    const uint32_t catch0 = t->ck_caught, miss0 = t->ck_missed;
    float last_x = cu, caught_s = -1; *moved = 0;
    bool hunting = false, targeted = false, stalked = false, lunged = false;
    for (float s = 0; s < limit && caught_s < 0; s += DT) {
        tank_tick(t, DT, advisor_force);
        hunting |= f->goal.id == GOAL_LZ_HUNT; targeted |= f->prey == ck;
        stalked |= f->pose == LZ_POSE_STALK; lunged |= f->pose == LZ_POSE_LUNGE;
        if (t->cricket[ck].state == CK_NONE) caught_s = s + DT;
        else { *moved += fabsf(t->cricket[ck].x - last_x); last_x = t->cricket[ck].x; f->hunger = fmaxf(f->hunger, 8.6f); f->energy = fmaxf(f->energy, 5); }
    }
    *missed = (unsigned)(t->ck_missed - miss0);
    if (caught_s < 0 || t->ck_caught != catch0 + 1 || !hunting || !targeted || !stalked || !lunged || *moved < CK_HOP_LO_PX) {
        printf("FAIL: %s: a starving lizard (seed %u): caught at %.1f s (bound %.0f), %u caught, hunt %d, targeted %d, stalked %d, lunged %d, the cricket moved %.0f px, %u misses\n",
               s_name, (unsigned)seed, caught_s, limit, (unsigned)(t->ck_caught - catch0), hunting, targeted, stalked, lunged, *moved, *missed);
        return -1;
    }
    return caught_s;
}
int selftest_crickets(void) {
    s_name = "selftest-crickets";
    tank_t *t = &s_t;
    char when[160];

    /* 1. the feed gesture: crickets from the keeper's hand where it was made (a catch of one is a meal); over the mealworm
          dish it fills the dish instead, and drops none */
    fresh(t, 601, 1, false);
    t->n_fish = 0;
    const terrain_t *tr = tank_terrain(t);
    const species_t *sp = tank_species(t);
    float dish_x, dish_y; tank_dish_point(t, &dish_x, &dish_y);
    const float feed_x = tank_floor_x(0.62f);
    if (!tank_dish_here(t) || fabsf(feed_x - dish_x) < 3 * DISH_FEED_PX)
        WF("the %s's dish (%d) at x %.0f, the test's feed spot %.0f", tank_habitat(t)->name, tank_dish_here(t), dish_x, feed_x);
    uint32_t drop0 = t->ck_dropped;
    tank_feed(t, feed_x, 3);
    int fed = 0;
    for (int i = 0; i < MAX_CRICKETS; i++) fed += t->cricket[i].state == CK_FALL && t->cricket[i].from_player && fabsf(t->cricket[i].x - feed_x) <= 14.01f;
    if (fed != 3 || t->ck_dropped != drop0 + 3 || t->dish_worms)
        WF("a feed at x %.0f: %d of the keeper's crickets falling from the hand, %u dropped, %d worms (want 3, 3, none)", feed_x, fed, (unsigned)(t->ck_dropped - drop0), t->dish_worms);
    tank_clear_food(t); drop0 = t->ck_dropped;
    tank_feed(t, dish_x + 0.5f * DISH_FEED_PX, 3);
    if (t->ck_dropped != drop0 || t->dish_worms != DISH_WORMS)
        WF("a feed over the dish: %u crickets, %d worms (want none, a full dish of %d)", (unsigned)(t->ck_dropped - drop0), t->dish_worms, DISH_WORMS);
    t->dish_worms = 0;
    printf("selftest-crickets: a feed gesture drops 3 of the keeper's crickets from the hand; one over the dish (%.0f px off it) fills it instead\n", 0.5f * DISH_FEED_PX);

    /* 2. every drop lands on the first top under it: dropped every 3 px across the glass, each falls straight down onto the
          highest ground at its x - a rock's or a ramp's top over the floor - never into rock, never through the glass */
    int landed[TERRAIN_PATHS_MAX] = { 0 }, drops = 0;
    const terrain_path_t *fl = &tr->path[0];
    for (float x = fl->x[0]; x <= fl->x[fl->n - 1]; x += 3) {
        const int ck = tank_cricket_drop(t, x, true);
        if (ck < 0) WF("no slot for a drop at x %.0f", x);
        const float x0 = t->cricket[ck].x, y0 = t->cricket[ck].y;
        for (int q = 0; q < (int)(3 / DT) && t->cricket[ck].state == CK_FALL; q++) tank_tick(t, DT, NULL);
        snprintf(when, sizeof when, "a drop at x %.1f", x0);
        if (ck_on_ground(t, ck, when)) return 1;
        const cricket_t *c = &t->cricket[ck];
        const int top = ck_first_top(tr, x0);
        if (fabsf(c->x - x0) > 0.01f || c->surf != top || c->y < y0)
            WF("%s fell to (%.1f, %.1f) on %s %d - the first top under it is %s %d", when, c->x, c->y, PATH_KIND[tr->path[c->surf].kind], c->surf, PATH_KIND[tr->path[top].kind], top);
        landed[c->surf]++; drops++;
        tank_clear_food(t);
    }
    printf("selftest-crickets: %d drops, every 3 px of the glass, each straight down onto the first top under it:", drops);
    int rock_tops = 0;
    for (int s = 0; s < tr->n_paths; s++) { printf(" %s %d %d", PATH_KIND[tr->path[s].kind], s, landed[s]); rock_tops += tr->path[s].kind == PATH_ROCK && landed[s]; }
    printf("\n");
    if (!rock_tops) WF("no drop from the hand landed on a rock's top");

    /* 3. off a rock: a fright up on a rock or a ramp sends a cricket off the front now and then (cornered, always) - down
          onto the first top under it, never into the rock it left. The fright is the keeper's rap beside it. A ledge under
          other ground (the desert's low ledge under the high rock, the mid shelf under the last ramp) takes no drop from the
          hand: a cricket reaches it so, off the ground over it - so every rock is landed on, by the hand or by a fall */
    int offs = 0, fell_on[TERRAIN_PATHS_MAX] = { 0 };
    for (int s = 1; s < tr->n_paths; s++) {
        const terrain_path_t *p = &tr->path[s];
        const float mx = 0.5f * (p->x[0] + p->x[p->n - 1]);
        for (int trial = 0; trial < 12; trial++) {               /* (until every rock under it has caught one, or 12 falls) */
            const int ck = tank_cricket_drop(t, mx, false);
            ck_stage(t, ck, s, mx, CK_SIT, false);
            bool off = false; int raps = 0;
            for (float q = 0; q < 120 && !off; q += DT) {
                const cricket_t *c = &t->cricket[ck];
                const float cx0 = c->x, cy0 = c->y; const int was = c->surf; const bool up = c->state != CK_FALL;   /* (before the rap: it may send it off) */
                if (c->state == CK_SIT && c->surf == s && raps < 400) {               /* a rap 40 px under it, to one side or the other */
                    const float rx = c->x + (raps & 1 ? 30.0f : -30.0f);
                    for (int k = 0; k < 3; k++) tank_touch_tap(t, rx, c->y + 40);
                    raps++;
                }
                tank_tick(t, DT, NULL);
                if (up && t->cricket[ck].state == CK_FALL) {                          /* off it: down to the first top under it */
                    const int want = ck_top_below(tr, cx0, cy0, was);
                    for (int k = 0; k < (int)(3 / DT) && t->cricket[ck].state == CK_FALL; k++) tank_tick(t, DT, NULL);
                    snprintf(when, sizeof when, "a fright off %s %d at x %.1f", PATH_KIND[p->kind], s, cx0);
                    if (ck_on_ground(t, ck, when)) return 1;
                    if (t->cricket[ck].surf != want || fabsf(t->cricket[ck].x - cx0) > 0.01f)
                        WF("%s landed on path %d at x %.1f - the first top under it is path %d", when, t->cricket[ck].surf, t->cricket[ck].x, want);
                    fell_on[t->cricket[ck].surf]++; off = true; offs++;
                }
            }
            if (!off) WF("%d raps never sent a cricket off %s %d (it is %s on path %d at x %.1f)", raps, PATH_KIND[p->kind], s,
                         CK_STATE_NAMES[t->cricket[ck].state], t->cricket[ck].surf, t->cricket[ck].x);
            tank_clear_food(t); t->startled = false; t->tap_count = 0;
            bool all = true;
            for (int r = 1; r < tr->n_paths; r++) all &= tr->path[r].kind != PATH_ROCK || landed[r] || fell_on[r];
            if (all && trial >= 1) break;
        }
    }
    printf("selftest-crickets: %d frights off the rocks and ramps, each a drop onto the first top under it:", offs);
    for (int s = 0; s < tr->n_paths; s++) printf(" %s %d %d", PATH_KIND[tr->path[s].kind], s, fell_on[s]);
    printf("\n");
    for (int s = 1; s < tr->n_paths; s++)
        if (tr->path[s].kind == PATH_ROCK && !landed[s] && !fell_on[s]) WF("rock %d took no cricket: none from the hand, none off the ground over it", s);

    /* 4. a hop is a short arc along its path; a lizard walking up sends it off, away, further */
    int ramp = -1;
    for (int s = 0; s < tr->n_paths && ramp < 0; s++) if (tr->path[s].kind == PATH_RAMP) ramp = s;
    if (ramp < 0) WF("the habitat has no ramp (a hop there: no cover on a ramp)");
    const terrain_path_t *rp = &tr->path[ramp];
    const float rx = 0.5f * (rp->x[0] + rp->x[rp->n - 1]);
    int ck = tank_cricket_drop(t, rx, false);
    ck_stage(t, ck, ramp, rx, CK_SIT, false);
    t->cricket[ck].timer = 0;
    bool hopped = false, came_down = false;
    float hop_dx = 0, hop_lift = 0;
    for (int q = 0; q < (int)(4 / DT); q++) {
        tank_tick(t, DT, NULL);
        const cricket_t *c = &t->cricket[ck];
        if (c->state == CK_HOP) {
            hopped = true;
            hop_dx = fmaxf(hop_dx, fabsf(c->x - rx));
            hop_lift = fmaxf(hop_lift, terrain_y(rp, c->x) + TERRAIN_FEET_DY - c->y);
            if (ck_on_ground(t, ck, "mid-hop")) return 1;
        } else if (hopped && c->state == CK_SIT) { came_down = true; break; }
    }
    if (!hopped || !came_down || hop_dx < CK_HOP_LO_PX - 0.5f || hop_dx > CK_HOP_HI_PX + 0.5f || hop_lift < 1)
        WF("an idle hop: hopped %d, came down %d, %.1f px along, %.1f px high", hopped, came_down, hop_dx, hop_lift);
    tank_clear_food(t);
    t->n_fish = 1;
    fish_t *f = &t->fish[0];
    f->away = false; f->facing = 1;
    const float fx = tank_floor_x(0.55f);
    stand(t, 0, 0, fx - tank_lizard_half_len(t, f) - 20);
    f->pose = LZ_POSE_WALK; f->speed = CK_CALM_PX_S + 10;
    ck = tank_cricket_drop(t, fx, false);
    ck_stage(t, ck, 0, fx, CK_SIT, false);
    tank_tick(t, 0.5f, NULL);                         /* (CK_NOTICE_PER_S x 0.5 s past 1: it notices) */
    if (t->cricket[ck].state != CK_HOP || t->cricket[ck].alert <= 0 || t->cricket[ck].x1 < fx + CK_FLEE_LO_PX - 0.5f)
        WF("a lizard walking up from the left: the cricket %s, alert %.1f, a hop %.1f -> %.1f", CK_STATE_NAMES[t->cricket[ck].state], t->cricket[ck].alert, fx, t->cricket[ck].x1);
    printf("selftest-crickets: an idle hop %.1f px along and %.1f px high on a ramp; a lizard walking up sent one %.1f px away\n", hop_dx, hop_lift, t->cricket[ck].x1 - fx);

    /* 5. cover: a frightened cricket crawls into cover (a hide's side, under a rock's end) and is out of every lizard's sight
          while it hides; it comes out again; uneaten, it is gone at CK_LIFE_S - and a night takes them all, the dish's worms
          stay. The lizard watching rests in a hide across the glass */
    tank_clear_food(t);
    tank_lizard_enter(t, 0, tank_habitat(t)->n_hides - 1); force(t, 0, GOAL_LZ_REST); f->hunger = 5;
    float hx, hy; tank_hide_point(t, 0, &hx, &hy);
    const float cx = clampf(hx, fl->x[0] + 10, fl->x[fl->n - 1] - 10);
    ck = tank_cricket_drop(t, cx, false);
    ck_stage(t, ck, 0, cx, CK_SIT, false);
    bool crawled = false, hidden = false;
    for (int q = 0; q < 600 && !hidden; q++) {
        cricket_t *c = &t->cricket[ck];
        if (c->state == CK_SIT) { c->timer = 0; c->alert = CK_ALERT_S; }
        tank_tick(t, 0.1f, advisor_force);
        crawled |= t->cricket[ck].state == CK_CRAWL;
        hidden |= t->cricket[ck].state == CK_HIDE;
    }
    if (!crawled || !hidden || tank_cricket_about(&t->cricket[ck]) || tank_food_sight(t, 0, NULL, NULL, NULL))
        WF("cover: crawled %d, hidden %d, about %d, in the lizard's sight %d", crawled, hidden, tank_cricket_about(&t->cricket[ck]), tank_food_sight(t, 0, NULL, NULL, NULL));
    float hidden_s = 0;
    while (hidden_s < CK_HIDE_HI_S + 1 && t->cricket[ck].state == CK_HIDE) { tank_tick(t, 0.1f, advisor_force); hidden_s += 0.1f; }
    if (t->cricket[ck].state == CK_HIDE || !tank_cricket_about(&t->cricket[ck]) || !tank_food_sight(t, 0, NULL, NULL, NULL))
        WF("cover: still %s after %.1f s (in sight %d) - it never came out", CK_STATE_NAMES[t->cricket[ck].state], hidden_s, tank_food_sight(t, 0, NULL, NULL, NULL));
    const uint32_t lost0 = t->ck_lost;
    t->cricket[ck].age = CK_LIFE_S - 0.05f;
    tank_tick(t, 0.1f, advisor_force);
    if (t->cricket[ck].state != CK_NONE || t->ck_lost != lost0 + 1)
        WF("uneaten at %.0f s: %s, %u gone", CK_LIFE_S, CK_STATE_NAMES[t->cricket[ck].state], (unsigned)(t->ck_lost - lost0));
    tank_cricket_drop(t, tank_floor_x(0.3f), true); tank_cricket_drop(t, tank_floor_x(0.7f), false); t->dish_worms = 3;
    tank_tick_sleep(t, 2 * 3600);
    if (tank_food_count(t) != 1 || t->dish_worms != 3 || t->ck_lost != lost0 + 3)
        WF("a night: %d food left (want the dish alone), %d worms (want 3), %u crickets gone (want 2)", tank_food_count(t), t->dish_worms, (unsigned)(t->ck_lost - lost0 - 1));
    t->dish_worms = 0;
    printf("selftest-crickets: crawled into cover and out of the lizard's sight %.1f s, out again; gone uneaten at %.0f s; a night took the crickets, not the worms\n", hidden_s, CK_LIFE_S);

    /* 6. the AUTO CRICKETS: the tank's own feeding is crickets at a random x, in the active phase only - none through the
          settle and the sleep, none with AUTO FEED off */
    fresh(t, 602, 1, true);
    t->n_fish = 1; t->trickle_off = false; t->autofeed_off = false;
    tank_clear_food(t); f = &t->fish[0]; f->hunger = 9;
    lights(t, false);
    const uint32_t dark_drop = t->ck_dropped;
    for (int q = 0; q < (int)sp->settle_s + 30; q++) { tank_tick(t, 1.0f, NULL); f->hunger = 9; }
    if (t->ck_dropped != dark_drop) WF("AUTO CRICKETS: %u dropped after lights-out (the settle and the sleep)", (unsigned)(t->ck_dropped - dark_drop));
    lights(t, true); t->autofeed_off = true;
    for (int q = 0; q < 20; q++) { tank_tick(t, 1.0f, NULL); f->hunger = 9; }
    if (t->ck_dropped != dark_drop) WF("AUTO FEED off: %u crickets dropped", (unsigned)(t->ck_dropped - dark_drop));
    t->autofeed_off = false;
    for (int q = 0; q < 60 && t->ck_dropped == dark_drop; q++) { tank_tick(t, 1.0f, NULL); f->hunger = 9; }
    bool mine = false; for (int i = 0; i < MAX_CRICKETS; i++) mine |= t->cricket[i].state != CK_NONE && t->cricket[i].from_player;
    if (t->ck_dropped == dark_drop || mine || tank_activity_phase(t) != PHASE_ACTIVE)
        WF("AUTO CRICKETS: none in a lit minute with a starving lizard (or one counted as the keeper's: %d)", mine);
    printf("selftest-crickets: AUTO CRICKETS - none through the settle and the sleep or with AUTO FEED off; lit, the tank's own (not the keeper's)\n");

    /* 7. a strike can miss: a comfortable adult at a cricket sitting unaware catches most, and each of the cold, a
          hatchling's clumsiness, a cricket mid-hop and a frightened one makes it miss more; all of them, nearly always */
    fresh(t, 603, 1, false);
    t->n_fish = 1;
    static const struct { int sk; const char *what; } SK[6] = { { 0, "easy" }, { SK_COLD, "cold" }, { SK_HATCH, "a hatchling" }, { SK_HOP, "mid-hop" },
                                                               { SK_ALERT, "frightened" }, { SK_COLD | SK_HATCH | SK_HOP | SK_ALERT, "all four" } };
    enum { N_STRIKES = 256 };
    int miss[6] = { 0 };
    for (int k = 0; k < 6; k++)
        for (int q = 0; q < N_STRIKES; q++) {
            const int r = strike_once(t, SK[k].sk);
            if (r < 0) WF("a strike (%s) was neither one catch nor one miss", SK[k].what);
            miss[k] += r;
        }
    printf("selftest-crickets: strikes missed of %d:", N_STRIKES);
    for (int k = 0; k < 6; k++) printf(" %s %d", SK[k].what, miss[k]);
    printf("\n");
    if (miss[0] == 0 || miss[0] > N_STRIKES / 3) WF("a comfortable adult at a cricket sitting unaware missed %d of %d (want some, most caught)", miss[0], N_STRIKES);
    for (int k = 1; k < 5; k++) if (miss[k] <= miss[0]) WF("%s: %d misses, no more than the easy strike's %d", SK[k].what, miss[k], miss[0]);
    if (miss[5] < N_STRIKES * 0.85f) WF("all four: only %d of %d missed", miss[5], N_STRIKES);

    /* 8. a starving lizard on the rule stub hunts a cricket on the move across the sand and catches it: stalked, frozen at,
          lunged at - every one of CK_HUNTS hunts (each its own tank) within CK_HUNT_LIMIT_S (200 such hunts, measured
          2026-11-05: median 20 s, 95% within 87 s, the slowest 125 s - a cricket's cover and the rule stub's wandering off
          while it is out of sight are the tail) */
    float worst = 0, sum = 0; unsigned misses = 0;
    for (int k = 0; k < CK_HUNTS; k++) {
        float moved = 0; unsigned missed = 0;
        const float at = ck_moving_hunt(t, 604 + 17 * k, CK_HUNT_LIMIT_S, &moved, &missed);
        if (at < 0) return 1;
        worst = fmaxf(worst, at); sum += at; misses += missed;
    }
    printf("selftest-crickets: a starving lizard on the rule stub, %d hunts of a cricket on the move - stalked, frozen at, lunged at, caught: %.1f s on average, at worst %.1f s (bound %.0f s); %u missed first\n",
           CK_HUNTS, sum / CK_HUNTS, worst, CK_HUNT_LIMIT_S, misses);

    /* 9. a catch is one meal: the keeper's cricket, caught, takes the pack's hunger_per_cricket off, counts once in every
          meal count (eaten, eaten from the keeper's hand, the keeper's feedings: the gesture became a meal) and earns the
          first-meal badge; a tank's own cricket caught after it is eaten, not the keeper's */
    fresh(t, 605, 1, false);
    t->n_fish = 1; f = ck_hunter(t, 0, 0.3f); f->hunger = 8;
    force(t, 0, GOAL_LZ_HUNT);
    const int eaten0 = f->eaten, player0 = f->eaten_player, feed0 = t->player_feedings;
    const uint32_t meal_catch0 = t->ck_caught;
    tank_feed(t, tank_floor_x(0.62f), 1);
    float drop_h = -1, meal_s = -1;
    for (float s = 0; s < CK_CATCH_S && meal_s < 0; s += DT) {
        const float h = f->hunger; const uint32_t c0 = t->ck_caught;
        tank_tick(t, DT, advisor_force);
        if (t->ck_caught != c0) { meal_s = s + DT; drop_h = h - f->hunger; }
    }
    if (meal_s < 0 || t->ck_caught != meal_catch0 + 1 || f->eaten != eaten0 + 1 || f->eaten_player != player0 + 1 || t->player_feedings != feed0 + 1
        || !(f->ms_bits & MS_FIRST_MEAL_FROM_YOU) || fabsf(drop_h - sp->hunger_per_cricket) > 0.01f)
        WF("the keeper's cricket: caught at %.1f s, %u caught, eaten +%d, from the hand +%d, feedings +%d, badge %d, hunger -%.3f (want 1 each, -%.2f)",
           meal_s, (unsigned)(t->ck_caught - meal_catch0), f->eaten - eaten0, f->eaten_player - player0, t->player_feedings - feed0,
           !!(f->ms_bits & MS_FIRST_MEAL_FROM_YOU), drop_h, sp->hunger_per_cricket);
    f->hunger = 8;
    tank_cricket_drop(t, tank_floor_x(0.62f), false);
    float own_s = -1;
    for (float s = 0; s < CK_CATCH_S && own_s < 0; s += DT) { const uint32_t c0 = t->ck_caught; tank_tick(t, DT, advisor_force); if (t->ck_caught != c0) own_s = s + DT; }
    if (own_s < 0 || f->eaten != eaten0 + 2 || f->eaten_player != player0 + 1 || t->player_feedings != feed0 + 1)
        WF("the tank's own cricket: caught at %.1f s, eaten +%d, from the hand +%d, feedings +%d (want 2, 1, 1)", own_s, f->eaten - eaten0, f->eaten_player - player0, t->player_feedings - feed0);
    printf("selftest-crickets: the keeper's cricket caught in %.1f s - hunger -%.2f, one meal in each count and the badge; the tank's own after it eaten, not the keeper's\n", meal_s, drop_h);

    /* 10. THE MEALWORM DISH: the habitat's fixed place on the sand, filled by the feed gesture over it. A sure meal and a slow
           one: a look in, a dab and a worm every time (never a miss), DISH_WORM_X of a cricket's worth, a chew before the next;
           empty, it is out of sight. A cricket is the better meal: one a little farther off than the dish goes first */
    fresh(t, 606, 1, false);
    t->n_fish = 1; f = ck_hunter(t, 0, 0.7f); f->hunger = 9;
    force(t, 0, GOAL_LZ_HUNT);
    if (tank_food_sight(t, 0, NULL, NULL, NULL)) WF("the empty dish is in sight");
    const int dfeed0 = t->player_feedings;
    tank_feed(t, dish_x, 3);
    float sx, sy;
    if (t->dish_worms != DISH_WORMS || !tank_food_sight(t, 0, &sx, &sy, NULL) || fabsf(sx - dish_x) > 0.01f) WF("the filled dish: %d worms, not in sight at its place", t->dish_worms);
    const uint32_t dmiss0 = t->ck_missed, worm0 = t->dish_eaten;
    int worms = 0; float first_s = -1, last_s = -1, gap_min = 1e9f, worth_min = 1e9f, worth_max = 0;
    for (float s = 0; s < 120 && t->dish_worms > 0; s += DT) {
        const float h = f->hunger; const uint32_t e0 = t->dish_eaten;
        tank_tick(t, DT, advisor_force);
        if (t->dish_eaten != e0) {
            worth_min = fminf(worth_min, h - f->hunger); worth_max = fmaxf(worth_max, h - f->hunger);
            if (last_s >= 0) gap_min = fminf(gap_min, s + DT - last_s);
            if (first_s < 0) first_s = s + DT;
            last_s = s + DT; worms++;
            f->hunger = 9;                                /* (still hungry: the next one) */
        }
        if (f->prey >= 0 && f->prey != PREY_DISH) WF("hunting the dish alone, its prey is slot %d", f->prey);
    }
    const float worth = DISH_WORM_X * sp->hunger_per_cricket;
    if (worms != DISH_WORMS || t->dish_eaten != worm0 + DISH_WORMS || t->dish_worms || t->ck_missed != dmiss0 || fabsf(worth_min - worth) > 0.01f || fabsf(worth_max - worth) > 0.01f
        || gap_min < DISH_CHEW_S || t->player_feedings != dfeed0 + 1 || tank_food_sight(t, 0, NULL, NULL, NULL))
        WF("the dish: %d worms eaten (want %d), %d left, %u misses, each -%.3f..%.3f hunger (want %.2f), %.2f s apart at least (a chew: %.1f), feedings +%d, empty in sight %d",
           worms, DISH_WORMS, t->dish_worms, (unsigned)(t->ck_missed - dmiss0), worth_min, worth_max, worth, gap_min, DISH_CHEW_S, t->player_feedings - dfeed0, tank_food_sight(t, 0, NULL, NULL, NULL));
    printf("selftest-crickets: the dish - its %d worms, every one taken (no miss), -%.2f hunger each, the first at %.1f s, the next at least %.2f s apart; then out of sight\n",
           worms, worth, first_s, gap_min);
    const float lx = clampf(dish_x + 80, fl->x[0] + 50, fl->x[fl->n - 1] - 200);
    for (int far = 0; far < 2; far++) {                   /* a cricket 1.3 x the dish's way off on the other side, then 2.4 x */
        tank_clear_food(t); t->dish_worms = DISH_WORMS;
        f = ck_hunter(t, 0, 0.5f); stand(t, 0, 0, lx); f->hunger = 7;
        const float d = lx - dish_x, kx = lx + (far ? 2.4f : 1.3f) * d;
        ck = tank_cricket_drop(t, kx, false); ck_stage(t, ck, 0, kx, CK_SIT, false); t->cricket[ck].timer = 1e9f;
        tank_tick(t, DT, advisor_force);
        if (f->prey != (far ? PREY_DISH : ck)) WF("a cricket %.1f x the dish's way off: its prey is %d (want %s)", far ? 2.4f : 1.3f, f->prey, far ? "the dish" : "the cricket");
    }
    printf("selftest-crickets: a cricket 1.3 x as far as the dish is the one it goes for; at 2.4 x, the dish\n");
    printf("selftest-crickets OK\n");
    return 0;
}

/* ================================================================ --selftest-gestures */
int selftest_gestures(void) {
    s_name = "selftest-gestures";
    tank_t *t = &s_t;

    /* 1. THE HAND: a finger resting on the glass is the keeper's hand resting in the terrarium, on the ground nearest the
          finger (here the sand under it). A trusting lizard comes from across the sand and stands beside it - the
          hold-approach, once, and its badge; a wary one (trust under 4) stays where it is. Both earn trust while it rests */
    fresh(t, 611, 2, false);
    const terrain_t *tr = tank_terrain(t);
    fish_t *f = &t->fish[0], *g = &t->fish[1];
    f->facing = g->facing = 1;
    stand(t, 0, 0, tank_floor_x(0.10f)); stand(t, 1, 0, tank_floor_x(0.32f));
    force(t, 0, GOAL_LZ_REST); force(t, 1, GOAL_LZ_REST);
    f->trust = 6; g->trust = 2;
    const float hand_x = tank_floor_x(0.93f), hand_y = terrain_y(&tr->path[0], hand_x) - 30;   /* (the finger a little over the open sand by the cool end) */
    const float ftrust0 = f->trust, gtrust0 = g->trust, gx0 = g->x;
    float approach_s = -1, held = 0, g_moved = 0;
    for (float s = 0; s < 120 && approach_s < 0; s += DT) {
        tank_touch_hold(t, hand_x, hand_y);
        tank_tick(t, DT, advisor_force); held += DT;
        for (int i = 0; i < 2; i++) { t->fish[i].hunger = 2; t->fish[i].energy = 10; }
        g_moved = fmaxf(g_moved, fabsf(g->x - gx0));
        if ((f->ms_bits & MS_FIRST_HOLD_APPROACH) && tank_at_finger(t, 0)) approach_s = s + DT;
    }
    for (float s = 0; s < 6; s += DT) {                   /* (a few seconds more: it settles beside the hand) */
        tank_touch_hold(t, hand_x, hand_y); tank_tick(t, DT, advisor_force); held += DT;
        for (int i = 0; i < 2; i++) { t->fish[i].hunger = 2; t->fish[i].energy = 10; }
        g_moved = fmaxf(g_moved, fabsf(g->x - gx0));
    }
    float px, py; tank_hand_point(t, &px, &py);
    if (fabsf(px - hand_x) > 0.01f || fabsf(py - terrain_y(&tr->path[0], hand_x)) > 0.01f) WF("the hand at (%.0f, %.0f) rests at (%.1f, %.1f), not on the sand under it", hand_x, hand_y, px, py);
    if (approach_s < 0 || t->hold_approaches != 1 || f->surf != 0 || fabsf(fabsf(f->x - px) - LZ_HAND_GAP) > LZ_ARRIVE_PX + 1 || (px > f->x ? 1 : -1) != f->facing)
        WF("the hand: the trusting lizard approached at %.1f s (%d approaches), stands %.1f px from it on path %d facing %d (want %.0f, facing it)",
           approach_s, t->hold_approaches, f->x - px, f->surf, f->facing, LZ_HAND_GAP);
    if ((g->ms_bits & MS_FIRST_HOLD_APPROACH) || tank_at_finger(t, 1) || g_moved > 2)
        WF("the wary lizard (trust %.1f) came %.0f px toward the hand (badge %d)", gtrust0, g_moved, !!(g->ms_bits & MS_FIRST_HOLD_APPROACH));
    if (!(f->trust > ftrust0 + 0.005f * held) || !(g->trust > gtrust0 + 0.005f * held))
        WF("trust over %.0f s of the hand: %.3f -> %.3f, the wary one %.3f -> %.3f", held, ftrust0, f->trust, gtrust0, g->trust);
    printf("selftest-gestures: the hand rests on the sand under the finger; the trusting lizard came across in %.1f s and stands %.0f px beside it, facing it (one approach, its badge); "
           "the wary one stayed; trust +%.3f / +%.3f in %.0f s\n", approach_s, fabsf(f->x - px), f->trust - ftrust0, g->trust - gtrust0, held);

    /* 2. the hand over the high rock: the ground nearest the finger is the rock's top - a trusting lizard climbs up to it */
    fresh(t, 614, 1, false);
    t->n_fish = 1; f = &t->fish[0];
    f->facing = -1; stand(t, 0, 0, tank_floor_x(0.9f)); force(t, 0, GOAL_LZ_REST); f->trust = 7;
    float bx, by; tank_bask_point(t, &bx, &by);
    float up_s = -1;
    for (float s = 0; s < 180 && up_s < 0; s += DT) {
        tank_touch_hold(t, bx, by - 30);
        tank_tick(t, DT, advisor_force); f->hunger = 2; f->energy = 10;
        if (f->surf == tr->bask_path && tank_at_finger(t, 0)) up_s = s + DT;
    }
    tank_hand_point(t, &px, &py);
    if (up_s < 0 || fabsf(py - terrain_y(&tr->path[tr->bask_path], px)) > 0.01f)
        WF("the hand over the high rock (resting at %.0f, %.0f): the lizard on path %d at %.0f, never beside it", px, py, f->surf, f->x);
    printf("selftest-gestures: the hand over the high rock rests on its top; a trusting lizard climbed from the cool end to it in %.1f s\n", up_s);

    /* 3. THE RAP: three quick taps on empty glass (80 px from the lizard, past its body's tap) - the lizard pays a fish's
          stress and trust and dashes for cover, the cricket by the taps flies; it stays in cover while the fright lasts
          (another rap meanwhile starts it again), then comes out */
    fresh(t, 612, 1, false);
    t->n_fish = 1; f = &t->fish[0];
    f->facing = 1; stand(t, 0, 0, tank_floor_x(0.45f)); force(t, 0, GOAL_LZ_EXPLORE);
    f->trust = 7; f->stress = 1; f->hunger = 2; f->energy = 10;
    float tap_x = f->x + 80;
    if (tap_x > tr->path[0].x[tr->path[0].n - 1] - 10) tap_x = f->x - 80;
    const float tap_y = terrain_y(&tr->path[0], tap_x);
    if (tank_tap_d2(t, 0, f->x, f->y, tap_x, tap_y) < TANK_TAP_R * TANK_TAP_R) WF("the rap's spot is a tap on the lizard");
    tank_clear_food(t);
    int ck = tank_cricket_drop(t, tap_x, false);
    ck_stage(t, ck, 0, tap_x, CK_SIT, false);
    const float stress0 = f->stress, rap_trust0 = f->trust;
    tank_touch_tap(t, tap_x, tap_y);
    tank_touch_tap(t, tap_x, tap_y);
    if (t->startled || f->spook_s > 0) WF("a rap after two taps");
    tank_touch_tap(t, tap_x, tap_y);
    if (!t->startled || fabsf(f->stress - (stress0 + 2.5f)) > 0.001f || fabsf(f->trust - (rap_trust0 - 0.4f)) > 0.001f ||
        f->spook_s <= 0 || t->cricket[ck].alert <= 0 || t->cricket[ck].state != CK_HOP)
        WF("the rap: startled %d, stress %.2f, trust %.2f, spooked %.1f s, the cricket %s alert %.1f",
           t->startled, f->stress, f->trust, f->spook_s, CK_STATE_NAMES[t->cricket[ck].state], t->cricket[ck].alert);
    float cover_s = -1, dash = 0;
    for (float s = 0; s < 80 && cover_s < 0; s += DT) {
        tank_tick(t, DT, advisor_force); f->hunger = 2; f->energy = 10;
        dash = fmaxf(dash, f->speed);
        if (f->hide >= 0) cover_s = s + DT;
    }
    if (cover_s < 0 || f->hide != f->spook_hide) WF("the rap: never into cover (hide %d, its dash's %d, spooked %.1f s)", f->hide, f->spook_hide, f->spook_s);
    const int hid = f->hide;
    float in_s = 0, out_s = -1;
    for (float s = 0; s < LZ_SPOOK_S + 60 && out_s < 0; s += DT) {
        if (fabsf(s - 8.0f) < DT * 0.5f) for (int k = 0; k < 3; k++) tank_touch_tap(t, tap_x, tap_y);   /* (another rap, 8 s in: in again for the whole fright) */
        tank_tick(t, DT, advisor_force); f->hunger = 2; f->energy = 10;
        if (f->hide >= 0) in_s = s + DT; else out_s = s + DT;
    }
    if (in_s < 8 + LZ_SPOOK_S * 0.9f || out_s < 0)
        WF("the rap: in its cover %.1f s after it got there, out at %.1f s (want the fright out first: %.0f s from the last rap, 8 s in)", in_s, out_s, LZ_SPOOK_S);
    printf("selftest-gestures: a rap cost the lizard 0.4 trust and +2.5 stress; the cricket flew; it dashed (%.0f px/s) into hide %d in %.1f s, stayed %.1f s (another rap 8 s in), then came out\n",
           dash, hid, cover_s, in_s);

    /* 4. A TAP ON A LIZARD is its card: the platforms' hit test runs along its body as drawn, nose to tail tip - an adult's
          nose is past the fish's TANK_TAP_R from its middle; a tap clear of it is the glass's */
    fresh(t, 615, 1, false);
    t->n_fish = 1; f = ck_hunter(t, 0, 0.5f);
    const float half = tank_lizard_half_len(t, f), r2 = TANK_TAP_R * TANK_TAP_R;
    if (half < TANK_TAP_R) WF("an adult's half length %.1f px is inside TANK_TAP_R %.0f: the test proves nothing", half, TANK_TAP_R);
    const struct { const char *what; float dx, dy; bool hit; } TP[] = {
        { "its nose", half, 0, true }, { "its tail tip", -half, 0, true }, { "its middle", 0, 0, true },
        { "just over its back", 0.5f * half, -(TANK_TAP_R - 4), true }, { "past its nose", half + TANK_TAP_R + 4, 0, false },
        { "over it", 0, -(TANK_TAP_R + 4), false } };
    for (int k = 0; k < (int)(sizeof TP / sizeof *TP); k++) {
        const bool hit = tank_tap_d2(t, 0, f->x, f->y, f->x + TP[k].dx, f->y + TP[k].dy) < r2;
        if (hit != TP[k].hit) WF("a tap at %s (%+.0f, %+.0f px): %s", TP[k].what, TP[k].dx, TP[k].dy, hit ? "the lizard's card" : "the glass");
    }
    printf("selftest-gestures: a tap on a lizard - its nose and its tail tip (%.0f px off its middle), its middle, over its back - is its card; past it and over it, the glass\n", half);

    /* 5. a tool owns the glass, as ever: none of them feeds or rests a hand; the mister's hold is its soak. Back to the bare
          hand, the same tap at the top drops crickets */
    fresh(t, 613, 1, false);
    t->n_fish = 1; f = &t->fish[0]; tank_clear_food(t);
    const float top_x = tank_floor_x(0.62f), top_y = tank_glass_top(top_x) + 1;
    const uint32_t tool_drop0 = t->ck_dropped;
    tank_set_tool(t, TOOL_SPONGE);
    tank_touch_tap(t, top_x, top_y); tank_touch_hold(t, f->x, f->y); tank_touch_drag(t, top_x, top_y + 20);
    if (t->tool != TOOL_SPONGE || t->hold_active || t->ck_dropped != tool_drop0) WF("the sponge in hand: a hand rested or crickets dropped");
    tank_set_tool(t, TOOL_SCISSORS);
    tank_touch_tap(t, top_x, top_y); tank_touch_hold(t, f->x, f->y); tank_touch_drag(t, top_x + 20, top_y + 20);
    if (t->tool != TOOL_SCISSORS || t->hold_active || t->ck_dropped != tool_drop0) WF("the scissors in hand: a hand rested or crickets dropped");
    tank_set_tool(t, TOOL_MISTER);
    tank_touch_hold(t, f->x, f->y); tank_touch_tap(t, top_x, top_y);
    if (t->tool != TOOL_MISTER || !t->mist_hold || t->hold_active || t->ck_dropped != tool_drop0) WF("the mister in hand: its hold not its own, or crickets dropped");
    tank_set_tool(t, TOOL_HAND);
    tank_touch_tap(t, top_x, top_y);
    if (t->ck_dropped != tool_drop0 + 3) WF("the bare hand's tap at the top dropped %u crickets, want 3", (unsigned)(t->ck_dropped - tool_drop0));
    printf("selftest-gestures: the sponge and the scissors keep their strokes, the mister its hold, none feeds or rests a hand; the bare hand's tap at the top drops 3 crickets\n");
    printf("selftest-gestures OK\n");
    return 0;
}

/* ================================================================ --selftest-terr-pay */
/* the lizards held to their goals (advisor_force) with the tank's progression, hungry but not starving, `limit` s - or until
   a cricket is caught (the seconds it took; -1 never) */
static float pay_live(tank_t *t, float limit, bool until_catch) {
    for (float s = 0; s < limit; s += DT) {
        const uint32_t c0 = t->ck_caught;
        tank_tick(t, DT, advisor_force); progression_tick(t, DT);
        for (int i = 0; i < t->n_fish; i++) t->fish[i].hunger = fminf(fmaxf(t->fish[i].hunger, 6), 8);
        if (until_catch && t->ck_caught != c0) return s + DT;
    }
    return until_catch ? -1 : limit;
}
int selftest_terr_pay(void) {
    s_name = "selftest-terr-pay";
    tank_t *t = &s_t;
    char what[160];
#define PAY(want, ...) do { const int got_ = (int)t->sd_balance - b0; if (got_ != (want)) { snprintf(what, sizeof what, __VA_ARGS__); WF("%s: paid %d, want %d", what, got_, (want)); } b0 = (int)t->sd_balance; } while (0)

    /* 1. KEEPER MEALS: a cricket from the keeper's hand, caught, pays SD_MEAL - once a lizard per SD_MEAL_GAP_S of awake
          time (a fish's own rule); a cricket the tank dropped itself pays nothing, even with the wait over */
    fresh(t, 621, 1, false);
    t->n_fish = 1;
    fish_t *f = ck_hunter(t, 0, 0.25f); f->hunger = 7;
    force(t, 0, GOAL_LZ_HUNT);
    progression_tick(t, DT);
    int b0 = (int)t->sd_balance;
    const float kx = tank_floor_x(0.6f);
    tank_feed(t, kx, 1);
    const float s1 = pay_live(t, CK_CATCH_S, true); pay_live(t, 1, false);
    if (s1 < 0) WF("the keeper's cricket was never caught");
    PAY(SD_MEAL, "a keeper's cricket caught at %.1f s", s1);
    tank_feed(t, kx, 1);
    const float s2 = pay_live(t, CK_CATCH_S, true); pay_live(t, 1, false);
    if (s2 < 0) WF("the second keeper's cricket was never caught");
    PAY(0, "a second keeper's cricket %.0f s after the first (the wait: %.0f s)", s2 + 1, SD_MEAL_GAP_S);
    pay_live(t, SD_MEAL_GAP_S, false);
    tank_cricket_drop(t, kx, false);
    if (pay_live(t, CK_CATCH_S, true) < 0) WF("the tank's own cricket was never caught");
    pay_live(t, 1, false);
    PAY(0, "the tank's own cricket, the wait over");
    tank_feed(t, kx, 1);
    if (pay_live(t, CK_CATCH_S, true) < 0) WF("the third keeper's cricket was never caught");
    pay_live(t, 1, false);
    PAY(SD_MEAL, "a keeper's cricket, the wait over");
    if (f->eaten_player != 3 || f->eaten != 4 || !(f->ms_bits & MS_FIRST_MEAL_FROM_YOU))
        WF("keeper meals: %d from the hand of %d eaten, badge %d (want 3 of 4)", f->eaten_player, f->eaten, !!(f->ms_bits & MS_FIRST_MEAL_FROM_YOU));
    printf("selftest-terr-pay: a keeper's cricket caught pays %d; the next within %.0f s nothing; the tank's own, nothing; the keeper's after the wait, %d again\n",
           SD_MEAL, SD_MEAL_GAP_S, SD_MEAL);

    /* 2. two lizards, a keeper's cricket each (each one's own, set down on the sand near it - the hand's landing is
          --selftest-crickets'): a pay each, the wait is each lizard's - and the dish the keeper filled: a worm is a keeper's
          meal too */
    fresh(t, 622, 2, false);
    ck_hunter(t, 0, 0.12f); ck_hunter(t, 1, 0.88f);
    t->fish[1].facing = -1;
    force(t, 0, GOAL_LZ_HUNT); force(t, 1, GOAL_LZ_HUNT);
    progression_tick(t, DT); b0 = (int)t->sd_balance;
    for (int i = 0; i < 2; i++) {
        const uint32_t d0 = t->ck_dropped; tank_feed(t, tank_floor_x(i ? 0.65f : 0.35f), 1);
        int ck = -1; for (int k = 0; k < MAX_CRICKETS; k++) if (t->cricket[k].state == CK_FALL && t->cricket[k].from_player) ck = k;
        if (ck < 0 || t->ck_dropped != d0 + 1) WF("two lizards: the feed dropped no keeper's cricket");
        const float x = tank_floor_x(i ? 0.72f : 0.28f);
        ck_stage(t, ck, 0, x, CK_SIT, true);
    }
    int caught = 0;
    for (float s = 0; s < 2 * CK_CATCH_S && caught < 2; s += DT) {
        const uint32_t c0 = t->ck_caught; pay_live(t, DT, false); caught += (int)(t->ck_caught - c0);
        for (int i = 0; i < 2; i++) if (t->fish[i].eaten_player && s_force[i] == GOAL_LZ_HUNT) force(t, i, GOAL_LZ_REST);   /* (fed: it leaves the other's) */
    }
    pay_live(t, 1, false);
    if (caught < 2 || t->fish[0].eaten_player != 1 || t->fish[1].eaten_player != 1)
        WF("two lizards: %d caught, %d / %d from the hand", caught, t->fish[0].eaten_player, t->fish[1].eaten_player);
    PAY(2 * SD_MEAL, "two lizards, a keeper's cricket each");
    fresh(t, 623, 1, false);
    t->n_fish = 1; f = ck_hunter(t, 0, 0.6f);
    force(t, 0, GOAL_LZ_HUNT);
    progression_tick(t, DT); b0 = (int)t->sd_balance;
    float dish_x, dish_y; tank_dish_point(t, &dish_x, &dish_y);
    tank_feed(t, dish_x, 3);
    for (float s = 0; s < 120 && t->dish_eaten == 0; s += DT) pay_live(t, DT, false);
    pay_live(t, 1, false);
    if (t->dish_eaten == 0) WF("the dish's worms were never eaten");
    PAY(SD_MEAL, "the dish the keeper filled: %u worms eaten", (unsigned)t->dish_eaten);
    printf("selftest-terr-pay: two lizards with a keeper's cricket each, %d each; a worm from the dish the keeper filled, %d\n", SD_MEAL, SD_MEAL);

    /* 3. CLEAN SHEDS: a shed that comes off clean in the damp humid hide, never stuck, pays SD_SHED_CLEAN; a stuck one
          cleared - by a stay in the damp hide, or by the mister's soak - is a clean shed counted, and pays nothing; a growth
          shed off clean pays SD_SHED_CLEAN and its stage's own on top */
    fresh(t, 624, 2, false);
    const species_t *sp = tank_species(t);
    const int humid = tank_humid_hide(t);
    if (humid < 0) WF("the %s has no humid hide", tank_habitat(t)->name);
    f = &t->fish[0];
    stand(t, 1, 0, tank_floor_x(0.9f)); force(t, 1, GOAL_LZ_REST);
    static const struct { const char *what; uint8_t shed; bool growth, soak; int pay; } SH[4] = {
        { "a shed off clean in the damp hide", SHED_SHEDDING, false, false, SD_SHED_CLEAN },
        { "a stuck shed cleared by a damp stay", SHED_STUCK, false, false, 0 },
        { "a stuck shed cleared by the mister's soak", SHED_STUCK, false, true, 0 },
        { "a growth shed off clean", SHED_SHEDDING, true, false, SD_SHED_CLEAN + SD_STAGE_JUV } };
    for (int k = 0; k < 4; k++) {
        t->moist[humid] = 1;
        progression_set_age(t, 0, SH[k].growth ? sp->stage_s[0] - 1 : 10);   /* (the growth one: a second short of the juvenile's age -
                                                                                 it passes it shedding, and that shed is the growth shed) */
        f->shed = SH[k].shed; f->shed_s = 400; f->shed_damp = 0; f->shed_growth = false;
        if (SH[k].soak) { tank_lizard_leave(t, 0); stand(t, 0, 0, tank_floor_x(0.5f)); }
        else tank_lizard_enter(t, 0, humid);
        force(t, 0, GOAL_LZ_REST);
        progression_tick(t, DT); b0 = (int)t->sd_balance;
        const uint16_t clean0 = f->shed_clean;
        if (SH[k].soak) {
            tank_set_tool(t, TOOL_MISTER);
            for (float s = 0; s < SOAK_HOLD_S + 0.5f; s += DT) { tank_touch_hold(t, f->x, f->y); tank_tick(t, DT, advisor_force); progression_tick(t, DT); }
            tank_set_tool(t, TOOL_HAND);
        }
        for (float s = 0; s < SHED_SOAK_S + 5 && f->shed_clean == clean0; s += DT) { tank_tick(t, DT, advisor_force); progression_tick(t, DT); }
        progression_tick(t, DT);
        if (f->shed_clean != clean0 + 1 || f->shed != SHED_NORMAL || f->stage != (SH[k].growth ? STAGE_JUV : STAGE_FRY))
            WF("%s: %d clean sheds (want %d), shed state %d, stage %d", SH[k].what, f->shed_clean, clean0 + 1, f->shed, f->stage);
        PAY(SH[k].pay, "%s", SH[k].what);
    }
    printf("selftest-terr-pay: a shed off clean, never stuck, pays %d; a stuck one cleared - a damp stay or the mister's soak - counts clean and pays 0; a growth shed off clean %d + the juvenile's %d\n",
           SD_SHED_CLEAN, SD_SHED_CLEAN, SD_STAGE_JUV);
#undef PAY
    printf("selftest-terr-pay OK\n");
    return 0;
}

/* ================================================================ --selftest-terrarium */
int selftest_terrarium(void) {
    s_name = "selftest-terrarium";
    tank_t *t = &s_t;
    fresh(t, 501, 3, true);                        /* the founding pair and one more, the tank's own trickle on */
    const species_t *sp = tank_species(t);
    const terrain_t *tr = tank_terrain(t);
    const int humid = tank_humid_hide(t);
    const float moist0 = t->moist[humid];
    const int lit_s = 40 * 60, dark_s = (int)sp->settle_s + 6 * 60, morning_s = 5 * 60;
    const int n = t->n_fish;
    long lit = 0, band47 = 0, band56 = 0, in_hide = 0, sleep = 0, sleep_ok = 0, shedding = 0, shed_in_humid = 0;
    long above = 0, on_kind[3] = { 0 }, on_bask_rock = 0, row_top = 0, zone_lit[6] = { 0 };
    long goal_frames[GOAL_COUNT] = { 0 };
    int zones[N_FISH_MAX] = { 0 }, stuck = 0, stuck_growth = 0, eaten0 = 0;
    bool zone_seen[N_FISH_MAX][6] = { { false } };
    stage_t stage0[N_FISH_MAX]; uint16_t clean0[N_FISH_MAX]; int8_t was_surf[N_FISH_MAX]; float was_x[N_FISH_MAX], was_y[N_FISH_MAX];
    for (int i = 0; i < n; i++) { stage0[i] = t->fish[i].stage; clean0[i] = t->fish[i].shed_clean; eaten0 += t->fish[i].eaten; was_surf[i] = t->fish[i].surf; was_x[i] = t->fish[i].x; was_y[i] = t->fish[i].y; }
    const uint32_t drop0 = t->ck_dropped, caught0 = t->ck_caught, missed0 = t->ck_missed, lost0 = t->ck_lost;
    uint32_t caught_lit = 0; double hunger_lit = 0; float hunger_max = 0;
    const int total = (int)((lit_s + dark_s + morning_s) / DT);
    for (int k = 0; k < total; k++) {
        const float at = k * DT;
        if (k == (int)(lit_s / DT)) lights(t, false);
        if (k == (int)((lit_s + dark_s) / DT)) lights(t, true);
        const uint32_t caught_was = t->ck_caught;
        tank_tick(t, DT, advisor_rules); progression_tick(t, DT);
        if (!t->night) caught_lit += t->ck_caught - caught_was;
        const int phase = tank_activity_phase(t);
        for (int i = 0; i < n; i++) {
            const fish_t *f = &t->fish[i];
            if (on_ground(t, i, "free-running", at) || stepped_ok(t, i, was_surf[i], was_x[i], was_y[i], "free-running", at)) return 1;
            was_surf[i] = f->surf; was_x[i] = f->x; was_y[i] = f->y;
            goal_frames[f->goal.id]++;
            int col = (int)(f->x / (TANK_W / 3.0f)), row = f->y < TANK_BOT / 2.0f ? 0 : 1; if (col > 2) col = 2; if (col < 0) col = 0;
            const int z = row * 3 + col;
            if (!zone_seen[i][z]) { zone_seen[i][z] = true; zones[i]++; }
            if (!t->night) {
                lit++; int h = (int)f->heat; band47 += h >= 4 && h <= 7; band56 += h >= 5 && h <= 6; in_hide += f->hide >= 0;
                hunger_lit += f->hunger; hunger_max = fmaxf(hunger_max, f->hunger);
                above += f->surf != 0; on_kind[tr->path[f->surf].kind]++; on_bask_rock += f->surf == tr->bask_path;
                row_top += row == 0; zone_lit[z]++;
            }
            if (phase == PHASE_SLEEP) { sleep++; sleep_ok += f->hide >= 0 || f->pose == LZ_POSE_REST; }
            if (f->shed == SHED_SHEDDING) { shedding++; shed_in_humid += f->hide == humid; }
            if (f->shed == SHED_STUCK) { stuck++; stuck_growth += f->shed_growth; }   /* (a timed shed in a dried hide sticks by design: the keeper's mister) */
        }
    }
    int grew = 0, clean = 0, eaten = -eaten0;
    for (int i = 0; i < n; i++) { grew += t->fish[i].stage > stage0[i]; clean += t->fish[i].shed_clean - clean0[i]; eaten += t->fish[i].eaten; }
    printf("selftest-terrarium: %d lizards, %d min lit, %.0f min dark, %d min lit: no keeper, the rule stub, the tank's own trickle\n",
           n, lit_s / 60, dark_s / 60.0f, morning_s / 60);
    printf("selftest-terrarium: lit, heat 4-7 %.0f%% of lizard-time (A13's band), 5-6 %.0f%%; in a hide %.0f%%; asleep, in a hide or lying still %.0f%%\n",
           100.0 * band47 / lit, 100.0 * band56 / lit, 100.0 * in_hide / lit, sleep ? 100.0 * sleep_ok / sleep : 0.0);
    printf("selftest-terrarium: lit, above the floor %.0f%% of lizard-time (on rocks %.0f%%, the basking rock %.0f%%, on ramps %.0f%%); the zone rows top / bottom %.0f%% / %.0f%%; zones",
           100.0 * above / lit, 100.0 * on_kind[PATH_ROCK] / lit, 100.0 * on_bask_rock / lit, 100.0 * on_kind[PATH_RAMP] / lit, 100.0 * row_top / lit, 100.0 * (lit - row_top) / lit);
    for (int z = 0; z < 6; z++) printf(" %d:%.0f%%", z + 1, 100.0 * zone_lit[z] / lit);
    printf("\n");
    printf("selftest-terrarium: sheds: %d off clean, %d lizards grew (a growth shed each), shedding time in the humid hide %.0f%%, stuck %.0f lizard-s (%.0f of it a growth shed); the humid hide %.2f -> %.2f, never misted\n",
           clean, grew, shedding ? 100.0 * shed_in_humid / shedding : 0.0, stuck * DT, stuck_growth * DT, moist0, t->moist[humid]);
    printf("selftest-terrarium: goals");
    for (int g = GOAL_FIRST; g < GOAL_FIRST + GOAL_N; g++) printf(" %s %.0f%%", GOAL_NAMES[g], 100.0 * goal_frames[g] / ((double)total * n));
    printf(" | zones each:"); for (int i = 0; i < n; i++) printf(" %d", zones[i]);
    printf(" | eaten %d\n", eaten);
    const double lizard_h = lit * DT / 3600.0, catches_per_lizard_h = caught_lit / lizard_h;
    printf("selftest-terrarium: AUTO CRICKETS: %u dropped, %u caught (%u lit: %.1f a lizard-hour, A15's >= 4), %u strikes missed (%.0f%% of strikes), %u gone uneaten; hunger lit mean %.1f, at most %.1f\n",
           t->ck_dropped - drop0, t->ck_caught - caught0, caught_lit, catches_per_lizard_h, t->ck_missed - missed0,
           100.0 * (t->ck_missed - missed0) / fmax(1, (t->ck_missed - missed0) + (t->ck_caught - caught0)), t->ck_lost - lost0, hunger_lit / lit, hunger_max);
    for (int i = 0; i < n; i++)
        printf("  %-5s %-5s %d clean shed%s, heat %.1f, %s, %s\n", t->fish[i].name, STAGE_NAMES[t->fish[i].stage], t->fish[i].shed_clean, t->fish[i].shed_clean == 1 ? "" : "s",
               t->fish[i].heat, GOAL_NAMES[t->fish[i].goal.id], t->fish[i].hide >= 0 ? HIDE_KIND_NAMES[tank_habitat(t)->hides[t->fish[i].hide].kind] : "out");
    if (100.0 * band47 / lit < 90) WF("lit, heat 4-7 only %.0f%% of the time (P2b's 99%%; the bar: 90%%)", 100.0 * band47 / lit);
    if (grew < n || stuck_growth) WF("%d of %d lizards grew, %.0f lizard-s stuck in a growth shed: the growth sheds did not come off clean on their own", grew, n, stuck_growth * DT);
    if (sleep && 100.0 * sleep_ok / sleep < 90) WF("asleep, in a hide or lying still only %.0f%% of the time", 100.0 * sleep_ok / sleep);
    if (!above || !row_top) WF("lit, %.1f%% of the time above the floor, %.1f%% in the top zone row: the rocks unused", 100.0 * above / lit, 100.0 * row_top / lit);
    if (catches_per_lizard_h < 4.0) WF("AUTO CRICKETS caught %.1f a lit lizard-hour (A15 wants >= 4)", catches_per_lizard_h);
    for (int i = 0; i < n; i++) if (zones[i] < 3) WF("%s saw %d of the zones in %d min", t->fish[i].name, zones[i], (lit_s + morning_s) / 60);
    printf("selftest-terrarium OK\n");
    return 0;
}
