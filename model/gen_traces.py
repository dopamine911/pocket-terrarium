#!/usr/bin/env python3
"""Trace generator for the pocket-tank / pocket-terrarium advisor model.

Runs a minimal headless sim, encodes per-animal state per the schema, asks a
teacher model (structured output) for a goal, and appends
{"state": ..., "goal": ...} lines to a JSONL file.

  python3 gen_traces.py --count 10 --dry-run    # print encoded states, no network
  python3 gen_traces.py --count 1000            # query Ollama, append to out/traces.jsonl
  python3 gen_traces.py --schema L1 --api openai --count 1000 --out out/l1_traces_a.jsonl
        # the lizard line, taught by the lab's chat-teacher (key from $LAB_API_KEY)

Stdlib only. Schema v2 is FROZEN (see schema.md); `--schema 3` emits the v3 line
(no fish names, + trust 0-9, tanks of 2-6 fish); `--schema 4` (2026-09-14) drops
the shadow - the predator left the game on 2026-09-13 - and adds `bored 0-9`,
how stale the fish's current activity is, so the teacher can push a fish out of
a rut (the bubble-column and follow-the-friend loops Strato saw on the device).
`--schema L1` (2026-11-04) is pocket-terrarium's lizard line
(model/schema-lizard.md): a Terrarium/Lizard sim beside Tank/Fish, the line
normalised to a species pack (docs/species-pack.md) by encode_l1_snap, and the
species-agnostic SYSTEM_PROMPT_L1. `--api openai` talks to an OpenAI-compatible
server (the lab's llama-swap) instead of Ollama.
Keep these encoders in exact sync with common/llm/advisor_core.c (L1: the parity
fixture testdata/l1_parity.jsonl, make_l1_parity.py).
"""

import argparse
import dataclasses
import json
import math
import os
import random
import subprocess
import sys
import time
import urllib.error
import urllib.request

from train_tokenizer import parse_schema

TANK_W, TANK_H = 448, 368
FISH_NAMES = ["mira", "bolt", "kelp", "nori", "pip", "sol"]
SCHEMA = 2          # set by --schema; 3 = v3 line, 4 = v4 line (see schema.md), "L1" = the lizard line (schema-lizard.md)
GOALS = [
    "seek_food", "flee_shadow", "visit_bubbles", "follow_friend",
    "explore", "rest", "dart_play", "inspect_reef",
]
GOALS_V4 = [g for g in GOALS if g != "flee_shadow"]   # no shadow, nothing to flee
# the pastimes boredom accrues on (rest counts by day only; eating never)
LEISURE = ["visit_bubbles", "follow_friend", "explore", "dart_play", "inspect_reef"]
# boredom dynamics, per sim tick (~a frame); mirrors common/tank.c BORED_*
BORED_PER_TICK, BORED_NEW_GOAL, BORED_NEW_ZONE, BORED_RELIEF_PER_TICK = 0.08, 4.0, 1.5, 0.15


def is_l1():
    return SCHEMA == "L1"


def goals():
    return GOALS_L1 if is_l1() else GOALS_V4 if SCHEMA >= 4 else GOALS

# Distance buckets in pixels (tank diagonal ~580).
NEAR, MID, FAR = 70, 180, 380

SYSTEM_PROMPT = (
    "You are the sparse instinct advisor for a small aquarium fish. "
    "You receive one line describing the fish's identity and drives (0-9) and what "
    "it senses (distances none/near/mid/far, directions as clock positions relative "
    "to its heading, 12 = dead ahead). Choose the single most fitting goal and an "
    "urgency 0-9. The goals: seek_food (hungry and food exists), flee_shadow (a "
    "shadow looms, more urgent the closer it is), rest (low energy, or nighttime "
    "calm), follow_friend (sociable, a friend in view), visit_bubbles (relaxed fish "
    "enjoy playing in the bubble column), dart_play (high energy and playful, a "
    "joyful burst), inspect_reef (curious about the reef, especially when near it), "
    "explore (default wandering when nothing else calls). "
    "Identity shades every choice. bold: a bold fish (7-9) takes risks, plays, and "
    "shrugs off distant shadows; a timid fish (0-2) startles easily, flees sooner, "
    "and prefers the reef's safety. social: only a very social fish (7-9) follows "
    "friends now and then; below that, following a friend is rare - friends are "
    "usually nearby in a small tank, so mere proximity is never a reason. stage: "
    "fry stay near friends and the reef and are easily scared; juv are playful "
    "and curious; adult are balanced; elder rest more and play less. "
    "A shadow is a predator: when a shadow is near, fleeing beats everything, and "
    "no fish should seek food near or under a shadow - survival first. "
    "At night fish wind down and rest by the reef unless something urgent calls. "
    "A content fish, fed and safe, should vary between the playful and curious "
    "goals rather than always exploring. Keep the last goal only while it still "
    "makes sense: stop seeking food when hunger is low, stop fleeing when no "
    "shadow is in sight. Use the whole urgency range: 9 is life-or-death, 7-8 "
    "pressing, 4-6 a normal want, 1-3 mild, 0 an idle whim."
)

# v3 additions (trust, and friend none): appended to the system prompt for --schema 3
SYSTEM_PROMPT_V3 = SYSTEM_PROMPT + (
    " trust: how much the fish trusts its keeper (0-9). A trusting fish (7-9) is "
    "calmer - it recovers from a fright sooner, is comfortable near the surface, "
    "and is a little bolder about food and play; a wary fish (0-2) is jumpier - it "
    "flees at less, stays lower and nearer the reef, and takes longer to settle. "
    "Trust shades but never overrides hunger or a predator. If no friend is in "
    "view (friend none), following is impossible - choose something else."
)

# v4 (2026-09-14): the shadow is gone from the game, so the predator paragraph
# goes; `bored` arrives; explore is a real activity, not the fallback; stress
# gets its own meaning now that nothing looms. Identity wording stays the v2
# text that produced the personality cliffs v3m ships with (docs/stats.md:
# the v3 rewrite flattened them - prompt-is-DNA). Check with prompt_check.py
# before any overnight run.
SYSTEM_PROMPT_V4 = (
    "You are the sparse instinct advisor for a small aquarium fish. "
    "You receive one line describing the fish's identity and drives (0-9) and what "
    "it senses (distances none/near/mid/far, directions as clock positions relative "
    "to its heading, 12 = dead ahead). Choose the single most fitting goal and an "
    "urgency 0-9. The goals: seek_food (hungry and food exists), rest (low energy, "
    "or nighttime calm), follow_friend (sociable, a friend in view), visit_bubbles "
    "(relaxed fish enjoy playing in the bubble column), dart_play (high energy and "
    "playful, a joyful burst), inspect_reef (curious about the reef, especially when "
    "near it), explore (cruising off to a part of the tank the fish has not seen in "
    "a while - a real activity in its own right, not a fallback: a healthy fish "
    "spends a good share of its day exploring). "
    "Identity shades every choice. bold: a bold fish (7-9) takes risks, plays, and "
    "roams the open water; a timid fish (0-2) startles easily, prefers the reef's "
    "safety, and does not dart: dart_play is only ever chosen by a fish with bold 5 "
    "or more and energy to burn - a fish with bold 0-3 never picks it, however "
    "energetic. social: only a very social fish (7-9) follows "
    "friends now and then; below that, following a friend is rare - friends are "
    "usually nearby in a small tank, so mere proximity is never a reason. stage: "
    "fry stay near friends and the reef and are easily scared; juv are playful "
    "and curious; adult are balanced; elder rest more and play less. "
    "trust: how much the fish trusts its keeper (0-9). A trusting fish (7-9) is "
    "calmer - it recovers from a fright sooner, is comfortable near the surface, "
    "and is a little bolder about food and play; a wary fish (0-2) is jumpier - it "
    "stays lower and nearer the reef, and takes longer to settle. "
    "Trust shades but never overrides hunger. "
    "stress: a stressed fish (7-9) has been startled or crowded - it settles by "
    "the reef or rests until it calms, and does not play. "
    "bored: how stale the fish's current activity has become (0 = just started, "
    "9 = stuck in a rut). A fresh fish (0-2) naturally keeps its last goal while "
    "it still makes sense. As boredom rises the fish wants a change: at 5-6 "
    "repeating last is unlikely, and at 7-9 the fish must choose something "
    "different from last - the more bored it is, the more it favors explore, "
    "which takes it somewhere new. Boredom only chooses among daytime pastimes: it "
    "never outranks hunger, and it never overrides the night. "
    "At night fish wind down and rest by the reef unless something urgent calls - "
    "a bored fish at night rests too; nothing about being bored justifies "
    "exploring in the dark. "
    "A content fish, fed and safe, spreads its time across explore, the bubbles, "
    "the reef, play and (if social) friends - no single pastime should dominate, "
    "and the bubble column is one option among several, never the default. "
    "If no friend is in view (friend none), following is impossible - choose "
    "something else. Stop seeking food when hunger is low. Use the whole urgency "
    "range: 9 is life-or-death, 7-8 pressing, 4-6 a normal want, 1-3 mild, 0 an "
    "idle whim."
)

