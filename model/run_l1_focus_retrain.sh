#!/bin/bash
# run_l1_focus_retrain.sh - the A10/A12 repair (docs/retrain-lizard.md, "Focused top-up").
#
# The first L1 student failed A10 (0.38: keep a fresh pastime) and A12 (0.15: a solitary lizard
# investigating). A first top-up (4,000 focused labels) got A10 to 0.56-0.66 but A12 only to
# 0.046-0.050: the leak was all in states where the solitary lizard had just been investigating
# (P 0.12 there, ~0 elsewhere). The teacher did the same (0.27): rule 6 "keep last" beat rule 7
# "never investigates". SYSTEM_PROMPT_L1 now says investigate stops making sense for a solitary
# lizard with the other near; on the teacher that took the case to 0.00 and A10 to 0.86 with every
# prompt_check row passing. Only 7 of the night's 44,968 labels are of the old kind, so the night
# stays; the old 4,000 focused labels are replaced by these, all under the fixed prompt.
#
# Then harness.py runs harness/runs/l1-focus2.toml: it folds the dataset, trains the three seeds
# (one model can pass or fail by luck) one per card, probes each on two n-400 panels, exports,
# and writes out/harness/l1-focus2/report.md with the selected seed. Needs LAB_API_KEY for the
# labels. Rerunning resumes the teacher labels, and the harness resumes its jobs.
set -euo pipefail
cd "$(dirname "$0")"
LOG=out/l1_focus2_pipeline.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

log "focused teacher labels: A10 2000, A12 1000 pairs, A12x 1000 pairs, seed 73"
python3 l1_focus.py --a10 2000 --a12-pairs 1000 --a12x-pairs 1000 --seed 73 --api openai --workers 4 \
    --max-requests 6000 --traces-out out/l1_focus2_traces.jsonl \
    --report-out out/l1_focus2_topup_report.json >> "$LOG" 2>&1
log "labels done: $(wc -l < out/l1_focus2_traces.jsonl) lines; training through the harness"

python3 harness.py run l1-focus2 "$@" >> "$LOG" 2>&1
log "pipeline finished: out/harness/l1-focus2/report.md names the seed; its model_q4.bin is the candidate"
