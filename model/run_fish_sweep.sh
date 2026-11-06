#!/bin/bash
# run_fish_sweep.sh - the fish model's size sweep (docs/pipeline.md, "Fish size sweep"): the 14M
# reference and the six sizes of run_size_sweep.sh on schema 4 (harness/runs/fish-*.toml), each
# trained with the loss on every token and on the decision tokens only, three seeds per run, one
# seed per card, on the dataset "fish-v4" (out/fish_v4_traces_*.jsonl, made by gen_traces.py
# --schema 4; this script generates nothing). Probes are probe_fish.py's.
#
# Same conventions as run_size_sweep.sh: the lab in training mode on both hosts unless
# SWEEP_MODE=opportunistic; finished runs (a report.json) are skipped, so rerunning continues; a
# failed run is logged and the sweep goes on; no teacher, no LAB_API_KEY.
set -uo pipefail
cd "$(dirname "$0")"
LOG=out/fish_sweep.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

SIZES="14m d384-n4 d256-n8 d256-n6 d256-n4 d192-n6 d192-n4"
[ -x out/runq4 ] || gcc -O3 -o out/runq4 runq4.c -lm

failed=""
for size in $SIZES; do
    for loss in all decision; do
        run="fish-$size-$loss"
        if [ -f "out/harness/$run/report.json" ]; then
            log "$run: report exists, skipped"
            continue
        fi
        log "$run: starting"
        if python3 harness.py run "$run" --poll 20 ${SWEEP_MODE:+--mode "$SWEEP_MODE"} >> "$LOG" 2>&1; then
            log "$run: done"
        else
            log "$run: FAILED (rc $?); see the log above and out/harness/$run/"
            failed="$failed $run"
        fi
    done
done

specs=""
for size in $SIZES; do
    for loss in all decision; do
        [ -f "out/harness/fish-$size-$loss/report.json" ] && specs="$specs fish-$size-$loss"
    done
done
# shellcheck disable=SC2086
python3 size_report.py --schema 4 --reference fish-14m-all \
    --markdown out/harness/fish-size-report.md $specs >> "$LOG" 2>&1
log "sweep finished${failed:+; FAILED:$failed}: out/harness/fish-size-report.md"
