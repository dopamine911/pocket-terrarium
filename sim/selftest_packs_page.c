#define _DEFAULT_SOURCE   /* mkdir, setenv under -std=c11 (glibc) */
#define _XOPEN_SOURCE 700
/* selftest_packs_page.c - issue #4 phase 4: THE PACKS PAGE on this board (common/packs_page.c, docs/card-library.md "The PACKS page").
 * The page is driven the way a finger does it - the platform's adv_touch / adv_tick / adv_render under the ADVANCED page - over the
 * real library, a card that is a folder (POCKET_CARD_DIR) and a flash that is a file (POCKET_TANK_PACKSTORE). The library's own
 * contract is --selftest-library; this holds what the page adds.
 *
 *   ./fishsim --selftest-packs-page          --card-shot PREFIX   (the page's states as PPMs, PREFIX_<state>.ppm)
 *
 *   1  the door: ADVANCED's foot has PACKS only when the board has a card slot (no slot: no button, the tap on its place does
 *      nothing); the tap opens the page on INSTALLED; BACK returns to ADVANCED; closing ADVANCED closes the page
 *   2  the glass: every pixel the page draws - empty, installed, a scan with one of every verdict, paged, the questions (one about
 *      the tank's own pack), the busy frame - is inside this board's glass and the page's box
 *   3  a request is drawn before it is done: a tap on SCAN / INSTALL / yes changes nothing until a frame that says what is happening
 *      was drawn and the next tick runs; one ADV_ACT_PACKS per change, none for a scan
 *   4  INSTALL (no question), UPDATE and REMOVE (the question first; CANCEL changes nothing), the button each row has by its verdict
 *      (none for built in, refused, no module), the rows following the work, the INSTALLED tab and a pack that is NOT LOADED
 *   5  a tank that lives on the pack: the question says so, REMOVE pauses it (PACK MISSING), INSTALL from the card resumes it,
 *      UPDATE keeps it going; a tank that does not is left alone
 *   6  paging: more rows than the glass holds - PREV / NEXT only where there is somewhere to go, the page after a removal
 *   7  words: no card, no folder, an empty folder, a card with more files than it lists */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tank.h"
#include "progression.h"
#include "species.h"
#include "habitat.h"
#include "advanced.h"
#include "render.h"
#include "packs/ptpk.h"
#include "packs/builtin.h"
#include "packs/library.h"
#include "packs/pstore.h"
#include "packs/caps.h"
#include "packs_page.h"
#include "card.h"
#include "sim_tmp.h"
#include "packs_fixture.h"

#if PT_CARD
int shelf_port_sim_cut(int writes);                          /* persist_port_sim.c */

static const char *s_name = "selftest-packs-page";
#define PF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static uint16_t FB[TANK_W * TANK_H];
static tank_t s_t;                                           /* (tank_t is big: never on the stack) */
static char s_card[700], s_packs[760], s_store[700];
static pf_blob_t S0, H0, M0;                                 /* this world's default species, habitat and model manifest, as files */

