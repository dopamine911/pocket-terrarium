#define _DEFAULT_SOURCE   /* dirent, lstat, O_NOFOLLOW under -std=c11 (glibc) */
/* card_port_sim.c - the simulator's microSD card: a folder (card.h; docs/card-library.md).
 *
 *   POCKET_CARD_DIR   the card's ROOT. Unset: this board has no card slot (card_slot() false, the PACKS page says so). Set to a
 *                     folder that is not there: a slot with no card (card_open: NO CARD). The packs are `<root>/packs/NAME.ptpk`
 *                     - exactly where they are on a real card - and nothing outside that one folder is ever opened.
 *
 * It is as strict as the board's port must be (a card is a stranger's): only regular files are listed (not a directory, a link, a
 * pipe: a FIFO named x.ptpk would block a plain open for ever), only names card_name_ok takes, the listing bounded and
 * ordered the same every run, a read never past the caller's buffer. It writes nothing. */
#include "card.h"

#if PT_CARD
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *root(void) { const char *d = getenv("POCKET_CARD_DIR"); return d && d[0] ? d : NULL; }
static bool s_open;

static void say(char *why, size_t n, const char *text) { if (why && n) snprintf(why, n, "%s", text); }

bool card_slot(void) { return root() != NULL; }

bool card_open(char *why, size_t n) {
    if (s_open) { say(why, n, "CARD BUSY"); return false; }
    const char *d = root();
    if (!d) { say(why, n, "NO CARD SLOT"); return false; }
    struct stat st;
    if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode)) { say(why, n, "NO CARD"); return false; }
    s_open = true;
    return true;
}
void card_close(void) { s_open = false; }

static bool path_of(char *out, size_t cap, const char *name) {
    const int k = snprintf(out, cap, "%s/%s/%s", root(), CARD_FOLDER, name);
    return k > 0 && (size_t)k < cap;
}

int card_list(card_entry_t *out, int max, bool *more, char *why, size_t why_n) {
    if (more) *more = false;
    if (!s_open || !root()) { say(why, why_n, "NO CARD"); return -1; }
    char dir[900];
    if (snprintf(dir, sizeof dir, "%s/%s", root(), CARD_FOLDER) >= (int)sizeof dir) { say(why, why_n, "NO PACKS FOLDER"); return -1; }
    DIR *dp = opendir(dir);
    if (!dp) { say(why, why_n, "NO PACKS FOLDER"); return -1; }
    int cnt = 0, total = 0;
    for (struct dirent *e; (e = readdir(dp)) != NULL; ) {
        if (!card_name_ok(e->d_name)) continue;
        char path[1000]; struct stat st;
        if (!path_of(path, sizeof path, e->d_name) || lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;   /* lstat: a link is not a file of the card */
        total++;
        int pos = 0;                                        /* the `max` smallest names, in order, whatever order the folder gives them in */
        while (pos < cnt && strcmp(out[pos].name, e->d_name) < 0) pos++;
        if (pos >= max) continue;
        const int last = cnt < max ? cnt : max - 1;         /* a full list loses its largest to make room */
        for (int k = last; k > pos; k--) out[k] = out[k - 1];
        snprintf(out[pos].name, sizeof out[pos].name, "%s", e->d_name);
        out[pos].size = st.st_size < 0 || st.st_size > 0x7fffffffLL ? 0x7fffffffu : (uint32_t)st.st_size;
        if (cnt < max) cnt++;
    }
    closedir(dp);
    if (more) *more = total > max;
    return cnt;
}

int card_read(const char *name, uint8_t *buf, size_t cap) {
    if (!s_open || !card_name_ok(name) || !buf) return -1;
    char path[1000];
    if (!path_of(path, sizeof path, name)) return -1;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);          /* (a link or a pipe is refused or does not block) */
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    size_t got = 0;
    while (got < cap) {                                     /* never past cap: the caller's buffer is cap bytes */
        const ssize_t r = read(fd, buf + got, cap - got < 4096 ? cap - got : 4096);
        if (r <= 0) break;
        got += (size_t)r;
    }
    uint8_t extra;
    const bool longer = got == cap && read(fd, &extra, 1) == 1;     /* one more byte: it is longer than the caller can take (a card file is never trusted to be small) */
    close(fd);
    return longer ? -1 : (int)got;
}
void *card_mem_alloc(size_t bytes) { return calloc(1, bytes); }
void  card_mem_free(void *p) { free(p); }
#endif
