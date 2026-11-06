/* worlds/aquarium/setup.c - THE AQUARIUM's first run and birth flow (setup_world.h; issue #4 phase 3): the pages, words and
 * hooks setup.c asks world_cur()->setup for. The fish's first run is the whole flow setup.c draws: WELCOME, each fry's
 * COLORS, FORM and NAME pages, the bubble column, CARE - nothing is passed by. The gene editor, the pages a fish is designed
 * on, is this file's: the terrarium's lizards have a form of their pack's and a MORPH page instead. Only this world's bin
 * links this file. */
#include <stdio.h>
#include "setup.h"
#include "setup_world.h"
#include "render.h"
#include "tank_events.h"

/* ---- THE EDITOR (setup.h): each fry's COLORS page and FORM page, before it is named ---- */

/* a swatch that cannot be picked: a bold ink X corner to corner (the editor's
   markings the body's own color) - a shape, so it reads without color */
static void struck_out(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t under) {
    const int n = w > h ? w : h;
    uint32_t r = under >> 16 & 255, g = under >> 8 & 255, b = under & 255;
    uint32_t ink = r * 299 + g * 587 + b * 114 < 100 * 1000 ? C_CAPT : C_PANEL;   /* light on a dark swatch (charcoal), ink on the rest */
    for (int i = 2; i <= n - 3; i++) {
        int px = x + 3 + (w - 6) * i / n, py = y + 3 + (h - 6) * i / n, qy = y + h - 3 - (h - 6) * i / n;
        render_rect(fb, stride, px - 2, py - 2, 4, 4, ink);
        render_rect(fb, stride, px - 2, qy - 2, 4, 4, ink);
    }
}
/* SWAP (COLORS): a tall key between the palettes, a double-headed arrow -
   up at the body row, down at the markings row: they trade places */
