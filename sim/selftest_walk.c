#define _DEFAULT_SOURCE   /* setenv under -std=c11 (glibc) */
/* selftest_walk.c - pocket-terrarium P2b: THE WALK (tank.h), the terrarium's locomotion, through the sim (tank_tick,
 * progression_tick, the advisors) - never by reading a constant back:
 *
 *   ./fishsim --selftest-walk        the lizards stay on the floor: a lit day, a feeding, a startle, a night and a
 *                                    morning on the rule stub, every lizard every frame in the floor's band and its
 *                                    body inside the glass at its height (each board: make ROUND=1 / WATCH=1 /
 *                                    SQUARE=1 check); a turn about is on the spot and swings it head-on; a walk stops
 *                                    and goes; each stage walks at its pack's pace, cold slower
 *   ./fishsim --selftest-goals       each of the seven goals reaches its place: bask the basking spot and stays; hide
 *                                    the hide its need picks (warm, cool when hot, humid with a shed soon, on or stuck
 *                                    - and a shed that comes moves it there, and comes off clean in it); hunt the
 *                                    pellet, down or falling; explore a stale zone; investigate the other (up to it,
 *                                    or a stand-off when territorial) or a place; dig and rest in place; the night
 *   ./fishsim --selftest-terrarium   a free-running terrarium, the rule stub its brain and nobody at the glass: a
 *                                    lit day, a night, a morning. The heat in band, the growth sheds off clean (the
 *                                    humid hide's own start, never misted), the night in hides; the shares printed
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

static const char *s_name = "";
#define WF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)
static tank_t s_t;                           /* (tank_t is big: never on the stack) */
static const float DT = 1.0f / 30;

/* a fresh terrarium on a scratch save (progression booted: the founding pair, hatchlings), n lizards, no pellets */
static void fresh(tank_t *t, uint32_t seed, int n, bool trickle) {
    const char *sav = "/tmp/pocket-terrarium-selftest-walk.sav";
    setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    tank_init(t, seed);
    progression_wake(t, 0);
    t->trickle_off = !trickle;
    for (int i = 0; i < MAX_FOOD; i++) t->food[i].alive = false;
    while (t->n_fish < n && tank_add_fish(t, 0, 1) >= 0) {}
    t->light_override = true; t->light_on = true; t->night = false;
}
static void lights(tank_t *t, bool on) { t->light_override = true; t->light_on = on; }

/* the floor, as the keeper sees it: its middle in the floor's band, its body (LZ_HALF_LEN x its size either side, the
 * fish body render.c draws it with) inside the glass at its height, no NaN */
static int on_floor(const tank_t *t, int i, const char *when, double at) {
    const fish_t *f = &t->fish[i];
    if (f->x != f->x || f->y != f->y) WF("%s %s at %.1f s: NaN (%f, %f)", f->name, when, at, f->x, f->y);
    if (f->y < PLACE_Y + LZ_LANE_BACK - 0.01f || f->y > PLACE_Y + LZ_LANE_FRONT + 0.01f)
        WF("%s %s at %.1f s: off the floor, y %.2f (the band is %.0f..%.0f)", f->name, when, at, f->y, PLACE_Y + LZ_LANE_BACK, PLACE_Y + LZ_LANE_FRONT);
    const float half = LZ_HALF_LEN * f->size;
    if (f->x - half < tank_glass_x0(f->y) - 0.01f || f->x + half > tank_glass_x1(f->y) + 0.01f)
        WF("%s %s at %.1f s: through the glass - its body %.1f..%.1f, the glass %.1f..%.1f at y %.1f", f->name, when, at,
           f->x - half, f->x + half, tank_glass_x0(f->y), tank_glass_x1(f->y), f->y);
    return 0;
}

