/* library.c - the card library (library.h; docs/card-library.md): scan, check, install, load at boot, remove */
#include "library.h"

#if PT_CARD
#include "ptpk_write.h"
#include "compat.h"
#include "caps.h"
#include "manifest.h"
#include "species.h"
#include "habitat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- small helpers ---- */
static void upper(char *dst, size_t cap, const char *src) {   /* the glass draws upper case: every message and reason goes out so */
    size_t i = 0;
    for (; cap && i + 1 < cap && src[i]; i++) { const char c = src[i]; dst[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
    if (cap) dst[i] = 0;
}
static void say(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *dst, size_t cap, const char *fmt, ...) {   /* snprintf then upper case, in place */
    if (!dst || !cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(dst, cap, fmt, ap); va_end(ap);
    upper(dst, cap, dst);
}
static const char *kind_word(int kind) { return kind == PTPK_KIND_SPECIES ? "SPECIES" : kind == PTPK_KIND_HABITAT ? "HABITAT" : kind == PTPK_KIND_MODEL ? "MODEL" : "PACK"; }

/* ---- what is installed ---- */
static lib_inst_t s_inst[PSTORE_SECTORS];
static int        s_n_inst;

static bool in_registry(int kind, uint32_t uid) {              /* a pack of this kind that is not built in is loaded under this uid */
    if (kind == PTPK_KIND_SPECIES) { const species_t *s = species_by_uid(uid); return s && !s->builtin; }
    if (kind == PTPK_KIND_HABITAT) { const habitat_t *h = habitat_by_uid(uid); return h && !h->builtin; }
    const model_manifest_t *m = manifest_by_uid(uid);
    return m && !m->builtin;
}
static void registry_remove(int kind, uint32_t uid) {
    if (kind == PTPK_KIND_SPECIES) species_remove(uid);
    else if (kind == PTPK_KIND_HABITAT) habitat_remove(uid);
    else manifest_remove(uid);
}
/* the NAME record is the first of a pack the encoder or tools/ptpk.py wrote (records in ascending id): read the sector's first bytes
   of body for it, so a pack that is NOT loaded can still be listed by name */
static void peek_name(const pstore_rec_t *r, char *out) {
    uint8_t b[4 + PACK_NAME_MAX];
    out[0] = 0;
    if (pstore_port_read((uint32_t)r->sector * PSTORE_SECTOR_BYTES + PSTORE_HEADER + PTPK_HEADER_BYTES, b, sizeof b)) {
        const unsigned tag = b[0] | b[1] << 8, len = b[2] | b[3] << 8;
        if ((tag & 0x7fffu) == 1 && len >= 1 && len < PACK_NAME_MAX) {
            for (unsigned i = 0; i < len; i++) out[i] = b[4 + i] >= 0x20 && b[4 + i] <= 0x7e ? (char)b[4 + i] : '?';
            out[len] = 0;
        }
    }
    if (!out[0]) snprintf(out, PACK_NAME_MAX, "UID %08x", (unsigned)r->uid);
    upper(out, PACK_NAME_MAX, out);
}
static void rebuild(void) {                                    /* the table from the store's records, in order of sector */
    s_n_inst = 0;
    pstore_rec_t r;
    for (int i = 0; i < PSTORE_SECTORS && pstore_at(i, &r); i++) {
        lib_inst_t *p = &s_inst[s_n_inst++];
        p->uid = r.uid; p->kind = r.kind; p->sector = (uint8_t)r.sector; p->pad = 0;
        peek_name(&r, p->name);
        p->loaded = in_registry(r.kind, r.uid);
    }
}
int library_installed_count(void) { return s_n_inst; }
const lib_inst_t *library_installed_at(int i) {
    if (i < 0 || i >= s_n_inst) return NULL;
    s_inst[i].loaded = in_registry(s_inst[i].kind, s_inst[i].uid);      /* (live: a test's packs_drop_loaded, or a pack loaded since the boot) */
    return &s_inst[i];
}
const lib_inst_t *library_installed_uid(uint32_t uid) {
    for (int i = 0; uid && i < s_n_inst; i++) if (s_inst[i].uid == uid) return library_installed_at(i);
    return NULL;
}
int library_free(void) { return pstore_free(); }

/* ---- the boot ---- */
/* each stored record that is not loaded yet, through the loader (manifests, habitats, species: a species lists habitats by uid and
   names goals the manifests list). buf: PTPK_FILE_MAX bytes of the caller's. How many are loaded after it */
static int load_store(uint8_t *buf, bool log) {
    int loaded = 0;
    char why[LIB_WHY_MAX];
    for (int pass = 0; pass < 3; pass++) {
        const int want = pass == 0 ? PTPK_KIND_MODEL : pass == 1 ? PTPK_KIND_HABITAT : PTPK_KIND_SPECIES;
        pstore_rec_t r;
        for (int i = 0; pstore_at(i, &r); i++) {
            if (r.kind != want) continue;
            if (in_registry(r.kind, r.uid)) { loaded++; continue; }
            const int n = pstore_read(&r, buf, PTPK_FILE_MAX);
            if (n < 0) { if (log) printf("library: %s uid %08x does not read true from the store\n", kind_word(r.kind), (unsigned)r.uid); continue; }
            packs_info_t info; why[0] = 0;
            switch (packs_add(buf, (size_t)n, PACKS_FILE, &info, why, sizeof why)) {
            case PACKS_ADDED: loaded++; if (log) printf("library: %s %s (uid %08x) loaded from the store\n", kind_word(info.kind), info.name, (unsigned)info.uid); break;
            case PACKS_SAME:  loaded++; break;
            case PACKS_REFUSED: if (log) printf("library: %s uid %08x NOT LOADED: %s\n", kind_word(r.kind), (unsigned)r.uid, why); break;
            }
        }
    }
    return loaded;
}
int library_boot(void) {
    packs_init();
    pstore_boot();
    uint8_t file[PTPK_FILE_MAX];                               /* the main task's stack, as packs_load_builtin's buffer */
    const int loaded = load_store(file, true);
    rebuild();
    printf("library: %d pack%s in the store, %d loaded\n", pstore_count(), pstore_count() == 1 ? "" : "s", loaded);
    return loaded;
}

/* a pack not loaded at boot may load now (its habitat or its model's words came in after it) */
static void retry_unloaded(uint8_t *buf) {
    for (int i = 0; i < s_n_inst; i++) if (!in_registry(s_inst[i].kind, s_inst[i].uid)) { load_store(buf, false); break; }
    rebuild();
}

bool library_installed_why(lib_work_t *w, const lib_inst_t *p, char *why, size_t n) {
    if (!p || !w) return false;
    if (in_registry(p->kind, p->uid)) return false;
    pstore_rec_t r;
    char text[LIB_WHY_MAX];
    packs_probe_t pr;
    if (!pstore_find(p->uid, &r)) { say(why, n, "NOT IN THE STORE"); return true; }
    const int len = pstore_read(&r, w->file, sizeof w->file);
    if (len < 0) { say(why, n, "ITS SECTOR DOES NOT READ TRUE"); return true; }
    text[0] = 0;
    if (packs_dry_check(w->file, (size_t)len, &w->scratch, &pr, text, sizeof text) == PACKS_DRY_BAD) say(why, n, "%s", text);
    else say(why, n, "NOT LOADED (LOAD IT AT THE NEXT BOOT)");
    return true;
}

/* ---- the dry check's verdict on one file ---- */
static void row_verdict(lib_row_t *row, packs_dry_t r, const packs_probe_t *pr, const char *why) {
    snprintf(row->name, sizeof row->name, "%s", pr->name);
    row->uid = pr->uid; row->crc = pr->crc; row->kind = (uint8_t)pr->kind; row->needs = 0; row->want = 0; row->why[0] = 0;
    switch (r) {
    case PACKS_DRY_BAD:
        if (pr->no_module) {
            char mods[48]; caps_list(pr->no_module, mods, sizeof mods);
            row->status = LIB_NO_MODULE; row->needs = pr->no_module;
            say(row->why, sizeof row->why, "NEEDS %s, WHICH THIS FIRMWARE LACKS", mods);
        } else if (pr->no_room) {
            row->status = LIB_REFUSED;
            say(row->why, sizeof row->why, "NO ROOM: THE %s POOL IS FULL - REMOVE A PACK FIRST", kind_word(pr->kind));
        } else {
            row->status = LIB_REFUSED; row->want = pr->want_habitat;
            say(row->why, sizeof row->why, "%s", why);
        }
        return;
    case PACKS_DRY_SAME:
        if (pr->held_builtin) { row->status = LIB_BUILTIN; say(row->why, sizeof row->why, "THE FIRMWARE HAS THIS PACK"); }
        else if (pstore_find(pr->uid, NULL)) row->status = LIB_INSTALLED;
        else row->status = pstore_free() > 0 ? LIB_INSTALLABLE : LIB_REFUSED;     /* loaded from elsewhere (the simulator's folder): installing makes it permanent */
        if (row->status == LIB_REFUSED) say(row->why, sizeof row->why, "THE STORE IS FULL: REMOVE A PACK FIRST");
        return;
    case PACKS_DRY_OTHER:
        if (pr->held_builtin) { row->status = LIB_REFUSED; say(row->why, sizeof row->why, "A BUILT-IN PACK HAS THIS UID: ANOTHER FILE CANNOT TAKE IT"); }
        else if (pstore_find(pr->uid, NULL)) { row->status = pstore_free() > 0 ? LIB_UPDATE : LIB_REFUSED; if (row->status == LIB_REFUSED) say(row->why, sizeof row->why, "THE STORE IS FULL: REMOVE A PACK FIRST"); }
        else { row->status = LIB_REFUSED; say(row->why, sizeof row->why, "ANOTHER PACK ALREADY LOADED HAS THIS UID"); }
        return;
    case PACKS_DRY_NEW:
        if (pstore_free() > 0) row->status = LIB_INSTALLABLE;
        else { row->status = LIB_REFUSED; say(row->why, sizeof row->why, "THE STORE IS FULL: REMOVE A PACK FIRST"); }
        return;
    }
}

/* ---- scan ---- */
bool library_scan(lib_work_t *w, char *why, size_t why_n) {
    w->n = 0; w->more = false;
    if (why && why_n) why[0] = 0;
    if (!card_slot()) { if (why && why_n) snprintf(why, why_n, "NO CARD SLOT"); return false; }
    if (!card_open(why, why_n)) { if (why && why_n && !why[0]) snprintf(why, why_n, "NO CARD"); return false; }
    const int cnt = card_list(w->ent, LIB_ROWS_MAX, &w->more, why, why_n);
    if (cnt < 0) { card_close(); if (why && why_n && !why[0]) snprintf(why, why_n, "NO PACKS FOLDER"); return false; }
    for (int i = 0; i < cnt; i++) {
        lib_row_t *row = &w->row[i];
        memset(row, 0, sizeof *row);
        memcpy(row->file, w->ent[i].name, CARD_NAME_MAX); row->file[CARD_NAME_MAX - 1] = 0;   /* (the same size on both sides: no format to truncate) */
        row->size = w->ent[i].size;
        if (!card_name_ok(row->file)) { row->status = LIB_REFUSED; say(row->why, sizeof row->why, "NOT A PLAIN PACK FILE NAME"); w->n = i + 1; continue; }   /* (the port filters them; this is the second lock) */
        if (row->size == 0) { row->status = LIB_REFUSED; say(row->why, sizeof row->why, "EMPTY FILE"); w->n = i + 1; continue; }
        if (row->size > PTPK_FILE_MAX) { row->status = LIB_REFUSED; say(row->why, sizeof row->why, "%u BYTES: A PACK IS AT MOST %d", (unsigned)row->size, PTPK_FILE_MAX); w->n = i + 1; continue; }
        const int got = card_read(row->file, w->file, PTPK_FILE_MAX);
        if (got < 0 || (uint32_t)got != row->size) { row->status = LIB_REFUSED; say(row->why, sizeof row->why, got < 0 ? "COULD NOT BE READ" : "SHORT READ: %d OF %u BYTES", got, (unsigned)row->size); w->n = i + 1; continue; }
        packs_probe_t pr; char why2[LIB_WHY_MAX]; why2[0] = 0;
        const packs_dry_t r = packs_dry_check(w->file, (size_t)got, &w->scratch, &pr, why2, sizeof why2);
        row_verdict(row, r, &pr, why2);
        w->n = i + 1;
    }
    card_close();
    for (int i = 0; i < w->n; i++) {                           /* a species whose habitat is a habitat on this card, not yet installed: say so instead of 'none is loaded' */
        lib_row_t *s = &w->row[i];
        if (s->status != LIB_REFUSED || !s->want) continue;
        for (int j = 0; j < w->n; j++) {
            const lib_row_t *h = &w->row[j];
            if (h->kind == PTPK_KIND_HABITAT && h->uid == s->want && (h->status == LIB_INSTALLABLE || h->status == LIB_UPDATE)) { say(s->why, sizeof s->why, "INSTALL ITS HABITAT FIRST: %s", h->name); break; }
        }
    }
    return true;
}

/* ---- install ---- */
lib_result_t library_add(lib_work_t *w, const uint8_t *file, size_t n, char *msg, size_t msg_n) {
    char why[LIB_WHY_MAX]; why[0] = 0;
    packs_probe_t pr;
    const packs_dry_t r = packs_dry_check(file, n, &w->scratch, &pr, why, sizeof why);
    bool update = false;
    switch (r) {
    case PACKS_DRY_BAD: {
        lib_row_t tmp; row_verdict(&tmp, r, &pr, why);
        say(msg, msg_n, "%s", tmp.why);
        return LIB_FAILED; }
    case PACKS_DRY_SAME:
        if (pr.held_builtin) { say(msg, msg_n, "THE FIRMWARE HAS THIS PACK"); return LIB_FAILED; }
        if (pstore_find(pr.uid, NULL)) { say(msg, msg_n, "%s IS INSTALLED ALREADY", pr.name); return LIB_DONE_SAME; }
        break;                                                 /* loaded from elsewhere: kept in the store below, the registry already has it */
    case PACKS_DRY_OTHER:
        if (pr.held_builtin) { say(msg, msg_n, "A BUILT-IN PACK HAS THIS UID"); return LIB_FAILED; }
        if (!pstore_find(pr.uid, NULL)) { say(msg, msg_n, "ANOTHER PACK ALREADY LOADED HAS THIS UID"); return LIB_FAILED; }
        update = true;
        break;
    case PACKS_DRY_NEW:
        break;
    }
    if (pstore_free() == 0) { say(msg, msg_n, "THE STORE IS FULL: REMOVE A PACK FIRST"); return LIB_FAILED; }
    char name[PACK_NAME_MAX]; snprintf(name, sizeof name, "%s", pr.name);
    /* the registry first (the loader, whole: a pack the dry check passed loads), the store second - and a refusal of the flash puts the
       registry back as it was, an update's old version too (it is still in the store, whole: the new one is written beside it). A cut
       between the two loses only RAM: the store still holds what it held. A file the registry has already (PACKS_DRY_SAME: loaded from
       elsewhere, the simulator's folder) goes into the store alone. w->file is free for the restore: a failing add is done with `file` */
    pstore_rec_t old_rec;
    const bool had_old = update && pstore_find(pr.uid, &old_rec);
    char why2[LIB_WHY_MAX]; why2[0] = 0;
    if (update) registry_remove(pr.kind, pr.uid);
    if (r != PACKS_DRY_SAME && packs_add(file, n, PACKS_FILE, NULL, why, sizeof why) == PACKS_REFUSED) {
        say(msg, msg_n, "%s", why);
        if (had_old) { const int ol = pstore_read(&old_rec, w->file, sizeof w->file); if (ol > 0) packs_add(w->file, (size_t)ol, PACKS_FILE, NULL, why2, sizeof why2); }
        return LIB_FAILED;
    }
    if (!pstore_put(pr.uid, pr.kind, file, n, why2, sizeof why2)) {
        say(msg, msg_n, "%s", why2);
        if (r != PACKS_DRY_SAME) registry_remove(pr.kind, pr.uid);
        if (had_old) { const int ol = pstore_read(&old_rec, w->file, sizeof w->file); if (ol > 0) packs_add(w->file, (size_t)ol, PACKS_FILE, NULL, why2, sizeof why2); }
        return LIB_FAILED;
    }
    rebuild();
    if (pr.kind != PTPK_KIND_SPECIES) retry_unloaded(w->file);   /* a habitat or a manifest may let a stored species load (w->file: the file is in the store now) */
    printf("library: %s %s (uid %08x) %s\n", kind_word(pr.kind), name, (unsigned)pr.uid, update ? "updated" : "installed");
    say(msg, msg_n, "%s %s", update ? "UPDATED" : "INSTALLED", name);
    return update ? LIB_DONE_UPDATED : LIB_DONE_INSTALLED;
}

lib_result_t library_install(lib_work_t *w, int i, char *msg, size_t msg_n) {
    if (i < 0 || i >= w->n) { say(msg, msg_n, "NO SUCH PACK"); return LIB_FAILED; }
    const lib_row_t row = w->row[i];                            /* (a copy: the scan's rows are not touched by the install) */
    if (row.status != LIB_INSTALLABLE && row.status != LIB_UPDATE) { say(msg, msg_n, "%s", row.why[0] ? row.why : "NOTHING TO INSTALL"); return LIB_FAILED; }   /* (a NO MODULE or REFUSED row says why) */
    if (!card_name_ok(row.file)) { say(msg, msg_n, "NOT A PLAIN PACK FILE NAME"); return LIB_FAILED; }
    char why[48]; why[0] = 0;
    if (!card_open(why, sizeof why)) { say(msg, msg_n, "%s", why[0] ? why : "NO CARD"); return LIB_FAILED; }
    const int got = card_read(row.file, w->file, PTPK_FILE_MAX);
    card_close();
    if (got < 0 || (uint32_t)got != row.size) { say(msg, msg_n, "THE CARD CHANGED: SCAN AGAIN"); return LIB_FAILED; }
    ptpk_header_t h;
    if (!ptpk_open(w->file, (size_t)got, &h, NULL, 0) || h.uid != row.uid || h.crc != row.crc) { say(msg, msg_n, "THE CARD CHANGED: SCAN AGAIN"); return LIB_FAILED; }
    return library_add(w, w->file, (size_t)got, msg, msg_n);
}

/* ---- remove ---- */
/* what depends on an installed pack: a habitat the only home an installed species has left, a manifest whose words an installed species
   names and no other manifest lists. The first such species' name, or NULL */
static const char *dependent_of(int kind, uint32_t uid, bool *by_goal) {
    for (int i = 0; i < species_count(); i++) {
        const species_t *s = species_at(i);
        if (s->builtin || !pstore_find(s->uid, NULL)) continue;
        if (kind == PTPK_KIND_HABITAT) {
            bool lists = false; int others = 0;
            for (int k = 0; k < s->n_habitat_uids && k < SPECIES_HABITATS_MAX; k++) {
                if (s->habitat_uid[k] == uid) lists = true;
                else if (habitat_by_uid(s->habitat_uid[k])) others++;
            }
            if (lists && !others) { *by_goal = false; return s->name; }
        } else if (kind == PTPK_KIND_MODEL) {
            const model_manifest_t *m = manifest_by_uid(uid);
            for (int g = 0; m && g < s->n_goals && g < SPECIES_GOALS_MAX; g++) {
                if (!manifest_has_goal(m, s->goal[g])) continue;
                bool elsewhere = false;
                for (int j = 0; j < manifest_count() && !elsewhere; j++) { const model_manifest_t *o = manifest_at(j); elsewhere = o->uid != uid && manifest_has_goal(o, s->goal[g]); }
                if (!elsewhere) { *by_goal = true; return s->name; }
            }
        }
    }
    return NULL;
}
bool library_removable(uint32_t uid, char *why, size_t why_n) {
    pstore_rec_t r;
    if (!pstore_find(uid, &r)) { say(why, why_n, "NOT INSTALLED FROM THE CARD"); return false; }
    bool goal = false;
    const char *dep = dependent_of(r.kind, uid, &goal);
    if (dep) { say(why, why_n, goal ? "%s NEEDS ITS GOAL WORDS: REMOVE IT FIRST" : "%s LIVES IN IT: REMOVE IT FIRST", dep); return false; }
    return true;
}
bool library_remove(uint32_t uid, char *why, size_t why_n) {
    if (!library_removable(uid, why, why_n)) return false;
    pstore_rec_t r;
    pstore_find(uid, &r);
    char name[PACK_NAME_MAX]; peek_name(&r, name);
    if (!pstore_remove(uid)) { say(why, why_n, "THE FLASH REFUSED IT"); return false; }     /* (nothing else has changed: the pack is still loaded and listed) */
    registry_remove(r.kind, uid);
    rebuild();
    printf("library: %s %s (uid %08x) removed\n", kind_word(r.kind), name, (unsigned)uid);
    return true;
}
#endif
