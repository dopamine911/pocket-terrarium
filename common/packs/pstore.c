/* pstore.c - the pack store (pstore.h): self-describing sectors, a header written last, no index */
#include "pstore.h"

#if PT_CARD
#include "ptpk.h"
#include "ptpk_write.h"
#include <stdio.h>
#include <string.h>

/* what the last boot / write left in RAM about each sector: a valid record, or nothing (blank, torn, dropped) */
typedef struct { uint32_t uid, seq; uint16_t len; uint8_t kind, valid; } slot_t;
static slot_t   s_slot[PSTORE_SECTORS];
static uint32_t s_seq;                                     /* the highest seq in the store: the next write is one more */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t sector_off(int k) { return (uint32_t)k * PSTORE_SECTOR_BYTES; }

/* the header's own test: the magic, the layout, a length a pack can have, a seq that was written (an erased word is 0xFFFFFFFF),
   a uid, a kind - and its CRC over the 28 bytes before it. The FILE after it is held by its own CRC (ptpk_open) and the loader */
static bool header_ok(const uint8_t *h, slot_t *out) {
    if (le32(h) != PSTORE_MAGIC || le16(h + 4) != 1) return false;
    const uint32_t len = le16(h + 6), seq = le32(h + 8), uid = le32(h + 12);
    if (len <= PTPK_HEADER_BYTES || len > PTPK_FILE_MAX) return false;
    if (seq == 0 || seq == 0xffffffffu || !uid) return false;
    if (h[16] != PTPK_KIND_SPECIES && h[16] != PTPK_KIND_HABITAT && h[16] != PTPK_KIND_MODEL) return false;
    if (ptpk_crc32(h, 28) != le32(h + 28)) return false;
    out->uid = uid; out->seq = seq; out->len = (uint16_t)len; out->kind = h[16]; out->valid = 1;
    return true;
}
static bool blank(const uint8_t *h) { for (int i = 0; i < PSTORE_HEADER; i++) if (h[i] != 0xff) return false; return true; }

/* sector k's file into buf (the slot's length), true when it is the file its header names: its length, and the PTPK file's own
   header, length and CRC (ptpk_open), its uid and kind the header's. Used by the boot's look and by every later read */
static bool read_file(int k, const slot_t *s, uint8_t *buf, size_t cap) {
    if (s->len > cap) return false;
    if (!pstore_port_read(sector_off(k) + PSTORE_HEADER, buf, s->len)) return false;
    ptpk_header_t ph;
    return ptpk_open(buf, s->len, &ph, NULL, 0) && ph.uid == s->uid && ph.kind == s->kind;
}

int pstore_boot(void) {
    uint8_t file[PTPK_FILE_MAX];                           /* (the boot's stack: the main task's, or the test's) */
    memset(s_slot, 0, sizeof s_slot); s_seq = 0;
    for (int k = 0; k < PSTORE_SECTORS; k++) {
        uint8_t h[PSTORE_HEADER];
        if (!pstore_port_read(sector_off(k), h, sizeof h) || blank(h)) continue;   /* (a sector the board cannot read is left alone: not ours to erase) */
        slot_t s = { 0, 0, 0, 0, 0 };
        if (header_ok(h, &s) && read_file(k, &s, file, sizeof file)) { s_slot[k] = s; if (s.seq > s_seq) s_seq = s.seq; continue; }
        printf("pstore: sector %d does not read true (a torn or damaged write): dropped\n", k);
        pstore_port_erase(k);                              /* gone for good: never loaded, and the sector is free again */
    }
    for (int i = 0; i < PSTORE_SECTORS; i++)               /* an update cut between its two steps: one (kind, uid) in two sectors - the newer stays */
        for (int j = i + 1; j < PSTORE_SECTORS && s_slot[i].valid; j++) {
            if (!s_slot[j].valid || s_slot[j].uid != s_slot[i].uid || s_slot[j].kind != s_slot[i].kind) continue;
            const int old = s_slot[i].seq < s_slot[j].seq ? i : j;
            printf("pstore: uid %08x is in sectors %d and %d (an update was cut short): the older one dropped\n", (unsigned)s_slot[i].uid, i, j);
            pstore_port_erase(old);
            s_slot[old].valid = 0;
        }
    return pstore_count();
}

