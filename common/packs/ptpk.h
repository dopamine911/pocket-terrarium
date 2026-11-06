/* ptpk.h - the PTPK pack file format and its loader (docs/ptpk.md is the byte-exact contract).
 *
 * A pack - one species, one habitat or one model manifest - is a file: a 20-byte header (magic, fmt, kind, flags, uid, len,
 * crc32 of the body) and a body of records `tag u16 | len u16 | value`, the tag's bit 15 being REQUIRED (a loader that does not
 * know the record refuses the pack; a clear bit: it skips the record). The loader here is the one entry for every origin: the
 * built-in packs are encoded from their const definitions at boot (ptpk_write.c, builtin.c: packs_load_builtin), the simulator
 * reads a folder (sim/packs_dir.c), the card will (phase 4) - all through packs_add. It trusts nothing: the header, the
 * length, the CRC, every record's size and range, the capabilities and the goals' words, the uid against the name, and then the
 * rules the engine always had (species_check, habitat_check, manifest_check) - a refused pack changes nothing and never crashes
 * it. The writer is ptpk_write.h, which also holds the format's two hashes (ptpk_crc32, ptpk_fnv1a).
 *
 * What the loader refuses by capability (issue #4 phase 2) is what this firmware can NEVER meet: a capability bit it does not
 * know in a species' needs, a movement mode it has no module for. A pack that merely does not work with this habitat or this
 * model loads, and is refused at the tank (compat.h packs_compatible). The world decides nothing here.
 *
 * Everything lands inline in the registry's pool (species.h / habitat.h / manifest.h): no malloc, no pointers into the file. */
#ifndef PTPK_H
#define PTPK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "species.h"
#include "habitat.h"
#include "manifest.h"

#define PTPK_FMT          1
#define PTPK_HEADER_BYTES 20
#define PTPK_BODY_MAX     2048                  /* a body of more is refused: it bounds every parse */
#define PTPK_FILE_MAX     (PTPK_HEADER_BYTES + PTPK_BODY_MAX)
enum { PTPK_KIND_SPECIES = 1, PTPK_KIND_HABITAT = 2, PTPK_KIND_MODEL = 3 };

/* spare pool slots, of each kind, for packs found at run time: the simulator's are for a folder of packs and the tests'; the
   firmware has NONE unless it carries the card library (PT_CARD, issue #4 phase 4: packs installed from the microSD card
   need somewhere to live) - on the 1.75C the aquarium already runs on a few KB of internal heap (issue: aquarium heap), and
   every spare costs a species_t (576 B + 4) and a habitat_t (404 B + 4) of .bss. With the card library: two species, two
   habitats (a pack and its habitat, or two of either) and one model manifest - 2160 B; without it, none and no .bss. The
   pool's size is this plus the built-in packs (species.c). A model's manifest comes with a model, one at a time:
   MANIFEST_SPARE */
#if PT_ALL_PACKS
#define PACKS_SPARE    12
#define MANIFEST_SPARE 4
#elif PT_CARD
#define PACKS_SPARE    2
#define MANIFEST_SPARE 1
#else
#define PACKS_SPARE    0
#define MANIFEST_SPARE 0
#endif

/* the header of an opened file: body points into the caller's bytes */
typedef struct { uint8_t fmt, kind; uint16_t flags; uint32_t uid, len, crc; const uint8_t *body; } ptpk_header_t;

/* the header, the length (exactly 20 + len bytes) and the CRC; false with the reason in why */
bool ptpk_open(const uint8_t *blob, size_t n, ptpk_header_t *h, char *why, size_t why_n);
/* the records of an opened species / habitat / manifest file into *out (zeroed first): every size, count and range, unknown
 * records by their REQUIRED bit, capabilities by their bits, goals by their words, the uid against UID_NS. *legacy_id: the
 * LEGACY_ID record (0: none). Not the pack's rules (species_check / habitat_check / manifest_check) and not the registry -
 * packs_add does both; the fuzz tests call these alone */
bool ptpk_read_species(const ptpk_header_t *h, species_t *out, uint8_t *legacy_id, char *why, size_t why_n);
bool ptpk_read_habitat(const ptpk_header_t *h, habitat_t *out, uint8_t *legacy_id, char *why, size_t why_n);
bool ptpk_read_manifest(const ptpk_header_t *h, model_manifest_t *out, char *why, size_t why_n);

