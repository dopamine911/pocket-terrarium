#define _DEFAULT_SOURCE   /* setenv under -std=c11 (glibc) */
/* selftest_packs.c - pocket-terrarium P2a: the packs and the state the engine keeps for a lizard, through the
 * sim (tank_tick, progression_tick, progression_slept, the touch calls), never by reading a constant back:
 *
 *   ./fishsim --selftest-packs    every species and habitat of both worlds against its rules (docs/species-pack.md
 *                                 "Validation", habitat.h; a habitat's ground on all four boards, terrain.h THE RULES),
 *                                 the rules refusing what they must, every fitting pair of this build's world held and
 *                                 the other world's refused (P2b: another bin's), every pair a tank can hold fitting and
 *                                 this world's, every habitat's lit ground (the floor, its rocks and ramps) reaching each
 *                                 fitting species' band
 *   ./fishsim --selftest-heat     the heat field and the body's heat: a cold lizard warms into its band on the basking
 *                                 spot (up on its rock) at its tau's pace and overheats past it, the air warmer up the
 *                                 rocks while lit, the cool side cools it, cold slows it, the digits at every pack's
 *                                 thresholds, the test pack 6 C warmer, the body asleep
 *   ./fishsim --selftest-shed     docs/shed.md "Tests (P2)": growth sheds (at once, mid-cycle, gating the stage), a dry
 *                                 hide sticking, both cures, the moisture awake and asleep, a sleep across a cycle, and
 *                                 the gecko's own timeline
 *   ./fishsim --selftest-mister   the third tool: a pass once a stroke, the soak and the spray, the DONE chip
 *
 * Every expectation is read from the packs (species.h, habitat.h) and the engine's own constants' meaning, so a
 * retuned pack still has to behave as its numbers say. The tank is a fresh one on a scratch save. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tank.h"
#include "progression.h"
#include "render.h"
#include "advisor_core.h"
#include "sim_tmp.h"

static const char *s_name = "";
#define PF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static tank_t s_t, s_u;                      /* (tank_t is big: never on the stack) */
static bool  s_parked[N_FISH_MAX];
static float s_px[N_FISH_MAX], s_py[N_FISH_MAX];
static bool  s_fed = true;                   /* parked lizards kept at hunger 2 (a starving one begs: progression's famine) */

/* a fresh tank on a scratch save, progression booted (two hatchlings), the tank's own trickle off and no food */
static void fresh(tank_t *t, uint32_t seed) {
    const char *sav = sim_tmp("pocket-terrarium-selftest-packs.sav");
    setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    tank_init(t, seed);
    progression_wake(t, 0);
    t->trickle_off = true;
    tank_clear_food(t);                          /* (tank_init's two pellets, or the terrarium's two crickets: nobody eats in these tests) */
    for (int i = 0; i < N_FISH_MAX; i++) s_parked[i] = false;
    s_fed = true;
}
static void park(int i, float x, float y) { s_parked[i] = true; s_px[i] = x; s_py[i] = y; }
static void park_all(const tank_t *t) {      /* (after every move: the swim never takes a parked lizard anywhere) */
    tank_t *w = (tank_t *)t;
    for (int i = 0; i < w->n_fish; i++) if (s_parked[i]) {
        fish_t *f = &w->fish[i];
        f->x = s_px[i]; f->y = s_py[i]; f->speed = f->target_speed = 0;
        if (s_fed) f->hunger = 2;
    }
}
/* into hide k and parked there: in a hide is a state since P2b (fish_t.hide - THE WALK, tank_lizard_enter), no longer a
 * spot; its goal rest, so it stays in (these tests run no advisor: a goal is what they set) */
static void park_in(tank_t *t, int i, int k) {
    tank_lizard_enter(t, i, k); t->fish[i].goal.id = GOAL_LZ_REST;
    park(i, t->fish[i].x, t->fish[i].y);
}
/* n frames of dt awake: the reflex, the parked lizards put back, progression (the tended clock) */
static void frames(tank_t *t, int n, float dt) {
    for (int k = 0; k < n; k++) { tank_tick(t, dt, NULL); park_all(t); progression_tick(t, dt); }
}
static void live(tank_t *t, float seconds, float dt) { frames(t, (int)(seconds / dt + 0.5f), dt); }
/* a point on the open floor (no hide) at fraction u, searched toward the middle */
static float open_floor_x(const tank_t *t, float u) {
    float x = tank_floor_x(u), mid = tank_floor_x(0.5f);
    while (tank_hide_at(t, x, PLACE_Y) >= 0) x += x < mid ? 1.0f : -1.0f;
    return x;
}
static int hide_of_kind(const tank_t *t, hide_kind_t kind) {
    const habitat_t *h = tank_habitat(t);
    for (int k = 0; k < h->n_hides; k++) if (h->hides[k].kind == kind) return k;
    return -1;
}
static const char *SHED_WORD[SHED_STATES] = { "normal", "soon", "shedding", "stuck", "clean" };

