# Lizard schema L1 — training runbook (prepared 2026-11-04; the long run NOT started)

The first model of pocket-terrarium: the species-agnostic lizard line
(`model/schema-lizard.md`), taught by the lab's `chat-teacher`, trained from
scratch (no fish data carries over: the goals are different). Everything up to
the overnight run is built; the pilot and the teacher-side prompt check ran on
2026-11-04 (below). The night, the training and the acceptance have not run.

| Piece | State |
|---|---|
| `gen_traces.py --schema L1`: Terrarium/Lizard sim, `encode_l1_snap`, `SYSTEM_PROMPT_L1`, `--api openai` | built, dry-run and pilot-tested |
| `make_l1_parity.py` → `testdata/l1_parity.jsonl` (R4, 420 lines) | built; the C side is `./fishsim --selftest-l1-encode` (P2a) |
| `train_tokenizer.py --schema L1` (vocab 59), `fold_traces.py --schema L1` | built, verified on the pilot's teacher answers |
| `pilot.py` + `pilot_l1.jsonl` (37 edge states) | built, run |
| `prompt_check.py --schema L1` (A2–A12 and rules 3, 7, 8 on the teacher) | built, run in full on the shipped text (the gate, below) |
| `eval.py --schema L1 --teacher --api openai` (A1) | built; needs a model |
| `train.py` / `export_q4.py` | unchanged; `POCKET_SCHEMA=L1` selects the lexicon |
| `probe_dist.py` for L1 (A2–A12 on the student) | **not built** |
| sim census A13–A16 (`--selftest-llm` with an L1 model, the test pack) | **not built** (needs P2b's lizard goals and motion in C) |
| shipping (`PT_MODEL_TAG lizard_v1`, the model partition) | P6 |

## The line and the teacher

```
zone 4 hunger 7 energy 5 stress 2 curiosity 6 heat 3 bold 4 social 2 stage adult trust 6 bored 1 shed no prey mid 2 other none warm far 11 shelter near 7 wall clear last explore phase active
```
Goals: hunt bask hide explore investigate dig rest. 43 state words at worst, 48 of
the 64 positions with BOS and the answer. Every number is already normalised to
the species pack (`docs/species-pack.md`); the sim runs both the gecko (pack 1)
and the A16 test pack (250: every `t_*` +6 °C) and their lines are drawn from the
same distribution.

Teacher: `chat-teacher` on the lab (llama-swap on zaphod, Gemma 4 31B, 4 slots ×
4K, `--reasoning off`), OpenAI-compatible: `POST {host}/chat/completions` with
`response_format: json_schema` (the seven goals, urgency 0–9), `temperature 0.7`,
`max_tokens 48`, `chat_template_kwargs: {enable_thinking: false}`, no `tools`.
The key comes from `$LAB_API_KEY` only and reaches curl through a pipe (never in
argv, never on disk). Put it in the environment without printing it:

```bash
export LAB_API_KEY="$(python3 -c 'import os, yaml; print(yaml.safe_load(open(os.path.expanduser("~/.omp/agent/models.yml")))["providers"]["lab"]["apiKey"])')"
```

## 0. Preflight (~10 min of teacher time; the required gate, R3)

```bash
cd model
python3 gen_traces.py --schema L1 --dry-run --count 30 --seed 1            # 30 lines, no network
python3 gen_traces.py --schema L1 --dry-run --count 2000 --seed 41 --stats > /dev/null   # coverage, stderr
python3 make_l1_parity.py --check                                          # the fixture is current
(cd ../sim && /tmp/terr-make.sh check)                                     # incl. --selftest-l1-encode (P2a)
python3 train_tokenizer.py --schema L1                                     # out/tokenizer_l1.bin, vocab 59
curl -s -H @<(printf 'Authorization: Bearer %s\n' "$LAB_API_KEY") \
     http://zaphod.tail740235.ts.net:8080/v1/models | grep -o chat-teacher # 0.3 reachable
python3 prompt_check.py --schema L1 --api openai --workers 4 --out out/prompt_check_l1.json   # 0.4: 464 calls
python3 pilot.py --api openai --workers 4                                  # 0.5: 111 calls, ~1 min
```

The sim's world is the C engine's as built (P2a): the four boards' glass and
floor, the desert habitat (`common/habitat/desert.c`) at its absolute temperatures
for every pack - floor 31 → 25 °C lit, 5 °C cooler dark, the lamp's pool 35 °C
within 70 px of the basking spot at 0.28 of the floor, the warm / humid / cool
hides at 0, 0.55 and 1 of the floor (30/30, 28/25, 24/21 °C lit/dark; inside
within 28 px), the humid hide starting at 0.6 and drying in 3 tended hours - read
as `tank_env_c_lit` reads it, the body following it as `body_follow` does, the
zone grid and the wall's contact point as `advisor_core_l1_snap` builds them. A16's
test pack (15% of terrariums) lives in the same desert, so it is cold but in the
pool. The lizards roam the whole glass as P2a's do (P2b brings walking).
Since P2c the C desert differs from this mirror: its basking spot is on the high rock (v 0.58, 0.20 along it), the
humid hide is at 0.40, lit air rises 4 °C toward the glass's top, and the lizards walk the floor, the terraces and the
ramps (`docs/habitat-pack.md` "The ground"); the mirror still has P2a's floor-level desert.

`--stats` on that dry run (2000 lines, seed 41; the rule stand-in moves the
lizards, so the shares are indicative - the teacher's decisions shape the real
run). No bucket under 2%; eight seeds keep the thinnest, starving with prey
near/mid/far, at 2.4–4.5%:

| Bucket | Share | Bucket | Share |
|---|---|---|---|
| heat 0–2 | 15.3% | hunger 8–9, prey near | 2.9% |
| heat 3–4 | 36.8% | hunger 8–9, prey mid | 2.7% |
| heat 5–6 | 40.2% | hunger 8–9, prey far | 2.8% |
| heat 7 | 3.9% | hunger 8–9, prey none | 9.8% |
| heat 8–9 | 3.9% | bored 7–9 | 29.5% |
| shed no / soon / yes | 70.5 / 13.3 / 16.1% | other near, social 0–2 | 8.1% |
| phase active / settle / sleep | 81.1 / 9.4 / 9.5% | other near, social 7–9 | 5.0% |
| stage hatch / juv / adult / elder | 9.9 / 14.2 / 53.9 / 22.1% | | |

The floor line puts the places low: zones 4–5 hold 71% of the lines and `wall`
is near 73%, mid 25%, clear 2% (the floor is glass to the wall rule, as for the fish).

prompt_check's L1 panel pins one thing on a content lizard (fed, comfortable,
calm, active) and prints what the student will inherit: A2–A12, and for the
tendencies paired cases - the same backgrounds twice, one field changed - so the
difference is that field's effect: bold 9 against bold 0 (A11; bored 2–5 so `last`
does not decide it), hatchling against adult at stress 4–6 and elder against adult
at energy 3–6 (rule 8), shed soon against no (rule 3). Plus `social9near` (rule 7: social 7–9 with the other near
investigates now and then), `content` (no goal over 0.45), `satedprey` (a snap at
prey in front is allowed now). A10 uses a `last` that still fits the state (rest
when a little tired, investigate with something to look at; appended to the
panel so every other case keeps its states); `bored0any` reports the old any-`last`
version. `--cases a,b` runs a subset on the same states (a pair's partner comes
along); `--out` keeps every answer. If a check is BAD, edit ONE sentence of
`SYSTEM_PROMPT_L1` and re-run it; never fix the teacher in the sim.

