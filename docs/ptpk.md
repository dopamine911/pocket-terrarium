# PTPK - the pack file format (fmt 1)

A **pack** is one species, one habitat or one model manifest as a file. The engine, the model and the renderer never name a
species or a habitat; they read the pack (`docs/species-pack.md`, `docs/habitat-pack.md`). Since issue #4 phase 1 a pack is
a PTPK file: the built-in packs are encoded from their C structs at boot and go through the same loader a pack from a
folder (the simulator, now) or from the microSD card (phase 4) goes through. One loader, one set of refusals. Phase 2 adds
capability matching: what a species needs, what a habitat, the firmware and the model provide ("Capabilities" below), and
the **model manifest** (`kind` 3), the model's own pack, which says which goal words it provides.

This page is the byte-exact contract. `common/packs/ptpk_write.c` writes it (from the built-in C structs;
`tools/ptpk.py build` writes a new pack from TOML), `common/packs/ptpk.c` reads it, `sim/selftest_ptpk.c` and
`sim/selftest_caps.c` hold them to it.

Phase 1 was "format and loader, built-in packs only"; phase 2 is "capability matching". `fmt` stays 1: phase 2 retires the
records it replaces (0x0003, a pack's world; 0x0105, the walk; their ids stay unused) and adds records, nothing is kept for
compatibility. Out of scope here and reserved below: the module split (phase 3), the card library (4), switching models (5),
turtles.

## The file

All numbers are **little-endian**. A float (`f32`) is an IEEE-754 binary32 and is always finite (no NaN, no infinity:
a loader refuses a pack that holds one). The file is exactly `20 + len` bytes: a header, then the body.

```
header, 20 bytes
  off  size  field   value
    0     4  magic   "PTPK" (0x50 0x54 0x50 0x4b)
    4     1  fmt     1
    5     1  kind    1 species | 2 habitat | 3 model manifest
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

`len <= 2048` (PTPK_BODY_MAX), so a file is at most 2068 bytes. Counts: 6 morphs, 8 goals, 4 habitats, 3 locomotion
entries per species; 8 goals per model manifest; 4 hides, 4 rocks (2..4 points each), 6 ramps per habitat. Texts: `NAME`
1..23 bytes, a morph's or pattern's name 1..11, a goal's 1..15, a model tag 1..15.

## Common records (a species and a habitat)

| id | name | REQ | value | meaning |
|---|---|---|---|---|
| 0x0001 | `NAME` | yes | text, 1..23 bytes | the name shown on cards: `LEOPARD GECKO` |
| 0x0002 | `UID_NS` | yes | `[a-z0-9._-]`, 3..47 bytes | the name the uid is derived from |
| 0x0003 | - | - | - | **retired** (phase 1 held a pack's world here). The id stays unused; a pack that carries it has an unknown REQUIRED record and is refused |
| 0x0004 | `LEGACY_ID` | no | `u8`: species 1..255, habitat 1..31 | the one-byte id **old saves** hold. Honoured only for the packs that ship in the firmware; ignored in a pack that arrives from a folder or a card (those are told apart by uid alone) |
| 0x0005 | `TEST_ONLY` | no | `u8` = 1 | compiled in for the tests (A16's warm pack), never offered to a keeper |
| 0x0010 | `WORLDS` | no | `u8` mask 1..255: bit 0 (1) aquarium, bit 1 (2) terrarium (`world.h` PT_WORLD_*) | the worlds whose shop and menus offer the pack: a **filter**, nothing more (`species_t.worlds`, `habitat_t.worlds`; absent: 0, every world). A bit nobody knows is kept: it filters the pack out of every world this firmware knows. It never refuses a pack and never decides a pair ("Capabilities") |

`WORLDS`, `LEGACY_ID` and `TEST_ONLY` are species and habitat records; in a model manifest they are unknown (below).

## Species records (`kind` 1)

Field names are `species_t`'s (`common/species.h`); units as there: degrees C, seconds of tended time (the
progression clock), pixels at the 1.8's scale. Per-stage arrays are hatchling, juvenile, adult, elder.

| id | name | REQ | value | fills |
|---|---|---|---|---|
| 0x0101 | `HABITATS` | yes | `u8 n` (1..4), `n x u32` habitat uid | the habitats it can live in, by uid (`habitat_uid[]`; the loader derives `habitats`, the bit-per-runtime-id mask `packs_compatible` reads, whenever a habitat or a species is added). At least one must be loaded for the pack to load. The list stays authoritative: the capability check is in addition to it |
| 0x0102 | `ACTIVITY` | yes | `u8 schedule` (0 day, 1 dusk, 2 night), `f32 settle_s` | `schedule`, `settle_s` (only day runs: `species_check`) |
| 0x0103 | `HEAT` | yes | `f32 t_cold, t_lo, t_hi, t_hot` (C), `f32 tau_s[4]` (s) | `t_cold t_lo t_hi t_hot`, `tau_s[]` |
| 0x0104 | `DRIVES` | yes | 8 x `f32`: `hunger_per_s, energy_per_s, energy_rest_per_s, curiosity_per_s, hunger_per_cricket, sleep_hunger_per_h, sleep_energy_per_h, sleep_stress_per_h` | the drives, in `species_t` order (`hunger_per_cricket`: what one catch takes off, 0..10 scale; per second awake except the three `per_h`) |
| 0x0106 | `LIFE` | yes | `f32 stage_s[3]` (tended age to reach juvenile, adult, elder), `u8 pop_cap` | `stage_s[]`, `pop_cap` (<= `POP_CAP`) |
| 0x0107 | `SHED` | yes | `u8 sheds` (0/1), `u8 shed_style` (0 whole, 1 patches), `f32 shed_every_s[4]`, `f32 shed_soon_s`, `f32 shed_window_s` | the shed cycle (`docs/shed.md`). Absent: `sheds` false, all zero (the fish) |
| 0x0108 | `DIET` | yes | `u8` bits: 1 cricket, 2 mealworm, 4 greens, 8 pellet | `diet` (any other bit: refused) |
| 0x0109 | `BODY` | yes | `u8 plan` (0 lizard, 1 fish), `f32 length_px, head_w, limb_len, tail_fat, eye_r`, `u8 eyelids` | `plan`, `geom` (a fish pack's geometry is 0) |
| 0x010a | `MORPHS` | no | `u8 n` (1..6), `n x (str name, u32 body, u32 accent)`; colours 0xRRGGBB | `morphs[]`, `n_morphs` (inline, no pointers) |
| 0x010b | `PATTERNS` | no | `u8 n` = 4, `4 x str` | `pattern[s]` for each stage: the pattern's name (`BANDED`, `SPOTTED` ... common/bodies/lizard/lizard.c reads the name) |
| 0x010c | `GOALS` | yes | `u8 n` (1..8), `n x str` goal name: 1..15 bytes of `[a-z][a-z0-9_]*`, none twice | `goal[]`, `n_goals`: the **names** of the goals its reflex performs. Resolved by `species_check` against the manifests, not at read time |
| 0x0110 | `LOCOMOTION` | yes | `u8 n` (1..3), `n x (u8 mode, u8 area, f32 px_s[4], f32 bout_s[2], f32 pause_s[2], f32 turn_s)`: 1 + 38 n bytes | `loco[]`, `n_loco`; the WALK entry's numbers fill `walk_px_s`, `bout_s`, `pause_s`, `turn_s` |
| 0x0111 | `MEDIA_NEEDS` | yes | `u32 needs`, `u32 wants` (8 bytes) | `media_needs`, `media_wants` |

Goals are stored by name because the goal ids in `tank.h` depend on order (`GOAL_LZ_FIRST == GOAL_FISH_FIRST +
FISH_GOALS`): a new model would shift them. The file's names are kept as they are (`species_t.goal[]`); no world's list is
consulted at read time. A model's manifest lists the words it provides (`hunt bask hide explore investigate dig rest` for
`lizard_v1`; `seek_food flee_shadow visit_bubbles follow_friend explore rest dart_play inspect_reef` for the fish model
`v3m`), and `species_check` refuses a name that is in the goal list of no manifest this build carries (`the goal "climb" is
in no model's goal list`: `climb` stays refused because no model has the word) and the loader refuses a name given twice. At
the tank the pair check asks the loaded model's own manifest ("Capabilities").

**LOCOMOTION.** A mode is a capability index (0 SWIM, 1 WALK, 2 SLITHER) and appears once; an area is a capability index
(4 WATER, 5 LAND, 6 BASK, 7 SHORE). A mode index that is not a known movement mode, or an area index that is not a known
area, is refused naming the capability (`capability bit N, which this firmware does not know` when no capability has that
number). A SWIM or SLITHER entry carries **no numbers yet**: all nine floats must be +0.0 (zero bits) - the swim runs on
`tank.h`'s constants; phase 3 relaxes this.

**MEDIA_NEEDS.** `needs` is the areas and fittings the species cannot live without (a subset of WATER LAND BASK SHORE LAMP
HIDES); `wants` is what the pack makes use of when it is there but runs without (any known capability). Unknown bits:
"Capabilities".

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
| 0x0210 | `AREAS` | yes | `u32 areas` | `areas`: a subset of WATER LAND BASK SHORE; a bit this firmware does not know is dropped (a habitat may offer more than this firmware can use); a known bit that is not an area is refused |
| 0x0211 | `PROVIDES` | yes | `u32 provides` | `provides`: a subset of LAMP HIDES CRICKET MEALWORM GREENS PELLET; unknown bits dropped; a known bit that is not a provision is refused |

## Model manifest records (`kind` 3)

A manifest is the model's answer to "what do you provide": its tag and its goal words. `NAME` and `UID_NS` (common records,
above) are required; `WORLDS`, `LEGACY_ID` and `TEST_ONLY` are unknown for kind 3 (refused if their REQUIRED bit is set,
skipped if not), and so is a species' or a habitat's record. The built-in manifests are `pt.model.lizard_v1` (uid
0xb551e3d0) and `pt.model.fish_v3m` (uid 0x4bc519a3).

