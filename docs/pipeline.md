# Training pipeline — from teacher to flashable binary

The distillation loop end to end. The teacher is any Ollama-served model
(the shipped weights were taught by gemma4:26b); set `--host` /
`HOST` to your Ollama server (default `http://localhost:11434`).

Training needs PyTorch — use a virtualenv:

```bash
python3 -m venv .venv && .venv/bin/pip install torch
```

`model/llama2.c/` is a clone of
[karpathy/llama2.c](https://github.com/karpathy/llama2.c) (training script +
reference inference):

```bash
git clone https://github.com/karpathy/llama2.c model/llama2.c
```

## The steps

```bash
cd model/llama2.c && make run && cd ../..                # 1. native llama2.c build

python3 model/gen_traces.py --count 10 --dry-run         # 2a. sample encoded states, no network
python3 model/gen_traces.py --count 3000 --max-minutes 45 --seed 1 \
    --out model/out/traces_x1.jsonl                      # 2b. generate teacher-labeled traces
                                                         #     (run several workers with distinct seeds/outs)

python3 model/fold_traces.py                             #     dedup + vocabulary check + label distribution
python3 model/train_tokenizer.py                         # 3. word-level tokenizer, vocab 58, no deps

.venv/bin/python model/train.py --data model/out/clean.jsonl \
    --dim 384 --n-layers 8 --n-heads 8 --max-seq-len 64 \
    --batch 64 --iters 4000 --lr 6e-4                    # 4. train (torch)

.venv/bin/python model/llama2.c/export.py model/out/model.bin \
    --checkpoint model/out/ckpt.pt                       # 5. export fp32 (--version 2 for int8)
python3 model/export_q4.py                               #    → 4-bit model_q4.bin

cc -O3 -o model/runw model/runw.c -lm                    # 6a. word-tokenizing runner
./model/runw model/out/model.bin -z model/out/tokenizer.bin \
    -t 0 -i "<state line> ->"                            # 6b. test a decision

python3 model/eval.py --count 60 --teacher               # 7. goal agreement vs the teacher
```

The prompt in step 6 is any [schema.md](../model/schema.md) state line with
` ->` appended; the model completes it with `<goal> urgency <0-9>` and stops.

## Training harness

`model/harness.py` runs step 4 onwards for declared experiments across the lab's GPU cards:
fold, train every seed (one job per card), probe, export, select. Config is TOML (stdlib,
no dependency) in `model/harness/`:

| File | Declares |
|---|---|
| `models.toml` | architectures (`l1-14m`: dim 384, 8 layers, 8 heads, seq 64) and the memory one job takes |
| `datasets.toml` | trace globs folded by `fold_traces.py`; an optional sha256 pins the folded file |
| `hosts.toml` | hosts, cards (PCI address, arch, memory kind, reserve, teacher card), images per arch |
| `runs/<run>.toml` | model, dataset, iters/batch/lr, seeds, probe panels, mode, CPU allowed, a validation reference |

```bash
cd model
python3 harness.py plan l1-focus2                      # the lab mode per host, every card's verdict, job -> card
python3 harness.py run l1-focus2                       # train, probe, export; ends with the report
python3 harness.py run smoke --mode opportunistic      # the whole path in ~5 min (300 iters)
python3 harness.py status l1-focus2                    # done / running where / queued / failed
python3 harness.py report l1-focus2                    # rebuild out/harness/l1-focus2/report.md
python3 harness.py selftest                            # planner, guard, margins, selection, validation
```

- **Modes.** `training` (the default for real runs) needs every host's llama-swap in the lab's
  `training` profile, which `local-ai-lab` owns. `opportunistic` places jobs next to the served
  models. It never evicts one: a card is used only if its free memory covers the model's
  `mem_mib` plus the card's `reserve_mib`, or a job waits. The teacher's card is skipped while
  `chat-teacher` is loaded. `plan` prints the mode it found on each host.
- **Cards.** One job per card. The container gets `/dev/kfd` and that card's render node only,
  looked up from its PCI address at launch. Measured 2026-11-05: an R9700 (gfx1201,
  `docker/Dockerfile.rocm-gfx120X`) at 0.06 s/iter, the 8060S at 0.15; peak 1.4–1.6 GB.
