#!/bin/bash
# run_l1_after_traces.sh - waits for the L1 trace workers to finish, then
# fold -> tokenizer -> train (CPU, 5000 iters) -> export -> probe_l1 (A2-A12).
# docs/retrain-lizard.md sections 2-5. Logs to out/l1_pipeline.log.
set -euo pipefail
cd "$(dirname "$0")"
LOG=out/l1_pipeline.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

while pgrep -f 'gen_traces.py --schema L1' > /dev/null; do sleep 60; done
log "traces done: $(cat out/l1_traces_*.jsonl | wc -l) lines"

python3 fold_traces.py --schema L1 out/l1_traces_*.jsonl --out out/l1_clean.jsonl >> "$LOG" 2>&1
python3 train_tokenizer.py --schema L1 --verify out/l1_clean.jsonl >> "$LOG" 2>&1
log "folded: $(wc -l < out/l1_clean.jsonl) clean pairs; training"

POCKET_SCHEMA=L1 .venv/bin/python train.py --data out/l1_clean.jsonl --device cpu --iters 5000 \
    --dim 384 --n-layers 8 --n-heads 8 --max-seq-len 64 --batch 64 --lr 6e-4 --out out/ckpt_l1.pt >> "$LOG" 2>&1
log "trained: $(grep 'best val_loss' "$LOG" | tail -1)"

.venv/bin/python export_q4.py out/model_q4_l1.bin --checkpoint out/ckpt_l1.pt >> "$LOG" 2>&1
.venv/bin/python llama2.c/export.py out/model_l1.bin --checkpoint out/ckpt_l1.pt >> "$LOG" 2>&1
log "exported: $(ls -la out/model_q4_l1.bin | awk '{print $5}') bytes (4-bit)"

.venv/bin/python probe_l1.py --ckpt out/ckpt_l1.pt >> "$LOG" 2>&1 || true
log "probe done (table above); pipeline finished"
