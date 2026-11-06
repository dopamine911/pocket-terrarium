/* card.h - the microSD card as a place packs come from (issue #4 phase 4, docs/card-library.md).
 *
 * THE CARD IS DATA ONLY: PTPK packs (docs/ptpk.md) - a species, a habitat, a model manifest - as plain files in one folder,
 * /packs/NAME.ptpk, no subfolders. Behaviour stays in the firmware. Nothing here writes to the card, ever, and nothing runs
 * from it: a file is read into memory, checked by the loader's own rules (packs/library.h) and, if it is good and the keeper
 * asks, copied into the board's flash (packs/pstore.h) so no tank depends on the card being in its slot.
 *
 * A card is reached in a VISIT: card_open ... card_list / card_read ... card_close, short and never per frame. The card
 * is read only when the keeper asks (the PACKS page's SCAN and INSTALL), never while a tank runs. One visit lists a card
 * and reads the files one at a time into a block the caller owns; nothing is kept between visits.
 *
 * THE PORT (the platform's, as persist_port_*): the simulator's card is a folder, POCKET_CARD_DIR (sim/card_port_sim.c; unset:
 * no card slot); the Waveshare 1.8's is its TF slot (firmware/main/card_port_sh8601.c, CONFIG_POCKET_TANK_CARD, compile-only:
 * UNVERIFIED on the board - docs/card-library.md says what is sourced and what is not). Both go through card_name_ok: a name
 * that is not a plain *.ptpk file name never leaves the port, so no file outside the one folder is ever opened (the names
 * come from a card the keeper was handed: they are not trusted). Built only when PT_CARD (world.h). */
#ifndef CARD_H
#define CARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "world.h"

#if PT_CARD
#define CARD_NAME_MAX 48        /* a listed file's name with its NUL: a plain *.ptpk name of 6..47 characters */
#define CARD_LIST_MAX 32        /* the most entries one visit lists (a card with more says so: card_list's *more) */
#define CARD_FOLDER   "packs"   /* the folder on the card, under its root: /packs/<name>.ptpk */

typedef struct { char name[CARD_NAME_MAX]; uint32_t size; } card_entry_t;

/* common/card.c: a name this port may open and list - 6..47 characters of [A-Za-z0-9._ -] ending .ptpk (any case; FAT keeps
 * the case it was given), not starting with a dot or a space, no "..", no '/', '\\' or ':' and nothing outside 0x20..0x7e.
 * Pure: no card involved. The ports call it on every name they list and on every name they are asked to read */
bool card_name_ok(const char *name);

/* ---- the port ---- */
/* this board has a card port at all, so the PACKS page may offer SCAN (the simulator: POCKET_CARD_DIR is set; the firmware
 * with the driver: the 1.8, positively - never a board whose GPIO 1, 2 and 3 are something else's). It does not say a card is
 * in the slot: card_open does */
bool card_slot(void);
/* begin a visit: the card is brought up and its root mounted. false, and why (short, upper case: "NO CARD", "CARD NOT
 * READABLE"), when it cannot be. Every true is answered by one card_close */
bool card_open(char *why, size_t n);
void card_close(void);                           /* end the visit and let the card go (idempotent) */
/* inside a visit: the regular files of /packs whose names card_name_ok takes (directories, links and anything else are not
 * listed), in name order, at most max of them; *more is true when there were others. The size is the file's: it may be 0
 * or past PTPK_FILE_MAX - that is the library's to refuse. Returns how many; -1 when the folder is not there ("NO PACKS
 * FOLDER") or cannot be read, with why */
int  card_list(card_entry_t *out, int max, bool *more, char *why, size_t why_n);
/* inside a visit: the whole file `name` (card_name_ok, else -1) into buf, at most cap bytes. Returns its length; -1 when it
 * is not there, is longer than cap (nothing is read past cap: a card file is never trusted to be small), or cannot be read */
int  card_read(const char *name, uint8_t *buf, size_t cap);
/* a block for the library's working area while its page is up (packs/library.h lib_work_t, 10460 B measured): the board's comes from
 * PSRAM - never .bss, never the internal heap the tank lives on - the simulator's from the heap; zeroed; NULL when there is none */
void *card_mem_alloc(size_t bytes);
void  card_mem_free(void *p);
#endif
#endif
