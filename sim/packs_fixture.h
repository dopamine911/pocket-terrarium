/* packs_fixture.h - pack files to test the card library with (issue #4 phase 4): the built-in packs as files, a variant of one
 * under a new name (so a new uid), and the record edits a hostile or odd file needs. The simulator's own - nothing of it goes
 * in a firmware. (sim/selftest_ptpk.c keeps its own copies of these helpers; this is the library's tests' - selftest_library.c,
 * selftest_packs_page.c.) */
#ifndef PACKS_FIXTURE_H
#define PACKS_FIXTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "packs/ptpk.h"

/* a file being made or edited: room past PTPK_FILE_MAX, so an over-long hostile one can be built in it */
typedef struct { uint8_t b[PTPK_FILE_MAX + 4096]; size_t n; } pf_blob_t;

uint32_t pf_rd16(const uint8_t *p);
uint32_t pf_rd32(const uint8_t *p);
void     pf_wr16(uint8_t *p, uint32_t v);
void     pf_wr32(uint8_t *p, uint32_t v);

/* the built-in pack of this kind (PTPK_KIND_*) this world runs, as the file the encoder makes of it: the world's default species,
 * its default habitat, the manifest of its model. false: not there (a build that does not carry it) */
bool pf_builtin(int kind, pf_blob_t *out);
/* the built-in file of a given uid (every pack of this build, either world's, test packs too) */
bool pf_builtin_uid(uint32_t uid, pf_blob_t *out);
/* the same file as a pack of its own: UID_NS and NAME get new last characters (the lengths stay; ns_tail "f1" gives
 * "pt.gecko.leopard" -> "pt.gecko.leopaf1", name_tail "F1" gives "LEOPARD GECKO" -> "LEOPARD GECKF1"); a model manifest's MODEL_TAG
 * takes ns_tail's first character as its last (a tag belongs to one manifest). Header made good. */
void pf_variant(pf_blob_t *out, const pf_blob_t *from, const char *ns_tail, const char *name_tail);
/* the header from the body: len, uid = FNV-1a of UID_NS, crc */
void pf_fix(pf_blob_t *k);
uint32_t pf_uid(const pf_blob_t *k);
/* a species file's habitats (the HABITATS record, 1..4 uids) set to these */
void pf_set_habitats(pf_blob_t *k, const uint32_t *uids, int n);
/* records by id: where one is (the tag's offset, its value's length), replaced (REQUIRED bit kept, length may change), deleted,
 * appended (tag as written: bit 15 REQUIRED). The header is NOT fixed by any of them (pf_fix) */
bool pf_rec_find(const pf_blob_t *k, unsigned id, size_t *at, size_t *vlen);
void pf_rec_set(pf_blob_t *k, unsigned id, const void *v, size_t l);
void pf_rec_delete(pf_blob_t *k, unsigned id);
void pf_rec_append(pf_blob_t *k, unsigned tag, const void *v, size_t l);
/* a file's bytes written as dir/name (the dir must exist): false when it could not be */
bool pf_write_file(const char *dir, const char *name, const void *b, size_t n);
#endif
