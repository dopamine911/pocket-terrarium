/* ptpk_write.h - the PTPK encoder: a species_t / habitat_t / model_manifest_t as a whole pack file (docs/ptpk.md is the
 * byte-exact contract; common/packs/ptpk.c is the reader this one mirrors, record for record), and the format's two hashes.
 *
 * The built-in packs' source of truth is a const species_t / habitat_t / model_manifest_t in C (common/species, common/habitat,
 * common/packs/models.c) that builtin.c lists. The firmware encodes each into a file at boot and loads it through packs_add like
 * any file from a folder or the card; the host emitter (tools/ptpk_emit.c) writes the same bytes to packs/<uid_ns>.ptpk. So this
 * file knows the format and nothing of the registry - no pool, no terrain, no tank - and the emitter links it alone.
 *
 * The file says what the struct says: its name, worlds (the WORLDS record, when set), test_only, its `id` as the LEGACY_ID record
 * (0: none), its goals by word, its capabilities (LOCOMOTION and MEDIA_NEEDS; AREAS and PROVIDES), its optional parts (a shed
 * cycle, morphs, patterns, hides, rocks, ramps, a dish) only when they are there; the runtime's own fields (`habitats`,
 * `builtin`) are not written. The uid is the hash of uid_ns and p->uid must be it: a def whose uid constant has gone stale is
 * refused, not written. */
#ifndef PTPK_WRITE_H
#define PTPK_WRITE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ptpk.h"
#include "manifest.h"

uint32_t ptpk_crc32(const uint8_t *b, size_t n);   /* CRC-32 as zlib's: "123456789" -> 0xcbf43926 */
uint32_t ptpk_fnv1a(const uint8_t *b, size_t n);   /* FNV-1a-32: a pack's uid is this of its UID_NS string */
/* the format's kinds of text, for the reader and the writer alike: 1..max bytes of 0x20..0x7e with no space at either end; a
   UID_NS, [a-z0-9._-], 3..47 bytes; a goal's word, 1..15 bytes of [a-z0-9_] with a lowercase letter first; a model's tag,
   1..15 bytes of [A-Za-z0-9._-] */
bool ptpk_text_ok(const uint8_t *s, size_t n, size_t max);
bool ptpk_ns_ok(const uint8_t *s, size_t n);
bool ptpk_goal_ok(const uint8_t *s, size_t n);
bool ptpk_tag_ok(const uint8_t *s, size_t n);

/* the pack as a file (header and body) into out, cap bytes at most: its size, or 0 when it cannot be written - a text, a count
 * or an enum the format cannot hold, a NaN or an infinity, a goal word the format does not take, a capability nobody names, a
 * body of over PTPK_BODY_MAX, a uid that is not uid_ns's hash. *why (optional) is then a short reason, a string that outlives
 * the call. Nothing here is the pack's rules (species_check / habitat_check / manifest_check): the loader runs those on the file
 * as on any other. */
size_t ptpk_encode_species(const species_t *p, const char *uid_ns, uint8_t *out, size_t cap, const char **why);
size_t ptpk_encode_habitat(const habitat_t *h, const char *uid_ns, uint8_t *out, size_t cap, const char **why);
size_t ptpk_encode_manifest(const model_manifest_t *m, const char *uid_ns, uint8_t *out, size_t cap, const char **why);
#endif
