/* species.c - the pack registry, the heat map, the phase and the pack rules (species.h) */
#include "species.h"
#include "habitat.h"
#include "tank.h"                          /* POP_CAP */
#include "packs/builtin.h"                 /* PTPK_BUILTIN_SPECIES: how many packs this build ships */
#include "packs/ptpk.h"                    /* packs_init, PACKS_SPARE */
#include <stdio.h>
#include <string.h>

/* (LZ_GOAL_NAMES and FISH_GOAL_NAMES, the words a pack names its goals by, are packs/ptpk_write.c's: the host emitter links
   them without this registry) */
const char *const PHASE_NAMES[PHASE_N] = { "active", "settle", "sleep" };

/* THE POOL (docs/ptpk.md "The loader"): this build's built-in packs and PACKS_SPARE slots for the ones found at run time,
 * in .bss - nothing is malloc'd on the device path. The loader fills it a slot at a time (species_slot, species_commit);
 * the built-in packs go in first, so a pack that is not built in is always at the end (species_drop_loaded). s_crc is
 * the body CRC of the file each came from, to tell the same file offered again from another with its uid. */
#define SPECIES_POOL (PTPK_BUILTIN_SPECIES + PACKS_SPARE)
static species_t s_pool[SPECIES_POOL];
static uint32_t  s_crc[SPECIES_POOL];
static int       s_n;

int species_count(void) { packs_init(); return s_n; }
const species_t *species_at(int i) { packs_init(); return i >= 0 && i < s_n ? &s_pool[i] : NULL; }
const species_t *species_get(uint8_t id) {
    packs_init();
    for (int i = 0; i < s_n; i++) if (s_pool[i].id == id) return &s_pool[i];
    return NULL;
}
const species_t *species_by_uid(uint32_t uid) {
    packs_init();
    for (int i = 0; uid && i < s_n; i++) if (s_pool[i].uid == uid) return &s_pool[i];
    return NULL;
}

/* its habitats' bits: the loaded habitats whose uids it lists (a habitat not loaded - another world's, a card's not yet read -
   sets none) */
static void relink(species_t *p) {
    p->habitats = 0;
    for (int k = 0; k < p->n_habitat_uids && k < SPECIES_HABITATS_MAX; k++) {
        const habitat_t *h = habitat_by_uid(p->habitat_uid[k]);
        if (h && h->id >= 1 && h->id <= 31) p->habitats |= 1u << h->id;
    }
}
void species_relink(void) { for (int i = 0; i < s_n; i++) relink(&s_pool[i]); }

species_t *species_slot(void) {
    packs_init();
    if (s_n >= SPECIES_POOL) return NULL;
    memset(&s_pool[s_n], 0, sizeof s_pool[s_n]);
    return &s_pool[s_n];
}

bool species_commit(species_t *p, bool builtin, uint8_t legacy_id, uint32_t crc, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (p != &s_pool[s_n] || s_n >= SPECIES_POOL) REFUSE("not the slot the registry gave out");
    if (!p->uid) REFUSE("no uid");
    for (int i = 0; i < s_n; i++) if (s_pool[i].uid == p->uid) REFUSE("uid %08x is %s's already", (unsigned)p->uid, s_pool[i].name);
    if (builtin) {                                         /* the one-byte id old saves hold */
        if (!legacy_id) REFUSE("a built-in pack has no legacy id");
        for (int i = 0; i < s_n; i++) if (s_pool[i].id == legacy_id) REFUSE("legacy id %d is %s's already", legacy_id, s_pool[i].name);
        p->id = legacy_id;
    } else {                                               /* a handle for this run: the lowest free (the built-in ids stay clear of it) */
        int id = 3;
        for (; id <= 249; id++) { bool used = false; for (int i = 0; i < s_n; i++) used |= s_pool[i].id == id; if (!used) break; }
        if (id > 249) REFUSE("no free id");
        p->id = (uint8_t)id;
    }
    p->builtin = builtin;
    relink(p);
    if (!p->habitats) REFUSE("it lists %d habitat(s) and none is loaded", p->n_habitat_uids);
    if (!species_check(p, why, n)) return false;
    s_crc[s_n] = crc; s_n++;
    return true;
    #undef REFUSE
}

pack_seen_t species_seen(uint32_t uid, uint32_t crc, const char **by) {
    packs_init();
    for (int i = 0; i < s_n; i++)
        if (s_pool[i].uid == uid) { if (by) *by = s_pool[i].name; return s_crc[i] == crc ? PACK_SAME : PACK_UID_TAKEN; }
    return PACK_NEW;
}

