# Lizard schema L1 — FROZEN (the first trace run started 2026-11-04; a change now means new traces)

The model's input and output for pocket-terrarium. It replaces the fish
schemas in `schema.md` (v2–v4), which stay as the record of what pocket-tank
shipped. Every rule here binds four places that must agree byte for byte:

| Where | What |
|---|---|
| `model/gen_traces.py --schema L1` | Python encoder + the sim that makes states |
| `model/train_tokenizer.py --schema L1` | the closed vocabulary |
| `common/llm/advisor_core.c` | the C encoder (selected when the tokenizer holds ` heat`) |
| `common/tank.h` `goal_id_t` | one enum value per goal token below |

A parity test (P4) feeds the same states to both encoders and diffs the lines.

**The model never learns a species.** Every number in the line is already
normalised to the lizard's species pack (`docs/species-pack.md`): `heat 5`
means "comfortable for this species", whatever the thermometer says. One model
serves every pack that fits the seven goals.

---

## State line (model input)

One lowercase line, single spaces, fixed field order:

```
zone <1-6> hunger <0-9> energy <0-9> stress <0-9> curiosity <0-9> heat <0-9>
bold <0-9> social <0-9> stage hatch|juv|adult|elder trust <0-9> bored <0-9> shed no|soon|yes
prey <sight> other <sight> warm <sight> shelter <sight> wall <dist> <clock>|clear
last <goal> phase active|settle|sleep
```

`<sight>` is `<dist> <clock>` or `none`. Line breaks above are for reading only.

Example:

```
zone 4 hunger 7 energy 5 stress 2 curiosity 6 heat 3 bold 4 social 2 stage adult trust 6 bored 1 shed no prey mid 2 other none warm far 11 shelter near 7 wall clear last explore phase active
```

### Fields

| Field | Values | Source (reflex layer) | Meaning to the model |
|---|---|---|---|
| `zone` | 1–6 | 3×2 grid of the terrarium, left→right, top row first | where it is |
| `hunger` | 0–9 | drive, pack rate | 9 = starving |
| `energy` | 0–9 | drive, pack rate; cold slows recovery | 0 = exhausted |
| `stress` | 0–9 | startles, crowding, overheating, a stuck shed | 9 = panicked |
| `curiosity` | 0–9 | drive, spent by exploring/investigating | 9 = restless to look around |
| `heat` | 0–9 | body temperature **normalised to the pack** | 0–2 cold and sluggish, 3–4 cool, **5–6 comfortable**, 7 warm, 8–9 overheating |
| `bold` | 0–9 | trait (`fish_t.bold` × 9, rounded) | risk-taking, roams open ground |
| `social` | 0–9 | trait | tolerance of other lizards; low = territorial |
| `stage` | hatch juv adult elder | life stage | hatchlings are skittish, elders rest more |
| `trust` | 0–9 | keeper relationship | shades calm vs flight from the hand |
| `bored` | 0–9 | as fish v4 (`BORED_*`, `tank.h:975`) | stale pastime: 7–9 must change |
| `shed` | no soon yes | shed state (`docs/shed.md`) | `soon`: pre-shed; `yes`: shedding now or stuck |
| `prey` | sight | nearest live cricket or mealworm dish within `far` | food |
| `other` | sight | nearest other lizard within `far` | company or a rival |
| `warm` | sight | the basking spot (lamp/mat centre) | where heat comes from |
| `shelter` | sight | nearest hide entrance, any kind; `near 12` while inside one | cover |
| `wall` | `<dist> <clock>` within `mid`, else `clear` | nearest glass | as fish |
| `last` | a goal | the goal it is carrying out | hysteresis |
| `phase` | active settle sleep | the pack's schedule (`docs/species-pack.md`) | `active`: its waking hours; `settle`: winding down, finish business; `sleep`: its rest hours |

Encoding rules carried over unchanged from the fish (`advisor_core.c`):
- Drives: float 0–10 truncated and clamped to an integer 0–9 (`drive9`, `advisor_core.c:43`).
- Traits: `round(trait × 9)`.
- Distances: `near` < 70 px, `mid` < 180 px, `far` < 380 px, else the sight is `none`
  (`SCHEMA_NEAR/MID/FAR`, `advisor_core.c:22-24`). On the round and watch boards the same pixel buckets apply.
- Clock: 1–12 relative to heading, 12 = dead ahead, 6 = behind (`clock_dir`, `advisor_core.c:32`).
  A lizard that turns on the spot changes every clock; that is intended.

Token count: 43 state words + `->` + 3 goal words + BOS = **48** of the 64
positions (`max_seq_len`). Room for ~15 more words before the model shape changes.

## Goal line (model output)

```
<goal> urgency <0-9>
```

| Goal | Reflex layer performs it (pack tunes speeds) |
|---|---|
| `hunt` | target the `prey` sight: stalk, freeze, tail-tip wiggle, lunge. Can miss. No prey: wander slowly toward where crickets were last |
| `bask` | walk to `warm`, flatten, stay; heat rises |
| `hide` | enter the hide that suits the need: humid if `shed` is soon/yes, cool if heat ≥ 7, else warm |
| `explore` | slow walk with tongue flicks into a zone not seen for a while |
| `investigate` | approach decor or `other`; tongue-flick. Low `social`: stand-off and tail wave instead of contact |
| `dig` | scrabble at the substrate in place (the playful/bored outlet) |
| `rest` | lie still where it is, eyes closing |