# L1 (2026-11-04): the lizard line, model/schema-lizard.md. Written from its
# "Behaviour the teacher must express" (all nine rules, in order), the field
# meanings and the seven goals; species-agnostic on purpose - every number in
# the line is already normalised to the lizard's species pack, so the prompt
# never names a species, a temperature or a time of day. Style as V4. Check with
# pilot.py (pilot_l1.jsonl) and prompt_check.py --schema L1 before any long run.
# History (chat-teacher; docs/retrain-lizard.md has the numbers): try 1 agreed
# 87.8% on the pilot; try 2 added "a cold lizard too" to rule 4 (A7 fixed); try 4
# synced rule 1 with the spec (a fed lizard may snap at prey in front of it) and
# put rule 7's bold clause as "rests or digs instead of exploring, even when
# curious"; rule 6's "it has only just begun ... settles into a pastime for a
# while" fixed A10; "explore is one choice among the four, never the default" took
# the content states' explore from 0.67 (try 4) and 0.50 to 0.29 on the same 24
# states. This text is the 2026-11-04 gate: prompt_check every row OK (464 calls),
# pilot 108/111 - only A11-bold0 (a timid lizard whose last was rest explores at bored 3).
SYSTEM_PROMPT_L1 = (
    "You are the sparse instinct advisor for a small pet lizard in a terrarium. "
    "You receive one line describing the lizard's drives and identity (0-9) and what "
    "it senses (distances none/near/mid/far, directions as clock positions relative "
    "to its heading, 12 = dead ahead, 6 = behind). Every number is already scaled to "
    "this lizard's own species, so read each one as it stands. Choose the single most "
    "fitting goal and an urgency 0-9. "
    "The fields: zone (where it is); hunger (9 = starving); energy (0 = exhausted); "
    "stress (9 = panicked); curiosity (9 = restless to look around); heat, its body "
    "temperature (0-2 cold and sluggish, 3-4 cool, 5-6 comfortable, 7 warm, 8-9 "
    "overheating); bold (risk-taking; a bold lizard roams open ground); social "
    "(tolerance of other lizards; low = territorial); stage (hatch, juv, adult, "
    "elder); trust (in its keeper); bored (how stale its current activity has become: "
    "0 = just started, 9 = stuck in a rut); shed (no; soon = about to shed; yes = "
    "shedding now, or an old skin stuck on it); prey (the nearest cricket or food "
    "dish); other (the nearest other lizard); warm (the basking spot, where heat comes "
    "from); shelter (the nearest hide; near 12 means it is inside one); wall (nearby "
    "glass); last (the goal it is carrying out); phase (active = its waking hours; "
    "settle = winding down toward its rest; sleep = its rest hours). "
    "The goals: hunt (stalk and catch the prey in sight; with prey none it can only "
    "wander where food was), bask (go to the warm spot and soak up heat - how a lizard "
    "warms up), hide (go into a hide; it picks the right one itself: a cool hide when "
    "hot, a humid hide when shedding, a warm one otherwise - how a lizard cools down, "
    "calms down, sheds its skin and spends its rest), explore (a slow walk with tongue "
    "flicks into a part of the terrarium it has not seen for a while - a real activity, "
    "not a fallback), investigate (approach and tongue-flick something: decor, or the "
    "other lizard), dig (scrabble at the substrate: play, and the cure for a rut), rest "
    "(lie still where it is, eyes closing). "
    "How a lizard chooses - tendencies that personality and the other drives shade: "
    "1. Starving beats everything except overheating: hunger 8-9 with prey in sight "
    "means hunt. But cold spoils a hunt: a cold lizard (heat 0-2) still hunts prey that "
    "is near; with prey far or none it basks first. A fed lizard may still snap at prey "
    "right in front of it. "
    "2. Temperature is the main reason to move: heat 0-3 means bask unless something "
    "urgent calls; heat 8-9 means hide; heat 5-6 is no pull either way. "
    "3. Shedding wants humidity: with shed soon or yes it hides far more often than usual "
    "(the humid hide is where the skin comes off) and its appetite drops - it hunts only "
    "when quite hungry, and with shed yes and hunger below 7 it rarely hunts. "
    "4. Phase sets the rhythm: in sleep it rests or hides unless starving - a cold lizard "
    "too: it hides to keep warm rather than basking; in settle it finishes its business "
    "first (eats if hungry, warms up if cold), then hides or rests; in active it is free. "
    "5. Stress 7-9 means hide, more so for a timid (bold 0-2) or wary (trust 0-2) lizard. "
    "6. Boredom moves it on: bored 7-9 means a goal different from last, usually "
    "explore or dig; bored 0-2 means it has only just begun: it keeps last while it still "
    "makes sense - a lizard settles into a pastime for a while. Boredom only "
    "chooses among pastimes: it never outranks hunger, heat, shedding, stress, "
    "tiredness or the phase. "
    "7. Personality: bold 7-9 roams and investigates, and hunts prey from farther away; "
    "bold 0-2 stays near shelter and rests or digs instead of exploring, even when "
    "curious. A lizard with social 0-2 and the other "
    "near avoids it (hides, or explores away) and never investigates it; social 7-9 "
    "investigates the other now and then. "
    "8. Stage: hatchlings are skittish - they hide at far less stress than adults - and "
    "hunt hard when hungry; elders rest more. "
    "9. Energy 0-2 means rest (or bask if also cold). "
    "A content lizard - fed, comfortable, calm, in its active phase - spreads its time "
    "across explore, investigate, dig and rest; no single pastime should dominate, and "
    "explore is one choice among the four, never the default. "
    "Apart from a snap at prey right in front of it, stop hunting when hunger is low. "
    "Use the whole urgency range: 9 is life-or-death, "
    "7-8 pressing, 4-6 a normal want, 1-3 mild, 0 an idle whim."
)


def system_prompt():
    if is_l1():
        return SYSTEM_PROMPT_L1
    return SYSTEM_PROMPT_V4 if SCHEMA >= 4 else SYSTEM_PROMPT_V3 if SCHEMA >= 3 else SYSTEM_PROMPT


def output_schema():
    return {
        "type": "object",
        "properties": {
            "goal": {"type": "string", "enum": goals()},
            "urgency": {"type": "integer", "minimum": 0, "maximum": 9},
        },
        "required": ["goal", "urgency"],
    }


OUTPUT_SCHEMA = output_schema()     # v2/v3 shape (kept for importers); v4 callers use output_schema()


STAGES = ["fry", "juv", "adult", "elder"]


class Fish:
    def __init__(self, name, rng):
        self.name = name
        self.x = rng.uniform(40, TANK_W - 40)
        self.y = rng.uniform(40, TANK_H - 40)
        self.heading = rng.uniform(0, 2 * math.pi)
        self.hunger = rng.randint(2, 7)
        self.energy = rng.randint(3, 8)
        self.stress = rng.randint(0, 3)
        self.curiosity = rng.randint(2, 8)
        self.reroll_identity(rng)
        self.goal = rng.choice(goals())
        self.goal_prev = None       # the goal before the current one (a return to it is no relief)
        self.bored = rng.uniform(0, 3)   # v4 only (ignored by the v2/v3 encoders)
        self.zone_last = None
        self.speed = rng.uniform(1.0, 2.2)

    def set_goal(self, goal):
        """the reflex/advisor layer changed the goal: a genuinely new one relieves
        boredom; bouncing back to the one it just left does not (tank.c)"""
        if goal == self.goal:
            return
        if goal != self.goal_prev:
            self.bored = max(0.0, self.bored - BORED_NEW_GOAL)
        self.goal_prev, self.goal = self.goal, goal

    def reroll_identity(self, rng):
        """v2 identity: uniform traits, stage weighted toward adult so the
        student sees every personality but the common case dominates."""
        self.bold = rng.randint(0, 9)
        self.social = rng.randint(0, 9)
        self.stage = rng.choices(STAGES, weights=[15, 20, 50, 15])[0]
        self.trust = rng.randint(0, 9)      # v3 only (ignored by the v2 encoder)


class Tank:
    """Minimal headless sim: enough dynamics to sweep the state space, not physics."""

    def __init__(self, rng, n_fish=None):
        self.rng = rng
        if n_fish is None:
            n_fish = rng.randint(2, 6) if SCHEMA >= 3 else 4     # v3: the population varies
        self.fish = [Fish(n, rng) for n in FISH_NAMES[:n_fish]]
        self.food = []          # [x, y] pellets, sink slowly
        self.shadow = None      # [x, y] or None
        self.bubble = (TANK_W * 0.8, TANK_H * 0.5)
        self.reef = (TANK_W * 0.15, TANK_H * 0.85)
        self.tick_n = 0
        self.night = False
        self.night_until = 0    # a perturbed night ends here (v4: it used to stick until a random flip)

    def tick(self):
        self.tick_n += 1
        rng = self.rng
        # Day/night flips occasionally. A night set by the "night" perturbation
        # ends on its own: before 2026-09-14 it stuck until the next random
        # flip and 55% of the v4 states were night (the first 640 pairs) -
        # half the teacher's night spent labelling "rest".
        # (and the flip is asymmetric for v4: a symmetric toggle is night half
        # the time; nights now end 3x faster than they start, ~25% night)
        if rng.random() < (0.006 if (self.night and SCHEMA >= 4) else 0.002):
            self.night = not self.night
        if self.night_until and self.tick_n > self.night_until:
            self.night = False; self.night_until = 0
        # Food: spawn at surface sometimes, sink, expire at floor.
        if rng.random() < 0.05 and len(self.food) < 6:
            self.food.append([rng.uniform(20, TANK_W - 20), 10.0])
        for p in self.food:
            p[1] = min(TANK_H - 5, p[1] + 0.6)
        # Shadow: appears, roams, leaves (v2/v3 only: the predator left the game).
        if SCHEMA >= 4:
            self.shadow = None
        elif self.shadow is None:
            if rng.random() < 0.008:
                self.shadow = [rng.uniform(0, TANK_W), rng.uniform(0, TANK_H * 0.4)]
        else:
            self.shadow[0] += rng.uniform(-4, 4)
            self.shadow[1] += rng.uniform(-2, 2)
            if rng.random() < 0.01:
                self.shadow = None

        for f in self.fish:
            self._steer(f)
            f.x = max(5, min(TANK_W - 5, f.x + math.cos(f.heading) * f.speed))
            f.y = max(5, min(TANK_H - 5, f.y + math.sin(f.heading) * f.speed))
            # Drives drift.
            if rng.random() < 0.012:
                f.hunger = min(9, f.hunger + 1)
            if f.goal == "rest":
                f.energy = min(9, f.energy + (1 if rng.random() < 0.1 else 0))
            elif rng.random() < 0.008:
                f.energy = max(0, f.energy - 1)
            if f.energy <= 1 and rng.random() < 0.05:
                f.set_goal("rest")
            if f.hunger >= 6 and self.food and rng.random() < 0.08:
                f.set_goal("seek_food")
            shadow_near = self.shadow and dist(f, self.shadow) < MID
            f.stress = min(9, f.stress + 1) if shadow_near and rng.random() < 0.2 \
                else max(0, f.stress - 1) if rng.random() < 0.03 else f.stress
            if rng.random() < 0.01:
                f.curiosity = max(0, min(9, f.curiosity + rng.choice([-1, 1])))
            # Eat pellets in range.
            for p in list(self.food):
                if dist(f, p) < 12:
                    self.food.remove(p)
                    f.hunger = max(0, f.hunger - 3)
            # Reflex layer swaps goals heuristically between advisor calls.
            if rng.random() < 0.01:
                f.set_goal(rng.choice(goals()))
            # Boredom (v4): the same pastime goes stale; a fresh zone or a new
            # goal (set_goal) relieves it; eating and night rest are never boring.
            leisure = f.goal in LEISURE or (f.goal == "rest" and not self.night)
            f.bored = min(9.0, f.bored + BORED_PER_TICK) if leisure \
                else max(0.0, f.bored - BORED_RELIEF_PER_TICK)
            z = zone(f)
            if z != f.zone_last:
                if f.zone_last is not None:
                    f.bored = max(0.0, f.bored - BORED_NEW_ZONE)
                f.zone_last = z

    def _steer(self, f):
        rng = self.rng
        target = None
        if f.goal == "seek_food" and self.food:
            target = min(self.food, key=lambda p: dist(f, p))
        elif f.goal == "visit_bubbles":
            target = self.bubble
        elif f.goal == "inspect_reef":
            target = self.reef
        elif f.goal == "follow_friend":
            target = min((o for o in self.fish if o is not f), key=lambda o: dist(f, o))
        elif f.goal == "flee_shadow" and self.shadow:
            away = math.atan2(f.y - self.shadow[1], f.x - self.shadow[0])
            f.heading = away
            return
        if target is not None:
            tx, ty = (target.x, target.y) if isinstance(target, Fish) else (target[0], target[1])
            f.heading = math.atan2(ty - f.y, tx - f.x)
        else:
            f.heading += rng.uniform(-0.3, 0.3)
        # Bounce off walls.
        if f.x < 20 or f.x > TANK_W - 20 or f.y < 20 or f.y > TANK_H - 20:
            f.heading = math.atan2(TANK_H / 2 - f.y, TANK_W / 2 - f.x) + rng.uniform(-0.4, 0.4)

    def perturb(self, f):
        """Push a fish into an edge case before sampling. Cases are weighted to
        counter the natural skew toward hungry-fish-near-food (seek_food was 67%
        of teacher labels before the sated/social/playful/famine cases existed)."""
        rng = self.rng
        cases = [
            "starve", "exhausted", "panic", "night", "crowd", "calm",
            "sated", "sated", "social", "playful", "famine", "curious",
            "persona", "persona",   # v2: resample identity to sweep trait space
        ]
        if SCHEMA >= 4:
            # v4: no shadow cases; a fish in a RUT (bored 4-9 on a pastime, the
            # loops Strato saw), a fish that has just started something (fresh),
            # and the lone fish stays
            cases += ["lonely", "bored", "bored", "bored", "fresh"]
        elif SCHEMA >= 3:
            # v3: a shadow in view while CALM (the v2 student learned stress, not
            # shadow distance, as its flee cue - docs/progression-next.md), and a
            # lone fish (friend none, 17/28K in v2 data - the v2 model breaks on it)
            cases += ["shadow_calm", "shadow_calm", "lonely"]
        case = rng.choice(cases)
        if case == "persona":
            f.reroll_identity(rng)
        elif case == "bored":
            f.bored = rng.randint(4, 9)
            f.goal = rng.choice(LEISURE)
            f.hunger = rng.randint(0, 5)
            f.energy = rng.randint(4, 9)
            f.stress = rng.randint(0, 3)
            if f.goal == "follow_friend" and len(self.fish) > 1:     # the friend is there to follow
                friend = rng.choice([o for o in self.fish if o is not f])
                friend.x = f.x + rng.uniform(-90, 90); friend.y = f.y + rng.uniform(-60, 60)
        elif case == "fresh":
            f.bored = rng.randint(0, 1)
            f.hunger = rng.randint(0, 5)
        elif case == "shadow_calm":
            f.stress = rng.randint(0, 3)
            ang = rng.uniform(0, 2 * math.pi); d = rng.uniform(30, 170)
            self.shadow = [max(0, min(TANK_W, f.x + math.cos(ang) * d)),
                           max(0, min(TANK_H, f.y + math.sin(ang) * d))]
        elif case == "lonely":
            for o in self.fish:
                if o is not f:
                    # push every friend out of sight (> FAR) - into the far corner
                    o.x = TANK_W - 5 if f.x < TANK_W / 2 else 5
                    o.y = TANK_H - 5 if f.y < TANK_H / 2 else 5
        elif case == "starve":
            f.hunger = rng.randint(8, 9)
        elif case == "exhausted":
            f.energy = rng.randint(0, 1)
        elif case == "panic":
            f.stress = rng.randint(7, 9)
            if self.shadow is None and SCHEMA < 4:      # v4: stress without a predator (taps, crowding)
                self.shadow = [f.x + rng.uniform(-60, 60), f.y - 50]
        elif case == "night":
            self.night = True
            if SCHEMA >= 4:
                self.night_until = self.tick_n + rng.randint(40, 160)   # 1-4 samples of night
        elif case == "crowd":
            for o in self.fish:
                if o is not f:
                    o.x = f.x + rng.uniform(-50, 50)
                    o.y = f.y + rng.uniform(-50, 50)
        elif case == "calm":
            f.stress = 0
            self.shadow = None
        elif case == "sated":
            # Well-fed, comfortable: no dominant survival drive.
            f.hunger = rng.randint(0, 2)
            f.energy = rng.randint(5, 9)
            f.stress = rng.randint(0, 2)
        elif case == "social":
            f.hunger = rng.randint(0, 3)
            friend = rng.choice([o for o in self.fish if o is not f])
            friend.x = f.x + rng.uniform(-100, 100)
            friend.y = f.y + rng.uniform(-60, 60)
        elif case == "playful":
            f.hunger = rng.randint(0, 2)
            f.energy = rng.randint(7, 9)
            f.curiosity = rng.randint(7, 9)
            f.stress = 0
        elif case == "famine":
            # No food in the tank at all; hunger without an answer.
            self.food.clear()
        elif case == "curious":
            f.hunger = rng.randint(0, 3)
            f.curiosity = rng.randint(8, 9)
            # Drop the fish near the reef or bubbles so the option is live.
            spot = rng.choice([self.reef, self.bubble])
            f.x = spot[0] + rng.uniform(-90, 90)
            f.y = spot[1] + rng.uniform(-60, 60)
            f.x = max(5, min(TANK_W - 5, f.x))
            f.y = max(5, min(TANK_H - 5, f.y))