/* ================================================================ plumbing */
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; remove(p); return 0; }
static void rm_rf(const char *dir) { nftw(dir, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static void fresh(void) {
    adv_close();
    shelf_port_sim_cut(-1);
    remove(s_store);
    rm_rf(s_card);
    mkdir(s_card, 0700); mkdir(s_packs, 0700);
    setenv("POCKET_CARD_DIR", s_card, 1);
    packs_drop_loaded();
    library_boot();
    tank_init(&s_t, 7);                                      /* a tank on the built-in pair, bound to the page */
    adv_bind_tank(&s_t);
}
static void upper2(char *up, const char *tail) { memset(up, 0, 4); for (int i = 0; i < 2 && tail[i]; i++) up[i] = tail[i] >= 'a' && tail[i] <= 'z' ? (char)(tail[i] - 32) : tail[i]; }
static void mk_species(pf_blob_t *o, const char *tail) { char up[4]; upper2(up, tail); pf_variant(o, &S0, tail, up); }
static bool card_put(const char *name, const pf_blob_t *b) { return pf_write_file(s_packs, name, b->b, b->n); }

/* the page as the platform drives it */
static void frame(void) { adv_render(FB, TANK_W); adv_tick(1.0f / 30); }                       /* one platform frame: draw, then the tick */
static void settle(void) { for (int k = 0; k < 4; k++) frame(); }
static int  adv_tap_xy(float x, float y) { adv_touch(x, y, true, 0); return adv_touch(x, y, false, 0); }
static int  tap(int what, int idx) { float x, y; if (!pp_target(what, idx, &x, &y)) return -1; return adv_tap_xy(x, y); }
static float packs_btn_x(void) { return PAGE_X + SET_UPD_X + SET_UPD_W / 2.0f; }
static float packs_btn_y(void) { return PAGE_Y + SET_FOOT_Y + MSP_CLOSE_H / 2.0f; }
static void adv_to_page(void) { adv_open(); adv_touch(0, 0, false, 0); adv_render(FB, TANK_W); }   /* the chord's finger lifts */
static bool open_page(void) { adv_to_page(); return adv_tap_xy(packs_btn_x(), packs_btn_y()) == ADV_ACT_TICK && adv_page() == ADV_PG_PACKS && pp_up(); }
/* a request through to its result: the tap, the frame that says what it is doing, the tick's work, the result adv_touch hands out.
   -1: the tap was not a request; -2: it was not busy after; -3: it was still busy after its frame. Else ADV_ACT_PACKS or ADV_ACT_NONE */
static int run_request(int what, int idx) {
    if (tap(what, idx) != ADV_ACT_TICK) return -1;
    if (!pp_busy()) return -2;
    frame();
    if (pp_busy()) return -3;
    return adv_touch(0, 0, false, 0);
}
static int scan_page(void) { return tap(PP_WHAT_TAB_CARD, 0) == ADV_ACT_TICK || pp_tab() == PP_TAB_CARD ? run_request(PP_WHAT_SCAN, 0) : -9; }
static int shown_with(const char *name_part, const char *button) {                              /* the row on view whose name has name_part and whose button is `button` (NULL: none) */
    for (int i = 0; i < pp_rows_shown(); i++) {
        char n[64]; const char *b = pp_row_button(i);
        if (!pp_row_text(i, n, sizeof n, NULL, 0) || (name_part && !strstr(n, name_part))) continue;
        if (button ? (b && !strcmp(b, button)) : !b) return i;
    }
    return -1;
}
static int count_buttons(const char *button) { int n = 0; for (int i = 0; i < pp_rows_shown(); i++) { const char *b = pp_row_button(i); if (b && !strcmp(b, button)) n++; } return n; }
static bool detail_has(int i, const char *s) { char d[160]; return pp_row_text(i, NULL, 0, d, sizeof d) && strstr(d, s); }
static lib_work_t *work(void) { static lib_work_t *w; if (!w) w = card_mem_alloc(sizeof *w); return w; }
static bool install_direct(const pf_blob_t *b) {
    char m[160];
    const bool ok = library_add(work(), b->b, b->n, m, sizeof m) == LIB_DONE_INSTALLED;
    if (!ok) printf("install_direct: %s\n", m);
    return ok;
}

/* ---- the glass ---- */
static bool in_glass(int x, int y) {
#if defined(TANK_ROUND)
    const float dx = x + 0.5f - TANK_W / 2.0f, dy = y + 0.5f - TANK_H / 2.0f, r = TANK_RAD - 1;   /* the glass: tank.h TANK_RAD, a pixel of margin */
    return dx * dx + dy * dy <= r * r;
#elif defined(TANK_CORNER_R)
    const float r = TANK_CORNER_R;
    const float cx = x < r ? r : x > TANK_W - 1 - r ? TANK_W - 1 - r : (float)x, cy = y < r ? r : y > TANK_H - 1 - r ? TANK_H - 1 - r : (float)y;
    const float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (r - 2) * (r - 2);
#else
    (void)x; (void)y; return true;
#endif
}
static int s_glass_states;
static int check_glass(const char *what) {
    int bx, by, bw, bh; pp_box(&bx, &by, &bw, &bh);
    const uint16_t bg = FB[0];
    int outside = 0, fx = -1, fy = -1, drawn = 0;
    for (int y = 0; y < TANK_H; y++) for (int x = 0; x < TANK_W; x++) {
        if (FB[y * TANK_W + x] == bg) continue;
        drawn++;
        if (in_glass(x, y) && x >= bx && x < bx + bw && y >= by && y < by + bh) continue;
        if (!outside++) { fx = x; fy = y; }
    }
    if (!drawn) PF("%s: nothing was drawn", what);
    if (outside) PF("%s: %d drawn pixels are outside the glass or the page's box (first at %d,%d; box %d,%d %dx%d)", what, outside, fx, fy, bx, by, bw, bh);
    s_glass_states++;
    return 0;
}

/* a card with one of everything and `installed` packs already in the flash: the installed files stay on the card (INSTALLED rows), and
   a built-in, a refused file, two new packs and - when something is installed - another version of the first */
static int build_everything(int installed) {
    pf_blob_t b, bi;
    fresh();
    char m[160];
    for (int i = 0; i < installed; i++) { char t[3] = { 'i', (char)('a' + i), 0 }; char nm[16]; snprintf(nm, sizeof nm, "inst%d.ptpk", i); mk_species(&b, t); card_put(nm, &b); if (!install_direct(&b)) return 1; }
    pf_builtin(PTPK_KIND_SPECIES, &bi); card_put("builtin.ptpk", &bi);
    pf_write_file(s_packs, "junk.ptpk", "not a pack at all, not at all, not", 34);
    for (int i = 0; i < 2; i++) { char t[3] = { 'n', (char)('a' + i), 0 }; char nm[16]; snprintf(nm, sizeof nm, "new%d.ptpk", i); mk_species(&b, t); card_put(nm, &b); }
    if (installed > 0) { pf_blob_t a, u; mk_species(&a, "ia"); pf_variant(&u, &a, "", "V2"); card_put("upd.ptpk", &u); }
    (void)m;
    return 0;
}

/* ================================================================ 1: the door */
static int t_door(void) {
    fresh();
    unsetenv("POCKET_CARD_DIR");
    if (card_slot()) PF("a slot with POCKET_CARD_DIR unset");
    adv_to_page();
    if (render_advanced_tap(packs_btn_x(), packs_btn_y()) == ADV_HIT_PACKS) PF("a PACKS button on a board with no card slot");
    if (adv_tap_xy(packs_btn_x(), packs_btn_y()) != ADV_ACT_NONE || adv_page() != ADV_PG_MAIN || pp_up()) PF("a tap where PACKS would be did something with no slot");
    adv_close();
    setenv("POCKET_CARD_DIR", s_card, 1);
    if (!card_slot()) PF("no slot with POCKET_CARD_DIR set");
    adv_to_page();
    if (render_advanced_tap(packs_btn_x(), packs_btn_y()) != ADV_HIT_PACKS) PF("no PACKS button where the foot's left slot is");
    if (render_advanced_tap(PAGE_X + SET_CLOSE_X + MSP_CLOSE_W / 2.0f, packs_btn_y()) != ADV_HIT_BACK) PF("BACK moved");
    adv_close();
    if (!open_page()) PF("the PACKS tap did not open the page");
    if (pp_tab() != PP_TAB_INSTALLED || pp_confirming() || pp_busy()) PF("the page did not open on INSTALLED, quiet");
    if (tap(PP_WHAT_BACK, 0) != ADV_ACT_TICK || adv_page() != ADV_PG_MAIN || pp_up()) PF("BACK did not return to ADVANCED and free the page");
    if (!open_page()) PF("the page did not open a second time");
    adv_close();
    if (pp_up() || adv_up()) PF("closing ADVANCED left the page up");
    printf("selftest-packs-page: the door: PACKS only with a card slot, opens on INSTALLED, BACK returns, closing ADVANCED frees the page\n");
    return 0;
}

/* ================================================================ 2: the glass */
static int t_glass(void) {
    if (build_everything(0)) PF("fixture");
    if (!open_page()) PF("open");
    settle(); if (check_glass("INSTALLED, empty")) return 1;
    if (build_everything(3)) PF("fixture with 3 installed");
    if (!open_page()) PF("open");
    settle(); if (check_glass("INSTALLED, some")) return 1;
    tap(PP_WHAT_TAB_CARD, 0); settle();
    if (check_glass("CARD, not scanned")) return 1;
    if (run_request(PP_WHAT_SCAN, 0) < 0) PF("scan did not run");
    settle(); if (check_glass("CARD, scanned")) return 1;
    if (pp_page_count() > 1) { tap(PP_WHAT_NEXT, 0); settle(); if (check_glass("CARD, second page")) return 1; tap(PP_WHAT_PREV, 0); }
    tap(PP_WHAT_SCAN, 0); adv_render(FB, TANK_W);
    if (check_glass("the busy frame")) return 1;
    adv_tick(1.0f / 30);
    int u = shown_with(NULL, "UPDATE");
    if (u < 0 && pp_page_count() > 1) { tap(PP_WHAT_NEXT, 0); u = shown_with(NULL, "UPDATE"); }
    if (u < 0) PF("no UPDATE row in a scan with another version of an installed pack");
    tap(PP_WHAT_ROW, u);
    if (!pp_confirming()) PF("UPDATE did not ask");
    adv_render(FB, TANK_W); if (check_glass("the UPDATE question")) return 1;
    tap(PP_WHAT_NO, 0);
    tap(PP_WHAT_TAB_INSTALLED, 0); tap(PP_WHAT_ROW, 0);
    if (!pp_confirming()) PF("REMOVE did not ask");
    adv_render(FB, TANK_W); if (check_glass("the REMOVE question")) return 1;
    tap(PP_WHAT_NO, 0);
    {   /* the longest question: the tank lives on the pack */
        const lib_inst_t *p = library_installed_at(0); char why[160];
        const species_t *sp = p ? species_by_uid(p->uid) : NULL;
        if (!sp || !tank_set_packs(&s_t, HABITAT_DEFAULT, sp->id, why, sizeof why)) PF("a tank refused an installed species: %s", why);
        tap(PP_WHAT_ROW, 0);
        if (!pp_confirm_lives()) PF("the question does not know the tank lives on the pack");
        adv_render(FB, TANK_W); if (check_glass("the REMOVE question about the tank's own pack")) return 1;
        tap(PP_WHAT_NO, 0);
        tap(PP_WHAT_TAB_CARD, 0); tap(PP_WHAT_NEXT, 0); adv_render(FB, TANK_W);
        int uu = shown_with(NULL, "UPDATE"); if (uu >= 0) { tap(PP_WHAT_ROW, uu); adv_render(FB, TANK_W); if (check_glass("the UPDATE question about the tank's own pack")) return 1; }
    }
    adv_close();
    printf("selftest-packs-page: the glass: every drawn pixel of the page - %d states, the questions and the busy frame among them - is inside this board's glass and the page's box (%d rows a page)\n", s_glass_states, pp_rows_per_page());
    return 0;
}

/* ================================================================ 3 + 4: requests, install, update, remove */
static int t_flow(void) {
    pf_blob_t a, b, u;
    fresh();
    mk_species(&a, "fa"); card_put("fa.ptpk", &a);
    mk_species(&b, "fb"); card_put("fb.ptpk", &b);
    { pf_blob_t bi; pf_builtin(PTPK_KIND_SPECIES, &bi); card_put("builtin.ptpk", &bi); }
    pf_write_file(s_packs, "junk.ptpk", "not a pack at all, not at all, not", 34);
    if (!open_page()) PF("open");
    if (tap(PP_WHAT_SCAN, 0) != -1) PF("SCAN is offered on the INSTALLED tab");
    if (tap(PP_WHAT_TAB_CARD, 0) != ADV_ACT_TICK || pp_tab() != PP_TAB_CARD) PF("the CARD tab");
    if (pp_scanned() || pp_rows_total() != 0) PF("rows before a SCAN");
    /* a request is drawn before it is done */
    if (tap(PP_WHAT_SCAN, 0) != ADV_ACT_TICK || !pp_busy()) PF("SCAN did not become a request");
    for (int k = 0; k < 5; k++) adv_tick(1.0f / 30);
    if (!pp_busy() || pp_scanned()) PF("SCAN ran before a frame that says so was drawn");
    if (tap(PP_WHAT_BACK, 0) != -1 || tap(PP_WHAT_TAB_INSTALLED, 0) != -1) PF("the buttons answer while a request waits");
    adv_render(FB, TANK_W);
    adv_tick(1.0f / 30);
    if (pp_busy() || !pp_scanned()) PF("SCAN did not run after its frame");
    if (adv_touch(0, 0, false, 0) != ADV_ACT_NONE) PF("a scan changes no pack, yet ADV_ACT_PACKS");
    if (pp_rows_total() != 4) PF("the scan has %d rows, wanted 4 (two good, a built-in, a junk file)", pp_rows_total());
    if (pp_page_count() == 1) {
        if (count_buttons("INSTALL") != 2 || count_buttons("REMOVE") != 0 || count_buttons("UPDATE") != 0) PF("buttons: %d INSTALL, %d REMOVE, %d UPDATE for two new packs", count_buttons("INSTALL"), count_buttons("REMOVE"), count_buttons("UPDATE"));
        int bi = -1, ju = -1;
        for (int i = 0; i < pp_rows_shown(); i++) { if (detail_has(i, "BUILT IN")) bi = i; if (detail_has(i, "REFUSED")) ju = i; }
        if (bi < 0 || pp_row_button(bi)) PF("a built-in pack's row: missing or has a button");
        if (ju < 0 || pp_row_button(ju)) PF("a refused file's row: missing or has a button");
    }
    /* INSTALL: no question; the request, its frame, the work, the one result */
    const int fa = shown_with("FA", "INSTALL");
    if (fa < 0 && pp_page_count() == 1) PF("no INSTALL for fa");
    if (fa >= 0) {
        if (run_request(PP_WHAT_ROW, fa) != ADV_ACT_PACKS) PF("INSTALL: no ADV_ACT_PACKS (\"%s\")", pp_message());
        if (library_installed_count() != 1 || strstr(pp_message(), "INSTALLED") == NULL) PF("INSTALL: %d installed, \"%s\"", library_installed_count(), pp_message());
        if (adv_touch(0, 0, false, 0) != ADV_ACT_NONE) PF("one change gave ADV_ACT_PACKS twice");
        const int now = shown_with("FA", "REMOVE");
        if (now != fa || !detail_has(now, "INSTALLED")) PF("the installed file's row is not REMOVE / INSTALLED after the install");
        if (tap(PP_WHAT_ROW, now) != ADV_ACT_TICK || !pp_confirming()) PF("REMOVE on the CARD tab did not ask");
        if (tap(PP_WHAT_NO, 0) != ADV_ACT_TICK || pp_confirming() || library_installed_count() != 1) PF("CANCEL changed something");
    }
    /* UPDATE: another file of that pack. Asks first; CANCEL changes nothing; yes replaces, once */
    pf_variant(&u, &a, "", "V2"); card_put("fa2.ptpk", &u);
    if (scan_page() != ADV_ACT_NONE) PF("a rescan changed packs");
    const int up = shown_with("V2", "UPDATE");
    if (up < 0) PF("no UPDATE row for another version of the installed pack");
    if (tap(PP_WHAT_ROW, up) != ADV_ACT_TICK || !pp_confirming() || pp_busy()) PF("UPDATE did not ask");
    if (tap(PP_WHAT_NO, 0) != ADV_ACT_TICK || library_installed_count() != 1 || strstr(library_installed_at(0)->name, "V2")) PF("CANCEL of UPDATE changed the pack");
    tap(PP_WHAT_ROW, up);
    if (run_request(PP_WHAT_YES, 0) != ADV_ACT_PACKS || library_installed_count() != 1 || !strstr(library_installed_at(0)->name, "V2") || strstr(pp_message(), "UPDATED") == NULL)
        PF("UPDATE yes: %d installed, \"%s\", \"%s\"", library_installed_count(), library_installed_at(0)->name, pp_message());
    if (shown_with("V2", "REMOVE") < 0 && pp_page_count() == 1) PF("after the update that file is not the installed one");
    /* the INSTALLED tab: the pack and its REMOVE; yes removes, the CARD rows follow */
    tap(PP_WHAT_TAB_INSTALLED, 0);
    if (pp_rows_total() != 1 || !detail_has(0, "SPECIES") || strcmp(pp_row_button(0), "REMOVE")) PF("INSTALLED tab: %d rows", pp_rows_total());
    if (tap(PP_WHAT_ROW, 0) != ADV_ACT_TICK || !pp_confirming()) PF("REMOVE did not ask on INSTALLED");
    if (run_request(PP_WHAT_YES, 0) != ADV_ACT_PACKS || library_installed_count() != 0 || strstr(pp_message(), "REMOVED") == NULL) PF("REMOVE yes: %d installed, \"%s\"", library_installed_count(), pp_message());
    if (pp_rows_total() != 0) PF("INSTALLED still lists a removed pack");
    tap(PP_WHAT_TAB_CARD, 0);
    if (shown_with("V2", "INSTALL") < 0 && shown_with("V2", "UPDATE") < 0 && pp_page_count() == 1) PF("the removed pack's file is not offered again");
    /* a pack that is NOT LOADED: its habitat gone from the flash, the board rebooted */
    adv_close();
    { pf_blob_t h, s; char up[4]; upper2(up, "xh");
      pf_variant(&h, &H0, "xh", up); mk_species(&s, "xs"); { uint32_t hu = pf_uid(&h); pf_set_habitats(&s, &hu, 1); }
      if (!install_direct(&h) || !install_direct(&s)) PF("a habitat and a species that lives in it did not install");
      if (!pstore_remove(pf_uid(&h))) PF("could not take the habitat out of the flash"); }
    packs_drop_loaded(); library_boot();
    if (!open_page()) PF("open");
    { char d[160] = ""; if (pp_rows_total() == 1) pp_row_text(0, NULL, 0, d, sizeof d);
      if (pp_rows_total() != 1 || strncmp(d, "NOT LOADED: ", 12) != 0 || strlen(d) <= 12) PF("a stored pack that cannot load: %d rows, detail \"%s\" (wanted NOT LOADED: and the loader's reason)", pp_rows_total(), d); }
    settle(); if (check_glass("NOT LOADED")) return 1;
    adv_close();
    printf("selftest-packs-page: a request is drawn before it is done (the buttons wait); INSTALL, UPDATE, REMOVE through the page - the question first, CANCEL changes nothing - one ADV_ACT_PACKS per change; the buttons follow each row's verdict; NOT LOADED shown\n");
    return 0;
}

/* ================================================================ 5: a tank that lives on the pack */
static int t_tank(void) {
    pf_blob_t a, u;
    char why[200];
    fresh();
    mk_species(&a, "ta"); card_put("ta.ptpk", &a);
    pf_variant(&u, &a, "", "U2");
    if (!open_page() || scan_page() != ADV_ACT_NONE) PF("open and scan");
    if (run_request(PP_WHAT_ROW, shown_with("TA", "INSTALL")) != ADV_ACT_PACKS) PF("INSTALL: \"%s\"", pp_message());
    const uint32_t uid = pf_uid(&a);
    const species_t *sp = species_by_uid(uid);
    if (!sp || !tank_set_packs(&s_t, HABITAT_DEFAULT, sp->id, why, sizeof why)) PF("the tank refused the installed species: %s", why);
    /* a tank that lives elsewhere is left alone */
    const int other_id = SPECIES_DEFAULT;
    {   pf_blob_t o; mk_species(&o, "tb"); if (!install_direct(&o)) PF("a second pack"); }
    tank_set_packs(&s_t, HABITAT_DEFAULT, other_id, why, sizeof why);
    tap(PP_WHAT_TAB_INSTALLED, 0);
    { int ib = -1; for (int i = 0; i < pp_rows_shown(); i++) { char n[64]; pp_row_text(i, n, sizeof n, NULL, 0); if (strstr(n, "TB")) ib = i; }
      if (ib < 0) PF("the second pack is not on INSTALLED");
      tap(PP_WHAT_ROW, ib);
      if (pp_confirm_lives()) PF("the question says the tank lives on a pack it does not");
      if (run_request(PP_WHAT_YES, 0) != ADV_ACT_PACKS || s_t.packs_missing) PF("removing a pack the tank does not live on paused it"); }
    /* a tank that lives on it: the question says so; REMOVE pauses it; INSTALL from the card resumes it */
    if (!tank_set_packs(&s_t, HABITAT_DEFAULT, species_by_uid(uid)->id, why, sizeof why)) PF("the tank refused the pack again: %s", why);
    { int ia = -1; for (int i = 0; i < pp_rows_shown(); i++) { char n[64]; pp_row_text(i, n, sizeof n, NULL, 0); if (strstr(n, "TA")) ia = i; }
      if (ia < 0) PF("the first pack is not on INSTALLED");
      tap(PP_WHAT_ROW, ia); }
    if (!pp_confirm_lives()) PF("the question does not say the tank lives on the pack");
    adv_render(FB, TANK_W); if (check_glass("the question about the tank's own pack")) return 1;
    if (run_request(PP_WHAT_YES, 0) != ADV_ACT_PACKS) PF("REMOVE: \"%s\"", pp_message());
    if (!s_t.packs_missing || s_t.pack_id != SPECIES_DEFAULT) PF("the tank did not wait (PACK MISSING) when the pack it lives on was removed");
    const double clock0 = s_t.clock;
    for (int k = 0; k < 300; k++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (s_t.clock != clock0) PF("a tank whose pack is gone moved on");
    if (scan_page() != ADV_ACT_NONE) PF("rescan");
    if (run_request(PP_WHAT_ROW, shown_with("TA", "INSTALL")) != ADV_ACT_PACKS) PF("INSTALL again: \"%s\"", pp_message());
    if (s_t.packs_missing || tank_species(&s_t)->uid != uid) PF("the tank did not resume at once on the reinstalled pack (missing %d)", s_t.packs_missing);
    /* UPDATE of the pack it lives on: the question says so; the tank keeps going on the new version */
    card_put("ta2.ptpk", &u);
    if (scan_page() != ADV_ACT_NONE) PF("rescan");
    const int up = shown_with("U2", "UPDATE");
    if (up < 0) PF("no UPDATE row");
    tap(PP_WHAT_ROW, up);
    if (!pp_confirm_lives()) PF("the UPDATE question does not say the tank lives on the pack");
    if (run_request(PP_WHAT_YES, 0) != ADV_ACT_PACKS) PF("UPDATE: \"%s\"", pp_message());
    if (s_t.packs_missing || !strstr(tank_species(&s_t)->name, "U2")) PF("the tank did not come back on the updated pack (\"%s\", missing %d)", tank_species(&s_t)->name, s_t.packs_missing);
    adv_close();
    printf("selftest-packs-page: a tank on the pack: the question says so; REMOVE pauses it, INSTALL resumes it at once, UPDATE keeps it on the new version; a tank elsewhere is left alone\n");
    return 0;
}

/* ================================================================ 6: paging */
static int t_paging(void) {
    fresh();
    if (!open_page()) PF("open");
    const int per = pp_rows_per_page(), total = per + 1;
    if (total > PSTORE_SECTORS) PF("a glass of %d rows a page cannot be paged with a store of %d sectors", per, PSTORE_SECTORS);
    for (int i = 0; i < total; i++) { pf_blob_t b; char t[3] = { 'p', (char)('a' + i), 0 }; mk_species(&b, t); if (!install_direct(&b)) PF("pack %d did not install", i); }
    settle();
    if (pp_rows_total() != total || pp_page_count() != 2 || pp_page_no() != 0 || pp_rows_shown() != per) PF("page 1 of %d packs: %d rows total, %d pages, page %d, %d shown (a glass holds %d)", total, pp_rows_total(), pp_page_count(), pp_page_no(), pp_rows_shown(), per);
    if (tap(PP_WHAT_PREV, 0) != -1) PF("PREV is offered on the first page");
    settle(); if (check_glass("the first page, full")) return 1;
    if (tap(PP_WHAT_NEXT, 0) != ADV_ACT_TICK || pp_page_no() != 1 || pp_rows_shown() != 1) PF("NEXT: page %d, %d rows shown", pp_page_no(), pp_rows_shown());
    if (tap(PP_WHAT_NEXT, 0) != -1) PF("NEXT is offered on the last page");
    settle(); if (check_glass("the last page")) return 1;
    if (tap(PP_WHAT_ROW, 0) != ADV_ACT_TICK || !pp_confirming()) PF("REMOVE on the last page did not ask");
    if (run_request(PP_WHAT_YES, 0) != ADV_ACT_PACKS) PF("REMOVE: \"%s\"", pp_message());
    if (pp_page_count() != 1 || pp_page_no() != 0 || pp_rows_shown() != per) PF("after removing the last page's only row: %d pages, on page %d, %d shown", pp_page_count(), pp_page_no(), pp_rows_shown());
    if (tap(PP_WHAT_NEXT, 0) != -1 || tap(PP_WHAT_PREV, 0) != -1) PF("a pager on a single page");
    adv_close();
    printf("selftest-packs-page: paging: %d packs on a glass of %d rows - PREV and NEXT only where there is somewhere to go; removing the last page's only row falls back to page 1\n", total, per);
    return 0;
}

/* ================================================================ 7: words */
static int t_words(void) {
    fresh();
    setenv("POCKET_CARD_DIR", sim_tmp("pp-no-such-card"), 1);
    if (!open_page() || scan_page() != ADV_ACT_NONE || pp_scanned() || !strstr(pp_message(), "NO CARD")) PF("no card: \"%s\"", pp_message());
    adv_close();
    setenv("POCKET_CARD_DIR", s_card, 1);
    rm_rf(s_packs);
    if (!open_page() || scan_page() != ADV_ACT_NONE || pp_scanned() || !strstr(pp_message(), "NO PACKS FOLDER")) PF("no folder: \"%s\"", pp_message());
    mkdir(s_packs, 0700);
    if (scan_page() != ADV_ACT_NONE || !pp_scanned() || pp_rows_total() != 0 || !strstr(pp_message(), "NO PACKS")) PF("an empty folder: \"%s\"", pp_message());
    for (int i = 0; i < CARD_LIST_MAX + 8; i++) { char nm[32]; snprintf(nm, sizeof nm, "f%03d.ptpk", i); pf_write_file(s_packs, nm, "x", 1); }
    if (scan_page() != ADV_ACT_NONE || pp_rows_total() != CARD_LIST_MAX || !strstr(pp_message(), "MORE")) PF("a card with more files than it lists: %d rows, \"%s\"", pp_rows_total(), pp_message());
    settle(); if (check_glass("a full listing")) return 1;
    adv_close();
    printf("selftest-packs-page: words: no card, no packs folder, an empty folder, more files than are listed (%d of %d shown)\n", CARD_LIST_MAX, CARD_LIST_MAX + 8);
    return 0;
}

int selftest_packs_page(void) {
    snprintf(s_card, sizeof s_card, "%s", sim_tmp("pp-card"));
    snprintf(s_packs, sizeof s_packs, "%s/%s", s_card, CARD_FOLDER);
    snprintf(s_store, sizeof s_store, "%s", sim_tmp("pp-packstore.bin"));
    setenv("POCKET_TANK_PACKSTORE", s_store, 1);
    packs_drop_loaded();
    if (!pf_builtin(PTPK_KIND_SPECIES, &S0) || !pf_builtin(PTPK_KIND_HABITAT, &H0) || !pf_builtin(PTPK_KIND_MODEL, &M0)) PF("this world's built-in species, habitat or manifest file is not there");
    const int r = t_door() || t_glass() || t_flow() || t_tank() || t_paging() || t_words();
    adv_close();
    unsetenv("POCKET_CARD_DIR");
    packs_drop_loaded(); remove(s_store); library_boot();
    if (work()) { card_mem_free(work()); }
    if (r) return 1;
    printf("selftest-packs-page OK\n");
    return 0;
}

/* ================================================================ --card-shot: the page's states as PPMs */
static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", TANK_W, TANK_H);
    for (int i = 0; i < TANK_W * TANK_H; i++) {
        const uint16_t p = FB_SWAP16(FB[i]);
        const unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void shot(const char *prefix, const char *name) {
    char path[1024]; snprintf(path, sizeof path, "%s_%s.ppm", prefix, name);
    adv_render(FB, TANK_W); write_ppm(path); printf("card-shot: %s\n", path);
}
int packs_page_shot(const char *prefix) {
    snprintf(s_card, sizeof s_card, "%s", sim_tmp("pp-card"));
    snprintf(s_packs, sizeof s_packs, "%s/%s", s_card, CARD_FOLDER);
    snprintf(s_store, sizeof s_store, "%s", sim_tmp("pp-packstore.bin"));
    setenv("POCKET_TANK_PACKSTORE", s_store, 1);
    packs_drop_loaded();
    if (!pf_builtin(PTPK_KIND_SPECIES, &S0) || !pf_builtin(PTPK_KIND_HABITAT, &H0) || !pf_builtin(PTPK_KIND_MODEL, &M0)) return 1;
    if (build_everything(6)) return 1;
    adv_to_page(); shot(prefix, "advanced");
    adv_tap_xy(packs_btn_x(), packs_btn_y()); settle(); shot(prefix, "installed");
    if (tap(PP_WHAT_NEXT, 0) == ADV_ACT_TICK) { settle(); shot(prefix, "installed2"); tap(PP_WHAT_PREV, 0); }
    tap(PP_WHAT_TAB_CARD, 0); settle(); shot(prefix, "card_unscanned");
    tap(PP_WHAT_SCAN, 0); adv_render(FB, TANK_W); shot(prefix, "busy"); adv_tick(1.0f / 30); settle(); shot(prefix, "card");
    if (tap(PP_WHAT_NEXT, 0) == ADV_ACT_TICK) { settle(); shot(prefix, "card2"); }
    tap(PP_WHAT_TAB_INSTALLED, 0); tap(PP_WHAT_ROW, 0); settle(); shot(prefix, "confirm"); tap(PP_WHAT_NO, 0);
    {   const lib_inst_t *p = library_installed_at(0); char why[160];
        const species_t *sp = p ? species_by_uid(p->uid) : NULL;
        if (sp && tank_set_packs(&s_t, HABITAT_DEFAULT, sp->id, why, sizeof why)) { tap(PP_WHAT_ROW, 0); settle(); shot(prefix, "confirm_tank"); tap(PP_WHAT_NO, 0); }
    }
    adv_close(); unsetenv("POCKET_CARD_DIR"); packs_drop_loaded(); remove(s_store); library_boot();
    return 0;
}
#else
int selftest_packs_page(void) { return 0; }
int packs_page_shot(const char *prefix) { (void)prefix; return 0; }
#endif
