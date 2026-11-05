# Lizard schema L1 — training runbook (prepared 2026-11-04; night run 2026-11-04; focused repair 2026-11-05)

The first model of pocket-terrarium: the species-agnostic lizard line
(`model/schema-lizard.md`), taught by the lab's `chat-teacher`, trained from
scratch (no fish data carries over: the goals are different). Everything up to
the overnight run is built; the pilot and the teacher-side prompt check ran on
2026-11-04 (below). The night (44,968 pairs) and its training ran 2026-11-04; that model
failed A10/A12. The focused repair (section 6, 2026-11-05) passes A1-A12 on three seeds;
the shipped candidate is `out/model_q4_l1_focus2.bin`. A13-A16 (the sim census) are still open.

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

Fold's L1 coverage block gives the teacher's rate on each acceptance slice of the night's
(sim) states. Those slices are wider than the panel's: on the first 27,196 clean pairs
(2026-11-04, mid-run) the A2 slice (hunger 9, prey near/mid, heat 3–7, anything else) read
0.79 hunt. Of the 124 misses (hide 82, rest 27, bask 15), 74 had `shed yes`, 22 stress 8–9 and
21 sleep/settle (overlapping).
On the same slice cut to active, stress < 8 and shed no, the rate was 0.93 (306 states).
prompt_check's and probe_l1's A2 is that clean case (see 5).

## 3. Train (CPU)

```bash
POCKET_SCHEMA=L1 .venv/bin/python train.py --data out/l1_clean.jsonl --device cpu --iters 100 \
    --dim 384 --n-layers 8 --n-heads 8 --max-seq-len 64 --batch 64 --lr 6e-4 --out /tmp/ckpt_probe.pt
# time those 100 iters first. PLAN: 1-3 h for 5000 on marvin or zaphod. This workstation
# (12 threads, 2026-11-04, the night run going) took ~1.3 s an iteration: 5000 ≈ 2 h.
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

```bash
.venv/bin/python probe_l1.py --ckpt out/ckpt_l1.pt --strict         # A2-A12, rules 3/7/8, content: the checkpoint, no C
python3 eval.py --schema L1 --count 60 --teacher --api openai \
    --model-bin out/model_l1.bin --tok-bin out/tokenizer_l1.bin     # A1: needs section 4's export and runw
