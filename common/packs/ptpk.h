/* ptpk.h - the PTPK pack file format and its loader (docs/ptpk.md is the byte-exact contract).
 *
 * A pack - one species or one habitat - is a file: a 20-byte header (magic, fmt, kind, flags, uid, len, crc32 of the
 * body) and a body of records `tag u16 | len u16 | value`, the tag's bit 15 being REQUIRED (a loader that does not know the
 * record refuses the pack; a clear bit: it skips the record). The loader here is the one entry for every origin: the
 * built-in packs are encoded from their const definitions at boot (ptpk_write.c, builtin.c: packs_load_builtin), the simulator
 * reads a folder (sim/packs_dir.c), the card will (phase 4) - all through packs_add. It trusts nothing: the header, the
 * length, the CRC, every record's size and range, the goals' names, the uid against the name, and then the rules the
 * engine always had (species_check, habitat_check) - a refused pack changes nothing and never crashes it. The writer is
 * ptpk_write.h, which also holds the format's two hashes (ptpk_crc32, ptpk_fnv1a).
 *
 * Everything lands inline in the registry's pool (species.h / habitat.h): no malloc, no pointers into the file. */
#ifndef PTPK_H
#define PTPK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "species.h"
#include "habitat.h"

#define PTPK_FMT          1
#define PTPK_HEADER_BYTES 20
#define PTPK_BODY_MAX     2048                  /* a body of more is refused: it bounds every parse */
#define PTPK_FILE_MAX     (PTPK_HEADER_BYTES + PTPK_BODY_MAX)
enum { PTPK_KIND_SPECIES = 1, PTPK_KIND_HABITAT = 2, PTPK_KIND_MODEL = 3 };   /* (a model manifest is reserved: refused) */

/* spare pool slots, of each kind, for packs found at run time: the firmware's two are for the card (phase 4), the
   simulator's twelve for a folder of packs and the tests' (the pool's size is this plus the built-in packs: species.c) */
#if PT_ALL_PACKS
#define PACKS_SPARE 12
#else
#define PACKS_SPARE 2
#endif

/* the header of an opened file: body points into the caller's bytes */
typedef struct { uint8_t fmt, kind; uint16_t flags; uint32_t uid, len, crc; const uint8_t *body; } ptpk_header_t;

/* the header, the length (exactly 20 + len bytes) and the CRC; false with the reason in why */
bool ptpk_open(const uint8_t *blob, size_t n, ptpk_header_t *h, char *why, size_t why_n);
/* the records of an opened species / habitat file into *out (zeroed first): every size, count and range, unknown
 * records by their REQUIRED bit, goals by name, the uid against UID_NS. *legacy_id: the LEGACY_ID record (0: none). Not
 * the pack's rules (species_check / habitat_check) and not the registry - packs_add does both; the fuzz tests call these
 * two alone */
bool ptpk_read_species(const ptpk_header_t *h, species_t *out, uint8_t *legacy_id, char *why, size_t why_n);
bool ptpk_read_habitat(const ptpk_header_t *h, habitat_t *out, uint8_t *legacy_id, char *why, size_t why_n);

/* ---- the registry's entry ---- */
typedef enum { PACKS_BUILTIN, PACKS_FILE } packs_origin_t;   /* only a built-in pack keeps its LEGACY_ID */
typedef enum {
    PACKS_ADDED,        /* loaded: it is in the registry now */
    PACKS_SAME,         /* that very file is loaded already (the built-in pack found again in a folder): nothing to do */
    PACKS_REFUSED       /* why says why; the registry is as it was */
} packs_result_t;
typedef struct { int kind; uint32_t uid; uint8_t id; char name[PACK_NAME_MAX]; } packs_info_t;   /* what was added (or was there) */
/* one pack's file bytes: ptpk_open, the dedupe by uid (before any parsing), the records into the next pool slot, the
 * world (a release image refuses the other world's pack), species_commit / habitat_commit (the id, the rules). info
 * (optional) is filled for PACKS_ADDED and PACKS_SAME */
packs_result_t packs_add(const uint8_t *blob, size_t n, packs_origin_t origin, packs_info_t *info, char *why, size_t why_n);

/* the built-in packs (builtin.h), habitats first: packs_load_builtin encodes each from its const definition into a buffer on the
 * caller's stack (PTPK_FILE_MAX) and offers it to packs_add, as a file's bytes. The registry loads them on its first use; the
 * firmware calls packs_init at boot, first thing in app_main on the main task's stack, before it starts a task that reads a pack */
extern bool g_packs_ready;
void packs_load_builtin(void);
static inline void packs_init(void) { if (!g_packs_ready) packs_load_builtin(); }
/* a release image refuses the other world's packs at load (a card pack of the aquarium in the terrarium's firmware would fill a
 * slot for nothing); the simulator (PT_ALL_PACKS) keeps both. The default is that; a test switches it to see the refusal */
void packs_world_filter(bool on);
/* every pack that is not built in, out (tests; a rescan from a clean state): the registries' own drop calls */
void packs_drop_loaded(void);
#endif
