# The decoration shelf - design on paper (2026-10-19, nothing built) - EARMARKED 0.3.9, D1-D6 DECIDED

> Strato, 2026-10-19: "a 'decoration shelf' mechanic where users can store
> decorative items for their tank for later. The only mechanism right now to
> remove something is to sell it. We should give folks the ability to store
> things they no longer want to feature in the tank, but may in the future."

Same day, the rule it grows out of (community feedback on seasonal items):
**nothing a keeper has bought ever disappears from their device.** A season
ends in the SHOP, never in the tank. The pumpkin is the first case: the
first release of November sets `SD_PUMPKIN_IN_SHOP 0` (progression.h) and
nothing else - a keeper who bought one keeps it, with its shop row, for as
long as they like. The shelf is where that pumpkin waits for next October.

## What it is, in one breath

Every piece the keeper can MOVE can also be put on the shelf (Strato:
"anything you can MOVE you can SHELVE"). A shelved piece is still theirs -
its spot, its depth, its color kept - but it is out of the tank: not drawn,
not lit, not an anchor, not a landmark, not cover. The living ones (coral,
reef cluster, sword plant) KEEP GROWING on the shelf, each in its own small
holding tank drawn on its shop row - a nice surprise when it comes back. The
row says ON THE SHELF and PUT BACK returns it where it was. Nothing is paid,
nothing is refunded, nothing is lost. SELL stays the only way to be rid of a
piece, and it still works from the shelf.

## What exists to build on (release-0.3.7 line; main has the same code)

- **Ownership is a bit:** `tank_t.sd_unlocks`, one `SD_ITEM_*` bit per item
  (nine). Everything that asks "is the castle here" reads that bit.
- **The placeable pieces** are the five decor slots (tank.c
  `DECOR_SLOT_ITEM`): castle 2, coral 3, cluster 4, pumpkin 8, plant 0.
  `tank_decor_placeable(item)` names them; the snail, shrimp, urchin and
  seahorse are residents and never placeable (the snail is not even sellable).
- **A piece's state** already lives in the save, append-only: `castle_x/z`,
  `coral_x/z/rgb/growth`, `cluster_x/z/scheme/growth`, `pumpkin_x/z`,
  `plant_x/z` + `veg_h[3][]`, and `decor_rank[5]` (the stack order).
- **SELL** (`progression_sell`): clears the bit, refunds `SD_SELL_PCT` 20%,
  and `tank_decor_reset` wipes the state to factory (the coral loses its
  color and its weeks of growth - the thing the shelf must NOT do).
- **The order of the pieces** (`tank_decor_order`) lists the OWNED slots in
  rank order; the renderer paints from that list, so the renderer has no
  ownership test of its own (0 `SD_ITEM_` decor gates in render.c).