void species_drop_loaded(void) {
    int k = 0;
    for (int i = 0; i < s_n; i++) if (s_pool[i].builtin) { if (k != i) { s_pool[k] = s_pool[i]; s_crc[k] = s_crc[i]; } k++; }
    s_n = k;
}

float species_heat_of(const species_t *p, float c) {
    if (c <= p->t_cold) return 0.0f;
    if (c < p->t_lo)    return 5.0f * (c - p->t_cold) / (p->t_lo - p->t_cold);
    if (c < p->t_hi)    return 5.0f + 2.0f * (c - p->t_lo) / (p->t_hi - p->t_lo);
    if (c < p->t_hot)   return 7.0f + 3.0f * (c - p->t_hi) / (p->t_hot - p->t_hi);
    return 10.0f;
}

float lizard_stage_scale(int stage) {
    static const float K[4] = { 0.42f, 0.68f, 1.0f, 1.08f };   /* a hatchling ~8 cm of an adult's ~20 */
    return K[stage < 0 ? 0 : stage > 3 ? 3 : stage];
}

/* SCHED_DAY (every v1 pack, PLAN.md D2): active while the light is on, settle
 * for the first settle_s of the dark, sleep for the rest of it. The other
 * schedules need a clock-based day (docs/species-pack.md "Phase", P5+):
 * species_check refuses them, so no pack that reaches here has one. */
int species_phase(const species_t *p, bool lit, float since_s) {
    if (lit) return PHASE_ACTIVE;
    return since_s < p->settle_s ? PHASE_SETTLE : PHASE_SLEEP;
}

/* the rules every pack keeps, its world's on top: a terrarium pack walks (THE WALK's pace and stop-and-go), one that
 * sheds keeps the shed cycle's (shed_every_s ...), and every pack's goals are of its own world's set */