python3 make_l1_parity.py --check                                   # R4, Python side (the C side runs in make check)
```

| Row | Script | State |
|---|---|---|
| A1 teacher agreement ≥ 72% | `eval.py --teacher` (above; the key in `$LAB_API_KEY`) | built |
| A2–A12, rules 3/7/8 and the content attractor, on the student | `probe_l1.py --ckpt out/ckpt_l1.pt` | built |
| the same rows on the teacher (what the student inherits) | `prompt_check.py --schema L1` | built |
| A13 heat 4–7 share ≥ 0.7, A14 zones/min ≥ 1.0, A15 crickets/h ≥ 4 | `./fishsim --census-llm ../model/out/model_q4_l1_focus2.bin ../model/out/tokenizer_l1.bin` (from `sim/`; `PT_CENSUS_SEED` for other terrariums) | built: section 7 |
| A16 the A13 census, test pack 250 in the test warm desert | the same with `250 31` after the tokenizer | built: section 7 |
| R4 C/Python encoder parity | `make_l1_parity.py --check`; `./fishsim --selftest-l1-encode` (in `make check`) | built |

`probe_l1.py` reads train.py's PyTorch checkpoint directly (llama2.c's `Transformer`: no export,
no C build) on prompt_check's L1 panel. It uses the same 464 lines (`panel_l1`, seed 11, `--n 40`)
and the same table code (`prompt_check.l1_table`), so teacher and student are scored on the same
states. Each line gets the device's decision distribution. That is BOS, the words and `->`, then
a softmax over the seven goal tokens only at temperature 1.0, as `q4_model_decide_follow`
samples. A row's value is therefore the expected rate of the device's sampled decisions, with no
sampling noise. A11 also gets its whole sweep: the bold-9 lines at bold 0..9, slope > 0, beside
the paired ends ≥ 0.15. The script prints row, value, threshold and PASS/FAIL. It writes
`out/probe_l1_<checkpoint>.json` with the rows, the sweep, the per-case mean distributions and
the goal-token mass after `->` (a sanity check: ~1 for a trained model). `--strict` exits 1 on
a FAIL. `--n 200` reads 2320 states (~15 s on CPU). `python3 probe_l1.py --selftest` checks the
arithmetic against a hand-made fake model (no torch). On the 100-iteration throwaway
(`/tmp/ckpt_probe.pt`, 542 pairs) it ran end to end in ~4 s and FAILed 11 rows (explore ≈ 0.5
everywhere). That proves the plumbing, not a model.

The schema measures A2–A12 "on synthetic and sim states". probe_l1 covers the synthetic half,
the panel. On sim states only the teacher's rates exist so far, in fold's coverage block (see 2).

The rows as `model/schema-lizard.md` states them since 2026-11-04 (and as the pilot,
prompt_check and fold's coverage block pin them): A2 for heat 3–7, A3/A4/A9/A10 in
`phase active`, A7 whatever the heat.

## 6. Focused repair (2026-11-05): A10/A12, the teacher's rule conflict, three seeds

The night's model (`ckpt_l1.pt`, 44,968 pairs, val 0.7333) failed two rows on the fixed probe:
A10 0.377 (≥ 0.40) and A12 0.148 (≤ 0.05). Every other row passed.

**Diagnosis.** `l1_focus.py` asks the teacher about states built for those two rows. The teacher
passed both: A10 kept 0.775, A12 0.006. So the night simply had too few such states. A first
top-up (4,000 focused labels, seed 71) got A10 to 0.56–0.66, but A12 only to 0.046–0.050. Three
more training seeds on those data then put A12 anywhere from 0.024 to 0.209. A split of the
student's A12 by `last` found the leak: P(investigate) was 0.12 when the solitary lizard was
already investigating, and about 0 for every other `last`. The teacher did the same, 0.273 on
150 such pairs (`--a12x-pairs`). Rule 6 (bored 0-2 keeps last) beat rule 7 (social 0-2 never
investigates). A clause in rule 7 alone took the teacher only to 0.187. A sentence in rule 6
itself took it to 0.000. That is the shipped text: "Investigate stops making sense the moment
the lizard is solitary (social 0-2) with the other near ...". Measured on the teacher with that
text: A10 0.863, A12 0.000, A12x 0.000 (social 7-9 still investigates, 0.987); every
prompt_check row OK. Only 7 of the night's 44,968 labels are of the old kind, so the night was
kept; the seed-71 labels (old prompt) were replaced.

**Run** (`run_l1_focus_retrain.sh`, `TRAIN_HOST=marvin`; ~1 h 30 min wall):

```bash
export LAB_API_KEY=...   # as in section 1
TRAIN_HOST=marvin setsid -f nohup bash run_l1_focus_retrain.sh > /dev/null 2>&1 < /dev/null
# log: out/l1_focus2_pipeline.log; reruns resume the teacher labels
```

- **Labels:** 6,000 focused labels, seed 73: A10 2000, A12 1000 pairs, A12x 1000 pairs; 42 min
  at 4 workers. On those labels the teacher kept 0.808 for A10, investigated 0.000 for A12 and
  A12x when solitary, and 0.968 / 0.994 when sociable.
- **Fold:** 50,968 clean pairs.
- **Training:** seeds 1337, 1 and 2 side by side on marvin's Radeon 8060S, in the ROCm container
  (`docker/Dockerfile.rocm-gfx1151`). Each ran at 0.42 s/iter, about 35 min. One seed alone runs
  at 0.15 s/iter, so three in parallel buys nothing over three in a row.
- **Probes:** each seed on two independent panels, `--n 400`: 240 states per row, 10x the fixed
  probe.

| Row (limit) | s1337 p11 / p23 | s1 p11 / p23 | **s2 p11 / p23 (shipped)** |
|---|---|---|---|
| A2 starving hunts (≥ 0.86) | 0.966 / 0.966 | 0.949 / 0.954 | 0.918 / 0.932 |
| A8 stressed hides (≥ 0.70) | 0.993 / 0.991 | 0.980 / 0.982 | 0.968 / 0.972 |
| A10 fresh pastime kept (≥ 0.40) | 0.739 / 0.674 | 0.757 / 0.707 | 0.731 / 0.717 |
| A11 bold 9 - bold 0 (≥ 0.15) | +0.545 / +0.579 | +0.523 / +0.570 | +0.541 / +0.560 |
| A12 solitary investigates (≤ 0.05) | 0.000 / 0.000 | 0.000 / 0.000 | 0.001 / 0.001 |
| r7 sociable investigates (≥ 0.10) | 0.983 / 0.983 | 0.983 / 0.985 | 0.975 / 0.975 |
| content: max single goal (< 0.45) | 0.423 / 0.395 | 0.411 / 0.406 | 0.402 / 0.381 |
| smallest margin over all rows | +0.027 | +0.039 | **+0.048** |

A3-A7 and A9 are ≥ 0.968 on every seed and panel; rules 3 and 8 are positive everywhere. The
shipped seed is the one with the widest smallest margin (chosen by that rule before the run).

- **Shipped:** seed 2 (val 0.7445), `out/model_q4_l1_focus2.bin`, 7.56 MB, max group error 0.0376.
- **A1** (`eval.py --teacher`, n 60, seed 777, greedy fp32 through `runw`): 49/60 (82%) for both
  the night's model and this one. Upstream's v4m shipped at 72%; its teacher agreed with itself
  82% of the time.
- **Still open:** A13-A16, the sim census, need the cricket/render work (P2c/P3b).

Lessons: probe a narrow row by `last` and by trait before adding data; a student that matches its
teacher's slip is learning the prompt, not failing to. Check every candidate on more than one
training seed, because a row near its limit can move 0.2 between seeds of the same data.

## 7. The sim census, A13–A16 (2026-11-05)

```bash
cd sim   # after a terrarium build; each run is one simulated hour, ~30 s
./fishsim --census-llm ../model/out/model_q4_l1_focus2.bin ../model/out/tokenizer_l1.bin          # A13-A15
./fishsim --census-llm ../model/out/model_q4_l1_focus2.bin ../model/out/tokenizer_l1.bin 250 31   # A16
PT_CENSUS_SEED=502 ./fishsim --census-llm ...                                                      # another terrarium
./fishsim --census-llm rules x 250 31                                                              # the rule stub, same bars
```

The model decides at the device's pace (one in flight, 3.7 s each, sampled at 1.0). Results on terrariums 501–505,
three lizards, nobody at the glass:

| Row (bar) | Gecko in the desert | A16: test pack in the test warm desert | Rule stub, gecko |
|---|---|---|---|
| A13 heat 4–7 (≥ 70%) | 98–99% | 96–97% | 98% |
| A14 zones per lizard-minute (≥ 1.0) | 2.56–2.78 | 2.66–2.92 | 2.61 |
| A15 crickets per lizard-hour (≥ 4) | 4.4–5.3 | 4.4–8.9 | 4.9 |

All three rows passed on all ten runs. The night's model also passed A13–A14 but missed A15 once (4.0 on terrarium
502). The model asked was 3.3 times a lizard-minute; the night it slept in a hide or lay still 98% of the time; every
growth shed came off clean.

- **The floor-desert question:** the night's traces came from the flat desert, and the terraces landed mid-run (PLAN P4
  "Known mismatch"). The census runs on the terraces and passes, so no re-mirror and no top-up night.
- **A16's habitat:** the test pack first ran in the gecko's own desert, and failed there. Only the basking spot reaches
  its band, so three lizards cannot share it for 70% of the day. The rule stub managed 16–42%, the model 42–70%; the
  model basked 57–68% of the time against ~10% for the gecko, so it did read the band. A16 now uses the test warm
  desert (habitat 31, test-only), the desert 6 °C warmer throughout (`model/schema-lizard.md`).
- **A15 is the narrowest row:** 4.4 on three of five terrariums for the gecko, against a bar of 4. That rate is set by
  how often the tank drops crickets on its own, not by the model: the rule stub gets the same 4.9. P2d tunes the feeding,
  then this census is the re-check.

## If the night goes wrong

- `out/l1_gen_*.log` fills with `[warn] OSError: curl failed` or `TimeoutError`:
  another client holds the slots; drop to 2–3 workers. A `server: ...` warning
  is the lab's own error text (401: the key is not in the environment).
- A worker dies: the others keep going; restart it as in "A worker died" above (a new
  seed, the pairs it still owes, its own file).
- A skewed label mix: the prompt, not the sim - edit one sentence, re-run
  prompt_check, regenerate only that worker's file.
