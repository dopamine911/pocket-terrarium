# Upstream divergence

This is the complete change map for pocket-terrarium relative to the upstream
release it forked, not just a description of the module split. For how the
resulting system fits together, including the diagrams, see the
[architecture overview](architecture-overview.md).

## Scope and provenance

| Role | Repository / revision | Meaning |
|---|---|---|
| Shared upstream base | [mediacutlet/pocket-tank v0.4.0](https://github.com/mediacutlet/pocket-tank/commit/bb49048b8f393cc63e060657076bba747973b4e2), `bb49048b8f393cc63e060657076bba747973b4e2` | Released 2026-11-03; common ancestry, not a fork-original contribution |
| Fork snapshot described here | [dopamine911/pocket-terrarium](https://github.com/dopamine911/pocket-terrarium/commit/fe3ec7665e2bf02199d31ba26165429b4616d779), `fe3ec7665e2bf02199d31ba26165429b4616d779` | `main` after PR #12, 2026-11-06; the endpoint of the inventory below |
| Later upstream comparison | [pocket-tank v0.4.1](https://github.com/mediacutlet/pocket-tank/commit/952ef4d4eadeaa1ef455292e1516376cd8365eb9), `952ef4d4eadeaa1ef455292e1516376cd8365eb9` | A separate release commit on top of the shared base, not merged into this fork snapshot |

The review uses the **full commit DAG**, `bb49048..fe3ec76`, including merged
branches: 76 commits, of which 67 are non-merge commits and nine are merges.
The net `git diff --name-status -M bb49048 fe3ec76` contains **262 changed-file
entries: 182 additions, 79 modifications and one detected rename**. Every entry
is recorded in the [categorized inventory](#complete-changed-file-inventory).
An added destination can contain code extracted from an existing file; these
counts are not a claim that all those lines were newly invented here. The two
new overview documents and their README index entries are outside that pinned
implementation snapshot.

**Status vocabulary:** *implemented* means present in the checked-in code or
artifacts at the fork snapshot; it does not mean a new public release or a
fresh hardware test. Historical measurements are explicitly attributed to the
runbook or board notes. *Planned* means the design reserves or describes it but
the snapshot does not provide the end-to-end feature. Earlier progress entries
in [PLAN.md](../PLAN.md) and the retraining runbook sometimes say “open” or “not
committed” before later entries close them; source at the pinned snapshot takes
precedence over those intermediate labels.

**Upstream-adoption finding:** the compared public source and history provide
**no evidence that upstream v0.4.1 adopted fork-original code or the fork's
world/species/pack architecture**. Shared ancestry explains the existing engine
and rendering similarities. That is an evidence statement about these revisions,
not proof about private development dates, conversations or inspiration. The
[upstream comparison](#comparison-with-upstream-v041) separates that conclusion
from upstream's own new work.

## What was inherited, rather than added by the fork

The ESP32-S3 application, four board layouts, hardware ports, SDL/LVGL host
simulator, quantized inference engine, fish behavior and art, genetics,
progression, append-only save discipline, decoration shelf, tank switcher,
audio mixer and sound bank, and networking/link/passport/gift machinery already
existed in v0.4.0. The fork adapts, partitions or disables those systems; it did
not originate all of them. In particular, moving aquarium code into
`common/worlds/aquarium/` and `common/bodies/fish/` is predominantly preservation
of upstream behavior under a new ownership boundary.

[LICENSE](../LICENSE) and [NOTICE](../NOTICE) remain inherited and unchanged:
PolyForm Noncommercial 1.0.0 and the upstream attribution still apply. The
fork's README adds its own introduction and operating instructions while keeping
the upstream README below it. Its retained upstream examples and release
language should not be read as a claim that every networked feature is enabled
in the terrarium.

## Terrarium physiology, terrain and keeper interaction

### Physiology and movement

The fork adds a species/habitat-driven lizard loop alongside the retained fish
world:

- **Species data:** the leopard gecko supplies its comfort band, thermal response
  per stage, drive and sleep rates, walking bouts/pauses/turn time, stage ages,
  shed timings, diet, population cap, geometry, five morphs and stage patterns.
  The fish roster also gains an explicit species pack rather than being deleted.
  The ordinary terrarium starts with leopard geckos in the desert; the warmer
  gecko/desert pair is test-only.
- **Heat:** body temperature follows local air/hide temperature, then maps through
  the species' heat band before it reaches the model. The desert has a warm-to-cool
  gradient, night cooling, height-dependent daytime warmth and a lamp pool on the
  high rock. Warm, humid and cool hides have their own temperatures. These are
  simulated physiology values, not measurements from real temperature/humidity
  sensors or control of a physical heat lamp.
- **Real walkable ground:** floor, three rock terraces and three switchback ramps
  are laid out from habitat coordinates on each board's glass. `terrain_route`
  uses a small Dijkstra graph with slope-dependent costs; the walk follows links
  between surfaces rather than teleporting vertically or swimming through rock.
  Validation checks reachability, slopes, headroom, hide clearance and the lamp's
  location on all four layouts.
- **Locomotion and goals:** stop-and-go walking, turns in place, slope-following
  bodies, exploration of stale zones, hiding/peeking, basking, digging, resting,
  investigation/standoff and hunting are separate from the aquarium's swim.
  A startled lizard runs to cover; an eligible lizard can climb toward a keeper's
  hand. The model chooses among `hunt`, `bask`, `hide`, `explore`, `investigate`,
  `dig`, `rest`; the reflex layer turns that choice into motion.
- **Sheds and growth:** normal → soon → shedding → clean/stuck is a tended-time
  upkeep loop. A sufficiently damp humid-hide stay clears a shed; a stuck shed
  raises stress and blocks stage advancement until cleared. Reaching the next
  stage's age starts or adopts a growth shed, and the stage changes only when
  it comes off. The mister moistens hides and can soak a stuck shed off; this
  also affects stress/trust. Sleep advances the appropriate tended clocks.

Sources: [species/gecko.c](../common/species/gecko.c),
[species/fish.c](../common/species/fish.c),
[habitat/desert.c](../common/habitat/desert.c),
[terrain.c](../common/terrain.c), [tank.c](../common/tank.c),
[walk.c](../common/worlds/terrarium/walk.c),
[shed.c](../common/worlds/terrarium/shed.c),
[species-pack.md](species-pack.md), [habitat-pack.md](habitat-pack.md),
[shed.md](shed.md).

### Food, touch and the two hand corrections

Crickets replace sinking pellets in the terrarium: they fall onto the first
surface below the drop, sit, hop, flee, crawl into cover, reappear and expire.
Hunts use reachable prey, last-seen prey memory, a stalk/freeze/lunge sequence and
miss probabilities affected by heat, stage and cricket state. A catch is one
meal. A keeper-filled mealworm dish is a second food source; automatic cricket
supply is held outside the active phase.

The keeper can drop crickets, fill the dish, hold a hand on the nearest ground,
tap a lizard's body for its card, startle it with repeated taps, wipe the glass
and use the mister. Two later fixes are behavior changes, not just refactoring:

1. [524cbea](https://github.com/dopamine911/pocket-terrarium/commit/524cbea)
   prevents stale aquarium decoration bits/positions from intercepting a
   terrarium hand hold and opening an invisible piece's placement page.
   Deliberate holds still edit unlocked desert decorations; locks, sand below
   a piece and a lizard in front leave the hold to the hand.
2. [5440f2e](https://github.com/dopamine911/pocket-terrarium/commit/5440f2e)
   credits each lizard that **actually walks under the hand's draw and reaches
   its own reachable slot on the correct surface**. It no longer applies the
   fish's 140/100-pixel ring test to lizards. Starting nearby can qualify;
   already resting there, moving the cursor onto an idle lizard, crossing for
   another reason or being visually near through a rock cannot. The tank's
   tally still counts the hold once, while each arriving lizard gets its badge.

Sources: [crickets.c](../common/worlds/terrarium/crickets.c),
[hunt.c](../common/worlds/terrarium/hunt.c),
[walk.c](../common/worlds/terrarium/walk.c),
[decor.c](../common/worlds/terrarium/decor.c),
[touch_port_ft3168.c](../firmware/main/touch_port_ft3168.c),
[sim/main.c](../sim/main.c), [selftest_walk.c](../sim/selftest_walk.c).

## Progression, setup, pages and art

The fork reuses the upstream ledger, care gates, birth flow, badges, item
numbering, placement pages, shelf and lock/sell machinery, with terrarium
content and several new rules:

| Capability | Implemented change |
|---|---|
| Long game | Gecko stage thresholds are 20 min / 90 min / 8 h of tended age, gated by a clean growth shed; the pack's normal population cap is three, under the inherited hard cap |
| Keeper pay | A keeper meal pays 1, rate-limited per lizard to 300 awake seconds; an uninterrupted clean shed pays 5, with stage pay on top for growth. Automatic food and clearing a shed that already stuck do not earn that shed/meal pay |
| Badges | First keeper meal, hand approach, cricket catch, bask, clean shed and dig; tank badges for a quiet night, stuck shed helped off, three clean sheds in a row, first adult and digging together, plus retained glass/personality/sale milestones. Fish-only notices are silent here |
| Arrivals | At the initial pair, every lizard's trust 6+, 12 keeper meals and four clean sheds earn a hatchling, subject to the pack cap. Higher-count tables also exist. There is no terrarium courtship/incubation simulation: the earned arrival appears from the humid hide after the staged delay and enters the birth flow |
| Upkeep shop | Sphagnum moss (40) halves hide drying, basking bulb (90) widens the warmth pool 1.5×, mealworm farm (140) fills the dish on purchase/mornings/qualifying sleep. Farm worms remain automatic until the keeper refills the dish |
| Decorations | Saguaro cactus (120), cow skull (160), driftwood (60), succulent pot (90). Cosmetic placement keeps clear of hides, dish, ramp feet and basking location; pieces can be moved, shelved, locked and sold back at the inherited 20% rate |
| First run and birth | Welcome names the loaded species; MORPH and NAME pages replace fish form/color choices, with no bubble-column page. Five gecko morphs use the inherited body-color gene/save path. **There is no species-selection menu yet:** the one shipped species is shown, not offered in a chooser |
| Words and page art | Lizard/hatchling/terrarium language, DESERT ROSES currency, NEW EGG, MY TERRARIUMS/MY DESERT, morph/family/card portraits, growth/egg glyphs, trait/food/tool/coin/badge/shop art and a desert loading look replace their fish-facing counterparts |

The lizard renderer adds 12 poses, five morphs, banded-to-spotted stage patterns,
shed appearances, blink/tongue/breath/walk/dig/strike/standoff animation and a
body-pose cache. The live desert scene draws terraces, hides and their occlusion
fronts, lamp/moon lighting, moisture, crickets, the dish, decorations and the
mister spray from engine state. Feet-based ordering keeps higher surfaces behind
lower ones. Separate light-state scene buffers and budgeted background baking
avoid rebaking an entire scene for every light switch; moisture changes patch
hide regions. Boot or a missing spare buffer can still require a whole bake:
this is not a universal frame-time guarantee.

The art pipeline adds 39 PNG inputs and `gen_terr_icons.py`; `gen_icons.py` now
emits shared pixels to `common/icons.c` and world-owned pixels to each world's
`icons.c`, instead of putting all world data into one generated unit.
Standalone art sheets and live-frame exporters/benchmarks are distinct tools:
the former exercise art variants; the latter stage and run the engine's renderer.

Sources: [progression.c](../common/progression.c),
[terrarium/fry.c](../common/worlds/terrarium/fry.c),
[terrarium/shop.c](../common/worlds/terrarium/shop.c),
[terrarium/milestones.c](../common/worlds/terrarium/milestones.c),
[terrarium/setup.c](../common/worlds/terrarium/setup.c),
[terrarium/view.c](../common/worlds/terrarium/view.c),
[bodies/lizard/](../common/bodies/lizard/),
[terrarium/scene.c](../common/worlds/terrarium/scene.c),
[scene_desert.c](../common/worlds/terrarium/scene_desert.c),
[gen_icons.py](../tools/gen_icons.py),
[gen_terr_icons.py](../tools/gen_terr_icons.py),
[the terrarium's long-game tables](progression.md).

## Model schema, training, evaluation and orchestration

### A real lizard model, not a renamed fish model

L1 adds normalized `heat`, `shed`, activity `phase`, and prey/other/warm/shelter
sightings to a species-agnostic state line. Its seven goals, four stage words
and closed **59-token vocabulary** are a distinct schema. The worst-case input
is 43 state words; BOS plus the line and `->` makes a **45-token prompt**, and
the answer still fits the inherited 64-position architecture.

The shared advisor core now builds a serializable L1 snapshot, encodes it,
detects L1 from the tokenizer's ` heat` token, maps the model's own seven
candidates into the combined fish/lizard goal enum, and exposes confidence and
runner-up probabilities by that enum. A bin refuses the other world's model.
Platform scheduling uses a world-appropriate food goal. The earlier temporary
fish-goal-to-lizard-goal bridge was removed at P6, not retained as a fallback.
The quantized transformer, tokenizer implementation and two-pass decision
approach are inherited, not new model architectures invented by this port.

The snapshot tracks `model/out/model_q4_lizard.bin` (**7,558,660 bytes**),
`model/out/tokenizer_lizard.bin` and the embedded firmware tokenizer copy.
`world.h` selects the model/tokenizer files; `version.h` supplies per-world tag,
length and SHA-256. The shipped terrarium tag is `lizard_v1`, the focused
repair's **seed 2**; the fish's `v3m` identity and files remain the aquarium's.

### Teacher and data pipeline

- Existing fish-schema generation stays available; `gen_traces.py` adds a
  Python `Terrarium`/`Lizard` simulator, L1 encoder and species-normalized teacher
  prompt, with a rule stand-in for dry runs. It samples drives, heat, sheds,
  sightings, food, keeper perturbations and multiple lizards.
- An OpenAI-compatible `/chat/completions` teacher path joins the existing
  Ollama path. It requests JSON-schema goal/urgency output, disables thinking,
  sends no tool definitions and reads `LAB_API_KEY` from the environment, passing
  the authorization header to curl through a pipe rather than argv or a file.
  The recorded L1 teacher is the lab's `chat-teacher` service, not an on-device
  API dependency.
- Tokenizer/fold/eval/prompt-check accept L1. Folding now validates L1 field
  order/ranges and answer shape as well as vocabulary and duplicates, and
  reports coverage of acceptance-relevant cases. Evaluation can ask either
  teacher API. `train.py` additionally reports peak GPU allocation; the actual
  transformer training/export foundation was already present upstream.
- `pilot.py` and its 37-state JSONL panel probe edge cases before a long run.
  `prompt_check.py` adds paired trait/stage/shed panels, shared scoring tables,
  case subsets and saved prompt/answer provenance. `probe_l1.py` applies those
  tables to checkpoint goal distributions at the device's sampling temperature,
  including a full boldness sweep; it is not a hardware-speed benchmark.
- `make_l1_parity.py`, a committed **420-line fixture**, and
  `sim/selftest_l1.c` cover C/Python encoder parity and live snapshots.
  `l1_focus.py` adds controlled, resumable A10/A12/A12x labeling. The two L1 shell
  pipelines automate post-trace folding/tokenizer/training/export/probe and
  focused labeling followed by the training harness.

**Recorded outcome, not verification performed for this document:** the
[retraining runbook](retrain-lizard.md) records the initial 44,968-pair model's
A10/A12 failures, a teacher “keep last” versus “solitary animals do not
investigate” rule conflict, 6,000 focused labels, 50,968 clean pairs, and the
shipped seed's A1 teacher agreement of 49/60 (82%). Its later sections record
passing synthetic panels and A13–A16 simulation censuses. It also records that
the Python training world still mirrors the **old floor-level desert**, while
the C simulation uses terraces. The terraced census passed, so the recorded
decision was not to regenerate the terrain mirror. Encoder parity does not
prove full simulator equivalence.

A16 is specifically the warmer test species **in a warmer test desert**. The
original test in the ordinary gecko desert failed because most of that habitat
could not offer its comfort band. The adjusted test retained the threshold;
it does not show that arbitrary packs can thrive in an unsuitable habitat.

### Declared multi-card training runs

`harness.py` and TOML model/dataset/host/run definitions add `plan`, `run`,
`status`, `report` and a harness selftest. The configured lab is marvin's 8060S
and zaphod's two R9700 cards, with local/remote CPU cards only when enabled.
The two ROCm Dockerfiles pin architecture-specific TheRock PyTorch wheels.

The harness folds/pins data by hash, synchronizes code/data, gives a container
only its assigned render node plus `/dev/kfd`, and schedules one job per card,
queuing later jobs. Training mode requires the lab's training profile;
opportunistic mode respects memory reserves and skips a loaded teacher's card.
It does not evict served models or switch the lab profile itself. Exit-code
files distinguish completion from a lost SSH connection; reruns adopt jobs,
collect results and retry a crashed job once. Per-job manifests and reports
record configuration, git/data/image identity, host/card, timing/memory/loss,
probe panels and seed selection.

Selection maximizes the smallest acceptance margin, ties within the measured
panel spread (with a floor), then uses validation loss. **Do not confuse that
selection with the checked-in weights:** the later harness baseline selects
seed 1337, but the runbook explicitly keeps the originally shipped seed 2.
The declared dataset needs its original trace files and the configured lab/SSH/
Docker environment; those are not all supplied by the small committed pilot
and parity fixtures.

Sources: [schema-lizard.md](../model/schema-lizard.md),
[advisor_core.c](../common/llm/advisor_core.c),
[gen_traces.py](../model/gen_traces.py),
[fold_traces.py](../model/fold_traces.py),
[train_tokenizer.py](../model/train_tokenizer.py),
[prompt_check.py](../model/prompt_check.py),
[probe_l1.py](../model/probe_l1.py),
[make_l1_parity.py](../model/make_l1_parity.py),
[l1_focus.py](../model/l1_focus.py), [harness.py](../model/harness.py),
[harness configuration](../model/harness/),
[training harness documentation](pipeline.md#training-harness),
[retrain-lizard.md](retrain-lizard.md).

## Packs, capabilities and persisted identity

Issue [#4](https://github.com/dopamine911/pocket-terrarium/issues/4) evolves from
compiled pack structs through a binary format and capability checking to the
implemented module split. These are separate changes, not one generic “packs”
feature:

| Layer | What changed at this snapshot |
|---|---|
| Authored data | `species_t` and `habitat_t` move species/climate/terrain/timing/look values out of hard-coded behavior. C const structs remain the source of built-ins |
| Binary format | PTPK fmt 1 has a 20-byte little-endian header, CRC-32 body, FNV-1a namespace UID, bounded TLV records and REQUIRED/optional handling; species, habitat and model-manifest kinds are supported |
| Loader and registries | One `packs_add` path validates built-ins and external files, stores inline data in fixed pools, rejects malformed records/values/conflicting identities, and quietly recognizes an already-loaded identical pack |
| Boot and emission | Firmware encodes its three built-ins (species, habitat, model manifest) from structs and loads those bytes through the same parser, first on the main task's stack. `ptpk_emit`/`make packs` produce all eight checked-in pack files for host/tools, rewriting only changed bytes |
| Authoring tools | `tools/ptpk.py build/dump/verify` handles TOML source and binary inspection, capability/goal records and exact binary32 conversion. This is not a second generator replacing the built-in C sources; device rule/pair checks still matter |
| Capabilities | Species needs derive from locomotion, areas/fittings and diet; checks require firmware support, habitat provision, the authoritative habitat UID list and goal words in the selected model's manifest. Unknown required needs are refused; unknown optional wants/offers can be dropped |
| World meaning | The earlier hard world gate is removed. `WORLDS` is an offer/menu filter; the build is a preset for modules, model and defaults. A file can be well formed yet an incompatible tank pair |
| Persistent packs | Saves append species/habitat UIDs and names, prefer UIDs on load and retain legacy IDs for built-ins. The runtime-assigned ID of an external file is not treated as durable identity |
| Missing/unmet pair | The tank pauses with PACK MISSING or PACK NEEDS, retains the saved pair and per-animal tail, and resumes when the pair can run. Defaults may stand in **for drawing**, not overwrite the desired saved identity. Notices and resume checks are implemented |
| Host library | The simulator scans `POCKET_PACKS_DIR` (default `../packs`), manifests/habitats before species, and rescans while a tank waits. This is a host folder loader, not an implemented device microSD library |

The simulator's `PT_ALL_PACKS=1` inventory contains both worlds' and test-only
packs, 12 spare species/habitat slots each and four spare manifest slots.
**Firmware has zero spare slots** after the RAM reduction: it carries one
non-test pack of each kind for its world. A new external pack therefore cannot
be added to that release configuration even if its semantics fit. The final
`release_check.c` expects a `no room` refusal for additional packs; older prose
in `ptpk.md` describing a foreign habitat/manifest loading in a release image
predates that final pool change.

The save remains **PTK2**, deliberately preserving the upstream layout and
fixtures rather than introducing the initially proposed new save magic. The
fork appends heat/shed/moisture/pack fields at 1956–2095, dish state at 2096,
clean-run/farm flags at 2097–2098, decoration positions at 2099–2102, and pack
UIDs/names at 2104–2159. The current struct is 2160 bytes. The existing layout
lock and newer-tail preservation remain, now in `save_layout.h`; the base
per-animal record is not expanded in place. Pack-tail identity prints also
track the intended animal across older-save slot reorderings.

Sources: [ptpk.md](ptpk.md), [common/packs/](../common/packs/),
[species.c](../common/species.c), [habitat.c](../common/habitat.c),
[compat.c](../common/packs/compat.c), [ptpk_emit.c](../tools/ptpk_emit.c),
[ptpk.py](../tools/ptpk.py), [packs_dir.c](../sim/packs_dir.c),
[save_layout.h](../common/save_layout.h),
[progression.c](../common/progression.c),
[release_check.c](../sim/release_check.c).

## Engine, world, body, setup, render and icon refactors

These ownership changes affect the aquarium as well as the new terrarium:

- **Core versus mechanics:** the large `tank.c` first splits by topic, then calls
  `loco_ops_t`, `food_ops_t` and `mech_ops_t` hooks. `tank_priv.h` holds the small
  shared engine-private interface. Aquarium swim/depth/pellets and the snail,
  shrimp, urchin, crab, seahorse and lane mechanics live under its world;
  terrarium walk/hunt/look/crickets/shed/decor live under its world.
  `common/locomotion/modules.c` lists the compiled modules and selects locomotion
  from species capabilities, food from diet, and world mechanics from served
  habitat areas. Public tank entry points dispatch through those tables.
- **Progression and first run:** `world_t`/`world_cur()` route shop effects,
  earning words, care gates, arrival behavior, milestone scans/names, restore
  hooks, rule advisor and default shelf names to `worlds/<world>/`. The aquarium
  boot show moves with that world; the terrarium uses its own immediate-ready
  implementation. `setup_world_t` supplies page definitions/skip flags/art/words;
  shared setup helpers are exposed as `setup_kit_*` rather than duplicating the
  whole first-run state machine. `save_t` and its lock move to `save_layout.h`.
- **Pixels versus scenes versus bodies:** `render_core.h` exposes the shared
  context, pixel/span/blend primitives, dirty/touched/box masks, scene-cache state,
  vignette and page drawing helpers. World scenes move out of `render.c`;
  `world_view_t` provides scene, tools, badge, shop, critter-card, shelf-thumb,
  loading-look and text/art choices. `body_ops_t` selects portraits, strips,
  unborn glyphs, morph/card words and stage terms by the species' body plan.
  The generic pages no longer choose world wording/art themselves.
- **Files and generated art:** fish drawing is in `bodies/fish/`; lizard drawing
  in `bodies/lizard/`; desert drawing in `worlds/terrarium/scene_desert.*`.
  The tiny `render_lizard.h` and `render_scene_desert.h` forwarding includes
  **still exist** at this snapshot. Icon definitions are split by owner while
  the public icon header/API is retained. The arrow box needed by both critter
  cards and the visitor pager stays in the shared render layer.
- **Build boundary:** Make/CMake discover the selected world's/body's module
  files by directory, with the ESP-IDF glob excluded from early script-mode
  requirements expansion. Firmware, simulator tests, exporters and command shell
  fence world-specific references so an image need not link the other world's
  implementation or keep its static state.

This is still **one world and one model per binary**, not a runtime plug-in
system or a mixed-species terrarium. `PT_ALL_PACKS` broadens the simulator's data
inventory/capability checks, not its linked movement/body/world implementations.
The build's goal set, advisor schema/model, milestone counts, `world_cur()` and
`world_view()` are still single-world choices. Pack files supply data for
implemented body/movement/goals, not executable new renderers or arbitrary AI.

Detailed contracts and diagrams are in [modules.md](modules.md),
[world-module.md](world-module.md) and the
[architecture overview](architecture-overview.md); the interfaces are
[locomotion.h](../common/locomotion.h), [mechanics.h](../common/mechanics.h),
[world_ops.h](../common/world_ops.h), [setup_world.h](../common/setup_world.h),
[world_view.h](../common/world_view.h), [body.h](../common/body.h) and
[render_core.h](../common/render_core.h).

### DRY helpers and the font are a separate earlier cleanup

PR [#2](https://github.com/dopamine911/pocket-terrarium/pull/2),
[fdb065c](https://github.com/dopamine911/pocket-terrarium/commit/fdb065c), adds
header-only `pt_util.h` for the copied clamps, lerp, RGB mixing/conversion,
little-endian reads/writes and cell hash. Callers in engine/render/save/shelf/
collection/gift/passport/link code use it instead of their copies. The crab's
matching hash uses the shared expression; equivalent render FNV helpers are
consolidated. This is intended expression-preserving deduplication, not a new
random field or wire format.

The printable ASCII 5×7 table formerly local to `update.c` becomes the shared
`render_font5x7[95][7]` in `render.c`; the uppercase UI draws from it, retaining
its special star/sparkle glyph separately. The update page's case-sensitive
network text uses the same table. This cleanup predates the later module moves
and also affects sources normally excluded by `PT_NET=0`.

Sources: [pt_util.h](../common/pt_util.h), [render.c](../common/render.c),
[render.h](../common/render.h), [update.c](../common/update.c), and the PR/commit.

## Network and storage identity

**The default fork images are offline in both worlds.** `PT_NET=0` removes
Wi-Fi update/provisioning, internet-clock and ESP-NOW/link sources from the
normal build, gates networked UI/commands, and omits visitors, gifts and
passports from normal operation. It is not merely a hidden settings button.
`NET=1` host binaries remain for the inherited protocol's regression suites;
that is not a supported release of networked gecko visits.

The fork keeps the original network code for future upstream integration.
Enabling it on hardware would still require a fork-owned release manifest/key
and link identity; the retained upstream OTA URL is not a terrarium update
service. Board marker relocation into `board_marker.c` makes a marker exist
even when the network source that formerly contained it is omitted.

| Identity | Shared base | Fork snapshot |
|---|---|---|
| Live save and neighboring firmware settings NVS namespace | `tank` | `terr`, including volume, brightness, orientation, sleep settings, battery/log/director keys |
| Shelf transaction journal namespace | `frame` | `terrframe` |
| Shelf slot / book magic | `PTS1` / `PTV1` | `TRS1` / `TRV1` |
| Firmware image marker | `PTBOARD` | `TRBOARD`; release/installer board inspection tools match it |
| Native simulator cache | `~/.cache/pocket-tank` | `~/.cache/pocket-terrarium` for save, storage/frame and optional network data |
| Tool build/QEMU cache defaults | `~/.cache/pocket-tank` | `~/.cache/pocket-terrarium` |

**Limits:** these identities separate the fork from pocket-tank, not the fork's
terrarium bin from its aquarium bin: both use `terr` and the native simulator's
same default cache. Docker adds a separate world/board directory. The firmware
flash partition layout and physical shelf storage are not made independent by
a new magic: using the fork's shelf can overwrite parked upstream tanks in the
shared storage area. Namespace isolation also is not protection against an NVS
partition erase/full flash. The save magic stays PTK2, board hardware IDs remain
the same, and the application project/output is still `pocket_tank.bin`.

Offline operation also changes time availability: no internet resynchronization
is available after power loss. In the default firmware, automatic PMIC night
power-off depends on an RTC; otherwise deep sleep preserves the running clock.
This is not a new RTC or a guarantee of knowing elapsed time through total power
loss on a clockless board.

Sources: [version.h](../common/version.h),
[progression.h](../common/progression.h), [shelf.c](../common/shelf.c),
[shelf.h](../common/shelf.h),
[persist_port_esp.c](../firmware/components/tankcore/persist_port_esp.c),
[main.c](../firmware/main/main.c), [director.c](../firmware/main/director.c),
[persist_port_sim.c](../sim/persist_port_sim.c),
[net_port_sim.c](../sim/net_port_sim.c), [PLAN P0](../PLAN.md).

## ESP-IDF compatibility, actual board fixes and memory

| Change | Evidence and boundary |
|---|---|
| ESP-IDF 5.5 send-callback compatibility | `LP_SENT_TO` selects `esp_now_send_info_t *` for IDF ≥5.5 and the peer MAC pointer for older IDF in both ESP-NOW send callbacks. This preserves the otherwise disabled network path's build compatibility; it is not the cause of the pendant's dark display |
| 1.75C rev1.1 detection/suspend fix | The ES7210 probe accepts address straps **0x40–0x43**, records the actual responding address, reports the bus if no board signature matches, and passes that address to microphone-ADC suspend. The tested pendant answered at **0x42**; the old fixed 0x40 probe failed its board signature and left its panel dark |
| Correct first model batch | `q4_model_reserve_batch` and the L1 initialization reserve **45 tokens before discretionary boot allocations**, rather than allocate 44 then grow to 63. The model allocator cannot free the first set on growth. The reservation is attempted at load; allocation failure still follows the existing inference allocation path |
| Renderer/world allocation separation | Terrarium PSRAM goes to its second light-state scene and lizard-body scratch, instead of the aquarium grass-loop/decor/depth caches. Unused world and network sources/state are omitted/fenced; the terrarium milestone watcher is world-owned rather than aquarium `.bss` |
| Pack pool RAM | `eb75fb5` changes firmware spare species/habitat slots 2→0 and spare manifests 1→0 until the card library exists, removing roughly **2.2 KB of `.bss`**. Host validation retains its larger pools; this is not an SD loading feature |
| Boot stack discipline | `packs_init()` runs first in `app_main`, so the bounded built-in pack encoding buffer uses the main task's stack, not an accidentally first-calling smaller task |

The [round-board notes](board-amoled-1.75c.md) and [PLAN P7](../PLAN.md)
record a flashed rev1.1 pendant with working panel/touch/setup and `lizard_v1`:
about **25 fps**, **4–6 ms rendering**, **4.0 s decisions at 11.9 tok/s**, and
**58 KB free internal heap**, with the card cache in PSRAM after the batch fix.
The earlier 1 KB/6.1 s observation is the recorded failure motivating the fix,
not the claimed final result. These are observations of that board/build, not
new measurements for this document or proof of equivalent behavior on every
board. Frame cost on the 1.8 and a completed terrarium QEMU boot remain open
in the recorded plan; the earlier QEMU attempt only reached the absent panel's
I2C path. The existing QEMU overlay/signature limitations are not repaired by
making the runner choose the right model.

Sources: [link_port_espnow.h](../firmware/main/link_port_espnow.h),
[link_spike.c](../firmware/main/link_spike.c),
[display_port_sh8601.c](../firmware/main/display_port_sh8601.c),
[codec_port.c](../firmware/main/codec_port.c),
[q4_model.c](../common/llm/q4_model.c),
[advisor_core.c](../common/llm/advisor_core.c),
[ptpk.h](../common/packs/ptpk.h),
[firmware/main/main.c](../firmware/main/main.c).

## Docker, builds, flashing, simulator, CI and documentation

### Build and distribution plumbing

- The simulator adds `WORLD=terrarium|aquarium`, separately named object/bin
  directories, `NET=0|1`, per-world test lists and a pack-emitter target.
  Firmware adds the Kconfig world choice and `sdkconfig.aquarium`; Make/CMake
  compile selected module directories. The CMake `CONFIGURE_DEPENDS` glob is
  guarded out of ESP-IDF's early script-mode requirements scan; only the main
  component registers the version header's configure dependency.
- Model filenames/identities become world-aware throughout flash scripts,
  tokenizer embedding, QEMU, installer generation and OTA-manifest generation.
  `model_trailer.py` reads the generated build configuration (or explicit world),
  prints/checks the selected model and emits its trailer. Installer assembly
  refuses mixed-world board builds on one installer page.
- Round/watch/square scripts add the correct model **and trailer** on model/full
  flashes. `flash_round.sh` explicitly supports `WORLD=aquarium` with a distinct
  build directory/overlay; switching worlds requires the matching model, not
  an app-only flash. The other scripts consume the world of their configured
  build, not an equivalent new world-selector interface.
- The project/output naming and underlying partition arrangement are retained.
  Making release helpers world-aware does **not** supply a fork-owned OTA
  service, rewrite the inherited release/installer workflows or enable offline
  images to provision over Wi-Fi.

### Desktop simulator and visual tools

The new multi-stage Debian Docker image builds SDL and pinned LVGL v9.2.2,
includes both worlds' existing model assets, sounds and packs, and needs no host
compiler/SDL development installation. `.dockerignore` restricts the build
context to required inputs, including the pack emitter added after pack support
landed. `tools/sim_docker.sh` selects world/board, builds a cached image, and
mounts persistent saves at
`~/.cache/pocket-terrarium/docker/<world>/<board>` (or `PT_SIM_DATA`). It runs
with the caller's UID/GID, drops capabilities and uses existing X11/XWayland
cookies instead of privileged mode or `xhost +`. PulseAudio/PipeWire-Pulse is
optional; absent audio sockets give a silent run. Headless mode uses SDL dummy
drivers. This is a native Linux desktop path, not a web simulator or a promise
of native Wayland/macOS/Windows container display support.

The SDL application adds whole-pixel `--zoom` (1–8, clamped to the screen) and
`--fullscreen`, including mouse-to-glass coordinate conversion and ignoring
letterbox clicks. `--lizard-sheet`, `--terr-shot`, `--terr-bench`, UI/decoration
shots and census paths cover art variants, real rendered frames and behavior.
Estimated “board” timings in the host art/bench tools remain estimates; they
must not be confused with the pendant measurements above.

### Regression/tooling coverage

New C suites cover packs/heat/shed/mister, walking/goals/prey/gestures/pay/
progression/setup/decor/live rendering, encoder parity, PTPK malformed-input and
round-trip cases, and capability/model-manifest/paused-pair behavior.
`release_check.c` separately compiles the common pack sources as
`PT_ALL_PACKS=0` for each world's production-style pack inventory. That is a
host check of those semantics, not an ESP-IDF firmware build or hardware run.

`sim_tmp.c/.h` replaces shared fixed `/tmp` selftest paths with run-local scratch
paths and owner-process cleanup; save/link/collection and screenshot harnesses
use those paths. Existing tests/staging are adapted to pack-specific stage ages
and the appropriate world's scene. The standalone host decision utility knows
both goal sets and documents world selection. Inherited audio/Q4 host tests
remain rather than being new tests attributed to the fork.

`make check-all` expands to both worlds on four boards plus their separate
network test binaries and release-pack checks. The existing Linux Docker check
wrapper gains both world lists. CI changes from one serial simulator job to
**eight board×world jobs**, each running `check` and `check-net`, with a separate
release-pack job and retained audio/Q4/Python host checks; `fail-fast: false`,
timeouts and stale-run cancellation are configured. These are configured check
paths, **not a claim that a suite or CI run was executed for this documentation**.

Documentation adds the port plan, L1 schema/runbook, species/habitat/shed and
PTPK contracts, module/world ownership guides, terrarium progression details,
board measurements, training-harness instructions and Docker/window usage.
The existing render-stack, memory, hardware and upstream gameplay documents
remain useful background, but are not all rewritten to describe every fork
path. Ignore rules add world/network build outputs, codegraph data and training
virtualenvs, while explicitly retaining the shipped lizard model/tokenizers.

Sources: [sim/Makefile](../sim/Makefile),
[tankcore/CMakeLists.txt](../firmware/components/tankcore/CMakeLists.txt),
[main/CMakeLists.txt](../firmware/main/CMakeLists.txt),
[model_trailer.py](../tools/model_trailer.py),
[flash_round.sh](../tools/flash_round.sh),
[make_installer.py](../tools/make_installer.py),
[make_ota_manifest.py](../tools/make_ota_manifest.py),
[sim/Dockerfile](../sim/Dockerfile), [sim_docker.sh](../tools/sim_docker.sh),
[sim_tmp.c](../sim/sim_tmp.c), [checks.yml](../.github/workflows/checks.yml),
[README Docker instructions](../README.md#run-the-simulator-in-docker-linux-desktop).

## Implemented versus reserved/deferred scope

| Present at the snapshot | Not supplied by the snapshot |
|---|---|
| One gecko species, desert habitat, fish/freshwater preservation, test-only warmer pair | Bearded dragon/monitor/chameleon/crested-gecko packs, arboreal/clinging/projectile-tongue behavior or a trained `climb` goal |
| Pack-driven daytime active/settle/sleep phase | Dusk/night schedules: named in the schema, but `species_check` explicitly refuses schedules other than `SCHED_DAY`; “a later schedule needs no retrain” is design intent, not current support |
| Walking ramps/rocks, hunts/dish/sheds/mister, cosmetic desert pieces | The early plan's latrine/spot-cleaning loop, isopod cleanup crew, greens feeding or a separate living potted-plant husbandry system |
| Earned hatchling and inherited birth/setup/genetics machinery | Eggs as a full laying/incubation/courtship simulation; the NEW EGG artwork/words alone do not implement one |
| Binary pack parser, authoring CLI and host folder scan | Device SD-card library/catalog/selection/download flow; firmware has no spare pack slots yet |
| Capability and goal-manifest checks, one selected world/model | Runtime world/model switching in one image, mixed species per animal, slither/turtle mechanics, a general executable plugin ABI; model-manifest INPUTS remains reserved |
| Documented model acceptance and one flashed/measured pendant | A new hardware acceptance run on every board, resolved 1.8 frame budget or completed terrarium QEMU boot |
| Offline build and protocol regression bins | Enabled production Wi-Fi/OTA/Improv/visits/gifts/passports/visitor collection |

These boundaries come from the current species checks, module lists, world/
model selection, pool definitions and setup code, not merely from the original
roadmap. P2d feeding/movement tuning remains identified in the plan; the runbook
calls cricket-rate A15 its narrowest census margin. This document records that
limit without proposing a new tuning or upstream-fix project.

## Comparison with upstream v0.4.1

Upstream's public `bb49048..952ef4d` range is **one release commit**, touching
107 files. It is not the history of our 76-commit fork range. Its major net
changes are different work:

| Upstream-only release work | How it relates to this fork snapshot |
|---|---|
| Seven purchasable backgrounds plus the original free background, palette/haze integration, shop tabs and thumbnails | Not our desert habitat/terrain/body/pack architecture. Upstream retains the large renderer/engine ownership structure |
| IMA ADPCM sound-bank support and audio paths/tests; bank shrinks from 579,858 to 174,556 bytes | The fork's audio changes are chiefly storage namespace use; it retains the inherited sound bank/mixer format |
| Revised touch/page action gating and new host touch harness/tests | Not the fork's lizard hand-target/routing corrections; our old gates do not thereby contain all v0.4.1 input fixes |
| Panel kept dark until the first completed flush, including wake | Different from our ES7210 address-range board-detection fix; upstream still uses the fixed 0x40 ES7210 probe |
| More power-cut-safe save/reset/shelf transaction handling and markers | Independent persistence work on top of the shared save discipline; not the fork's namespace/PTPK UID/missing-pack changes |
| Two visitor places, fry PEEK, gift resume/departure-passport handling, `roster_shift` versus `roster_gen` | Networked aquarium behavior, absent from the default offline fork feature set; the fork retains the earlier protocol base for regression builds |
| Quick clock/time-check logic and Hatchery crew checks | Upstream boot/network/tooling work, not a lizard-model/schema/training contribution |

Pinned upstream sources: [release diff](https://github.com/mediacutlet/pocket-tank/commit/952ef4d4eadeaa1ef455292e1516376cd8365eb9),
[backgrounds](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/common/backgrounds.h),
[audio](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/common/audio.c),
[touch gates](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/common/touch_gate.h),
[display port](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/firmware/main/display_port_sh8601.c),
[save/progression](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/common/progression.c),
[tank link](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/common/tank_link.c),
[clock gate](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/common/clock_gate.h),
[preflight](https://github.com/mediacutlet/pocket-tank/blob/952ef4d4eadeaa1ef455292e1516376cd8365eb9/tools/preflight.py).

### Why the finding is “no evidence of adoption”

The source/history comparison found no upstream counterpart to the fork's L1
model or training harness, species/habitat/PTPK/capability infrastructure, world/
body module split, shared `pt_util.h` cleanup or shared-font cutover. Upstream
v0.4.1 has no `common/llm` delta; it still keeps private math helpers and separate
ASCII/uppercase 5×7 tables. Similar scene/noise/mix techniques can be traced to
older shared upstream code rather than requiring a copy from this fork.

Patch-equivalence comparison marks every fork non-merge commit as unique
(`git cherry upstream/main fe3ec76` produced only `+`). A supplemental exact-line
screen of added source in `common/`, `firmware/`, `sim/` and `tools/`, excluding
baseline lines and numeric asset data, found no novel shared added lines longer
than 45 stripped characters. Those are useful negative findings, not proof
against a rewritten/manual adaptation or conceptual inspiration. Public release
squashing also does not expose upstream's private implementation chronology.
No stronger attribution or plagiarism claim follows from these observations.

## Practical port and merge implications

This comparison is not a request to fix upstream or a proposal to merge it now.
It does show where a later port would need care:

- **Follow ownership, not old filenames.** An upstream hunk in `tank.c`,
  `render.c`, progression or setup can now belong in an aquarium module, a body
  implementation, a world descriptor or the shared core. Generated icon changes
  belong in input assets/generators and their correct world outputs. Applying a
  monolithic patch directly is not equivalent to integrating its behavior.
- **Saves have a shared prefix, not one interchangeable identity.** Preserve
  frozen offsets/newer tails, the fork's append-only fields, UID/resume behavior
  and the offline storage namespaces when considering upstream transaction work.
  Fork shelf markers do not create a second physical storage partition.
- **Model/application/world form a unit.** The tokenizer, goal enum/schema,
  manifest, tag/hash/trailer and selected world must agree. An app-only world
  switch does not replace the model partition. Capability acceptance is not a
  substitute for linked mechanics or a renderer for a new body plan.
- **Keep board fixes distinct.** The IDF callback signature, ES7210 address
  detection, upstream first-frame blanking and upstream input/clock fixes solve
  separate problems in overlapping files. One does not supersede the others.
- **A nonnetworked image and network regression binary differ.** Importing
  upstream link/UI/clock changes must respect that compile boundary; their
  existence in source is not permission to reactivate upstream OTA or protocol
  identity for the fork.
- **Audio is a format/API integration, not only a smaller asset.** Upstream's
  `audio_init` bank units/codec support and host checks change with the ADPCM
  bank. Replacing only the sound binary would not port that feature.
- **Conflicts are concrete, not hypothetical.** A read-only merge-tree rehearsal
  of these pinned tips reported **20 conflicting paths**, including the old
  monolithic render/tank/progression/setup/icon areas, main integration and
  build lists. That is an integration warning, not a claim that a merge,
  conflict resolution or behavior verification has been completed.

## Commit and PR landmarks

Short hashes link to the fork's actual history; the table groups related
changes rather than assigning a new feature to every integration merge.

| Area | Traceable commits / review reference |
|---|---|
| Fork ancestry and first compatibility work | [e2edbf3](https://github.com/dopamine911/pocket-terrarium/commit/e2edbf3) merges upstream v0.4.0; [acd7222](https://github.com/dopamine911/pocket-terrarium/commit/acd7222) adds the port plan and IDF callback compatibility |
| Offline/storage identity | [df33a37](https://github.com/dopamine911/pocket-terrarium/commit/df33a37) |
| Species/schema/shed design and state | [90f0277](https://github.com/dopamine911/pocket-terrarium/commit/90f0277), [0e5c355](https://github.com/dopamine911/pocket-terrarium/commit/0e5c355), [aa52367](https://github.com/dopamine911/pocket-terrarium/commit/aa52367) |
| L1 teacher, parity, prompt gate and frozen traces | [ed39d24](https://github.com/dopamine911/pocket-terrarium/commit/ed39d24), [5e2f7d8](https://github.com/dopamine911/pocket-terrarium/commit/5e2f7d8), [1deb316](https://github.com/dopamine911/pocket-terrarium/commit/1deb316), [6d63be7](https://github.com/dopamine911/pocket-terrarium/commit/6d63be7) |
| World builds, walking, lizard/desert art | [16db118](https://github.com/dopamine911/pocket-terrarium/commit/16db118) |
| DRY/font cleanup | [fdb065c](https://github.com/dopamine911/pocket-terrarium/commit/fdb065c), merged as [PR #2](https://github.com/dopamine911/pocket-terrarium/pull/2) / `a6a5641` |
| Terrain/terraces | [fd88222](https://github.com/dopamine911/pocket-terrarium/commit/fd88222) |
| Student probe and post-trace automation | [5957ee4](https://github.com/dopamine911/pocket-terrarium/commit/5957ee4), [4d68af2](https://github.com/dopamine911/pocket-terrarium/commit/4d68af2) |
| Focused model repair | [136e789](https://github.com/dopamine911/pocket-terrarium/commit/136e789) |
| Crickets/dish/hands and live scene | [d222c80](https://github.com/dopamine911/pocket-terrarium/commit/d222c80) |
| Model census and shipping cutover | [8fe0c5e](https://github.com/dopamine911/pocket-terrarium/commit/8fe0c5e), [72c3d8d](https://github.com/dopamine911/pocket-terrarium/commit/72c3d8d) |
| Actual pendant fix and batch reservation | [c09f11c](https://github.com/dopamine911/pocket-terrarium/commit/c09f11c), [7cdec73](https://github.com/dopamine911/pocket-terrarium/commit/7cdec73) |
| Multi-card training harness | [771430b](https://github.com/dopamine911/pocket-terrarium/commit/771430b), [PR #5](https://github.com/dopamine911/pocket-terrarium/pull/5) / `5f9c46f` |
| Progression/UI/art/decor | [9d53324](https://github.com/dopamine911/pocket-terrarium/commit/9d53324), [cdbb4e8](https://github.com/dopamine911/pocket-terrarium/commit/cdbb4e8), [15a22d6](https://github.com/dopamine911/pocket-terrarium/commit/15a22d6) |
| Docker desktop/window support | [0f897dd](https://github.com/dopamine911/pocket-terrarium/commit/0f897dd), [50b1055](https://github.com/dopamine911/pocket-terrarium/commit/50b1055), [3c65936](https://github.com/dopamine911/pocket-terrarium/commit/3c65936) |
| Hand-route and earned-arrival corrections | [524cbea](https://github.com/dopamine911/pocket-terrarium/commit/524cbea), [5440f2e](https://github.com/dopamine911/pocket-terrarium/commit/5440f2e) |
| File packs and capability matching | [5c6c498](https://github.com/dopamine911/pocket-terrarium/commit/5c6c498), [bb0dbab](https://github.com/dopamine911/pocket-terrarium/commit/bb0dbab), [issue #4](https://github.com/dopamine911/pocket-terrarium/issues/4) phases 1–2 |
| Module discovery, generated icon ownership, CMake early scan | [fa19217](https://github.com/dopamine911/pocket-terrarium/commit/fa19217), [b4b5ad7](https://github.com/dopamine911/pocket-terrarium/commit/b4b5ad7), [c78f6be](https://github.com/dopamine911/pocket-terrarium/commit/c78f6be) and its integration copies/follow-through |
| Engine mechanics split and dispatch | [91e5936](https://github.com/dopamine911/pocket-terrarium/commit/91e5936), [948553f](https://github.com/dopamine911/pocket-terrarium/commit/948553f), [307dc0e](https://github.com/dopamine911/pocket-terrarium/commit/307dc0e) |
| Render/core/world/body/text extraction | [42614a1](https://github.com/dopamine911/pocket-terrarium/commit/42614a1), [cb34980](https://github.com/dopamine911/pocket-terrarium/commit/cb34980), [ba9226e](https://github.com/dopamine911/pocket-terrarium/commit/ba9226e), [31d84e7](https://github.com/dopamine911/pocket-terrarium/commit/31d84e7), [a1068d0](https://github.com/dopamine911/pocket-terrarium/commit/a1068d0), [3a1813f](https://github.com/dopamine911/pocket-terrarium/commit/3a1813f) |
| World/progression/setup/save ownership | [537b0b3](https://github.com/dopamine911/pocket-terrarium/commit/537b0b3), [75e8672](https://github.com/dopamine911/pocket-terrarium/commit/75e8672), [d00fb4c](https://github.com/dopamine911/pocket-terrarium/commit/d00fb4c), [05d5008](https://github.com/dopamine911/pocket-terrarium/commit/05d5008) |
| Integration fences and module docs | `732204f`, `178c1cd`, `60b9f6c`, `4219932` merge the module slices; [feddaf1](https://github.com/dopamine911/pocket-terrarium/commit/feddaf1), [e72bdd3](https://github.com/dopamine911/pocket-terrarium/commit/e72bdd3), [3d676e8](https://github.com/dopamine911/pocket-terrarium/commit/3d676e8), [a68735a](https://github.com/dopamine911/pocket-terrarium/commit/a68735a) finish fences/docs |
| Aquarium round flash, pool RAM and CI | [650e699](https://github.com/dopamine911/pocket-terrarium/commit/650e699), [eb75fb5](https://github.com/dopamine911/pocket-terrarium/commit/eb75fb5), [84c1f23](https://github.com/dopamine911/pocket-terrarium/commit/84c1f23); integrated by `32ff432` and [PR #12](https://github.com/dopamine911/pocket-terrarium/pull/12) / `fe3ec76` |

## Complete changed-file inventory

This is the exact net name/status inventory of the implementation snapshots,
categorized once per destination. `A` = added, `M` = modified, `R097` = Git's
97%-similarity rename. No tracked deletion remains in the net diff. Intermediate
files introduced then moved/deleted inside the fork range do not appear as
separate net changes; the history above includes those transitions. Paths below
are repository-relative, and the rename records both source and destination.

To reproduce the scope without restricting history to first-parent merges:

```bash
git log --reverse --date=short --format='%h %ad %s' bb49048..fe3ec76
git diff --name-status -M bb49048 fe3ec76
git diff bb49048 fe3ec76 -- <path>
# Separate question: upstream's own release work after the shared base
git log --oneline bb49048..952ef4d
git diff bb49048 952ef4d -- <path>
```

The inventory includes generated C/icon data, binary model/tokenizer/pack assets,
all modified platform/test/tool files and documentation, not only hand-written
modules. It deliberately does not count inherited files unchanged by the fork.

| Category | Entries | A | M | R |
|---|---:|---:|---:|---:|
| Repository metadata and documentation | 16 | 9 | 7 | 0 |
| Terrarium icon source assets | 39 | 39 | 0 | 0 |
| Shared engine, UI, utility and save contracts | 38 | 16 | 22 | 0 |
| Species, habitats, pack registries and pack files | 30 | 30 | 0 | 0 |
| World and body implementations | 50 | 49 | 0 | 1 |
| Model runtime, training and orchestration | 29 | 19 | 10 | 0 |
| Firmware integration and board ports | 26 | 3 | 23 | 0 |
| Simulator, fixtures and test harnesses | 20 | 13 | 7 | 0 |
| Build, flash, authoring and desktop tools | 14 | 4 | 10 | 0 |
| **Total** | **262** | **182** | **79** | **1** |

### Repository metadata and documentation

```text
A  .dockerignore
M  .github/workflows/checks.yml
M  .gitignore
A  PLAN.md
M  README.md
M  docs/BOARDS.md
M  docs/board-amoled-1.75c.md
A  docs/habitat-pack.md
A  docs/modules.md
M  docs/pipeline.md
M  docs/progression.md
A  docs/ptpk.md
A  docs/retrain-lizard.md
A  docs/shed.md
A  docs/species-pack.md
A  docs/world-module.md
```

### Terrarium icon source assets

```text
A  assets/icons/deco_driftwood_terr.png
A  assets/icons/deco_pot_terr.png
A  assets/icons/deco_saguaro_terr.png
A  assets/icons/deco_skull_terr.png
A  assets/icons/foot_my_tanks_terr.png
A  assets/icons/foot_sand_dollar_terr.png
A  assets/icons/icon_bold_terr.png
A  assets/icons/icon_cautious_terr.png
A  assets/icons/icon_curious_terr.png
A  assets/icons/icon_hunger_terr.png
A  assets/icons/icon_shy_terr.png
A  assets/icons/icon_social_terr.png
A  assets/icons/icon_solo_terr.png
A  assets/icons/ms_clean_run_terr.png
A  assets/icons/ms_first_adult_terr.png
A  assets/icons/ms_first_bubbles_terr.png
A  assets/icons/ms_first_dart_terr.png
A  assets/icons/ms_first_feeding_terr.png
A  assets/icons/ms_first_follow_terr.png
A  assets/icons/ms_first_glass_cleaning_terr.png
A  assets/icons/ms_first_hold_approach_terr.png
A  assets/icons/ms_first_meal_terr.png
A  assets/icons/ms_first_play_session_terr.png
A  assets/icons/ms_first_quiet_night_terr.png
A  assets/icons/ms_first_sale_terr.png
A  assets/icons/ms_first_seagrass_terr.png
A  assets/icons/ms_first_shed_terr.png
A  assets/icons/ms_shed_helped_terr.png
A  assets/icons/ms_tank_changed_someone_terr.png
A  assets/icons/shop_bulb_terr.png
A  assets/icons/shop_driftwood_terr.png
A  assets/icons/shop_farm_terr.png
A  assets/icons/shop_moss_terr.png
A  assets/icons/shop_pot_terr.png
A  assets/icons/shop_saguaro_terr.png
A  assets/icons/shop_sand_dollar_16_terr.png
A  assets/icons/shop_sand_dollar_64_terr.png
A  assets/icons/shop_skull_terr.png
A  assets/icons/tool_mister.png
```

### Shared engine, UI, utility and save contracts

```text
M  common/advisor.c
M  common/advisor.h
A  common/body.h
M  common/collection.c
M  common/gift.c
M  common/icons.c
M  common/icons.h
A  common/locomotion.h
A  common/locomotion/modules.c
A  common/mechanics.h
M  common/notice.c
M  common/notice.h
M  common/passport.c
M  common/progression.c
M  common/progression.h
A  common/pt_util.h
M  common/render.c
M  common/render.h
A  common/render_core.h
A  common/render_lizard.h
A  common/render_scene_desert.h
A  common/save_layout.h
M  common/setup.c
M  common/setup.h
A  common/setup_world.h
M  common/shelf.c
M  common/shelf.h
M  common/tank.c
M  common/tank.h
M  common/tank_link.c
A  common/tank_priv.h
A  common/terrain.c
A  common/terrain.h
M  common/update.c
M  common/version.h
A  common/world.h
A  common/world_ops.h
A  common/world_view.h
```

### Species, habitats, pack registries and pack files

```text
A  common/habitat.c
A  common/habitat.h
A  common/habitat/desert.c
A  common/habitat/freshwater.c
A  common/packs/builtin.c
A  common/packs/builtin.h
A  common/packs/caps.c
A  common/packs/caps.h
A  common/packs/compat.c
A  common/packs/compat.h
A  common/packs/manifest.c
A  common/packs/manifest.h
A  common/packs/models.c
A  common/packs/ptpk.c
A  common/packs/ptpk.h
A  common/packs/ptpk_tags.h
A  common/packs/ptpk_write.c
A  common/packs/ptpk_write.h
A  common/species.c
A  common/species.h
A  common/species/fish.c
A  common/species/gecko.c
A  packs/pt.fish.roster.ptpk
A  packs/pt.gecko.leopard.ptpk
A  packs/pt.habitat.desert.ptpk
A  packs/pt.habitat.freshwater.ptpk
A  packs/pt.model.fish_v3m.ptpk
A  packs/pt.model.lizard_v1.ptpk
A  packs/pt.test.desert-warm.ptpk
A  packs/pt.test.gecko-warm.ptpk
```

### World and body implementations

```text
A  common/bodies/fish/fish.c
A  common/bodies/fish/fish.h
A  common/bodies/lizard/body.c
A  common/bodies/lizard/lizard.c
A  common/bodies/lizard/lizard.h
A  common/worlds/aquarium/aquarium.c
A  common/worlds/aquarium/aquarium_priv.h
R097  common/boot_show.c -> common/worlds/aquarium/boot_show.c
A  common/worlds/aquarium/critters/crab.c
A  common/worlds/aquarium/critters/lanes.c
A  common/worlds/aquarium/critters/seahorse.c
A  common/worlds/aquarium/critters/shrimp.c
A  common/worlds/aquarium/critters/snail.c
A  common/worlds/aquarium/critters/urchin.c
A  common/worlds/aquarium/depth.c
A  common/worlds/aquarium/fry.c
A  common/worlds/aquarium/icons.c
A  common/worlds/aquarium/milestones.c
A  common/worlds/aquarium/pellets.c
A  common/worlds/aquarium/restore.c
A  common/worlds/aquarium/rules.c
A  common/worlds/aquarium/scene.c
A  common/worlds/aquarium/scene.h
A  common/worlds/aquarium/setup.c
A  common/worlds/aquarium/shop.c
A  common/worlds/aquarium/swim.c
A  common/worlds/aquarium/view.c
A  common/worlds/aquarium/world.c
A  common/worlds/terrarium/boot_show.c
A  common/worlds/terrarium/crickets.c
A  common/worlds/terrarium/decor.c
A  common/worlds/terrarium/fry.c
A  common/worlds/terrarium/hunt.c
A  common/worlds/terrarium/icons.c
A  common/worlds/terrarium/look.c
A  common/worlds/terrarium/milestones.c
A  common/worlds/terrarium/restore.c
A  common/worlds/terrarium/rules.c
A  common/worlds/terrarium/scene.c
A  common/worlds/terrarium/scene.h
A  common/worlds/terrarium/scene_desert.c
A  common/worlds/terrarium/scene_desert.h
A  common/worlds/terrarium/setup.c
A  common/worlds/terrarium/shed.c
A  common/worlds/terrarium/shop.c
A  common/worlds/terrarium/terrarium.c
A  common/worlds/terrarium/terrarium_priv.h
A  common/worlds/terrarium/view.c
A  common/worlds/terrarium/walk.c
A  common/worlds/terrarium/world.c
```

### Model runtime, training and orchestration

```text
M  common/llm/advisor_core.c
M  common/llm/advisor_core.h
M  common/llm/q4_model.c
M  common/llm/q4_model.h
A  model/docker/Dockerfile.rocm-gfx1151
A  model/docker/Dockerfile.rocm-gfx120X
M  model/eval.py
M  model/fold_traces.py
M  model/gen_traces.py
A  model/harness.py
A  model/harness/datasets.toml
A  model/harness/hosts.toml
A  model/harness/models.toml
A  model/harness/runs/l1-focus2.toml
A  model/harness/runs/smoke.toml
A  model/l1_focus.py
A  model/make_l1_parity.py
A  model/out/model_q4_lizard.bin
A  model/out/tokenizer_lizard.bin
A  model/pilot.py
A  model/pilot_l1.jsonl
A  model/probe_l1.py
M  model/prompt_check.py
A  model/run_l1_after_traces.sh
A  model/run_l1_focus_retrain.sh
A  model/schema-lizard.md
A  model/testdata/l1_parity.jsonl
M  model/train.py
M  model/train_tokenizer.py
```

### Firmware integration and board ports

```text
M  firmware/components/tankcore/CMakeLists.txt
M  firmware/components/tankcore/persist_port_esp.c
M  firmware/host_test/decide_host.c
M  firmware/main/CMakeLists.txt
M  firmware/main/Kconfig.projbuild
M  firmware/main/advisor_llm_esp.c
M  firmware/main/audio_port_es8311.c
M  firmware/main/batlog.c
A  firmware/main/board_marker.c
M  firmware/main/board_pins.h
M  firmware/main/brightness.c
M  firmware/main/codec_port.c
M  firmware/main/codec_port.h
M  firmware/main/director.c
M  firmware/main/display_port.h
M  firmware/main/display_port_sh8601.c
M  firmware/main/link_dev.c
M  firmware/main/link_port_espnow.c
M  firmware/main/link_port_espnow.h
M  firmware/main/link_spike.c
M  firmware/main/main.c
M  firmware/main/net_port_esp.c
A  firmware/main/tokenizer_lizard.bin
M  firmware/main/touch_port_ft3168.c
M  firmware/run_qemu.sh
A  firmware/sdkconfig.aquarium
```

### Simulator, fixtures and test harnesses

```text
A  sim/Dockerfile
M  sim/Makefile
M  sim/advisor_llm.c
M  sim/main.c
M  sim/net_port_sim.c
A  sim/packs_dir.c
A  sim/packs_dir.h
M  sim/persist_port_sim.c
A  sim/release_check.c
A  sim/selftest_caps.c
M  sim/selftest_coll.c
A  sim/selftest_l1.c
M  sim/selftest_link.c
A  sim/selftest_packs.c
A  sim/selftest_ptpk.c
A  sim/selftest_walk.c
A  sim/sheet_lizard.c
A  sim/shot_terrarium.c
A  sim/sim_tmp.c
A  sim/sim_tmp.h
```

### Build, flash, authoring and desktop tools

```text
M  tools/check_linux.sh
M  tools/flash.sh
M  tools/flash_round.sh
M  tools/flash_square.sh
M  tools/flash_watch.sh
M  tools/gen_icons.py
A  tools/gen_terr_icons.py
M  tools/make_installer.py
M  tools/make_ota_manifest.py
M  tools/model_trailer.py
M  tools/pt_boards.py
A  tools/ptpk.py
A  tools/ptpk_emit.c
A  tools/sim_docker.sh
```