Urgency 0–9 scales speed and commitment, as for fish. Parser tolerance as for
fish: an unknown goal keeps the previous one; a missing urgency is 5.

## Vocabulary (59 ids)

| Group | Words |
|---|---|
| specials | `<unk>` BOS EOS |
| keys | zone hunger energy stress curiosity heat bold social stage trust bored shed prey other warm shelter wall last phase urgency |
| numbers | 0 1 2 3 4 5 6 7 8 9 10 11 12 |
| distances | none near mid far clear |
| stages | hatch juv adult elder |
| shed | no soon yes |
| phase | active settle sleep |
| goals | hunt bask hide explore investigate dig rest |
| separator | `->` |

No word plays two roles: the sighting is `shelter` because `hide` is a goal,
and the phase is `sleep` because `rest` is a goal.

## Trace format

As fish (`schema.md`, "Trace format"): JSONL `{"state": "...", "goal": "hunt urgency 8"}`,
teacher replies constrained to `{"goal": <enum>, "urgency": 0-9}`, training
documents `<BOS>state -> goal`.

---

## Behaviour the teacher must express

The teacher prompt (P4) is written from this list, and the acceptance checks
below test it. Each rule is a tendency, not a law: personality and the other
drives shade it.

1. **Starving beats everything except overheating.** Hunger 8–9 → `hunt` when prey is
   in sight. A cold lizard (heat 0–2) with prey `near` still hunts; with prey `far`
   or `none` it basks first. A fed lizard may still snap at prey right in front of
   it: geckos are opportunists (the pilot's teacher insisted, and it is true).
2. **Temperature is the main reason to move.** Heat 0–3 → `bask` unless something
   urgent. Heat 8–9 → `hide` (the reflex picks the cool hide). Heat 5–6 → no thermal pull.
3. **Shedding wants humidity.** `shed soon|yes` → `hide` far more often; appetite
   drops (`shed yes` and hunger < 7 → rarely `hunt`).
4. **Phase sets the rhythm.** `sleep` → `rest` or `hide` unless starving. `settle` →
   finish business (eat if hungry, warm up if cold), then `hide`/`rest`. `active` → free.
5. **Stress → cover.** Stress 7–9 → `hide`, more so for timid (bold 0–2) and low-trust lizards.
6. **Boredom moves it on.** Bored 7–9 → a goal different from `last`, usually `explore`
   or `dig`. Bored 0–2 → keep `last` while it still makes sense.
7. **Personality.** Bold 7–9 → more `explore`/`investigate`, hunts from farther.
   Bold 0–2 → stays near `shelter`. Social 0–2 with `other` near → avoid it (`hide` or
   `explore` away), never `investigate` it. Social 7–9 → `investigate` the other now and then.
8. **Stage.** Hatchlings: skittish, hide more, hunt hard when hungry. Elders: rest more.
9. **Energy.** Energy 0–2 → `rest` (or `bask` if also cold).

## Acceptance (P6) — written before any training

Measured with `model/probe_dist.py`-style sweeps on synthetic and sim states,
sampling as on the device (temperature 1.0). A model ships only if every row holds.

| # | Check | Pass |
|---|---|---|
| A1 | Teacher agreement (`eval.py --teacher`, n ≥ 60) | ≥ 72% |
| A2 | hunger 9, prey near or mid, heat 3–7 → `hunt` | ≥ 0.86 |
| A3 | `phase active`, hunger 9, prey none, heat 0–2 → `bask` | ≥ 0.6 |
| A4 | `phase active`, heat 0–2, hunger ≤ 5, stress ≤ 4 → `bask` | ≥ 0.8 |
| A5 | heat 9 → `hide` | ≥ 0.8 |
| A6 | `shed yes`, hunger ≤ 6 → `hide` | ≥ 0.6 |
| A7 | `phase sleep`, hunger ≤ 7 → `rest` or `hide` | ≥ 0.85 |
| A8 | stress 8–9 → `hide` | ≥ 0.7 |
| A9 | `phase active`, bored 8–9 → a goal ≠ `last` | ≥ 0.6 |
| A10 | `phase active`, bored 0–1, content state → `last` kept | ≥ 0.4 |
| A11 | bold 0 → 9 sweep: P(`explore` + `investigate`) rises | slope > 0, ≥ 0.15 apart at the ends |
| A12 | social 0, `other` near: P(`investigate`) | ≤ 0.05 |
| A13 | Sim census (`--selftest-llm`): share of awake time with heat in 4–7 | ≥ 0.7 |
| A14 | Sim census: distinct zones per lizard-minute | ≥ 1.0 (lizards roam less than fish' 1.5) |
| A15 | Sim census: crickets caught per lizard-hour when fed normally | ≥ 4 (they are not starving) |
| A16 | Pack independence: the A13 census rerun with a test pack whose comfort band is 6 °C warmer than the gecko's | heat 4–7 share ≥ 0.7 there too |

A16 is the check on R6 (PLAN.md). The model only ever sees normalised lines, so
the test is on the whole loop: the pack's normalisation plus the same model must
keep a differently built lizard inside its own comfort band.