/* ================================================================ --selftest-packs */
int selftest_packs(void) {
    s_name = "selftest-packs";
    char why[200];
    const int ns = species_count(), nh = habitat_count();
    if (ns < 2 || nh < 1) PF("%d species packs and %d habitats compiled in (the gecko and A16's test pack, the desert at least)", ns, nh);
    for (int i = 0; i < ns; i++) {
        const species_t *p = species_at(i);
        if (!species_check(p, why, sizeof why)) PF("species %s (id %d): %s", p->name, p->id, why);
        for (int j = 0; j < i; j++) if (species_at(j)->id == p->id) PF("species %s and %s share id %d", species_at(j)->name, p->name, p->id);
        if (species_get(p->id) != p) PF("species_get(%d) is not %s", p->id, p->name);
        char shed[64];
        if (p->sheds) snprintf(shed, sizeof shed, "sheds every %.0f..%.0f s", p->shed_every_s[0], p->shed_every_s[3]);
        else snprintf(shed, sizeof shed, "no sheds (the stage at its age)");
        printf("selftest-packs: species %3d %-15s ok: %s, band %.0f / %.0f / %.0f / %.0f C, stages at %.0f / %.0f / %.0f s, %s, %d morphs%s\n",
               p->id, p->name, p->world == WORLD_TERRARIUM ? "terrarium" : "aquarium", p->t_cold, p->t_lo, p->t_hi, p->t_hot,
               p->stage_s[0], p->stage_s[1], p->stage_s[2], shed, p->n_morphs, p->test_only ? " (tests only)" : "");
    }
    for (int i = 0; i < nh; i++) {
        const habitat_t *h = habitat_at(i);
        if (!habitat_check(h, why, sizeof why)) PF("habitat %s (id %d): %s", h->name, h->id, why);
        for (int j = 0; j < i; j++) if (habitat_at(j)->id == h->id) PF("habitats %s and %s share id %d", habitat_at(j)->name, h->name, h->id);
        if (habitat_get(h->id) != h) PF("habitat_get(%d) is not %s", h->id, h->name);
        printf("selftest-packs: habitat %3d %-15s ok: %s, floor %.0f -> %.0f C (dark -%.0f), basking spot %.0f C at %.2f, %d hides, humidity %.2f\n",
               h->id, h->name, h->world == WORLD_TERRARIUM ? "terrarium" : "aquarium", h->floor_warm_c, h->floor_cool_c, h->night_drop_c, h->bask_c, h->bask_u, h->n_hides, h->humidity);
    }
    /* the pairs: a habitat a species lists is of its world, and its lit ground (the floor, its rocks, its ramps) reaches the
     * species' band somewhere; a tank holds a fitting pair of this build's world and refuses the other world's (P2b: that
     * pair is another bin's - its ground is still checked here, staged straight into the tank as that bin holds it) */
    int fit_ours = 0;
    for (int i = 0; i < ns; i++) for (int j = 0; j < nh; j++) {
        const species_t *p = species_at(i); const habitat_t *h = habitat_at(j);
        if (((p->habitats >> h->id) & 1u) && p->world != h->world) PF("%s lists the %s, a habitat of another world", p->name, h->name);
        if (!habitat_fits(h, p)) continue;
        tank_init(&s_t, 1);
        const bool ours = p->world == PT_WORLD_ID;
        if (ours && !tank_set_packs(&s_t, h->id, p->id)) PF("a tank refused %s in the %s, a pair of its world (%s) that fits", p->name, h->name, PT_WORLD_NAME);
        if (!ours) {
            const uint8_t h0 = s_t.habitat_id, p0 = s_t.pack_id;
            if (tank_set_packs(&s_t, h->id, p->id) || s_t.habitat_id != h0 || s_t.pack_id != p0) PF("the %s's tank took %s in the %s, another world's pair", PT_WORLD_NAME, p->name, h->name);
            s_t.habitat_id = h->id; s_t.pack_id = p->id;
        }
        fit_ours += ours;
        float lo = 1e9f, hi = -1e9f, at = -1, at_y = 0;
        const terrain_t *tr = tank_terrain(&s_t);
        for (int s = 0; s < tr->n_paths; s++) {
            const terrain_path_t *q = &tr->path[s];
            for (float x = fmaxf(q->x[0], tank_glass_x0(q->y[0]) + 1); x < fminf(q->x[q->n - 1], tank_glass_x1(q->y[q->n - 1]) - 1); x += 0.5f) {
                const float y = terrain_y(q, x);
                if (tank_hide_at(&s_t, x, y) >= 0) continue;          /* the ground itself, not a hide */
                float c = tank_env_c_lit(&s_t, x, y, true);
                lo = fminf(lo, c); hi = fmaxf(hi, c);
                if (at < 0 && c >= p->t_lo && c <= p->t_hi) { at = x; at_y = y; }
            }
        }
        if (at < 0) PF("the %s's lit ground (%.1f..%.1f C) never reaches %s's band %.0f-%.0f C", h->name, lo, hi, p->name, p->t_lo, p->t_hi);
        printf("selftest-packs: %s in the %s: %s; the lit ground runs %.1f..%.1f C and is in its band (%.0f-%.0f C) first at (%.0f, %.0f)\n",
               p->name, h->name, ours ? "held" : "refused (another world's)", lo, hi, p->t_lo, p->t_hi, at, at_y);
    }
    /* every pair a tank can hold fits and is this world's - each fitting one of this world, no other; a fresh tank holds
     * the world's default pair; A16's test pack lives in the desert (the terrarium's) */
    tank_init(&s_t, 1);
    if (s_t.habitat_id != HABITAT_DEFAULT || s_t.pack_id != SPECIES_DEFAULT) PF("a new tank holds habitat %d / species %d, not the defaults", s_t.habitat_id, s_t.pack_id);
    int held = 0;
    for (int hid = 0; hid < 256; hid++) for (int pid = 0; pid < 256; pid++) {
        if (!tank_set_packs(&s_t, (uint8_t)hid, (uint8_t)pid)) {
            if (s_t.habitat_id != HABITAT_DEFAULT && held == 0) PF("a refused pair changed the tank");
            continue;
        }
        const habitat_t *h = habitat_get((uint8_t)hid); const species_t *p = species_get((uint8_t)pid);
        if (!h || !p || !habitat_fits(h, p) || h->world != p->world) PF("a tank took habitat %d with species %d, a pair that does not fit", hid, pid);
        if (p->world != PT_WORLD_ID) PF("the %s's tank took habitat %d with species %d, another world's pair", PT_WORLD_NAME, hid, pid);
        held++;
        tank_set_packs(&s_t, HABITAT_DEFAULT, SPECIES_DEFAULT);
    }
    if (held != fit_ours) PF("a tank holds %d pairs, but %d fitting pairs are the %s's", held, fit_ours, PT_WORLD_NAME);
    if (PT_TERRARIUM && !tank_set_packs(&s_t, HABITAT_DESERT, SPECIES_TEST_WARM)) PF("A16's test pack is refused the desert");
    uint8_t h0 = s_t.habitat_id, p0 = s_t.pack_id;
    if (tank_set_packs(&s_t, 99, SPECIES_DEFAULT) || tank_set_packs(&s_t, HABITAT_DEFAULT, 99) || s_t.habitat_id != h0 || s_t.pack_id != p0)
        PF("an unknown habitat or species was taken, or the refusal moved the tank");
    printf("selftest-packs: %d (habitat, species) pairs of 65536 can be held in the %s, every one fits and is its world's%s\n",
           held, PT_WORLD_NAME, PT_TERRARIUM ? "; the test pack lives in the desert" : "");
    /* the rules refuse what they must: one broken copy of the gecko (and the desert) each */
    species_t b; habitat_t hb; int refused = 0;
    #define REFUSE_S(stmt, what) do { b = *species_get(SPECIES_GECKO); stmt; if (species_check(&b, why, sizeof why)) PF("a gecko with %s passed its rules", what); refused++; } while (0)
    #define REFUSE_H(stmt, what) do { hb = *habitat_get(HABITAT_DESERT); stmt; if (habitat_check(&hb, why, sizeof why)) PF("a desert with %s passed its rules", what); refused++; } while (0)
    #define REFUSE_HW(stmt, what, word) do { REFUSE_H(stmt, what); if (!strstr(why, word)) PF("a desert with %s was refused for another reason: %s", what, why); } while (0)
    REFUSE_S(b.t_lo = b.t_hi + 1, "its band out of order");
    REFUSE_S(b.t_hi = b.t_lo + 1.5f, "a comfort band 1.5 C wide");
    REFUSE_S(b.tau_s[2] = 0, "no tau for an adult");
    REFUSE_S(b.hunger_per_s = 0, "no hunger");
    REFUSE_S(b.sleep_stress_per_h = -2, "a negative sleep rate");
    REFUSE_S(b.stage_s[1] = b.stage_s[0], "two stages at one age");
    REFUSE_S(b.shed_every_s[0] = b.shed_soon_s + b.shed_window_s - 1, "more than half its life shedding");
    REFUSE_S(b.pop_cap = 0, "no room for itself");
    REFUSE_S(b.pop_cap = POP_CAP + 1, "a cap past POP_CAP");
    REFUSE_S(b.n_morphs = 0, "no morph");
    REFUSE_S(b.goals |= 1u << LZ_CLIMB, "a climb (a goal past the schema's seven)");
    REFUSE_S(b.schedule = SCHED_DUSK, "a dusk schedule (no clock-based day yet)");
    REFUSE_S(b.habitats = 0, "nowhere to live");
    REFUSE_H(hb.floor_warm_c = hb.floor_cool_c - 1, "its warm end colder than its cool end");
    REFUSE_H(for (int k = 0; k < hb.n_hides; k++) hb.hides[k].kind = HIDE_WARM, "no humid hide");
    REFUSE_H(hb.hides[1].u = hb.hides[0].u + 0.05f, "two hides on one spot");
    REFUSE_H(hb.bask_c = hb.floor_warm_c - 5, "a lamp that does not warm its spot");
    REFUSE_H(hb.moist_start = 1.5f, "moisture past 1");
    /* ... and its ground (terrain.h THE RULES, on all four boards): each refused for its own reason */
    REFUSE_HW(hb.ramps[0].u_lo = 0.46f, "a ramp too steep to walk (a step)", "steeper");
    REFUSE_HW(hb.n_ramps = 2, "the high rock without a ramp", "out of reach");
    REFUSE_HW(hb.rocks[0].v[0] = hb.rocks[0].v[1] = 0.05f, "a ledge on the floor", "raised rock");
    REFUSE_HW(hb.rocks[2].v[0] = hb.rocks[2].v[1] = 0.25f, "a rock with no room under it", "apart");
    REFUSE_HW(hb.bask_surf = 1, "the basking spot under the high rock", "over the basking spot");
    REFUSE_HW(hb.hides[1].u = 0.6f, "a ramp low over the humid hide", "hides the hide");
    REFUSE_HW(hb.ramps[0].u_lo = 1.0f, "a ramp's foot at the glass", "could not reach");
    REFUSE_HW(hb.ramps[1].u_lo = 0.9f, "a ramp ending off its ledge", "off its surfaces");
    REFUSE_HW(hb.rise_c = -1, "air colder up toward the lamp", "cools toward the lamp");
    b = *species_get(SPECIES_GECKO); b.world = WORLD_AQUARIUM;
    if (habitat_fits(habitat_get(HABITAT_DESERT), &b)) PF("a fish of the aquarium fits the desert");
    printf("selftest-packs: the rules refuse %d broken packs (band, rates, stages, shed cycle, cap, morphs, a climber, a dusk schedule, no home; "
           "the desert reversed, without a humid hide, hides on one spot, a cold lamp, wet past 1; its ground with a step, a rock out of reach, "
           "a ledge on the floor, no room under a rock, the basking spot under a rock, a hide under a ramp, a ramp at the glass, a ramp off its "
           "ledge, the air cooler up), and a pair of two worlds\n", refused);
    #undef REFUSE_HW
    #undef REFUSE_S
    #undef REFUSE_H
    printf("selftest-packs OK\n");
    return 0;
}