- **Robustness.** A job is finished only when its `exit_code` file exists on the host: a failed
  ssh is "unknown". A container gone without one crashed, and the job is retried once. Killing
  the harness (or its ssh) and rerunning `run` adopts running jobs and collects finished ones.
- **Outputs**, per job in `out/harness/<run>/s<seed>/`: `ckpt.pt`, `model_q4.bin`,
  `model_fp32.bin`, the probe JSONs, the logs, and `manifest.json`. The manifest records the git
  SHA, dataset hash, configs, host/card, image ID, torch/HIP, s/iter, peak memory, and the
  kept checkpoint's val loss, goal NLL and goal accuracy.
- **Goal metrics** (train.py, every eval): on whole held-out docs, the label goal's NLL under the
  device's softmax over the goal tokens only (`goal_nll`), how often the top goal is the label
  (`goal_acc`), and the loss on the decision tokens. `val_loss` is mostly the state words, which
  the device never generates. A run's `loss = "decision"` (`train.py --loss decision`) trains on
  the tokens after `->` only and keeps the checkpoint with the best goal NLL.
- **Report**, in `out/harness/<run>/report.md`: jobs, every row × seed × panel with its limit,
  the seed-to-seed sd against the panel-to-panel spread, each seed's smallest margin, the
  selected seed, and the check against `[validate]` if the run has one. Margins are scaled by
  the row's headroom (limit to best possible: 1 for a rate, 1/9 for the A11 slope, 0 for a
  `<=` row), so a row pinned near its floor (A12, limit 0.05) can't decide every ranking.
  Selection takes the widest smallest scaled margin. Seeds within the binding rows' scaled panel
  spread of it (at least `tie_floor`, 0.005) are tied and go to the lower goal NLL.
- **Teacher agreement**, `model/agree.py --ckpt A.pt B.pt ...`: a fixed, student-independent
  state set (seed 777, 300 states), K = 3 teacher answers each, cached in
  `out/agree_teacher_cache.jsonl` so further checkpoints cost no teacher calls. It reports greedy
  agreement (A1 without its noise), expected agreement under the device's sampling, the
  teacher's own self-agreement, their ratio, and NLL, each with a 95% bootstrap CI.
  `--api rules` runs it offline; `--selftest` checks the arithmetic.
- **Curiosity panel**, `model/curiosity.py student --ckpt ... --out FILE`: matched states in which
  only `curiosity` varies (0/3/6/9; nine cases: calm, hungry, hot, cold, stressed, exhausted,
  sleep, shed, a solitary lizard with another near), the checkpoints' goal probabilities on them,
  and the paired 9-0 change per case. `teacher` mode asks the teacher for the same states
  (capped, cached, resumable) and writes focused `{state, goal}` labels. No thresholds: it
  describes. Measured on the l1-focus2 checkpoints, see issue #7.

## Size sweep

`model/run_size_sweep.sh` trains six sizes around the 14M (`harness/models.toml`: 384x4, 256x8,
256x6, 256x4, 192x6, 192x4; dim and the FFN width are multiples of the q4 group size 64), each
with the loss on every token and on the decision tokens only, three seeds per run, on the same
data, iterations and probes as `l1-focus2` (`harness/runs/size-*.toml`). Training mode is the
default; `SWEEP_MODE=opportunistic` shares the cards with the served models. It needs no teacher.
The result is `out/harness/size-report.md` (`model/size_report.py`): per run the parameters,
q4 size, the q4 runner's speed on this machine (`out/runq4`, `gcc -O3 -o out/runq4 runq4.c -lm`),
a pendant speed projected from the pendant's measured 11.9 tok/s at 14M (a projection:
memory bandwidth differs between an x86 core and the ESP32-S3), goal NLL and accuracy, how many
seeds pass every probe row, the worst scaled margin and the rows a seed fails. A smaller model
passes only if every seed passes every row; the table is evidence, not a promotion rule.

Measured on the pendant (1.75C, 2026-11-06): the 256x4 decision-only model (3.2M, 1.72 MB, seed 2 of
`size-d256-n4-decision`), flashed as model partition + trailer only (`lz_d256n4`; the app and the
save untouched), decides in about 1.18 s at 40.6-41.3 tok/s with 84 KB of internal heap free, against
about 4 s at 11.9 tok/s for the 14M. The projection from the x86 runner (51.6 tok/s) was 25% high.
Sim census A15 (crickets per lit lizard-hour, bar >= 4; one catch is about 0.44) over 30 terrariums:
the 14M missed 0, 256x4 missed 5 (each at 3.6-4.0), 192x4 missed 2; the means match (about 5.6-5.8).