| id | name | REQ | value | meaning |
|---|---|---|---|---|
| 0x0301 | `MODEL_TAG` | yes | text, 1..15 bytes of `[A-Za-z0-9._-]` | the model's tag as `common/version.h` names it (`lizard_v1`, `v3m`; `PT_MODEL_TAG`) |
| 0x0302 | `MODEL_GOALS` | yes | as `GOALS`: `u8 n` (1..8), `n x str` | the model's goal words, in its candidate order |

## Capabilities

A tank runs only when `species needs ⊆ (habitat provides ∩ firmware provides ∩ model provides)`. No world field, record or
build-time constant (`PT_WORLD`) decides anything in this rule. A **world** is a preset
(which firmware modules, which model, which default packs) and a filter (`WORLDS`: the shop and the menus). A species'
own habitat list (`HABITATS`) stays authoritative; the capability check is **in addition** (it is what lets a crossover
habitat work later).

One u32 bit set, `caps_t` (`common/packs/caps.h`):

| bit | name | kind | meaning |
|---|---|---|---|
| 0 | SWIM | movement mode | the firmware has the swim module |
| 1 | WALK | movement mode | the walk module (`tank.c` THE WALK) |
| 2 | SLITHER | movement mode | named; no firmware has it yet |
| 3 | - | movement (room) | not a known bit (the unknown-capability tests use it) |
| 4 | WATER | area | water to swim in |
| 5 | LAND | area | ground to walk on (the terrarium's floor, rocks, ramps) |
| 6 | BASK | area | a basking spot under a lamp |
| 7 | SHORE | area | where water meets land (needs WATER and LAND) |
| 8 | CRICKET | food | `DIET_CRICKET << 8` |
| 9 | MEALWORM | food | `DIET_MEALWORM << 8` (the habitat's dish) |
| 10 | GREENS | food | `DIET_GREENS << 8` |
| 11 | PELLET | food | `DIET_PELLET << 8` |
| 12 | LAMP | fitting | a heat lamp |
| 13 | HIDES | fitting | hides (a humid one among them if LAND) |

Masks: `CAPS_MOVES` bits 0..2, `CAPS_AREAS` 4..7, `CAPS_FOODS` 8..11, `CAPS_FITTINGS` 12..13, `CAPS_KNOWN` all of those. Any
other bit (3, 14..31) is **unknown** to this firmware. In a pack file a single capability (a mode or an area) is its bit
**index** (`u8`, 0..31); a set is the u32 mask. Names, for messages and (lowercased) for source files: SWIM WALK SLITHER
WATER LAND BASK SHORE CRICKET MEALWORM GREENS PELLET LAMP HIDES.

- **Species needs** (`species_needs(s)`, derived in one place) = every LOCOMOTION entry's mode bit and area bit, | `MEDIA_NEEDS`
  `needs`, | the `DIET` foods (the diet bits `<< 8`).
- **Habitat provides** (`habitat_provides(h)`) = `AREAS` | `PROVIDES` | the movement its areas carry (WATER gives SWIM; LAND
  gives WALK and SLITHER).
- **Firmware provides** (`firmware_caps()`, `common/packs/caps.h`, `CAPS_FW`): a build constant. Terrarium: WALK LAND BASK LAMP HIDES
  CRICKET MEALWORM. Aquarium: SWIM WATER PELLET. A PT_ALL_PACKS build (the simulator and the tools): both worlds' union.
  `firmware_caps()` says it, beside the module list in `common/locomotion/modules.c` (`docs/modules.md`): the two places the step that
  builds both worlds into one image changes, nothing around them; the simulator may override it with
  `firmware_caps_override(bool on, caps_t caps)` (declared only `#if PT_ALL_PACKS`), which is how the tests give a build a
  single module or both.
- **Model provides** (`model_provides()`): the goal words of the loaded model's manifest, the one whose tag is
  `PT_MODEL_TAG` (`lizard_v1` in a terrarium build, `v3m` in an aquarium build).

**Unknown bits.** In a species' `needs` (or a LOCOMOTION mode or area): the loader **refuses** the pack, naming the bit
("unknown REQUIRED capability bit"; a firmware that cannot do what the species cannot live without must not run it). In
`wants`, in a habitat's `AREAS` or `PROVIDES`: the bit is **dropped** ("unknown optional capability bit skipped"). A known
bit of the wrong class (a movement mode or a food in `needs`; an area in `PROVIDES`) is refused.

**The simulator keeps both worlds' packs loaded.** The release image loads only its world's built-ins because the firmware
links only those; a PT_ALL_PACKS build loads all of them. A cross-world pair (the gecko in an aquarium build) is then
refused at the tank by the **model** leg: the gecko's goal `hunt` is not in the fish model's manifest.

**The pair check** (`packs_compatible(species, habitat, why, n)`, which `tank_set_packs` uses): in this order, the first
failure is the reason:

1. the species lists the habitat (its `habitats` bit): `<SPECIES> does not live in <HABITAT>` (short need `ANOTHER HABITAT`);
2. needs the firmware lacks: `<SPECIES> needs <CAP, CAP>; this firmware has none` (capabilities in bit order, `, ` between);
3. needs the habitat lacks: `<SPECIES> needs <CAP, CAP>; <HABITAT> has none` (`LEOPARD GECKO needs WALK, LAND, BASK, CRICKET,
   MEALWORM, HIDES; FRESHWATER has none`: the freshwater has no land, and so none of what land carries);
4. goals the model lacks: `<SPECIES> needs the goal "<word>"; the <tag> model has none` (short need `GOAL <WORD>`).

The **short need** (for the keeper's card) is the first unmet capability's name (`BASK`), `GOAL CLIMB`, or
`ANOTHER HABITAT`. `packs_check(s, h, need, need_n, why, why_n)` gives both texts; `packs_check_with(s, h, fw, model, need,
need_n, why, why_n)` takes the providers as arguments (the tests). `packs_offered(s, h)` = both packs' `WORLDS` include this
build's world, neither is test-only, and the pair is compatible: the filter the shop and the menus will call (no menu exists
yet).

## What a loader refuses

**At load**: what the firmware can never meet, or a file that is not a pack. The reason, naming the capability when there is
one, is in the log line (`why`); the file is dropped and the registry unchanged. In this order:

1. no header (shorter than 20 B), the wrong magic, `fmt` 0 or newer than the loader's, `kind` 0 or unknown (1, 2 and 3 are
   known), a flag set, `uid` 0, `len` 0 or over 2048, a file that is not exactly `20 + len` bytes, a CRC that does not match
   the body.
2. a pack whose uid is already loaded among its kind (the identical file: dropped without a word; any other: refused); a
   manifest whose tag is already loaded under another uid.
3. a record that overruns the body, a known id twice, a record of the wrong size, an unknown REQUIRED id (for a manifest: a
   species' or a habitat's record, or `WORLDS`, `LEGACY_ID`, `TEST_ONLY`, with the bit set), a count past its limit, a
   non-finite float, an enum out of range, a text with a byte outside 0x20..0x7e (or with a leading or trailing space), a
   goal name that is malformed or given twice, a `UID_NS` whose hash is not the header's uid, a record the pack cannot do
   without missing (species: `NAME UID_NS HABITATS ACTIVITY HEAT DRIVES LIFE DIET
   BODY GOALS LOCOMOTION MEDIA_NEEDS`; habitat: `NAME UID_NS CLIMATE BASK MOISTURE AREAS PROVIDES`; manifest: `NAME UID_NS
   MODEL_TAG MODEL_GOALS`; the rest have a default: no `SHED`, `MORPHS`, `PATTERNS`, `HIDES`, `ROCKS`, `RAMPS`, `DISH`,
   `WORLDS`).
4. a capability this firmware does not know in `needs` or in a LOCOMOTION mode or area, a known bit of the wrong class,
   non-zero numbers in a SWIM or SLITHER entry.
5. a movement mode `firmware_caps()` lacks (`it moves by SWIM, and this firmware has no such module`: a terrarium firmware
   refuses a fish pack because it has no SWIM). It is said **before anything about habitats**: a pack no habitat could help is
   refused for that, whether or not its habitats are loaded.
6. a species that lists no habitat that is loaded.
7. the rules the engine always had, and the capability rules: `species_check` / `habitat_check` / `manifest_check`
   (`docs/species-pack.md` "Validation", `docs/habitat-pack.md` "Rules"): the heat band (inside -100..200 C: a hostile float
   must not walk the monotonic check for ever), the rates, the shed cycle, the ground on all four boards ...; a goal in no
   carried manifest (`the goal "climb" is in no model's goal list`; a release image carries only its own model's manifest, the
   simulator both).
8. no room: the pool is full (below).

There is no world refusal: another world's pack loads (the world is a filter, not a gate) unless 4, 5 or 7 refuse it. In a release
image (`make -C sim check-release`: `sim/release_check.c`) that is the other world's species - it moves by a mode this
firmware lacks - while the other world's habitat and manifest load, as they need nothing.

**At the tank**: what is left is a question about a pair. A pack that loaded can still fail the pair check (the habitat is not
in the species' list, the habitat lacks a need, the model lacks a goal: "Capabilities"). `tank_set_packs` refuses the pair
and returns the reason (`no species with id N` / `no habitat with id N` for an unknown id); a saved pair that fails keeps
the tank paused ("A tank and its packs").

A refused pack never changes the registry and never crashes the loader: `sim/selftest_ptpk.c` flips every bit and cuts
every length of every built-in pack (manifests included), mutates the body and fixes the CRC up so the record parser sees
the damage, and feeds the loader garbage.

## Versioning

`fmt` is 1. **Adding a record never changes the format**; only changing the meaning of a REQUIRED record bumps `fmt`.
A loader reads every fmt up to its own and refuses a newer one. A new optional record leaves the REQUIRED bit off and
is skipped by firmware that does not know it; a pack that cannot work without a new record sets the bit and older
firmware refuses it.

## Reserved for later phases

Not defined by fmt 1; the ids are held so nothing else takes them.

| id | name | phase | what |
|---|---|---|---|
| 0x0303 | `INPUTS` | 5 | a model manifest record for the model's inputs (reserved, not defined) |

LOCOMOTION's "no numbers yet" for a swim or slither entry is relaxed in phase 3 (the swim's numbers move out of `tank.h`
into the pack); it changes no layout.

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
  packs (counted from the table in `common/packs/builtin.h`) plus spare slots for packs found at run time (none in the firmware
  until the card library of phase 4 needs them - each costs a `species_t` of 576 B and a `habitat_t` of 400 B of `.bss`, and the 1.75C's
  aquarium runs on a few KB of internal heap; twelve in the simulator). `species_commit` / `habitat_commit` / the manifest registry refuse a
  pack when it is full.
- **Built-in packs.** `common/packs/builtin.c` lists them (manifests and habitats first; a species lists its habitats by uid and names the goals of the manifests). The
  firmware ships no `.ptpk` bytes: only the structs and the encoder. `packs_init()` (idempotent; the firmware calls it
  at boot, the simulator and the tests get it on first use) encodes each entry of this build's table into a buffer on
  the stack and offers it to `packs_add(..., PACKS_BUILTIN)` - the entry a folder or card file goes through, so a
  built-in pack meets the same validation, `species_check` / `habitat_check` and uid dedupe. A release firmware loads
  only its own world's non-test packs (and its own model's manifest); a PT_ALL_PACKS build (the simulator and the tools)
  all of them (both manifests). The simulator then also scans its folder.
