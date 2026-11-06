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

## Extra tools

- `model/prompt_check.py` — 7-minute teacher/prompt sanity check. **Run this
  before any long generation run**; a subtly broken prompt wastes a night.
- `model/probe_dist.py` — the student's full goal *distribution* on real and
  synthetic states (evidence behind [progression-next.md](progression-next.md))
- `model/teacher_vs_student.py` — side-by-side decisions for the same states
- Schema v3 cycle: `--schema 3` on gen_traces / train_tokenizer / eval,
  `POCKET_SCHEMA=3` for train.py — runbook in [retrain-v3.md](retrain-v3.md)