## Fish size sweep

The same sweep on the fish model (schema 4; issue #14). Nothing here ships a model: the aquarium
stays on upstream's `model_q4.bin` until someone decides otherwise.

**Data.** The fish traces are not on this workstation, on marvin or zaphod, or in git
(`model/out/*` is ignored upstream), and the shipped model's `.pt` checkpoint is gone, so the data
is regenerated: `model/run_fish_data.sh` asks `chat-teacher` (the lab; four workers, key in
`LAB_API_KEY`) for `gen_traces.py --schema 4` labels, 24,000 pairs by default, then folds them to
`out/fish_v4_clean.jsonl`. Pin that fold's sha256 under `["fish-v4"]` in `harness/datasets.toml`.
These are v4-only labels, not upstream's v4m mix (23,366 new + 28,247 v2/v3 pairs re-encoded by
`convert_to_v4.py`), whose old labels carried the personality cliffs. So `fish-14m` trained here is
the sweep's reference, not a reproduction of the shipped model; the shipped model enters only as
an outside reference in the sim census. Run `python3 prompt_check.py --schema 4 --api openai`
first: on 2026-11-05 `chat-teacher` passed every row except dart | bold 9 (0.05 against 0.10).
Upstream's tag for the shipped fish model in `common/version.h` reads `v3m`, but its tokenizer is
byte-identical to a fresh `--schema 4` one (md5 7e4b64d9...), so it is a v4 model.

**Runs.** `harness/models.toml` `fish-14m` (384x8, the reference) and the six sizes of the lizard
sweep, `harness/runs/fish-*.toml` for every size with the loss on every token and on the decision
tokens only, three seeds each, probes n=40 on seeds 11 and 23, `model/run_fish_sweep.sh` for the
lot (14 runs; `SWEEP_MODE=opportunistic` shares the cards, but the R9700 that serves the teacher is
skipped while `chat-teacher` is loaded). `size_report.py --schema 4` writes
`out/harness/fish-size-report.md`; it has no pendant projection until the fish 14M is measured on the
pendant (`--pendant-toks`).

**Rows** (`prompt_check.v4_table`, one table for the teacher and for `model/probe_fish.py`, which scores
a checkpoint's expected rates on the v4 panel by the device's softmax over the fish's eight goal
tokens; flee_shadow is in the vocabulary, though no label uses it). Limits are the ones
`prompt_check.py` has always printed for schema 4, fixed before any run:

| row | limit |
|---|---|
| F1 P(follow \| social 9) | >= 0.40 |
| F2 P(follow \| social 1) | <= 0.05 |
| F3 P(dart \| bold 9, energetic) | report only (the teacher itself gives 0.05) |
| F4 P(dart \| bold 1, energetic) | <= 0.03 |
| F5 P(seek_food \| starving) | >= 0.95 |
| F6 P(seek_food \| lonely, not hungry) | <= 0.05 |
| F7 P(bubbles again \| bored 9, last bubbles) | <= 0.15 |
| F8 P(explore \| bored 9, last bubbles) | >= 0.25 |
| F9 P(follow again \| bored 9, last follow) | <= 0.15 |
| F10 P(bubbles kept \| bored 0, last bubbles) | >= 0.40 |
| F11 P(rest \| night, bored 9, last rest) | >= 0.70 |
| F12 P(flee \| any v4 case) | <= 0.001 (pass/fail, no margin) |
| F13 max single goal \| content | < 0.45 |

**Sim census.** `model/fish_census.py` runs the aquarium sim's `--selftest-llm` on any model and
tokenizer without editing the sim (scratch directories of symlinks; build `sim/fishsim-aquarium`
first, command in its docstring). `--minutes 0` is a one-simulated-minute smoke; for a comparison use
real time (`--minutes 5 --repeats 3`). The sim's seed is fixed, so repeats differ by sampling only.
It prints goal shares, landmark time, crowding, zones per fish-minute, longest stretch, torn
decisions and survival overrides. There is no rule stub for the fish and no pass bar: a candidate is
read against the fish model it replaces on the same tanks.

