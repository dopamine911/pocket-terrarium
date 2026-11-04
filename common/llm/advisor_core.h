/* advisor_core.h — the platform-agnostic half of the LLM advisor: state
 * encoding (schema v2, and v3 once a v3 tokenizer is loaded), inference with
 * the shipped 4-bit engine, and the distribution layer (sampled goal token,
 * confidence, runner-up). sim/advisor_llm.c and firmware/main/advisor_llm_esp.c
 * only add threading around this. One copy of the encoder = one place for the
 * byte-exact contract with model/gen_traces.py. */
#ifndef ADVISOR_CORE_H
#define ADVISOR_CORE_H
#include "tank.h"
#include "q4_model.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* open model + tokenizer; alloc is the run-state allocator (PSRAM on device).
 * Detects the schema from the tokenizer (a " trust" token = v3, " bored" = v4,
 * " heat" = the lizard's L1: model/schema-lizard.md). Its goal words are its
 * own set of tank.h's goals (the fish's eight, the lizard's seven); a model of
 * the other world is refused (false) - but for the terrarium's fish model, its
 * temporary brain (THE TEMPORARY FISH-MODEL BRIDGE, below). */
bool advisor_core_init(const uint8_t *model_bin, size_t model_len,
                       const uint8_t *tok_bin, size_t tok_len,
                       void *(*alloc)(size_t), uint32_t seed);
#define ADVISOR_SCHEMA_L1 100                   /* advisor_core_schema() with a lizard tokenizer */
int  advisor_core_schema(void);                 /* 2, 3, 4 or ADVISOR_SCHEMA_L1; 0 if not loaded */
const q4_config_t *advisor_core_config(void);
q4_model_t *advisor_core_model(void);           /* for q4_model_bench */

/* encode fish idx's state line per schema (no trailing " ->"): the fish's
 * v2-v4, or L1 (advisor_core_l1_snap + advisor_core_encode_l1); in the
 * terrarium with a fish model, the bridge's v4 line of a lizard */
void advisor_core_encode(const tank_t *t, int idx, char *out, size_t n);

/* ---- schema L1 (model/schema-lizard.md) ----
 * One lizard as the L1 line sees it: exactly the fields of the parity
 * fixture's `snap` (model/testdata/l1_parity.jsonl, written by
 * model/make_l1_parity.py; R4 in PLAN.md), so advisor_core_encode_l1 is a
 * pure function the C and Python encoders can be diffed on. */
typedef struct { bool on; float x, y; } l1_point_t;   /* a sighting's point; on = false: none */
typedef struct {
    uint8_t    pack;                    /* species pack id (species.h): body_c -> heat on its map */
    int        zone;                    /* 1..6, precomputed (the encoder computes no zones) */
    float      x, y, heading;           /* the lizard: px, radians - for every distance and clock */
    float      body_c;                  /* degrees C */
    float      hunger, energy, stress, curiosity, trust, bored;   /* 0..10: drive9 */
    float      bold, social;            /* 0..1: (int)(v * 9 + 0.5) */
    int        stage;                   /* 0 hatch, 1 juv, 2 adult, 3 elder (L1_STAGE_NAMES) */
    int        shed;                    /* 0 no, 1 soon, 2 yes (L1_SHED_NAMES) */
    int        last;                    /* LZ_* (species.h LZ_GOAL_NAMES) */
    int        phase;                   /* PHASE_* (species.h PHASE_NAMES) */
    l1_point_t prey, other, warm, shelter;
    bool       in_shelter;              /* true: `shelter near 12`, whatever the shelter point */
    l1_point_t wall;                    /* the nearest glass's contact point: `wall <b> <clock>` only near or mid, else `clear` */
} l1_snap_t;
extern const char *const L1_STAGE_NAMES[4];     /* hatch juv adult elder */
extern const char *const L1_SHED_NAMES[3];      /* no soon yes */
/* the L1 state line of s (no trailing " ->"), byte for byte model/gen_traces.py encode_l1_snap */
void advisor_core_encode_l1(const l1_snap_t *s, char *out, size_t n);
/* fish idx of the tank as an L1 snapshot: zone as the fish encoder's grid, prey = the nearest pellet (until
 * P2c's crickets), other = the nearest other lizard, warm = the basking spot, shelter = the hide it is in or the
 * nearest (in_shelter: it is in one - fish_t.hide, THE WALK), wall = the fish encoder's contact point, last = its
 * goal (a lizard goal's LZ_* index), phase = tank_activity_phase */
void advisor_core_l1_snap(const tank_t *t, int idx, l1_snap_t *s);
/* THE TEMPORARY FISH-MODEL BRIDGE (P2b; advisor_core.c has all of it - DELETE IN P6 with the lizard model): the
 * terrarium's bin thinks with the v4 fish model until then. Its answers are the lizard goals they play (seek_food
 * hunt, visit_bubbles bask, inspect_reef and flee_shadow hide, follow_friend investigate, explore explore, rest
 * rest, dart_play dig), and a lizard's goal reads back as a fish goal on the v4 line's `last` */
goal_id_t advisor_bridge_lizard_goal(goal_id_t fish);
goal_id_t advisor_bridge_fish_goal(goal_id_t lizard);

/* run the model on a state line. Returns the goal (id == GOAL_COUNT if the
 * output was unparseable: keep the previous goal), with confidence and
 * runner_up filled from the goal-token softmax - always a goal of its build's
 * world. tokens_out (may be NULL) receives prompt+generated token count for
 * tok/s accounting. */
goal_t advisor_core_infer(const char *state, int *tokens_out);

/* the distribution layer knobs: sampling on (the model's own distribution,
 * T = 1) or greedy. Never a fallback - the model owns the decision either
 * way; sampling just stops collapsing its variety into the mode. */
extern bool  advisor_core_sample;
extern bool advisor_core_two_pass;             /* the goal + " urgency" as one batched pass (default on); off = the old three passes */
extern float advisor_core_temp;
/* last decision's full goal distribution (for debug / narration), indexed by goal_id_t: the build's world's goals
 * (GOAL_FIRST .. GOAL_FIRST + GOAL_N - 1) filled, the rest 0 */
const float *advisor_core_last_probs(void);
/* true once any decision's odds came back not a number (NaN / inf): a damaged
 * model. Sticky until the next start; the platform raises NOTICE_MODEL_DAMAGED. */
bool advisor_core_damaged(void);
#endif