- **Ids.** A built-in pack keeps its one-byte id (species 1 gecko, 2 fish, 250 the test pack; habitat 1 desert, 2
  freshwater, 31 the test desert) so a save from before uids loads unchanged. Any other pack is given the lowest free
  runtime id (species 3..249, habitat 3..30) when it is added; that id is a handle for this run only and is never stored.
- **Habitats and species.** `species_t.habitats` is derived: the registry recomputes it from `habitat_uid[]` whenever a
  pack is added. Manifests and habitats are loaded before species (the built-in table and the folder scan both do it).
- **Capabilities.** `firmware_caps()` (`common/packs/caps.h`: the constants `CAPS_FW_*`, beside the module list `common/locomotion/modules.c`) and
  `model_provides()` (`common/packs/manifest.c`; "Capabilities") are the two providers
  the pair check reads. **The model manifest registry** (`common/packs/manifest.c`, `model_manifest_t`: name, uid, builtin,
  tag, goals) is the third registry beside species and habitats, with a pool of its own: this build's built-in manifests
  (one in a release image, two in the simulator) plus spare slots (none in the firmware yet). A manifest from a folder loads into a
  spare slot; its tag must be new (the same tag under another uid is refused).
- **One entry point.** `packs_add(blob, n, origin, ...)` takes the bytes of one file of any kind (species, habitat, model
  manifest): the built-in packs (origin `PACKS_BUILTIN`), the simulator's folder (`POCKET_PACKS_DIR`, default `../packs`,
  `sim/packs_dir.c`) and the card (phase 4) all call it. It returns `PACKS_ADDED`, `PACKS_SAME` (the identical pack was
  already there) or `PACKS_REFUSED` with the reason.
