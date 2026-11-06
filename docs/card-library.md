# The card library (issue #4 phase 4)

The microSD card as a **library of packs**: list the packs on the card, check them before use, install them into the board's
flash so a tank never depends on the card being in its slot, remove them. Phases 1-3 made a pack a file (`docs/ptpk.md`),
gave every pack the same loader and the same refusals, and split the engine from its worlds (`docs/modules.md`); this phase
adds the place packs come from.

**Status, in one line.** Everything that does not need the card hardware is built for real and proven in the simulator, where
the card is a folder. The 1.8's card driver is written against the vendor's documented wiring and **only compiled** (behind
the Kconfig option `CONFIG_POCKET_TANK_CARD`, default off): it has never run on a board. `docs/board-amoled-1.8-card.md` says
which numbers are sourced from where, what is unverified, and how to try it on a 1.8.

## What the card holds

**Data only**: PTPK packs - a species, a habitat, a model manifest (`docs/ptpk.md`). Behaviour stays in the firmware (the
ESP32-S3 cannot run code from a card). Models and tokenizers are phase 5, not here.

```
<card root>/packs/NAME.ptpk       one pack per file, flat: no subfolders are read
```

A file name is data from a stranger, not a path (`common/card.c`, `card_name_ok`): 6..47 characters of `[A-Za-z0-9._ -]`, ending
`.ptpk` (any case; FAT keeps the case it was given), not starting with a dot or a space, no `..`, nothing outside 0x20..0x7e.
Anything else is never listed and never opened - not by the simulator's port, not by the board's: a name with a separator, a
stream colon or a control byte cannot leave the one folder, and a directory, a link or a pipe named `x.ptpk` is not a file of
the card. At most 32 files are listed (the 32 smallest names in byte order, the same every time); a card with more says so.

## The visit