static void swap_button(uint16_t *fb, int stride) {
    const int x = SETUP_SWAP_X, y = SETUP_SWAP_Y, w = SETUP_SWAP_W, h = SETUP_SWAP_H, cx = x + w / 2;
    render_rect(fb, stride, x, y, w, h, C_KEY);
    render_rect_edge(fb, stride, x, y, w, h, C_EDGE);
    const int head = 9;                                  /* each arrowhead: rows 2..18 px wide */
    for (int i = 0; i < head; i++) {
        int half = 1 + i;
        render_rect(fb, stride, cx - half, y + 6 + i, 2 * half, 1, C_TEXT);          /* up, its tip at the top */
        render_rect(fb, stride, cx - half, y + h - 7 - i, 2 * half, 1, C_TEXT);      /* down, its tip at the foot */
    }
    render_rect(fb, stride, cx - 2, y + 6 + head, 4, h - 12 - 2 * head, C_TEXT);    /* the shaft between them */
}
/* the editor's rows (setup.h, THE EDITOR): where each sits and what a pick is */
typedef struct { int y, h, x, px, w, n, hit0, gene; const char *label; } edit_row_t;
static const edit_row_t FORM_ROWS[] = {
    { SETUP_FORM_SHAPE_Y, SETUP_FORM_TILE_H, SETUP_TILE_X, SETUP_TILE_PX, SETUP_TILE_W, SHAPE_N, SETUP_HIT_SHAPE0, GENE_SHAPE, "SHAPE" },
    { SETUP_FORM_FINS_Y,  SETUP_FORM_TILE_H, SETUP_TILE_X, SETUP_TILE_PX, SETUP_TILE_W, FINS_N,  SETUP_HIT_FINS0,  GENE_FINS,  "FINS" },
};
static const edit_row_t COLOR_ROWS[] = {
    { SETUP_COLOR_BODY_Y, SETUP_COLOR_SW_H,  SETUP_SW_X,   SETUP_SW_PX,   SETUP_SW_W,   LOOK_N,    SETUP_HIT_BODY0, GENE_BODY,    "BODY" },
    { SETUP_COLOR_PAT_Y,  SETUP_COLOR_PAT_H, SETUP_PAT_TILE_X, SETUP_PAT_TILE_PX, SETUP_PAT_TILE_W, PATTERN_TILES, SETUP_HIT_PAT0, GENE_PATTERN, "PATTERN" },
    { SETUP_COLOR_ACC_Y,  SETUP_COLOR_SW_H,  SETUP_SW_X,   SETUP_SW_PX,   SETUP_SW_W,   LOOK_N,    SETUP_HIT_ACC0,  GENE_ACCENT,  "MARKINGS" },
};
static bool form_page(int page) { return page == SETUP_PG_FORM_A || page == SETUP_PG_FORM_B; }
static int edit_rows(int page, const edit_row_t **r) {
    if (form_page(page)) { *r = FORM_ROWS; return (int)(sizeof FORM_ROWS / sizeof *FORM_ROWS); }
    *r = COLOR_ROWS; return (int)(sizeof COLOR_ROWS / sizeof *COLOR_ROWS);
}
/* the page's genes at random (ROLL): never markings that match the body */
static void edit_roll(tank_t *t, int fish, int page) {
    const edit_row_t *r; int n = edit_rows(page, &r);
    for (int k = 0; k < n; k++) {
        int nv = r[k].gene == GENE_PATTERN ? PATTERN_N : r[k].n;   /* ROLL can land on SOLID too */
        int v = (int)tank_randf(t, 0, nv - 0.001f);
        if (r[k].gene == GENE_ACCENT)
            for (int guard = 0; guard < 16 && LOOK_ACCENT[v] == t->fish[fish].color; guard++) v = (int)tank_randf(t, 0, r[k].n - 0.001f);
        tank_set_gene(t, fish, r[k].gene, v);
    }
}
static int edit_hit(int page, float x, float y) {        /* (page coordinates) the pick under the finger; 0 = none */
    if (!form_page(page) && setup_kit_in_box(x, y, SETUP_SWAP_X, SETUP_SWAP_Y, SETUP_SWAP_W, SETUP_SWAP_H, 6)) return SETUP_HIT_SWAP;   /* before the rows' bands */
    /* the rows: a band from a row's label to halfway into the gap under
       it (the last one 24 px past its foot - fingers land low), the
       nearest choice along x */
    const edit_row_t *r; int n = edit_rows(page, &r);
    for (int k = 0; k < n; k++) {
        int top = r[k].y - SETUP_LABEL_DY - 4;
        int bot = k + 1 < n ? (r[k].y + r[k].h + r[k + 1].y - SETUP_LABEL_DY - 4) / 2 : r[k].y + r[k].h + 24;
        if (k + 1 < n && bot < r[k].y + r[k].h) bot = r[k].y + r[k].h;
        int rw = (r[k].n - 1) * r[k].px + r[k].w;
        if (y < top || y >= bot || x < r[k].x - 12 || x >= r[k].x + rw + 12) continue;
        int i = (int)((x - r[k].x + (r[k].px - r[k].w) * 0.5f) / r[k].px);
        if (i < 0) i = 0;
        if (i >= r[k].n) i = r[k].n - 1;
        return r[k].hit0 + i;
    }
    return 0;
}
static void edit_tap(tank_t *t, int page, int fish, int id) {   /* the fish wears a pick at once: the fry on stage, the grown portrait */
    const fish_t *f = &t->fish[fish];
    if (id == SETUP_HIT_ROLL) { edit_roll(t, fish, page); tank_emit(TEV_WHEEL_TICK, -1); return; }
    if (id == SETUP_HIT_SWAP && !form_page(page)) {      /* body <-> markings: one palette, so exact */
        int b = tank_gene(f, GENE_BODY), a = tank_gene(f, GENE_ACCENT);
        tank_set_look(t, fish, LOOK_COLORS[a], LOOK_COLORS[b]);
        tank_emit(TEV_WHEEL_TICK, -1); return;
    }
    const edit_row_t *r; int n = edit_rows(page, &r);
    for (int k = 0; k < n; k++)
        if (id >= r[k].hit0 && id < r[k].hit0 + r[k].n) {
            int v = id - r[k].hit0;
            if (r[k].gene == GENE_ACCENT && LOOK_ACCENT[v] == f->color) { setup_kit_set_note(75); return; }   /* struck out: say why (~3 s) */
            if (r[k].gene == GENE_PATTERN && v == f->pattern) v = PATTERN_SOLID;   /* the picked tile again: deselected - solid */
            tank_set_gene(t, fish, r[k].gene, v);
            return;
        }
}
/* straight on the tank, the fry ringed on stage (NOW), the grown fish on a dark plate (GROWN UP), the rows under a panel band */
static void edit_page(const tank_t *t, uint16_t *fb, int stride, double clock, int page, int fish) {
    const int CX = PAGE_W / 2;
    const fish_t *f = &t->fish[fish];
    char cap[40]; snprintf(cap, sizeof cap, "%s FOR THE %s FISH", form_page(page) ? "A FORM" : "COLORS", fish ? "SECOND" : "FIRST");   /* not named yet (0.4) */
    setup_kit_text_c(fb, stride, CX, SETUP_Y + 7, 2, C_CAPT, cap);
    setup_kit_nav(fb, stride, true, "NEXT", false);
    render_button(fb, stride, SETUP_ROLL_X, SETUP_TOP_BTN_Y, SETUP_TOP_BTN_W, SETUP_BTN_H, C_KEY, C_DIM, "ROLL", 2);
    const edit_row_t *r; int n = edit_rows(page, &r);
    int band = SETUP_ED_CAPTION_Y - 6;                       /* from the captions down: fish swim behind them */
    render_rect_blend(fb, stride, -PAGE_X, band, TANK_W, r[n - 1].y + r[n - 1].h + 10 - band, C_PANEL, 110);
    setup_kit_fish_ring(fb, stride, f, 17 * f->size + 6, f->color);
    render_rect_blend(fb, stride, SETUP_ED_GROWN_X - 72, SETUP_ED_PREVIEW_Y - 26, 144, 54, C_PANEL, 150);   /* clear of the buttons */
    render_fish_preview(fb, stride, SETUP_ED_GROWN_X, SETUP_ED_PREVIEW_Y, SETUP_ED_GROWN_SIZE, f->color, f->fin, f->accent, f, FISH_ROOM_CARD, clock);
    setup_kit_text_c(fb, stride, (SETUP_ED_NOW_X + SETUP_ED_GROWN_X) / 2, SETUP_ED_PREVIEW_Y - 10, 3, C_CAPT, ">");
    setup_kit_text_c(fb, stride, SETUP_ED_NOW_X, SETUP_ED_CAPTION_Y, 2, C_CAPT, "NOW");
    setup_kit_text_c(fb, stride, SETUP_ED_GROWN_X, SETUP_ED_CAPTION_Y, 2, C_CAPT, "GROWN UP");
    for (int k = 0; k < n; k++) {
        int cur = tank_gene(f, r[k].gene);
        char lab[32];
        if (r[k].gene == GENE_PATTERN) snprintf(lab, sizeof lab, "%s  %s", r[k].label, PATTERN_NAMES[cur]);   /* its tiles carry no names */
        else if (r[k].gene == GENE_ACCENT && setup_kit_note() > 0) snprintf(lab, sizeof lab, "%s  SAME AS ITS BODY", r[k].label);
        else snprintf(lab, sizeof lab, "%s", r[k].label);
        bool unused = r[k].gene == GENE_ACCENT && f->pattern == PATTERN_SOLID;   /* a solid fish wears no markings */
        render_text(fb, stride, r[k].x + 2, r[k].y - SETUP_LABEL_DY, 2, unused ? setup_kit_dim(C_CAPT, 45) : C_CAPT, lab);
        for (int i = 0; i < r[k].n; i++) {
            int x = r[k].x + i * r[k].px, y = r[k].y, w = r[k].w, h = r[k].h;
            bool on = i == cur;
            if (r[k].gene == GENE_BODY || r[k].gene == GENE_ACCENT) {
                uint32_t rgb = r[k].gene == GENE_BODY ? LOOK_BODY[i] : LOOK_ACCENT[i];
                bool off = r[k].gene == GENE_ACCENT && rgb == f->color;   /* markings the body's own color would vanish */
                render_rect(fb, stride, x, y, w, h, rgb);                 /* in its own color, so it can be told (Strato
                                                                             10-04: dimmed, it was a mystery color) */
                render_rect_edge(fb, stride, x, y, w, h, on ? C_TEXT : C_DIM);
                if (on) render_rect_edge(fb, stride, x + 1, y + 1, w - 2, h - 2, C_TEXT);
                if (off) struck_out(fb, stride, x, y, w, h, rgb);              /* a shape says "not this one", never a hue */
                continue;
            }
            /* a tile: a small grown fish of that option, in this fish's colors */
            fish_t form = *f;
            if (r[k].gene == GENE_SHAPE) form.shape = (uint8_t)i;
            else if (r[k].gene == GENE_FINS) form.fins = (uint8_t)i;
            else form.pattern = (uint8_t)i;
            render_rect(fb, stride, x, y, w, h, C_KEY);
            render_rect_edge(fb, stride, x, y, w, h, on ? C_TEXT : C_DIM);
            if (on) render_rect_edge(fb, stride, x + 1, y + 1, w - 2, h - 2, C_TEXT);
            if (r[k].gene == GENE_PATTERN)
                render_fish_preview(fb, stride, x + w / 2.0f, y + h / 2.0f, 1.4f, f->color, f->fin, f->accent, &form, FISH_ROOM_ROW, clock);
            else {
                render_fish_preview(fb, stride, x + w / 2.0f, y + 21, 1.0f, f->color, f->fin, f->accent, &form, FISH_ROOM_ROW, clock);
                setup_kit_text_c(fb, stride, x + w / 2, y + h - 17, 2, on ? C_TEXT : setup_kit_dim(C_CAPT, 60),
                                 r[k].gene == GENE_SHAPE ? SHAPE_NAMES[i] : FINS_NAMES[i]);
            }
        }
        if (unused)                                     /* a solid fish: the markings row stands back (still a pick away -
                                                           choosing a pattern brings them back) */
            render_rect_blend(fb, stride, r[k].x - 2, r[k].y - 2, (r[k].n - 1) * r[k].px + r[k].w + 4, r[k].h + 4, C_PANEL, 150);
    }
    if (!form_page(page)) swap_button(fb, stride);
    setup_kit_dots(fb, stride, PAGE_H - 14);
}
static const setup_page_t EDITOR = { edit_hit, edit_tap, edit_page };   /* the FORM pages' and the COLORS pages' both */

