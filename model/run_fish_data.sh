#!/bin/bash
# run_fish_data.sh - the fish model's schema-4 labels from the lab's teacher, for the fish size
# sweep (docs/pipeline.md, "Fish size sweep"; issue #14). It stands in for step 1 of
# run_v4_overnight.sh (Ollama on the Mac Mini, whose data is not on this machine or in git):
# the same gen_traces.py --schema 4 states and SYSTEM_PROMPT_V4, asked of chat-teacher on
# the lab. Four workers (chat-teacher has four slots), one seed and one output file each.
#
# Needs the lab serving chat-teacher (NOT training mode) and LAB_API_KEY in the environment.
# Run `python3 prompt_check.py --schema 4 --api openai` first (450 calls, ~3 min): every row
# but dart | bold 9 must be OK (2026-11-05 chat-teacher: that one reads 0.05, the others pass).
#
#   LAB_API_KEY=... COUNT=6000 bash run_fish_data.sh        # 24,000 pairs
#
# gen_traces.py appends to its --out and a seed makes the same states, so rerunning after an
# interruption asks the teacher for states it already labelled: fold_traces.py drops the exact
# duplicates, at the price of the repeated calls. Set SEED_BASE to a new number to continue with
# fresh states instead. The fold and the harness's dataset pin come after this (run_fish_sweep.sh).
set -uo pipefail
cd "$(dirname "$0")"
LOG=out/fish_data.log
COUNT=${COUNT:-6000}
SEED_BASE=${SEED_BASE:-141}
HOST=${HOST:-http://zaphod.tail740235.ts.net:8080/v1}
MODEL=${MODEL:-chat-teacher}
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }
[ -n "${LAB_API_KEY:-}" ] || { echo "run_fish_data: LAB_API_KEY is not set" >&2; exit 1; }

mkdir -p out
log "fish v4 labels: $COUNT pairs x 4 workers, seeds $SEED_BASE.., $MODEL @ $HOST"
pids=()
for w in 0 1 2 3; do
    s=$(printf "\\x$(printf %x $((97 + w)))")      # a b c d
    POCKET_SCHEMA=4 python3 gen_traces.py --schema 4 --api openai --host "$HOST" --model "$MODEL" \
        --count "$COUNT" --seed $((SEED_BASE + w)) --out "out/fish_v4_traces_${SEED_BASE}_$s.jsonl" \
        > "out/fish_v4_gen_${SEED_BASE}_$s.log" 2>&1 &
    pids+=($!)
    log "worker $s pid $! seed $((SEED_BASE + w)) -> out/fish_v4_traces_${SEED_BASE}_$s.jsonl"
done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
log "workers done (rc $rc): $(cat out/fish_v4_traces_${SEED_BASE}_*.jsonl | wc -l | tr -d ' ') raw pairs"
python3 fold_traces.py --schema 4 out/fish_v4_traces_*.jsonl --out out/fish_v4_clean.jsonl 2>&1 | tee -a "$LOG"
log "fish data finished: out/fish_v4_clean.jsonl; pin its sha256 in harness/datasets.toml (\"fish-v4\")"
exit $rc
