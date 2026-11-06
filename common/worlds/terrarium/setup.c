/* worlds/terrarium/setup.c - THE TERRARIUM's first run and birth flow (setup_world.h; issue #4 phase 3): the pages, words and
 * hooks setup.c asks world_cur()->setup for. A lizard's form is its pack's and there is no bubble column, so the first run's
 * pages are WELCOME (the pack named), a MORPH page (the colors page's place) and a NAME page for each hatchling, then CARE:
 * the FORM pages and BUBBLES are passed by both ways. Only this world's bin links this file. */
#include <math.h>
#include <stdio.h>
#include "setup.h"
#include "setup_world.h"
#include "render.h"
#include "tank_events.h"
#include "render_lizard.h"   /* the MORPH page: lizard_morph_index / lizard_morph_body */

static int s_morphs;         /* the MORPH page (below): the pack's morph count, from begin (at most SETUP_MORPH_MAX) */
static void begin(const tank_t *t) {
    s_morphs = tank_species(t)->n_morphs; if (s_morphs > SETUP_MORPH_MAX) s_morphs = SETUP_MORPH_MAX;
}

/* THE MORPH PAGE (the terrarium's, in the colors page's place, P5): the pack's morphs as the FORM page's named tiles,
   SETUP_TILE_N a row and a short last row centred, a grown lizard of that morph on each. A tap gives the hatchling that
   morph's own body colour (render_lizard.h lizard_morph_body): the save keeps it and every drawing of the lizard reads
   the morph back from it (lizard_morph_of). ROLL picks one at random */
static void morph_tile(int m, int *x, int *y) {
    const int row = m / SETUP_TILE_N, left = s_morphs - row * SETUP_TILE_N, in_row = left < SETUP_TILE_N ? left : SETUP_TILE_N;
    *x = (PAGE_W - (in_row - 1) * SETUP_TILE_PX - SETUP_TILE_W) / 2 + (m % SETUP_TILE_N) * SETUP_TILE_PX;
    *y = SETUP_MORPH_Y(row);
}
static void morph_set(tank_t *t, int fish, int m) { tank_set_gene(t, fish, GENE_BODY, lizard_morph_body(tank_species(t), m)); }
static int morph_hit(int page, float x, float y) {   /* (page coordinates) the nearest tile in the row band under the finger; 0 = none */
    for (int row = 0; row * SETUP_TILE_N < s_morphs; row++) {
        const int y0 = SETUP_MORPH_Y(row), last = (row + 1) * SETUP_TILE_N >= s_morphs;
        const int top = row ? (SETUP_MORPH_Y(row - 1) + SETUP_FORM_TILE_H + y0) / 2 : y0 - SETUP_LABEL_DY - 4;
        const int bot = last ? y0 + SETUP_FORM_TILE_H + 24 : (y0 + SETUP_FORM_TILE_H + SETUP_MORPH_Y(row + 1)) / 2;   /* (fingers land low) */
        if (y < top || y >= bot) continue;
        int best = 0; float bd = SETUP_TILE_PX * 0.5f + 12;
        for (int m = row * SETUP_TILE_N; m < s_morphs && m < (row + 1) * SETUP_TILE_N; m++) {
            int tx, ty; morph_tile(m, &tx, &ty);
            const float d = fabsf(x - (tx + SETUP_TILE_W * 0.5f));
            if (d < bd) { bd = d; best = SETUP_HIT_MORPH0 + m; }
        }
        return best;
    }
    return 0;
}
static void morph_tap(tank_t *t, int page, int fish, int id) {   /* a tile, or ROLL - one of the pack's morphs */
    if (id == SETUP_HIT_ROLL) { morph_set(t, fish, (int)tank_randf(t, 0, s_morphs - 0.001f)); tank_emit(TEV_WHEEL_TICK, -1); }
    else if (id >= SETUP_HIT_MORPH0 && id < SETUP_HIT_MORPH0 + s_morphs) morph_set(t, fish, id - SETUP_HIT_MORPH0);
}
/* NOW and GROWN UP on plates - a lizard on stage walks the floor, under the tiles - then the pack's morphs as named tiles
   under a panel band, the hatchling's ringed. No row label: the title says MORPH, and on the round glass a label crowded
   the NOW caption over it */