- **The pair.** `packs_compatible` / `packs_check` / `packs_check_with` / `packs_offered` ("Capabilities") are the questions
  asked of loaded packs; `tank_set_packs` returns false and a reason. The `s_one_world` filter of phase 1 is gone.
- **The simulator's folder.** Every `*.ptpk` in the folder is offered, manifests and habitats before species; a file with a
  new uid is available (`species_by_uid`); a corrupt one is refused with a log line (`packs: <file>: REFUSED: <why>`) and
  nothing else happens. While a tank is waiting for a pack the folder is read again every few seconds.

## A tank and its packs (the save)

The save stores the species' and the habitat's uid in a section **appended at the end** of `save_t` (`species_uid`,
`habitat_uid`, and their names as they were, for the notice), beside the one-byte ids it always held. On load a
pack is found by uid first, by the one-byte id if the save has no uid (an older save); a built-in pack keeps writing
its one-byte id so an older build still reads the save, a pack with none writes 0.

When the saved pair is not available - a pack that is not loaded, a pair that does not fit, a need the firmware, the habitat or
the model cannot meet - the tank does **not** fall back to the defaults. It keeps the saved uids, shows **PACK MISSING** (and
the pack's name) and **pauses**: the animals hold still, nothing grows, no hunger or drive advances, the clock stands, no
sleep is lived through, the keeper's hand does nothing on the glass. Every save while it waits writes the same uids and the
same per-animal tail it loaded. When the pack appears (a rescan of the folder, a reboot with the pack present) the tank
resumes where it stopped.

**An unmet need** is the same paused state: both packs are loaded but the pair fails the check. The card says **`PACK NEEDS
<NEED>`** (the short need: `BASK`, `GOAL CLIMB`, `ANOTHER HABITAT`; `progression_packs_need(t)`, "" when the pack itself is not
loaded), then the pack's name, then the world line. `progression` looks again every second and resumes the tank when the pair
is satisfied.

## Authoring

**The built-in packs are C.** The `const species_t` / `habitat_t` in `common/species/gecko.c`, `common/species/fish.c`,
`common/habitat/desert.c` and `common/habitat/freshwater.c` are the source (the two A16 test packs are derived there
from the gecko and the desert), and the two model manifests are C data in `common/packs/models.c`
(`MODEL_MANIFEST_LIZARD_V1`, `MODEL_MANIFEST_FISH_V3M`, tags from `common/version.h`). `common/packs/ptpk_write.c` encodes a
struct into a whole PTPK file: the tags, layouts and REQUIRED bits of this page, goals by name, records in ascending id.
`tools/ptpk_emit.c`, built and run by `make -C sim packs` (part of `make check` and of the default simulator build), writes
`packs/<uid_ns>.ptpk` for the eight built-ins, rewriting a file only when its bytes differ. Those files are checked in: they
are what a card gets, and they cannot go stale against the structs. Nothing else generates or checks in pack bytes.

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
name). `dump` prints the same form and `dump | build` reproduces the file byte for byte; `dump` and `verify` work on kind 3.
Goal names are only syntax-checked by the tool (the loader and the manifests decide what a word means).