def dist(f, other):
    ox, oy = (other.x, other.y) if isinstance(other, Fish) else (other[0], other[1])
    return math.hypot(f.x - ox, f.y - oy)


def bucket(d):
    if d < NEAR:
        return "near"
    if d < MID:
        return "mid"
    if d < FAR:
        return "far"
    return None


def clock(f, ox, oy):
    """Clock position of (ox,oy) relative to fish heading; 12 = dead ahead."""
    rel = math.atan2(oy - f.y, ox - f.x) - f.heading
    hour = round((rel % (2 * math.pi)) / (2 * math.pi) * 12) % 12
    return 12 if hour == 0 else hour


def sighting(f, pos):
    if pos is None:
        return "none"
    ox, oy = (pos.x, pos.y) if isinstance(pos, Fish) else (pos[0], pos[1])
    b = bucket(dist(f, (ox, oy)))
    return "none" if b is None else f"{b} {clock(f, ox, oy)}"


def zone(f):
    col = min(2, int(f.x / (TANK_W / 3)))
    row = min(1, int(f.y / (TANK_H / 2)))
    return row * 3 + col + 1


def wall_field(f):
    dists = {3: TANK_W - f.x, 9: f.x, 6: TANK_H - f.y, 12: f.y}  # world-frame sides
    side, d = min(dists.items(), key=lambda kv: kv[1])
    b = bucket(d)
    if b in ("near", "mid"):
        # Re-express the wall direction relative to heading.
        wx = {3: TANK_W, 9: 0, 6: f.x, 12: f.x}[side]
        wy = {3: f.y, 9: f.y, 6: TANK_H, 12: 0}[side]
        return f"{b} {clock(f, wx, wy)}"
    return "clear"


def encode(tank, f):
    if is_l1():
        return encode_l1(tank, f)       # (tank: a Terrarium, f: a Lizard)
    nearest_food = min(tank.food, key=lambda p: dist(f, p)) if tank.food else None
    friend = min((o for o in tank.fish if o is not f), key=lambda o: dist(f, o))
    friend_s = sighting(f, friend)
    if SCHEMA >= 4:
        return (
            f"zone {zone(f)} "
            f"hunger {f.hunger} energy {f.energy} stress {f.stress} curiosity {f.curiosity} "
            f"bold {f.bold} social {f.social} stage {f.stage} trust {f.trust} bored {int(f.bored)} "
            f"food {sighting(f, nearest_food)} "
            f"friend {friend_s} bubble {sighting(f, tank.bubble)} reef {sighting(f, tank.reef)} "
            f"wall {wall_field(f)} last {f.goal} time {'night' if tank.night else 'day'}"
        )
    if SCHEMA >= 3:
        return (
            f"zone {zone(f)} "
            f"hunger {f.hunger} energy {f.energy} stress {f.stress} curiosity {f.curiosity} "
            f"bold {f.bold} social {f.social} stage {f.stage} trust {f.trust} "
            f"food {sighting(f, nearest_food)} shadow {sighting(f, tank.shadow)} "
            f"friend {friend_s} bubble {sighting(f, tank.bubble)} reef {sighting(f, tank.reef)} "
            f"wall {wall_field(f)} last {f.goal} time {'night' if tank.night else 'day'}"
        )
    friend_field = "none" if friend_s == "none" else f"{friend.name} {friend_s}"
    return (
        f"fish {f.name} zone {zone(f)} "
        f"hunger {f.hunger} energy {f.energy} stress {f.stress} curiosity {f.curiosity} "
        f"bold {f.bold} social {f.social} stage {f.stage} "
        f"food {sighting(f, nearest_food)} shadow {sighting(f, tank.shadow)} "
        f"friend {friend_field} bubble {sighting(f, tank.bubble)} reef {sighting(f, tank.reef)} "
        f"wall {wall_field(f)} last {f.goal} time {'night' if tank.night else 'day'}"
    )


# ---------------------------------------------------------------- schema L1
# The lizard line (model/schema-lizard.md). The model never learns a species:
# a pack (docs/species-pack.md) turns degrees C into `heat` 0-9 and its rates
# drive the sim. encode_l1_snap() is THE Python L1 encoder: the parity fixture
# (make_l1_parity.py -> testdata/l1_parity.jsonl) and the trace sim both go
# through it, and common/llm/advisor_core.c must match it byte for byte (R4).
GOALS_L1 = ["hunt", "bask", "hide", "explore", "investigate", "dig", "rest"]
STAGES_L1 = ["hatch", "juv", "adult", "elder"]
SHED_L1 = ["no", "soon", "yes"]
PHASES_L1 = ["active", "settle", "sleep"]
L1_FIELDS = ["zone", "hunger", "energy", "stress", "curiosity", "heat", "bold", "social", "stage",
             "trust", "bored", "shed", "prey", "other", "warm", "shelter", "wall", "last", "phase"]
L1_DRIVES = ["hunger", "energy", "stress", "curiosity", "trust", "bored"]   # floats 0..10 -> drive9
L1_SIGHTS = ["prey", "other", "warm", "shelter"]


@dataclasses.dataclass(frozen=True)
class Pack:
    """The part of species_t (docs/species-pack.md "The contract") the model
    pipeline reads: the heat band, the schedule and the rates. Units as there:
    degrees C, seconds of tended time, 0..10 drives."""
    id: int
    name: str
    settle_s: float                 # SCHED_DAY: settle = the first settle_s after lights-out
    t_cold: float
    t_lo: float
    t_hi: float
    t_hot: float
    tau_s: tuple                    # body warms/cools toward its surroundings, per stage
    hunger_per_s: float
    energy_per_s: float
    energy_rest_per_s: float
    curiosity_per_s: float
    hunger_per_cricket: float
    sleep_hunger_per_h: float       # asleep: hunger up, energy up, stress DOWN by these
    sleep_energy_per_h: float
    sleep_stress_per_h: float       # (a magnitude: the validation wants every rate > 0)
    stage_s: tuple                  # tended age for juv, adult, elder (each starts a growth shed)
    pop_cap: int
    shed_every_s: tuple             # per stage: end of one shed -> next pre-shed
    shed_soon_s: float
    shed_window_s: float


GECKO = Pack(id=1, name="LEOPARD GECKO", settle_s=600.0,
             t_cold=20.0, t_lo=28.0, t_hi=32.0, t_hot=38.0, tau_s=(60.0, 90.0, 120.0, 150.0),
             hunger_per_s=0.0045, energy_per_s=0.006, energy_rest_per_s=0.05, curiosity_per_s=0.004,
             hunger_per_cricket=3.0, sleep_hunger_per_h=0.6, sleep_energy_per_h=2.0, sleep_stress_per_h=2.0,
             stage_s=(1200.0, 5400.0, 28800.0), pop_cap=3,
             shed_every_s=(900.0, 1800.0, 10800.0, 21600.0), shed_soon_s=300.0, shed_window_s=600.0)
# A16's test pack: the gecko with its comfort band 6 C warmer. Same lines, same
# model; only the normalisation differs (so the line distribution must not).
TEST_WARM = dataclasses.replace(GECKO, id=250, name="TEST PACK +6C",
                                t_cold=26.0, t_lo=34.0, t_hi=38.0, t_hot=44.0)
PACKS = {p.id: p for p in (GECKO, TEST_WARM)}


def heat_of(pack, body_c):
    """body temperature (C) -> the model's heat as a float 0..10, before drive9
    (docs/species-pack.md "Heat"): 0 at t_cold, 5 at t_lo, 7 at t_hi, 10 at t_hot."""
    p = PACKS[pack] if isinstance(pack, int) else pack
    if body_c <= p.t_cold:
        return 0.0
    if body_c < p.t_lo:
        return 5.0 * (body_c - p.t_cold) / (p.t_lo - p.t_cold)
    if body_c < p.t_hi:
        return 5.0 + 2.0 * (body_c - p.t_lo) / (p.t_hi - p.t_lo)
    if body_c < p.t_hot:
        return 7.0 + 3.0 * (body_c - p.t_hi) / (p.t_hot - p.t_hi)
    return 10.0


def body_c_of(pack, heat):
    """the inverse of heat_of on 0..10 (the sim and the fixture aim at a heat digit with it)"""
    p = PACKS[pack] if isinstance(pack, int) else pack
    heat = max(0.0, min(10.0, heat))
    if heat < 5.0:
        return p.t_cold + heat / 5.0 * (p.t_lo - p.t_cold)
    if heat < 7.0:
        return p.t_lo + (heat - 5.0) / 2.0 * (p.t_hi - p.t_lo)
    return p.t_hi + (heat - 7.0) / 3.0 * (p.t_hot - p.t_hi)


def drive9(v):
    """float 0..10 -> digit 0..9: truncate and clamp (advisor_core.c drive9)"""
    return 0 if v < 0 else 9 if v > 9 else int(v)