static void morph_page(const tank_t *t, uint16_t *fb, int stride, double clock, int page, int fish) {
    const int CX = PAGE_W / 2;
    const fish_t *f = &t->fish[fish];
    const species_t *sp = tank_species(t);
    const int cur = lizard_morph_index(sp, f);
    setup_kit_text_c(fb, stride, CX, SETUP_Y + 7, 2, C_CAPT, fish ? "THE SECOND LIZARD'S MORPH" : "THE FIRST LIZARD'S MORPH");
    setup_kit_nav(fb, stride, true, "NEXT", false);
    render_button(fb, stride, SETUP_ROLL_X, SETUP_TOP_BTN_Y, SETUP_TOP_BTN_W, SETUP_BTN_H, C_KEY, C_DIM, "ROLL", 2);
    int x, y; morph_tile(s_morphs - 1, &x, &y);
    const int band = SETUP_ED_CAPTION_Y - 6;
    render_rect_blend(fb, stride, -PAGE_X, band, TANK_W, y + SETUP_FORM_TILE_H + 10 - band, C_PANEL, 110);
    for (int k = 0; k < 2; k++) {                                   /* NOW (the hatchling as it is), GROWN UP */
        const int px = k ? SETUP_ED_GROWN_X : SETUP_ED_NOW_X;
        render_rect_blend(fb, stride, px - 72, SETUP_ED_PREVIEW_Y - 26, 144, 54, C_PANEL, 150);
        if (k) render_fish_preview(fb, stride, px, SETUP_ED_PREVIEW_Y, SETUP_ED_GROWN_SIZE, f->color, f->fin, f->accent, f, FISH_ROOM_CARD, clock);
        else render_fish_portrait(fb, stride, px, SETUP_ED_PREVIEW_Y, SETUP_ED_GROWN_SIZE, f, FISH_ROOM_CARD, clock);
    }
    setup_kit_text_c(fb, stride, (SETUP_ED_NOW_X + SETUP_ED_GROWN_X) / 2, SETUP_ED_PREVIEW_Y - 10, 3, C_CAPT, ">");
    setup_kit_text_c(fb, stride, SETUP_ED_NOW_X, SETUP_ED_CAPTION_Y, 2, C_CAPT, "NOW");
    setup_kit_text_c(fb, stride, SETUP_ED_GROWN_X, SETUP_ED_CAPTION_Y, 2, C_CAPT, "GROWN UP");
    for (int m = 0; m < s_morphs; m++) {
        morph_tile(m, &x, &y);
        const bool on = m == cur;
        const uint32_t body = LOOK_COLORS[lizard_morph_body(sp, m)];
        render_rect(fb, stride, x, y, SETUP_TILE_W, SETUP_FORM_TILE_H, C_KEY);
        render_rect_edge(fb, stride, x, y, SETUP_TILE_W, SETUP_FORM_TILE_H, on ? C_TEXT : C_DIM);
        if (on) render_rect_edge(fb, stride, x + 1, y + 1, SETUP_TILE_W - 2, SETUP_FORM_TILE_H - 2, C_TEXT);
        render_fish_preview(fb, stride, x + SETUP_TILE_W / 2.0f, y + 21, 1.0f, body, tank_fin_for(body), f->accent, f, FISH_ROOM_ROW, clock);
        setup_kit_text_c(fb, stride, x + SETUP_TILE_W / 2, y + SETUP_FORM_TILE_H - 17, 2, on ? C_TEXT : setup_kit_dim(C_CAPT, 60), sp->morphs[m].name);
    }
    setup_kit_dots(fb, stride, PAGE_H - 14);
}
static const setup_page_t MORPH_PAGE = { morph_hit, morph_tap, morph_page };

/* a lizard shows its body colour's gene alone, as its morph (render_lizard.h): a new morph is news, a new fin or pattern
   gene draws nothing on it and is not */
static bool mutation_words(const tank_t *t, const fish_t *f, char *out, size_t n) {
    const species_t *sp = tank_species(t); const int m = lizard_morph_index(sp, f);
    if (f->mutated - 1 != GENE_BODY || m < 0) return false;
    snprintf(out, n, "SOMETHING NEW: %s", sp->morphs[m].name);
    return true;
}

/* WELCOME names its pack under a grown one in the first hatchling's morph: the bin offers one species for now (PLAN P5),
   so it is shown, not asked - the morphs are the keeper's to pick next */