/* ---- the registry's entry ---- */
typedef enum { PACKS_BUILTIN, PACKS_FILE } packs_origin_t;   /* only a built-in pack keeps its LEGACY_ID */
typedef enum {
    PACKS_ADDED,        /* loaded: it is in the registry now */
    PACKS_SAME,         /* that very file is loaded already (the built-in pack found again in a folder): nothing to do */
    PACKS_REFUSED       /* why says why; the registry is as it was */
} packs_result_t;
typedef struct { int kind; uint32_t uid; uint8_t id; char name[PACK_NAME_MAX]; } packs_info_t;   /* what was added (or was there); id 0 for a manifest */
/* one pack's file bytes: ptpk_open, the dedupe by uid (before any parsing), the records into the next pool slot,
 * species_commit / habitat_commit / manifest_commit (the id, the rules). info (optional) is filled for PACKS_ADDED and
 * PACKS_SAME */
packs_result_t packs_add(const uint8_t *blob, size_t n, packs_origin_t origin, packs_info_t *info, char *why, size_t why_n);

#if PT_CARD
/* ---- the dry check (issue #4 phase 4, docs/card-library.md): what packs_add WOULD do with a file, with nothing committed ----
 * The card library lists a card's files with a verdict on each before anything is installed. packs_dry_check runs everything
 * packs_add runs - the header, the length, the CRC, the dedupe by uid, every record, the capabilities, the goals' words, the
 * rules (species_check / habitat_check / manifest_check), this firmware's movement modules, a habitat the species lives in,
 * the model's tag held by one manifest - on a species_t / habitat_t / model_manifest_t in memory the CALLER owns (scratch: the
 * library's block in PSRAM, or a stack), and takes no pool slot: the registry is exactly as it was. The one difference from
 * packs_add is the order of two refusals: a pack that is bad is said to be bad before the pool is said to be full (a pack that
 * can never be installed is not told to make room for itself). */
typedef union { species_t s; habitat_t h; model_manifest_t m; } packs_scratch_t;
typedef struct {
    int      kind;               /* PTPK_KIND_*; 0 when the header did not open */
    uint32_t uid, crc;           /* the header's, once it opened (else 0) */
    char     name[PACK_NAME_MAX];/* the pack's own NAME, once its records were read ("" before) */
    caps_t   no_module;          /* a species that moves by a mode this firmware has no module for: those modes (0: none) */
    uint32_t want_habitat;       /* a species that lists no habitat that is loaded: the first it lists (0: not that refusal) */
    bool     held;               /* the registry holds a pack of this kind under this uid ... */
    bool     held_builtin;       /* ... a built-in one */
    bool     no_room;            /* a good pack, and the pool of its kind is full */
} packs_probe_t;
typedef enum {
    PACKS_DRY_NEW,               /* a good pack and a free slot: packs_add would load it */
    PACKS_DRY_SAME,              /* that very file is loaded already: packs_add would say PACKS_SAME */
    PACKS_DRY_OTHER,             /* a good pack, but another file holds its uid (held_builtin: a built-in's): an UPDATE when the holder came
                                    from the card - packs_add would refuse it; replacing the holder is the library's */
    PACKS_DRY_BAD                /* refused: why says why (probe: no_module, want_habitat, no_room say which kind of refusal) */
} packs_dry_t;
packs_dry_t packs_dry_check(const uint8_t *blob, size_t n, packs_scratch_t *scratch, packs_probe_t *probe, char *why, size_t why_n);
#endif

/* the built-in packs (builtin.h), manifests and habitats first: packs_load_builtin encodes each from its const definition into a
 * buffer on the caller's stack (PTPK_FILE_MAX) and offers it to packs_add, as a file's bytes. The registry loads them on its
 * first use; the firmware calls packs_init at boot, first thing in app_main on the main task's stack, before it starts a task
 * that reads a pack */
extern bool g_packs_ready;
void packs_load_builtin(void);
static inline void packs_init(void) { if (!g_packs_ready) packs_load_builtin(); }
/* every pack that is not built in, out (tests; a rescan from a clean state): the registries' own drop calls */
void packs_drop_loaded(void);
#endif
