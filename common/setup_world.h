/* setup_world.h - the first-run flow's world module (issue #4 phase 3): what setup.c used to decide by which world the bin was
 * built for - the pages a world has, the words each says, the code only it runs - as ONE const descriptor per world,
 * defined in common/worlds/<world>/setup.c and reached through world_cur()->setup (world_ops.h). setup.c is the flow's
 * engine: the state machine, the gestures, the hit tests and the pages both worlds have. It never asks which world it is.
 *
 * The rules are world_ops.h's: data first - a word, a mask, a count -, a hook only where the world RUNS something or draws
 * a page only it has. A hook gets what it reads as arguments, draws with THE KIT below (setup.c's own helpers, exported
 * as setup_kit_*) and the public headers (tank.h, render.h, tank_events.h), and keeps its state in its own file (the
 * terrarium's morph count is in its file, set by `begin`). Each world's file holds only that world's: the other world's bin
 * links none of it, and the aquarium's file includes no lizard header. */
#ifndef SETUP_WORLD_H
#define SETUP_WORLD_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tank.h"

typedef struct setup_world_s setup_world_t;
extern const setup_world_t aquarium_setup;     /* common/worlds/aquarium/setup.c (the aquarium's bin only) */
extern const setup_world_t terrarium_setup;    /* common/worlds/terrarium/setup.c (the terrarium's bin only) */

/* ---- THE KIT: the first-run flow's palette (the reset prompt's: ink panel, teal edge, calm / lit buttons) and the helpers
 * it draws with, which a world's pages share. Page coordinates (render.h), as every drawing of the flow ---- */
#define C_PANEL  0x04141a
#define C_EDGE   0x9fd8e2
#define C_INNER  0x1c2f36
#define C_DIM    0x2a3f45
#define C_KEY    0x0e2229
#define C_TEXT   0xffffff
#define C_CAPT   0x9fd8e2
#define C_GO     0x155e58      /* BEGIN: the teal of a charging battery */
#define C_GO_E   0x38dcc7
#define C_FILL   0x5f8a92      /* the family page's trait bars: a muted teal the parents' ticks stand out on */
#define C_NEW    0xffd166      /* a mutation (0.4): gold - NEW!, the announcement's line and ring */

void     setup_kit_text_c(uint16_t *fb, int stride, int cx, int y, int scale, uint32_t rgb, const char *s);   /* s centred on cx */
/* BACK (CANCEL on a rename page) and the next button, along the top row or at the foot; `go` = the lit, teal one */
void     setup_kit_nav(uint16_t *fb, int stride, bool top, const char *next_label, bool go);
void     setup_kit_dots(uint16_t *fb, int stride, int y);     /* the page dots: where the keeper is in the flow (a page the world passes by has no dot) */
uint32_t setup_kit_dim(uint32_t c, int pct);                  /* c toward the ink, pct% of it left */
/* a ring round a fish where it swims: the fish is in the TANK, the drawing on the page */
void     setup_kit_fish_ring(uint16_t *fb, int stride, const fish_t *f, float r, uint32_t rgb);
/* a fish's parent w (0 = parent_uid[0] / parent_a, 1 = the other), wherever it lives: one of ours, a fish of another tank
 * (PT_NET), else "?" (known = false) */
typedef struct { const char *name; uint32_t color; float bold, sociable; bool known; } setup_parent_t;
setup_parent_t setup_kit_parent_of(const tank_t *t, const fish_t *f, int w);
/* a caption with a name in it, the name in its own colour: `pre` NAME `post` (post may be NULL) */
void     setup_kit_text_named(uint16_t *fb, int stride, int x, int y, const char *pre, const char *name, uint32_t name_rgb, const char *post);
bool     setup_kit_in_box(float x, float y, int bx, int by, int bw, int bh, int m);   /* (x, y) in the box grown by m px all round: a fingertip's slop */
/* a page's transient note, in frames: a world's page raises it (the editor's "SAME AS ITS BODY", ~3 s after a tap on a struck-out
 * swatch) and reads it while it draws; the engine counts it down each frame and clears it when the page changes */
int      setup_kit_note(void);
void     setup_kit_set_note(int frames);

