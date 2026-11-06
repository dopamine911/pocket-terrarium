# PTPK - the pack file format (fmt 1)

A **pack** is one species or one habitat as a file. The engine, the model and the renderer never name a species or a
habitat; they read the pack (`docs/species-pack.md`, `docs/habitat-pack.md`). Since issue #4 phase 1 a pack is a PTPK
file: the built-in packs are encoded from their C structs at boot and go through the same loader a pack from a
folder (the simulator, now) or from the microSD card (phase 4) goes through. One loader, one set of refusals.

This page is the byte-exact contract. `common/packs/ptpk_write.c` writes it (from the built-in C structs;
`tools/ptpk.py build` writes a new pack from TOML), `common/packs/ptpk.c` reads it, `sim/selftest_ptpk.c` holds
them to it.

Phase 1 is "format and loader, built-in packs only". Out of scope here and reserved below: capability matching
(phase 2), the module split (3), the card library (4), switching models (5), turtles.

## The file

All numbers are **little-endian**. A float (`f32`) is an IEEE-754 binary32 and is always finite (no NaN, no infinity:
a loader refuses a pack that holds one). The file is exactly `20 + len` bytes: a header, then the body.

```
header, 20 bytes
  off  size  field   value
    0     4  magic   "PTPK" (0x50 0x54 0x50 0x4b)
    4     1  fmt     1
    5     1  kind    1 species | 2 habitat | 3 model manifest (reserved, refused by this loader)
    6     2  flags   0: every bit is reserved; a pack with a flag set is refused
    8     4  uid     the pack's 32-bit identity: FNV-1a-32 of its UID_NS string (below); never 0
   12     4  len     the body's length in bytes, 0 < len <= 2048 (PTPK_BODY_MAX)
   16     4  crc32   CRC-32 (the zlib / IEEE one: reflected, polynomial 0xedb88320, init and final xor 0xffffffff) of the body
body, len bytes
   records, back to back: tag u16 | rlen u16 | value (rlen bytes)
```

The issue's sketch called the header 16 B; its fields (4 + 1 + 1 + 2 + 4 + 4 + 4) add up to **20**, and the fields
are what this format keeps. A file longer than `20 + len` is refused too: new data goes into new records, never past
the end.

### Records and the REQUIRED bit

A record is `tag u16 | rlen u16 | value`. The tag's bit 15 is **REQUIRED**; the low 15 bits are the record's id.

- A loader that knows the id reads the record (the bit is the writer's statement; this loader does not hold a known
  record to it).
- A loader that does **not** know the id **refuses the whole pack** if the bit is set, and **skips** the record
  (`rlen` bytes) if it is clear. So a pack marks the records it cannot work without as REQUIRED and older firmware
  refuses it instead of half-running it; a new optional detail (a pattern, a sound) leaves the bit off and older firmware
  still loads the pack.
- Records may come in any order; the encoder (`common/packs/ptpk_write.c`) and `tools/ptpk.py` write them in ascending id. A known id twice is refused (an unknown
  optional one twice is skipped twice).
- A known record's `rlen` is exactly its layout's size (a list's, exactly what its counts say): **no trailing bytes
  inside a record**. A record's value never grows later; new data is a new record.
- The records of a pack must fit its `len` exactly (a record that runs past the body, or a 1-3 byte tail that cannot
  hold a record header, is a truncated pack: refused).

### Value types

| type | bytes |
|---|---|
| `u8`, `u16`, `u32` | 1, 2, 4, little-endian |
| `f32` | 4, IEEE-754 binary32, finite |
| `str` | `u8 n`, then `n` bytes (no NUL) |
| *text* | a string of the bytes 0x20..0x7e (printable ASCII), not starting or ending with a space, at least 1 byte |

Whole-record strings (`NAME`, `UID_NS`) are the record's value, with no length byte (`rlen` is the length).

### uid

`uid = FNV-1a-32(UID_NS bytes)`: start at 2166136261; for each byte `h = (h xor byte) * 16777619 mod 2^32`.
`UID_NS` is the pack's name as a string, `[a-z0-9._-]`, 3..47 bytes, by convention `vendor.kind.thing`:
`pt.gecko.leopard`, `pt.habitat.desert`. The header's `uid` must equal the hash of the `UID_NS` record (a flipped bit in
either is refused) and is never 0 (a name that hashes to 0 is renamed).