bool species_check(const species_t *p, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)
    if (!p->name[0]) REFUSE("no name");
    if (p->world != WORLD_AQUARIUM && p->world != WORLD_TERRARIUM) REFUSE("world %d is no world", (int)p->world);
    if (!p->habitats || (p->habitats & 1u)) REFUSE("habitats %08x: it must live somewhere, and habitat ids start at 1", (unsigned)p->habitats);
    if (!(p->t_cold < p->t_lo && p->t_lo < p->t_hi && p->t_hi < p->t_hot))
        REFUSE("the heat band is out of order (%.1f / %.1f / %.1f / %.1f)", p->t_cold, p->t_lo, p->t_hi, p->t_hot);
    if (p->t_hi - p->t_lo < 2.0f) REFUSE("the comfort band is %.1f C wide, under 2", p->t_hi - p->t_lo);
    const struct { const char *what; float v; } RATE[] = {
        { "settle_s", p->settle_s },
        { "tau_s[hatch]", p->tau_s[0] }, { "tau_s[juv]", p->tau_s[1] }, { "tau_s[adult]", p->tau_s[2] }, { "tau_s[elder]", p->tau_s[3] },
        { "hunger_per_s", p->hunger_per_s }, { "energy_per_s", p->energy_per_s }, { "energy_rest_per_s", p->energy_rest_per_s },
        { "curiosity_per_s", p->curiosity_per_s }, { "hunger_per_cricket", p->hunger_per_cricket },
        { "sleep_hunger_per_h", p->sleep_hunger_per_h }, { "sleep_energy_per_h", p->sleep_energy_per_h }, { "sleep_stress_per_h", p->sleep_stress_per_h },
        { "stage_s[juv]", p->stage_s[0] }, { "stage_s[adult]", p->stage_s[1] }, { "stage_s[elder]", p->stage_s[2] },
    };
    for (size_t k = 0; k < sizeof RATE / sizeof RATE[0]; k++)
        if (!(RATE[k].v > 0)) REFUSE("%s is %g, not > 0", RATE[k].what, RATE[k].v);
    if (p->sheds) {
        const struct { const char *what; float v; } SHED[] = {
            { "shed_every_s[hatch]", p->shed_every_s[0] }, { "shed_every_s[juv]", p->shed_every_s[1] },
            { "shed_every_s[adult]", p->shed_every_s[2] }, { "shed_every_s[elder]", p->shed_every_s[3] },
            { "shed_soon_s", p->shed_soon_s }, { "shed_window_s", p->shed_window_s },
        };
        for (size_t k = 0; k < sizeof SHED / sizeof SHED[0]; k++)
            if (!(SHED[k].v > 0)) REFUSE("%s is %g, not > 0 (it sheds)", SHED[k].what, SHED[k].v);
    }
    if (p->world == WORLD_TERRARIUM) {                    /* it walks (tank.c THE WALK) */
        for (int s = 0; s < 4; s++) if (!(p->walk_px_s[s] > 0)) REFUSE("walk_px_s[%d] is %g, not > 0 (a terrarium pack walks)", s, p->walk_px_s[s]);
        if (!(p->bout_s[0] > 0 && p->bout_s[0] <= p->bout_s[1])) REFUSE("bout_s %.2f..%.2f is no span of seconds", p->bout_s[0], p->bout_s[1]);
        if (!(p->pause_s[0] > 0 && p->pause_s[0] <= p->pause_s[1])) REFUSE("pause_s %.2f..%.2f is no span of seconds", p->pause_s[0], p->pause_s[1]);
        if (!(p->turn_s > 0)) REFUSE("turn_s is %g, not > 0", p->turn_s);
        if (p->plan == PLAN_LIZARD) {                     /* its body drawn stays inside the glass, and its middle still reaches every ramp's end */
            const float most = lizard_half_len(&p->geom, lizard_stage_scale(STAGE_ELDER) * 1.05f);   /* (tank.c tank_lizard_scale's largest) */
            if (!(p->geom.length_px > 0) || most > TERRAIN_LINK_IN_PX)
                REFUSE("geom.length_px %g: its biggest elder's half length %.1f px passes TERRAIN_LINK_IN_PX %.0f (a ramp's end out of its reach)", p->geom.length_px, most, TERRAIN_LINK_IN_PX);
        }
    }
    if (!(p->stage_s[0] < p->stage_s[1] && p->stage_s[1] < p->stage_s[2]))
        REFUSE("stage_s is not strictly increasing (%.0f / %.0f / %.0f)", p->stage_s[0], p->stage_s[1], p->stage_s[2]);
    for (int s = 0; s < 4 && p->sheds; s++)
        if (p->shed_every_s[s] < p->shed_soon_s + p->shed_window_s)
            REFUSE("shed_every_s[%d] %.0f is under shed_soon_s + shed_window_s (%.0f): more than half its life shedding", s, p->shed_every_s[s], p->shed_soon_s + p->shed_window_s);
    if (p->pop_cap < 1 || p->pop_cap > POP_CAP) REFUSE("pop_cap %d is outside 1..%d", p->pop_cap, POP_CAP);
    if (p->n_morphs < 1 || p->n_morphs > SPECIES_MORPHS_MAX) REFUSE("%d morphs, not 1..%d", p->n_morphs, SPECIES_MORPHS_MAX);
    for (int s = 0; s < 4; s++) if (!p->pattern[s].name[0]) REFUSE("no pattern for stage %d", s);
    if (p->schedule != SCHED_DAY) REFUSE("schedule %d needs the clock-based day (P5+): only SCHED_DAY runs", (int)p->schedule);
    if (!p->goals) REFUSE("no goals");
    const unsigned own = p->world == WORLD_TERRARIUM ? LZ_SCHEMA_GOALS : FISH_SCHEMA_GOALS;   /* its world's schema */
    if (p->goals & ~own) REFUSE("goals %03x need a goal its world's schema does not have (a climber): refused until the schema grows one", p->goals);
    /* the heat map: monotonic, 5.0 at t_lo and 7.0 at t_hi (and 0 / 10 past the ends). A band is a few tens of degrees: one far outside
       any body's (a file's hostile float - 1e19 C, where a step of 0.01 changes nothing) would walk this loop for ever, so it is refused first */
    if (!(p->t_cold > -100.0f && p->t_hot < 200.0f)) REFUSE("the heat band %.1f..%.1f C is outside -100..200 C", p->t_cold, p->t_hot);
    float prev = -1;
    for (float c = p->t_cold - 5; c <= p->t_hot + 5; c += 0.01f) {
        float h = species_heat_of(p, c);
        if (h < prev) REFUSE("the heat map falls at %.2f C (%.4f after %.4f)", c, h, prev);
        if (h < 0 || h > 10) REFUSE("the heat map leaves 0..10 at %.2f C (%.4f)", c, h);
        prev = h;
    }
    if (species_heat_of(p, p->t_lo) != 5.0f || species_heat_of(p, p->t_hi) != 7.0f)
        REFUSE("the heat map reads %.4f at t_lo and %.4f at t_hi, want 5 and 7", species_heat_of(p, p->t_lo), species_heat_of(p, p->t_hi));
    if (species_heat_of(p, p->t_cold) != 0.0f || species_heat_of(p, p->t_hot) != 10.0f)
        REFUSE("the heat map reads %.4f at t_cold and %.4f at t_hot, want 0 and 10", species_heat_of(p, p->t_cold), species_heat_of(p, p->t_hot));
    return true;
    #undef REFUSE
}