A species source (units as above):

```toml
[pack]
kind = "species"                     # "species" | "habitat" | "model"
uid_ns = "pt.gecko.leopard"
name = "LEOPARD GECKO"
worlds = ["terrarium"]               # optional; ["aquarium"] ["terrarium"] or both: the WORLDS record (filter only)
legacy_id = 1                        # optional; built-in packs only
test_only = false                    # optional
habitats = ["pt.habitat.desert"]     # by uid_ns

[activity]   schedule = "day"        # "day" | "dusk" | "night"
             settle_s = 600.0
[heat]       t_cold = 20.0           # ... t_lo, t_hi, t_hot
             tau_s = [60.0, 90.0, 120.0, 150.0]
[drives]     hunger_per_s = 0.0045   # ... the eight, by name
[[locomotion]]                       # 1..3 tables, one per mode (required)
             mode = "walk"           # "swim" | "walk" | "slither"
             area = "land"           # "water" | "land" | "bask" | "shore"
             px_s = [24.0, 22.0, 18.0, 15.0]     # walk only: a swim / slither table has no number keys
             bout_s = [0.8, 3.0]
             pause_s = [0.5, 2.5]
             turn_s = 0.7
[media]      needs = ["land", "bask", "hides"]   # required; names only: water land bask shore lamp hides
             wants = []                          # optional
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
[goals]      names = ["hunt", "bask", "hide", "explore", "investigate", "dig", "rest"]   # [a-z][a-z0-9_]*, 1..15 bytes, 1..8, distinct
```