A uid identifies a pack **among the packs of its kind** (a species and a habitat may share one; nothing holds them to
be apart). Two files with the same uid: the second is dropped if it is the same file (same length and CRC: the built-in
pack found again in a folder), and **refused** if it is any other file - this is also how two different names whose
hashes collide are caught ("collisions refused at load").

The uid is the pack's identity for good: it is what a save stores (below), what a species lists its habitats by, and
what a pack from a card is known by. It is never reused.

### Limits

`len <= 2048` (PTPK_BODY_MAX), so a file is at most 2068 bytes. Counts: 6 morphs, 8 goals, 4 habitats per species;
4 hides, 4 rocks (2..4 points each), 6 ramps per habitat. Texts: `NAME` 1..23 bytes, a morph's or pattern's name 1..11,
a goal's 1..15.

## Common records (a species and a habitat)

| id | name | REQ | value | meaning |
|---|---|---|---|---|
| 0x0001 | `NAME` | yes | text, 1..23 bytes | the name shown on cards: `LEOPARD GECKO` |
| 0x0002 | `UID_NS` | yes | `[a-z0-9._-]`, 3..47 bytes | the name the uid is derived from |
| 0x0003 | `WORLD` | yes | `u8`: 1 aquarium, 2 terrarium | the world this pack's goals, body and ground belong to (`world.h` PT_WORLD_*). A tank holds a species and a habitat of **its build's world**; a release firmware refuses the other world's pack at load, the simulator (PT_ALL_PACKS) keeps both |
| 0x0004 | `LEGACY_ID` | no | `u8`: species 1..255, habitat 1..31 | the one-byte id **old saves** hold. Honoured only for the packs that ship in the firmware; ignored in a pack that arrives from a folder or a card (those are told apart by uid alone) |
| 0x0005 | `TEST_ONLY` | no | `u8` = 1 | compiled in for the tests (A16's warm pack), never offered to a keeper |

## Species records (`kind` 1)

Field names are `species_t`'s (`common/species.h`); units as there: degrees C, seconds of tended time (the
progression clock), pixels at the 1.8's scale. Per-stage arrays are hatchling, juvenile, adult, elder.