def trait9(v):
    """trait 0..1 -> digit 0..9, rounded as the C does: (int)(v * 9 + 0.5)"""
    return int(v * 9 + 0.5)


def clock_at(x, y, heading, ox, oy):
    """clock position of (ox, oy) seen from (x, y) facing `heading`; 12 = dead
    ahead. The C's arithmetic (clock_dir): wrap up into [0, 2pi), round half up."""
    rel = math.atan2(oy - y, ox - x) - heading
    while rel < 0:
        rel += 2 * math.pi
    hour = int(rel / (2 * math.pi) * 12 + 0.5) % 12
    return 12 if hour == 0 else hour


def sight_at(x, y, heading, pt):
    """`<bucket> <clock>` or `none` (absent, or beyond far)"""
    if pt is None:
        return "none"
    b = bucket(math.hypot(pt[0] - x, pt[1] - y))
    return "none" if b is None else f"{b} {clock_at(x, y, heading, pt[0], pt[1])}"


def encode_l1_snap(s):
    """The L1 state line of one lizard snapshot (the parity fixture's `snap`).

    s: pack (id), zone (1-6, precomputed), x, y, heading (radians), body_c (C),
    hunger energy stress curiosity trust bored (0..10), bold social (0..1),
    stage, shed, last, phase (words), prey other warm shelter ([x, y] or None),
    in_shelter (bool: shelter reads `near 12` whatever the point), wall (the
    nearest glass contact point or None: shown only within mid, else `clear`)."""
    x, y, h = s["x"], s["y"], s["heading"]
    wall = s["wall"]
    wall_s = "clear"
    if wall is not None:
        b = bucket(math.hypot(wall[0] - x, wall[1] - y))
        if b in ("near", "mid"):
            wall_s = f"{b} {clock_at(x, y, h, wall[0], wall[1])}"
    shelter_s = "near 12" if s["in_shelter"] else sight_at(x, y, h, s["shelter"])
    return (
        f"zone {s['zone']} hunger {drive9(s['hunger'])} energy {drive9(s['energy'])} "
        f"stress {drive9(s['stress'])} curiosity {drive9(s['curiosity'])} "
        f"heat {drive9(heat_of(s['pack'], s['body_c']))} "
        f"bold {trait9(s['bold'])} social {trait9(s['social'])} stage {s['stage']} "
        f"trust {drive9(s['trust'])} bored {drive9(s['bored'])} shed {s['shed']} "
        f"prey {sight_at(x, y, h, s['prey'])} other {sight_at(x, y, h, s['other'])} "
        f"warm {sight_at(x, y, h, s['warm'])} shelter {shelter_s} wall {wall_s} "
        f"last {s['last']} phase {s['phase']}"
    )


_DIGITS = {str(i) for i in range(10)}
_CLOCKS = {str(i) for i in range(1, 13)}
_L1_VALUES = {"zone": {str(i) for i in range(1, 7)}, "stage": set(STAGES_L1), "shed": set(SHED_L1),
              "last": set(GOALS_L1), "phase": set(PHASES_L1)}


def check_l1_line(line):
    """[] if `line` is a well-formed L1 state line - every field in order, every
    value in its domain, single spaces - else the list of problems."""
    w = line.split(" ")
    if "" in w:
        return ["empty word: double, leading or trailing space"]
    i = 0
    for key in L1_FIELDS:
        if i >= len(w) or w[i] != key:
            return [f"word {i}: expected '{key}', got {w[i] if i < len(w) else 'end of line'!r}"]
        v = w[i + 1] if i + 1 < len(w) else None
        nxt = w[i + 2] if i + 2 < len(w) else None
        i += 2
        if key in _L1_VALUES or key in L1_DRIVES or key in ("heat", "bold", "social"):
            ok = v in _L1_VALUES.get(key, _DIGITS)
        elif key in L1_SIGHTS:
            ok = v == "none" or (v in ("near", "mid", "far") and nxt in _CLOCKS)
            i += v != "none"
        else:   # wall
            ok = v == "clear" or (v in ("near", "mid") and nxt in _CLOCKS)
            i += v != "clear"
        if not ok:
            return [f"{key}: bad value {v!r}" + (f" {nxt!r}" if key in L1_SIGHTS or key == "wall" else "")]
    return [] if i == len(w) else [f"{len(w) - i} trailing words"]


def check_l1_goal(goal):
    """[] if `goal` is `<goal> urgency <0-9>` with one of the seven goals"""
    w = goal.split(" ")
    ok = len(w) == 3 and w[0] in GOALS_L1 and w[1] == "urgency" and w[2] in _DIGITS
    return [] if ok else [f"bad goal line {goal!r}"]


def parse_l1_line(line):
    """an L1 line back into fields: digits as ints, words as words, the
    sightings and the wall as (bucket, clock) or None"""
    w = line.split()
    d, i = {}, 0
    while i < len(w):
        k, v = w[i], w[i + 1]
        if k in L1_SIGHTS or k == "wall":
            if v in ("none", "clear"):
                d[k], i = None, i + 2
            else:
                d[k], i = (v, int(w[i + 2])), i + 3
        else:
            d[k], i = (int(v) if v.isdigit() else v), i + 2
    return d


# ---- the L1 sim: a headless terrarium with enough dynamics to sweep the line's
# state space and to let the teacher's decisions play out. Its WORLD is the C
# engine's as built (P2a, 2026-11-04), so the night's states are the device's:
#  - the four boards' glass and floor (tank.h TANK_W / TANK_BOT; tank.c
#    tank_glass_x0 / x1 / top / clamp: a rectangle, round corners, the bowl's circle);
#  - the desert habitat in absolute degrees C for every pack (common/habitat/desert.c),
#    laid out on the floor line (tank.h PLACE_Y = TANK_BOT - 24, PLACE_MARGIN, HIDE_R)
#    and read as tank.c tank_env_c_lit reads it; the body follows it (tank.c body_follow);
#  - heat's effects (tank.h HEAT_SLOW, HEAT_HOT), the stuck shed's stress floor and the
#    shed and the humid hide's drying on tended time (tank.c stuck_floor, shed_step,
#    tank_shed_clock);
#  - the zone grid and the wall's contact point (advisor_core.c zone_of, wall_contact).
# Its lizards roam the whole glass, as P2a's do (they still swim like the fish; P2b
# brings walking), and run the pack's drive rates (the engine's from P2b on).
# Units: seconds, degrees C, 0..10 drives, px.
L1_DT = 1.0                       # s per sim tick
L1_ASK_EVERY_S = 20               # default s of sim time between two asks of the same lizard
# boredom per SECOND (common/tank.h BORED_*; the fish sim above runs in frames)
BORED_PER_S, BORED_ZONE_STALE_S, BORED_RELIEF_PER_S = 0.15, 20.0, 0.3
# the shed (tank.h SHED_SOAK_S, SHED_DAMP, STUCK_*, MIST_PASS; progression.h SLEEP_GROWTH_FRAC)
SHED_SOAK_S, SHED_DAMP, STUCK_STRESS_PER_S, STUCK_STRESS_MAX = 60.0, 0.35, 0.005, 6.0
MIST_PASS, SLEEP_GROWTH_FRAC = 0.35, 0.25
# the places on the floor line (tank.h: PLACE_Y = TANK_BOT - PLACE_DY) and a hide's reach (HIDE_R)
PLACE_DY, PLACE_MARGIN, HIDE_R = 24.0, 30.0, 28.0
# heat's effects (tank.h): under HEAT_SLOW it is slower (x HEAT_SLOW_MIN at heat 0); from
# HEAT_HOT on, stress is pulled toward 4 + 2 x (heat - 8) at HEAT_HOT_PULL a second
HEAT_SLOW, HEAT_SLOW_MIN, HEAT_HOT, HEAT_HOT_PULL = 3.0, 0.5, 8.0, 0.15
L1_LEISURE = ("bask", "explore", "investigate", "dig")   # + rest and hide in the active phase
# keepers: feed every s, crickets per lizard per feed, mist rate /s (a drying hide, by day),
# soak rate /s (a stuck shed)
KEEPERS = {"diligent": (240.0, (2, 3), 1 / 400, 1 / 300),
           "ok": (450.0, (1, 3), 1 / 1500, 1 / 900),
           "neglectful": (1200.0, (1, 2), 1 / 8000, 0.0)}
# edge cases pushed on a lizard before sampling (weights by repetition, as the fish's)
L1_CASES = (["cold"] * 5 + ["hot"] * 4 + ["starve"] * 4 + ["starve_cold"] * 2 + ["famine"]
            + ["shed_soon"] + ["shed_yes"] + ["stuck"] + ["night"] + ["stress"] * 3 + ["bored"] * 3
            + ["fresh"] + ["territorial"] * 2 + ["social"] + ["lonely"] + ["persona"] * 3 + ["exhausted"]
            + ["sated"] * 2 + ["curious"] + ["in_hide"] + ["wall"] + ["prey_near"] * 2)
# the packs a terrarium holds: the gecko, now and then A16's test pack - in the same desert
# at the same absolute temperatures, so it is cold everywhere but in the lamp's pool
L1_PACK_MIX = ((GECKO, 85), (TEST_WARM, 15))


@dataclasses.dataclass(frozen=True)
class Board:
    """One board's world (tank.h TANK_W, TANK_H, TANK_BOT; tank.c tank_glass_*):
    the glass is the frame's rectangle, the rectangle with round corners
    (TANK_CORNER_R) or the bowl's circle (TANK_RAD), the floor at TANK_BOT."""
    name: str
    w: float
    h: float
    bot: float
    corner_r: float = 0.0
    rad: float = 0.0

    def _half(self, d):                 # the bowl: half the chord d px off the centre line (glass_half)
        r2 = self.rad * self.rad - d * d
        return math.sqrt(r2) if r2 > 0 else 0.0

    def _inset(self, v, length):        # the corners: the glass's inset v px along a side (corner_inset)
        r = self.corner_r
        d = r - v if v < r else v - (length - r) if v > length - r else 0.0
        if d <= 0:
            return 0.0
        return r if d >= r else r - math.sqrt(r * r - d * d)

    def x0(self, y):
        return self.rad - self._half(y - self.rad) if self.rad else self._inset(y, self.h) if self.corner_r else 0.0

    def x1(self, y):
        return self.rad + self._half(y - self.rad) if self.rad else \
            self.w - self._inset(y, self.h) if self.corner_r else self.w

    def top(self, x):
        return self.rad - self._half(x - self.rad) if self.rad else self._inset(x, self.w) if self.corner_r else 0.0

    def clamp(self, x, y, m):
        """tank_glass_clamp: (x, y) brought to at least m inside the glass and above the floor"""
        if self.rad:
            y = min(y, self.bot - m)
            dx, dy = x - self.rad, y - self.rad
            d, lim = math.hypot(dx, dy), self.rad - m
            if d > lim and d > 0:
                x, y = self.rad + dx * lim / d, self.rad + dy * lim / d
                if y > self.bot - m:            # where the glass meets the floor: along the floor to the glass
                    y = self.bot - m
                    half = max(0.0, self._half(y - self.rad) - m)
                    x = self.rad - half if dx < 0 else self.rad + half
            return x, y
        x, y = min(self.w - m, max(m, x)), min(self.h - m, max(m, y))
        if self.corner_r:                       # out of a corner: back along its radius (corner_clamp)
            r = self.corner_r
            cx = r if x < r else self.w - r if x > self.w - r else None
            cy = r if y < r else self.h - r if y > self.h - r else None
            if cx is not None and cy is not None:
                d, lim = math.hypot(x - cx, y - cy), r - m
                if d > lim and d > 0:
                    x, y = cx + (x - cx) * lim / d, cy + (y - cy) * lim / d
        return x, y


# the four boards (docs/BOARDS.md; tank.h), weighted toward the 1.8
L1_BOARDS = ((Board("1.8", 448, 368, 368), 50), (Board("square", 480, 480, 480, corner_r=40), 20),
             (Board("watch", 410, 502, 502, corner_r=100), 15), (Board("round", 466, 466, 396, rad=233), 15))


