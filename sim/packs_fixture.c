/* packs_fixture.c - pack files to test the card library with (packs_fixture.h) */
#include "packs_fixture.h"
#include "packs/builtin.h"
#include "packs/manifest.h"
#include "packs/ptpk_write.h"
#include "species.h"
#include "habitat.h"
#include <stdio.h>
#include <string.h>

uint32_t pf_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
uint32_t pf_rd32(const uint8_t *p) { return pf_rd16(p) | pf_rd16(p + 2) << 16; }
void pf_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void pf_wr32(uint8_t *p, uint32_t v) { pf_wr16(p, v); pf_wr16(p + 2, v >> 16); }

bool pf_builtin_uid(uint32_t uid, pf_blob_t *out) {
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {
        if (PTPK_BUILTIN[i].uid != uid) continue;
        const char *why = "";
        out->n = builtin_encode(&PTPK_BUILTIN[i], out->b, sizeof out->b, &why);
        return out->n != 0;
    }
    return false;
}
bool pf_builtin(int kind, pf_blob_t *out) {
    uint32_t uid = 0;
    if (kind == PTPK_KIND_SPECIES) { const species_t *s = species_get(SPECIES_DEFAULT); uid = s ? s->uid : 0; }
    else if (kind == PTPK_KIND_HABITAT) { const habitat_t *h = habitat_get(HABITAT_DEFAULT); uid = h ? h->uid : 0; }
    else if (kind == PTPK_KIND_MODEL) { const model_manifest_t *m = model_provides(); uid = m ? m->uid : 0; }
    return uid && pf_builtin_uid(uid, out);
}

bool pf_rec_find(const pf_blob_t *k, unsigned id, size_t *at, size_t *vlen) {
    for (size_t o = PTPK_HEADER_BYTES; o + 4 <= k->n; ) {
        const unsigned tag = pf_rd16(k->b + o), l = pf_rd16(k->b + o + 2);
        if (o + 4 + l > k->n) return false;
        if ((tag & 0x7fffu) == id) { *at = o; *vlen = l; return true; }
        o += 4 + l;
    }
    return false;
}
uint32_t pf_uid(const pf_blob_t *k) { return pf_rd32(k->b + 8); }
void pf_fix(pf_blob_t *k) {
    size_t at, l;
    pf_wr32(k->b + 12, (uint32_t)(k->n - PTPK_HEADER_BYTES));
    if (pf_rec_find(k, 2, &at, &l)) pf_wr32(k->b + 8, ptpk_fnv1a(k->b + at + 4, l));
    pf_wr32(k->b + 16, ptpk_crc32(k->b + PTPK_HEADER_BYTES, k->n - PTPK_HEADER_BYTES));
}
void pf_rec_append(pf_blob_t *k, unsigned tag, const void *v, size_t l) {
    pf_wr16(k->b + k->n, tag); pf_wr16(k->b + k->n + 2, (uint32_t)l);
    if (l) memcpy(k->b + k->n + 4, v, l);
    k->n += 4 + l;
}
void pf_rec_delete(pf_blob_t *k, unsigned id) {
    size_t at, l;
    if (!pf_rec_find(k, id, &at, &l)) return;
    memmove(k->b + at, k->b + at + 4 + l, k->n - (at + 4 + l));
    k->n -= 4 + l;
}
void pf_rec_set(pf_blob_t *k, unsigned id, const void *v, size_t l) {
    size_t at, ol;
    if (!pf_rec_find(k, id, &at, &ol)) return;
    const unsigned tag = pf_rd16(k->b + at);
    memmove(k->b + at + 4 + l, k->b + at + 4 + ol, k->n - (at + 4 + ol));
    pf_wr16(k->b + at, tag); pf_wr16(k->b + at + 2, (uint32_t)l);
    if (l) memcpy(k->b + at + 4, v, l);
    k->n = k->n - ol + l;
}

void pf_variant(pf_blob_t *out, const pf_blob_t *from, const char *ns_tail, const char *name_tail) {
    if (out != from) { memcpy(out->b, from->b, from->n); out->n = from->n; }
    size_t at, l;
    if (pf_rec_find(out, 2, &at, &l) && strlen(ns_tail) <= l) memcpy(out->b + at + 4 + l - strlen(ns_tail), ns_tail, strlen(ns_tail));
    if (pf_rec_find(out, 1, &at, &l) && strlen(name_tail) <= l) memcpy(out->b + at + 4 + l - strlen(name_tail), name_tail, strlen(name_tail));
    if (out->b[5] == PTPK_KIND_MODEL && pf_rec_find(out, 0x301, &at, &l) && l) out->b[at + 4 + l - 1] = (uint8_t)ns_tail[0];
    pf_fix(out);
}

void pf_set_habitats(pf_blob_t *k, const uint32_t *uids, int n) {
    uint8_t v[1 + 4 * SPECIES_HABITATS_MAX];
    if (n < 1) n = 1;
    if (n > SPECIES_HABITATS_MAX) n = SPECIES_HABITATS_MAX;
    v[0] = (uint8_t)n;
    for (int i = 0; i < n; i++) pf_wr32(v + 1 + 4 * i, uids[i]);
    pf_rec_set(k, 0x101, v, 1 + 4 * (size_t)n);
    pf_fix(k);
}

bool pf_write_file(const char *dir, const char *name, const void *b, size_t n) {
    char path[900];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = n == 0 || fwrite(b, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