int pstore_count(void) { int n = 0; for (int k = 0; k < PSTORE_SECTORS; k++) n += s_slot[k].valid; return n; }
int pstore_free(void) { return PSTORE_SECTORS - pstore_count(); }
static void rec_of(int k, pstore_rec_t *out) { out->sector = k; out->uid = s_slot[k].uid; out->seq = s_slot[k].seq; out->kind = s_slot[k].kind; out->len = s_slot[k].len; }
bool pstore_at(int i, pstore_rec_t *out) {
    for (int k = 0; k < PSTORE_SECTORS; k++) if (s_slot[k].valid && i-- == 0) { rec_of(k, out); return true; }
    return false;
}
bool pstore_find(int kind, uint32_t uid, pstore_rec_t *out) {
    for (int k = 0; uid && k < PSTORE_SECTORS; k++) if (s_slot[k].valid && s_slot[k].uid == uid && s_slot[k].kind == kind) { if (out) rec_of(k, out); return true; }
    return false;
}

int pstore_read(const pstore_rec_t *r, uint8_t *buf, size_t cap) {
    if (!r || r->sector < 0 || r->sector >= PSTORE_SECTORS) return -1;
    uint8_t h[PSTORE_HEADER];
    slot_t s;
    if (!pstore_port_read(sector_off(r->sector), h, sizeof h) || !header_ok(h, &s) || s.uid != r->uid || s.kind != r->kind || s.seq != r->seq) return -1;   /* (the sector has changed since the record was made) */
    return read_file(r->sector, &s, buf, cap) ? (int)s.len : -1;
}

static bool fail(char *why, size_t n, const char *text) { if (why && n) snprintf(why, n, "%s", text); return false; }

bool pstore_put(uint32_t uid, int kind, const uint8_t *file, size_t n, char *why, size_t why_n) {
    if (!file || n <= PTPK_HEADER_BYTES || n > PTPK_FILE_MAX || !uid) return fail(why, why_n, "NOT A PACK FILE");
    ptpk_header_t ph;
    if (!ptpk_open(file, n, &ph, NULL, 0) || ph.uid != uid || ph.kind != kind) return fail(why, why_n, "NOT THAT PACK'S FILE");
    int old = -1, fresh = -1;
    for (int k = 0; k < PSTORE_SECTORS; k++) {
        if (s_slot[k].valid && s_slot[k].uid == uid && s_slot[k].kind == kind) old = k;
        else if (!s_slot[k].valid && fresh < 0) fresh = k;
    }
    if (fresh < 0) return fail(why, why_n, "THE STORE IS FULL: REMOVE A PACK FIRST");
    uint8_t h[PSTORE_HEADER];
    memset(h, 0, sizeof h);
    put32(h, PSTORE_MAGIC); h[4] = 1; h[6] = (uint8_t)n; h[7] = (uint8_t)(n >> 8);
    put32(h + 8, s_seq + 1); put32(h + 12, uid); h[16] = (uint8_t)kind;
    put32(h + 28, ptpk_crc32(h, 28));
    /* the file first, the header LAST: until the header is whole this sector reads as nothing, so a cut anywhere leaves the store as it was */
    if (!pstore_port_erase(fresh) || !pstore_port_write(sector_off(fresh) + PSTORE_HEADER, file, n) || !pstore_port_write(sector_off(fresh), h, sizeof h))
        return fail(why, why_n, "THE FLASH REFUSED IT");
    s_seq++;
    s_slot[fresh].uid = uid; s_slot[fresh].seq = s_seq; s_slot[fresh].len = (uint16_t)n; s_slot[fresh].kind = (uint8_t)kind; s_slot[fresh].valid = 1;
    if (old >= 0) {                                        /* an update: the new version is whole, now the old goes (a cut before this leaves both; the boot keeps the newer) */
        s_slot[old].valid = 0;
        pstore_port_erase(old);
    }
    return true;
}

bool pstore_remove(int kind, uint32_t uid) {
    for (int k = 0; uid && k < PSTORE_SECTORS; k++) {
        if (!s_slot[k].valid || s_slot[k].uid != uid || s_slot[k].kind != kind) continue;
        if (!pstore_port_erase(k)) return false;
        s_slot[k].valid = 0;
        return true;
    }
    return true;                                           /* never here */
}
#endif