/* ================================================================ --selftest-heat */
int selftest_heat(void) {
    s_name = "selftest-heat";
    tank_t *t = &s_t;
    tank_init(t, 11); tank_new_population(t); t->trickle_off = true;
    const species_t *sp = tank_species(t);
    const habitat_t *hb = tank_habitat(t);
    float bx, by; tank_bask_point(t, &bx, &by);
    const float bask = tank_env_c_lit(t, bx, by, true);
    int cool = hide_of_kind(t, HIDE_COOL);
    if (cool < 0) PF("the %s has no cool hide", hb->name);
    float cx, cy; tank_hide_point(t, cool, &cx, &cy);
    const float cool_c = tank_env_c_lit(t, cx, cy, true);
    /* the habitat is set for its species' thermoregulation: the spot hotter than the band, the cool hide under it */
    if (!(bask > sp->t_hi)) PF("the basking spot (%.1f C) is not above %s's t_hi (%.1f C)", bask, sp->name, sp->t_hi);
    if (!(cool_c < sp->t_lo)) PF("the cool hide (%.1f C) is not below %s's t_lo (%.1f C)", cool_c, sp->name, sp->t_lo);
    const float x0 = tank_floor_x(0), x1 = tank_floor_x(1), bu = fminf(fmaxf((bx - x0) / (x1 - x0), 0), 1);
    const float dark_floor = hb->floor_warm_c + (hb->floor_cool_c - hb->floor_warm_c) * bu - hb->night_drop_c;   /* the floor's own under it, dark */
    if (fabsf(bask - hb->bask_c) > 0.01f || fabsf(tank_env_c_lit(t, bx, by, false) - dark_floor) > 0.01f)
        PF("the basking spot reads %.2f C lit (its habitat says %.1f) and %.2f C dark (the dark floor's %.2f: the lamp goes out with the light)", bask, hb->bask_c, tank_env_c_lit(t, bx, by, false), dark_floor);
    printf("selftest-heat: the %s for the %s: the basking spot %.1f C up on its rock (%.0f px over the floor; over t_hi %.0f), the cool hide %.1f C (under t_lo %.0f); dark, the spot %.1f C\n",
           hb->name, sp->name, bask, PLACE_Y - by, sp->t_hi, cool_c, sp->t_lo, tank_env_c_lit(t, bx, by, false));
    /* the air rises toward the lamp while lit: along each rock, past the lamp's pool, rise_c x its height's share warmer than
       the floor under it - and no warmer dark */
    {
        const terrain_t *tr = tank_terrain(t);
        int probes = 0; float most = 0;
        for (int s = 1; s <= tr->n_rocks; s++) {
            const terrain_path_t *q = &tr->path[s];
            for (float x = q->x[0]; x <= q->x[q->n - 1]; x += 4) {
                const float y = terrain_y(q, x);
                if (tank_dist(x, y, bx, by) <= hb->bask_r_px || tank_hide_at(t, x, PLACE_Y) >= 0) continue;
                const float up = tank_env_c_lit(t, x, y, true) - tank_env_c_lit(t, x, PLACE_Y, true), want = hb->rise_c * (PLACE_Y - y) / PLACE_Y;
                const float up_dark = tank_env_c_lit(t, x, y, false) - tank_env_c_lit(t, x, PLACE_Y, false);
                if (fabsf(up - want) > 0.01f || fabsf(up_dark) > 0.01f)
                    PF("rock %d at (%.0f, %.0f): %.2f C over the floor under it lit (want %.2f: %.1f C x its height) and %.2f dark (want 0)", s, x, y, up, want, hb->rise_c, up_dark);
                probes++; most = fmaxf(most, up);
            }
        }
        if (!probes) PF("no rock clear of the lamp's pool to probe the air's rise");
        printf("selftest-heat: the air rises toward the lamp: up on the rocks, past its pool, up to %.1f C over the floor under them lit (%d probes); dark, none\n", most, probes);
    }
    /* 1. a cold lizard parked on the basking spot warms into its band, at its tau's pace, and on past it */
    fish_t *f = &t->fish[0];
    for (int i = 0; i < t->n_fish; i++) s_parked[i] = false;
    park(0, bx, by); park_all(t);
    f->body_c = sp->t_cold; f->stress = 0;
    const float tau = sp->tau_s[f->stage & 3], dt = 1.0f / 30.0f;
    const float want = tau * logf((bask - sp->t_cold) / (bask - sp->t_lo));
    float reached = -1; int digit = -1;
    for (int n = 1; n <= (int)(tau * 8 / dt); n++) {
        tank_tick(t, dt, NULL); park_all(t);
        if (reached < 0 && f->body_c >= sp->t_lo) { reached = n * dt; digit = (int)f->heat; }
    }
    if (reached < 0) PF("parked on the basking spot from %.0f C, it never reached its band (%.1f C after %.0f s)", sp->t_cold, f->body_c, tau * 8);
    if (fabsf(reached - want) > 0.02f * want + 2 * dt) PF("it reached t_lo at %.1f s, its tau (%.0f s) says %.1f s", reached, tau, want);
    if (digit != 5) PF("at t_lo its heat digit is %d, not 5", digit);
    if (!(f->body_c > sp->t_hi && f->heat >= HEAT_HOT)) PF("left on the spot it did not overheat (%.1f C, heat %.1f)", f->body_c, f->heat);
    if (!(f->stress > 1.5f)) PF("overheating (heat %.1f) added no stress (%.2f)", f->heat, f->stress);
    printf("selftest-heat: a cold %s on the basking spot: in its band at %.1f s (its tau says %.1f), then %.1f C, heat %.1f, stress %.1f\n",
           sp->name, reached, want, f->body_c, f->heat, f->stress);
    /* 2. the cool side cools it */
    float xc = open_floor_x(t, 0.9f);
    park(0, xc, PLACE_Y); park_all(t);
    const float env_c = tank_env_c(t, xc, PLACE_Y);
    float prev = f->body_c, start = f->body_c;
    for (int s = 0; s < (int)(tau * 6); s++) {
        for (int n = 0; n < 30; n++) { tank_tick(t, dt, NULL); park_all(t); }
        if (f->body_c > prev + 1e-4f) PF("on the cool side it warmed (%.2f -> %.2f C)", prev, f->body_c);
        prev = f->body_c;
    }
    if (!(f->body_c < sp->t_lo && fabsf(f->body_c - env_c) < 0.05f)) PF("on the cool side it settled at %.2f C, the floor there is %.2f C", f->body_c, env_c);
    printf("selftest-heat: on the cool side (x %.0f, %.1f C) it cooled %.1f -> %.1f C, heat %.1f\n", xc, env_c, start, f->body_c, f->heat);
    /* 3. cold slows it: the same lizard in the same moment, cold and comfortable (copies of one tank) - a moment it
       walks (THE WALK stops and goes, and turns about standing: a stop's pace is 0 whatever the heat) */
    tank_init(&s_u, 12); tank_new_population(&s_u); s_u.trickle_off = true;
    for (int n = 0; n < 60; n++) tank_tick(&s_u, dt, NULL);               /* under way */
    const float probe[3] = { sp->t_cold - 3, sp->t_cold + (sp->t_lo - sp->t_cold) * 0.3f, (sp->t_lo + sp->t_hi) * 0.5f };   /* heat 0, 1.5, 6 */
    float speed[3] = { 0, 0, 0 };
    int waited = 0;
    for (; waited < (int)(120 / dt); waited++) {                          /* the next frame it walks, comfortable */
        memcpy(t, &s_u, sizeof *t);
        t->fish[0].body_c = probe[2]; t->fish[0].heat = species_heat_of(sp, probe[2]);
        tank_tick(t, dt, NULL);
        if (t->fish[0].pose == LZ_POSE_WALK && t->fish[0].target_speed > 1) break;
        tank_tick(&s_u, dt, NULL);
    }
    if (waited >= (int)(120 / dt)) PF("its pace cold: in 120 s under way it never walked (pose %s)", LZ_POSE_NAMES[t->fish[0].pose]);
    for (int k = 0; k < 3; k++) {
        memcpy(t, &s_u, sizeof *t);
        t->fish[0].body_c = probe[k]; t->fish[0].heat = species_heat_of(sp, probe[k]);
        tank_tick(t, dt, NULL);
        speed[k] = t->fish[0].target_speed;
    }
    float h1 = species_heat_of(sp, probe[1]);
    if (!(speed[2] > 1) || fabsf(speed[0] / speed[2] - HEAT_SLOW_MIN) > 0.005f || fabsf(speed[1] / speed[2] - (HEAT_SLOW_MIN + (1 - HEAT_SLOW_MIN) * h1 / HEAT_SLOW)) > 0.01f)
        PF("its pace cold: %.2f / %.2f / %.2f px/s at heat 0 / %.1f / 6 (want x%.2f at 0, rising to x1 at heat %.0f)", speed[0], speed[1], speed[2], h1, HEAT_SLOW_MIN, HEAT_SLOW);
    printf("selftest-heat: its pace: %.1f px/s comfortable, x%.2f at heat %.1f, x%.2f at heat 0\n", speed[2], speed[1] / speed[2], h1, speed[0] / speed[2]);
    /* 4. the digits at every pack's thresholds, through the map and the L1 line */
    for (int i = 0; i < species_count(); i++) {
        const species_t *p = species_at(i);
        const float at[4] = { p->t_cold, p->t_lo, p->t_hi, p->t_hot }, map[4] = { 0, 5, 7, 10 };
        static const char *const word[4] = { " heat 0 ", " heat 5 ", " heat 7 ", " heat 9 " };
        for (int k = 0; k < 4; k++) {
            if (species_heat_of(p, at[k]) != map[k]) PF("%s at %.1f C reads heat %.4f, want %.0f", p->name, at[k], species_heat_of(p, at[k]), map[k]);
            l1_snap_t s; memset(&s, 0, sizeof s);
            s.pack = p->id; s.zone = 1; s.body_c = at[k]; s.last = LZ_REST; s.phase = PHASE_ACTIVE;
            char line[400]; advisor_core_encode_l1(&s, line, sizeof line);
            if (!strstr(line, word[k])) PF("%s at %.1f C: the L1 line has no `%s`: %s", p->name, at[k], word[k], line);
        }
    }
    printf("selftest-heat: every pack reads heat 0 / 5 / 7 / 10 at t_cold / t_lo / t_hi / t_hot - digits 0 / 5 / 7 / 9 on the L1 line\n");
    /* 5. the test pack's band sits 6 C higher: the same body 6 C warmer reads the gecko's heat */
    const species_t *g = species_get(SPECIES_GECKO), *w = species_get(SPECIES_TEST_WARM);
    if (w->t_cold - g->t_cold != 6 || w->t_lo - g->t_lo != 6 || w->t_hi - g->t_hi != 6 || w->t_hot - g->t_hot != 6)
        PF("the test pack is not the gecko 6 C warmer (%.0f/%.0f/%.0f/%.0f vs %.0f/%.0f/%.0f/%.0f)", w->t_cold, w->t_lo, w->t_hi, w->t_hot, g->t_cold, g->t_lo, g->t_hi, g->t_hot);
    for (float c = g->t_cold - 4; c <= g->t_hot + 4; c += 0.05f)
        if (fabsf(species_heat_of(w, c + 6) - species_heat_of(g, c)) > 1e-4f) PF("at %.2f C the gecko reads %.3f, the test pack at %.2f C %.3f", c, species_heat_of(g, c), c + 6, species_heat_of(w, c + 6));
    /* ... and on the basking spot that overheats the gecko, a test-pack lizard settles where its own band says */
    tank_init(&s_u, 13); tank_new_population(&s_u);
    if (!tank_set_packs(&s_u, HABITAT_DESERT, SPECIES_TEST_WARM)) PF("the desert refused the test pack");
    s_u.fish[0].body_c = w->t_cold;
    for (int n = 0; n < (int)(w->tau_s[s_u.fish[0].stage & 3] * 10 / dt); n++) { tank_tick(&s_u, dt, NULL); s_u.fish[0].x = bx; s_u.fish[0].y = by; }
    float hw = s_u.fish[0].heat, hg = species_heat_of(g, bask);
    if (fabsf(hw - species_heat_of(w, bask)) > 0.01f || !(hw < hg)) PF("on the spot (%.1f C) the test pack reads heat %.2f, the gecko %.2f", bask, hw, hg);
    if (bask >= w->t_lo && bask <= w->t_hi && !(hw >= 5 && hw < 7)) PF("the spot is in the test pack's band but it reads heat %.2f", hw);
    printf("selftest-heat: the test pack's band is the gecko's +6 C: the same reading 6 C warmer; on the spot (%.1f C) it reads heat %.1f where a gecko reads %.1f\n", bask, hw, hg);
    /* 6. asleep: the body follows the dark terrarium where it lies */
    float xw = open_floor_x(t, 0.12f);
    f->x = xw; f->y = PLACE_Y;
    const float b0 = f->body_c, env_d = tank_env_c_lit(t, xw, PLACE_Y, false);
    tank_tick_sleep(t, 3600);
    const float want_d = env_d + (b0 - env_d) * expf(-3600.0f / sp->tau_s[f->stage & 3]);
    if (fabsf(f->body_c - want_d) > 0.01f || fabsf(f->heat - species_heat_of(sp, f->body_c)) > 1e-4f) PF("an hour asleep at %.1f C (dark) took it %.2f -> %.2f C, want %.2f", env_d, b0, f->body_c, want_d);
    printf("selftest-heat: an hour asleep on the dark floor (%.1f C): %.1f -> %.1f C, heat %.1f\n", env_d, b0, f->body_c, f->heat);
    printf("selftest-heat OK\n");
    return 0;
}