@dataclasses.dataclass(frozen=True)
class Habitat:
    """habitat_t (common/habitat.h): lit, the floor runs straight from floor_warm_c at
    its warm (left, u 0) end to floor_cool_c at its cool end, and the lamp's pool
    rises to bask_c at the basking spot from the floor's own at bask_r_px; dark, the
    floor is night_drop_c cooler and the lamp is out. hides: (kind, u, lit C, dark C)."""
    id: int
    name: str
    floor_warm_c: float
    floor_cool_c: float
    night_drop_c: float
    bask_u: float
    bask_c: float
    bask_r_px: float
    hides: tuple
    moist_start: float
    moist_dry_per_s: float


DESERT = Habitat(id=1, name="DESERT", floor_warm_c=31.0, floor_cool_c=25.0, night_drop_c=5.0,   # habitat/desert.c
                 bask_u=0.28, bask_c=35.0, bask_r_px=70.0,
                 hides=(("warm", 0.0, 30.0, 30.0), ("humid", 0.55, 28.0, 25.0), ("cool", 1.0, 24.0, 21.0)),
                 moist_start=0.6, moist_dry_per_s=1.0 / 10800)


def _trait(rng, digit=None):
    """a 0..1 trait that encodes as `digit` (uniform over 0-9 unless given)"""
    d = rng.randint(0, 9) if digit is None else digit
    return min(1.0, max(0.0, (d + rng.uniform(-0.45, 0.45)) / 9))


def l1_zone(x, y, w, bot):
    """advisor_core.c zone_of: the 3x2 grid, columns by TANK_W, rows by TANK_BOT"""
    col = min(2, int(x / (w / 3)))
    row = min(1, int(y / (bot / 2)))
    return row * 3 + col + 1


class Lizard:
    def __init__(self, terr, rng):
        self.x, self.y = terr.clamp(rng.uniform(30, terr.w - 30), rng.uniform(30, terr.bot - 30))
        self.heading = rng.uniform(0, 2 * math.pi)
        self.reroll_identity(terr, rng)
        self.body_c = (terr.pack.t_lo + terr.pack.t_hi) / 2    # a new arrival: mid-band (tank.c tank_lizard_fresh)
        self.hunger = rng.uniform(1, 7)
        self.energy = rng.uniform(3, 9)
        self.stress = rng.uniform(0, 3)
        self.curiosity = rng.uniform(2, 8)
        self.bored = rng.uniform(0, 3)
        self.goal, self.goal_prev, self.urgency = rng.choice(GOALS_L1), None, 5
        self.hide = terr.hide_at(self.x, self.y)    # the hide it is inside: within HIDE_R of its point (tank_hide_at)
        self.want_hide = None       # the hide it is heading for
        self.target, self.target_t = None, 0.0     # explore point / investigate subject
        self.last_prey = None
        self.strike_cd = 0.0
        self.zone_last, self.zone_seen = None, {}
        self.shed = "normal"        # normal | soon | shedding | stuck  (the line: no | soon | yes | yes)
        self.shed_t = self.damp_s = 0.0
        self.growth = False         # this shed is a growth shed: the stage changes when it comes off clean
        self.since_shed = rng.uniform(0, terr.pack.shed_every_s[self.stage_i])

    @property
    def stage(self):
        return STAGES_L1[self.stage_i]

    def reroll_identity(self, terr, rng):
        """traits uniform over their digits, stage weighted toward adult, an age inside the stage"""
        self.bold, self.social = _trait(rng), _trait(rng)
        self.trust = rng.uniform(0, 10)
        self.stage_i = rng.choices(range(4), weights=[15, 20, 45, 20])[0]
        edges = (0.0,) + terr.pack.stage_s + (terr.pack.stage_s[-1] * 1.5,)
        self.age = rng.uniform(edges[self.stage_i], edges[self.stage_i + 1])

    def set_goal(self, goal, urgency=5):
        """the advisor or the reflex changed the goal: a genuinely new one relieves
        boredom; bouncing back to the one it just left does not (tank.c)"""
        self.urgency = urgency
        if goal == self.goal:
            return
        if goal != self.goal_prev:
            self.bored = max(0.0, self.bored - BORED_NEW_GOAL)
        self.goal_prev, self.goal = self.goal, goal
        self.target, self.want_hide = None, None

    def dist(self, p):
        return math.hypot(p[0] - self.x, p[1] - self.y)

    def tick(self, terr, rng, dt, ph):
        p = terr.pack
        self._move(terr, rng, dt)
        self.hide = terr.hide_at(self.x, self.y)
        # the body follows where it is, exactly over dt at its stage's tau (tank.c body_follow):
        # the habitat's degrees C there with the light as it is (tank_env_c)
        env = terr.env_c(self.x, self.y, terr.light)
        self.body_c = env + (self.body_c - env) * math.exp(-dt / p.tau_s[self.stage_i])
        heat = heat_of(p, self.body_c)
        asleep = ph == "sleep"
        if asleep:          # (a shed's slower hunger is its appetite awake: tank.c tank_tick_sleep)
            self.hunger += p.sleep_hunger_per_h / 3600 * dt
            self.energy += p.sleep_energy_per_h / 3600 * dt
            self.stress -= p.sleep_stress_per_h / 3600 * dt
        else:
            slow = 0.5 if self.shed in ("soon", "shedding") else 1.0     # tank.h SHED_HUNGER_X: off its food
            self.hunger += p.hunger_per_s * dt * slow
            if self.goal == "rest":
                self.energy += p.energy_rest_per_s * dt * min(1.0, max(0.3, heat / 5))   # cold slows recovery
            else:
                self.energy -= p.energy_per_s * dt * (1.6 if self.goal in ("dig", "hunt") else 1.0)
            self.curiosity += dt * (-0.012 if self.goal in ("explore", "investigate") else p.curiosity_per_s)
            self.stress -= dt * (0.01 + 0.002 * self.trust + (0.03 if self.hide else 0.0))   # calms; faster when hidden
            to = 4 + 2 * (heat - HEAT_HOT)                                # overheating pulls stress up (tank.c)
            if heat >= HEAT_HOT and self.stress < to:
                self.stress += HEAT_HOT_PULL * (to - self.stress) * dt
            for o in terr.lizards:
                if o is not self and self.dist((o.x, o.y)) < 60:
                    self.stress += dt * 0.03 * (1 - self.social)      # crowding; worse for the territorial
            if rng.random() < dt / 1200:
                self.stress += rng.uniform(1.5, 4) * (1.3 - self.bold)    # a rap on the glass
        self._shed_tick(terr, rng, dt * (SLEEP_GROWTH_FRAC if asleep else 1.0))
        if self.shed == "stuck":            # a stuck shed's floor under the stress (tank.c stuck_floor)
            self.stress = max(self.stress, min(STUCK_STRESS_MAX, STUCK_STRESS_PER_S * self.shed_t))
        # boredom: a pastime goes stale; a meal or the night's rest never does;
        # a zone not seen for a while is a small relief (tank.c)
        leisure = self.goal in L1_LEISURE or (self.goal in ("rest", "hide") and ph == "active")
        self.bored += dt * (BORED_PER_S if leisure else -BORED_RELIEF_PER_S)
        z = l1_zone(self.x, self.y, terr.w, terr.bot)
        if z != self.zone_last:
            if self.zone_last is not None and terr.clock - self.zone_seen.get(z, -1e9) > BORED_ZONE_STALE_S:
                self.bored -= BORED_NEW_ZONE
            self.zone_last = z
        self.zone_seen[z] = terr.clock
        for k in ("hunger", "energy", "stress", "curiosity", "bored", "trust"):
            setattr(self, k, min(10.0, max(0.0, getattr(self, k))))
        self._reflex(terr, rng, dt, ph, heat)

    def _shed_tick(self, terr, rng, tdt):
        """docs/shed.md on tended time: normal -> soon -> shedding -> clean, or
        stuck until a damp stay or a soak; reaching the next stage's age starts a
        growth shed at once (or makes the shed under way the growth shed)"""
        p = terr.pack
        self.age += tdt
        reached = self.stage_i < 3 and self.age >= p.stage_s[self.stage_i]
        if self.shed == "normal":
            self.since_shed += tdt
            if reached or self.since_shed >= p.shed_every_s[self.stage_i]:
                self.shed, self.shed_t, self.growth = "soon", 0.0, reached
            return
        self.growth = self.growth or reached
        self.shed_t += tdt
        if self.shed == "soon":
            if self.shed_t >= p.shed_soon_s:
                self.shed, self.shed_t, self.damp_s = "shedding", 0.0, 0.0
            return
        if self.hide == "humid" and terr.moisture >= SHED_DAMP:
            self.damp_s += tdt
        soak = self.shed == "stuck" and rng.random() < terr.soak_rate * tdt
        if self.damp_s >= SHED_SOAK_S or soak:
            self.shed, self.since_shed, self.shed_t, self.damp_s = "normal", 0.0, 0.0, 0.0
            if self.growth:
                self.stage_i = min(3, self.stage_i + 1)
            self.growth = False
            if soak:        # the mister: a little stress, a little trust from a hand it knows
                self.stress = min(10.0, self.stress + 1.0)
                if self.trust >= 6:
                    self.trust = min(10.0, self.trust + 0.2)
        elif self.shed == "shedding" and self.shed_t >= p.shed_window_s:
            self.shed, self.shed_t, self.damp_s = "stuck", 0.0, 0.0    # (shed_t: tended seconds stuck so far)

    def _step_toward(self, terr, tx, ty, step, rng=None):
        self.heading = math.atan2(ty - self.y, tx - self.x) % (2 * math.pi)
        if rng is not None:
            self.heading = (self.heading + rng.gauss(0, 0.05)) % (2 * math.pi)
        self.x, self.y = terr.clamp(self.x + math.cos(self.heading) * step, self.y + math.sin(self.heading) * step)

    def _explore_point(self, terr, rng):
        """a point in one of the two zones seen least recently"""
        z = rng.choice(sorted(range(1, 7), key=lambda k: self.zone_seen.get(k, -1e9))[:2])
        col, row = (z - 1) % 3, (z - 1) // 3
        return terr.clamp(rng.uniform(col * terr.w / 3 + 10, (col + 1) * terr.w / 3 - 10),
                          rng.uniform(row * terr.bot / 2 + 10, (row + 1) * terr.bot / 2 - 10))

    def _move(self, terr, rng, dt):
        """the reflex layer performing the goal (model/schema-lizard.md "Goal line")"""
        heat = heat_of(terr.pack, self.body_c)
        speed = 20.0 * (0.6 + self.urgency / 12.0)
        if heat < HEAT_SLOW:                                    # cold: sluggish (tank.c, x0.5 at heat 0)
            speed *= HEAT_SLOW_MIN + (1 - HEAT_SLOW_MIN) * heat / HEAT_SLOW
        self.strike_cd = max(0.0, self.strike_cd - dt)
        g = self.goal
        tgt, stop = None, 0.0
        if g == "hunt":
            prey = terr.nearest(self, terr.prey_points())
            if prey is not None and self.dist(prey) < FAR:
                self.last_prey = prey
                if self.dist(prey) < 16:
                    self._strike(terr, rng, prey, heat)
                    return
                tgt = prey
                speed *= 0.5 if self.dist(prey) < 45 else 1.0     # stalk the last stretch
            else:                                                 # nothing in sight: where food was
                if self.last_prey is None or self.dist(self.last_prey) < 10:
                    self.last_prey = self._explore_point(terr, rng)
                tgt, speed = self.last_prey, speed * 0.5
        elif g == "bask":
            tgt, stop = terr.bask, 6.0
        elif g == "hide":           # in once within HIDE_R of the hide's point; it settles at the middle
            if self.want_hide is None:      # humid if shedding, cool if hot, else warm
                self.want_hide = "humid" if self.shed != "normal" else "cool" if heat >= 7 else "warm"
            tgt, stop = terr.hides[self.want_hide], 2.0
        elif g == "explore":
            if self.target is None or isinstance(self.target, Lizard) or terr.clock > self.target_t \
                    or self.dist(self.target) < 6:
                self.target, self.target_t = self._explore_point(terr, rng), terr.clock + 40
            tgt, speed, stop = self.target, speed * 0.6, 5.0
        elif g == "investigate":
            if self.target is None:
                others = [o for o in terr.lizards if o is not self and self.dist((o.x, o.y)) < FAR]
                o = min(others, key=lambda q: self.dist((q.x, q.y))) if others else None
                self.target = o if o is not None and rng.random() < 0.3 + 0.6 * self.social \
                    else rng.choice(terr.decor)
            if isinstance(self.target, Lizard):
                tgt = (self.target.x, self.target.y)
                stop = 75.0 if self.social < 0.25 else 22.0      # the territorial: a stand-off, a tail wave
                if self.social < 0.25 and self.dist(tgt) < 90:
                    self.stress += 0.04 * dt
                    self.target.stress += 0.04 * dt
            else:
                tgt, stop = self.target, 20.0
            speed *= 0.7
        if tgt is None or self.dist(tgt) <= stop + 0.5:   # dig, rest, or there: in place, the head turns now and then
            if rng.random() < dt / 8:
                self.heading = (self.heading + rng.uniform(-0.8, 0.8)) % (2 * math.pi)
            return
        self._step_toward(terr, tgt[0], tgt[1], min(speed * dt, self.dist(tgt) - stop), rng)

    def _strike(self, terr, rng, prey, heat):
        """a lunge at the prey in reach: cold spoils it, a pre-shed lizard often doesn't try"""
        if self.strike_cd > 0:
            return
        self.strike_cd = 2.5
        if self.shed in ("soon", "shedding") and rng.random() < 0.5:
            return
        p_hit = 0.6 * min(1.0, heat / 3.0)
        if terr.dish is not None and prey == terr.dish:             # mealworms in a dish don't run
            if rng.random() < 1.5 * p_hit:
                terr.worms -= 1
                self.hunger = max(0.0, self.hunger - terr.pack.hunger_per_cricket)
            return
        c = min(terr.crickets, key=lambda q: math.hypot(q[0] - prey[0], q[1] - prey[1]))
        if rng.random() < p_hit:
            terr.crickets.remove(c)
            self.hunger = max(0.0, self.hunger - terr.pack.hunger_per_cricket)
        else:
            c[2] = 0.0                                              # missed: it hops off at once

    def _reflex(self, terr, rng, dt, ph, heat):
        """goal changes between advisor calls (tank.c's urgent paths), rare on purpose"""
        prey = terr.nearest(self, terr.prey_points())
        if self.energy < 0.8 and self.goal != "rest" and rng.random() < 0.02 * dt:
            self.set_goal("rest", 6)
        elif (self.stress >= 8 or heat >= 9) and self.goal != "hide" and rng.random() < 0.03 * dt:
            self.set_goal("hide", 8)
        elif self.hunger >= 7 and heat >= 3 and prey is not None and self.dist(prey) < NEAR \
                and self.goal != "hunt" and rng.random() < 0.04 * dt:
            self.set_goal("hunt", 7)
        elif rng.random() < 0.002 * dt:
            self.set_goal(rng.choice(GOALS_L1), rng.randint(2, 7))