/* a mutation in words for the announcement (0.4): "SOMETHING NEW: SAIL FINS" - a fish shows all five genes, so each is news */
static bool mutation_words(const tank_t *t, const fish_t *f, char *out, size_t n) {
    static const char *const PAT[PATTERN_N] = { "BARS", "A MASK", "AN EYE BAND", "NO MARKINGS" };
    switch (f->mutated - 1) {
    case GENE_SHAPE:   snprintf(out, n, "SOMETHING NEW: A %s BODY", SHAPE_NAMES[f->shape]); break;
    case GENE_FINS:    snprintf(out, n, "SOMETHING NEW: %s FINS", FINS_NAMES[f->fins]); break;
    case GENE_PATTERN: snprintf(out, n, "SOMETHING NEW: %s", PAT[f->pattern < PATTERN_N ? f->pattern : 0]); break;
    case GENE_BODY:    snprintf(out, n, "SOMETHING NEW: ITS BODY COLOR"); break;
    default:           snprintf(out, n, "SOMETHING NEW: ITS MARKINGS"); break;
    }
    return true;
}

static const char *const WELCOME[] = {
    "TWO FRY HAVE MOVED IN.",
    "THEY EAT, PLAY, REST AND",
    "GROW UP WHILE YOU WATCH.",
    "EACH ONE THINKS FOR ITSELF.",
    "FIRST, LET'S MEET THEM.",
};
static const char *const CARE[] = {
    "TAP THE SURFACE TO FEED",
    "HOLD A FINGER AND THEY VISIT",
    "TAP A FISH TO CHECK ON IT",
    "DRAG THE GLASS TO WIPE ALGAE",
    "SWIPE ACROSS GRASS TO TRIM",
};

