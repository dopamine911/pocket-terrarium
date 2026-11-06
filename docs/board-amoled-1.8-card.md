# The 1.8's card slot: what is sourced, what is not, and the bench test

The card library (`docs/card-library.md`) is proven in the simulator, where the card is a folder. **This page is about the one part
that is not: the 1.8's microSD driver (`firmware/main/card_port_sh8601.c`).** It is written from the vendor's documents and
**compiled, never run on a board**. No shipped image enables it (`CONFIG_POCKET_TANK_CARD`, default off, offered for the 1.8 only).
Nothing below says it works. It says what it was written from, what would have to be true for it to work, and how someone with a 1.8
and a card can find out in about ten minutes.

## Where the pins come from

| signal | 1.8 | source | checked on this repo's hardware |
|---|---|---|---|
| CLK (SCK in SPI mode) | GPIO 2 | the vendor's schematic, SD-CARD block (`ESP32-S3-Touch-AMOLED-1.8.pdf`, Waveshare wiki), and `BSP_SD_CLK` (`GPIO_NUM_2`) in the vendor's BSP (`waveshare/esp32_s3_touch_amoled_1_8`, `bsp_sdcard_mount()`) | **no** |
| CMD (MOSI) | GPIO 1 | the same two (`BSP_SD_CMD`, `GPIO_NUM_1`) | **no** |
| D0 (MISO) | GPIO 3 | the same two (`BSP_SD_D0`, `GPIO_NUM_3`) | **no** |
| D1, D2 | not connected | the schematic | **no** |
| DAT3 / CS | **not a GPIO**: bit 7 of the TCA9554 expander's output register (EXIO7, I2C 0x20) | the schematic; `board_pins.h` and `board_sd_select()` in `display_port_sh8601.c` already drive it (the tank switcher's step 2 test, `sd_spike.c`, left out of pocket-tank's 0.3.9.3, release notes D2) | the expander's other bits are in use on this repo's 1.8 every boot; the SD bit has not been exercised |
| card detect | the slot's switch goes to ground, not to a pin | the schematic | **no** - a card is found only by trying |
| pull-ups | 10 k to 3V3 on CS, MOSI, SCK, MISO | the schematic | **no** |

These were read from the vendor's documents when the port was written (2026-11-06). The pin numbers are Kconfig options
(`POCKET_TANK_CARD_PIN_CLK` / `_CMD` / `_D0`) so a wrong one is a menuconfig change, not a code change; the build refuses any of them
that is a pin the display, I2C or touch already uses (`CARD_PIN_TAKEN`, a `_Static_assert`).

**Which board.** `card_slot()` is `board_has_expander()`: the 1.8, positively (its expander answered and no ES7210). GPIO 1, 2 and 3
are other boards' pins (the 1.75C's panel reset, touch reset and power-key sense), so the port never touches them anywhere else, the
Kconfig option is not offered for the round, the square or the watch, and a card visit on a board whose expander did not answer
returns `NO CARD SLOT` before it touches a pin. On the 1.8 itself nothing else in `firmware/` uses GPIO 1, 2 or 3 (searched, 2026-11-06).

## What the driver does

- **SDMMC, 1-bit** (the default; the vendor's own way). EXIO7 is driven **high before the first clock** and stays high: a card whose
  DAT3 is low when CMD0 arrives drops into SPI mode and stays there until its power is cycled.
- **SPI** (`POCKET_TANK_CARD_SPI`, an alternative if SDMMC will not mount): `SPI3_HOST` (the display has `SPI2`), EXIO7 held **low** for
  the whole visit as the chip select. A card that has been through it will not answer SDMMC until it has lost power: take it out and
  put it back before going back.
- **Never writes**, never formats (`format_if_mount_failed` is false): files are opened `"rb"`, directories read, nothing else.
- FAT12/16/32 only. ESP-IDF's FatFS has exFAT off, so the factory format of a 64 GB or larger card is `CARD NOT READABLE`: format it
  FAT32. Long file names are required (a `.ptpk` name has a four-letter extension); the build stops without `CONFIG_FATFS_LFN_HEAP`
  (`firmware/sdkconfig.defaults` has it).
- A visit is short and only when the keeper taps SCAN or INSTALL: the card is brought up, read and let go. Nothing runs from it.
- Names are data from a stranger: `card_name_ok` (`common/card.c`, tested in the simulator) lets only plain `*.ptpk` names through, one
  folder, no separators.

## What is proven, and where

| | |
|---|---|
| the library, the pack store, the page, the tank on a removed pack | the simulator: `--selftest-library` (scan, install, update, remove, every power cut, hostile cards and stores, fuzz) and `--selftest-packs-page` on all four boards, in both worlds - `make check-all` |
| the firmware **compiles** with the option off, on the 1.8 and the round, both worlds | ESP-IDF 5.5.2: `.bss` byte-identical to `main`; image a few dozen bytes larger (below) |
| the firmware **compiles and links** with the option on, on the 1.8, both worlds | the same (below) |
| the pack store's region of flash (the `storage` partition's unused 0x6000..0x10000) | `_Static_assert`s in `persist_port_esp.c`: clear of the NVS rescue copy and of the shelf. **Never run on a board.** |
| the card driver, the pins, EXIO7's timing, the heap it costs while mounted | **nothing**. This page's bench test is the first run |

## What it costs

**Measured** (`idf.py size`, ESP-IDF 5.5.2, the 1.8 config; main is `fe3ec76`, the commit before the card library):