The walk is the numbers of the `walk` `[[locomotion]]` table (there is no separate walk table). Capability names are the lowercased names of
"Capabilities". `[media] needs` takes {water, land, bask, shore, lamp, hides} only (a movement or a food name is an error:
it belongs in `[[locomotion]]` / `[diet]`); `wants` takes any known capability name; an unknown name is an error. **`bitNN`**
(NN 0..31, not one of the 13 named bits; bit 3 and 14..31 are unknown) names a capability this tool does not: it is accepted
in `wants`, `[areas] has` and `[provides] has` (the loader drops it) and not in `needs`, a mode or an area. Each list: no name
twice. `worlds` entries are "aquarium" / "terrarium", encoded as the mask 1 / 2 (both 3); an empty list or an absent key
writes no `WORLDS` record. An unknown bit in `wants` / `provides` prints as `bitNN` in `dump`; an unknown bit in `needs`, a
mode or an area makes `dump` and `verify` fail, as the loader refuses it.

A habitat source:

```toml
[pack]       kind = "habitat"        # + uid_ns, name, worlds, legacy_id, test_only as above
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
[areas]      has = ["land", "bask"]  # required: water land bask shore (+ bitNN)
[provides]   has = ["lamp", "hides", "cricket", "mealworm"]   # required: lamp hides cricket mealworm greens pellet (+ bitNN)
```