| id | name | REQ | value | fills |
|---|---|---|---|---|
| 0x0101 | `HABITATS` | yes | `u8 n` (1..4), `n x u32` habitat uid | the habitats it can live in, by uid (`habitat_uid[]`; the loader derives `habitats`, the bit-per-runtime-id mask `habitat_fits` reads, whenever a habitat or a species is added). At least one must be loaded for the pack to load |
| 0x0102 | `ACTIVITY` | yes | `u8 schedule` (0 day, 1 dusk, 2 night), `f32 settle_s` | `schedule`, `settle_s` (only day runs: `species_check`) |
| 0x0103 | `HEAT` | yes | `f32 t_cold, t_lo, t_hi, t_hot` (C), `f32 tau_s[4]` (s) | `t_cold t_lo t_hi t_hot`, `tau_s[]` |
| 0x0104 | `DRIVES` | yes | 8 x `f32`: `hunger_per_s, energy_per_s, energy_rest_per_s, curiosity_per_s, hunger_per_cricket, sleep_hunger_per_h, sleep_energy_per_h, sleep_stress_per_h` | the drives, in `species_t` order (`hunger_per_cricket`: what one catch takes off, 0..10 scale; per second awake except the three `per_h`) |
| 0x0105 | `WALK` | yes | `f32 walk_px_s[4]` (px/s), `f32 bout_s[2]`, `f32 pause_s[2]` (s, lo..hi), `f32 turn_s` (s) | the walk of a terrarium pack. Absent: all zero (an aquarium pack swims) |
| 0x0106 | `LIFE` | yes | `f32 stage_s[3]` (tended age to reach juvenile, adult, elder), `u8 pop_cap` | `stage_s[]`, `pop_cap` (<= `POP_CAP`) |
| 0x0107 | `SHED` | yes | `u8 sheds` (0/1), `u8 shed_style` (0 whole, 1 patches), `f32 shed_every_s[4]`, `f32 shed_soon_s`, `f32 shed_window_s` | the shed cycle (`docs/shed.md`). Absent: `sheds` false, all zero (the fish) |
| 0x0108 | `DIET` | yes | `u8` bits: 1 cricket, 2 mealworm, 4 greens, 8 pellet | `diet` (any other bit: refused) |
| 0x0109 | `BODY` | yes | `u8 plan` (0 lizard, 1 fish), `f32 length_px, head_w, limb_len, tail_fat, eye_r`, `u8 eyelids` | `plan`, `geom` (a fish pack's geometry is 0) |
| 0x010a | `MORPHS` | no | `u8 n` (1..6), `n x (str name, u32 body, u32 accent)`; colours 0xRRGGBB | `morphs[]`, `n_morphs` (inline, no pointers) |
| 0x010b | `PATTERNS` | no | `u8 n` = 4, `4 x str` | `pattern[s]` for each stage: the pattern's name (`BANDED`, `SPOTTED` ... render_lizard.c reads the name) |
| 0x010c | `GOALS` | yes | `u8 n` (1..8), `n x str` goal name, lowercase | `goals`: the **names** of the goals its reflex performs, turned into bits when loaded |

Goals are stored by name because the goal ids in `tank.h` depend on order (`GOAL_LZ_FIRST == GOAL_FISH_FIRST +
FISH_GOALS`): a new model would shift them. The loader matches each name against the pack's world's list - the
terrarium's `hunt bask hide explore investigate dig rest` (`LZ_GOAL_NAMES`, bit k = the k-th), the aquarium's
`seek_food flee_shadow visit_bubbles follow_friend explore rest dart_play inspect_reef` (`FISH_GOAL_NAMES`) - and
refuses a name that is not in it (a `climb` needs a model that has the word) and a name given twice. Phase 2 takes the
list from the model's manifest.

`MORPHS` and `PATTERNS` are optional so a future firmware that draws a species some other way can ignore them, but
`species_check` still asks this firmware for a morph and a pattern for every stage.

## Habitat records (`kind` 2)

Field names are `habitat_t`'s (`common/habitat.h`); temperatures in C; `u`, `v` fractions of the glass
(`docs/habitat-pack.md` "The ground").

| id | name | REQ | value | fills |
|---|---|---|---|---|
| 0x0201 | `CLIMATE` | yes | `f32 floor_warm_c, floor_cool_c, night_drop_c, rise_c` | the air |
| 0x0202 | `BASK` | yes | `u8 bask_surf`, `f32 bask_u, bask_c, bask_r_px` | the basking spot and the lamp's pool (`bask_r_px` 0: no lamp) |
| 0x0203 | `HIDES` | yes | `u8 n` (0..4), `n x (u8 kind, f32 u, f32 t_lit_c, f32 t_dark_c)`; kind 0 warm, 1 humid, 2 cool | `hides[]`, `n_hides`. Absent: none |
| 0x0204 | `ROCKS` | yes | `u8 n` (0..4), `n x (u8 npts (2..4), u8 shelf (0/1), npts x (f32 u, f32 v))` | `rocks[]`, `n_rocks`. Absent: none |
| 0x0205 | `RAMPS` | yes | `u8 n` (0..6), `n x (u8 lo, u8 hi, f32 u_lo, f32 u_hi)` | `ramps[]`, `n_ramps`. Absent: none |
| 0x0206 | `MOISTURE` | yes | `f32 moist_start, moist_dry_per_s, humidity` | the humid hide's starting moisture and its drying (per tended second), the air's baseline |
| 0x0207 | `DISH` | yes | `f32 dish_u` | the mealworm dish at `dish_u` along the floor. Absent: no dish |

`AREAS` (water and land, the shores between them) for a crossover habitat is reserved (below); adding it changes
no existing record.

## What a loader refuses

In this order, with the reason in the log line (`why`):

1. no header (shorter than 20 B), the wrong magic, `fmt` 0 or newer than the loader's, `kind` 0 or unknown, `kind` 3
   (a model manifest: not loaded yet), a flag set, `uid` 0, `len` 0 or over 2048, a file that is not exactly
   `20 + len` bytes, a CRC that does not match the body.
2. a species or a habitat whose uid is already loaded (the identical file: dropped without a word; any other: refused).
3. a record that overruns the body, a known id twice, a record of the wrong size, an unknown REQUIRED id, a count
   past its limit, a non-finite float, an enum out of range, a text with a byte outside 0x20..0x7e (or with a leading or
   trailing space), a goal name that is not the world's, a `UID_NS` whose hash is not the header's uid, a `WORLD` that is
   not this build's (a release firmware), a species that lists no habitat that is loaded, a record the pack cannot do without
   missing (a species: `NAME UID_NS WORLD HABITATS ACTIVITY HEAT DRIVES LIFE DIET BODY GOALS`; a habitat: `NAME UID_NS WORLD CLIMATE BASK MOISTURE`;
   the rest have a default: no `WALK`, `SHED`, `MORPHS`, `PATTERNS`, `HIDES`, `ROCKS`, `RAMPS`, `DISH`).
4. the rules the engine always had: `species_check` / `habitat_check` (`docs/species-pack.md` "Validation",
   `docs/habitat-pack.md` "Rules"): the heat band (inside -100..200 C: a hostile float must not walk the monotonic check for ever), the rates, the shed cycle, the ground on all four boards ...
5. no room: the pool is full (below).

A refused pack never changes the registry and never crashes the loader: `sim/selftest_ptpk.c` flips every bit and cuts
every length of every built-in pack, mutates the body and fixes the CRC up so the record parser sees the damage, and
feeds the loader garbage.

## Versioning

`fmt` is 1. **Adding a record never changes the format**; only changing the meaning of a REQUIRED record bumps `fmt`.
A loader reads every fmt up to its own and refuses a newer one. A new optional record leaves the REQUIRED bit off and
is skipped by firmware that does not know it; a pack that cannot work without a new record sets the bit and older
firmware refuses it.

## Reserved for later phases

Not defined by fmt 1; the ids are held so nothing else takes them.

| id | name | phase | what |
|---|---|---|---|
| 0x0010 | `WORLDS` | 2 | bitmask, a filter for the shop and menus (`WORLD` stays the key until capability matching replaces it) |
| 0x0110 | `LOCOMOTION` | 2 | a list of {mode, area, speed by stage, bout/pause, turn} |
| 0x0111 | `MEDIA_NEEDS` | 2 | water or land, a basking spot, a shore |
| 0x0210 | `AREAS` | 2 | water, land and the shores between them (a paludarium) |
| 0x0300.. | model manifest (`kind` 3) | 2 | the model's goal names and inputs |

**A species on each animal** (mixed tanks, later). A tank holds one species today (`save_t.pack_id` / `species_uid`).
When several species share a tank each animal carries its own: a new save tail array `uint32_t animal_species_uid[N_FISH_MAX]`,
0 meaning "the tank's species" (`fish_save_t` is frozen). It needs no new pack record; it is written down here so the
format does not have to leave room for it.

## The loader

`common/packs/ptpk.c` parses (`ptpk_open`: header, length, CRC; `ptpk_read_species` / `ptpk_read_habitat`: the
records into a `species_t` / `habitat_t`); `common/species.c` and `common/habitat.c` are the registry. Everything in a
pack is inline: a name is a `char[]`, the morphs and patterns are arrays, nothing points into the file, so the file's
bytes are free to go as soon as the pack is loaded.

- **The pool.** The registry holds loaded packs in a fixed array - no malloc on the device path: this build's built-in
  packs (counted from the table in `common/packs/builtin.h`) plus spare slots for packs found at run time (2 of
  each kind in the firmware, 12 in the simulator). `species_commit` / `habitat_commit` refuse a pack when it is full.
- **Built-in packs.** `common/packs/builtin.c` lists them (habitats first; a species lists its habitats by uid). The
  firmware ships no `.ptpk` bytes: only the structs and the encoder. `packs_init()` (idempotent; the firmware calls it
  at boot, the simulator and the tests get it on first use) encodes each entry of this build's table into a buffer on
  the stack and offers it to `packs_add(..., PACKS_BUILTIN)` - the entry a folder or card file goes through, so a
  built-in pack meets the same validation, `species_check` / `habitat_check` and uid dedupe. A release firmware loads
  only its own world's non-test packs; a PT_ALL_PACKS build (the simulator and the tools) all of them. The simulator
  then also scans its folder.
- **Ids.** A built-in pack keeps its one-byte id (species 1 gecko, 2 fish, 250 the test pack; habitat 1 desert, 2
  freshwater, 31 the test desert) so a save from before uids loads unchanged. Any other pack is given the lowest free
  runtime id (species 3..249, habitat 3..30) when it is added; that id is a handle for this run only and is never stored.
- **Habitats and species.** `species_t.habitats` is derived: the registry recomputes it from `habitat_uid[]` whenever a
  pack is added. Habitats are loaded before species (the built-in table and the folder scan both do it).
- **One entry point.** `packs_add(blob, n, origin, ...)` takes the bytes of one file: the built-in packs (origin
  `PACKS_BUILTIN`), the simulator's folder (`POCKET_PACKS_DIR`, default `../packs`, `sim/packs_dir.c`) and the card
  (phase 4) all call it. It returns `PACKS_ADDED`, `PACKS_SAME` (the identical pack was already there) or
  `PACKS_REFUSED` with the reason.
- **The simulator's folder.** Every `*.ptpk` in the folder is offered, habitats first; a file with a new uid is available
  (`species_by_uid`); a corrupt one is refused with a log line (`packs: <file>: REFUSED: <why>`) and nothing else
  happens. While a tank is waiting for a pack the folder is read again every few seconds.

## A tank and its packs (the save)

The save stores the species' and the habitat's uid in a section **appended at the end** of `save_t` (`species_uid`,
`habitat_uid`, and their names as they were, for the notice), beside the one-byte ids it always held. On load a
pack is found by uid first, by the one-byte id if the save has no uid (an older save); a built-in pack keeps writing
its one-byte id so an older build still reads the save, a pack with none writes 0.

When the saved pair is not available - a pack that is not loaded, a pair that does not fit, another world's - the tank
does **not** fall back to the defaults. It keeps the saved uids, shows **PACK MISSING** (and the pack's name) and **pauses**:
the animals hold still, nothing grows, no hunger or drive advances, the clock stands, no sleep is lived through,
the keeper's hand does nothing on the glass. Every save while it waits writes the same uids and the same per-animal
tail it loaded. When the pack appears (a rescan of the folder, a reboot with the pack present) the tank resumes where
it stopped.

## Authoring

**The built-in packs are C.** The `const species_t` / `habitat_t` in `common/species/gecko.c`, `common/species/fish.c`,
`common/habitat/desert.c` and `common/habitat/freshwater.c` are the source (the two A16 test packs are derived there
from the gecko and the desert). `common/packs/ptpk_write.c` encodes a struct into a whole PTPK file: the tags, layouts
and REQUIRED bits of this page, goals by name, records in ascending id. `tools/ptpk_emit.c`, built and run by
`make -C sim packs` (part of `make check` and of the default simulator build), writes `packs/<uid_ns>.ptpk` for the six
built-ins, rewriting a file only when its bytes differ. Those files are checked in: they are what a card gets, and
they cannot go stale against the structs. Nothing else generates or checks in pack bytes.

**A new pack for a card or a folder** is authored as TOML and compiled with `tools/ptpk.py` (Python 3.11+, standard
library only: `tomllib`):

```
tools/ptpk.py build SRC.toml [-o F]   compile one source into a pack (default F: ./<uid_ns>.ptpk)
tools/ptpk.py dump FILE.ptpk          check the header, the CRC and the uid, and print the pack as the source form
tools/ptpk.py verify FILE.ptpk        check the header, length, CRC and the record walk; exit 1 when the file is bad
```

`build` is the only writer; its source is not a built-in's and lives outside the repo's `packs/`. Floats in a source
are decimal literals converted to the nearest binary32 *exactly* (no double rounding); `dump` writes the shortest
decimal that gives the same binary32 back, so `dump` then `build` reproduces a pack byte for byte. Unknown keys in a
source are errors; a species' `habitats` are `uid_ns` strings (`dump` writes an integer uid for a habitat it cannot
name).

A species source (units as above):

```toml
[pack]
kind = "species"
uid_ns = "pt.gecko.leopard"
name = "LEOPARD GECKO"
world = "terrarium"                  # "aquarium" | "terrarium"
legacy_id = 1                        # optional; built-in packs only
test_only = false                    # optional
habitats = ["pt.habitat.desert"]     # by uid_ns

[activity]   schedule = "day"        # "day" | "dusk" | "night"
             settle_s = 600.0
[heat]       t_cold = 20.0           # ... t_lo, t_hi, t_hot
             tau_s = [60.0, 90.0, 120.0, 150.0]
[drives]     hunger_per_s = 0.0045   # ... the eight, by name
[walk]       walk_px_s = [24.0, 22.0, 18.0, 15.0]    # optional (absent: it swims)
             bout_s = [0.8, 3.0]
             pause_s = [0.5, 2.5]
             turn_s = 0.7
[life]       stage_s = [1200.0, 5400.0, 28800.0]
             pop_cap = 3
[shed]       sheds = true            # optional (absent: it does not shed)
             style = "whole"         # "whole" | "patches"
             every_s = [900.0, 1800.0, 10800.0, 21600.0]
             soon_s = 300.0
             window_s = 600.0
[diet]       eats = ["cricket", "mealworm"]          # "cricket" "mealworm" "greens" "pellet"
[body]       plan = "lizard"         # "lizard" | "fish"
             length_px = 84.0        # ... head_w, limb_len, tail_fat, eye_r
             eyelids = true
[[morphs]]   name = "NORMAL"
             body = 0xe3b83f
             accent = 0x3b2c22
[patterns]   by_stage = ["BANDED", "BANDED", "SPOTTED", "SPOTTED"]
[goals]      names = ["hunt", "bask", "hide", "explore", "investigate", "dig", "rest"]
```

A habitat source:

```toml
[pack]       kind = "habitat"        # + uid_ns, name, world, legacy_id, test_only as above
[climate]    floor_warm_c = 31.0     # ... floor_cool_c, night_drop_c, rise_c
[bask]       surface = 3
             u = 0.2                 # ... c, r_px
[[hides]]    kind = "warm"           # "warm" | "humid" | "cool"
             u = 0.0
             t_lit_c = 30.0
             t_dark_c = 30.0
[[rocks]]    shelf = false
             points = [[0.114, 0.165], [0.42, 0.165]]     # (u, v), left to right
[[ramps]]    lo = 0
             hi = 1
             u_lo = 0.84
             u_hi = 0.42
[moisture]   start = 0.6             # ... dry_per_s, humidity
[dish]       u = 0.2                 # optional (absent: no dish)
```

The built-in packs: `pt.gecko.leopard` (id 1), `pt.fish.roster` (id 2: the aquarium's six-look fish), `pt.test.gecko-warm`
(id 250: A16's test pack, the gecko 6 C warmer: `common/species/gecko.c` derives it from the gecko and
`--selftest-heat` holds it to it), `pt.habitat.desert` (1), `pt.habitat.freshwater` (2), `pt.test.desert-warm` (31: the
desert 6 C warmer throughout, derived in `common/habitat/desert.c`).

## Tests

`./fishsim --selftest-ptpk` (both worlds; part of `make check`): the headers are exact (20 bytes, the uid, the CRC);
every built-in **round-trips**: `ptpk_encode_*` of its `const species_t` / `habitat_t` and `ptpk_read_*` of those bytes
give the struct back, field for field (the runtime-derived fields, the `habitats` mask and the ids, aside); the
encoder's bytes equal `packs/<uid_ns>.ptpk` when the simulator finds that file (run from `sim/`, `../packs`); the fish
pack's drives are `tank.h`'s numbers; every
bit flip and every truncation of every built-in is refused, and the same fuzz with the CRC fixed up never crashes the
record parser and never loads a pack that fails its rules; an unknown optional record is skipped and an unknown
REQUIRED one refused; a duplicate record, a bad goal name, a uid that is not the name's, a collision, a full pool,
a pack from another world are refused; a folder with a new pack, a corrupt one, a short one and a duplicate loads the
first and refuses the rest; a tank whose pack is missing pauses, keeps its uids through a save and a reload, and
resumes when the pack comes. `make check` also builds and runs `tools/ptpk_emit.c` (`make -C sim packs`), so
`packs/*.ptpk` cannot be stale against the structs.