| build | `.bss` | image (`Total image size`) | app partition free |
|---|---|---|---|
| terrarium, main | 44 952 B | 1 595 899 B | 37 % |
| terrarium, card library **off** | 44 952 B (same) | 1 595 963 B (+64) | 37 % |
| terrarium, card library **on** | 47 880 B (+2 928) | 1 674 951 B (+79 052) | 35 % |
| aquarium, main | 46 512 B | 1 739 863 B | 32 % |
| aquarium, card library **off** | 46 512 B (same) | 1 739 927 B (+64) | 32 % |
| aquarium, card library **on** | 49 440 B (+2 928) | 1 821 051 B (+81 188) | 30 % |

The 1.75C (card library off, the only configuration it has): terrarium `.bss` 44 808 B (main: 44 808), image +72 B; aquarium `.bss` 47 456 B
(main: 47 456), image +52 B. The few dozen bytes with the option off were not chased: the code that changed with the option off is the ADVANCED page's touch wrapper and the
shared `species_admit` / `manifest_commit` refactor; none of the library, the store or the page is compiled.

The +2 928 B of `.bss` with the option on is the pack pools' spare slots (2 species, 2 habitats, 1 manifest = 2 160 B, the same in both
worlds) and the library's own tables. The +79 to +81 KB of image is the SD host, FatFS and the library's code.

**Estimated, not measured** (from ESP-IDF 5.5.2's sources and this tree's `sdkconfig`; the port's header has the arithmetic): about 1 KB
of internal heap while a card is mounted, about 1.3 KB more at a sector read's peak, FatFS's buffers (~13 KB) in PSRAM. The library's
working block (10 460 B, plus the page's 768 B) is allocated from PSRAM when the page opens and freed when it closes. The 1.75C's
aquarium is the board that runs short of internal heap (issue #11) and **does not get this option**.

## The bench test

You need: a 1.8 with a working tank, a microSD card **formatted FAT32** (not exFAT), and the repo.

1. **Build with the option on.** Add a file with the one line `CONFIG_POCKET_TANK_CARD=y` to the defaults (or `idf.py menuconfig` →
   pocket-tank), and build the 1.8's image of the world you want:
   `idf.py -B build-card -DSDKCONFIG=build-card/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;<that file>" build`
   (add `sdkconfig.aquarium` after it for the aquarium). Flash the **app** the way you flash a 1.8 build (`docs/BOARDS.md`); the model
   and the save are untouched.
2. **Make a card.** FAT32, a folder `packs` at the root, then:
   - copy the repo's `packs/*.ptpk` into it - this world's own three files (species, habitat, model manifest) should read **BUILT IN**;
   - author one new pack with `tools/ptpk.py build` (`docs/ptpk.md` "Authoring") - a species that lives in the world's own habitat;
   - leave a file that is not a pack in there too (`echo hello > packs/junk.ptpk`).
3. **Open the page.** On the settings page hold BOOT for five seconds and, still holding it, touch the glass: ADVANCED opens (`common/advanced.h`).
   The foot has **PACKS** at the left and BACK at the right. (On a 1.8 whose option is off there is no PACKS button: that is correct.)
4. **Look at the serial log** (`idf.py monitor`) while you do each step, and note what the page says:

| step | what should happen | log lines (`card:` tag) | if not |
|---|---|---|---|
| PACKS → CARD tab → SCAN, card in | READING THE CARD, then one row per file with its verdict: BUILT IN, NEW SPECIES (yours), REFUSED (junk) | `visit open (SDMMC 1-bit CLK 2 CMD 1 D0 3, EXIO7 high)`, `mounted: <name>, <n> MB`, `/packs: N .ptpk files listed`, `visit closed` | `mount failed: ESP_ERR_...` is the first thing to send. `NO CARD` with a card in: a pin, EXIO7, or the card (try another; try FAT32) |
| SCAN with no card | `NO CARD` | `mount failed: ESP_ERR_TIMEOUT (0x107)` or `NOT_FOUND` | a hang or a reboot here is the finding |
| INSTALL on yours | `INSTALLED <NAME>`; a click | `library: SPECIES <NAME> ... installed` | |
| INSTALLED tab | the pack, with REMOVE | | |
| take the card **out**, reboot | the pack is still there: PACKS → INSTALLED lists it, not NOT LOADED | `library: 1 packs in the store, 1 loaded` | the store is the piece of flash that has never run |
| REMOVE (with the card out) | asks first; yes removes; if your tank lives on it, it waits (PACK MISSING) | `library: ... removed` | |

5. **Then try to break it, gently:** a card that is not FAT (exFAT: should say `CARD NOT READABLE`, not hang), a card pulled during a scan,
   a card with 40 files (should list 32 and say there are more), a 0-byte `.ptpk`, a name with a space. Each should end in words on the page.
6. **If SDMMC will not mount**, build again with `POCKET_TANK_CARD_SPI` on, **take the card out and put it back first** (the card may be
   stuck in SPI mode), and repeat step 4.

**What to send back:** the serial log of the first SCAN (all `card:` lines and any `E (...)` / `W (...)` lines before them), which
build (SDMMC or SPI), the card (size, brand, FAT type), and whether the display or touch glitched during a visit. A visit that
corrupts the display, drops the touch or hangs is a finding even if the pack installs.

## What a failure would look like, and what it does not break

The card is only read. The tank, its save and the shelf are not on the card, and a visit does not write to them. A visit runs inside
the tank task's ADVANCED tick (after the page has drawn READING THE CARD), so the glass and the tank loop wait while it runs: a driver
that returns slowly or not at all would show there first. The pack store is 40 KB of the `storage` partition that nothing else uses,
bounded by `_Static_assert`s against the NVS rescue copy and the shelf. None of this has been exercised on a board.
