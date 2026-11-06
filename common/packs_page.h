/* packs_page.h - THE PACKS PAGE (issue #4 phase 4, docs/card-library.md "The PACKS page"): the keeper's door to the card library.
 *
 * Under the ADVANCED page (advanced.h: the hidden BOOT chord on the settings page - a deliberate action): a PACKS button in the
 * foot's left slot (only on a board with a card slot: card.h card_slot), BACK stays right. Two tabs:
 *   INSTALLED  the packs in the board's flash (library.h), each with REMOVE; a pack that did not load says NOT LOADED and why.
 *   CARD       SCAN reads the card (one blocking visit; the page draws READING THE CARD first), then one row per file with its
 *              verdict and the one action it has: INSTALL, UPDATE, REMOVE (when that very file is installed), or none.
 * REMOVE and UPDATE ask first (a page of their own); when the tank lives on that pack the page says so - it will wait, paused
 * (PACK MISSING), and come back when the pack is. Every blocking call (scan, install, remove) is run by pp_tick AFTER a frame
 * that says what it is doing was drawn. The page's working area (lib_work_t 10460 B + 768 B of reasons, measured) is allocated when the page opens (card.h card_mem_alloc:
 * PSRAM on the board) and freed when it closes.
 *
 * Laid out from the settings page's own constants (render.h SET_*), so it sits inside the glass on all four boards; written once
 * here. Platform-independent; the platform routes one action (advanced.h ADV_ACT_PACKS) and logs pp_message(). A build with
 * PT_CARD 0 carries none of it. */
#ifndef PACKS_PAGE_H
#define PACKS_PAGE_H
#include "world.h"

#if PT_CARD
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tank.h"

/* ADVANCED binds the tank the keeper is looking at: REMOVE / UPDATE of a pack it lives on pauses it (progression_pack_leaving),
 * INSTALL / UPDATE of a pack it waits for resumes it */
void pp_bind_tank(tank_t *t);

bool pp_open(void);                 /* the page up on INSTALLED; false (and the page not up) when its memory could not be had */
void pp_close(void);                /* the page down, its memory freed (idempotent) */
bool pp_up(void);
void pp_tick(float dt);             /* runs the one blocking call the last tap asked for, once a frame that says so was drawn */
bool pp_take_changed(void);         /* one-shot: the pack set changed (an install, an update, a removal) since last asked - ADVANCED hands the platform ADV_ACT_PACKS, which logs pp_message() */
/* a finger that stayed where it landed: (x, y) on the frame. Returns PP_NONE, PP_TICK (a step, a tab, a page, a request: the
 * click) or PP_BACK (BACK: ADVANCED's main page again) */
enum { PP_NONE = 0, PP_TICK, PP_BACK };
int  pp_tap(float x, float y);
void pp_render(uint16_t *fb, int stride);
const char *pp_message(void);       /* the last result in words ("INSTALLED GECKO", "NO CARD", ...), upper case; "" before any */

/* for tests and the director */
enum { PP_TAB_INSTALLED = 0, PP_TAB_CARD };
enum { PP_WHAT_TAB_INSTALLED, PP_WHAT_TAB_CARD, PP_WHAT_SCAN, PP_WHAT_BACK, PP_WHAT_PREV, PP_WHAT_NEXT,
       PP_WHAT_ROW,             /* idx: the row's action button, in the rows shown now (0..pp_rows_shown()-1) */
       PP_WHAT_YES, PP_WHAT_NO  /* the confirm page's two buttons */ };
bool pp_target(int what, int idx, float *x, float *y);   /* a point on the frame inside that button; false when it is not drawn now */
int  pp_tab(void);
int  pp_rows_shown(void);           /* rows on the page now */
int  pp_rows_total(void);           /* rows in the tab (all pages) */
int  pp_page_no(void);              /* the page shown, 0-based */
int  pp_page_count(void);
bool pp_confirming(void);
const char *pp_row_button(int i);   /* the button of row i of the rows shown ("INSTALL", "UPDATE", "REMOVE"), NULL when it has none or there is no such row */
bool pp_row_text(int i, char *name, size_t nn, char *detail, size_t dn);   /* the two lines drawn for row i of the rows shown (either buffer may be NULL) */
bool pp_confirm_lives(void);        /* the open question is about a pack the bound tank lives on: it says so */
bool pp_busy(void);                 /* a blocking call is waiting for its frame */
bool pp_scanned(void);              /* the CARD tab holds a scan */
int  pp_rows_per_page(void);        /* what this glass holds */
void pp_box(int *x, int *y, int *w, int *h);   /* the page's content box on the frame: every drawn pixel of the page's own is inside it, and it is inside the glass */
#endif
#endif