The card is reached in a **visit**: `card_open` ... `card_list` / `card_read` ... `card_close` (`common/card.h`, the platform's
port). It is short, bracketed and only when the keeper asks (the PACKS page's SCAN and INSTALL); **never while a tank runs, never per
frame**. One visit lists the card and reads its files one at a time into a block the caller owns (`lib_work_t`, 10460 B measured: in PSRAM on
the board, the heap in the simulator - never `.bss`, never a task's stack); nothing is kept between visits. INSTALL reads the one file
again in a second visit - the card may have been changed since the scan - and requires its size, uid and CRC to be the scan's.
Nothing here writes to the card.

## The library (`common/packs/library.c`)

| call | what it does |
|---|---|
| `library_scan` | one visit: every file read, run through the loader's own **dry check**, given a verdict |
| `library_install` / `library_add` | the file checked again, loaded through `packs_add(PACKS_FILE)` so it is usable at once, then copied into the store |
| `library_boot` | at every boot, after `packs_init`: everything in the store loaded through the same loader, manifests and habitats first |
| `library_remove` | a pack out of the store and the registry; `library_removable` asks first |

**The dry check** (`packs_dry_check`, `common/packs/ptpk.c`) is `packs_add` without the commit: the same header, length, CRC, uid
dedupe, every record, capabilities, goal words, `species_check` / `habitat_check` / `manifest_check` (the commits run the same
`species_admit` / `manifest_admit` the check does - one set of refusals), this firmware's movement modules, the habitat a species
lives in, a model's tag held by one manifest - on a species / habitat / manifest in memory the *caller* owns (the library's PSRAM
block) and **no pool slot taken**: the registry is exactly as it was after a scan. The one difference from `packs_add` is the order
of two refusals: a pack that is bad is said to be bad before the pool is said to be full.

**Verdicts** (one per file):

| verdict | meaning | the page offers |
|---|---|---|
| INSTALLABLE | a good pack, not installed, and room for it | INSTALL |
| INSTALLED | that very file (its CRC) is installed | REMOVE |
| UPDATE | a pack installed from the card has this uid; this is another file (a new version) | UPDATE |
| BUILT IN | the file of a pack the firmware ships (the repo's `packs/` copied onto a card) | nothing |
| NO MODULE | a good pack this firmware has no movement module for; the reason names it (`NEEDS SWIM, WHICH THIS FIRMWARE LACKS`) | nothing |
| REFUSED | the loader's own reason: damaged (CRC, short), not a pack, an unknown REQUIRED record, a rule broken, a built-in's uid in another file, no room in the pool or the store, 0 bytes, over 2068 bytes (refused by its size before a byte is read), a species whose habitat is not loaded (`INSTALL ITS HABITAT FIRST: <name>` when that habitat is on this card) | nothing |

**Dependencies.** A species lists its habitats and names its goals' words; the boot and a scan treat them in the order
manifests, habitats, species. `library_remove` refuses a habitat that is the only home an installed species has left, and a manifest
whose goal words an installed species needs and no other manifest lists - until that species is removed (`LEOPARD GECKO LIVES IN IT:
REMOVE IT FIRST`). A stored pack that cannot load at boot (its habitat was removed behind the library's back) is **kept**, listed NOT
LOADED with the loader's reason, and loads when what it needed arrives (every habitat or manifest install retries them).

**Updates.** Same uid, another file: the new version goes into a second sector, then the old one is erased (below); in the registry
the old pack is taken out and the new one loaded, the old put back if the flash refuses the new. A pack the keeper's tank lives on is
the page's to pause first (below).

## Where installed packs live: the pack store (`common/packs/pstore.c`)

**Not NVS.** The numbers, from the tree: the `nvs` partition is 0x6000 = 6 pages of 4096 B; a page is 126 entries of 32 B after its
header and state bitmap, NVS keeps one page free for its garbage collection: **5 x 126 = 630 entries** usable. A blob costs an index
entry, a header entry per chunk and one entry per 32 B: the tank's save, 2160 B today, is **70 entries**, and `nvs_set_blob` writes
the new copy before it erases the old (140 at the peak; the director's parked `bk` copy is another 70, a developer's tool). The
board's settings, `adv/tfit`, the battery history, the bedtime log, the shelf's frame journal and its four namespaces add about 35.
So a default build holds ~105 entries and peaks near 175 - and `save_layout.h`'s own budget keeps the rest for the save to grow into:
it is sized for a 4000 B save, three copies at once (3 x 128 = 384), "under 2/3 of the 630, so GC always has room"; **past that, saves
can start failing for space and `main.c nvs_start` erases an NVS it cannot open** - the keeper's tank. A pack is `2 + ceil(bytes/32)`
entries: the gecko 17, the fish 15, a habitat 7..11 - and 67 for a pack of the format's limit, 2068 B. A full set of five maximal
packs would take 335 entries, over half the partition on its own. It fits today with typical packs; it does not fit the save's growth
budget, and a store that can take the tank's room is the wrong store for something the keeper may pull in and out.

**The `storage` partition's unused front.** `storage` (`partitions.csv`: 0xD20000, 0x2E0000) is the shelf's from 0x10000 (`shelf.h`: 120
slots of 24 KB to its end) and the NVS rescue copy's at 0 (`main.c nvs_start` writes 0x6000 bytes there); **0x6000..0x10000, 40 KB, nobody
uses.** That is the store: 10 sectors of 4 KB, one pack to a sector (`PSTORE_SECTORS`, `PSTORE_BASE`; `persist_port_esp.c` asserts it
stays clear of both neighbours). The pool is the smaller limit on the board (2 + 2 + 1 slots), so the store never fills before it.
The simulator's store is a file, `POCKET_TANK_PACKSTORE`, 40 KB erased to 0xFF.

**A sector describes itself, so there is no index to disagree with the packs** and a power cut leaves the state before or the state
after, never half:

```
    0  magic "PTPS"   4  layout u16 = 1   6  len u16 (the file's bytes)   8  seq u32 (rises with every write)
   12  uid u32        16 kind u8          17..27 zero                      28 crc32 u32 over bytes 0..27
   32  the PTPK file, len bytes (held by its own CRC and the loader's checks)
```

- **Install:** erase a free sector, program the file at 32, program the header at 0 **last**. Until the header is whole the sector
  reads as blank or torn, so a cut anywhere leaves what was there.
- **Update:** the new version into a *second* free sector (a higher `seq`), then erase the first. A cut between leaves two sectors of
  one uid; the boot keeps the higher `seq` and erases the other.
- **Remove:** erase the sector.
- **Boot** (`pstore_boot`): every sector read; one that does not read true - bad magic or layout, a length no pack has, a `seq` that
  was never written, a header whose CRC fails, a file that does not pass `ptpk_open`, a uid or kind the file does not carry - is
  **erased and never loaded**. Never a crash: the file is a stranger's even in the board's own flash.

`library_boot` then loads the records through `packs_add` exactly as a folder's or a card's file goes. The order of operations on
install is the registry first (the loader, whole, so a pack the dry check passed loads), the store second; a refusal of the flash
puts the registry back, an update's old version too. A cut between the two loses only RAM.

**The pool.** `PACKS_SPARE` / `MANIFEST_SPARE` (`common/packs/ptpk.h`): the firmware had none (PR #12: each spare species costs
576 B + 4 of `.bss`, a habitat 404 B + 4, a manifest 180 B + 4; the 1.75C's aquarium already runs on a few KB of internal heap).
**With the card library (`PT_CARD`) it has 2 species, 2 habitats and 1 manifest: 2160 B of `.bss`**, the same in both worlds; the
library's own tables are about 450 B more (`lib_inst_t` x 10, the store's sector table). A build without it keeps 0 and its `.bss`
exactly as it was (measured: `docs/board-amoled-1.8-card.md` "What it costs").

## The tank on a removed pack

REMOVE of a pack a tank lives on is allowed, and warned about on the page. The page calls `progression_pack_leaving(tank, uid)`
*before* the registry changes: the tank is made to wait exactly as a save that names a pack that is not here does (`docs/ptpk.md`
"A tank and its packs") - paused, **PACK MISSING**, the defaults drawn, its pair's uids and names kept for every save - and resumes
the moment the pack is installed again (the tick's look once a second, or `progression_pack_arrived` at once after an install). An
UPDATE of the pack it lives on pauses and resumes it the same way, on the new version. A tank on a pack that is installed is
unaffected by the card going: it loads from the store at every boot.

## The PACKS page

Under the ADVANCED page (`docs/BOARDS.md`; hidden behind the BOOT chord on the settings page: this is a deliberate action): a PACKS
button in the foot's left slot, BACK stays right. The button is drawn, and its tap answered, **only on a board with a card slot**
(`card_slot()`: the simulator with `POCKET_CARD_DIR` set; the firmware's 1.8 with the driver). Two tabs:

- **INSTALLED** - the packs in the board's flash, one row each (name; its kind, or `NOT LOADED: <the loader's reason>` in amber for a
  stored pack that cannot load), each with REMOVE.
- **CARD** - SCAN reads the card; then one row per file: the pack's name (or the file's, if it did not open), a line with its verdict
  and reason (`NEW SPECIES`, `SPECIES - INSTALLED`, `SPECIES - ANOTHER VERSION IS INSTALLED`, `BUILT IN`, `NEEDS: ...`, `REFUSED: ...`), and the one
  button that verdict has: INSTALL, UPDATE, REMOVE (when that very file is installed), or none. The rows follow the work (a pack
  installed turns its file's row into REMOVE and another version of it into UPDATE; a removal turns them back) without a second visit.

More rows than the glass holds (4 on the 1.8, the square and the round, 7 on the watch) are paged: `<` and `>` beside the page number,
drawn only when there is somewhere to go. A line under the list says the last result (`INSTALLED LEOPARD GECKO`, `NO CARD`, `NO PACKS
FOLDER`, `32 LISTED: THE CARD HAS MORE`, ...).

**Requests.** A scan, an install and a removal are one blocking call each. A tap only *asks*; the page then draws READING THE CARD (or
REMOVING) and `adv_tick` runs the call on the **next** tick - so the frame that says what is happening is always drawn first - and the
buttons are dead until it is done. ADVANCED hands the platform one `ADV_ACT_PACKS` per change (an install, an update or a removal; a
scan is not one) through the next `adv_touch`, and the platform logs `pp_message()`.

**Questions.** REMOVE and UPDATE ask first, on a page of their own with CANCEL and the action; INSTALL does not (nothing is lost).
When the tank the platform bound (`adv_bind_tank`) lives on the pack, the question says `YOUR TANK LIVES ON IT. IT WILL WAIT, PAUSED,
UNTIL THE PACK IS BACK.` - in these words in both worlds. A REMOVE the library would refuse (`LEOPARD GECKO LIVES IN IT: REMOVE IT FIRST`) does
not ask: the line under the list gives the reason.

Written once in `common/` (`packs_page.c`; one layout is the source of the drawing, the taps and the tests' targets), laid out from the
settings page's own constants on all four glasses; `--selftest-packs-page` asserts every pixel it draws is inside the glass and the
page's box. **A build with `PT_CARD` 0 carries none of it** - no button, no page, no code. The page's working area is one block
(`lib_work_t`, 10460 B measured by `--selftest-library`, and 768 B of reasons for the rows on view) allocated by `pp_open` from `card_mem_alloc` (PSRAM on the
board) and freed by `pp_close` - when the keeper leaves the page or ADVANCED closes.

## The simulator

| | |
|---|---|
| `POCKET_CARD_DIR` | the card's root (`<root>/packs/NAME.ptpk`). Unset: no card slot. A folder that is not there: a slot with no card |
| `POCKET_TANK_PACKSTORE` | the pack store's file (default `~/.cache/pocket-terrarium/packstore.bin`); a selftest gets its own |
| `make CARD=0` | the simulator without the library (`fishsim*-nocard`): the configuration of a firmware that leaves the Kconfig option off |
| `fishsim --selftest-library` | scan, check, install, the store across reboots and power cuts (every write cut, and torn), update, remove, a hostile card and store, fuzz, a tank on an installed pack |
| `fishsim --selftest-packs-page` | the page on this board, driven through ADVANCED's own `adv_touch` / `adv_tick` / `adv_render`: the door (PACKS only with a slot), every drawn pixel inside this board's glass in ten-odd states, a request drawn before it is done, INSTALL / UPDATE / REMOVE and their questions, a tank on the pack, paging, the words for no card / no folder / an empty folder / too many files |
| `fishsim --card-shot PREFIX` | the page's states as PPMs |

`POCKET_PACKS_DIR` (phase 1) is unchanged: a developer's convenience that auto-loads a folder at start. The card library is the
keeper's path and is tested apart from it.

## What it does not do

No models or tokenizers (phase 5). No writing to the card. No subfolders. No hot-plug: a card is found by trying (the 1.8's slot has no
card-detect line). No upgrade of an installed pack on its own: an UPDATE is the keeper's tap. A pack's *behaviour* is the firmware's:
a pack that needs a movement module the firmware lacks is NO MODULE, not a download.