/* ================================================================ --selftest-walk */
int selftest_walk(void) {
    s_name = "selftest-walk";
    tank_t *t = &s_t;
    fresh(t, 301, 3, false);
    const species_t *sp = tank_species(t);
    int turns = 0, turn_bad = 0, walk_frames = 0, stops = 0;
    float turn_drift = 0, min_headon = 1;
    float tx[N_FISH_MAX] = { 0 }; bool turning[N_FISH_MAX] = { false }; float head_min[N_FISH_MAX];
    uint8_t was_pose[N_FISH_MAX];
    for (int i = 0; i < t->n_fish; i++) was_pose[i] = t->fish[i].pose;
    /* 1. a day, a meal, a startle, a night, a morning - every frame on the floor */
    const int day = (int)(600 / DT), night = (int)((sp->settle_s + 300) / DT), morning = (int)(120 / DT);
    for (int k = 0; k < day + night + morning; k++) {
        if (k == day) lights(t, false);
        if (k == day + night) lights(t, true);
        if (k == (int)(60 / DT)) { for (int i = 0; i < t->n_fish; i++) t->fish[i].hunger = 7.5f; tank_feed(t, tank_floor_x(0.3f), 2); tank_feed(t, tank_floor_x(0.8f), 2); }
        if (k == (int)(200 / DT)) for (int n = 0; n < 4; n++) tank_touch_tap(t, t->fish[0].x, t->fish[0].y - 20);   /* a rap: the startle */
        tank_tick(t, DT, advisor_rules); progression_tick(t, DT);
        for (int i = 0; i < t->n_fish; i++) {
            const fish_t *f = &t->fish[i];
            if (on_floor(t, i, k < day ? "by day" : k < day + night ? "in the dark" : "in the morning", k * DT)) return 1;
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
            if (was_pose[i] == LZ_POSE_WALK && (f->pose == LZ_POSE_STAND || f->pose == LZ_POSE_FLICK) && fabsf(f->to_x - f->x) > LZ_ARRIVE_PX + 1) stops++;
            was_pose[i] = f->pose;
        }
    }
    const float walk_min = walk_frames * DT / 60;
    if (turns < 6 || turn_bad || turn_drift > 0.01f) WF("%d turns about: %d never swung head-on (|yaw| min %.2f), it moved %.2f px turning", turns, turn_bad, min_headon, turn_drift);
    if (walk_min < 2 || stops / walk_min < 4) WF("%.1f lizard-minutes walking with %d stops on the way: no stop-and-go", walk_min, stops);
    printf("selftest-walk: %d lizards, %.0f min lit + %.0f min dark + 2 min lit (a meal, a startle): every frame on the floor's band, every body inside the glass\n",
           t->n_fish, 600 / 60.0f, (sp->settle_s + 300) / 60);
    printf("selftest-walk: %d turns about, each on the spot (%.3f px moved at most) and through head-on; %.1f lizard-minutes walking, %.1f stops a minute on the way\n",
           turns, turn_drift, walk_min, stops / walk_min);

    /* 2. its pace: each stage at its pack's walk_px_s (explore's slow walk, urgency 5: x0.8 x1.05), cold slower */
    float pace[4];
    for (int st = 0; st < 4; st++) {
        fresh(t, 310 + st, 1, false);
        if (st) progression_set_age(t, 0, progression_stage_age(t, (stage_t)st) + 1);
        fish_t *f = &t->fish[0];
        if ((int)f->stage != st) WF("staged as stage %d it is %d", st, f->stage);
        f->x = tank_floor_x(0.1f); f->goal.id = GOAL_LZ_EXPLORE; f->goal.urgency = 5; f->explore_set = false;
        for (int z = 0; z < 6; z++) f->zone_seen[z] = 0;
        f->energy = 9; f->hunger = 2; f->body_c = (sp->t_lo + sp->t_hi) / 2;
        float top = 0;
        for (int k = 0; k < (int)(30 / DT); k++) { tank_tick(t, DT, NULL); f->energy = 9; f->hunger = 2; if (f->pose == LZ_POSE_WALK) top = fmaxf(top, f->speed); }
        pace[st] = top;
        const float want = sp->walk_px_s[st] * 0.8f * (0.8f + 5 * 0.05f);
        if (fabsf(top - want) > 0.01f * want) WF("stage %d explores at %.2f px/s, its pack says %.2f (%.1f x 0.8 x 1.05)", st, top, want, sp->walk_px_s[st]);
        if (st == 2) {                                   /* ... and cold, sluggish: heat 0 halves it */
            f->body_c = sp->t_cold - 2; float cold = 0;
            for (int k = 0; k < (int)(10 / DT); k++) { tank_tick(t, DT, NULL); f->body_c = sp->t_cold - 2; f->energy = 9; if (f->pose == LZ_POSE_WALK) cold = fmaxf(cold, f->speed); }
            if (fabsf(cold - HEAT_SLOW_MIN * want) > 0.01f * want) WF("cold (heat %.1f) it walks %.2f px/s, want x%.1f of %.2f", f->heat, cold, HEAT_SLOW_MIN, want);
        }
    }
    printf("selftest-walk: exploring at urgency 5 a hatchling walks %.1f px/s, a juvenile %.1f, an adult %.1f, an elder %.1f (the pack's %.0f / %.0f / %.0f / %.0f x 0.84); cold, half\n",
           pace[0], pace[1], pace[2], pace[3], sp->walk_px_s[0], sp->walk_px_s[1], sp->walk_px_s[2], sp->walk_px_s[3]);
    printf("selftest-walk OK\n");
    return 0;
}

