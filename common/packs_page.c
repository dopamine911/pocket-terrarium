/* packs_page.c - THE PACKS PAGE: see packs_page.h and docs/card-library.md. One layout (the functions below the constants) is
 * the source of the drawing, the taps and the tests' targets, so they cannot disagree. */
#include "packs_page.h"

#if PT_CARD
#include <stdio.h>
#include <string.h>
#include "render_core.h"
#include "packs/library.h"
#include "progression.h"
#include "species.h"
#include "habitat.h"

/* ---- the layout, in PAGE space (render.h: the settings page's own grid, so it sits inside the glass on all four boards) ---- */
#define PP_ROWS_MAX 8
#define PP_LX       (PAGE_BOWL ? 46 : SET_LABEL_X)             /* the bowl: the tabs' top corners (y 48) are 37 px in from the page's edge on the circle, and the foot's buttons keep their own */
#define PP_RX       (SET_SEG_X + SET_SPAN_W - (PAGE_BOWL ? 8 : 0))
#define PP_TAB_Y    (SET_TITLE_Y + 34)
#define PP_TAB_H    30
#define PP_LIST_Y   (PP_TAB_Y + PP_TAB_H + 8)
#define PP_PAGER_H  26
#define PP_LIST_END (SET_FOOT_Y - 8 - PP_PAGER_H - 4)       /* the pager row sits between the list and the foot */
#define PP_ROW_H    44
#define PP_BTN_W    104
#define PP_BTN_H    34
#define PP_ARW_W    36
#define PP_SEP      0x1c2f36
#define PP_OK       0x7fe0a8                                 /* a pack that loaded / the message of a thing that worked */
#define PP_BAD      0xffc94a                                 /* NOT LOADED: the warning's amber */

typedef struct {
    lib_work_t w;
    char       why[PP_ROWS_MAX][LIB_WHY_MAX];                /* INSTALLED tab: why a visible pack is NOT LOADED ("" when it is) */
} pp_mem_t;

enum { PEND_NONE = 0, PEND_SCAN, PEND_INSTALL, PEND_REMOVE };
enum { CONF_NONE = 0, CONF_REMOVE, CONF_UPDATE };

static pp_mem_t *s_m;
static tank_t   *s_tank;
static int       s_tab, s_page, s_conf, s_pend;
static int       s_row;                                      /* the confirm's / the pending install's row: an index into the tab's rows */
static uint32_t  s_uid;                                      /* the confirm's / the pending removal's pack */
static bool      s_scanned, s_drawn, s_changed;
static char      s_msg[LIB_WHY_MAX + 32];
static char      s_scan_why[LIB_WHY_MAX];

void pp_bind_tank(tank_t *t) { s_tank = t; }
bool pp_up(void) { return s_m != NULL; }
const char *pp_message(void) { return s_msg; }
bool pp_take_changed(void) { bool c = s_changed; s_changed = false; return c; }
int  pp_tab(void) { return s_tab; }
bool pp_confirming(void) { return s_conf != CONF_NONE; }
bool pp_busy(void) { return s_pend != PEND_NONE; }
bool pp_scanned(void) { return s_scanned; }

static void say(const char *s) { snprintf(s_msg, sizeof s_msg, "%s", s); }

int pp_rows_per_page(void) {
    int n = (PP_LIST_END - PP_LIST_Y) / PP_ROW_H;
    return n > PP_ROWS_MAX ? PP_ROWS_MAX : n;
}
int pp_rows_total(void) {
    if (!s_m) return 0;
    return s_tab == PP_TAB_INSTALLED ? library_installed_count() : (s_scanned ? s_m->w.n : 0);
}
int pp_page_count(void) { int t = pp_rows_total(), p = pp_rows_per_page(); return t <= 0 ? 1 : (t + p - 1) / p; }
int pp_page_no(void) { return s_page; }
int pp_rows_shown(void) {
    int left = pp_rows_total() - s_page * pp_rows_per_page(), p = pp_rows_per_page();
    return left < 0 ? 0 : left > p ? p : left;
}

