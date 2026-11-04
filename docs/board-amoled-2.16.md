# Fourth board: Waveshare ESP32-S3-Touch-AMOLED-2.16, the SQUARE (480 x 480) - port notes, 2026-10-12

A desk box with a square glass and the USB at its foot (Strato's photo in
resources/ESP32-S3-Touch-AMOLED-2.16/images/). Sources:
`resources/ESP32-S3-Touch-AMOLED-2.16/` (schematic, wiki page, and
`...-factory-readback-2026-10-12.bin`: the whole 16 MB flash as it shipped,
read back before the first flash - written at 0x0 it restores the board),
github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 and Waveshare's BSP
component `waveshare/esp32_s3_touch_amoled_2_16` v2.0.1 (the panel's init
sequence came from it; its touch driver is the CST9217 component). The
feasibility notes this file held from 2026-09-17 (sim at 480x480, the
estimates) are in git history; what follows is the board on the bench.

## Hardware (what the port uses)

| piece | the 2.16 | notes |
|---|---|---|
| SoC | ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash | the 1.8's: the model, partitions, PSRAM plan carry over |
| panel | CO5300, 480 x 480, QSPI on GPIO 4/5/6/7, clock 38, CS 12, reset 39 | the 1.75C's clock pin; no gap; the whole 480 as the window |
| touch | CST9220 at 0x5A, reset GPIO 40, INT 11 (polled) | the 1.75C's CST9217 family: read the same way (touch_port cst9217_*) |
| I2C | SDA 15 / SCL 14 | as every board |
| PMIC | AXP2101: DCDC1 = VCC3V3, ALDO1 = A3V3 (codec + ES7210) | the panel's 3V3 is VCC3V3 (no rail to cut), DSI_PWR_EN pulled up |
| audio | ES8311: DSDIN 8, BCLK 9, LRCK 45, MCLK **42**, amp 46 | the 1.8's pins but the master clock (16 there) |
| ES7210 mics | 0x40 | suspended at boot like the 1.75C's / the watch's |
| RTC | PCF85063 at 0x51 | time runs through a power-off; the clock sync page once (0.3.9), and after a stop (no cell, unplugged) |
| IMU | QMI8658 at 0x6B | axes not read yet: the 1.8's default |
| keys | PWR through the PMIC, SYS_OUT on GPIO 16; BOOT GPIO 0; KEY3 GPIO 18 (unused) | |
| cell | 3.7 V **1000 mAh** (Waveshare's optional pack; the 1.8 = 200, the pendant 500, the watch 400) | 4.3 h awake at 60% brightness, the light on: 100 -> 66%, the firmware learned 8.5 %/h = ~12 h on screen, ~79 mA (docs/batlog/2026-10-12_square-4h-awake.txt); VBAT 4127 -> 3841 mV over it. A NIGHT POWERED OFF (2026-10-12/13): 77% -> 77% in 10 h 10 min, 3936 -> 3968 mV - under the gauge's 1%, so under ~1 mA (docs/batlog/2026-10-13_square-night-off.txt); the RTC keeps the time |
| other | TF slot (1/2/3/41), vibration none | unused |

**How the board is told at boot** (display_port_init): no expander + an
ES7210 + an RTC is the WATCH's signature too. So the square's touch reset
(GPIO 40, the watch's I2S data line - an output there, harmless) is let go
and 0x5A asked for: a CST92xx answers on the square, the watch's FT3168
sits at 0x38.

## The square build (`TANK_SQUARE`)

- **The world** is the rectangle's a size up: 480 x 480, the pages (448 x
  368) centred on it at PAGE_X 16 / PAGE_Y 56 - every page shows whole, with
  a 56 px band above and below it. The glass has the 1.8's kind of corner:
  `TANK_CORNER_R` 40 (by the maker's pictures, not measured - the watch's
  corner code, keyed on TANK_CORNER_R, does the rest).
- **The floor**: TANK_FX0..TANK_FX1 = 10..470 (the sand line meets the arcs
  8 px in). Bed 1 against the right glass at TANK_FX1 - 72, bed 2 at
  TANK_FX1 - 170 (the bowl's spots: the rectangle's 0.84 x 480 left bed 1 a
  frond short of the wall). The reef bed and the sword plant: the defaults.
- **Fronds**: a segment is 4.2 px (3.2 on the 1.8): growth 1 still reaches
  the surface of a tank 112 px taller; trims pay at the 1.8's pitch.
- **Film grid**: 24 x 24 cells of 20 px = 576 (16 px cells would be 900; the
  save keeps 644).
- **The panel's way up** is its MADCTL: 0xA0 - Waveshare's own "rotation
  0" - which is the tank upright with the USB at the FOOT, Strato's normal
  grip. (An afternoon was spent at 0x60: upright USB-UP, the way he first
  held it. The tank's IMU flip hid the difference; update mode, which
  never reads the IMU, showed its crosses upside down.) Director
  `madctl <hex>` tries another live; a wake re-sends what was tried.
- **Touch**: the chip's report is swapped then x-mirrored against the
  picture (`touch mirror 5`; the bowl's is 3; it was 6 at 0x60). CALIBRATED
  2026-10-12 in Strato's two grips - the box in a palm with the thumb, and
  in one hand with the other's index finger - 18 presses each, in the
  FINAL frame (docs/touch/2026-10-12_square-B-thumb.log, -B-index.log;
  the -thumb / -index / -turned / -check logs are the USB-up frame and
  the carried-over map that read 20 px left at the left column). The
  thumb reaches the far column 14 px further than the index finger, so
  ONE map, the mean of the two fits (CAL_S_*: x 1.052, y 1.025 with the
  offsets 13.1 / 6.8 composed onto the first cut): each grip ~7 px off at
  the far-left column, inside a fingertip elsewhere; the check pass with
  it on (-B-check.log, the thumb): y gain 1.008 offset -0.4, x gain 1.010
  offset -7.4 - the thumb's half of the split. The finger lands ~20
  px low (CAL_LAND + the bias) and ~8 px toward the tapping hand
  (CAL_S_LAND_X, the watch's number), both turning with the picture; the
  USB-up turned sitting is what told the landing from the panel.
- **The IMU**: held the usual way (USB down, tilted back) g = [1166 13033
  10739] - gravity +Y, so up = +Y here (the 1.8's is -Y); the live flip
  follows a turn (Strato: "it flips when I rotated it around correctly").
- **Frame**: sent row for row like the watch's (native_flush), 24 rows a
  stripe; the 180-degree flip in the same path (the IMU's live flip, axes
  unverified).
- **Sleep**: the 1.75C's shape - panel reset held high, touch reset low, CS
  high, the QSPI lines held low (the panel stays on VCC3V3), the panel's
  deep standby and the chip's own sleep behind `sleepcfg`. Nothing measured.
- **Charging** (2026-10-13, Strato: "all boards charging in nearly 2
  hours"): the charge current is per board now, ~0.75 C of the maker's
  cell - the 1.8 150 mA, the pendant 400, the watch 300, the square 500
  (battery_port_axp2101.c `board_charge_ma`; it was 100 mA everywhere,
  the 1.8's, and the square learned 12.2 %/h on it). On the square's
  bench, at 83-85% (director `charge <mA>`): VBAT 4097 mV at 300 mA, 4150
  at 500, 4192 at 700 - ~0.25 ohm in the pack's path, so a faster current
  meets 4.2 V and starts its taper EARLIER (~85% at 500, ~78% at 700, ~70%
  at 1000): fast to about 80%, then every rate is the taper. The hub port
  held 4.82 V at 1000 mA, never throttled; the die read 46 C at 300 mA,
  51 C at 1000. NOT measured yet: a whole charge from low (the battery
  log records it on its own - run it down, plug it in awake, read
  `batlog`), and the box's warmth over that hour.
  The square ships at 500 mA, 0.5 C (700 was tried on the bench): any USB
  port gives it, and a keeper's own smaller cell on that connector stays
  at or under 1 C down to 500 mAh. A port that cannot give it sags, and
  the chip's VINDPM at 4.36 V throttles the charge to what there is.
  The end of the charge and the precharge are per board too (2026-10-13):
  the chip came up at 125 / 125-150 mA on all four, and 125 mA against the
  1.8's 150 mA charge stopped it as the taper began. Now about a tenth of
  the cell: the 1.8 25 mA, the pendant 50, the watch 50, the square 100.
- **The PWR key is the MIDDLE button** (Strato, 2026-10-12): a short press
  sleeps it (save, panel off, the 1200 s grace, a press wakes it - a drowse
  wake, the same uptime), 1.5 s cuts it (save, the PMIC's soft cut, a press
  boots). Both tried on the glass. The USB console drops during the grace.
- **Audio**: the box drives its speaker harder than the 1.8's - at the
  1.8's -6 dB master "a little loud, slightly blown out"; AUDIO_MASTER_DB
  is -12 on this build (what the old quiet was, right by ear).
- **Corners**: the 40 px picture corner reads right against the bezel, but
  the bezel clipped the stats card and the battery pill at the 1.8's
  insets: the square's own RENDER_CARD_X/Y 26 / 22 and the pill 44 in,
  22 down (render.h).

Build and flash: `tools/flash_square.sh` (`--full` for a blank board:
bootloader, table, model + its trailer, a cleared NVS). It builds with
`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.square"` into
`~/.cache/pocket-tank/fw-build-216` and finds the board by USB serial
(`SQUARE_SERIAL`, tools/boards.local.sh).
The sim: `make SQUARE=1` -> `./fishsim-square`; `make SQUARE=1 check` and
`make check-all` (four worlds now); `tools/board_sheet.py` draws it fourth.

## First boot, 2026-10-12

- Panel and touch up on the first flash (the BSP's init, 600 ms waits cut
  to 120 / 20). Internal heap 38 KB free with the tank running (the 1.8 ~54,
  the watch ~45; the dirty mask is 28.8 KB here against the 1.8's 20.6).
- 20.3 - 25.5 fps with two fry, flush 15 - 21 ms (fill 12 - 18, the wire
  never waited); a decision 4.25 - 4.44 s at 9.6 - 10.1 tok/s - at the 4.2 s
  limit's edge, like the watch and the pendant.
- The model copied from ~/.cache/pocket-tank is the OLD one (Aug 21): the
  tank ran rules until model/out's (Sep 15, v3m) was written with its
  trailer. flash_square.sh --full writes both now.
- Two selftests were tuned to the 1.8's floor and failed here before the
  beds moved: the urchin's bite grew with its walk (the test now starts it
  the 1.8's 143 px from the frond on every board), and a flat-cut bed
  regrew within 0.03 because bed 1 had four fronds (now five, against the
  glass).

## Open

- The bands above and below the pages (56 px each): live with them first.
- Audio: the other cues by ear over days; the PWR key (sleep / wake /
  power-off), a night's draw, the installer / OTA / site for a fourth
  image (pt_boards.py, the workflows and make_installer take it already).
