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
# Then three training seeds (one model can pass or fail by luck), each probed on two large panels
# (n 400: 240 states per row, 10x the fixed probe). Writes *_focus2 artifacts; the earlier models
# stay. Needs LAB_API_KEY. Rerunning resumes the teacher labels.
# TRAIN_HOST=marvin trains over ssh on marvin's Radeon 8060S (Strix Halo, gfx1151) inside
# docker/Dockerfile.rocm-gfx1151 (AMD's nightly ROCm PyTorch; nothing installed on the host), in
# ~/pt-train/model there: 0.15 s/iter against 1.22 on this CPU, the same loss curve.
# Unset: trains here on the CPU, one seed after another.
set -euo pipefail
cd "$(dirname "$0")"
LOG=out/l1_focus2_pipeline.log
SEEDS="1337 1 2"
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

log "focused teacher labels: A10 2000, A12 1000 pairs, A12x 1000 pairs, seed 73"
python3 l1_focus.py --a10 2000 --a12-pairs 1000 --a12x-pairs 1000 --seed 73 --api openai --workers 4 \
    --max-requests 6000 --traces-out out/l1_focus2_traces.jsonl \
    --report-out out/l1_focus2_topup_report.json >> "$LOG" 2>&1
log "labels done: $(wc -l < out/l1_focus2_traces.jsonl) lines"

python3 fold_traces.py --schema L1 out/l1_traces_*.jsonl out/l1_focus2_traces.jsonl \
    --out out/l1_clean_focus2.jsonl >> "$LOG" 2>&1
python3 train_tokenizer.py --schema L1 --verify out/l1_clean_focus2.jsonl >> "$LOG" 2>&1
log "folded: $(wc -l < out/l1_clean_focus2.jsonl) clean pairs; training seeds $SEEDS"

TRAIN_ARGS="--data out/l1_clean_focus2.jsonl --iters 5000 --dim 384 --n-layers 8 --n-heads 8 --max-seq-len 64 --batch 64 --lr 6e-4"
if [ -n "${TRAIN_HOST:-}" ]; then
    R=pt-train/model IMG=pt-train-rocm:gfx1151
    rsync -a --exclude out --exclude .venv --exclude __pycache__ ./ "$TRAIN_HOST:$R/"
    rsync -a out/l1_clean_focus2.jsonl out/tokenizer_l1.bin "$TRAIN_HOST:$R/out/"
    ssh -o BatchMode=yes "$TRAIN_HOST" "cd $R && (docker image inspect $IMG > /dev/null 2>&1 || \
        docker build -q -t $IMG -f docker/Dockerfile.rocm-gfx1151 docker)" >> "$LOG" 2>&1
    for s in $SEEDS; do   # the GPU runs the three side by side
        ssh -o BatchMode=yes "$TRAIN_HOST" "cd $R && rm -f out/ckpt_l1_focus2_s$s.pt out/train_focus2_s$s.log && \
            docker run -d --rm --name pt-focus2-s$s --device /dev/kfd --device /dev/dri \
              --group-add \$(getent group render | cut -d: -f3) --group-add \$(getent group video | cut -d: -f3) \
              --security-opt seccomp=unconfined -u \$(id -u):\$(id -g) -e HOME=/tmp -e USER=\$USER -e LOGNAME=\$USER \
              -e POCKET_SCHEMA=L1 -v \$HOME/$R:/w $IMG sh -c 'python train.py $TRAIN_ARGS --device cuda --seed $s \
              --out out/ckpt_l1_focus2_s$s.pt > out/train_focus2_s$s.log 2>&1'" >> "$LOG" 2>&1
    done
    log "training on $TRAIN_HOST's GPU (containers pt-focus2-s*)"
    while true; do   # a failed ssh is not "finished": only an answer with no container is
        s=$(ssh -o BatchMode=yes -o ConnectTimeout=20 "$TRAIN_HOST" 'docker ps -q -f name=pt-focus2-s') || { sleep 30; continue; }
        [ -z "$s" ] && break; sleep 30
    done
    for s in $SEEDS; do
        rsync -a "$TRAIN_HOST:$R/out/ckpt_l1_focus2_s$s.pt" "$TRAIN_HOST:$R/out/train_focus2_s$s.log" out/
        tail -1 out/train_focus2_s$s.log | sed "s/^/seed $s: /" >> "$LOG"
    done
else
    for s in $SEEDS; do
        POCKET_SCHEMA=L1 .venv/bin/python train.py $TRAIN_ARGS --device cpu --seed $s \
            --out out/ckpt_l1_focus2_s$s.pt > out/train_focus2_s$s.log 2>&1
        tail -1 out/train_focus2_s$s.log | sed "s/^/seed $s: /" >> "$LOG"
    done
fi
log "trained"

for s in $SEEDS; do
    for p in 11 23; do
        .venv/bin/python probe_l1.py --ckpt out/ckpt_l1_focus2_s$s.pt --n 400 --seed $p \
            --out out/probe_l1_focus2_s${s}_p$p.json >> "$LOG" 2>&1 || true
    done
    log "seed $s probed (panels 11 and 23, n 400)"
done
log "pipeline finished; pick a seed, then export_q4.py it to out/model_q4_l1_focus2.bin"