/* INSTALLED tab: why each pack on view is not loaded (read once per change, not per frame) */
static void refresh_why(void) {
    if (!s_m || s_tab != PP_TAB_INSTALLED) return;
    for (int i = 0; i < PP_ROWS_MAX; i++) s_m->why[i][0] = 0;
    for (int i = 0, n = pp_rows_shown(); i < n; i++) {
        const lib_inst_t *p = library_installed_at(s_page * pp_rows_per_page() + i);
        if (p && !p->loaded && !library_installed_why(&s_m->w, p, s_m->why[i], LIB_WHY_MAX)) s_m->why[i][0] = 0;
        if (p && !p->loaded && !s_m->why[i][0]) snprintf(s_m->why[i], LIB_WHY_MAX, "NOT LOADED");
    }
}
static void clamp_page(void) {
    int pc = pp_page_count();
    if (s_page >= pc) s_page = pc - 1;
    if (s_page < 0) s_page = 0;
}
static void goto_tab(int tab) { s_tab = tab; s_page = 0; s_conf = CONF_NONE; refresh_why(); }

bool pp_open(void) {
    if (s_m) return true;
    s_m = card_mem_alloc(sizeof *s_m);
    if (!s_m) return false;
    s_tab = PP_TAB_INSTALLED; s_page = 0; s_conf = CONF_NONE; s_pend = PEND_NONE;
    s_scanned = false; s_drawn = false; s_changed = false; s_msg[0] = 0; s_scan_why[0] = 0;
    refresh_why();
    return true;
}
void pp_close(void) {
    if (!s_m) return;
    card_mem_free(s_m); s_m = NULL;
    s_scanned = false; s_pend = PEND_NONE; s_conf = CONF_NONE;
}

/* ---- one row's words ---- */
static const char *kind_word(int k) { return k == PTPK_KIND_SPECIES ? "SPECIES" : k == PTPK_KIND_HABITAT ? "HABITAT" : k == PTPK_KIND_MODEL ? "MODEL" : "PACK"; }
typedef struct { char name[PACK_NAME_MAX + 8]; char detail[LIB_WHY_MAX + 24]; const char *btn; uint32_t uid; uint32_t rgb; } row_t;
static void row_at(int idx, row_t *r) {        /* idx: the row's number in the whole tab */
    memset(r, 0, sizeof *r); r->rgb = MSP_QUIET;
    if (s_tab == PP_TAB_INSTALLED) {
        const lib_inst_t *p = library_installed_at(idx);
        if (!p) return;
        snprintf(r->name, sizeof r->name, "%s", p->name[0] ? p->name : "UNNAMED");
        r->uid = p->uid; r->btn = "REMOVE";
        int slot = idx - s_page * pp_rows_per_page();
        if (p->loaded) snprintf(r->detail, sizeof r->detail, "%s", kind_word(p->kind));
        else { snprintf(r->detail, sizeof r->detail, "NOT LOADED: %s", slot >= 0 && slot < PP_ROWS_MAX ? s_m->why[slot] : ""); r->rgb = PP_BAD; }
        return;
    }
    const lib_row_t *w = &s_m->w.row[idx];
    snprintf(r->name, sizeof r->name, "%s", w->name[0] ? w->name : w->file);
    r->uid = w->uid;
    switch (w->status) {
    case LIB_INSTALLABLE: snprintf(r->detail, sizeof r->detail, "NEW %s", kind_word(w->kind)); r->btn = "INSTALL"; r->rgb = PP_OK; break;
    case LIB_INSTALLED:   snprintf(r->detail, sizeof r->detail, "%s - INSTALLED", kind_word(w->kind)); r->btn = "REMOVE"; break;
    case LIB_UPDATE:      snprintf(r->detail, sizeof r->detail, "%s - ANOTHER VERSION IS INSTALLED", kind_word(w->kind)); r->btn = "UPDATE"; r->rgb = PP_OK; break;
    case LIB_BUILTIN:     snprintf(r->detail, sizeof r->detail, "%s - BUILT IN", kind_word(w->kind)); break;
    case LIB_NO_MODULE:   snprintf(r->detail, sizeof r->detail, "NEEDS: %s", w->why); r->rgb = PP_BAD; break;
    default:              snprintf(r->detail, sizeof r->detail, "REFUSED: %s", w->why); r->rgb = PP_BAD; break;
    }
}
static void fit(char *s, int scale, int maxw) {   /* cut s to maxw pixels */
    for (int n = (int)strlen(s); n > 0 && text_w(s, scale) > maxw; ) s[--n] = 0;
}