/* ================================================================ --selftest-shed */
/* the first frame-time at which lizard i's shed is `state` (or its stage `stage`), living dt frames up to `limit` s; -1 = never */
static float until_state(tank_t *t, int i, int state, float limit, float dt) {
    for (float s = 0; s < limit; s += dt) { frames(t, 1, dt); if (t->fish[i].shed == state) return s + dt; }
    return -1;
}
int selftest_shed(void) {
    s_name = "selftest-shed";
    tank_t *t = &s_t;
    const float dt = 0.1f;
    /* ---- 1. the gecko's own timeline, awake (docs/shed.md): a hatchling's first timed shed is under way when the
       juvenile age arrives - it becomes the growth shed - and in a damp humid hide it comes off clean SHED_SOAK_S into
       shedding: a juvenile then. One that never goes in sticks and stays a hatchling */
    fresh(t, 101);
    const species_t *sp = tank_species(t);
    const int humid = tank_humid_hide(t), warm = hide_of_kind(t, HIDE_WARM);
    if (humid < 0 || warm < 0) PF("the habitat has no humid or no warm hide");
    float hx, hy, wx, wy; tank_hide_point(t, humid, &hx, &hy); tank_hide_point(t, warm, &wx, &wy);
    park_in(t, 0, humid); park_in(t, 1, warm);
    float t_soon = -1, t_shed = -1, t_growth = -1, t_juv = -1; int soons = 0;
    for (float s = 0; s < sp->shed_every_s[0] + sp->shed_soon_s + SHED_SOAK_S + 30; s += dt) {
        int was = t->fish[0].shed;
        frames(t, 1, dt);
        const fish_t *f = &t->fish[0];
        if (f->shed == SHED_SOON && was != SHED_SOON) { soons++; if (t_soon < 0) t_soon = s + dt; }
        if (f->shed == SHED_SHEDDING && t_shed < 0) t_shed = s + dt;
        if (f->shed_growth && t_growth < 0) t_growth = s + dt;
        if (f->stage == STAGE_JUV && t_juv < 0) t_juv = s + dt;
    }
    const float w_soon = sp->shed_every_s[0], w_shed = w_soon + sp->shed_soon_s, w_juv = fmaxf(w_shed, sp->stage_s[0]) + SHED_SOAK_S;
    if (fabsf(t_soon - w_soon) > 0.2f || fabsf(t_shed - w_shed) > 0.2f) PF("the hatchling's timed shed: soon at %.1f s, shedding at %.1f s (its pack says %.0f / %.0f)", t_soon, t_shed, w_soon, w_shed);
    if (fabsf(t_growth - sp->stage_s[0]) > 0.2f || soons != 1) PF("the juvenile age (%.0f s) made it a growth shed at %.1f s, %d pre-sheds (want the one under way)", sp->stage_s[0], t_growth, soons);
    if (fabsf(t_juv - w_juv) > 0.2f) PF("in the damp humid hide it became a juvenile at %.1f s, want %.0f s (shedding %.0f + %.0f s damp)", t_juv, w_juv, w_shed, SHED_SOAK_S);
    if (t->fish[0].shed != SHED_NORMAL || t->fish[0].shed_clean != 1 || fabsf(t->fish[0].shed_s - (sp->shed_every_s[1] - ((float)t->clock - t_juv))) > 0.5f)
        PF("off clean: %s, %d clean, the timer at %.0f s (want normal, 1, the juvenile's %.0f s restarted)", SHED_WORD[t->fish[0].shed], t->fish[0].shed_clean, t->fish[0].shed_s, sp->shed_every_s[1]);
    if (!(t->fish[0].ms_bits & MS_REACHED_JUV)) PF("a juvenile without its badge");
    if (t->fish[1].stage != STAGE_FRY || !t->fish[1].shed_growth || t->fish[1].shed != SHED_SHEDDING)
        PF("the hatchling that never went in: stage %d, %s, growth %d (want a hatchling shedding its growth shed)", t->fish[1].stage, SHED_WORD[t->fish[1].shed], t->fish[1].shed_growth);
    printf("selftest-shed: the gecko's timeline, awake: soon at %.0f s, shedding at %.0f s, the juvenile age at %.0f s took the shed under way, a juvenile at %.0f s (%.1f min) in the damp humid hide; the other, dry, still a hatchling\n",
           t_soon, t_shed, t_growth, t_juv, t_juv / 60);
    float stuck_at = until_state(t, 1, SHED_STUCK, sp->shed_window_s, dt);
    if (stuck_at < 0 || t->fish[1].stage != STAGE_FRY) PF("out of the humid hide its shed never stuck, or it grew anyway (stage %d)", t->fish[1].stage);

    /* ---- 2. the stage's age starts a growth shed AT ONCE, the shed timer far off, and the stage changes when it comes
       off clean, never at the age */
    fresh(t, 102);
    fish_t *f = &t->fish[0];
    progression_set_age(t, 0, sp->stage_s[0] - 10);
    f->shed = SHED_NORMAL; f->shed_s = 1e5f;
    park_in(t, 0, warm); park(1, wx, wy - 40);
    live(t, 9.5f, dt);
    if (f->shed != SHED_NORMAL || f->stage != STAGE_FRY) PF("10 s short of the juvenile age: %s, stage %d", SHED_WORD[f->shed], f->stage);
    live(t, 1.0f, dt);
    double age = progression_age_s(t, 0);
    if (f->shed != SHED_SOON || !f->shed_growth || fabsf(f->shed_s - (sp->shed_soon_s - (float)(age - sp->stage_s[0]))) > dt + 0.01f || f->stage != STAGE_FRY)
        PF("0.5 s past the juvenile age (timer far off): %s, growth %d, %.1f s left, stage %d (want a growth shed's pre-shed from the age's moment, still a hatchling)",
           SHED_WORD[f->shed], f->shed_growth, f->shed_s, f->stage);
    if (until_state(t, 0, SHED_SHEDDING, sp->shed_soon_s + 1, dt) < 0 || f->stage != STAGE_FRY) PF("its pre-shed never turned into shedding, or it grew at the age");
    park_in(t, 0, humid);
    float clean_at = -1;
    for (float s = 0; s < SHED_SOAK_S + 5 && clean_at < 0; s += dt) { frames(t, 1, dt); if (f->stage == STAGE_JUV) clean_at = s + dt; }
    age = progression_age_s(t, 0);
    if (fabsf(clean_at - SHED_SOAK_S) > 0.2f || f->shed != SHED_NORMAL || age < sp->stage_s[0] + sp->shed_soon_s + SHED_SOAK_S - 1)
        PF("the growth shed in the damp hide: a juvenile after %.1f s damp (want %.0f), %s, at age %.0f s", clean_at, SHED_SOAK_S, SHED_WORD[f->shed], age);
    printf("selftest-shed: the juvenile age with the shed timer far off: a growth shed at once, still a hatchling; a juvenile at age %.0f s (the age was %.0f s), when it came off clean\n",
           age, sp->stage_s[0]);

    /* ---- 3. the age reached mid-cycle: the shed under way is the growth shed, no second one; out of the hide it sticks,
       the stage held; a damp stay clears it, and it grows then */
    fresh(t, 103);
    progression_set_age(t, 0, sp->stage_s[0] - 100);
    f->shed = SHED_SHEDDING; f->shed_s = 400; f->shed_damp = 0;
    park_in(t, 0, warm); park(1, wx, wy - 40);
    int saw_soon = 0;
    for (float s = 0; s < 101; s += dt) { frames(t, 1, dt); saw_soon += f->shed == SHED_SOON; }
    if (f->shed != SHED_SHEDDING || !f->shed_growth || saw_soon || f->stage != STAGE_FRY) PF("the age mid-shed: %s, growth %d, %d frames of a second pre-shed, stage %d", SHED_WORD[f->shed], f->shed_growth, saw_soon, f->stage);
    for (float s = 0; s < 400 && f->shed != SHED_STUCK; s += dt) { frames(t, 1, dt); saw_soon += f->shed == SHED_SOON; }
    live(t, 120, dt);
    if (f->shed != SHED_STUCK || f->stage != STAGE_FRY || saw_soon) PF("its window out of the hide: %s, stage %d (want stuck, the stage held)", SHED_WORD[f->shed], f->stage);
    tank_mist_pass(t, humid); tank_mist_pass(t, humid);
    park_in(t, 0, humid);
    float cleared = -1;
    for (float s = 0; s < SHED_SOAK_S + 5 && cleared < 0; s += dt) { frames(t, 1, dt); if (f->shed != SHED_STUCK) cleared = s + dt; }
    if (fabsf(cleared - SHED_SOAK_S) > 0.2f || f->shed != SHED_NORMAL || f->stage != STAGE_JUV || saw_soon) PF("a damp stay on the stuck growth shed: cleared after %.1f s (want %.0f), %s, stage %d", cleared, SHED_SOAK_S, SHED_WORD[f->shed], f->stage);
    printf("selftest-shed: the age mid-shed: that shed is the growth shed, no second pre-shed; stuck out of the hide, the stage held; a damp stay cleared it in %.0f s and it grew\n", cleared);

    /* ---- 4. a dry hide: the growth shed sticks, stress rises toward 6 and no further by itself, the stage holds; the
       mister's soak clears it */
    fresh(t, 104);
    t->moist[humid] = 0;
    progression_set_age(t, 0, sp->stage_s[0] - 1);
    f->shed = SHED_NORMAL; f->shed_s = 1e5f; f->stress = 0; f->trust = 7;
    park_in(t, 0, humid); park_in(t, 1, warm);
    if (until_state(t, 0, SHED_STUCK, 2 + sp->shed_soon_s + sp->shed_window_s, dt) < 0) PF("in the dry humid hide the growth shed never stuck (%s)", SHED_WORD[f->shed]);
    if (f->stage != STAGE_FRY) PF("stuck in a dry hide it grew anyway");
    const float half = STUCK_STRESS_MAX / STUCK_STRESS_PER_S / 2;
    live(t, half, dt);
    float s_half = f->stress;
    live(t, half + 100, dt);
    float s_full = f->stress;
    live(t, 300, dt);
    float s_late = f->stress;
    if (fabsf(s_half - STUCK_STRESS_MAX / 2) > 0.15f || fabsf(s_full - STUCK_STRESS_MAX) > 0.05f || s_late > STUCK_STRESS_MAX + 0.05f)
        PF("stuck: stress %.2f after %.0f s, %.2f after %.0f s, %.2f later (want half of %.0f, then %.0f and no further)", s_half, half, s_full, 2 * half + 100, s_late, STUCK_STRESS_MAX, STUCK_STRESS_MAX);
    if (f->stage != STAGE_FRY || f->shed != SHED_STUCK) PF("stuck %.0f s: stage %d, %s (the stage must hold until it is cleared)", 2 * half + 400, f->stage, SHED_WORD[f->shed]);
    printf("selftest-shed: a dry humid hide: the growth shed stuck, stress %.1f after %.0f s, %.1f after %.0f s and %.1f later; age %.0f s and still a hatchling\n",
           s_half, half, s_full, 2 * half + 100, s_late, progression_age_s(t, 0));
    tank_set_tool(t, TOOL_MISTER);
    float jump = 0, before = f->stress;
    for (float s = 0; s < SOAK_HOLD_S + 0.5f; s += dt) {
        float was = f->stress;
        tank_touch_hold(t, hx, hy); frames(t, 1, dt);
        if (f->stress - was > jump) jump = f->stress - was;
        if (s < SOAK_HOLD_S - 0.4f && f->shed != SHED_STUCK) PF("the soak cleared it after %.1f s, before its %.0f s", s, SOAK_HOLD_S);
    }
    if (f->shed != SHED_NORMAL || f->stage != STAGE_JUV || fabsf(f->trust - 7.2f) > 0.01f || jump < 0.95f)
        PF("the soak: %s, stage %d, trust %.2f (want 7.2), its stress jumped %.2f (want +1) from %.2f", SHED_WORD[f->shed], f->stage, f->trust, jump, before);
    tank_set_tool(t, TOOL_HAND);
    printf("selftest-shed: the mister's soak (%.0f s held) cleared the stuck growth shed: a juvenile, stress +%.1f, trust 7.0 -> %.1f\n", SOAK_HOLD_S, jump, f->trust);

    /* ---- 5. the humid hide's moisture: dry in its habitat's time awake, four times as long asleep */
    fresh(t, 105);
    const habitat_t *hb = tank_habitat(t);
    t->moist[humid] = 1;
    float half_m = -1, dry_awake = -1;
    for (int s = 1; s <= (int)(2 / hb->moist_dry_per_s); s++) {
        progression_tick(t, 1.0f);                                    /* (the tended clock awake, a second a call) */
        if (s == (int)(0.5f / hb->moist_dry_per_s)) half_m = t->moist[humid];
        if (t->moist[humid] <= 0) { dry_awake = (float)s; break; }
    }
    if (dry_awake < 0 || fabsf(dry_awake - 1 / hb->moist_dry_per_s) > 10 || fabsf(half_m - 0.5f) > 0.002f)
        PF("awake the full hide dried in %.0f s (half at %.3f), its habitat says %.0f s", dry_awake, half_m, 1 / hb->moist_dry_per_s);
    t->moist[humid] = 1;
    float dry_asleep = -1;
    for (int s = 1; s <= (int)(2 / (hb->moist_dry_per_s * SLEEP_GROWTH_FRAC * 900)); s++) {   /* quarter hours asleep, up to twice the time it should take */
        progression_slept(t, 900);
        if (t->moist[humid] <= 0) { dry_asleep = s * 900.0f; break; }
    }
    if (dry_asleep < 0 || fabsf(dry_asleep / dry_awake - 1 / SLEEP_GROWTH_FRAC) > 900 / dry_awake + 0.01f)
        PF("asleep the full hide dried in %.0f s, %.2f x awake (want %.0f x)", dry_asleep, dry_asleep / dry_awake, 1 / SLEEP_GROWTH_FRAC);
    printf("selftest-shed: the humid hide dries full -> 0 in %.1f h awake and %.1f h asleep (%.2f x)\n", dry_awake / 3600, dry_asleep / 3600, dry_asleep / dry_awake);

    /* ---- 6. a sleep across a whole shed cycle: the timers at the sleep rate, the states right on waking. A juvenile in the
       damp humid hide comes off clean in the night; one out of it wakes stuck, its stress floor there; a hatchling whose
       juvenile age falls in the night wakes in that growth shed's pre-shed */
    fresh(t, 106);
    while (t->n_fish < 3) tank_add_fish(t, 0, 1);
    t->moist[humid] = 1;
    for (int i = 0; i < 2; i++) { progression_set_age(t, i, sp->stage_s[0] + 10); t->fish[i].shed = SHED_NORMAL; t->fish[i].shed_s = 100; t->fish[i].shed_clean = 0; t->fish[i].stress = 0; }
    const float night_tended = 100 + sp->shed_soon_s + sp->shed_window_s + 200;   /* the juveniles: 100 s to the pre-shed, the cycle, 200 s past its window */
    const float c_in = night_tended - 150;                                       /* the hatchling reaches the juvenile age 150 s before the morning */
    progression_set_age(t, 2, sp->stage_s[0] - c_in); t->fish[2].shed = SHED_NORMAL; t->fish[2].shed_s = 1e5f;
    tank_lizard_enter(t, 0, humid); tank_lizard_enter(t, 1, warm); tank_lizard_enter(t, 2, hide_of_kind(t, HIDE_COOL));   /* (in them: asleep, nobody walks) */
    const float slept = night_tended / SLEEP_GROWTH_FRAC;
    progression_slept(t, slept);
    const fish_t *a = &t->fish[0], *b = &t->fish[1], *c = &t->fish[2];
    const float a_clean = 100 + sp->shed_soon_s + SHED_SOAK_S;      /* tended s into the night it came off */
    const float a_left = sp->shed_every_s[STAGE_JUV] - (night_tended - a_clean);
    if (a->shed != SHED_NORMAL || a->shed_clean != 1 || a->shed_s < a_left - 1 || a->shed_s > a_left + 300 * SLEEP_GROWTH_FRAC + 1)   /* (the clean's slice: its rest waits) */
        PF("the juvenile asleep in the damp hide wakes %s, %d clean, its timer at %.0f s (want normal, 1, about %.0f)", SHED_WORD[a->shed], a->shed_clean, a->shed_s, a_left);
    if (b->shed != SHED_STUCK || fabsf(b->shed_s - 200) > 0.5f) PF("the juvenile asleep out of it wakes %s, %.0f s stuck (want stuck, 200 s)", SHED_WORD[b->shed], b->shed_s);
    if (c->shed != SHED_SOON || !c->shed_growth || c->stage != STAGE_FRY || fabsf(c->shed_s - (sp->shed_soon_s - (night_tended - c_in))) > 0.5f)
        PF("the hatchling whose age fell in the night wakes %s, growth %d, stage %d, %.0f s left (want its growth shed's pre-shed, %.0f s left)",
           SHED_WORD[c->shed], c->shed_growth, c->stage, c->shed_s, sp->shed_soon_s - (night_tended - c_in));
    if (fabsf(t->moist[humid] - (1 - hb->moist_dry_per_s * night_tended)) > 0.001f) PF("the hide dried to %.4f in the night, want %.4f", t->moist[humid], 1 - hb->moist_dry_per_s * night_tended);
    for (int i = 0; i < 3; i++) park(i, t->fish[i].x, t->fish[i].y);
    frames(t, 1, 1.0f / 30);
    if (b->stress < STUCK_STRESS_PER_S * 200 - 0.01f) PF("woken stuck %.0f s, its stress is %.2f (the floor is %.2f)", b->shed_s, b->stress, STUCK_STRESS_PER_S * 200);
    printf("selftest-shed: %.1f h asleep (%.0f tended s): the one in the damp hide woke clean (%.0f s to its next), the dry one stuck %.0f s (stress %.1f), "
           "the hatchling in its growth shed's pre-shed (%.0f s left); the hide at %.3f\n", slept / 3600, night_tended, a->shed_s, b->shed_s, b->stress, c->shed_s, t->moist[humid]);

    /* ---- 7. off its food before a shed: hunger at SHED_HUNGER_X while soon or shedding, awake */
    fresh(t, 107);
    s_fed = false;
    park_in(t, 0, warm); park(1, wx, wy - 40);
    f = &t->fish[0]; f->shed = SHED_NORMAL; f->shed_s = 1e5f; f->hunger = 2;
    live(t, 20, dt);
    float dh_normal = f->hunger - 2;
    f->shed = SHED_SHEDDING; f->shed_s = 1e4f; f->hunger = 2;
    live(t, 20, dt);
    float dh_shed = f->hunger - 2;
    if (!(dh_normal > 0) || fabsf(dh_shed / dh_normal - SHED_HUNGER_X) > 0.02f) PF("hunger rose %.3f normal and %.3f shedding in 20 s (want x%.1f)", dh_normal, dh_shed, SHED_HUNGER_X);
    printf("selftest-shed: shedding it goes off its food: hunger +%.3f in 20 s against +%.3f (x%.2f)\n", dh_shed, dh_normal, dh_shed / dh_normal);
    printf("selftest-shed OK\n");
    return 0;
}

