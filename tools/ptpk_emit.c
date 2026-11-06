#define _DEFAULT_SOURCE   /* mkdir under -std=c11 (glibc) */
/* ptpk_emit.c - writes the built-in packs' files from the C structs that are their source (docs/ptpk.md "Authoring").
 *
 *   ptpk_emit OUTDIR     OUTDIR/<uid_ns>.ptpk for every built-in pack - the test packs and the other world's too
 *
 * Each file is what common/packs/ptpk_write.c makes of the const species_t / habitat_t / model_manifest_t that
 * common/packs/builtin.c lists (common/species, common/habitat, common/packs/models.c): no registry, no loader - this links the
 * encoder, the table and those definitions
 * alone. A file that already holds the bytes is left alone (its mtime, and git, stay quiet); one line a file. sim/Makefile
 * builds and runs it with ../packs (`make packs`) on every `make` and `make check`, so the .ptpk files in packs/ - what a card
 * gets - cannot go stale against the built-ins.
 *
 * Exit 0: every file is as the structs say. 1: a pack that cannot be encoded or a file that cannot be written. 2: usage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "packs/builtin.h"
#include "packs/ptpk_write.h"

#if !PT_ALL_PACKS
#error "ptpk_emit writes every built-in pack: build it with -DPT_ALL_PACKS=1 (sim/Makefile, `make packs`)"
#endif

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: ptpk_emit OUTDIR\n"); return 2; }
    const char *dir = argv[1];
    mkdir(dir, 0777);                                          /* (it is there: the repo's packs/; a fresh place for a test) */
    int rc = 0;
    for (int i = 0; i < PTPK_BUILTIN_N; i++) {
        const builtin_pack_t *b = &PTPK_BUILTIN[i];
        static uint8_t made[PTPK_FILE_MAX], held[PTPK_FILE_MAX + 1];
        const char *why = "";
        const size_t n = builtin_encode(b, made, sizeof made, &why);
        if (!n) { fprintf(stderr, "ptpk_emit: %s: cannot be encoded: %s\n", b->uid_ns, why); rc = 1; continue; }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.ptpk", dir, b->uid_ns);
        size_t got = 0;
        FILE *f = fopen(path, "rb");
        const bool existed = f != NULL;
        if (f) { got = fread(held, 1, sizeof held, f); fclose(f); }
        if (existed && got == n && memcmp(held, made, n) == 0) { printf("ptpk_emit: %s: %zu bytes, as it was\n", path, n); continue; }
        f = fopen(path, "wb");
        bool ok = f != NULL && fwrite(made, 1, n, f) == n;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) { fprintf(stderr, "ptpk_emit: %s: cannot be written\n", path); rc = 1; continue; }
        printf("ptpk_emit: %s: %zu bytes, %s\n", path, n, existed ? "rewritten" : "written");
    }
    return rc;
}