/* straight on the tank: the fish being named wears a ring in its own colour */
static void name_preview(const fish_t *f, bool young, uint16_t *fb, int stride, double clock) {
    setup_kit_fish_ring(fb, stride, f, 17 * f->size + 6, f->color);
}

/* each of the fish's five genes, its value, and where it came from - a parent (* = that parent's hidden copy), or NEW! */
static bool family_rows(const tank_t *t, const fish_t *f, uint16_t *fb, int stride) {
    bool hidden = false;
    for (int g = 0; g < GENE_N; g++) {
        int y = SETUP_FAM_ROW_Y + g * SETUP_FAM_ROW_DY, vx = SETUP_FAM_VALUE_X;
        static const char *const LABEL[GENE_N] = { "SHAPE", "FINS", "PATTERN", "BODY", "MARKINGS" };
        render_text(fb, stride, SETUP_FAM_LABEL_X, y, 2, C_CAPT, LABEL[g]);
        if (g == GENE_BODY || g == GENE_ACCENT) {
            uint32_t rgb = g == GENE_BODY ? f->color : f->accent;
            render_rect(fb, stride, vx, y - 1, 16, 16, rgb); render_rect_edge(fb, stride, vx, y - 1, 16, 16, C_INNER);
        } else render_text(fb, stride, vx, y, 2, C_TEXT, g == GENE_SHAPE ? SHAPE_NAMES[f->shape] : g == GENE_FINS ? FINS_NAMES[f->fins] : PATTERN_NAMES[f->pattern]);
        if (f->mutated == g + 1) { render_text(fb, stride, SETUP_FAM_FROM_X, y, 2, C_NEW, "NEW!"); continue; }
        setup_parent_t p = setup_kit_parent_of(t, f, (f->from_b >> g & 1) ? 1 : 0);
        bool sec = f->from_carried >> g & 1;
        hidden |= sec;
        setup_kit_text_named(fb, stride, SETUP_FAM_FROM_X, y, "", p.name, p.known ? p.color : C_DIM, sec ? " *" : NULL);
    }
    return hidden;
}