class Terrarium:
    """Minimal headless terrarium on one of the four boards: a pack in the desert
    habitat (absolute degrees C), its three hides and basking spot on the floor
    line, crickets, a keeper, 1-3 lizards."""

    def __init__(self, rng, pack=None, n_lizards=None, board=None):
        self.rng = rng
        self.pack = PACKS[pack] if pack is not None else \
            rng.choices([q for q, _ in L1_PACK_MIX], weights=[k for _, k in L1_PACK_MIX])[0]
        self.board = board or rng.choices([b for b, _ in L1_BOARDS], weights=[k for _, k in L1_BOARDS])[0]
        self.w, self.bot = self.board.w, self.board.bot
        self.habitat = DESERT
        # the floor line and its places (tank.c floor_span, tank_floor_x, tank_hide_point, tank_bask_point)
        self.place_y = self.bot - PLACE_DY
        self.fx0 = self.board.x0(self.place_y) + PLACE_MARGIN
        self.fx1 = self.board.x1(self.place_y) - PLACE_MARGIN
        self.hides = {kind: (self.floor_x(u), self.place_y) for kind, u, _, _ in self.habitat.hides}
        self.hide_c = {kind: (lit, dark) for kind, _, lit, dark in self.habitat.hides}
        self.bask = (self.floor_x(self.habitat.bask_u), self.place_y)
        self.decor = [self.clamp(rng.uniform(30, self.w - 30), rng.uniform(30, self.bot - 30), 20.0)
                      for _ in range(rng.randint(2, 4))]
        self.dish = self.clamp(rng.uniform(40, self.w - 40), rng.uniform(40, self.bot - 40), 30.0) \
            if rng.random() < 0.4 else None
        self.worms = rng.randint(0, 6) if self.dish else 0
        self.crickets = []      # [x, y, seconds to the next hop]
        self.moisture = self.habitat.moist_start       # the humid hide: a new terrarium's (habitat moist_start)
        self.keeper = rng.choices(list(KEEPERS), weights=[45, 40, 15])[0]
        self.feed_every, self.feed_n, self.mist_rate, self.soak_rate = KEEPERS[self.keeper]
        self.clock = 0.0
        self.feed_at = rng.uniform(0, self.feed_every)
        self.light = rng.random() < 0.8
        if self.light:
            self.light_t, self.switch_at = rng.uniform(0, 4000), rng.uniform(300, 5000)
        else:
            self.light_t = rng.uniform(0, self.pack.settle_s + 500)
            self.switch_at = max(0.0, self.pack.settle_s - self.light_t) + rng.uniform(60, 600)
        n = n_lizards or rng.choices([1, 2, 3], weights=[25, 40, 35])[0]
        self.lizards = [Lizard(self, rng) for _ in range(min(n, self.pack.pop_cap))]

    def phase(self):
        """SCHED_DAY (docs/species-pack.md): active while the light is on, settle
        for the first settle_s of the dark, sleep for the rest of it"""
        if self.light:
            return "active"
        return "settle" if self.light_t < self.pack.settle_s else "sleep"

    def floor_x(self, u):
        return self.fx0 + (self.fx1 - self.fx0) * u

    def floor_u(self, x):
        return min(1.0, max(0.0, (x - self.fx0) / (self.fx1 - self.fx0)))

    def clamp(self, x, y, m=6.0):
        return self.board.clamp(x, y, m)

    def inside(self, x, y, m):
        cx, cy = self.board.clamp(x, y, m)
        return abs(cx - x) < 1e-9 and abs(cy - y) < 1e-9

    def hide_at(self, x, y):
        """tank.c tank_hide_at: the first hide whose point is within HIDE_R, else None"""
        for kind, (hx, hy) in self.hides.items():
            if math.hypot(x - hx, y - hy) < HIDE_R:
                return kind
        return None

    def env_c(self, x, y, lit):
        """tank.c tank_env_c_lit: degrees C at (x, y) - inside a hide its own; else the
        floor's warm-to-cool line, the night's drop when dark, the lamp's pool when lit"""
        k = self.hide_at(x, y)
        if k is not None:
            return self.hide_c[k][0] if lit else self.hide_c[k][1]
        h = self.habitat
        c = h.floor_warm_c + (h.floor_cool_c - h.floor_warm_c) * self.floor_u(x)
        if not lit:
            return c - h.night_drop_c
        d, r = math.hypot(x - self.bask[0], y - self.bask[1]), h.bask_r_px
        if d < r:
            c += (h.bask_c - c) * (1 - d * d / (r * r))
        return c

    def prey_points(self):
        pts = [(c[0], c[1]) for c in self.crickets]
        if self.dish is not None and self.worms > 0:
            pts.append(self.dish)
        return pts

    @staticmethod
    def nearest(lz, pts):
        return min(pts, key=lz.dist) if pts else None

    def tick(self, dt=L1_DT):
        rng = self.rng
        self.clock += dt
        self.light_t += dt
        if self.clock >= self.switch_at:        # the light flips; nights are ~15% of the time
            self.light, self.light_t = not self.light, 0.0
            self.switch_at = self.clock + (rng.uniform(2400, 5400) if self.light
                                           else self.pack.settle_s + rng.uniform(120, 900))
        ph = self.phase()
        # the keeper: crickets by day, a mist when the humid hide dries, a soak for a stuck shed
        if self.light and self.clock >= self.feed_at:
            self.feed_at = self.clock + self.feed_every * rng.uniform(0.6, 1.4)
            for _ in range(min(8 - len(self.crickets), rng.randint(*self.feed_n) * len(self.lizards))):
                cx, cy = self.clamp(rng.uniform(20, self.w - 20), rng.uniform(20, self.bot - 20), 8.0)
                self.crickets.append([cx, cy, rng.uniform(1, 5)])
            if self.dish is not None and rng.random() < 0.3:
                self.worms = min(8, self.worms + rng.randint(2, 5))
        # the humid hide dries on tended time (tank.c tank_shed_clock: asleep at SLEEP_GROWTH_FRAC)
        self.moisture = max(0.0, self.moisture - self.habitat.moist_dry_per_s * dt
                            * (SLEEP_GROWTH_FRAC if ph == "sleep" else 1.0))
        if self.light and self.moisture < 0.6 and rng.random() < self.mist_rate * dt:
            self.moisture = min(1.0, self.moisture + MIST_PASS)
        # crickets hop, flee a hunter, and now and then slip away into the decor
        for c in list(self.crickets):
            if rng.random() < dt / 1200.0:
                self.crickets.remove(c)
                continue
            c[2] -= dt
            if c[2] <= 0:
                hunter = next((lz for lz in self.lizards if lz.goal == "hunt" and lz.dist(c) < 50), None)
                if hunter is not None:
                    a, d = math.atan2(c[1] - hunter.y, c[0] - hunter.x) + rng.uniform(-0.6, 0.6), rng.uniform(20, 50)
                else:
                    a, d = rng.uniform(0, 2 * math.pi), rng.uniform(8, 35)
                c[0], c[1] = self.clamp(c[0] + math.cos(a) * d, c[1] + math.sin(a) * d, 8.0)
                c[2] = rng.uniform(1.5, 6.0)
        for lz in self.lizards:
            lz.tick(self, rng, dt, ph)

    def _at(self, lz, kind):
        """a point inside the glass `kind` (near/mid/far) away from lz, or None if there is no room there"""
        lo, hi = {"near": (12, 65), "mid": (75, 175), "far": (185, 370)}[kind]
        for _ in range(40):
            d, a = self.rng.uniform(lo, hi), self.rng.uniform(0, 2 * math.pi)
            x, y = lz.x + math.cos(a) * d, lz.y + math.sin(a) * d
            if self.inside(x, y, 8.0):
                return x, y
        return None

    def place_prey(self, lz, kind):
        """the only prey: one cricket `kind` away, or none at all (the dish emptied)"""
        self.crickets.clear()
        self.worms = 0
        pt = self._at(lz, kind) if kind else None
        if pt is not None:
            self.crickets.append([pt[0], pt[1], self.rng.uniform(2, 6)])

    def place_other(self, lz, kind):
        """the nearest other lizard `kind` away, or every other lizard out of sight
        (lz in a corner, they in the opposite one); a lone lizard stays alone"""
        rng = self.rng
        others = [o for o in self.lizards if o is not lz]
        if kind is None:
            cx, cy = rng.choice([0, 1]), rng.choice([0, 1])
            lz.x, lz.y = self.clamp(abs(cx * self.w - rng.uniform(10, 40)), abs(cy * self.bot - rng.uniform(10, 40)))
            for o in others:
                o.x, o.y = self.clamp(abs((1 - cx) * self.w - rng.uniform(10, 40)),
                                      abs((1 - cy) * self.bot - rng.uniform(10, 40)))
            return
        if not others:
            return
        o = min(others, key=lambda q: lz.dist((q.x, q.y)))
        pt = self._at(lz, kind)
        if pt is not None:
            o.x, o.y = pt

    def perturb(self, lz):
        """push one lizard (or its terrarium) into an edge case before sampling"""
        rng, p = self.rng, self.pack
        case = rng.choice(L1_CASES)

        def heat_to(lo, hi):
            lz.body_c = body_c_of(p, rng.uniform(lo, hi))
        if case == "cold":
            heat_to(0.0, 3.99)
        elif case == "hot":
            heat_to(7.0, 10.0)
        elif case == "starve":
            lz.hunger = rng.uniform(8, 10)
            self.place_prey(lz, rng.choice(["near", "near", "mid", "mid", "far", "far", None]))   # (none comes naturally)
        elif case == "starve_cold":      # rule 1's edge: cold and starving, prey near / far / none
            lz.hunger = rng.uniform(8, 10)
            heat_to(0.0, 2.99)
            self.place_prey(lz, rng.choice(["near", "mid", "far", None]))
        elif case == "famine":
            self.place_prey(lz, None)
        elif case == "shed_soon":
            lz.shed, lz.shed_t = "soon", rng.uniform(0, p.shed_soon_s)
        elif case == "shed_yes":
            lz.shed, lz.shed_t, lz.damp_s = "shedding", rng.uniform(0, p.shed_window_s), 0.0
            lz.hunger = rng.uniform(0, 8)
        elif case == "stuck":
            lz.shed, lz.damp_s = "stuck", 0.0
            lz.stress = max(lz.stress, rng.uniform(2, STUCK_STRESS_MAX))
        elif case == "night":            # a short dark spell: settle, or deep in the sleep
            self.light = False
            self.light_t = rng.uniform(0, p.settle_s * 0.9) if rng.random() < 0.5 \
                else p.settle_s + rng.uniform(0, 3600)
            self.switch_at = self.clock + rng.uniform(40, 200)
        elif case == "stress":
            lz.stress = rng.uniform(7, 10)
        elif case == "bored":            # a rut: content, on a pastime for too long
            lz.bored = rng.uniform(4, 10)
            lz.set_goal(rng.choice(L1_LEISURE + ("rest", "hide")), rng.randint(3, 6))
            lz.bored = rng.uniform(4, 10)
            lz.hunger, lz.energy, lz.stress = rng.uniform(0, 6), rng.uniform(4, 10), rng.uniform(0, 4)
            heat_to(4.0, 7.0)
        elif case == "fresh":
            lz.bored, lz.hunger = rng.uniform(0, 1.5), rng.uniform(0, 5)
        elif case == "territorial":
            lz.social = _trait(rng, rng.randint(0, 2))
            self.place_other(lz, rng.choice(["near", "near", "mid"]))
        elif case == "social":
            lz.social = _trait(rng, rng.randint(7, 9))
            self.place_other(lz, rng.choice(["near", "mid"]))
        elif case == "lonely":
            self.place_other(lz, None)
        elif case == "persona":
            lz.reroll_identity(self, rng)
            lz.curiosity = rng.uniform(0, 10)
        elif case == "exhausted":
            lz.energy = rng.uniform(0, 3)
            if rng.random() < 0.3:
                heat_to(0.0, 3.0)
        elif case == "sated":
            lz.hunger, lz.energy, lz.stress = rng.uniform(0, 3), rng.uniform(5, 10), rng.uniform(0, 3)
            lz.curiosity = rng.uniform(0, 10)
            heat_to(4.5, 7.0)
        elif case == "curious":
            lz.curiosity, lz.hunger = rng.uniform(8, 10), rng.uniform(0, 4)
        elif case == "in_hide":
            h = rng.choice(list(self.hides))
            lz.goal, lz.want_hide = "hide", h
            lz.x, lz.y = self.hides[h]
        elif case == "wall":            # this close to one side of the glass (the floor counts, as in wall_contact)
            side, m = rng.randrange(4), rng.uniform(6, 60)
            if side == 0:
                lz.x = self.board.x0(lz.y) + m
            elif side == 1:
                lz.x = self.board.x1(lz.y) - m
            elif side == 2:
                lz.y = self.board.top(lz.x) + m
            else:
                lz.y = self.bot - m
            lz.x, lz.y = self.clamp(lz.x, lz.y)
        elif case == "prey_near":
            self.place_prey(lz, rng.choice(["near", "mid"]))
            lz.hunger = rng.uniform(0, 10)
        return case


