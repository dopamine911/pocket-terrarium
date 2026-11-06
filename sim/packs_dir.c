#define _DEFAULT_SOURCE   /* dirent, stat under -std=c11 (glibc) */
/* packs_dir.c - the simulator's folder of packs (packs_dir.h) */
#include "packs_dir.h"
#include "packs/ptpk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *sim_packs_dir(void) {
    const char *d = getenv("POCKET_PACKS_DIR");
    return d && d[0] ? d : "../packs";
}

/* a refusal already said, by file name, size, mtime and reason: a rescan of the same file stays quiet */
typedef struct { char name[160]; long size; long mtime; char why[96]; } said_t;
#define SAID_MAX 48
static said_t s_said[SAID_MAX];
static int    s_n_said;

static bool said_before(const char *name, long size, long mtime, const char *why) {
    for (int i = 0; i < s_n_said; i++) {
        said_t *s = &s_said[i];
        if (strcmp(s->name, name) != 0) continue;
        if (s->size == size && s->mtime == mtime && strncmp(s->why, why, sizeof s->why - 1) == 0) return true;
        s->size = size; s->mtime = mtime; snprintf(s->why, sizeof s->why, "%.*s", (int)sizeof s->why - 1, why);   /* the file changed (or its reason): say it again */
        return false;
    }
    if (s_n_said < SAID_MAX) {
        said_t *s = &s_said[s_n_said++];
        snprintf(s->name, sizeof s->name, "%.*s", (int)sizeof s->name - 1, name); s->size = size; s->mtime = mtime; snprintf(s->why, sizeof s->why, "%.*s", (int)sizeof s->why - 1, why);
    }
    return false;
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int sim_packs_scan(const char *dir, bool log) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    enum { FILES_MAX = 64, NAME_MAX_ = 160 };
    static char names[FILES_MAX][NAME_MAX_];
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        const size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".ptpk") != 0 || l >= NAME_MAX_ || n >= FILES_MAX) continue;
        memcpy(names[n++], e->d_name, l + 1);
    }
    closedir(d);
    qsort(names, (size_t)n, NAME_MAX_, name_cmp);            /* the same order every run */
    int added = 0;
    for (int pass = 0; pass < 2; pass++) {                   /* habitats first: a species lists its habitats by uid */
        for (int i = 0; i < n; i++) {
            char path[768]; snprintf(path, sizeof path, "%.400s/%.200s", dir, names[i]);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            char why[200] = "";
            static uint8_t buf[PTPK_FILE_MAX + 1];
            size_t got = 0;
            if ((size_t)st.st_size > PTPK_FILE_MAX) {
                snprintf(why, sizeof why, "%ld bytes: a pack is at most %d", (long)st.st_size, PTPK_FILE_MAX);
            } else {
                FILE *f = fopen(path, "rb");
                if (!f) snprintf(why, sizeof why, "cannot be read");
                else { got = fread(buf, 1, sizeof buf, f); fclose(f); if (got != (size_t)st.st_size) { snprintf(why, sizeof why, "short read (%zu of %ld bytes)", got, (long)st.st_size); got = 0; } }
            }
            const bool habitat = !why[0] && got > 5 && memcmp(buf, "PTPK", 4) == 0 && buf[5] == PTPK_KIND_HABITAT;
            if ((pass == 0) != habitat) continue;            /* (a file that cannot be told goes with the species: refused there) */
            if (!why[0]) {
                packs_info_t info;
                switch (packs_add(buf, got, PACKS_FILE, &info, why, sizeof why)) {
                case PACKS_ADDED:
                    added++;
                    if (log) printf("packs: %s: loaded %s %s (uid %08x, id %d)\n", names[i], info.kind == PTPK_KIND_HABITAT ? "habitat" : "species", info.name, (unsigned)info.uid, info.id);
                    continue;
                case PACKS_SAME:                             /* the built-in pack's own file, or one read before: nothing to say */
                    continue;
                case PACKS_REFUSED:
                    break;
                }
            }
            if (log && !said_before(names[i], (long)st.st_size, (long)st.st_mtime, why)) printf("packs: %s: REFUSED: %s\n", names[i], why);
        }
    }
    return added;
}

void sim_packs_poll(bool waiting, float dt) {
    static float since;
    if (!waiting) { since = 0; return; }
    since += dt;
    if (since < 2.0f) return;
    since = 0;
    sim_packs_scan(sim_packs_dir(), true);
}