A model manifest source has `[pack]` (`kind = "model"`, `uid_ns`, `name`, and `tag`: 1..15 of `[A-Za-z0-9._-]`) and `[goals]`,
and no other section:

```toml
[pack]       kind = "model"
             uid_ns = "pt.model.lizard_v1"
             name = "LIZARD V1"
             tag = "lizard_v1"
[goals]      names = ["hunt", "bask", "hide", "explore", "investigate", "dig", "rest"]
```

The built-in packs: `pt.gecko.leopard` (id 1), `pt.fish.roster` (id 2: the aquarium's six-look fish), `pt.test.gecko-warm`
(id 250: A16's test pack, the gecko 6 C warmer: `common/species/gecko.c` derives it from the gecko and
`--selftest-heat` holds it to it), `pt.habitat.desert` (1), `pt.habitat.freshwater` (2), `pt.test.desert-warm` (31: the
desert 6 C warmer throughout, derived in `common/habitat/desert.c`), and the model manifests `pt.model.lizard_v1` and
`pt.model.fish_v3m` (no one-byte id: a manifest is found by tag and uid).

## Tests

`./fishsim --selftest-ptpk` (both worlds; part of `make check`): the headers are exact (20 bytes, the uid, the CRC);
every built-in **round-trips**: `ptpk_encode_*` of its `const species_t` / `habitat_t` / manifest and `ptpk_read_*` of those
bytes give the struct back, field for field (the runtime-derived fields, the `habitats` mask and the ids, aside); the
encoder's bytes equal `packs/<uid_ns>.ptpk` when the simulator finds that file (run from `sim/`, `../packs`); the fish
pack's drives are `tank.h`'s numbers; every bit flip and every truncation of every built-in is refused, and the same fuzz
with the CRC fixed up never crashes the record parser and never loads a pack that fails its rules; an unknown optional record
is skipped and an unknown REQUIRED one refused; a duplicate record, a bad goal name, a uid that is not the name's, a
collision, a full pool are refused; a folder with a new pack, a corrupt one, a short one and a duplicate loads the first and
refuses the rest; a tank whose pack is missing pauses, keeps its uids through a save and a reload, and resumes when the
pack comes.

`./fishsim --selftest-caps` (`sim/selftest_caps.c`, in the shared list): every built-in pair passes the check; a synthetic
crossover (needs WALK and SWIM, a habitat that provides both) is accepted when the firmware has both and refused on a
single-module firmware, naming the module; a need the habitat lacks is refused with its name; a goal missing from the
manifest is refused naming it; an unknown REQUIRED capability bit is refused and an unknown optional one skipped; manifest
fuzz (bit flips, truncations) is refused; an unmet need behaves like a missing pack (the tank pauses, keeps its uids and
resumes) with a folder pack. `make check` also builds and runs `tools/ptpk_emit.c` (`make -C sim packs`), so `packs/*.ptpk`
cannot be stale against the structs.