def l1_snapshot(terr, lz):
    """the parity fixture's `snap` for one lizard, as advisor_core.c advisor_core_l1_snap builds it"""
    others = [o for o in terr.lizards if o is not lz]
    other = min(others, key=lambda o: lz.dist((o.x, o.y))) if others else None
    prey = terr.nearest(lz, terr.prey_points())
    hide = terr.nearest(lz, list(terr.hides.values()))          # tank_nearest_hide: the first of equals
    b = terr.board
    gx0, gx1, gy0 = b.x0(lz.y), b.x1(lz.y), b.top(lz.x)          # wall_contact: the glass at its height and column
    sides = [((gx1, lz.y), gx1 - lz.x), ((gx0, lz.y), lz.x - gx0), ((lz.x, b.bot), b.bot - lz.y),
             ((lz.x, gy0), lz.y - gy0)]
    wall = min(sides, key=lambda s: s[1])[0]
    return {"pack": terr.pack.id, "zone": l1_zone(lz.x, lz.y, b.w, b.bot), "x": lz.x, "y": lz.y,
            "heading": lz.heading, "body_c": lz.body_c, "hunger": lz.hunger, "energy": lz.energy,
            "stress": lz.stress, "curiosity": lz.curiosity, "trust": lz.trust, "bored": lz.bored,
            "bold": lz.bold, "social": lz.social, "stage": lz.stage,
            "shed": {"normal": "no", "soon": "soon"}.get(lz.shed, "yes"), "last": lz.goal,
            "phase": terr.phase(), "prey": list(prey) if prey else None,
            "other": [other.x, other.y] if other else None, "warm": list(terr.bask), "shelter": list(hide),
            "in_shelter": terr.hide_at(lz.x, lz.y) is not None, "wall": list(wall)}


def encode_l1(terr, lz):
    return encode_l1_snap(l1_snapshot(terr, lz))


def new_world(rng):
    return Terrarium(rng) if is_l1() else Tank(rng)


def members(world):
    return world.lizards if is_l1() else world.fish


def warm_up(world):
    """settle a new world out of its initial pose"""
    for _ in range(300 if is_l1() else 200):
        world.tick()


def ticks_between(world):
    """sim ticks before the next sample: the fish's 40 frames; L1 spreads them so
    each lizard is asked about every L1_ASK_EVERY_S whatever the population
    (a lone lizard sampled every 10 s gave near-duplicate states)"""
    if is_l1():
        return max(5, round(L1_ASK_EVERY_S / len(world.lizards)))
    return 40


def rule_goal(state, rng):
    """A stand-in teacher for --dry-run (and pilot.py --api rules): the nine rules
    of schema-lizard.md as plain code, so a dry run's lizards move roughly as
    the teacher would move them. Not a label source - the teacher is."""
    s = parse_l1_line(state)
    heat, hunger, energy, stress, bored = s["heat"], s["hunger"], s["energy"], s["stress"], s["bored"]
    prey = s["prey"][0] if s["prey"] else None
    other = s["other"][0] if s["other"] else None
    ph, shed, last = s["phase"], s["shed"], s["last"]

    def out(g, u):
        return f"{g} urgency {max(0, min(9, u))}"
    if heat >= 8:
        return out("hide", heat)
    if hunger >= 8 and prey is not None and (heat >= 3 or prey == "near"):
        return out("hunt", hunger)
    if stress >= 7:
        return out("hide", stress)
    if hunger >= 8 and heat <= 2 and ph != "sleep":
        return out("bask", 7)
    if ph == "sleep":
        return out("hide" if heat <= 2 or shed != "no" or rng.random() < 0.4 else "rest", 3)
    if energy <= 2:
        return out("bask" if heat <= 2 else "rest", 6)
    if heat <= 3:
        return out("bask", 8 - heat)
    if shed == "yes" or (shed == "soon" and rng.random() < 0.7):
        return out("hide", 6)
    if ph == "settle":
        if hunger >= 5 and prey is not None:
            return out("hunt", hunger - 1)
        return out(rng.choice(["hide", "rest"]), 3)
    if hunger >= 6 and prey is not None and (prey != "far" or s["bold"] >= 7):
        return out("hunt", hunger - 1)
    if s["social"] <= 2 and other == "near":
        return out(rng.choice(["hide", "explore"]), 5)
    if s["stage"] == "hatch" and stress >= 4:
        return out("hide", 5)
    pastimes = ["explore", "investigate", "dig", "rest", "hide"]
    if bored >= 7:
        return out(rng.choice([g for g in ("explore", "dig") if g != last] or pastimes), 5)
    if bored <= 2 and last in pastimes:
        return out(last, 4)
    weights = [2 + s["bold"] / 3, 1 + s["bold"] / 4 + (s["social"] / 3 if other else 0), 1.5,
               1 + (2 if s["stage"] == "elder" else 0), 0.5 + (2 if s["bold"] <= 2 else 0)]
    return out(rng.choices(pastimes, weights=weights)[0], rng.randint(2, 5))


# teacher APIs: (default base URL, default model)
API_DEFAULTS = {
    "ollama": ("http://192.168.0.139:11434", "gemma4:26b"),
    # the lab (PLAN.md P4): llama-swap on zaphod over Tailscale; chat-teacher = Gemma 4 31B, 4 slots x 4K
    "openai": ("http://zaphod.tail740235.ts.net:8080/v1", "chat-teacher"),
}
LAB_KEY_ENV = "LAB_API_KEY"     # the OpenAI-compatible server's key: from the environment only


def teacher_defaults(api, host=None, model=None):
    h, m = API_DEFAULTS.get(api, (None, None))
    return host or h, model or m


def _curl_post(url, body, timeout, secret_header=None):
    """POST a JSON body with curl and parse the JSON reply. A secret header
    reaches curl through a pipe (-H @/dev/fd/N): never in argv (ps), never on disk.
    Transport is curl, not urllib. Root cause found 2026-08-21: Little Snitch
    had a per-process rule allowing this app's Python only to pypi.org /
    pythonhosted.org (left over from the torch install) while curl had
    "any outgoing" - python's sockets silently stalled. Rule fixed; curl kept
    because it is robust to that class of drift and costs ~10ms per call."""
    cmd = ["curl", "-s", "--max-time", str(int(timeout)), url, "-H", "Content-Type: application/json", "-d", "@-"]
    fds = ()
    if secret_header:
        r, w = os.pipe()
        os.write(w, (secret_header + "\n").encode())
        os.close(w)
        cmd += ["-H", f"@/dev/fd/{r}"]
        fds = (r,)
    try:
        proc = subprocess.run(cmd, input=body, capture_output=True, timeout=timeout + 10, pass_fds=fds)
    except subprocess.TimeoutExpired:
        raise TimeoutError(f"no reply in {timeout + 10:.0f} s") from None
    finally:
        for fd in fds:
            os.close(fd)
    if proc.returncode != 0 or not proc.stdout:
        raise OSError(f"curl failed rc={proc.returncode}")
    return json.loads(proc.stdout)


def ollama_raw(host, model, state, timeout):
    """the teacher's reply text from Ollama's /api/chat (and no usage)"""
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt()},
            {"role": "user", "content": state},
        ],
        "format": output_schema(),
        "stream": False,
        "think": False,
        "keep_alive": "5m",
        "options": {"temperature": 0.7, "num_predict": 48, "num_ctx": 512},
    }).encode()
    return _curl_post(f"{host}/api/chat", body, timeout)["message"]["content"], {}