/* ---- the buttons' rectangles, in page space ---- */
typedef struct { int x, y, w, h; } rect_t;
static rect_t r_tab(int t) { int w = (PP_RX - PP_LX - 8) / 2; return (rect_t){ PP_LX + t * (w + 8), PP_TAB_Y, w, PP_TAB_H }; }
static rect_t r_row_btn(int i) { return (rect_t){ PP_RX - PP_BTN_W, PP_LIST_Y + i * PP_ROW_H + (PP_ROW_H - PP_BTN_H) / 2, PP_BTN_W, PP_BTN_H }; }
static rect_t r_scan(void) { return (rect_t){ SET_UPD_X, SET_FOOT_Y, SET_UPD_W, MSP_CLOSE_H }; }
static rect_t r_back(void) { return (rect_t){ SET_CLOSE_X, SET_FOOT_Y, MSP_CLOSE_W, MSP_CLOSE_H }; }
#define PAGER_Y (PP_LIST_END + 2)
static rect_t r_next(void) { return (rect_t){ PP_RX - PP_ARW_W, PAGER_Y, PP_ARW_W, PP_PAGER_H }; }
static rect_t r_prev(void) { return (rect_t){ PP_RX - PP_ARW_W - 6 - 54 - 6 - PP_ARW_W, PAGER_Y, PP_ARW_W, PP_PAGER_H }; }
static rect_t r_no(void) { return r_scan(); }
static rect_t r_yes(void) { return r_back(); }
static bool in(rect_t r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static bool scan_shown(void) { return s_tab == PP_TAB_CARD && s_conf == CONF_NONE; }
static bool pager_shown(void) { return s_conf == CONF_NONE && pp_page_count() > 1; }

void pp_box(int *x, int *y, int *w, int *h) {
    int y0 = SET_TITLE_Y, y1 = SET_FOOT_Y + MSP_CLOSE_H;
    if (x) *x = PAGE_X + PP_LX;
    if (y) *y = PAGE_Y + y0;
    if (w) *w = PP_RX - PP_LX;
    if (h) *h = y1 - y0;
}
bool pp_target(int what, int idx, float *x, float *y) {
    rect_t r; bool ok = false;
    if (!s_m) return false;
    switch (what) {
    case PP_WHAT_TAB_INSTALLED: r = r_tab(0); ok = s_conf == CONF_NONE && !s_pend; break;
    case PP_WHAT_TAB_CARD:      r = r_tab(1); ok = s_conf == CONF_NONE && !s_pend; break;
    case PP_WHAT_SCAN:          r = r_scan(); ok = scan_shown() && !s_pend; break;
    case PP_WHAT_BACK:          r = r_back(); ok = s_conf == CONF_NONE && !s_pend; break;
    case PP_WHAT_PREV:          r = r_prev(); ok = pager_shown() && s_page > 0; break;
    case PP_WHAT_NEXT:          r = r_next(); ok = pager_shown() && s_page < pp_page_count() - 1; break;
    case PP_WHAT_ROW: {
        r = r_row_btn(idx < 0 ? 0 : idx);
        row_t row;
        ok = s_conf == CONF_NONE && !s_pend && idx >= 0 && idx < pp_rows_shown();
        if (ok) { row_at(s_page * pp_rows_per_page() + idx, &row); ok = row.btn != NULL; }
        break; }
    case PP_WHAT_YES: r = r_yes(); ok = s_conf != CONF_NONE && !s_pend; break;
    case PP_WHAT_NO:  r = r_no();  ok = s_conf != CONF_NONE && !s_pend; break;
    default: return false;
    }
    if (!ok) return false;
    if (x) *x = PAGE_X + r.x + r.w / 2.0f;
    if (y) *y = PAGE_Y + r.y + r.h / 2.0f;
    return true;
}

const char *pp_row_button(int i) {
    static row_t r;
    if (!s_m || i < 0 || i >= pp_rows_shown()) return NULL;
    row_at(s_page * pp_rows_per_page() + i, &r);
    return r.btn;
}
bool pp_row_text(int i, char *name, size_t nn, char *detail, size_t dn) {
    row_t r;
    if (!s_m || i < 0 || i >= pp_rows_shown()) return false;
    row_at(s_page * pp_rows_per_page() + i, &r);
    if (name && nn) snprintf(name, nn, "%s", r.name);
    if (detail && dn) snprintf(detail, dn, "%s", r.detail);
    return true;
}

/* ---- what a tank lives on ---- */
static bool lives_on(uint32_t uid) {
    if (!s_tank || s_tank->packs_missing || !uid) return false;
    return tank_species(s_tank)->uid == uid || tank_habitat(s_tank)->uid == uid;
}

bool pp_confirm_lives(void) { return s_m && s_conf != CONF_NONE && lives_on(s_uid); }

/* ---- the tap ---- */
static void ask_remove(uint32_t uid, int row) {
    char why[LIB_WHY_MAX];
    if (!library_removable(uid, why, sizeof why)) { say(why); return; }
    s_uid = uid; s_row = row; s_conf = CONF_REMOVE;
}
int pp_tap(float x, float y) {
    if (!s_m || s_pend) return PP_NONE;
    x -= PAGE_X; y -= PAGE_Y;
    if (s_conf != CONF_NONE) {
        if (in(r_no(), x, y)) { s_conf = CONF_NONE; return PP_TICK; }
        if (in(r_yes(), x, y)) {
            s_pend = s_conf == CONF_REMOVE ? PEND_REMOVE : PEND_INSTALL; s_conf = CONF_NONE; s_drawn = false;
            return PP_TICK;
        }
        return PP_NONE;
    }
    if (in(r_back(), x, y)) return PP_BACK;
    if (in(r_tab(0), x, y)) { if (s_tab != PP_TAB_INSTALLED) { goto_tab(PP_TAB_INSTALLED); return PP_TICK; } return PP_NONE; }
    if (in(r_tab(1), x, y)) { if (s_tab != PP_TAB_CARD) { goto_tab(PP_TAB_CARD); return PP_TICK; } return PP_NONE; }
    if (scan_shown() && in(r_scan(), x, y)) { s_pend = PEND_SCAN; s_drawn = false; return PP_TICK; }
    if (pager_shown() && in(r_prev(), x, y) && s_page > 0) { s_page--; refresh_why(); return PP_TICK; }
    if (pager_shown() && in(r_next(), x, y) && s_page < pp_page_count() - 1) { s_page++; refresh_why(); return PP_TICK; }
    for (int i = 0, n = pp_rows_shown(); i < n; i++) {
        if (!in(r_row_btn(i), x, y)) continue;
        int idx = s_page * pp_rows_per_page() + i;
        row_t row; row_at(idx, &row);
        if (!row.btn) return PP_NONE;
        if (!strcmp(row.btn, "REMOVE")) { ask_remove(row.uid, idx); return PP_TICK; }
        s_row = idx; s_uid = row.uid;
        if (!strcmp(row.btn, "UPDATE")) { s_conf = CONF_UPDATE; return PP_TICK; }
        s_pend = PEND_INSTALL; s_drawn = false;                   /* INSTALL: nothing to lose, no question */
        return PP_TICK;
    }
    return PP_NONE;
}

/* ---- the blocking calls ---- */
static void rows_after_install(int i, uint32_t uid) {   /* the scan's rows say what is true now, as far as the card's own files can tell */
    for (int j = 0; j < s_m->w.n; j++) {
        lib_row_t *r = &s_m->w.row[j];
        if (r->uid != uid || (r->status != LIB_INSTALLABLE && r->status != LIB_UPDATE && r->status != LIB_INSTALLED)) continue;
        r->status = j == i ? LIB_INSTALLED : LIB_UPDATE;
    }
}
static void rows_after_remove(uint32_t uid) {
    for (int j = 0; j < s_m->w.n; j++) {
        lib_row_t *r = &s_m->w.row[j];
        if (r->uid == uid && (r->status == LIB_INSTALLED || r->status == LIB_UPDATE)) r->status = LIB_INSTALLABLE;
    }
}
void pp_tick(float dt) {
    (void)dt;
    if (!s_m || s_pend == PEND_NONE || !s_drawn) return;   /* a frame that says what is happening goes first */
    int what = s_pend; s_pend = PEND_NONE;
    if (what == PEND_SCAN) {
        s_scan_why[0] = 0;
        if (!library_scan(&s_m->w, s_scan_why, sizeof s_scan_why)) { s_scanned = false; s_m->w.n = 0; say(s_scan_why[0] ? s_scan_why : "CARD NOT READABLE"); }
        else {
            s_scanned = true;
            if (s_m->w.more) snprintf(s_msg, sizeof s_msg, "%d LISTED: THE CARD HAS MORE", s_m->w.n);
            else if (s_m->w.n == 0) say("NO PACKS ON THE CARD");
            else snprintf(s_msg, sizeof s_msg, "%d PACK%s ON THE CARD", s_m->w.n, s_m->w.n == 1 ? "" : "S");
        }
        s_page = 0; clamp_page();
        return;
    }
    char msg[LIB_WHY_MAX + 32];
    if (what == PEND_INSTALL) {
        lib_row_t *r = &s_m->w.row[s_row];
        uint32_t uid = r->uid;
        if (r->status == LIB_UPDATE && s_tank && lives_on(uid)) progression_pack_leaving(s_tank, uid);   /* BEFORE the registry changes */
        lib_result_t lr = library_install(&s_m->w, s_row, msg, sizeof msg);
        if (s_tank) progression_pack_arrived(s_tank);       /* (a tank that waits for it resumes; one paused for the old version when the update failed, too) */
        say(msg);
        if (lr == LIB_DONE_INSTALLED || lr == LIB_DONE_UPDATED) { rows_after_install(s_row, uid); s_changed = true; }
        else if (lr == LIB_DONE_SAME) rows_after_install(s_row, uid);
    } else {
        uint32_t uid = s_uid; char why[LIB_WHY_MAX];
        const lib_inst_t *p = library_installed_uid(uid);
        char nm[PACK_NAME_MAX]; snprintf(nm, sizeof nm, "%s", p ? p->name : "PACK");
        bool left = s_tank && lives_on(uid) && progression_pack_leaving(s_tank, uid);   /* BEFORE the registry changes */
        if (library_remove(uid, why, sizeof why)) { snprintf(s_msg, sizeof s_msg, "REMOVED %s", nm); rows_after_remove(uid); s_changed = true; }
        else { if (left && s_tank) progression_pack_arrived(s_tank); say(why); }
    }
    clamp_page(); refresh_why();
}

/* ---- the drawing ---- */
static void text_mid(ctx_t *c, int y, int scale, uint32_t rgb, const char *s) { draw_text(c, (PAGE_W - text_w(s, scale)) / 2, y, scale, rgb, s); }
static void btn_lit(ctx_t *c, rect_t r, const char *label) {
    button(c, r.x, r.y, r.w, r.h, MSP_TEAL, MSP_TEAL, "", 2);
    draw_text(c, r.x + (r.w - text_w(label, 2)) / 2, r.y + (r.h - 14) / 2, 2, MSP_INK, label);
}
static void btn_out(ctx_t *c, rect_t r, const char *label, uint32_t rgb) {
    button(c, r.x, r.y, r.w, r.h, 0x1c2f36, rgb, "", 2);
    draw_text(c, r.x + (r.w - text_w(label, 2)) / 2, r.y + (r.h - 14) / 2, 2, rgb == MSP_TEAL ? 0xffffff : rgb, label);
}
static void draw_confirm(ctx_t *c) {
    const bool upd = s_conf == CONF_UPDATE;
    char name[PACK_NAME_MAX + 8] = "";
    if (upd) snprintf(name, sizeof name, "%s", s_m->w.row[s_row].name[0] ? s_m->w.row[s_row].name : s_m->w.row[s_row].file);
    else { const lib_inst_t *p = library_installed_uid(s_uid); snprintf(name, sizeof name, "%s", p && p->name[0] ? p->name : "THIS PACK"); }
    text_mid(c, SET_TITLE_Y, 3, 0xffffff, upd ? "UPDATE?" : "REMOVE?");
    int y = SET_TITLE_Y + 50;
    fit(name, 2, PP_RX - PP_LX); text_mid(c, y, 2, MSP_TEAL, name); y += 34;
    if (lives_on(s_uid)) {
        text_mid(c, y, 2, PP_BAD, "YOUR TANK LIVES ON IT."); y += 24;
        text_mid(c, y, 2, 0xffffff, "IT WILL WAIT, PAUSED,"); y += 22;
        text_mid(c, y, 2, 0xffffff, "UNTIL THE PACK IS BACK."); y += 30;
    }
    if (upd) { text_mid(c, y, 2, MSP_QUIET, "THE OLD VERSION IS REPLACED."); }
    else     { text_mid(c, y, 2, MSP_QUIET, "THE PACK GOES FROM THE BOARD;"); y += 22; text_mid(c, y, 2, MSP_QUIET, "THE CARD IS NOT TOUCHED."); }
    btn_out(c, r_no(), "CANCEL", MSP_TEAL);
    btn_lit(c, r_yes(), upd ? "UPDATE" : "REMOVE");
}
void pp_render(uint16_t *fb, int stride) {
    if (!s_m) return;
    ctx_t c = ctx_page(fb, stride);
    rect_fill(&c, -PAGE_X, -PAGE_Y, TANK_W, TANK_H, MSP_INK);
    if (s_pend) {
        const char *w = s_pend == PEND_REMOVE ? "REMOVING" : "READING THE CARD";
        text_mid(&c, SET_TITLE_Y, 3, 0xffffff, "PACKS");
        text_mid(&c, (PP_LIST_Y + PP_LIST_END) / 2 - 7, 2, MSP_TEAL, w);
        s_drawn = true;
        return;
    }
    if (s_conf != CONF_NONE) { draw_confirm(&c); return; }
    char t[48];
    text_mid(&c, SET_TITLE_Y, 3, 0xffffff, "PACKS");
    snprintf(t, sizeof t, "INSTALLED %d", library_installed_count());
    for (int k = 0; k < 2; k++) {
        rect_t r = r_tab(k);
        const char *lab = k == 0 ? t : "CARD";
        if (k == s_tab) btn_lit(&c, r, lab); else btn_out(&c, r, lab, MSP_TEAL);
    }
    const int tw = PP_RX - PP_LX - PP_BTN_W - 10;
    for (int i = 0, n = pp_rows_shown(); i < n; i++) {
        row_t row; row_at(s_page * pp_rows_per_page() + i, &row);
        int ry = PP_LIST_Y + i * PP_ROW_H;
        fit(row.name, 2, tw); fit(row.detail, 1, tw);
        draw_text(&c, PP_LX, ry + 4, 2, 0xffffff, row.name);
        draw_text(&c, PP_LX, ry + 26, 1, row.rgb, row.detail);
        if (row.btn) btn_out(&c, r_row_btn(i), row.btn, MSP_TEAL);
        rect_fill(&c, PP_LX, ry + PP_ROW_H - 1, PP_RX - PP_LX, 1, PP_SEP);
    }
    if (pp_rows_total() == 0) {
        if (s_tab == PP_TAB_INSTALLED) { text_mid(&c, PP_LIST_Y + 14, 2, MSP_QUIET, "NO PACKS INSTALLED"); text_mid(&c, PP_LIST_Y + 38, 2, MSP_QUIET, "TAP CARD, THEN SCAN"); }
        else if (!s_scanned) { text_mid(&c, PP_LIST_Y + 14, 2, MSP_QUIET, "TAP SCAN TO READ"); text_mid(&c, PP_LIST_Y + 38, 2, MSP_QUIET, "THE CARD"); }
    }
    int mw = (pager_shown() ? r_prev().x : PP_RX) - PP_LX - 8;
    if (s_msg[0]) {
        char m[LIB_WHY_MAX + 32]; snprintf(m, sizeof m, "%s", s_msg);
        int sc = text_w(m, 2) <= mw ? 2 : 1;
        fit(m, sc, mw);
        draw_text(&c, PP_LX, PAGER_Y + (sc == 2 ? 6 : 9), sc, PP_OK, m);
    }
    if (pager_shown()) {
        char pg[32]; snprintf(pg, sizeof pg, "%d/%d", s_page + 1, pp_page_count());
        rect_t a = r_prev(), b = r_next();
        btn_out(&c, a, "<", s_page > 0 ? MSP_TEAL : PP_SEP);
        btn_out(&c, b, ">", s_page < pp_page_count() - 1 ? MSP_TEAL : PP_SEP);
        draw_text(&c, a.x + a.w + 6 + (54 - text_w(pg, 2)) / 2, PAGER_Y + 6, 2, MSP_QUIET, pg);
    }
    if (scan_shown()) btn_lit(&c, r_scan(), "SCAN");
    btn_out(&c, r_back(), "BACK", MSP_TEAL);
}
#endif