### The pilot (2026-11-04, `chat-teacher`, from this workstation over Tailscale)

`pilot_l1.jsonl`: 37 hand-written edge states, each with the goals a teacher
that follows the nine rules may give and the acceptance row or rule it probes.
(Round 1 had 41: "hunger 1, prey near → not hunt" went when rule 1 gained "a fed
lizard may still snap at prey right in front of it"; the hatchling-stress,
shed-soon and tired-elder states became prompt_check's paired comparisons; the
bold-0 A11 state is incurious since round 3 - curiosity 2, as A11 means timid
and incurious.) `--api rules` runs the states through `gen_traces.rule_goal` (the
rules as plain code): the expectations are consistent with the rules.

| | try 1 (first prompt) | try 2 | try 3 (dropped) | try 4 (full runs) |
|---|---|---|---|---|
| states × asks | 41 × 3 | 31 × 2 | 12 × 2 | 38 × 3 |
| agreement | 108/123 = 87.8%; **108/114 on the 38 states** | 53/62 = 85.5% | 16/24, no change | **109/114 = 95.6%** (36/38 states fully) |
| JSON failures | 0/123 | 0/62 | 0/24 | 0/114 |
| rate (4 workers) | 159.8 decisions/min, 1.47 s median | 145.9/min | 118/min | 146.7/min, 1.49 s median, 1.65 s p90 |
| prompt tokens (system + state + template) | 1,062–1,070 | 1,138–1,146 | 1,166–1,173 | 1,142–1,150; answer 19–21; largest request 1,171 of the 4K slot |

