/* sim_tmp.c - see sim_tmp.h: one scratch directory per run, gone when the run exits */
#define _DEFAULT_SOURCE   /* mkdtemp, nftw under -std=c11 (glibc) */
#define _XOPEN_SOURCE 700
#include "sim_tmp.h"
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char  s_dir[512];
static pid_t s_owner;                                           /* the process that made it removes it (a forked child never does) */
static const char *base_dir(void) { const char *b = getenv("TMPDIR"); return b && b[0] ? b : "/tmp"; }
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; remove(p); return 0; }
static void cleanup(void) { if (s_dir[0] && getpid() == s_owner) nftw(s_dir, rm_one, 16, FTW_DEPTH | FTW_PHYS); }

const char *sim_tmp_dir(void) {
    if (!s_dir[0]) {
        snprintf(s_dir, sizeof s_dir, "%s/pocket-terrarium-XXXXXX", base_dir());
        if (!mkdtemp(s_dir)) { snprintf(s_dir, sizeof s_dir, "%s/pocket-terrarium-%d", base_dir(), (int)getpid()); mkdir(s_dir, 0700); }
        s_owner = getpid();
        atexit(cleanup);
    }
    return s_dir;
}

const char *sim_tmp(const char *name) {
    enum { PATHS_MAX = 192, PATH_MAX_ = 768 };
    static char seen[PATHS_MAX][PATH_MAX_];
    static int n;
    char path[PATH_MAX_];
    const int len = snprintf(path, sizeof path, "%s/%s", sim_tmp_dir(), name);
    if (len < 0 || len >= (int)sizeof path) return "/dev/null/sim-tmp";
    for (int i = 0; i < n; i++) if (strcmp(seen[i], path) == 0) return seen[i];
    if (n >= PATHS_MAX) return "/dev/null/sim-tmp";
    memcpy(seen[n], path, (size_t)len + 1);
    return seen[n++];
}

const char *sim_tmp_keep(const char *name) {
    static char ring[8][600]; static int k;
    char *p = ring[k++ & 7];
    snprintf(p, sizeof ring[0], "%s/pocket-terrarium-%d-%s", base_dir(), (int)getpid(), name);
    return p;
}