/* the placement page: the pieces' depths in words - the castle, the pumpkin and the volcano stand among the plants (the plants
   grow in front of one BEHIND, it stands in front of them IN FRONT), the sword plant has one depth over the seagrass - and the
   plant's stripe rises to the tips of its leaves (tank_veg_bed) */
static const char *const PLANT_HINT[DECOR_Z_N] = { "IN FRONT OF THE SEAGRASS", "", "IN FRONT OF THE SEAGRASS" };   /* (the sword plant:
                                                                   one depth since 0.4, over the seagrass - the fish choose their own) */
static const char *const CASTLE_HINT[DECOR_Z_N] = { "THE PLANTS GROW IN FRONT OF IT", "", "IT STANDS IN FRONT OF THE PLANTS" };
static void place(const tank_t *t, int item, int z, setup_place_t *out) {
    out->hint = item == 2 || item == 8 || item == 9 ? CASTLE_HINT[z] : PLANT_HINT[z];
    if (item == 0) tank_veg_bed(t, 3, NULL, NULL, &out->top, NULL);   /* the leaves' reach */
    else out->top = tank_decor_fy(t, item) - tank_decor_height(item);   /* its highest pixel on its depth's floor line (0.3.9) */
    out->low = false;
}

const setup_world_t aquarium_setup = {
    .skip = 0,
    .form = &EDITOR,
    .colors = &EDITOR,
    .mutation_words = mutation_words,
    .welcome_title_y = 36, .welcome_lines_y = 96,
    .welcome_lines = WELCOME, .n_welcome_lines = (int)(sizeof WELCOME / sizeof *WELCOME),
    .name_first = "NAME THE FIRST FISH", .name_second = "NAME THE SECOND FISH", .name_birth = "NAME THE NEW FRY",
    .name_rename = "RENAME YOUR FISH",
    .name_preview = name_preview,
    .care_lines = CARE, .n_care_lines = (int)(sizeof CARE / sizeof *CARE),
    .born_title = "A NEW FRY!", .born_parents = "BORN TO %s AND %s",
    .born_shop = "IN AT THE SURFACE.",
    .born_staged = "DOWN IN THE GRASS.",
    .family_rows = family_rows,
    .place = place,
};