def ask_ollama(host, model, state, timeout):
    content, _ = ollama_raw(host, model, state, timeout)
    obj = json.loads(content)
    goal = obj["goal"] if obj.get("goal") in goals() else None
    if goal is None:
        return None
    urgency = obj.get("urgency")
    urgency = urgency if isinstance(urgency, int) and 0 <= urgency <= 9 else 5
    return f"{goal} urgency {urgency}"


def openai_raw(host, model, state, timeout):
    """the teacher's reply text and token usage from an OpenAI-compatible
    /chat/completions (the lab's llama-swap). No `tools` in the request: the lab's
    known Qwen bug is tools together with a JSON response format."""
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt()},
            {"role": "user", "content": state},
        ],
        "response_format": {"type": "json_schema",
                            "json_schema": {"name": "decision", "strict": True, "schema": output_schema()}},
        "temperature": 0.7,
        "max_tokens": 48,
        # harmless on chat-teacher (the server runs --reasoning off); without it a
        # thinking Gemma spends all 48 tokens thinking and returns nothing (PLAN.md P4)
        "chat_template_kwargs": {"enable_thinking": False},
        "stream": False,
    }).encode()
    key = os.environ.get(LAB_KEY_ENV)
    resp = _curl_post(f"{host.rstrip('/')}/chat/completions", body, timeout,
                      f"Authorization: Bearer {key}" if key else None)
    if "error" in resp:
        err = resp["error"]
        raise OSError(f"server: {err.get('message', err) if isinstance(err, dict) else err}"[:200])
    return resp["choices"][0]["message"].get("content") or "", resp.get("usage") or {}


def parse_decision(content):
    """the teacher's JSON reply -> ("<goal> urgency <u>", None), or (None, "bad_json" | "bad_goal");
    a missing or out-of-range urgency is 5, as the device parser has it"""
    try:
        obj = json.loads(content)
    except (TypeError, ValueError):
        return None, "bad_json"
    if not isinstance(obj, dict) or obj.get("goal") not in goals():
        return None, "bad_goal"
    u = obj.get("urgency")
    u = u if isinstance(u, int) and not isinstance(u, bool) and 0 <= u <= 9 else 5
    return f"{obj['goal']} urgency {u}", None


_RULES_RNG = random.Random(0)


def teacher_call(api, host, model, state, timeout):
    """One decision with its details (pilot.py): {"goal": "<goal> urgency <u>" or
    None, "fail": None | "bad_json" | "bad_goal", "raw": the reply text, "usage":
    token counts (openai), "secs": wall time}. Transport errors raise.
    api "rules" (L1 only) asks rule_goal instead of a model: no network."""
    t0 = time.time()
    if api == "openai":
        raw, usage = openai_raw(host, model, state, timeout)
    elif api == "ollama":
        raw, usage = ollama_raw(host, model, state, timeout)
    elif api == "rules":
        g = rule_goal(state, _RULES_RNG).split()
        raw, usage = json.dumps({"goal": g[0], "urgency": int(g[2])}), {}
    else:
        raise ValueError(f"unknown api {api!r}")
    goal, fail = parse_decision(raw)
    return {"goal": goal, "fail": fail, "raw": raw, "usage": usage, "secs": time.time() - t0}


def ask_teacher(api, host, model, state, timeout):
    """the decision as a goal line, or None for an unusable reply; transport errors raise"""
    if api == "ollama":
        return ask_ollama(host, model, state, timeout)
    return teacher_call(api, host, model, state, timeout)["goal"]


def apply_goal(agent, goal):
    """an advisor decision feeds back into the sim (a lizard also takes the urgency)"""
    w = goal.split()
    if is_l1():
        agent.set_goal(w[0], int(w[2]))
    else:
        agent.set_goal(w[0])


def l1_stats(states, labels):
    """a dry run's coverage: how often each edge the acceptance needs turns up"""
    n = len(states)
    rows = [parse_l1_line(s) for s in states]

    def share(pred):
        return f"{100 * sum(1 for r in rows if pred(r)) / n:.1f}%"

    def spread(key, fn=lambda v: v):
        c = {}
        for r in rows:
            k = fn(r[key])
            c[k] = c.get(k, 0) + 1
        return " ".join(f"{k}:{100 * v / n:.0f}%" for k, v in sorted(c.items(), key=lambda kv: str(kv[0])))
    print(f"\n{n} states", file=sys.stderr)
    for key in ("phase", "heat", "hunger", "energy", "stress", "curiosity", "bored", "shed", "stage", "zone"):
        print(f"  {key:9s} {spread(key)}", file=sys.stderr)
    for key in ("prey", "other", "warm", "shelter", "wall"):
        empty = "clear" if key == "wall" else "none"
        print(f"  {key:9s} {spread(key, lambda v: v[0] if v else empty)}", file=sys.stderr)
    print(f"  edges     heat 0-2 {share(lambda r: r['heat'] <= 2)}, heat 8-9 {share(lambda r: r['heat'] >= 8)}, "
          f"hunger 8-9 {share(lambda r: r['hunger'] >= 8)} (with prey near/mid "
          f"{share(lambda r: r['hunger'] >= 8 and r['prey'] and r['prey'][0] != 'far')}), "
          f"stress 7-9 {share(lambda r: r['stress'] >= 7)}, bored 7-9 {share(lambda r: r['bored'] >= 7)}, "
          f"inside a hide {share(lambda r: r['shelter'] == ('near', 12))}, social 0-2 with other near "
          f"{share(lambda r: r['social'] <= 2 and r['other'] and r['other'][0] == 'near')}", file=sys.stderr)

    def prey_is(kind):
        return lambda r: r["hunger"] >= 8 and (r["prey"][0] if r["prey"] else "none") == kind

    def other_near(lo, hi):
        return lambda r: r["other"] is not None and r["other"][0] == "near" and lo <= r["social"] <= hi
    buckets = ([(f"heat {a}-{b}" if a != b else f"heat {a}", lambda r, a=a, b=b: a <= r["heat"] <= b)
                for a, b in ((0, 2), (3, 4), (5, 6), (7, 7), (8, 9))]
               + [(f"shed {v}", lambda r, v=v: r["shed"] == v) for v in SHED_L1]
               + [(f"phase {v}", lambda r, v=v: r["phase"] == v) for v in PHASES_L1]
               + [(f"hunger 8-9, prey {k}", prey_is(k)) for k in ("near", "mid", "far", "none")]
               + [("bored 7-9", lambda r: r["bored"] >= 7),
                  ("other near, social 0-2", other_near(0, 2)), ("other near, social 7-9", other_near(7, 9))]
               + [(f"stage {v}", lambda r, v=v: r["stage"] == v) for v in STAGES_L1])
    print("  coverage (share of lines; < 2% flagged):", file=sys.stderr)
    for name, pred in buckets:
        k = sum(1 for r in rows if pred(r))
        print(f"    {name:26s} {k:6d}  {100 * k / n:5.1f}%{'   <-- under 2%' if k < 0.02 * n else ''}", file=sys.stderr)
    c = {}
    for g in labels:
        c[g] = c.get(g, 0) + 1
    print("  rule stand-in goals: " + " ".join(f"{g}:{100 * k / n:.0f}%"
                                               for g, k in sorted(c.items(), key=lambda kv: -kv[1])), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--count", type=int, required=True, help="number of (state, goal) pairs")
    ap.add_argument("--dry-run", action="store_true", help="print states, ask no teacher")
    ap.add_argument("--api", choices=("ollama", "openai"), default="ollama",
                    help="teacher API: Ollama /api/chat, or OpenAI-compatible /chat/completions "
                         f"(the lab; key from ${LAB_KEY_ENV})")
    ap.add_argument("--host", default=None, help="teacher base URL (default: ollama "
                    f"{API_DEFAULTS['ollama'][0]}, openai {API_DEFAULTS['openai'][0]})")
    ap.add_argument("--model", default=None, help="teacher model (default: ollama "
                    f"{API_DEFAULTS['ollama'][1]}, openai {API_DEFAULTS['openai'][1]})")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out", "traces.jsonl"))
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--ticks-between", type=int, default=None,
                    help=f"sim ticks between samples (default 40 frames; L1 {L1_ASK_EVERY_S} s / the lizards)")
    ap.add_argument("--perturb", type=float, default=0.45, help="edge-case perturbation probability")
    ap.add_argument("--timeout", type=float, default=30.0, help="per-request timeout, seconds")
    ap.add_argument("--max-minutes", type=float, default=None,
                    help="hard wall-clock cap; stop cleanly when exceeded")
    ap.add_argument("--schema", type=parse_schema, choices=(2, 3, 4, "L1"), default=2,
                    help="state-line schema: 2 = frozen, 3 = shipped (no names, trust, 2-6 fish), "
                         "4 = next cycle (no shadow, + bored), L1 = the lizard line (schema-lizard.md)")
    ap.add_argument("--repopulate", type=int, default=250,
                    help="v3/v4/L1: rebuild the world with a new random population every N samples")
    ap.add_argument("--stats", action="store_true", help="L1 dry run: the coverage of the edges, to stderr")
    args = ap.parse_args()
    global SCHEMA
    SCHEMA = args.schema
    host, model = teacher_defaults(args.api, args.host, args.model)
    fixed_ticks = args.ticks_between
    deadline = time.time() + args.max_minutes * 60 if args.max_minutes else None

    rng = random.Random(args.seed)
    tank = new_world(rng)
    warm_up(tank)

    out = None
    if not args.dry_run:
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        out = open(args.out, "a")

    written = failures = 0
    fish_idx = 0
    repop_at = 0            # L1: `written` at the last repopulation
    seen, labels = [], []
    try:
        while written < args.count:
            if deadline and time.time() > deadline:
                print(f"[max-minutes reached] stopping at {written} pairs", file=sys.stderr)
                break
            # (fish: as it always was - which keeps a tank whose size does not divide the
            # count's multiples of --repopulate for several periods; L1: every --repopulate)
            due = written - repop_at >= args.repopulate if is_l1() else written and written % args.repopulate == 0
            if (is_l1() or SCHEMA >= 3) and due and fish_idx % len(members(tank)) == 0:
                tank = new_world(rng)                  # a different population (L1: pack, keeper, layout too)
                warm_up(tank)
                repop_at = written
            for _ in range(fixed_ticks or ticks_between(tank)):
                tank.tick()
            pop = members(tank)
            f = pop[fish_idx % len(pop)]
            fish_idx += 1
            if rng.random() < args.perturb:
                tank.perturb(f)
            state = encode(tank, f)

            if args.dry_run:
                print(state)
                written += 1
                if is_l1():         # no teacher: the rule stand-in moves the lizards instead
                    goal = rule_goal(state, rng)
                    apply_goal(f, goal)
                    seen.append(state)
                    labels.append(goal.split()[0])
                continue

            try:
                goal = ask_teacher(args.api, host, model, state, args.timeout)
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, KeyError, OSError) as e:
                failures += 1
                print(f"[warn] {type(e).__name__}: {e}", file=sys.stderr)
                if failures >= 5 and written == 0:
                    sys.exit(f"aborting: {failures} consecutive failures reaching {host}")
                continue
            if goal is None:
                failures += 1
                continue

            failures = 0
            out.write(json.dumps({"state": state, "goal": goal}) + "\n")
            out.flush()
            written += 1
            apply_goal(f, goal)  # advisor decision feeds back into the sim
            if written % 50 == 0:
                print(f"{written}/{args.count}")
    finally:
        if out:
            out.close()
    if args.dry_run and args.stats and seen:
        l1_stats(seen, labels)
    if not args.dry_run:
        print(f"done: {written} pairs -> {args.out}")


if __name__ == "__main__":
    main()
