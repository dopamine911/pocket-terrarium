/* pstore.h - THE PACK STORE: the packs installed from the card, kept in the board's flash (issue #4 phase 4, docs/card-library.md).
 *
 * Installing a pack copies it here, so a tank never depends on the card being in its slot, and every boot loads what is here
 * through the same loader as a built-in pack or a file from a folder (library.h library_boot -> packs_add). This is NOT NVS: the
 * `nvs` partition is 6 pages of which the tank's save, the shelf's frame and the board's settings already hold what its budget
 * allows (save_layout.h "NVS budget": past two thirds a save can fail for space, and an NVS that cannot be opened is erased) - a
 * store of packs there would be taking room the keeper's tank may need. The `storage` partition is the shelf's from 0x10000
 * (shelf.h) and the NVS rescue copy's at 0 (main.c nvs_start: 0x6000 bytes); the 40 KB between them, 0x6000..0x10000, nobody
 * uses. That is the store: PSTORE_SECTORS sectors of 4 KB, one pack to a sector.
 *
 * A SECTOR DESCRIBES ITSELF, so there is no index to disagree with the packs and a power cut at any point leaves either the
 * state before or the state after, never half:
 *     0  magic "PTPS" (4)   4  layout u16 = 1   6  len u16 (the file's bytes, 20 < len <= PTPK_FILE_MAX)
 *     8  seq u32 (rises with every write)       12 uid u32 (the file's)      16 kind u8   17..27 zero
 *    28  crc32 u32 over bytes 0..27              32  the PTPK file, len bytes (held by its own CRC: ptpk_open)
 * A write is: erase a free sector, program the file at 32, program the header at 0 LAST. Before the header is whole the sector
 * is blank or torn and reads as nothing; a sector whose header fails its CRC, or whose file does not pass ptpk_open, is erased at
 * the next boot, never loaded. An UPDATE writes the new version into a second sector (a higher seq) and only then erases the
 * first: a cut between leaves two sectors of one uid, and the boot keeps the higher seq and erases the other. A REMOVE is an erase.
 *
 * The flash is the platform's (the ports below: persist_port_esp.c over the partition, persist_port_sim.c over a file
 * POCKET_TANK_PACKSTORE, which the simulator's selftests may cut the power of, shelf_port_sim_cut). Built only when PT_CARD. */
#ifndef PSTORE_H
#define PSTORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "world.h"

#if PT_CARD
#define PSTORE_SECTOR_BYTES 4096
#define PSTORE_SECTORS      10                       /* the 40 KB at storage + 0x6000 (0x10000 less the NVS rescue copy's 0x6000) */
#define PSTORE_BASE         0x6000                   /* ... from the start of the `storage` partition */
#define PSTORE_HEADER       32
#define PSTORE_MAGIC        0x53505450u              /* "PTPS" little-endian */

/* ---- the ports: the store's flash, PSTORE_SECTORS sectors from 0 (offsets and sizes in bytes, within the store) ---- */
bool pstore_port_read(uint32_t off, void *buf, size_t n);
bool pstore_port_write(uint32_t off, const void *buf, size_t n);   /* programs erased flash (bits only fall); false when the board refused it / the power was cut */
bool pstore_port_erase(int sector);                                /* one 4 KB sector back to 0xFF */

typedef struct { int sector; uint32_t uid, seq; uint8_t kind; uint16_t len; } pstore_rec_t;

/* the boot's look at the flash: every sector read, the valid ones listed (a uid in two sectors: the higher seq stays, the
 * other is erased), every other sector that is not blank erased (a torn write, a flipped bit - dropped, never loaded).
 * Returns how many records. Idempotent: it may be run again (a test's reboot) */
int  pstore_boot(void);
int  pstore_count(void);                             /* the records, as the last boot / write left them */
bool pstore_at(int i, pstore_rec_t *out);            /* record i in order of sector */
bool pstore_find(uint32_t uid, pstore_rec_t *out);   /* the record of this uid, if there is one */
int  pstore_free(void);                              /* sectors a new pack could go into */
/* record r's file into buf (cap bytes): its length, or -1 when the sector no longer reads true (its CRC is checked again) */
int  pstore_read(const pstore_rec_t *r, uint8_t *buf, size_t cap);
/* put a file (kind, uid: the pack's own, as packs_check read them) in: a new pack in a free sector, or the new version of a pack
 * already here in a second one and the first erased. false with why ("THE STORE IS FULL", "THE FLASH REFUSED IT") leaves what was
 * here as it was - a power cut anywhere in it too */
bool pstore_put(uint32_t uid, int kind, const uint8_t *file, size_t n, char *why, size_t why_n);
bool pstore_remove(uint32_t uid);                    /* the pack's sector erased; true when it is gone (or never was here) */
#endif
#endif