**Results (2026-11-06, `chat-teacher` labels, 24,056 clean pairs, sha256 e7874137...; 14 runs, 3 seeds each,
opportunistic mode, about 1 h 45 min).** The labels are not upstream's: seek_food is 55.6% (upstream's
gemma4:26b: 35%), rest 24%, explore 11%, follow_friend 3.6%, inspect_reef 3.4%, visit_bubbles 1.7%. With
food in view in 97% of the generator's states the teacher eats on 43-63% of the hunger 1-3 ones.

- **No run passes every row, the 14M reference included**, so the table cannot say "the smallest size that
  passes". The rows every run fails track the skew: F6 (seek_food | lonely, not hungry; the 14M reads 0.16-0.17
  against <= 0.05), F13 (content: explore 0.45-0.54 against < 0.45) and F10 (bubbles kept: 0.25-0.57 against
  >= 0.40, from 1.7% visit_bubbles labels). F5 (starving), F8 (bored explore), F11 (night rest) and F12 (no
  flee) pass at every size and in both loss modes. Failing values out of 6 (3 seeds x 2 panels), summed over
  the rows: 14M all-token 16, 14M decision-only 20, 384x4 26 / 19, 256x8 15 / 12, 256x6 13 / 11, 256x4 17 / 15,
  192x6 14 / 18, 192x4 19 / 11 (all-token / decision-only). That spread is inside the seed noise.
- **One size-linked failure.** 256x4 decision-only reads F1 (follow | social 9) = 0.05, 6 of 6 values failing,
  against 0.54-0.65 for the 14M. 384x4 reads 0.33-0.39 on F1. The other sizes sit at 0.38-0.62.
- **Goal NLL** is lower with the decision-only loss at every size (14M 0.258 against 0.280; 1.78M 0.243 against
  0.276), but unlike the lizard sweep that does not carry over to the rows (the 14M decision-only run fails 20
  values against 16).
- **Speed on this machine** (q4 runner, 46-token fish prompt): 14M 125 tok/s, 7.1M 247, 6.4M 271, 4.8M 362,
  3.2M 545, 2.7M 646, 1.78M 955-981. The lizard sweep's pendant speedups were 74% of the x86 ones (3.2M: 3.4x
  against 4.6x), so about the same ratios should hold for the fish; none of this was measured on the pendant.
- **Sim census** (`fish_census.py --minutes 5 --repeats 3`, selected seed of each run; 3 repeats of 300 s,
  zero survival overrides everywhere): the 14M trained here differs from the shipped model more than any size
  differs from the 14M. Goal share, shipped / this-data 14M: inspect_reef 27 / 6%, follow_friend 17 / 28%,
  seek_food 2 / 13%, rest 0 / 2%, explore 36 / 32%, visit_bubbles 16 / 19%; time at a landmark 54 / 36%,
  mean bored 4.6 / 2.8. Across the eight trained candidates inspect_reef is 4-15%, follow_friend 23-37%,
  seek_food 4-15%, with no trend by size, and zones per fish-minute 3.6-4.4 against 3.4 shipped. The census
  asks at the sim's own rhythm (108-133 asks per 300 s whatever the model's speed), so it does not model the
  decision speed.
- **What this supports.** Sizes down to 1.78M behave like the 14M trained on the same labels, on these probes
  and this census, with 256x4 decision-only and 384x4 the exceptions on F1. It does not say a small model
  would behave like the shipped fish model: the labels are what differ. Not run: any candidate on the pendant,
  the shipped model's probe rows (its checkpoint is gone, so no student-side F rows exist for it), a mix with
  re-encoded v2/v3 data, or labels from a balanced prompt.

## Extra tools

- `model/prompt_check.py` — 7-minute teacher/prompt sanity check. **Run this
  before any long generation run**; a subtly broken prompt wastes a night.
- `model/probe_dist.py` — the student's full goal *distribution* on real and
  synthetic states (evidence behind [progression-next.md](progression-next.md))
- `model/teacher_vs_student.py` — side-by-side decisions for the same states
- Schema v3 cycle: `--schema 3` on gen_traces / train_tokenizer / eval,
  `POCKET_SCHEMA=3` for train.py — runbook in [retrain-v3.md](retrain-v3.md)
