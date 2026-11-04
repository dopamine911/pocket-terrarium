#!/bin/bash
# flash_square.sh - build the SQUARE image (the 480 x 480 tank, CONFIG_POCKET_TANK_SQUARE)
# and flash it to the square (Waveshare ESP32-S3-Touch-AMOLED-2.16).
# The board is found by its USB SERIAL, never by port name: the tank, the
# round board, the watch and the square share a bench and the names swap.
#
#   tools/flash_square.sh               # build, flash the app
#   tools/flash_square.sh --build-only
#   tools/flash_square.sh --full        # a blank board: clear nvs, the model, bootloader + table + app
#   SQUARE_SERIAL=AA:BB:CC:DD:EE:FF tools/flash_square.sh    # the board, by its USB serial (or set it once in tools/boards.local.sh)
#
# Its own build directory and sdkconfig (~/.cache/pocket-tank/fw-build-216):
# the tank's fw-build is never touched. No preflight: this board holds no tank
# anyone would miss yet. docs/board-amoled-2.16.md has the rest.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f tools/boards.local.sh ] && . tools/boards.local.sh   # your boards' USB serials (not tracked)
SERIAL=${SQUARE_SERIAL:-}
[ -n "$SERIAL" ] || { echo "flash_square: set SQUARE_SERIAL=<the board's USB serial> (in tools/boards.local.sh, or the environment)"; exit 1; }
B=${TANK_BUILD:-~/.cache/pocket-tank/fw-build-216}   # another checkout (a worktree) builds in its own dir: a build dir is tied to its source path
. ~/esp/esp-idf/export.sh > /dev/null 2>&1 || { echo "flash_square: ESP-IDF export failed"; exit 1; }
[ -f firmware/keys/ota_signing_key.pem ] || { echo "flash_square: no firmware/keys/ota_signing_key.pem - tools/ota_key.sh"; exit 1; }
LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
( cd firmware && idf.py -B "$B" -DSDKCONFIG="$B/sdkconfig" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.square" build ) > "$LOG" 2>&1 \
  || { echo "flash_square: build FAILED"; grep -Ei "error|fatal|failed" "$LOG" | head -30; exit 1; }
grep -E "pocket_tank.bin binary size" "$LOG" || true
grep -q "CONFIG_POCKET_TANK_SQUARE=y" "$B/sdkconfig" || { echo "flash_square: $B is not a SQUARE build"; exit 1; }
[[ " $* " == *" --build-only "* ]] && exit 0
PORT=$(python - "$SERIAL" <<'PY'
import sys
from serial.tools import list_ports
want = sys.argv[1].upper()
for p in list_ports.comports():
    if (p.serial_number or "").upper() == want:
        print(p.device.replace("/dev/tty.", "/dev/cu.")); sys.exit(0)
sys.exit(1)
PY
) || { echo "flash_square: no board with USB serial $SERIAL on USB"; exit 1; }
echo "flash_square: the square ($SERIAL) is $PORT"
ESPTOOL="python -m esptool --chip esp32s3 -p $PORT -b 460800"
if [[ " $* " == *" --full "* ]]; then
  $ESPTOOL --after no_reset erase_region 0x9000 0x7000 | grep -E "Erase completed|rror"
  $ESPTOOL --after no_reset write_flash 0x290000 model/out/model_q4.bin | grep -E "Wrote|verified|rror"
  python tools/model_trailer.py --check --build-dir "$B" || exit 1          # the model's trailer (docs/OTA.md): without it the tank runs rules
  $ESPTOOL --after no_reset write_flash 0xA8F000 "$B/model_trailer.bin" | grep -E "Wrote|verified|rror"
  ( cd "$B" && $ESPTOOL write_flash "@flash_args" | grep -E "Wrote|verified|rror" )
else
  # otadata too: after an over-the-air update the board boots ota_1, and an app written to ota_0 alone would never run
  $ESPTOOL write_flash 0xA90000 "$B/ota_data_initial.bin" 0x10000 "$B/pocket_tank.bin" | grep -E "Wrote|verified|rror"
fi
echo "flash_square: done"
