/* library.h - THE CARD LIBRARY: list the packs on the microSD card, check them before use, install them into flash, keep them
 * (issue #4 phase 4, docs/card-library.md). Platform-neutral: the card is card.h's port, the flash packs/pstore.h's.
 *
 *   library_scan     one visit to the card: every /packs/NAME.ptpk is read (one at a time, into the caller's block), run through the
 *                    loader's own dry check (ptpk.h packs_dry_check - nothing is committed, no pool slot is taken) and given a
 *                    VERDICT: INSTALLABLE, INSTALLED (that very file is installed), UPDATE (a pack installed from the card has
 *                    this uid; this is another file), BUILT IN (the file of a pack the firmware ships), NO MODULE (a good pack
 *                    this firmware has no movement module for: it names which) or REFUSED (the loader's reason).
 *   library_install  a second visit reads that one file again (the card may have been changed since the scan: its size and CRC
 *                    must still be the scan's), runs the check once more, copies the file into the store and loads it through
 *                    packs_add(PACKS_FILE) so it is usable at once.
 *   library_boot     at every boot, after packs_init: what is in the store is loaded through the SAME loader, manifests and
 *                    habitats first. A pack removed from the card keeps working; one that no longer reads true is dropped; one
 *                    that no longer loads (its habitat was removed) is kept and listed NOT LOADED, never an error.
 *   library_remove   a pack out of the store and the registry. A tank that lives on it is the page's to warn and pause
 *                    (progression.h progression_pack_leaving): it becomes PACK MISSING (docs/ptpk.md) and resumes if the pack
 *                    is installed again. A built-in pack cannot be removed; a habitat or a model manifest an installed species
 *                    depends on is refused until that species is removed.
 *
 * Safety (the files are a stranger's): a name that is not a plain *.ptpk is never listed (card.h card_name_ok); a file over
 * PTPK_FILE_MAX is refused by its size before a byte is read; the PTPK validation is the loader's, whole; at most
 * CARD_LIST_MAX files are listed and the page says when there were more; a hostile pack never crashes anything - it is a
 * REFUSED row. Built only when PT_CARD. */
#ifndef LIBRARY_H
#define LIBRARY_H
#include "world.h"

#if PT_CARD
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "card.h"
#include "ptpk.h"
#include "pstore.h"

#define LIB_ROWS_MAX CARD_LIST_MAX
#define LIB_WHY_MAX  96                              /* a reason, upper case, cut to fit */

typedef enum {
    LIB_INSTALLABLE,        /* a good pack that is not installed: INSTALL */
    LIB_INSTALLED,          /* that very file (its CRC) is installed: REMOVE */
    LIB_UPDATE,             /* a good pack whose uid an installed pack has, in another file: UPDATE */
    LIB_BUILTIN,            /* the file of a pack the firmware ships: nothing to do */
    LIB_NO_MODULE,          /* a good pack this firmware has no module for: `needs` names it (caps.h) */
    LIB_REFUSED             /* `why` says why: it is not a pack, it is damaged, it breaks a rule, no room, ... */
} lib_status_t;

typedef struct {
    char     file[CARD_NAME_MAX];   /* its name on the card (a plain *.ptpk) */
    char     name[PACK_NAME_MAX];   /* the pack's own NAME ("" when the file did not get that far) */
    uint32_t uid, crc;              /* the file's header (0 when it did not open) */
    uint32_t size;                  /* the file's bytes on the card */
    uint8_t  kind;                  /* PTPK_KIND_* (0: not told) */
    uint8_t  status;                /* lib_status_t */
    caps_t   needs;                 /* LIB_NO_MODULE: the movement modules it needs and this firmware lacks */
    uint32_t want;                  /* a species refused for lack of a habitat: the first it lists (the scan's 'install its habitat first' reads it) */
    char     why[LIB_WHY_MAX];      /* LIB_REFUSED / LIB_NO_MODULE / LIB_INSTALLABLE-but-for: the reason, upper case */
} lib_row_t;

/* the library's working area while a scan or an install runs - the PACKS page allocates one with card_mem_alloc when it opens
 * (10460 B, measured, in PSRAM on the board) and frees it when it closes: nothing here is .bss and nothing sits on a task's stack */
typedef struct {
    lib_row_t       row[LIB_ROWS_MAX];
    card_entry_t    ent[LIB_ROWS_MAX];  /* the listing the rows are made from */
    int             n;              /* rows filled by the last scan */
    bool            more;           /* the card holds more packs than LIB_ROWS_MAX: the rest are not listed */
    uint8_t         file[PTPK_FILE_MAX + 1];   /* the file in hand */
    packs_scratch_t scratch;        /* the dry check's species / habitat / manifest */
} lib_work_t;

/* a pack in the store, as the boot (and every install and remove since) left it */
typedef struct {
    uint32_t uid;
    uint8_t  kind;                  /* PTPK_KIND_* */
    uint8_t  loaded;                /* it is in the registry now */
    uint8_t  sector;                /* where it lives in the store */
    uint8_t  pad;
    char     name[PACK_NAME_MAX];
} lib_inst_t;

typedef enum {
    LIB_FAILED,             /* nothing changed; msg says why */
    LIB_DONE_INSTALLED,     /* a new pack, in the store and the registry */
    LIB_DONE_UPDATED,       /* the new version of an installed pack, in place of the old */
    LIB_DONE_SAME           /* that very file was installed already: nothing to do */
} lib_result_t;

/* BOOT: after packs_init (the firmware's main task, before its tasks start: the file buffer is this call's stack's; the
 * simulator's main). pstore_boot, then every record through packs_add. How many were loaded. Logs a line a pack that is not */
int  library_boot(void);
int  library_installed_count(void);
const lib_inst_t *library_installed_at(int i);         /* in order of sector; NULL past the end */
const lib_inst_t *library_installed_uid(int kind, uint32_t uid);   /* a pack's identity is (kind, uid): a species and a habitat may share a uid */
bool library_installed_why(lib_work_t *w, const lib_inst_t *p, char *why, size_t n);   /* why one that is NOT LOADED is not (the loader's words, from its file again, in w's buffers); false: it is loaded */
int  library_free(void);                               /* packs that can still be installed: the store's free sectors */

/* SCAN: one visit (card_open ... card_close). false and why ("NO CARD", "NO PACKS FOLDER", ...) when the card cannot be listed;
 * else w->n rows, packs in name order, w->more set when the card had more. Reads each file once, into w->file */
bool library_scan(lib_work_t *w, char *why, size_t why_n);
/* INSTALL / UPDATE row i of the last scan (a second visit: the file is read again and must still be the scan's), or any file's
 * bytes by library_add (a test, a tool). msg: "INSTALLED <NAME>" / "UPDATED <NAME>" / the reason it failed (upper case) */
lib_result_t library_install(lib_work_t *w, int i, char *msg, size_t msg_n);
lib_result_t library_add(lib_work_t *w, const uint8_t *file, size_t n, char *msg, size_t msg_n);   /* w: its scratch is the check's */
/* REMOVE: the pack's sector erased and the pack out of the registry. false with why when it cannot be: not installed, built in,
 * another installed pack depends on it ("LEOPARD GECKO LIVES IN IT: REMOVE IT FIRST"), the flash refused */
bool library_remove(int kind, uint32_t uid, char *why, size_t why_n);
bool library_removable(int kind, uint32_t uid, char *why, size_t why_n);   /* the same refusals, nothing done: the page asks before it confirms */
#endif
#endif