/* ================================================================ --selftest-mister */
static void stroke(tank_t *t, float x0, float y0, float x1, float y1, int n) {   /* a finger drawn from one point to the other, then lifted */
    for (int k = 0; k <= n; k++) { float u = (float)k / n; tank_touch_drag(t, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u); tank_tick(t, 1.0f / 60, NULL); park_all(t); }
    for (int k = 0; k < 2; k++) { tank_tick(t, 1.0f / 60, NULL); park_all(t); }
}
/* a finger held at (x, y) for `seconds` (the platforms report a hold every frame, as tank.h asks); the biggest one-frame stress jump of lizard i */
static float hold(tank_t *t, float x, float y, float seconds, int i) {
    float jump = 0;
    for (float s = 0; s < seconds; s += 1.0f / 30) {
        float was = t->fish[i].stress;
        tank_touch_hold(t, x, y); tank_tick(t, 1.0f / 30, NULL); park_all(t); progression_tick(t, 1.0f / 30);
        if (t->fish[i].stress - was > jump) jump = t->fish[i].stress - was;
    }
    for (int k = 0; k < 2; k++) { tank_tick(t, 1.0f / 30, NULL); park_all(t); progression_tick(t, 1.0f / 30); }   /* the finger lifts */
    return jump;
}
int selftest_mister(void) {
    s_name = "selftest-mister";
    tank_t *t = &s_t;
    fresh(t, 201);
    const int humid = tank_humid_hide(t), warm = hide_of_kind(t, HIDE_WARM);
    float hx, hy, wx, wy; tank_hide_point(t, humid, &hx, &hy); tank_hide_point(t, warm, &wx, &wy);
    park(0, wx, wy); park(1, wx + 60, wy);
    /* the toolbox: the mister is the third third; picked up, it is in hand */
    const int by = RENDER_TOOLS_Y + RENDER_TOOLS_H / 2;
    if (render_tools_hit(RENDER_TOOLS_X + RENDER_TOOLS_W * 5 / 6, by) != TOOL_MISTER || render_tools_hit(RENDER_TOOLS_X + RENDER_TOOLS_W / 6, by) != TOOL_SPONGE ||
        render_tools_hit(RENDER_TOOLS_X + RENDER_TOOLS_W / 2, by) != TOOL_SCISSORS) PF("the toolbox's thirds are not sponge / scissors / mister");
    tank_set_tool(t, TOOL_MISTER);
    if (t->tool != TOOL_MISTER) PF("the mister did not go in hand");
    /* a stroke over the humid hide: one pass, however long it lingers or however often it crosses; the next stroke another */
    float m = 0.1f; t->moist[humid] = m;
    stroke(t, hx - 70, hy - 8, hx + 70, hy + 8, 40);
    if (fabsf(t->moist[humid] - (m + MIST_PASS)) > 1e-5f) PF("one stroke over the humid hide: moisture %.3f, want %.3f", t->moist[humid], m + MIST_PASS);
    m = t->moist[humid];
    for (int k = 0; k <= 120; k++) { float u = sinf(k * 0.2f); tank_touch_drag(t, hx + u * 60, hy); tank_tick(t, 1.0f / 60, NULL); park_all(t); }   /* back and forth, one stroke */
    for (int k = 0; k < 2; k++) tank_tick(t, 1.0f / 60, NULL);
    if (fabsf(t->moist[humid] - (m + MIST_PASS)) > 1e-5f) PF("a stroke crossing the hide six times: moisture %.3f, want one pass (%.3f)", t->moist[humid], m + MIST_PASS);
    stroke(t, hx - 70, hy, hx + 70, hy, 40);
    if (fabsf(t->moist[humid] - 1.0f) > 1e-5f) PF("a third pass from %.3f: %.3f, want it full (1)", m + MIST_PASS, t->moist[humid]);
    t->moist[humid] = 0.2f;
    tank_touch_drag(t, hx, hy); tank_tick(t, 1.0f / 60, NULL); tank_tick(t, 1.0f / 60, NULL);   /* a tap */
    if (fabsf(t->moist[humid] - (0.2f + MIST_PASS)) > 1e-5f) PF("a tap on the humid hide: moisture %.3f, want %.3f", t->moist[humid], 0.2f + MIST_PASS);
    m = t->moist[humid];
    stroke(t, tank_floor_x(0.05f), 60, tank_floor_x(0.4f), 120, 40);                           /* over the open terrarium */
    if (t->moist[humid] != m) PF("a stroke over the open terrarium misted the humid hide");
    for (int i = 0; i < ALGAE_CELLS; i++) t->algae[i] = 200;
    int32_t cleaned = t->cells_cleaned, trims = t->trims;
    float vx0, vx1; tank_veg_bed(t, 1, &vx0, &vx1, NULL, NULL);
    stroke(t, vx0 - 20, TANK_BOT - 40.0f, vx1 + 20, TANK_BOT - 40.0f, 60);                    /* through the film and the grass */
    if (t->cells_cleaned != cleaned || t->trims != trims) PF("the mister wiped %d cells and cut %d times", (int)(t->cells_cleaned - cleaned), (int)(t->trims - trims));
    for (int i = 0; i < ALGAE_CELLS; i++) t->algae[i] = 0;
    tank_set_tool(t, TOOL_SPONGE); m = t->moist[humid];
    stroke(t, hx - 70, hy, hx + 70, hy, 40);
    if (t->moist[humid] != m) PF("the sponge misted the humid hide");
    tank_set_tool(t, TOOL_MISTER);
    printf("selftest-mister: a stroke over the humid hide is one pass (+%.2f) however it crosses, a tap one too, the open glass none; it never wipes or cuts, the sponge never mists\n", MIST_PASS);
    /* the soak: held SOAK_HOLD_S on a stuck lizard it comes off - stress +1, trust +0.2 on a trusting one - and once a hold */
    fish_t *f = &t->fish[0], *g = &t->fish[1];
    f->shed = SHED_STUCK; f->shed_s = 100; f->shed_damp = 0; f->trust = 7; f->stress = 0;
    float j0 = hold(t, wx, wy, SOAK_HOLD_S - 0.3f, 0);
    if (f->shed != SHED_STUCK || j0 > 0.5f) PF("%.1f s of the mister held on a stuck lizard already acted (%s, a jump of %.2f)", SOAK_HOLD_S - 0.3f, SHED_WORD[f->shed], j0);
    float j1 = hold(t, wx, wy, SOAK_HOLD_S + 3, 0);
    if (f->shed != SHED_NORMAL || f->shed_clean != 1 || fabsf(f->trust - 7.2f) > 0.01f || j1 < 0.95f || j1 > 1.05f || t->hold_time != 0)
        PF("the soak: %s, %d clean, trust %.2f (want 7.2), stress jumped %.2f once (want +1), the hold drew fish (%.1f s)", SHED_WORD[f->shed], f->shed_clean, f->trust, j1, t->hold_time);
    printf("selftest-mister: held %.0f s on a stuck lizard: it came off clean, stress +%.2f once in a %.0f s hold, trust 7.0 -> %.1f; the finger drew no one\n", SOAK_HOLD_S, j1, SOAK_HOLD_S + 3, f->trust);
    /* the spray: a healthy lizard only dislikes it; a trusting stuck one gains no trust when it is not stuck */
    g->shed = SHED_NORMAL; g->shed_s = 1e5f; g->trust = 7; g->stress = 0;
    float j2 = hold(t, wx + 60, wy, SOAK_HOLD_S + 0.5f, 1);
    if (g->shed != SHED_NORMAL || g->trust != 7 || j2 < 0.95f) PF("the spray on a healthy lizard: %s, trust %.2f, stress jumped %.2f (want +1, nothing else)", SHED_WORD[g->shed], g->trust, j2);
    /* ... and one that walks off from under it is let go: no spray */
    g->stress = 0; float j3 = 0;
    for (float s = 0; s < SOAK_HOLD_S + 0.5f; s += 1.0f / 30) {
        if (s > 1) park(1, wx + 60 + MIST_KEEP_R + 20, wy);
        float was = g->stress;
        tank_touch_hold(t, wx + 60, wy); tank_tick(t, 1.0f / 30, NULL); park_all(t); progression_tick(t, 1.0f / 30);
        if (g->stress - was > j3) j3 = g->stress - was;
    }
    if (j3 > 0.5f) PF("a lizard that walked from under the mister was sprayed (+%.2f)", j3);
    printf("selftest-mister: a healthy lizard held %.0f s is sprayed (stress +%.2f, nothing else); one that walks off is let go\n", SOAK_HOLD_S, j2);
    /* the DONE chip says the mister is in hand; the box takes it back after TOOL_IDLE_S untouched */
    static uint16_t fb[TANK_W * TANK_H];
    memset(fb, 0, sizeof fb); render_tool_chip(t, -1, fb, TANK_W);
    int lit = 0; for (int i = 0; i < TANK_W * TANK_H; i++) lit += fb[i] != 0;
    if (!lit || !render_tool_chip_hit(t, RENDER_CARD_X + 20, RENDER_CARD_Y + 10)) PF("no DONE chip with the mister in hand");
    for (int k = 0; k < (int)((TOOL_IDLE_S + 1) * 30); k++) tank_tick(t, 1.0f / 30, NULL);
    if (t->tool != TOOL_HAND) PF("the mister stayed in hand %.0f s untouched", TOOL_IDLE_S + 1);
    printf("selftest-mister: the DONE chip shows it (%d px); back in the box after %.0f s untouched\n", lit, TOOL_IDLE_S);
    printf("selftest-mister OK\n");
    return 0;
}