/* ================================================================ --selftest-goals */
static goal_id_t s_force[N_FISH_SLOTS];      /* the goal each lizard is held to (GOAL_COUNT: the rule stub's) */
static goal_t advisor_force(const tank_t *t, int i, bool request) {
    if (s_force[i] >= GOAL_COUNT) return advisor_rules(t, i, request);
    goal_t g = t->fish[i].goal; g.id = s_force[i]; g.urgency = 5; g.confidence = 1; g.runner_up = GOAL_COUNT;
    return g;
}
static void force(tank_t *t, int i, goal_id_t g) { s_force[i] = g; t->fish[i].goal.id = g; t->fish[i].goal.urgency = 5; t->fish[i].goal_age = 0; t->fish[i].explore_set = false; }
static void hold_drives(tank_t *t) { for (int i = 0; i < t->n_fish; i++) { t->fish[i].hunger = fminf(t->fish[i].hunger, 3); t->fish[i].energy = fmaxf(t->fish[i].energy, 6); } }
/* run until pred(t) holds or limit s pass: the seconds it took, -1 = never */
static float until(tank_t *t, float limit, bool (*pred)(const tank_t *), bool drives) {
    for (float s = 0; s < limit; s += DT) {
        tank_tick(t, DT, advisor_force); progression_tick(t, DT);
        if (drives) hold_drives(t);
        if (pred(t)) return s + DT;
    }
    return -1;
}
static float s_px; static int s_k;
static bool at_x(const tank_t *t) { return fabsf(t->fish[0].x - s_px) <= LZ_ARRIVE_PX && t->fish[0].speed == 0; }
static bool near_x(const tank_t *t) { return fabsf(t->fish[0].x - s_px) <= LZ_ARRIVE_PX; }   /* (an explorer walks on to its next point) */
static bool in_hide_k(const tank_t *t) { return t->fish[0].hide == s_k; }
static bool ate(const tank_t *t) { return t->fish[0].eaten > s_k; }
static const char *kind_of(const tank_t *t, int k) { return k >= 0 && k < tank_habitat(t)->n_hides ? HIDE_KIND_NAMES[tank_habitat(t)->hides[k].kind] : "none"; }
static int hide_of_kind(const tank_t *t, hide_kind_t kind) {
    const habitat_t *h = tank_habitat(t);
    for (int k = 0; k < h->n_hides; k++) if (h->hides[k].kind == kind) return k;
    return -1;
}
/* lizard 0 alone (the others parked far off, never in its way), at floor fraction u, held to goal g */
static void stage_one(tank_t *t, uint32_t seed, float u, goal_id_t g) {
    fresh(t, seed, 2, false);
    for (int i = 0; i < N_FISH_SLOTS; i++) s_force[i] = GOAL_LZ_REST;
    fish_t *f = &t->fish[0];
    f->x = tank_floor_x(u); f->y = PLACE_Y; f->body_c = (tank_species(t)->t_lo + tank_species(t)->t_hi) / 2;
    f->hunger = 2; f->energy = 8; f->stress = 0; f->shed = SHED_NORMAL; f->shed_s = 1e5f;
    t->fish[1].x = tank_floor_x(u < 0.5f ? 0.98f : 0.02f); t->fish[1].shed_s = 1e5f;
    force(t, 0, g); force(t, 1, GOAL_LZ_REST);
}
int selftest_goals(void) {
    s_name = "selftest-goals";
    tank_t *t = &s_t;
    const species_t *sp = tank_species(t);
    float bx, by; tank_bask_point(t, &bx, &by);
    /* bask: to the basking spot, flat on it, and there it stays as it warms */
    stage_one(t, 401, 0.95f, GOAL_LZ_BASK); s_px = bx;
    float got = until(t, 90, at_x, true);
    if (got < 0) WF("bask: never reached the basking spot (x %.1f, the spot %.1f)", t->fish[0].x, bx);
    float c0 = t->fish[0].body_c; int moved = 0;
    for (int k = 0; k < (int)(30 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); moved += fabsf(t->fish[0].x - bx) > LZ_ARRIVE_PX; }
    if (moved || t->fish[0].pose != LZ_POSE_BASK || !(t->fish[0].body_c > c0 + 0.5f))
        WF("bask: on the spot it left it %d frames, pose %s, %.1f -> %.1f C", moved, LZ_POSE_NAMES[t->fish[0].pose], c0, t->fish[0].body_c);
    printf("selftest-goals: bask - from the cool end to the basking spot in %.1f s, flat on it 30 s, %.1f -> %.1f C\n", got, c0, t->fish[0].body_c);

    /* hide: the hide its need picks - warm comfortable, cool hot, humid with a shed soon, on or stuck */
    static const struct { const char *need; hide_kind_t kind; int shed; float body; } NEED[] = {
        { "comfortable", HIDE_WARM, SHED_NORMAL, 0 }, { "hot (heat 9)", HIDE_COOL, SHED_NORMAL, 1 }, { "a shed soon", HIDE_HUMID, SHED_SOON, 0 },
        { "shedding", HIDE_HUMID, SHED_SHEDDING, 0 }, { "stuck", HIDE_HUMID, SHED_STUCK, 0 },
    };
    for (size_t n = 0; n < sizeof NEED / sizeof NEED[0]; n++) {
        const int k = hide_of_kind(t, NEED[n].kind);
        float hx, hy; tank_hide_point(t, k, &hx, &hy);
        stage_one(t, 410 + (uint32_t)n, hx < tank_floor_x(0.5f) ? 0.95f : 0.05f, GOAL_LZ_HIDE);
        fish_t *f = &t->fish[0];
        f->shed = (uint8_t)NEED[n].shed; f->shed_s = 1e5f;
        if (NEED[n].body) f->body_c = sp->t_hot + 1;
        s_k = k;
        got = -1;
        for (float s = 0; s < 120 && got < 0; s += DT) {    /* (hot: held hot - it is the need, not the trip, that is tested) */
            tank_tick(t, DT, advisor_force); hold_drives(t);
            if (NEED[n].body && f->hide < 0) { f->body_c = sp->t_hot + 1; f->heat = species_heat_of(sp, f->body_c); }
            if (f->hide >= 0) got = s + DT;
        }
        if (got < 0 || f->hide != k) WF("hide, %s: in the %s hide after %.1f s, want the %s one", NEED[n].need, kind_of(t, f->hide), got, kind_of(t, k));
        l1_snap_t s; advisor_core_l1_snap(t, 0, &s);
        if (!s.in_shelter || s.shelter.x != hx) WF("hide, %s: in it, the L1 snapshot says in_shelter %d at %.1f", NEED[n].need, s.in_shelter, s.shelter.x);
        bool peeked = false, hidden = false;
        for (int q = 0; q < (int)(60 / DT); q++) {
            tank_tick(t, DT, advisor_force); hold_drives(t);
            if (f->hide != k) WF("hide, %s: it came out of the %s hide on its own", NEED[n].need, kind_of(t, k));
            peeked |= f->pose == LZ_POSE_PEEK; hidden |= f->pose == LZ_POSE_HIDDEN;
        }
        if (!peeked || !hidden) WF("hide, %s: a minute inside, hidden %d, peeking %d (want both)", NEED[n].need, hidden, peeked);
        printf("selftest-goals: hide, %s - into the %s hide in %.1f s; a minute in it, hidden and peeking out; in_shelter on the L1 line\n", NEED[n].need, kind_of(t, k), got);
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

    /* hunt: a pellet on the sand - to it, a freeze, a lunge, caught (a lunge may miss: it tries again) */
    stage_one(t, 430, 0.15f, GOAL_LZ_HUNT);
    fish_t *f = &t->fish[0];
    f->hunger = 8;
    food_t *p = &t->food[0];
    *p = (food_t){ .x = tank_floor_x(0.55f), .y = FOOD_FLOOR_Y, .age = 50, .alive = true, .floor_s = 0.1f };
    s_k = f->eaten;
    bool froze = false, lunged = false;
    got = -1;
    for (float s = 0; s < 60 && got < 0; s += DT) {
        tank_tick(t, DT, advisor_force); f->hunger = fmaxf(f->hunger, 7); p->floor_s = 0.1f;   /* (it stays on the sand) */
        froze |= f->pose == LZ_POSE_STALK; lunged |= f->pose == LZ_POSE_LUNGE;
        if (ate(t)) got = s + DT;
    }
    if (got < 0 || !froze || !lunged) WF("hunt: a pellet on the sand - eaten after %.1f s, froze %d, lunged %d", got, froze, lunged);
    printf("selftest-goals: hunt - a pellet on the sand %.0f px off: stalked, a freeze, a lunge, caught in %.1f s\n", tank_floor_x(0.55f) - tank_floor_x(0.15f), got);
    /* ... one still falling: it waits under it, and takes it once it lands */
    stage_one(t, 431, 0.2f, GOAL_LZ_HUNT);
    f->hunger = 8;
    tank_feed(t, tank_floor_x(0.6f), 1);
    int fp = -1; for (int i = 0; i < MAX_FOOD; i++) if (t->food[i].alive) fp = i;
    if (fp < 0) WF("hunt: no pellet fed");
    s_k = f->eaten; float under = -1; got = -1;
    for (float s = 0; s < 120 && got < 0; s += DT) {
        bool falling = t->food[fp].alive && t->food[fp].floor_s <= 0;
        tank_tick(t, DT, advisor_force); f->hunger = fmaxf(f->hunger, 7);
        if (falling && under < 0 && fabsf(t->food[fp].x - (f->x + f->facing * 12 * f->size)) < 12 && f->speed == 0) under = s + DT;
        if (ate(t)) got = s + DT;
    }
    if (under < 0 || got < 0) WF("hunt: a falling pellet - under it at %.1f s, eaten at %.1f s", under, got);
    printf("selftest-goals: hunt - a pellet still falling: waiting under it from %.1f s, caught as it landed (%.1f s)\n", under, got);

    /* explore: to a floor zone it has not seen for longest */
    stage_one(t, 440, 0.1f, GOAL_LZ_EXPLORE);
    f = &t->fish[0];
    for (int z = 0; z < 6; z++) f->zone_seen[z] = t->clock;
    f->zone_seen[4] = t->clock - 60; f->zone_seen[5] = t->clock - 120;       /* the middle stale, the right end stalest */
    const int start_col = (int)(f->x / (TANK_W / 3.0f));
    tank_tick(t, DT, advisor_force);
    const float ex = f->explore_x; const int want_col = (int)(ex / (TANK_W / 3.0f));
    s_px = ex;
    got = until(t, 90, near_x, true);
    if (start_col != 0 || !f->explore_set || want_col < 1 || got < 0)
        WF("explore: from column %d it chose x %.0f (column %d; want a stale one, 1 or 2) and reached it after %.1f s", start_col, ex, want_col, got);
    printf("selftest-goals: explore - from the zone it had just seen to stale column %d (x %.0f) in %.1f s\n", want_col, ex, got);

    /* investigate: the other lizard - up to it (social), or a stand-off (territorial) - and with nobody, a place */
    for (int low = 0; low < 2; low++) {
        stage_one(t, 450 + low, 0.1f, GOAL_LZ_INVESTIGATE);
        f = &t->fish[0];
        fish_t *o = &t->fish[1]; o->x = tank_floor_x(0.75f);
        f->sociable = low ? 0.1f : 0.8f;
        float closest = 1e9f; got = -1;
        for (float s = 0; s < 90; s += DT) {
            tank_tick(t, DT, advisor_force); hold_drives(t); o->x = tank_floor_x(0.75f); o->speed = 0;
            float gap = fabsf(o->x - f->x);
            closest = fminf(closest, gap);
            if (got < 0 && f->speed == 0 && gap <= (low ? LZ_STANDOFF_PX : LZ_CONTACT_PX) + 12 && f->facing == (o->x > f->x ? 1 : -1)) got = s + DT;
        }
        if (got < 0) WF("investigate (social %.1f): never came to the other, %.0f px at closest", f->sociable, closest);
        if (low && (closest < LZ_STANDOFF_PX - 2 || f->pose != LZ_POSE_STANDOFF)) WF("investigate (territorial): it came %.0f px close, pose %s (a stand-off is %.0f off)", closest, LZ_POSE_NAMES[f->pose], LZ_STANDOFF_PX);
        if (!low && closest > LZ_CONTACT_PX + 12) WF("investigate (social): no closer than %.0f px", closest);
        printf("selftest-goals: investigate, social %.1f - facing the other at %.0f px in %.1f s (%s)\n", f->sociable, closest, got, low ? "a stand-off, never closer" : "nose to it");
    }
    fresh(t, 455, 1, false); t->n_fish = 1;                                   /* (alone: the second founder gone) */
    s_force[0] = GOAL_LZ_INVESTIGATE; force(t, 0, GOAL_LZ_INVESTIGATE);
    f = &t->fish[0]; f->x = tank_floor_x(0.45f);
    for (int k = 0; k < (int)(60 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); }
    {   bool at_place = false; float px, py;
        tank_bask_point(t, &px, &py); at_place |= fabsf(px - f->x) <= LZ_ARRIVE_PX + 1;
        for (int k = 0; k < tank_habitat(t)->n_hides; k++) { tank_hide_point(t, k, &px, &py); at_place |= fabsf(px - f->x) <= LZ_ARRIVE_PX + 1; }
        if (!at_place || f->pose != LZ_POSE_FLICK) WF("investigate alone: at x %.0f (pose %s), no place", f->x, LZ_POSE_NAMES[f->pose]);
        printf("selftest-goals: investigate, alone - to a place (x %.0f), tongue-flicking it\n", f->x);
    }

    /* dig and rest: where it stands (out of a hide first, for a dig; a rest in a hide stays in) */
    for (int g = 0; g < 2; g++) {
        stage_one(t, 460 + g, 0.4f, g ? GOAL_LZ_REST : GOAL_LZ_DIG);
        f = &t->fish[0]; const float x0 = f->x; float drift = 0;
        for (int k = 0; k < (int)(20 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); drift = fmaxf(drift, fabsf(f->x - x0)); }
        const uint8_t want = g ? LZ_POSE_REST : LZ_POSE_DIG;
        if (drift > 1 || f->pose != want) WF("%s: moved %.1f px in 20 s, pose %s", g ? "rest" : "dig", drift, LZ_POSE_NAMES[f->pose]);
        const int humid = hide_of_kind(t, HIDE_HUMID);
        tank_lizard_enter(t, 0, humid);
        for (int k = 0; k < (int)(5 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); }
        if (g ? f->hide != humid : (f->hide >= 0 || f->pose != LZ_POSE_DIG)) WF("%s in a hide: hide %d, pose %s", g ? "rest" : "dig", f->hide, LZ_POSE_NAMES[f->pose]);
        printf("selftest-goals: %s - in place (%.2f px in 20 s); in a hide it %s\n", g ? "rest, lying still" : "dig, scrabbling", drift, g ? "stays in" : "comes out and digs at the mouth");
    }

    /* the night: in the sleep phase an active goal is lived as rest; on the rule stub every lizard is soon in a hide */
    stage_one(t, 470, 0.3f, GOAL_LZ_EXPLORE);
    f = &t->fish[0];
    lights(t, false);
    for (int k = 0; k < (int)((sp->settle_s + 5) / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); }
    if (tank_activity_phase(t) != PHASE_SLEEP) WF("night: phase %s after %.0f s dark", PHASE_NAMES[tank_activity_phase(t)], sp->settle_s + 5);
    const float x0 = f->x; float drift = 0;
    for (int k = 0; k < (int)(20 / DT); k++) { tank_tick(t, DT, advisor_force); hold_drives(t); drift = fmaxf(drift, fabsf(f->x - x0)); }
    if (drift > 1 || f->pose != LZ_POSE_REST) WF("night: exploring in the sleep phase it moved %.1f px, pose %s (want it lying still)", drift, LZ_POSE_NAMES[f->pose]);
    for (int i = 0; i < N_FISH_SLOTS; i++) s_force[i] = GOAL_COUNT;   /* the rule stub */
    float all_in = -1;
    for (float s = 0; s < 180 && all_in < 0; s += DT) {
        tank_tick(t, DT, advisor_force); hold_drives(t);
        bool in = true; for (int i = 0; i < t->n_fish; i++) in &= t->fish[i].hide >= 0;
        if (in) all_in = s + DT;
    }
    if (all_in < 0) WF("night: on the rule stub not every lizard went into a hide in 3 min");
    printf("selftest-goals: night - exploring in the sleep phase it lies still (%.2f px in 20 s); on the rule stub all in hides in %.1f s\n", drift, all_in);
    printf("selftest-goals OK\n");
    return 0;
}

/* ================================================================ --selftest-terrarium */
int selftest_terrarium(void) {
    s_name = "selftest-terrarium";
    tank_t *t = &s_t;
    fresh(t, 501, 3, true);                        /* the founding pair and one more, the tank's own trickle on */
    const species_t *sp = tank_species(t);
    const int humid = tank_humid_hide(t);
    const float moist0 = t->moist[humid];
    const int lit_s = 40 * 60, dark_s = (int)sp->settle_s + 6 * 60, morning_s = 5 * 60;
    const int n = t->n_fish;
    long lit = 0, band47 = 0, band56 = 0, in_hide = 0, sleep = 0, sleep_ok = 0, shedding = 0, shed_in_humid = 0;
    long goal_frames[GOAL_COUNT] = { 0 };
    int zones[N_FISH_MAX] = { 0 }, stuck = 0, stuck_growth = 0, eaten0 = 0;
    bool zone_seen[N_FISH_MAX][6] = { { false } };
    stage_t stage0[N_FISH_MAX]; uint16_t clean0[N_FISH_MAX];
    for (int i = 0; i < n; i++) { stage0[i] = t->fish[i].stage; clean0[i] = t->fish[i].shed_clean; eaten0 += t->fish[i].eaten; }
    const int total = (int)((lit_s + dark_s + morning_s) / DT);
    for (int k = 0; k < total; k++) {
        const float at = k * DT;
        if (k == (int)(lit_s / DT)) lights(t, false);
        if (k == (int)((lit_s + dark_s) / DT)) lights(t, true);
        tank_tick(t, DT, advisor_rules); progression_tick(t, DT);
        const int phase = tank_activity_phase(t);
        for (int i = 0; i < n; i++) {
            const fish_t *f = &t->fish[i];
            if (on_floor(t, i, "free-running", at)) return 1;
            goal_frames[f->goal.id]++;
            int z = (int)(f->x / (TANK_W / 3.0f)) + 3; if (z > 5) z = 5;
            if (!zone_seen[i][z]) { zone_seen[i][z] = true; zones[i]++; }
            if (!t->night) { lit++; int h = (int)f->heat; band47 += h >= 4 && h <= 7; band56 += h >= 5 && h <= 6; in_hide += f->hide >= 0; }
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
    printf("selftest-terrarium: sheds: %d off clean, %d lizards grew (a growth shed each), shedding time in the humid hide %.0f%%, stuck %.0f lizard-s (%.0f of it a growth shed); the humid hide %.2f -> %.2f, never misted\n",
           clean, grew, shedding ? 100.0 * shed_in_humid / shedding : 0.0, stuck * DT, stuck_growth * DT, moist0, t->moist[humid]);
    printf("selftest-terrarium: goals");
    for (int g = GOAL_FIRST; g < GOAL_FIRST + GOAL_N; g++) printf(" %s %.0f%%", GOAL_NAMES[g], 100.0 * goal_frames[g] / ((double)total * n));
    printf(" | floor zones each:"); for (int i = 0; i < n; i++) printf(" %d", zones[i]);
    printf(" | pellets eaten %d\n", eaten);
    for (int i = 0; i < n; i++)
        printf("  %-5s %-5s %d clean shed%s, heat %.1f, %s, %s\n", t->fish[i].name, STAGE_NAMES[t->fish[i].stage], t->fish[i].shed_clean, t->fish[i].shed_clean == 1 ? "" : "s",
               t->fish[i].heat, GOAL_NAMES[t->fish[i].goal.id], t->fish[i].hide >= 0 ? HIDE_KIND_NAMES[tank_habitat(t)->hides[t->fish[i].hide].kind] : "out");
    if (100.0 * band47 / lit < 70) WF("lit, heat 4-7 only %.0f%% of the time (A13's bar: 70%%)", 100.0 * band47 / lit);
    if (grew < n || stuck_growth) WF("%d of %d lizards grew, %.0f lizard-s stuck in a growth shed: the growth sheds did not come off clean on their own", grew, n, stuck_growth * DT);
    if (sleep && 100.0 * sleep_ok / sleep < 90) WF("asleep, in a hide or lying still only %.0f%% of the time", 100.0 * sleep_ok / sleep);
    for (int i = 0; i < n; i++) if (zones[i] < 3) WF("%s saw %d of the floor's 3 zones in %d min", t->fish[i].name, zones[i], (lit_s + morning_s) / 60);
    printf("selftest-terrarium OK\n");
    return 0;
}