/* ---- a page the world draws in the place of one of the engine's (the FORM and COLORS pages): its hit test, its taps, its
 * drawing. Each gets the SETUP_PG_* in hand (one hook set may serve a world's FORM and COLORS pages both) and the fish the
 * page is about. The page keeps the engine's frame of it: setup_hit answers the top row's BACK / NEXT / ROLL before it asks
 * `hit`, and `draw` draws the title, the nav row (setup_kit_nav), its own ROLL button and the page dots (setup_kit_dots) itself ---- */
typedef struct {
    int  (*hit)(int page, float x, float y);             /* the element id under the finger (page coordinates), 0 = none */
    void (*tap)(tank_t *t, int page, int fish, int id);  /* a tap on element `id`, ROLL included */
    void (*draw)(const tank_t *t, uint16_t *fb, int stride, double clock, int page, int fish);
} setup_page_t;

/* ---- the placement page's world flavour (setup.c: a piece's page, SETUP_PG_PLACE): what the piece's depth, its words and
 * its stripe over the water say in this world ---- */
typedef struct {
    const char *hint;      /* the line under the DEPTH bar (or the order squares' relation, which fills it first) */
    float       top;       /* the piece's highest pixel, in tank coordinates: the stripe over its footprint rises to it */
    bool        low;       /* a small piece on its floor line (the terrarium's decorations): the stripe runs to its feet, no chevron
                              over it, the "drag it" line above it */
} setup_place_t;

#define SETUP_SKIP(pg) (1u << (pg))                      /* a SETUP_PG_* (setup.h) in the skip mask below */

struct setup_world_s {
    /* ---- the first run's pages ---- */
    uint32_t skip;                                       /* the pages both ways (NEXT, BACK, the dots) pass by: SETUP_SKIP(SETUP_PG_*) or'd */
    void   (*begin)(const tank_t *t);                    /* the first run opens (setup_begin): the world's own state for its pages; NULL = none */
    const setup_page_t *form;                            /* the world's page for the FORM pages (SETUP_PG_FORM_A / _B); NULL = none (skip them) */
    const setup_page_t *colors;                          /* ... and for the COLORS pages (SETUP_PG_COLORS_A / _B); NULL = none */
    bool   (*mutation_words)(const tank_t *t, const fish_t *f, char *out, size_t n);   /* "SOMETHING NEW: ..." for f's mutated gene
                                                            (1 .. GENE_N in f->mutated: the engine has checked it); false = nothing to say of it */

    /* WELCOME: the title's and the lines' y (down from the panel's top, SETUP_Y), the art between them, the lines (a 24 px pitch) */
    int         welcome_title_y, welcome_lines_y;
    void      (*welcome_art)(const tank_t *t, uint16_t *fb, int stride, double clock);          /* NULL = none */
    const char *const *welcome_lines; int n_welcome_lines;

    /* the NAME pages: their titles (the first run's two, the birth flow's, the rename page's) and the portrait of the one being
       named (`young`: its GROWN UP form is drawn beside it, from x SETUP_ED_GROWN_X on, and this one at SETUP_ED_NOW_X) */
    const char *name_first, *name_second, *name_birth, *name_rename;
    void      (*name_preview)(const fish_t *f, bool young, uint16_t *fb, int stride, double clock);

    /* CARE: the lines about its own animals, ahead of the two that end every world's (setup.c) */
    const char *const *care_lines; int n_care_lines;

    /* ---- the birth flow ---- */
    /* BORN: the title; how the parents are named (a format of two %s: parent 0's name, parent 1's); where it came in - bought
       from the shop, else the staged arrival */
    const char *born_title, *born_parents, *born_shop, *born_staged;
    /* FAMILY: the gene rows, from y SETUP_FAM_ROW_Y down; true = a row shows a gene its parent only carried (the "* HIDDEN IN
       THAT PARENT" note follows) */
    bool      (*family_rows)(const tank_t *t, const fish_t *f, uint16_t *fb, int stride);

    /* ---- the placement page ---- */
    void      (*place)(const tank_t *t, int item, int z, setup_place_t *out);   /* piece `item` at depth z */
};

#endif
