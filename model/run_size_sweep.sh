#!/bin/bash
# run_size_sweep.sh - the model-size sweep (docs/pipeline.md, "Size sweep"): six sizes around the
# 14M, each trained with the loss on every token and on the decision tokens only, three seeds
# per run, one seed per card. Same data, seeds, iterations and probes as l1-focus2, so every
# row compares directly with that run (the 14M baselines are harness/runs/l1-focus2*.toml).
#
# By default it needs the lab in training mode on both hosts (`lab training-mode on` on zaphod):
# the harness refuses otherwise. SWEEP_MODE=opportunistic shares the cards with the served models
# instead (slower: a card with too little free memory waits; the guard only checks at a job's
# start). Needs no teacher and no LAB_API_KEY. The harness resumes finished jobs
# and this script skips runs that already have a report, so rerunning continues where it
# stopped. A run that fails is logged and the sweep goes on. In training mode, turn the lab back
# with `lab training-mode off` when the log says the sweep finished.
set -uo pipefail
cd "$(dirname "$0")"
LOG=out/size_sweep.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

SIZES="d384-n4 d256-n8 d256-n6 d256-n4 d192-n6 d192-n4"
[ -x out/runq4 ] || gcc -O3 -o out/runq4 runq4.c -lm

failed=""
for size in $SIZES; do
    for loss in all decision; do
        run="size-$size-$loss"
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
        [ -f "out/harness/size-$size-$loss/report.json" ] && specs="$specs size-$size-$loss"
    done
done
BASE=${BASELINE_DIR:-out/harness}
refs=""
for r in l1-focus2 l1-focus2-decision; do
    [ -f "$BASE/$r/report.json" ] && refs="$refs $r=$BASE/$r"
done
# shellcheck disable=SC2086
python3 size_report.py --reference l1-focus2 --markdown out/harness/size-report.md $refs $specs >> "$LOG" 2>&1
log "sweep finished${failed:+; FAILED:$failed}: out/harness/size-report.md"
