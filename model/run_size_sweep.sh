#!/bin/bash
# run_size_sweep.sh - the model-size sweep (docs/pipeline.md, "Size sweep"): six sizes around the
# 14M, each trained with the loss on every token and on the decision tokens only, three seeds
# per run, one seed per card. Same data, seeds, iterations and probes as l1-focus2, so every
# row compares directly with that run (the 14M baselines are harness/runs/l1-focus2*.toml).
#
# By default it needs the lab in training mode on both hosts (`lab training-mode on` on zaphod):
# the harness refuses otherwise. SWEEP_MODE=opportunistic shares the cards with the served models
# instead (slower: a card with too little free memory waits; the guard only checks at a job's
# start). Needs no teacher and no LAB_API_KEY. The harness resumes finished jobs, and this script
# skips a run only when its report holds EVERY configured seed (size_report.py --complete): a
# report written after a failed job does not count, so rerunning continues where it stopped. A
# run that fails is logged and the sweep goes on, but the exit status says so: 0 only when every
# run is complete and the report was written; 1 when a run failed or is incomplete, or the
# report could not be generated. In training mode, turn the lab back with `lab training-mode off`
# when the log says the sweep finished.
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
        if python3 size_report.py --complete "$run"; then
            log "$run: report complete, skipped"
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

# every run that has a report goes in the table, a partial one too (the table labels it INCOMPLETE and names the missing seeds)
specs=""
incomplete=""
for size in $SIZES; do
    for loss in all decision; do
        run="size-$size-$loss"
        [ -f "out/harness/$run/report.json" ] && specs="$specs $run"
        python3 size_report.py --complete "$run" || incomplete="$incomplete $run"
    done
done
BASE=${BASELINE_DIR:-out/harness}
refs=""
for r in l1-focus2 l1-focus2-decision; do
    [ -f "$BASE/$r/report.json" ] && refs="$refs $r=$BASE/$r"
done
# the pendant projection scales from the 14M baseline's speed: without its report there is no projection, not another model's
refarg=""
case "$refs" in
    *" l1-focus2="*) refarg="--reference l1-focus2" ;;
    *) log "no l1-focus2 baseline report under $BASE: the table has no pendant projection" ;;
esac
report_ok=1
# shellcheck disable=SC2086
python3 size_report.py $refarg --markdown out/harness/size-report.md $refs $specs >> "$LOG" 2>&1 || report_ok=0

rc=0
[ -n "$failed" ] && rc=1
[ -n "$incomplete" ] && rc=1
[ "$report_ok" = 1 ] || rc=1
msg="sweep finished"
[ -n "$failed" ] && msg="$msg; FAILED:$failed"
[ -n "$incomplete" ] && msg="$msg; INCOMPLETE (not every seed collected):$incomplete"
if [ "$report_ok" = 1 ]; then
    log "$msg: out/harness/size-report.md"
else
    log "$msg; THE REPORT WAS NOT GENERATED (size_report.py failed: see the log above)"
fi
exit $rc