Before → after per rule on the same 38 states (try 1 → try 4): rule 1 100 → 100%,
2 93 → 100%, 3 100 → 100%, 4 88 → 100%, 5 100 → 100%, 6 100 → 100%, 7 75 → 75%,
8 100 → 67%, 9 100 → 100%. Two states changed: A7-sleep-cold 0/3 → 3/3 (try 2's
"a cold lizard too: it hides to keep warm rather than basking"), R8-elder-tired
3/3 → 1/3 (a tendency state that flips between every prompt: 3/3, 1/2, 2/2, 1/3).
Every A-row state agrees 100% except A11-bold0.

Try 4 = try 2 + rule 1 synced with the spec (a fed lizard may snap at prey in
front of it) + rule 7's bold clause "bold 7-9 roams and investigates ...; bold 0-2
stays near shelter and rests or digs instead of exploring, even when curious". The
A11/`investigate` changes were measured one at a time on a mini panel (prompt_check
`--n 16 --cases bold9,social9near,social0`: 9 bold pairs, 8 social 7–9 with the
other near, 9 social 0, plus the pilot's A11 pair × 3; 41 requests each):

| prompt | change | A11 paired (bold 9 − 0) | bold 0 roams | social 7–9 near: investigate | A12 | pilot A11-bold0 |
|---|---|---|---|---|---|---|
| p0 | try 2 + rule 1 synced | +0.33 | 0.67 | 1.00 | 0.00 | explore 3/3 |
| p1 | p0 + triggers in the goal list (investigate = something specific close by: the other near when social 5+, a hide or decor; explore = a zone not seen lately) | +0.33 | 0.67 | 1.00 | 0.00 | explore 3/3 |
| p2 | p0 + the bold clause | +0.33 | 0.67 | 0.88 | 0.00 | explore 3/3 |
| **p3** | p2 + "even when curious" (= try 4) | **+0.44** | 0.56 | 1.00 | 0.00 | explore 3/3 |
| p4 | p3 + explore "seldom ... for a timid lizard" | +0.44 | 0.56 | 0.75 | 0.00 | explore 3/3 |
| p5 | p3 + curiosity "a timid lizard looks around from near cover" | +0.33 | 0.67 | 0.75 | 0.00 | explore 3/3 |

Read: the "investigate never chosen" of round 1 came from the pilot - no state had
a social lizard with the other near. Given one, the teacher investigates 75–100%
(more than rule 7's "now and then"), and bold-9 lizards investigate 11–38%. A11
passes at every step; what it fights is curiosity, not the prompt's bold words:
the bold-0 lizards that still explore are the curious ones (curiosity 6+ explores,
5 or less rests or hides), and the pilot's A11-bold0 state has curiosity 6.

### prompt_check, full panel (try 4 prompt, 408 calls, 0 failures)

| Row | Teacher | Want |
|---|---|---|
| A2 hunt (hunger 9, prey near/mid, heat 3–7) | 0.96 | ≥ 0.86 |
| A3 bask (active, hunger 9, prey none, heat 0–2) | 1.00 | ≥ 0.6 |
| A4 bask (active, heat 0–2, hunger ≤ 5, stress ≤ 4) | 1.00 | ≥ 0.8 |
| A5 hide (heat 9) | 1.00 | ≥ 0.8 |
| A6 hide (shed yes, hunger ≤ 6) | 1.00 | ≥ 0.6 |
| A7 rest or hide (sleep, hunger ≤ 7) | 1.00 (hide 0.96) | ≥ 0.85 |
| A8 hide (stress 8–9) | 1.00 | ≥ 0.7 |
| A9 a goal ≠ last (active, bored 8–9) | 1.00 | ≥ 0.6 |
| A10 last kept (active, bored 0–1, any `last`) | **0.12 BAD** | ≥ 0.4 |
| A11 explore+investigate, bold 9 − bold 0 (24 pairs) | +0.33 | ≥ 0.15 |
| A12 investigate (social 0, other near) | 0.00 | ≤ 0.05 |
| rule 7: investigate (social 7–9, other near) | 0.85 | now and then |
| rule 8: hide, hatchling − adult (16 pairs) | +0.12 | > 0 |
| rule 3: hide, shed soon − no (16 pairs) | +0.44 | > 0 |
| max single goal, content | **0.67 explore BAD** (investigate 0.21, rest 0.12) | < 0.45 |
| hunt, hunger 0–2 with prey near | 0.75 (allowed) | — |

A10 and the explore attractor are one problem: the teacher leaves rest, dig and
investigate for explore even at bored 0–1 (kept: dig 0/7, rest 0/6; part of that
is the old probe - resting with energy 5–9 no longer "makes sense", hence the
A10 case that now keeps `last` plausible). On 9 such states: try 4 kept `last`
0.22 with content explore 0.89. Rule 6's "bored 0-2 means it has only just begun:
it keeps last while it still makes sense - a lizard settles into a pastime for a
while" **took A10 to 0.67 and content explore to 0.56** (investigate 0.33);
dropping explore's "a real activity, not a fallback" did nothing. Guards for it
on the same states: A9 1.00, A11 +0.22 (try 4: +0.44), A12 0.00; and the 12 pilot
states at bored 0–2, most of them a need that must beat `last` (starving, cold,
overheating, shedding, sleep), 33/36 - only R8-elder-tired, 0/3. That sentence
went in (781 words, sha1 dec03c6062); the content attractor was still over 0.45.

Requests in round 2: 894 of 900 (six mini panels 246, full pilot 114, full
prompt_check 408, the A10 follow-up 126).

### Round 3 and the gate (2026-11-04, the shipped text)

The explore attractor, one sentence at a time on prompt_check's 24 `content`
states of the full panel (the same states as the try-4 run) with A10/A11 (9 each)
and A9/A12 (4 each) as guards: the round-2 text left explore at 0.50 there (try
4: 0.67). Round 1 added "and explore is one choice among the four, never the
default" to the content-lizard sentence: content explore **0.29** (investigate
0.29, rest 0.25, dig 0.17), A10 0.67, A11 +0.44, A9 1.00, A12 0.00 - it passed, so
no second round. The text that ships: 792 words, sha1 19434161a1, 1,172–1,180
prompt tokens a request, the largest request 1,202 of the 4K slot.

The gate on that exact text - prompt_check, full panel (464 calls) and the pilot
(37 states × 3 = 111): 0 failures in 575 replies, 152.5 decisions/min with 4 workers.

| Row or tendency | Check | Teacher (try 4 → gate) | Want | |
|---|---|---|---|---|
| A1 | student agrees with the teacher | needs a model (`eval.py`) | ≥ 72% | — |
| A2 | hunt: hunger 9, prey near/mid, heat 3–7 | 0.96 → **0.92**; pilot 12/12 | ≥ 0.86 | PASS |
| A3 | bask: active, hunger 9, prey none, heat 0–2 | 1.00 → **1.00**; pilot 6/6 | ≥ 0.6 | PASS |
| A4 | bask: active, heat 0–2, hunger ≤ 5, stress ≤ 4 | 1.00 → **1.00**; pilot 6/6 | ≥ 0.8 | PASS |
| A5 | hide: heat 9 | 1.00 → **1.00**; pilot 9/9 | ≥ 0.8 | PASS |
| A6 | hide: shed yes, hunger ≤ 6 | 1.00 → **1.00**; pilot 9/9 | ≥ 0.6 | PASS |
| A7 | rest or hide: sleep, hunger ≤ 7 | 1.00 → **1.00** (hide 0.96); pilot 12/12 | ≥ 0.85 | PASS |
| A8 | hide: stress 8–9 | 1.00 → **1.00**; pilot 6/6 | ≥ 0.7 | PASS |
| A9 | a goal ≠ last: active, bored 8–9 | 1.00 → **1.00**; pilot 9/9 | ≥ 0.6 | PASS |
| A10 | last kept: active, bored 0–1, `last` still fits | new → **0.58** (any `last`: 0.12 → 0.62); pilot 3/3 | ≥ 0.4 | PASS |
| A11 | explore+investigate, bold 9 − bold 0, 24 pairs | +0.33 → **+0.42**; pilot bold-9 3/3, **bold-0 0/3** | ≥ 0.15 | PASS (pilot state FAIL) |
| A12 | investigate: social 0, other near | 0.00 → **0.04** (1 of 24); pilot 6/6 | ≤ 0.05 | PASS |
| A13–A16 | sim census, test pack | not built (C, P2b/P6) | | — |
| rule 7 | investigate: social 7–9, other near | 0.85 → **0.80** | now and then (≥ 0.10) | PASS |
| rule 8 | hide: hatchling − adult, stress 4–6, 16 pairs | +0.12 → **+0.25** | > 0 | PASS |
| rule 8 | rest: elder − adult, energy 3–6, 16 pairs | new → **+0.19** | > 0 | PASS |
| rule 3 | hide: shed soon − no, 16 pairs | +0.44 → **+0.50** | > 0 | PASS |
| content | max single goal | 0.67 explore → **0.29** (explore 0.29, investigate 0.25, rest 0.25, dig 0.21) | < 0.45 | PASS |
| rule 1 | hunt: hunger 0–2, prey near (a snap is allowed) | 0.75 → 0.67 | — | report |
| pilot | all 37 states | **108/111 = 97.3%**; the 36 unchanged states 108/108 in both try 4 and the gate | | |

The one failing state, A11-bold0 (bold 0, curiosity 2, energy 8, bored 3, `last
rest`): explore at urgency 3, every time. Twelve diagnostic asks: energy 5 →
explore 3/3; a hide near → explore 3/3; bored 1 → rest 2/3; `last dig` → rest 3/3.
So it is not curiosity: the lizard was resting at bored 3, the teacher moves it on
(rule 6), and with rest just left its timid options are dig or hide - it explores
gently instead. The paired A11 holds; the state is an edge of rule 6 against rule 7.

Requests in round 3: 684 of 750 (content baseline 38, the explore round 59,
prompt_check 464, pilot 111, the diagnostic 12).

## 1. The night (unattended; ~5–6 h)

Four workers = the four slots; each worker is sequential because the teacher's
decision feeds back into its sim. The pilot measured ~150–160 decisions/min with
4 parallel requests (PLAN measured 126.7/min with fish states): 45,000 pairs in
~4.7–5.9 h. The key goes into the environment first (above); `--api openai`
defaults to the lab's host and `chat-teacher`.

```bash
cd model
mkdir -p out
for w in a b c d; do
  seed=$((41 + $(printf '%d' "'$w") - 97))            # a b c d -> 41 42 43 44
  nohup python3 gen_traces.py --schema L1 --api openai --count 11250 --max-minutes 420 \
      --seed $seed --out out/l1_traces_$w.jsonl > out/l1_gen_$w.log 2>&1 &
  echo $! > out/l1_gen_$w.pid
done
```
4 × 11,250 = 45,000 pairs; `--max-minutes 420` stops a slow night cleanly at 7 h
with what it has. Each log prints every 50 pairs (`50/11250`), warnings as
`[warn] ...`.

Watching it:
```bash
wc -l out/l1_traces_*.jsonl                                       # pairs so far
pgrep -af 'gen_traces.py --schema L1' | cut -c1-110                 # the live workers (the .pid files can hold a wrapper's pid, not python's)
python3 fold_traces.py --schema L1 out/l1_traces_a.jsonl --out /tmp/l1_peek.jsonl   # the first ~200: no goal over 40%
pkill -f 'gen_traces.py --schema L1'                               # stop all (the files keep every pair written)
```
The fold's coverage block prints the teacher's rate on each acceptance slice
(A2–A9, A12) in the data itself.

**A worker died** (its pid gone, its log ending in neither `done:` nor `[max-minutes reached]`):
restart it with a NEW seed - the old seed replays the same terrariums from the
start - for the pairs it still owes, appending to its own file (gen_traces
appends; a half-written last line is dropped by the fold, exact repeats are
de-duplicated). Seeds 51–54 are the first restarts of a–d, 61–64 the second:
```bash
w=b; seed=52                                          # (a 51, b 52, c 53, d 54; then 61-64)
left=$((11250 - $(wc -l < out/l1_traces_$w.jsonl)))
nohup python3 gen_traces.py --schema L1 --api openai --count $left --max-minutes 300 \
    --seed $seed --out out/l1_traces_$w.jsonl >> out/l1_gen_$w.log 2>&1 &
echo $! > out/l1_gen_$w.pid
```
(`--max-minutes`: what is left of the night.) A worker that cannot reach the teacher
at its start stops after five failures (`aborting: 5 consecutive failures`): the lab
is down or the key is not in the environment - fix that and start it again. Later in
the night a worker rides out an outage (a `[warn]` per failed ask, at most `--timeout`
30 s each) and carries on when the lab is back.

## 2. Fold and verify

```bash
python3 fold_traces.py --schema L1 out/l1_traces_*.jsonl --out out/l1_clean.jsonl
python3 train_tokenizer.py --schema L1 --verify out/l1_clean.jsonl
```
fold drops off-vocabulary words, any state that is not a well-formed L1 line
(field order and values) and any goal that is not `<goal> urgency <0-9>`.

## 3. Train (CPU)

```bash
POCKET_SCHEMA=L1 .venv/bin/python train.py --data out/l1_clean.jsonl --device cpu --iters 100 \
    --dim 384 --n-layers 8 --n-heads 8 --max-seq-len 64 --batch 64 --lr 6e-4 --out /tmp/ckpt_probe.pt
# time those 100 iters first (PLAN: 1-3 h for 5000 on marvin or zaphod) [INFERENCE until measured]
POCKET_SCHEMA=L1 .venv/bin/python train.py --data out/l1_clean.jsonl --device cpu --iters 5000 \
    --dim 384 --n-layers 8 --n-heads 8 --max-seq-len 64 --batch 64 --lr 6e-4 --out out/ckpt_l1.pt
```

## 4. Export

```bash
.venv/bin/python export_q4.py out/model_q4_l1.bin --checkpoint out/ckpt_l1.pt     # the device's 4-bit file
.venv/bin/python llama2.c/export.py out/model_l1.bin --checkpoint out/ckpt_l1.pt  # fp32 for runw / eval
cc -O3 -o runw runw.c -lm
./runw out/model_l1.bin -z out/tokenizer_l1.bin -t 0 -i "<an L1 line> ->"
```

## 5. Acceptance (`model/schema-lizard.md`, every row before shipping)

| Row | Script | State |
|---|---|---|
| A1 teacher agreement ≥ 72% | `eval.py --schema L1 --count 60 --teacher --api openai --model-bin out/model_l1.bin --tok-bin out/tokenizer_l1.bin` | built |
| A2–A12 on the student (sampling at temperature 1.0) | an L1 `probe_dist.py` (the same sweeps as prompt_check's L1 panel, on the checkpoint) | **not built** |
| A2–A12 on the teacher (what the student inherits) | `prompt_check.py --schema L1` | built |
| A13 heat 4–7 share ≥ 0.7, A14 zones/min ≥ 1.0, A15 crickets/h ≥ 4 | `./fishsim --selftest-llm` census with the L1 model | **not built** (P2b/P6) |
| A16 the A13 census on test pack 250 | the same census, pack 250 | **not built** (pack 250 exists in Python; C in P2a) |
| R4 C/Python encoder parity | `make_l1_parity.py --check` + `./fishsim --selftest-l1-encode` | built (Python); C by P2a |

The rows as `model/schema-lizard.md` states them since 2026-11-04 (and as the pilot,
prompt_check and fold's coverage block pin them): A2 for heat 3–7, A3/A4/A9/A10 in
`phase active`, A7 whatever the heat.

## If the night goes wrong

- `out/l1_gen_*.log` fills with `[warn] OSError: curl failed` or `TimeoutError`:
  another client holds the slots; drop to 2–3 workers. A `server: ...` warning
  is the lab's own error text (401: the key is not in the environment).
- A worker dies: the others keep going; restart it as in "A worker died" above (a new
  seed, the pairs it still owes, its own file).
- A skewed label mix: the prompt, not the sim - edit one sentence, re-run
  prompt_check, regenerate only that worker's file.