- **The other readers of the bit** (the sites the shelf must turn): tank.c
  `tank_reef_spot` (the cluster as the fish's landmark), the seahorse's
  anchor POSTs on the castle / coral / cluster, `tank_decor_hit` (the
  placement page's finger), the coral and cluster growth ticks (~1718, ~1732),
  `tank_decor_owned`; progression.c `reef_visit` (~1003, the FIRST REEF
  badge's visit). The TMS_FIRST_REEF availability (render.c ~3897, "once the
  reef cluster is bought") stays on OWNED - a shelved cluster is still
  bought. So do the growth ticks `coral_grow` / `cluster_grow` (~1718,
  ~1732): they gate on OWNED today, which is exactly "keeps growing on the
  shelf" - no change there.
- **The sword plant is bed 3 of the grass** (`tank_veg_beds` returns 4 once
  the plant is owned; `veg_grow` grows every bed; `veg_inside` makes a bed
  cover; the trim, the smother band and the urchin read the beds too). On
  the shelf the bed must GROW but be neither drawn, nor cover, nor cut, nor
  smothered, nor grazed: `tank_veg_beds` stays 4 (growth, save), and the
  cover / hit / trim / smother / urchin / draw paths skip bed 3 unless
  `tank_decor_in(t, 0)`. Director.c prints and `place` guard (8 sites), sim selftests (77
  mentions, mostly setup) are the rest.
- **The shop UI** (render.c `render_shop`): a row per item - IN THE TANK
  with a dim IN TANK button once owned; the owned placeable piece's modal
  has MOVE | SELL (SELL armed, then "+N OK?"). The modal is `SHP_MODAL_W` 352
  px, buttons `MSP_HOW_W` 100 with a 16 px gap on every shipped board (one
  number in render.h).
- **The placement page** (setup.c `setup_begin_place`): drag x, DEPTH tiles,
  the order arrows, SELL top left (armed twice), DONE. MOVE from the modal
  re-opens it; a purchase opens it at once.

## The design

### State: one more bit mask

`uint32_t sd_shelved` in tank_t, saved append-only at the next free offset
(1908 on release-0.3.7 today; main's 0.4 tail ends at 1864 inside the
reserved `pad_04` block - the two lines MUST pick the same offset, so this
field is placed once, on the line that builds it, and the other line
reserves it in the same commit that merges forward). Invariant: `sd_shelved`
is a subset of `sd_unlocks`, and only placeable bits are ever set (the load
masks it: `sv.sd_shelved & t->sd_unlocks & DECOR_BITS`).

One new predicate is the word the whole tank reads:

    bool tank_decor_in(const tank_t *t, int item);   /* owned and NOT on the shelf */

Every site listed above that means "is the piece in the water" moves from
`sd_unlocks & SD_ITEM_X` to `tank_decor_in(t, item)`. `tank_decor_order`
lists only pieces IN the tank, so the renderer, the bake, the pumpkin's night
light, the castle's shadow, the anchor posts, the hit test and the stack all
follow from that one change. `tank_decor_owned` keeps its meaning (bought).

### Two operations, beside buy and sell (progression.c)

    bool progression_shelve(tank_t *t, int item);    /* owned, placeable, in the tank -> on the shelf; saves */
    bool progression_unshelve(tank_t *t, int item);  /* on the shelf -> back in, raised to the top of its stack; saves */

- Shelve: set the bit, `decor_rank[slot] = 0` (it leaves the stack; the
  others close up on the next normalize), `mark_dirty`, save. No money moves.
  A toast is wrong here (toasts are dollars); the row's new words and the
  little tank are the confirmation.
- Unshelve: clear the bit, `tank_decor_raise` (the piece placed last goes on
  top - the 2026-10-17 rule applies to a return as to a purchase), save, then
  the placement page opens exactly as after a purchase - with the OLD spot
  and depth already in place, so DONE with no drag puts it back where it
  stood. The coral's page shows its kept color.
- Sell from the shelf: `progression_sell` as today (clear both bits, refund,
  reset). The refund is the same 20% whether in the tank or on the shelf.
- Buy: unchanged. A piece on the shelf is owned, so its row never offers
  UNLOCK; a sold piece is not owned, so its row offers UNLOCK again (and the
  pumpkin's row vanishes when the build no longer sells it - see below).

### Growing on the shelf (D4, Strato: "keep them growing, that might be a nice surprise")

The living pieces grow on the shelf at the same pace as in the tank: the
coral and the cluster through their existing ticks (owned, not in - no
change), the sword plant through `veg_grow` over all four beds (bed 3 stays
a bed; it just is not in the water). A coral put away at 60% comes back
weeks later in full fan, its color kept; a plant comes back at its ceiling.
Nothing on the shelf can be hurt: no smother, no cutting, no grazing, no
algae. The two things that stop with the piece out of the water are its
effects on the tank and the keeper's hands on it.

**Each shelved living decoration is shown inside its own small tank** on
its shop row (Strato): the row's 64 px icon slot becomes a little glass box
- a water tint, a sand line, the glass's edge in MSP_TEAL - with the piece
drawn LIVE from its state inside it (the coral in its color at its growth,
the cluster in its scheme at its size, the plant's fronds at their heights).
The castle and the pumpkin, which do not live, sit as their shop icon on a
shelf line (a board under the icon) - "on the shelf" said with a picture,
not a hue. The shelved piece's modal shows the same at 2x where the icon
goes today, with a line under the name: GROWING  64% (the coral and the
cluster) or GROWING (the plant); the castle and the pumpkin say ON THE SHELF.
How it is drawn: the piece's painter draws at full size into the PSRAM decor
scratch (where the coral / cluster / plant are already prepared for the
bake), then a box filter brings it down ~3:1 into a 64 x 64 cache per shelved
piece, rebuilt when the shop opens and whenever its growth moves a percent -
never per frame, so the page keeps its frame rate ([[overlay-frame-rate]]).
If the painters cannot be pointed at a scratch cheaply on every board, the
fallback is the shop icon inside the glass box with a growth bar under it;
the live picture is the goal.

Everything else that happens because a piece is in the tank stops with it:
the pumpkin's light and the castle's shadow (not baked), the seahorse's
posts on it (it finds a blade), the fish's reef landmark (back to the tank's
own `reef_x/y`), cover from the plant (the stress decay reads the beds that
are there, as after a sale).

### The shop page

- **Row**, owned and in: as today (IN THE TANK, dim IN TANK).
- **Row**, owned and shelved: the little tank (living) or the icon on its
  shelf line (castle, pumpkin) in the icon slot, the second line **ON THE
  SHELF**, the button **PUT BACK** (teal, live: one tap unshelves and opens
  the placement page). Words and pictures, not color, carry the state
  ([[pocket-tank-battery-page]]: Strato is color blind).
- **Modal**, owned and in (the placeable five): today MOVE | SELL. The shelf
  needs a third act. Two layouts, D2 decides:
  - (a) three buttons MOVE | SHELVE | SELL: 3 x 100 + 2 x 16 = 332 px in a
    352 px modal - 10 px of margin each side, tighter than anything else on
    the page, and SELL sits a thumb's width from SHELVE on a 1.8.
  - (b) **DECIDED (D2):** MOVE | SHELVE in the modal; SELL leaves the modal
    and lives where it already also lives - the placement page's top left
    (MOVE then SELL). Selling is the rare, irreversible act and the extra tap
    is the right price for it ([[forgive-the-kids]]); shelving is the
    everyday one and gets the modal. The "SELLS BACK FOR N" line moves to
    the placement page's SELL arm ("+N OK?" already says it there).
- **Modal**, owned and shelved: the little tank at 2x, the line GROWING 64%
  / GROWING / ON THE SHELF, buttons **PLACE | SELL** (SELL armed as today).
- **Modal**, not owned: unchanged.
- The 28-char line budget holds every new string: ON THE SHELF 12, SHELVE 6,
  PLACE 5, PUT BACK 8 (the row button is 116 px wide; 8 chars at scale 2 is
  ~95 px - fits there, NOT in a 100 px modal button, which is why the modal
  verb is SHELVE and not PUT AWAY; D3 may trade the words).

No new page. The shop's rows ARE the shelf: a keeper sees at a glance what
is in the tank, what is on the shelf, what is still for sale. A shelf strip
on the milestones TANK row or a shelf icon on the glass is a later nicety,
not part of this.

### The pumpkin, and seasons after it

- The November release flips `SD_PUMPKIN_IN_SHOP` to 0. `progression_
  pumpkin_offered` already keeps the row for an owner (owned includes
  shelved), `shp_rows` already drops it otherwise, the selftest already
  follows the switch. Nothing else to do for the pumpkin itself.
- An owner who SELLS an out-of-season pumpkin cannot buy it back until a
  build sells it again. The shelved modal's SELL stays (it is their piece)
  but for a piece the build does not sell the line under the words reads
  **NOT IN THE SHOP NOW** in both its modals, so the choice is informed.
- Next October a release sets the flag back to 1 and the lantern is for
  sale again, at the same 75; a keeper with one on the shelf just presses
  PUT BACK. That is the seasonal pattern from now on: a shop flag per
  seasonal piece, flipped by the release before and after the season;
  never a date gate, never a removal (D5 confirms it is "every October").

### Saves, rollbacks, the other boards

- Append-only, 4 bytes. An older firmware (0.3.8) reading a save with a
  shelved piece ignores the field: the piece REAPPEARS in the tank at its
  kept spot. Nothing lost, the rollback rule holds ([[rollback-keeps-tail]]).
  Moving forward again, the shelf bit is still set and it goes back on the
  shelf.
- The backup page copies NVS raw: nothing to do. The tank link carries
  fish, not decor: nothing to do. The staged tank (director) gets
  `shelve <piece>` / `unshelve <piece>` beside `buy` / `sell` / `place`.
- Per board: no distance x time numbers (BOARDS.md rule 5 is not touched);
  the shop modal's widths are one set of numbers for all four, the P4's wide
  shop is its own layout and takes the same three states.

### Tests (before anything is flashed)

- `--selftest-shop` grows: buy castle + coral, set the coral's color and
  grow it to 0.6, shelve both; assert `tank_decor_in` false, `tank_decor_
  order` lists neither, `tank_reef_spot` falls back, the seahorse's post list
  has none on them, 1 h of ticks MOVES `coral_growth` past 0.6 by the same
  step an in-tank hour gives; the plant: shelved, its bed grows, is not
  cover (`veg_inside` false at its fronds), the trim and the urchin leave it,
  a smothered tank with only bed 3 tall is not smothered; save, reload (both
  bits kept), unshelve the coral: color and growth intact, rank on top; sell
  the castle from the shelf: refund 30, both bits clear, state reset. Then the pumpkin: shelved, out of the shop (flag 0) - row kept,
  `progression_buy` refused, PUT BACK works.
- Pixel pins (selftest-shop's existing day / night compares): a tank with a
  shelved pumpkin paints and lights exactly as one that never had it; a
  shelved castle casts no shadow.
- The golden save gains one file; QEMU boot; check-all x 4 worlds as every
  release.

### Copy for the keeper (one plain instruction; [[keeper-copy-keep-it-simple]])

Changelog / manual: "Put a decoration away for later. In the shop, tap it,
then SHELVE. It waits on the shelf, and a coral or a plant keeps growing in
its own little tank there. Tap PUT BACK when you want it in the tank again.
Selling is still the only way to let a piece go." Pip's note can be one line
about the pumpkin: it leaves the shop, yours stays, and now it has a shelf to
winter on.

## Size and when

A day and a half to two: ~40 lines of state and the two operations, ~40 for
the predicate sweep (the plant's bed paths are most of it), ~80 in the shop
page and the placement page, the little tanks (the scratch draw, the box
filter, the cache, the glass box and the shelf line) another ~120, the
director, the selftest, the docs. **EARMARKED 0.3.9**, the first release of
November, with the pumpkin flag (the two tell one story), cut from the
release-0.3.7 line and merged forward into main, which must reserve the save
offset.

## Decisions - ALL SIX TAKEN BY STRATO, 2026-10-19

- **D1. Which pieces:** all five placeables, the sword plant included -
  "anything you can MOVE you can SHELVE."
- **D2. Where SHELVE and SELL sit:** MOVE | SHELVE in the modal; SELL only
  on the placement page (one more tap to sell). The "SELLS BACK FOR N" line
  leaves the modal.
- **D3. The words:** SHELVE / PLACE / PUT BACK / ON THE SHELF.
- **D4. Growth on the shelf:** KEEPS GROWING (Strato: "that might be a nice
  surprise"), and every shelved living decoration is shown inside its own
  small tank on its row and in its modal. The frozen alternative is dropped.
- **D5. The pumpkin's seasons:** back in the shop every October at 75, the
  flag flipped by the release before and after; never a date gate.
- **D6. The release:** 0.3.9, the first release of November, with the
  pumpkin flag.

First step when it starts: `--selftest-shop` grows first (the asserts
above), then the state and the predicate sweep, then the shop page, then the
little tanks - the one piece with an engineering question (the painters
into a scratch), so it comes last and has its fallback.