static void welcome_art(const tank_t *t, uint16_t *fb, int stride, double clock) {
    const int CX = PAGE_W / 2;
    const fish_t *f0 = &t->fish[0];
    render_fish_preview(fb, stride, CX, SETUP_Y + 80, 1.3f, f0->color, f0->fin, f0->accent, f0, FISH_ROOM_CARD, clock);
    setup_kit_text_c(fb, stride, CX, SETUP_Y + 106, 2, C_TEXT, tank_species(t)->name);
}
static const char *const WELCOME[] = {
    "TWO HATCHLINGS HAVE MOVED IN.",
    "THEY HUNT, BASK, REST AND",
    "GROW UP WHILE YOU WATCH.",
    "EACH ONE THINKS FOR ITSELF.",
    "FIRST, LET'S MEET THEM.",
};
static const char *const CARE[] = {
    "TAP THE TOP TO DROP CRICKETS",
    "HOLD A FINGER AND THEY VISIT",
    "TAP A LIZARD TO CHECK ON IT",
    "DRAG THE GLASS TO WIPE IT",
    "KEEP THE HUMID HIDE MISTED",
};

/* a lizard walks the floor, under the wheel's band: the one being named is on plates instead - NOW and GROWN UP side by
   side while it is young (as the MORPH page), itself alone once grown */
static void name_preview(const fish_t *f, bool young, uint16_t *fb, int stride, double clock) {
    const int CX = PAGE_W / 2;
    const int px = young ? SETUP_ED_NOW_X : CX;
    render_rect_blend(fb, stride, px - 64, SETUP_NAME_PREVIEW_Y - 24, 128, 48, C_PANEL, 150);
    render_fish_portrait(fb, stride, px, SETUP_NAME_PREVIEW_Y, SETUP_NAME_GROWN_SIZE, f, FISH_ROOM_FAM, clock);
}

/* a lizard shows one gene, its body colour's, as its MORPH (render_lizard.h): that row alone */
static bool family_rows(const tank_t *t, const fish_t *f, uint16_t *fb, int stride) {
    bool hidden = false;
    const species_t *sp = tank_species(t); const int m = lizard_morph_index(sp, f), y = SETUP_FAM_ROW_Y;
    render_text(fb, stride, SETUP_FAM_LABEL_X, y, 2, C_CAPT, "MORPH");
    render_text(fb, stride, SETUP_FAM_VALUE_X, y, 2, C_TEXT, m >= 0 ? sp->morphs[m].name : "?");
    if (f->mutated == GENE_BODY + 1) render_text(fb, stride, SETUP_FAM_FROM_X, y, 2, C_NEW, "NEW!");
    else {
        setup_parent_t p = setup_kit_parent_of(t, f, (f->from_b >> GENE_BODY & 1) ? 1 : 0);
        hidden = f->from_carried >> GENE_BODY & 1;
        setup_kit_text_named(fb, stride, SETUP_FAM_FROM_X, y, "", p.name, p.known ? p.color : C_DIM, hidden ? " *" : NULL);
    }
    return hidden;
}

/* the placement page: a decoration (P5b) is small, on the floor line - its stripe runs to its feet, no chevron over it, the
   line above it - and keeps clear of the hides (tank_deco_slide: a hide, the dish, a ramp's foot or the basking spot is never
   covered). The terrarium has no other piece to place */
static void place(const tank_t *t, int item, int z, setup_place_t *out) {
    const bool deco = tank_deco_index(item) >= 0;
    out->hint = deco ? "IT KEEPS CLEAR OF THE HIDES" : "";
    out->top = tank_decor_fy(t, item) - tank_decor_height(item);   /* its highest pixel on its depth's floor line (0.3.9) */
    out->low = deco;
}

const setup_world_t terrarium_setup = {
    .skip = SETUP_SKIP(SETUP_PG_FORM_A) | SETUP_SKIP(SETUP_PG_FORM_B) | SETUP_SKIP(SETUP_PG_BUBBLES),   /* (so no .form: a lizard's form is its pack's) */
    .begin = begin,
    .colors = &MORPH_PAGE,
    .mutation_words = mutation_words,
    .welcome_title_y = 24, .welcome_lines_y = 140,
    .welcome_art = welcome_art,
    .welcome_lines = WELCOME, .n_welcome_lines = (int)(sizeof WELCOME / sizeof *WELCOME),
    .name_first = "NAME THE FIRST LIZARD", .name_second = "NAME THE SECOND LIZARD", .name_birth = "NAME THE HATCHLING",
    .name_rename = "RENAME YOUR LIZARD",
    .name_preview = name_preview,
    .care_lines = CARE, .n_care_lines = (int)(sizeof CARE / sizeof *CARE),
    .born_title = "A NEW HATCHLING!", .born_parents = "%s AND %s'S EGG",
    .born_shop = "SET DOWN ON THE SAND.",   /* (a staged one hatches in the humid hide, a bought one is set on the floor) */
    .born_staged = "HATCHED IN THE HUMID HIDE.",
    .family_rows = family_rows,
    .place = place,
};
