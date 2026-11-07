#!/bin/bash
# qemu_models.sh - boot the QEMU build with several model files and report each one's first decision:
# does the firmware load the model, how long the emulator takes, and at what tok/s.
#
#   qemu_models.sh [-n BOOTS] [-t SECONDS] [-b BUILD_DIR] [-o OUT_DIR] NAME=path/model_q4.bin ...
#
# Build the QEMU image first: the overlay (quad 4 MB PSRAM, no panel, no signing) and the scalar dot
# kernels (QEMU does not emulate the S3's PIE vector unit; common/llm/q4_model.c: POCKET_TANK_NO_PIE),
# the aquarium's:
#   . ~/esp/esp-idf/export.sh; B=~/.cache/pocket-terrarium/fw-build-qemu-aq
#   idf.py -B $B -DSDKCONFIG=$B/sdkconfig -DCMAKE_C_FLAGS="-DPOCKET_TANK_NO_PIE" \
#     -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu;sdkconfig.aquarium" build
# (the terrarium's: drop sdkconfig.aquarium). The world's tokenizer is linked in from
# firmware/main/tokenizer.bin; the build also wants model/out/model_q4.bin to exist.
#
# Each boot, every candidate gets its own flash image (bootloader, partition table, app, the model at
# 0x290000 and its trailer at 0xA8F000) and its own emulator, all at once: the emulator runs at the host
# core's speed, so keep the count at or below its cores. A fresh tank waits in its first-run setup for a
# panel to answer it, and the emulator has no way to send the director `setup off` (it listens on the
# USB-Serial-JTAG, which the machine does not connect), so only the first decision (about 2 s after
# boot) happens; the median of BOOTS boots is the figure. The emulator is not cycle-accurate and runs
# the scalar kernels: tok/s compare the models with each other and say nothing about the pendant. The
# internal-heap figures belong to this build (no panel), not to the pendant.
set -euo pipefail
cd "$(dirname "$0")"
BOOTS=5
SECS=25
B="$HOME/.cache/pocket-terrarium/fw-build-qemu-aq"
OUT="$HOME/.cache/pocket-terrarium/qemu-models"
while getopts "n:t:b:o:" o; do
  case $o in n) BOOTS=$OPTARG ;; t) SECS=$OPTARG ;; b) B=$OPTARG ;; o) OUT=$OPTARG ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { sed -n 2,5p "$0"; exit 2; }
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
QEMU_VER="${QEMU_VER:-esp_develop_9.2.2_20250817}"
export PATH="$HOME/.espressif/tools/qemu-xtensa/$QEMU_VER/qemu/bin:$PATH"
mkdir -p "$OUT"
WORLD=$(grep -q "CONFIG_POCKET_TANK_WORLD_AQUARIUM 1" "$B/config/sdkconfig.h" && echo aquarium || echo terrarium)

for spec in "$@"; do        # one image per candidate, reused by every boot
  name=${spec%%=*}; model=${spec#*=}
  [ -f "$model" ] || { echo "qemu_models: no model file $model" >&2; exit 1; }
  python ../tools/model_trailer.py --model "$model" --world "$WORLD" --tag "q_${name:0:12}" --out "$OUT/$name.trailer.bin" >/dev/null
  esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$OUT/$name.flash.bin" \
    0x0 "$B/bootloader/bootloader.bin" 0x8000 "$B/partition_table/partition-table.bin" \
    0x10000 "$B/pocket_tank.bin" 0x290000 "$model" 0xA8F000 "$OUT/$name.trailer.bin" >/dev/null
done
for boot in $(seq 1 "$BOOTS"); do
  pids=()
  for spec in "$@"; do
    name=${spec%%=*}
    : > "$OUT/$name.$boot.log"
    timeout "$SECS" qemu-system-xtensa -nographic -machine esp32s3 -m 4M \
      -drive file="$OUT/$name.flash.bin",if=mtd,format=raw,file.locking=off \
      -serial "file:$OUT/$name.$boot.log" -monitor none >/dev/null 2>&1 &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || true; done      # timeout's own exit (124) is the normal end
  echo "qemu_models: boot $boot/$BOOTS done" >&2
done

python3 - "$OUT" "$BOOTS" "$@" <<'PY'
import re, statistics, sys
out, boots = sys.argv[1], int(sys.argv[2])
print(f"{'candidate':24s} {'boots':>5s} {'first decision ms (median, range)':>34s} {'tok/s':>6s} {'heap int KB':>11s}  boot")
for spec in sys.argv[3:]:
    name = spec.split("=")[0]
    ms, tps, heap, loaded, trailer = [], [], [], 0, 0
    for b in range(1, boots + 1):
        text = open(f"{out}/{name}.{b}.log", errors="replace").read()
        loaded += "advisor LLM" in text
        trailer += bool(re.search(r"trailer: model \S+, \d+ bytes - good", text))
        m = re.search(r"-> \w+ urgency \d p=[\d.]+\s+\[(\d+) ms, ([\d.]+) tok/s\]", text)
        if m:
            ms.append(float(m[1])); tps.append(float(m[2]))
        h = re.search(r"after the advisor: internal heap (\d+) KB", text)
        if h:
            heap.append(int(h[1]))
    state = f"{loaded}/{boots} advisor LLM, trailer {trailer}/{boots}"
    if ms:
        print(f"{name:24s} {len(ms):5d} {statistics.median(ms):12.0f} ({min(ms):.0f}-{max(ms):.0f}){'':8s} "
              f"{statistics.median(tps):6.1f} {statistics.median(heap) if heap else 0:11.0f}  {state}")
    else:
        print(f"{name:24s} {0:5d} {'-':>34s} {'-':>6s} {'-':>11s}  {state} (no decision; see {out}/{name}.1.log)")
PY
