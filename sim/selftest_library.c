#define _DEFAULT_SOURCE   /* mkdir, mkfifo, symlink, setenv, unsetenv, nftw under -std=c11 (glibc) */
#define _XOPEN_SOURCE 700
/* selftest_library.c - issue #4 phase 4, "the card library": list the packs on the microSD card, check them before use, install them
 * into flash so a tank never depends on the card, keep them across reboots and power cuts, remove them (docs/card-library.md; the
 * contract is common/card.h, common/packs/library.h and pstore.h). The card here is a folder (POCKET_CARD_DIR, sim/card_port_sim.c)
 * and the board's flash a file (POCKET_TANK_PACKSTORE, sim/persist_port_sim.c) whose power the test cuts - the real calls, the real
 * loader, no stand-ins. The PACKS page's own test is --selftest-packs-page.
 *
 *   ./fishsim --selftest-library
 *
 *   1  names: card_name_ok table; a card of directories, pipes, links, hidden and odd names lists only its plain files, and a read
 *      of a hostile name is refused
 *   2  scan: a card with one of everything - a good species, habitat and model manifest, the built-in files, a damaged, a short, an
 *      empty, a huge, a foreign file, a species whose habitat is also on the card, one that needs a movement mode no firmware has,
 *      the built-in uid under another file - and every verdict and reason; the scan takes no pool slot
 *   3  install and keep: a pack installed is loaded at once and again after a reboot, with the card gone; habitats, manifests and
 *      species in dependency order; a species installed before its habitat in the store still loads
 *   4  update: the same uid in another file replaces the old one in the registry and the store
 *   5  remove: gone from both; a built-in or an unknown uid refused; a habitat an installed species lives in, and a manifest whose
 *      words it needs, refused until that species goes
 *   6  full: the store at its sectors, the pool at its slots - each a refusal that changes nothing
 *   7  wrong firmware: a pack this firmware has no module for is NO MODULE naming it, and cannot be installed
 *   8  power cuts: an install, an update and a remove cut at every write and erase (and torn half-way): the next boot finds the
 *      state before or the state after, never anything between, drops what is torn, and the store works again
 *   9  a hostile store: flipped, torn, doubled, garbage sectors - never a crash, never a pack loaded that does not read true; a
 *      stored pack that cannot load is kept, listed NOT LOADED, and loads when what it needed arrives
 *  10  fuzz: mutated packs through the dry check, through install and through the card's scan - nothing crashes, a refusal changes
 *      nothing; a card of a thousand files lists 32 and says so
 *  11  a tank on an installed pack: it saves, the pack is removed and the tank waits (PACK MISSING), the pack comes back from the
 *      card and it resumes; an update of the pack it lives on; a reboot with the pack installed or gone */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tank.h"
#include "progression.h"
#include "notice.h"
#include "species.h"
#include "habitat.h"
#include "packs/caps.h"
#include "packs/compat.h"
#include "packs/manifest.h"
#include "packs/ptpk.h"
#include "packs/ptpk_write.h"
#include "packs/builtin.h"
#include "packs/library.h"
#include "packs/pstore.h"
#include "card.h"
#include "sim_tmp.h"
#include "packs_fixture.h"

#if PT_CARD
int shelf_port_sim_cut(int writes);                          /* persist_port_sim.c: every write and erase after n more fails (-1: never) */
extern int g_sim_pstore_tear;                                /* ... a failing write programs / erases its first half before it fails */

static const char *s_name = "selftest-library";
#define PF(...) do { printf("FAIL: %s: ", s_name); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static tank_t s_t;                                           /* (tank_t is big: never on the stack) */
static lib_work_t *W;                                        /* the library's working block, as the page allocates it */
static char s_card[700], s_packs[760], s_store[700];
static pf_blob_t S0, H0, M0;                                 /* this world's default species, habitat and model manifest, as files */

/* ================================================================ plumbing */
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; remove(p); return 0; }
static void rm_rf(const char *dir) { nftw(dir, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static uint32_t s_rng = 0x2545f491u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* a clean board and an empty card: the store erased, the registry holding the built-in packs alone, /packs there and empty */
static void fresh(void) {
    shelf_port_sim_cut(-1); g_sim_pstore_tear = 0;
    firmware_caps_override(false, 0);
    remove(s_store);
    rm_rf(s_card);
    mkdir(s_card, 0700); mkdir(s_packs, 0700);
    setenv("POCKET_CARD_DIR", s_card, 1);
    packs_drop_loaded();
    library_boot();
}
/* a reboot: the registry back to the built-in packs, the store read again from its flash */
static int reboot(void) { shelf_port_sim_cut(-1); g_sim_pstore_tear = 0; packs_drop_loaded(); return library_boot(); }
static bool card_put(const char *name, const pf_blob_t *b) { return pf_write_file(s_packs, name, b->b, b->n); }
static bool card_put_raw(const char *name, const void *p, size_t n) { return pf_write_file(s_packs, name, p, n); }

/* pack files of this world: its default species, habitat and manifest under new names. tail: two characters, a name's end */
static void mk_species(pf_blob_t *o, const char *tail) { char up[4] = { 0 }; for (int i = 0; i < 2 && tail[i]; i++) up[i] = tail[i] >= 'a' && tail[i] <= 'z' ? (char)(tail[i] - 32) : tail[i]; pf_variant(o, &S0, tail, up); }
static void mk_habitat(pf_blob_t *o, const char *tail) { char up[4] = { 0 }; for (int i = 0; i < 2 && tail[i]; i++) up[i] = tail[i] >= 'a' && tail[i] <= 'z' ? (char)(tail[i] - 32) : tail[i]; pf_variant(o, &H0, tail, up); }
static void mk_manifest(pf_blob_t *o, const char *tail) { pf_variant(o, &M0, tail, "M2"); }
static void goals_value(uint8_t *v, size_t *n, const char *const *words, int count) {   /* the GOALS / MODEL_GOALS value: u8 n, n x (u8 len, bytes) */
    size_t p = 0;
    v[p++] = (uint8_t)count;
    for (int i = 0; i < count; i++) { const size_t l = strlen(words[i]); v[p++] = (uint8_t)l; memcpy(v + p, words[i], l); p += l; }
    *n = p;
}
/* a species whose last goal word is another word (one a manifest of its own lists) */
static void mk_species_goal(pf_blob_t *o, const char *tail, const char *word) {
    mk_species(o, tail);
    const species_t *d = species_get(SPECIES_DEFAULT);
    const char *w[SPECIES_GOALS_MAX]; int n = d->n_goals;
    for (int i = 0; i < n; i++) w[i] = d->goal[i];
    w[n - 1] = word;
    uint8_t v[200]; size_t l; goals_value(v, &l, w, n);
    pf_rec_set(o, 0x10c, v, l); pf_fix(o);
}
static void mk_manifest_goal(pf_blob_t *o, const char *tail, const char *word) {   /* a manifest of a tag of its own that lists this word alone */
    mk_manifest(o, tail);
    const char *w[1] = { word };
    uint8_t v[40]; size_t l; goals_value(v, &l, w, 1);
    pf_rec_set(o, 0x302, v, l); pf_fix(o);
}
static void mk_slither(pf_blob_t *o, const char *tail) {     /* its way of moving rewritten to SLITHER (no firmware has the module): the one entry's mode, its numbers zero */
    mk_species(o, tail);
    size_t at, l;
    if (pf_rec_find(o, 0x110, &at, &l) && l >= 2 + 36) { uint8_t *v = o->b + at + 4; v[1] = 2; memset(v + 3, 0, 36); }
    pf_fix(o);
}

/* the scan, and a row by file name */
static char s_why[160];
static bool scan(void) { return library_scan(W, s_why, sizeof s_why); }
static const lib_row_t *row_of(const char *file) { for (int i = 0; i < W->n; i++) if (strcmp(W->row[i].file, file) == 0) return &W->row[i]; return NULL; }
static int row_index(const char *file) { for (int i = 0; i < W->n; i++) if (strcmp(W->row[i].file, file) == 0) return i; return -1; }
static const char *status_word(int s) { static const char *const N[] = { "INSTALLABLE", "INSTALLED", "UPDATE", "BUILT IN", "NO MODULE", "REFUSED" }; return s >= 0 && s <= LIB_REFUSED ? N[s] : "?"; }
static bool has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }
static char s_msg[160];
static lib_result_t install_file(const char *file) { const int i = row_index(file); return i < 0 ? LIB_FAILED : library_install(W, i, s_msg, sizeof s_msg); }

#define EXPECT(file, want, ...) do { \
    const lib_row_t *r_ = row_of(file); \
    if (!r_) PF("%s is not in the scan's rows (%d rows)", file, W->n); \
    if (r_->status != (want)) PF("%s is %s (\"%s\"), want %s", file, status_word(r_->status), r_->why, status_word(want)); \
    } while (0)
#define EXPECT_WHY(file, text) do { const lib_row_t *r_ = row_of(file); if (!r_ || !has(r_->why, text)) PF("%s: the reason \"%s\" does not say \"%s\"", file, r_ ? r_->why : "(no row)", text); } while (0)

static int counts(int *ns, int *nh, int *nm) { *ns = species_count(); *nh = habitat_count(); *nm = manifest_count(); return 0; }

/* ================================================================ 1: names and a hostile card */
static int t_names(void) {
    static const char *const OK[] = { "a.ptpk", "pt.gecko.leopard.ptpk", "A B-c_d.PTPK", "x.PtPk", "0.ptpk", "a b.ptpk", "a.b.c.ptpk",
                                      "123456789012345678901234567890123456789012.ptpk" };   /* 47 characters */
    static const char *const BAD[] = { "", ".ptpk", "ptpk", "a.ptp", "a.ptpkx", "../x.ptpk", "a/b.ptpk", "/a.ptpk", "a\\b.ptpk", "a:b.ptpk", "..a.ptpk", ".a.ptpk", " a.ptpk",
                                       "a..b.ptpk", "a\tb.ptpk", "a\nb.ptpk", "a\x01.ptpk", "\xc3\xa9.ptpk", "a%2f.ptpk", "a*.ptpk", "a?.ptpk", "a\"b.ptpk",
                                       "1234567890123456789012345678901234567890123.ptpk" };   /* 48 characters: no room for its NUL */
    for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) if (!card_name_ok(OK[i])) PF("the plain name \"%s\" is refused", OK[i]);
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) if (card_name_ok(BAD[i])) PF("the name \"%s\" is taken", BAD[i]);
    if (card_name_ok(NULL)) PF("a NULL name is taken");
    char nul[CARD_NAME_MAX + 8]; memset(nul, 'a', sizeof nul); if (card_name_ok(nul)) PF("a name with no end inside the buffer is taken");
    /* a card of everything a card is not supposed to hold */
    fresh();
    pf_blob_t good; mk_species(&good, "g1");
    card_put("good.ptpk", &good);
    card_put("UPPER.PTPK", &good);
    card_put(".hidden.ptpk", &good); card_put("noext", &good); card_put("x.txt", &good); card_put("bad\nname.ptpk", &good); card_put("a b.ptpk", &good);
    char p[900];
    snprintf(p, sizeof p, "%s/dir.ptpk", s_packs); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/fifo.ptpk", s_packs); mkfifo(p, 0600);
    { char tgt[900], lnk[900]; snprintf(tgt, sizeof tgt, "%s/good.ptpk", s_packs); snprintf(lnk, sizeof lnk, "%s/link.ptpk", s_packs); if (symlink(tgt, lnk) != 0) PF("no symlink made"); }
    { char lnk[900]; snprintf(lnk, sizeof lnk, "%s/etc.ptpk", s_packs); if (symlink("/etc/hostname", lnk) != 0) PF("no symlink made"); }
    char why[80];
    if (!card_open(why, sizeof why)) PF("no visit: %s", why);
    card_entry_t e[CARD_LIST_MAX]; bool more = true;
    const int n = card_list(e, CARD_LIST_MAX, &more, why, sizeof why);
    if (n != 3 || more) PF("the card lists %d files (more %d), want its 3 plain ones", n, more);
    if (strcmp(e[0].name, "UPPER.PTPK") || strcmp(e[1].name, "a b.ptpk") || strcmp(e[2].name, "good.ptpk")) PF("the listing is %s, %s, %s: not the plain files in byte order", e[0].name, e[1].name, e[2].name);
    uint8_t buf[PTPK_FILE_MAX + 1];
    static const char *const EVIL[] = { "../x.ptpk", "../../etc/passwd", "/etc/passwd", "dir.ptpk/../good.ptpk", "link.ptpk", "etc.ptpk", "fifo.ptpk", "dir.ptpk", "nothere.ptpk", ".hidden.ptpk", "a\nb.ptpk" };
    for (size_t i = 0; i < sizeof EVIL / sizeof EVIL[0]; i++) if (card_read(EVIL[i], buf, sizeof buf) >= 0) PF("card_read took the hostile name \"%s\"", EVIL[i]);
    if (card_read("good.ptpk", buf, sizeof buf) != (int)good.n || memcmp(buf, good.b, good.n)) PF("the plain file did not read back whole");
    if (card_read("good.ptpk", buf, good.n - 1) >= 0) PF("a file longer than the caller's cap was read");
    if (card_open(why, sizeof why)) PF("a second visit opened inside the first");
    card_close();
    if (card_read("good.ptpk", buf, sizeof buf) >= 0) PF("a read worked with no visit open");
    unsetenv("POCKET_CARD_DIR");
    if (card_slot() || card_open(why, sizeof why)) PF("a card is there with no POCKET_CARD_DIR");
    setenv("POCKET_CARD_DIR", s_card, 1);
    printf("selftest-library: names: %zu plain names taken, %zu hostile ones (a path, a stream, a dot, a space, a control byte, 48 characters, the wrong ending) refused; a card of a directory, a pipe, two links, a hidden file, an odd name and a stray .txt lists "
           "its 3 plain files in byte order, and card_read refuses %zu hostile names, a file past its cap and a read outside a visit\n", sizeof OK / sizeof OK[0], sizeof BAD / sizeof BAD[0], sizeof EVIL / sizeof EVIL[0]);
    return 0;
}

/* ================================================================ 2: a card of everything */
static int t_scan(void) {
    fresh();
    pf_blob_t b, h1, h2, m1;
    mk_species(&b, "s1");          card_put("b-species.ptpk", &b);
    card_put("b-species-copy.ptpk", &b);                       /* the very same file under another name */
    mk_habitat(&h1, "h1");         card_put("a-habitat.ptpk", &h1);
    mk_manifest(&m1, "k1");        card_put("c-model.ptpk", &m1);   /* (a tail that does not start with either model tag's last character: a tag is one manifest's) */
    pf_blob_t s_in_h2;
    mk_habitat(&h2, "h2");         card_put("d-habitat2.ptpk", &h2);
    mk_species(&s_in_h2, "s2");
    { uint32_t u = pf_uid(&h2); pf_set_habitats(&s_in_h2, &u, 1); }
    card_put("d-species-in-h2.ptpk", &s_in_h2);                  /* lives in a habitat that is only on the card: install that first */
    pf_blob_t bi;
    if (!pf_builtin(PTPK_KIND_SPECIES, &bi)) PF("no built-in species file");
    card_put("e-builtin-species.ptpk", &bi);
    if (!pf_builtin(PTPK_KIND_MODEL, &bi)) PF("no built-in model file");
    card_put("e-builtin-model.ptpk", &bi);
    pf_blob_t same_uid; pf_variant(&same_uid, &S0, "", "ZZ");  /* the built-in's UID_NS, another NAME: another file under a built-in's uid */
    card_put("e-same-uid-as-builtin.ptpk", &same_uid);
    pf_blob_t bad = b; bad.b[PTPK_HEADER_BYTES + 30] ^= 0x10;  card_put("f-corrupt.ptpk", &bad);
    pf_blob_t cut = b; cut.n -= 10;                            card_put("f-short.ptpk", &cut);
    card_put_raw("f-empty.ptpk", "", 0);
    { static uint8_t huge[5000]; memset(huge, 0xaa, sizeof huge); card_put_raw("f-huge.ptpk", huge, sizeof huge); }
    card_put_raw("f-text.ptpk", "hello, this is not a pack", 25);
    pf_blob_t unk = b; { const uint8_t v[1] = { 1 }; pf_rec_append(&unk, 0x7ff0 | 0x8000u, v, 1); pf_fix(&unk); }
    card_put("f-unknown-required.ptpk", &unk);                 /* an unknown REQUIRED record: refused whole */

    pf_blob_t sl; mk_slither(&sl, "sl");                       card_put("g-slither.ptpk", &sl);   /* a movement mode no firmware has */
    int ns0, nh0, nm0; counts(&ns0, &nh0, &nm0);
    if (!scan()) PF("the scan failed: %s", s_why);
    int ns1, nh1, nm1; counts(&ns1, &nh1, &nm1);
    if (ns0 != ns1 || nh0 != nh1 || nm0 != nm1) PF("a scan changed the registry (%d/%d/%d species/habitats/manifests -> %d/%d/%d)", ns0, nh0, nm0, ns1, nh1, nm1);
    if (W->n != 16 || W->more) PF("the scan listed %d files (more %d), want 16", W->n, W->more);
    for (int i = 1; i < W->n; i++) if (strcmp(W->row[i - 1].file, W->row[i].file) >= 0) PF("the rows are not in name order (%s before %s)", W->row[i - 1].file, W->row[i].file);
    EXPECT("b-species.ptpk", LIB_INSTALLABLE);
    EXPECT("b-species-copy.ptpk", LIB_INSTALLABLE);
    EXPECT("a-habitat.ptpk", LIB_INSTALLABLE);
    EXPECT("c-model.ptpk", LIB_INSTALLABLE);
    EXPECT("d-habitat2.ptpk", LIB_INSTALLABLE);
    EXPECT("d-species-in-h2.ptpk", LIB_REFUSED);  EXPECT_WHY("d-species-in-h2.ptpk", "INSTALL ITS HABITAT FIRST");
    if (!has(row_of("d-species-in-h2.ptpk")->why, row_of("d-habitat2.ptpk")->name)) PF("the species' reason does not name its habitat \"%s\": %s", row_of("d-habitat2.ptpk")->name, row_of("d-species-in-h2.ptpk")->why);
    char why_h2[LIB_WHY_MAX]; snprintf(why_h2, sizeof why_h2, "%s", row_of("d-species-in-h2.ptpk")->why);
    EXPECT("e-builtin-species.ptpk", LIB_BUILTIN);
    EXPECT("e-builtin-model.ptpk", LIB_BUILTIN);
    EXPECT("e-same-uid-as-builtin.ptpk", LIB_REFUSED);  EXPECT_WHY("e-same-uid-as-builtin.ptpk", "BUILT-IN");
    EXPECT("f-corrupt.ptpk", LIB_REFUSED);  EXPECT_WHY("f-corrupt.ptpk", "CRC");
    EXPECT("f-short.ptpk", LIB_REFUSED);    EXPECT_WHY("f-short.ptpk", "SHORT");
    EXPECT("f-empty.ptpk", LIB_REFUSED);    EXPECT_WHY("f-empty.ptpk", "EMPTY");
    EXPECT("f-huge.ptpk", LIB_REFUSED);     EXPECT_WHY("f-huge.ptpk", "5000");
    EXPECT("f-text.ptpk", LIB_REFUSED);
    EXPECT("f-unknown-required.ptpk", LIB_REFUSED);
    EXPECT("g-slither.ptpk", LIB_NO_MODULE);  EXPECT_WHY("g-slither.ptpk", "SLITHER");
    if (row_of("g-slither.ptpk")->needs != CAP_SLITHER) PF("the slither pack needs %08x, want SLITHER", (unsigned)row_of("g-slither.ptpk")->needs);
    const lib_row_t *rs = row_of("b-species.ptpk");
    if (rs->kind != PTPK_KIND_SPECIES || rs->uid != pf_uid(&b) || rs->size != b.n || !rs->name[0] || rs->crc != pf_rd32(b.b + 16)) PF("the species' row is kind %d uid %08x size %u crc %08x \"%s\"", rs->kind, (unsigned)rs->uid, (unsigned)rs->size, (unsigned)rs->crc, rs->name);
    if (row_of("a-habitat.ptpk")->kind != PTPK_KIND_HABITAT || row_of("c-model.ptpk")->kind != PTPK_KIND_MODEL) PF("the habitat's or the manifest's row has the wrong kind");
    if (row_of("e-builtin-species.ptpk")->uid != S0.b[8] + (S0.b[9] << 8) + (S0.b[10] << 16) + ((uint32_t)S0.b[11] << 24)) PF("the built-in species' row has not its uid");
    /* the page's own reading of a row that is refused for its HABITAT only: after the habitat is installed it is installable */
    if (install_file("d-habitat2.ptpk") != LIB_DONE_INSTALLED) PF("the second habitat did not install: %s", s_msg);
    if (!scan()) PF("the rescan failed: %s", s_why);
    EXPECT("d-species-in-h2.ptpk", LIB_INSTALLABLE);
    EXPECT("d-habitat2.ptpk", LIB_INSTALLED);
    printf("selftest-library: scan: 16 files, one visit - INSTALLABLE x5 (a species and its copy, two habitats, a manifest), BUILT IN x2, NO MODULE x1 (SLITHER), REFUSED x8 (a CRC, a short file, an empty one, 5000 bytes, text, an unknown REQUIRED record, a built-in's "
           "uid, a species whose habitat is only on the card: \"%s\"), the rows in name order, no pool slot taken; after its habitat is installed that species is installable\n", why_h2);
    return 0;
}

/* ================================================================ 3: install, keep, reboot */
static int t_install(void) {
    fresh();
    pf_blob_t s1, h1, m1, s2;
    mk_species(&s1, "s1"); card_put("s1.ptpk", &s1);
    mk_habitat(&h1, "h1"); card_put("h1.ptpk", &h1);
    mk_manifest(&m1, "k1"); card_put("m1.ptpk", &m1);
    mk_species(&s2, "s2"); { uint32_t u = pf_uid(&h1); pf_set_habitats(&s2, &u, 1); } card_put("s2.ptpk", &s2);
    if (!scan()) PF("scan: %s", s_why);
    const int ns0 = species_count(), nh0 = habitat_count(), nm0 = manifest_count();
    if (install_file("s1.ptpk") != LIB_DONE_INSTALLED || !has(s_msg, "INSTALLED")) PF("the species did not install: %s", s_msg);
    const species_t *p = species_by_uid(pf_uid(&s1));
    if (!p || p->builtin || p->id < 3 || species_count() != ns0 + 1) PF("an installed species is not loaded as a pack of its own (id %d)", p ? p->id : -1);
    if (library_installed_count() != 1 || pstore_count() != 1 || pstore_free() != PSTORE_SECTORS - 1) PF("the store holds %d / %d packs", library_installed_count(), pstore_count());
    const lib_inst_t *ip = library_installed_at(0);
    if (!ip || ip->uid != pf_uid(&s1) || !ip->loaded || ip->kind != PTPK_KIND_SPECIES || strcmp(ip->name, p->name) != 0) PF("the installed list does not show the species loaded under its name");
    if (!scan()) PF("rescan: %s", s_why);
    EXPECT("s1.ptpk", LIB_INSTALLED);
    if (install_file("s1.ptpk") != LIB_FAILED) PF("an installed pack installed again");
    if (library_add(W, s1.b, s1.n, s_msg, sizeof s_msg) != LIB_DONE_SAME) PF("the same file added again was not 'same'");
    if (install_file("h1.ptpk") != LIB_DONE_INSTALLED || install_file("m1.ptpk") != LIB_DONE_INSTALLED) PF("the habitat or the manifest did not install: %s", s_msg);
    if (!scan()) PF("rescan: %s", s_why);
    EXPECT("s2.ptpk", LIB_INSTALLABLE);
    if (install_file("s2.ptpk") != LIB_DONE_INSTALLED) PF("the species that lives in the installed habitat did not install: %s", s_msg);
    if (species_count() != ns0 + 2 || habitat_count() != nh0 + 1 || manifest_count() != nm0 + 1) PF("the registry holds %d/%d/%d, want +2 species, +1 habitat, +1 manifest", species_count(), habitat_count(), manifest_count());
    const habitat_t *hb = habitat_by_uid(pf_uid(&h1));
    const species_t *sp2 = species_by_uid(pf_uid(&s2));
    char why[200];
    if (!hb || !sp2 || !((sp2->habitats >> hb->id) & 1u)) PF("the installed species does not live in the installed habitat");
    tank_init(&s_t, 9);
    if (!tank_set_packs(&s_t, hb->id, sp2->id, why, sizeof why)) PF("a tank refused an installed species in an installed habitat: %s", why);
    if (!manifest_by_tag(manifest_by_uid(pf_uid(&m1))->tag)) PF("the installed manifest is not found by its tag");
    /* the card goes: the packs stay, now and after a reboot */
    unsetenv("POCKET_CARD_DIR"); rm_rf(s_card);
    int got = reboot();
    if (got != 4) PF("a reboot loaded %d of 4 installed packs", got);
    if (!species_by_uid(pf_uid(&s1)) || !species_by_uid(pf_uid(&s2)) || !habitat_by_uid(pf_uid(&h1)) || !manifest_by_uid(pf_uid(&m1))) PF("an installed pack is gone after a reboot without the card");
    if (library_installed_count() != 4) PF("the installed list holds %d, want 4", library_installed_count());
    for (int i = 0; i < 4; i++) if (!library_installed_at(i)->loaded) PF("installed pack %d (%s) is not loaded after the reboot", i, library_installed_at(i)->name);
    tank_init(&s_t, 10);
    hb = habitat_by_uid(pf_uid(&h1)); sp2 = species_by_uid(pf_uid(&s2));
    if (!tank_set_packs(&s_t, hb->id, sp2->id, why, sizeof why)) PF("a tank refused the installed pair after the reboot: %s", why);
    for (int k = 0; k < 600; k++) tank_tick(&s_t, 1.0f / 30, NULL);
    /* the store's order is the order of install; a species that comes BEFORE its habitat in the store must still load (the boot loads habitats first): update the
       habitat - its new version goes into a free sector after the species' - and reboot */
    setenv("POCKET_CARD_DIR", s_card, 1); mkdir(s_card, 0700); mkdir(s_packs, 0700);
    pf_blob_t h1b; pf_variant(&h1b, &h1, "", "H9"); card_put("h1b.ptpk", &h1b);
    if (!scan()) PF("rescan: %s", s_why);
    EXPECT("h1b.ptpk", LIB_UPDATE);
    int sec_h_before = -1, sec_s = -1;
    for (int i = 0; i < library_installed_count(); i++) { const lib_inst_t *q = library_installed_at(i); if (q->uid == pf_uid(&h1)) sec_h_before = q->sector; if (q->uid == pf_uid(&s2)) sec_s = q->sector; }
    if (install_file("h1b.ptpk") != LIB_DONE_UPDATED) PF("the habitat did not update: %s", s_msg);
    int sec_h_after = -1;
    for (int i = 0; i < library_installed_count(); i++) { const lib_inst_t *q = library_installed_at(i); if (q->uid == pf_uid(&h1)) sec_h_after = q->sector; }
    if (!(sec_h_after > sec_s) || sec_h_before == sec_h_after) PF("the habitat's update stayed in sector %d (the species' is %d): the order test needs the habitat after it", sec_h_after, sec_s);
    got = reboot();
    if (got != 4) PF("a reboot with the habitat's sector after its species' loaded %d of 4", got);
    if (!species_by_uid(pf_uid(&s2)) || !strstr(habitat_by_uid(pf_uid(&h1))->name, "H9")) PF("after the reboot the species is %s and the habitat is \"%s\" (the updated one ends in H9)", species_by_uid(pf_uid(&s2)) ? "there" : "gone", habitat_by_uid(pf_uid(&h1)) ? habitat_by_uid(pf_uid(&h1))->name : "gone");
    printf("selftest-library: install: a species, a habitat, a manifest and a species that lives in that habitat installed from the card (the registry +2/+1/+1, a tank takes the pair); the card gone (and its folder), a reboot loads all 4 from the store "
           "through the one loader, and so does a reboot that finds the species in sector %d before its habitat's updated version in sector %d; the installed list shows them loaded\n", sec_s, sec_h_after);
    return 0;
}

/* ================================================================ 4: update, 5: remove */
static int t_update_remove(void) {
    fresh();
    pf_blob_t s1, s1b, h1, s2, m2, sg;
    mk_species(&s1, "s1"); pf_variant(&s1b, &s1, "", "V2");
    card_put("v1.ptpk", &s1);
    if (!scan() || install_file("v1.ptpk") != LIB_DONE_INSTALLED) PF("v1 did not install: %s %s", s_why, s_msg);
    const char *name1 = species_by_uid(pf_uid(&s1))->name; char n1[PACK_NAME_MAX]; snprintf(n1, sizeof n1, "%s", name1);
    rm_rf(s_packs); mkdir(s_packs, 0700); card_put("v2.ptpk", &s1b);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("v2.ptpk", LIB_UPDATE);
    if (pf_uid(&s1) != pf_uid(&s1b) || pf_rd32(s1.b + 16) == pf_rd32(s1b.b + 16)) PF("the update fixture is not 'same uid, another file'");
    const int ns = species_count();
    if (install_file("v2.ptpk") != LIB_DONE_UPDATED || !has(s_msg, "UPDATED")) PF("the update failed: %s", s_msg);
    if (species_count() != ns || strcmp(species_by_uid(pf_uid(&s1))->name, n1) == 0) PF("the registry holds %d species (was %d), the species is still \"%s\"", species_count(), ns, species_by_uid(pf_uid(&s1))->name);
    if (library_installed_count() != 1 || pstore_count() != 1) PF("the store holds %d packs after an update, want 1", pstore_count());
    pstore_rec_t r; pstore_find(pf_uid(&s1), &r);
    uint8_t buf[PTPK_FILE_MAX];
    if (pstore_read(&r, buf, sizeof buf) != (int)s1b.n || memcmp(buf, s1b.b, s1b.n)) PF("the store does not hold the new version's bytes");
    if (!scan()) PF("rescan: %s", s_why);
    EXPECT("v2.ptpk", LIB_INSTALLED);
    reboot();
    if (strcmp(species_by_uid(pf_uid(&s1))->name, n1) == 0) PF("a reboot brought the old version back");
    /* remove */
    char why[200];
    if (!library_remove(pf_uid(&s1), why, sizeof why)) PF("the species did not remove: %s", why);
    if (species_by_uid(pf_uid(&s1)) || library_installed_count() != 0 || pstore_count() != 0) PF("a removed pack is still loaded or stored");
    reboot();
    if (species_by_uid(pf_uid(&s1))) PF("a removed pack came back at the reboot");
    if (library_remove(pf_uid(&s1), why, sizeof why)) PF("a pack that is not installed was 'removed'");
    const species_t *def = species_get(SPECIES_DEFAULT);
    if (library_remove(def->uid, why, sizeof why) || !species_by_uid(def->uid)) PF("a built-in pack was removed (%s)", why);
    if (library_remove(0, why, sizeof why) || library_remove(0xdeadbeefu, why, sizeof why)) PF("an unknown uid was 'removed'");
    /* dependents: a habitat an installed species lives in, a manifest whose goal word it needs */
    fresh();
    mk_habitat(&h1, "h1"); mk_species(&s2, "s2"); { uint32_t u = pf_uid(&h1); pf_set_habitats(&s2, &u, 1); }
    card_put("h1.ptpk", &h1); card_put("s2.ptpk", &s2);
    if (!scan() || install_file("h1.ptpk") != LIB_DONE_INSTALLED || !scan() || install_file("s2.ptpk") != LIB_DONE_INSTALLED) PF("the habitat and its species did not install: %s", s_msg);
    if (library_removable(pf_uid(&h1), why, sizeof why) || !has(why, "LIVES IN IT") || !has(why, species_by_uid(pf_uid(&s2))->name)) PF("the habitat was removable with its species on it (%s)", why);
    if (library_remove(pf_uid(&h1), why, sizeof why) || !habitat_by_uid(pf_uid(&h1)) || pstore_count() != 2) PF("the habitat was removed from under its species");
    if (!library_remove(pf_uid(&s2), why, sizeof why) || !library_remove(pf_uid(&h1), why, sizeof why)) PF("the species, then its habitat, did not remove: %s", why);
    if (habitat_by_uid(pf_uid(&h1)) || species_by_uid(pf_uid(&s2)) || pstore_count() != 0) PF("a removed habitat or species is still there");
    fresh();
    mk_manifest_goal(&m2, "q2", "climb"); mk_species_goal(&sg, "sc", "climb");
    card_put("m2.ptpk", &m2); card_put("sg.ptpk", &sg);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("sg.ptpk", LIB_REFUSED); EXPECT_WHY("sg.ptpk", "CLIMB");     /* (the loader's sentence, upper-cased for the glass) */
    EXPECT("m2.ptpk", LIB_INSTALLABLE);
    if (install_file("m2.ptpk") != LIB_DONE_INSTALLED) PF("the manifest did not install: %s", s_msg);
    if (!manifests_know_goal("climb")) PF("the installed manifest's goal word is unknown");
    if (!scan()) PF("rescan: %s", s_why);
    EXPECT("sg.ptpk", LIB_INSTALLABLE);
    if (install_file("sg.ptpk") != LIB_DONE_INSTALLED) PF("the species of the manifest's word did not install: %s", s_msg);
    if (library_remove(pf_uid(&m2), why, sizeof why) || !has(why, "GOAL") || !manifest_by_uid(pf_uid(&m2))) PF("the manifest was removed with a species on its words (%s)", why);
    if (!library_remove(pf_uid(&sg), why, sizeof why) || !library_remove(pf_uid(&m2), why, sizeof why)) PF("the species, then its manifest, did not remove: %s", why);
    if (manifests_know_goal("climb")) PF("a removed manifest's word is still known");
    printf("selftest-library: update and remove: the same uid in another file replaced the pack in the registry and the store (one record, the new bytes, kept at a reboot); a pack removed leaves the registry, the store and the next boot; a pack that is not installed, a built-in "
           "and an unknown uid are refused; a habitat an installed species lives in (\"%s\") and a manifest whose goal word it needs are refused until the species is removed\n", "LIVES IN IT: REMOVE IT FIRST");
    return 0;
}

/* ================================================================ 6: the store full, the pool full */
static int t_full(void) {
    fresh();
    char tail[3] = { 0 };
    pf_blob_t b;
    int installed = 0;
    for (int i = 0; i < PSTORE_SECTORS; i++) {
        tail[0] = 'a' + i / 10; tail[1] = '0' + i % 10;
        mk_species(&b, tail);
        if (library_add(W, b.b, b.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("pack %d of %d did not install: %s", i + 1, PSTORE_SECTORS, s_msg);
        installed++;
    }
    if (pstore_free() != 0 || library_free() != 0) PF("the store has %d sectors free after %d installs", pstore_free(), installed);
    mk_species(&b, "zz");
    card_put("z.ptpk", &b);
    const int ns = species_count();
    if (library_add(W, b.b, b.n, s_msg, sizeof s_msg) != LIB_FAILED || !has(s_msg, "STORE IS FULL")) PF("a pack went into a full store: %s", s_msg);
    if (species_count() != ns || species_by_uid(pf_uid(&b))) PF("a refused install changed the registry");
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("z.ptpk", LIB_REFUSED); EXPECT_WHY("z.ptpk", "STORE IS FULL");
    { pf_blob_t first; mk_species(&first, "a0"); pf_blob_t first2; pf_variant(&first2, &first, "", "Q2");
      if (library_add(W, first2.b, first2.n, s_msg, sizeof s_msg) != LIB_FAILED || !has(s_msg, "FULL")) PF("an update was written with no sector to put it beside (%s)", s_msg); }
    char why[100];
    pf_blob_t victim; mk_species(&victim, "a3");
    if (!library_remove(pf_uid(&victim), why, sizeof why)) PF("remove from a full store: %s", why);
    if (library_add(W, b.b, b.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("an install after a remove failed: %s", s_msg);
    if (!scan() || row_of("z.ptpk")->status != LIB_INSTALLED) PF("the card's z.ptpk is not INSTALLED after it was");
    printf("selftest-library: full: %d packs fill the store's %d sectors; the next is refused (\"%s\"), the registry unchanged, the card's row says STORE IS FULL, an update with no sector beside the old is refused too; one remove makes room for one install\n",
           installed, PSTORE_SECTORS, "THE STORE IS FULL: REMOVE A PACK FIRST");
    /* the pool: filled by the loader directly (packs_add: the simulator's folder, the tests), so the library meets a pool with no slot */
    fresh();
    int added = 0;
    for (int i = 0; i < 60; i++) {
        tail[0] = 'a' + i / 10; tail[1] = '0' + i % 10;
        mk_species(&b, tail);
        if (packs_add(b.b, b.n, PACKS_FILE, NULL, why, sizeof why) != PACKS_ADDED) break;
        added++;
    }
    if (added != PACKS_SPARE) PF("the species pool took %d spare packs, PACKS_SPARE is %d", added, PACKS_SPARE);
    mk_species(&b, "yy"); card_put("y.ptpk", &b);
    const int cnt = species_count();
    if (library_add(W, b.b, b.n, s_msg, sizeof s_msg) != LIB_FAILED || !has(s_msg, "NO ROOM")) PF("a pack was installed into a full pool: %s", s_msg);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("y.ptpk", LIB_REFUSED); EXPECT_WHY("y.ptpk", "NO ROOM");
    if (species_count() != cnt || pstore_count() != 0) PF("a refused install left a pack behind (registry %d -> %d, store %d)", cnt, species_count(), pstore_count());
    packs_drop_loaded();
    printf("selftest-library: full: with the species pool at its %d spare slots a pack is refused (\"%s\"), nothing stored, nothing loaded\n", PACKS_SPARE, row_of("y.ptpk")->why);
    return 0;
}

/* ================================================================ 7: a pack this firmware cannot move */
static int t_no_module(void) {
    fresh();
    /* the other world's species on this world's firmware: the simulator carries both, so its firmware is overridden to this world's own modules */
    pf_blob_t other;
    const uint32_t uid = PT_TERRARIUM ? SPECIES_UID_FISH : SPECIES_UID_GECKO;
    if (!pf_builtin_uid(uid, &other)) PF("no built-in file for the other world's species");
    pf_blob_t o2; pf_variant(&o2, &other, "o1", "O1");
    card_put("other.ptpk", &o2);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("other.ptpk", LIB_INSTALLABLE);                      /* the simulator's firmware has both modules: a pack for either is fine */
    firmware_caps_override(true, PT_TERRARIUM ? CAPS_FW_TERRARIUM : CAPS_FW_AQUARIUM);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("other.ptpk", LIB_NO_MODULE);
    const caps_t want = PT_TERRARIUM ? CAP_SWIM : CAP_WALK;
    if (row_of("other.ptpk")->needs != want) PF("the other world's pack needs %08x, want %08x", (unsigned)row_of("other.ptpk")->needs, (unsigned)want);
    EXPECT_WHY("other.ptpk", PT_TERRARIUM ? "SWIM" : "WALK");
    const int ns = species_count();
    if (install_file("other.ptpk") != LIB_FAILED || species_count() != ns || pstore_count() != 0) PF("a pack for a module this firmware lacks was installed (%s)", s_msg);
    if (!has(s_msg, PT_TERRARIUM ? "SWIM" : "WALK")) PF("the refusal does not name the module: %s", s_msg);
    if (library_add(W, o2.b, o2.n, s_msg, sizeof s_msg) != LIB_FAILED) PF("library_add took it");
    firmware_caps_override(false, 0);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("other.ptpk", LIB_INSTALLABLE);
    printf("selftest-library: wrong firmware: the other world's species is INSTALLABLE where the firmware has both modules, NO MODULE naming %s on a firmware of this world's modules alone (needs %08x), refused by install, nothing stored or loaded\n",
           PT_TERRARIUM ? "SWIM" : "WALK", (unsigned)want);
    return 0;
}

/* ================================================================ 8: power cuts */
typedef struct { int before, after, torn_dropped; } tally_t;
/* what the store and the registry say about uid after a reboot: 0 not installed, else the file's crc (and the registry and the store agree) */
static int state_after_boot(uint32_t uid, uint32_t *crc, const char *what) {
    pstore_rec_t r; uint8_t buf[PTPK_FILE_MAX];
    const bool stored = pstore_find(uid, &r);
    const bool loaded = species_by_uid(uid) != NULL;
    if (stored != loaded) { printf("FAIL: %s: %s: the store %s the pack and the registry %s it\n", s_name, what, stored ? "holds" : "lacks", loaded ? "holds" : "lacks"); return -1; }
    if (!stored) { *crc = 0; return 0; }
    const int n = pstore_read(&r, buf, sizeof buf);
    if (n < 0) { printf("FAIL: %s: %s: the stored pack does not read true after the boot\n", s_name, what); return -1; }
    *crc = pf_rd32(buf + 16);
    return 1;
}
static int cut_sweep(int op, int tear, tally_t *t) {         /* op 0 install, 1 update, 2 remove */
    pf_blob_t v1, v2; mk_species(&v1, "c1"); pf_variant(&v2, &v1, "", "C2");
    char what[80];
    for (int cut = 0; cut < 9; cut++) {
        fresh();
        if (op != 0 && library_add(W, v1.b, v1.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("the starting pack did not install: %s", s_msg);
        snprintf(what, sizeof what, "%s cut at write %d%s", op == 0 ? "install" : op == 1 ? "update" : "remove", cut, tear ? " (torn)" : "");
        g_sim_pstore_tear = tear; shelf_port_sim_cut(cut);
        bool ok = false; char why[100];
        if (op == 0) ok = library_add(W, v1.b, v1.n, s_msg, sizeof s_msg) == LIB_DONE_INSTALLED;
        else if (op == 1) ok = library_add(W, v2.b, v2.n, s_msg, sizeof s_msg) == LIB_DONE_UPDATED;
        else ok = library_remove(pf_uid(&v1), why, sizeof why);
        const bool unharmed = cut > 5;
        if (unharmed && !ok) PF("%s: an operation with no cut in its way failed (%s)", what, s_msg);
        reboot();
        uint32_t crc; const int st = state_after_boot(pf_uid(&v1), &crc, what);
        if (st < 0) return 1;
        const uint32_t c1 = pf_rd32(v1.b + 16), c2 = pf_rd32(v2.b + 16);
        bool is_before, is_after;
        if (op == 0) { is_before = st == 0; is_after = st == 1 && crc == c1; }
        else if (op == 1) { is_before = st == 1 && crc == c1; is_after = st == 1 && crc == c2; }
        else { is_before = st == 1 && crc == c1; is_after = st == 0; }
        if (!is_before && !is_after) PF("%s: the boot found state %d crc %08x: neither the state before nor the state after", what, st, (unsigned)crc);
        if (ok && !is_after) PF("%s: the library said it was done and the boot lost it", what);
        if (pstore_count() != (op == 0 ? is_after : op == 1 ? 1 : is_before)) PF("%s: the store holds %d records after the boot", what, pstore_count());
        if (is_before) t->before++; else t->after++;
        /* the store works again: a pack in, a pack out, whatever the cut left (a torn sector erased by the boot) */
        pf_blob_t x; mk_species(&x, "x1");
        if (library_add(W, x.b, x.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("%s: the store would not take a pack after the boot: %s", what, s_msg);
        if (!library_remove(pf_uid(&x), why, sizeof why)) PF("%s: the pack put in after the boot would not come out: %s", what, why);
        if (unharmed) break;
    }
    return 0;
}
static int t_cuts(void) {
    tally_t t = { 0, 0, 0 };
    for (int op = 0; op < 3; op++) for (int tear = 0; tear < 2; tear++) if (cut_sweep(op, tear, &t)) return 1;
    g_sim_pstore_tear = 0; shelf_port_sim_cut(-1);
    if (!t.before || !t.after) PF("the sweep found only one outcome (before %d, after %d)", t.before, t.after);
    printf("selftest-library: power cuts: an install, an update and a remove, each cut at every write and erase it makes and again torn half-way (%d + %d outcomes): every boot finds exactly the state before or the state after (%d before, %d after), "
           "the registry and the store agree, nothing torn is loaded, an unfinished update keeps one version, and the store takes and gives up a pack after each\n", t.before, t.after, t.before, t.after);
    return 0;
}

/* ================================================================ 9: a hostile store */
static bool flip_store(int sector, int off, uint8_t xor_) {
    uint8_t b[1];
    if (!pstore_port_read((uint32_t)sector * PSTORE_SECTOR_BYTES + (uint32_t)off, b, 1)) return false;
    const uint8_t c = (uint8_t)(b[0] ^ xor_);                        /* (bits may only fall in flash: clear what was set, or erase and write whole) */
    if ((c & b[0]) == c) return pstore_port_write((uint32_t)sector * PSTORE_SECTOR_BYTES + (uint32_t)off, &c, 1);
    uint8_t whole[PSTORE_SECTOR_BYTES];
    if (!pstore_port_read((uint32_t)sector * PSTORE_SECTOR_BYTES, whole, sizeof whole)) return false;
    whole[off] = c;
    return pstore_port_erase(sector) && pstore_port_write((uint32_t)sector * PSTORE_SECTOR_BYTES, whole, sizeof whole);
}
static int t_hostile_store(void) {
    fresh();
    pf_blob_t a, b, c;
    mk_species(&a, "a1"); mk_species(&b, "b1"); mk_species(&c, "c1");
    if (library_add(W, a.b, a.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED || library_add(W, b.b, b.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED || library_add(W, c.b, c.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("the starting packs did not install: %s", s_msg);
    pstore_rec_t ra, rb;
    /* one bit anywhere in a sector's header, or in its file: the pack is dropped at the next boot, the others kept, never a crash */
    int hits = 0;
    for (int bit = 0; bit < 8 * (PSTORE_HEADER + (int)b.n); bit += bit < 8 * PSTORE_HEADER ? 3 : 41) {          /* the header's bits close together, the file's far apart */
        fresh();
        library_add(W, a.b, a.n, s_msg, sizeof s_msg); library_add(W, b.b, b.n, s_msg, sizeof s_msg); library_add(W, c.b, c.n, s_msg, sizeof s_msg);
        pstore_find(pf_uid(&b), &rb);
        if (!flip_store(rb.sector, bit / 8, (uint8_t)(1u << (bit % 8)))) PF("could not flip bit %d", bit);
        reboot();
        if (!species_by_uid(pf_uid(&a)) || !species_by_uid(pf_uid(&c))) PF("a flipped bit in another pack's sector (bit %d) took a good pack with it", bit);
        const bool kept = species_by_uid(pf_uid(&b)) != NULL;
        pstore_rec_t q;
        if (kept) {                                      /* a flip that left the pack whole (the seq's bits are CRC'd, so this is a bit in the zero padding or past the file's end) cannot be a changed pack */
            uint8_t buf[PTPK_FILE_MAX];
            if (!pstore_find(pf_uid(&b), &q) || pstore_read(&q, buf, sizeof buf) != (int)b.n || memcmp(buf, b.b, b.n)) PF("bit %d of sector %d: the pack loaded as something else", bit, rb.sector);
        } else hits++;
        if (pstore_find(pf_uid(&b), &q) != kept) PF("bit %d: the store and the registry disagree about the pack", bit);
        if (library_installed_count() != (kept ? 3 : 2)) PF("bit %d: %d installed", bit, library_installed_count());
    }
    if (hits < 10) PF("only %d flips dropped the pack: the test is not reaching the header and the file", hits);
    /* a header that says more than there is, a file of another uid, a kind that is not the file's: dropped */
    fresh();
    library_add(W, a.b, a.n, s_msg, sizeof s_msg);
    pstore_find(pf_uid(&a), &ra);
    {   uint8_t sec[PSTORE_SECTOR_BYTES];
        pstore_port_read((uint32_t)ra.sector * PSTORE_SECTOR_BYTES, sec, sizeof sec);
        static const struct { const char *what; int off; uint32_t v; } MUT[] = { { "len 0xffff", 6, 0xffff }, { "len 0", 6, 0 }, { "len 21", 6, 21 }, { "seq 0", 8, 0 }, { "seq all ones", 8, 0xffffffffu },
                                                                                  { "uid 0", 12, 0 }, { "another uid", 12, 0x12345678u } };
        for (size_t k = 0; k < sizeof MUT / sizeof MUT[0]; k++) {
            uint8_t m[PSTORE_SECTOR_BYTES]; memcpy(m, sec, sizeof m);
            const int w = MUT[k].off == 6 ? 2 : 4;
            for (int i = 0; i < w; i++) m[MUT[k].off + i] = (uint8_t)(MUT[k].v >> (8 * i));
            const uint32_t crc = ptpk_crc32(m, 28);                                 /* (a header that is lying, with its CRC made good) */
            for (int i = 0; i < 4; i++) m[28 + i] = (uint8_t)(crc >> (8 * i));
            pstore_port_erase(ra.sector); pstore_port_write((uint32_t)ra.sector * PSTORE_SECTOR_BYTES, m, sizeof m);
            reboot();
            if (species_by_uid(pf_uid(&a)) || pstore_count() != 0) PF("a header with %s loaded a pack", MUT[k].what);
            pstore_port_erase(ra.sector); pstore_port_write((uint32_t)ra.sector * PSTORE_SECTOR_BYTES, sec, sizeof sec);   /* (the good sector again) */
        }
        reboot();
        if (!species_by_uid(pf_uid(&a))) PF("the good sector, put back, does not load");
    }
    /* garbage in every sector, from the dice: the boot drops it all and leaves a store that works */
    for (int round = 0; round < 40; round++) {
        uint8_t g[PSTORE_SECTOR_BYTES];
        for (int k = 0; k < PSTORE_SECTORS; k++) {
            for (int i = 0; i < PSTORE_SECTOR_BYTES; i++) g[i] = (uint8_t)rnd();
            if (round % 3 == 0) { memcpy(g, "PTPS", 4); g[4] = 1; g[5] = 0; }       /* ... some dressed as headers */
            if (round % 4 == 0) { g[6] = (uint8_t)(24 + rnd() % 200); g[7] = 0; }
            pstore_port_erase(k); pstore_port_write((uint32_t)k * PSTORE_SECTOR_BYTES, g, sizeof g);
        }
        reboot();
        if (pstore_count() != 0 || library_installed_count() != 0) PF("garbage in the store (round %d) left %d packs", round, pstore_count());
        pf_blob_t x; mk_species(&x, "x1");
        if (library_add(W, x.b, x.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED || !library_remove(pf_uid(&x), s_why, sizeof s_why)) PF("the store would not work after garbage (round %d)", round);
    }
    /* the same uid in two sectors (an update cut short): the newer stays - made by hand: the second copy written beside the first */
    fresh();
    library_add(W, a.b, a.n, s_msg, sizeof s_msg);
    pf_blob_t a2; pf_variant(&a2, &a, "", "N2");
    pstore_find(pf_uid(&a), &ra);
    {   const int fresh_sec = ra.sector == 0 ? 1 : 0;
        uint8_t h[PSTORE_HEADER]; memset(h, 0, sizeof h);
        memcpy(h, "PTPS", 4); h[4] = 1; h[6] = (uint8_t)a2.n; h[7] = (uint8_t)(a2.n >> 8);
        const uint32_t seq = ra.seq + 1, uid = pf_uid(&a2);
        for (int i = 0; i < 4; i++) { h[8 + i] = (uint8_t)(seq >> (8 * i)); h[12 + i] = (uint8_t)(uid >> (8 * i)); }
        h[16] = PTPK_KIND_SPECIES;
        const uint32_t crc = ptpk_crc32(h, 28); for (int i = 0; i < 4; i++) h[28 + i] = (uint8_t)(crc >> (8 * i));
        pstore_port_erase(fresh_sec); pstore_port_write((uint32_t)fresh_sec * PSTORE_SECTOR_BYTES + PSTORE_HEADER, a2.b, a2.n); pstore_port_write((uint32_t)fresh_sec * PSTORE_SECTOR_BYTES, h, sizeof h);
        reboot();
        if (pstore_count() != 1 || strstr(species_by_uid(pf_uid(&a))->name, "N2") == NULL) PF("two sectors of one uid: the boot kept %d records, name \"%s\"", pstore_count(), species_by_uid(pf_uid(&a)) ? species_by_uid(pf_uid(&a))->name : "(none)");
    }
    /* a stored pack that cannot load now is KEPT and listed NOT LOADED, and loads when what it needed arrives */
    fresh();
    pf_blob_t h1, s2; mk_habitat(&h1, "h1"); mk_species(&s2, "s2"); { uint32_t u = pf_uid(&h1); pf_set_habitats(&s2, &u, 1); }
    if (library_add(W, h1.b, h1.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED || library_add(W, s2.b, s2.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("the habitat and its species did not install: %s", s_msg);
    pstore_remove(pf_uid(&h1));                                                   /* (the habitat's sector erased behind the library's back) */
    reboot();
    if (species_by_uid(pf_uid(&s2)) || library_installed_count() != 1) PF("the species without its habitat was loaded or dropped (%d installed)", library_installed_count());
    const lib_inst_t *ip = library_installed_at(0);
    if (!ip || ip->loaded || ip->uid != pf_uid(&s2) || !ip->name[0]) PF("the stored species is not listed NOT LOADED under its name");
    char why[LIB_WHY_MAX];
    if (!library_installed_why(W, ip, why, sizeof why) || !has(why, "HABITAT")) PF("NOT LOADED has no reason that names the habitat: %s", why);
    if (library_add(W, h1.b, h1.n, s_msg, sizeof s_msg) != LIB_DONE_INSTALLED) PF("the habitat did not come back: %s", s_msg);
    if (!species_by_uid(pf_uid(&s2)) || !library_installed_at(0)->loaded || !library_installed_at(1)->loaded) PF("the species did not load when its habitat arrived");
    printf("selftest-library: a hostile store: %d bit flips in the header and the file of one sector each dropped that pack alone (the others kept, the store and the registry agreeing); lying headers (length 0, 21, 65535, seq 0, seq all ones, uid 0, another uid) with their "
           "CRC made good, 40 rounds of garbage in every sector (some dressed as headers) and two sectors of one uid never loaded a pack that does not read true and left a store that takes a pack; a stored species that cannot load is kept, listed NOT LOADED "
           "with its reason, and loads when its habitat is installed\n", hits);
    return 0;
}

/* ================================================================ 10: fuzz */
static int t_fuzz(void) {
    fresh();
    pf_blob_t base[3]; mk_species(&base[0], "z1"); mk_habitat(&base[1], "z2"); mk_manifest(&base[2], "z3");
    const int ns = species_count(), nh = habitat_count(), nm = manifest_count();
    int refused = 0, took = 0, rows_refused = 0;
    for (int i = 0; i < 1500; i++) {
        pf_blob_t m = base[rnd() % 3];
        const int mode = (int)(rnd() % 6);
        if (mode == 0) m.b[rnd() % m.n] ^= (uint8_t)(1u << (rnd() % 8));                                     /* a bit */
        else if (mode == 1) m.n = rnd() % (m.n + 1);                                                          /* a cut */
        else if (mode == 2) m.b[rnd() % m.n] = (uint8_t)rnd();                                                /* a byte */
        else if (mode == 3) { for (int k = 0; k < 4; k++) m.b[PTPK_HEADER_BYTES + rnd() % (m.n - PTPK_HEADER_BYTES)] ^= (uint8_t)rnd(); pf_fix(&m); }   /* the body mauled, the header made good */
        else if (mode == 4) { const size_t at = PTPK_HEADER_BYTES + rnd() % (m.n - PTPK_HEADER_BYTES); m.b[at] = (uint8_t)rnd(); pf_fix(&m); }
        else { for (size_t k = PTPK_HEADER_BYTES; k < m.n; k++) m.b[k] = (uint8_t)rnd(); pf_fix(&m); }          /* a body of noise */
        packs_probe_t pr; char why[200];
        const packs_dry_t r = packs_dry_check(m.b, m.n, &W->scratch, &pr, why, sizeof why);
        if (species_count() != ns || habitat_count() != nh || manifest_count() != nm) PF("a dry check (iteration %d) changed the registry", i);
        if (r == PACKS_DRY_BAD) refused++;
        const lib_result_t lr = library_add(W, m.b, m.n, s_msg, sizeof s_msg);
        if (lr != LIB_FAILED) {                                   /* a mutant that is a good pack: in the store AND the registry, and out again */
            took++;
            const uint32_t uid = pf_rd32(m.b + 8);
            if (!pstore_find(uid, NULL) || !(species_by_uid(uid) || habitat_by_uid(uid) || manifest_by_uid(uid))) PF("an install that said done (iteration %d) left the pack out of the store or the registry", i);
            char w2[100]; if (!library_remove(uid, w2, sizeof w2)) PF("an installed fuzz pack would not remove (iteration %d): %s", i, w2);
        }
        if (pstore_count() != 0 || species_count() != ns || habitat_count() != nh || manifest_count() != nm) PF("iteration %d left %d packs in the store and %d/%d/%d in the registry", i, pstore_count(), species_count(), habitat_count(), manifest_count());
    }
    /* the same mutants as the card's files, through the scan */
    for (int round = 0; round < 6; round++) {
        rm_rf(s_card); mkdir(s_card, 0700); mkdir(s_packs, 0700);
        for (int i = 0; i < CARD_LIST_MAX; i++) {
            pf_blob_t m = base[rnd() % 3];
            if (i % 3 == 0) m.b[rnd() % m.n] ^= (uint8_t)(1u << (rnd() % 8)); else if (i % 3 == 1) m.n = rnd() % (m.n + 1); else { m.b[PTPK_HEADER_BYTES + rnd() % (m.n - PTPK_HEADER_BYTES)] = (uint8_t)rnd(); pf_fix(&m); }
            char nm_[40]; snprintf(nm_, sizeof nm_, "m%02d.ptpk", i);
            card_put(nm_, &m);
        }
        if (!scan() || W->n != CARD_LIST_MAX) PF("the scan of 32 mutants failed or listed %d (%s)", W->n, s_why);
        for (int i = 0; i < W->n; i++) rows_refused += W->row[i].status == LIB_REFUSED;
        if (species_count() != ns || habitat_count() != nh || manifest_count() != nm) PF("a scan of mutants changed the registry");
    }
    /* a card of a thousand files: 32 listed, in byte order, and it says there were more */
    rm_rf(s_card); mkdir(s_card, 0700); mkdir(s_packs, 0700);
    for (int i = 0; i < 1000; i++) { char nm_[40]; snprintf(nm_, sizeof nm_, "f%04d.ptpk", (int)((i * 7919) % 1000)); card_put_raw(nm_, "x", 1); }
    pf_blob_t g; mk_species(&g, "g9"); card_put("a-good.ptpk", &g);
    if (!scan()) PF("scan of a full card: %s", s_why);
    if (W->n != LIB_ROWS_MAX || !W->more) PF("a card of 1001 files lists %d (more %d), want %d and more", W->n, W->more, LIB_ROWS_MAX);
    if (strcmp(W->row[0].file, "a-good.ptpk") || W->row[0].status != LIB_INSTALLABLE || strcmp(W->row[1].file, "f0000.ptpk") || strcmp(W->row[31].file, "f0030.ptpk")) PF("the listing of 1001 files is not the first 32 in byte order (%s, %s ... %s)", W->row[0].file, W->row[1].file, W->row[31].file);
    printf("selftest-library: fuzz: 1500 mutants (a bit, a cut, a byte, a mauled body, noise) through the dry check, through install and as 6 x 32 files of a card: none crashed, every refusal changed neither registry nor store (%d refused by the dry check, %d went in and out again, %d REFUSED rows); a card of 1001 files lists "
           "the first %d in byte order and says there are more\n", refused, took, rows_refused, LIB_ROWS_MAX);
    return 0;
}

/* ================================================================ 11: a tank on an installed pack */
#define SAVE_BYTES 2160                    /* sizeof(save_t) today (progression.c) */
static long file_read(uint8_t *buf, size_t max) { FILE *f = fopen(getenv("POCKET_TANK_SAVE"), "rb"); if (!f) return -1; const size_t n = fread(buf, 1, max, f); fclose(f); return (long)n; }
static int t_tank(void) {
    fresh();
    const char *sav = sim_tmp("library.sav"); setenv("POCKET_TANK_SAVE", sav, 1); remove(sav);
    pf_blob_t h1, s4, s4b;
    mk_habitat(&h1, "t2"); mk_species(&s4, "t4"); { uint32_t u = pf_uid(&h1); pf_set_habitats(&s4, &u, 1); }   /* a species that lives in the installed habitat alone */
    pf_variant(&s4b, &s4, "", "U2");
    card_put("h1.ptpk", &h1); card_put("s4.ptpk", &s4);
    if (!scan() || install_file("h1.ptpk") != LIB_DONE_INSTALLED || !scan() || install_file("s4.ptpk") != LIB_DONE_INSTALLED) PF("the habitat and the species did not install: %s %s", s_why, s_msg);
    const uint32_t suid = pf_uid(&s4), huid = pf_uid(&h1);
    char why[200];
    tank_init(&s_t, 31); progression_wake(&s_t, 0);
    {   const species_t *sp = species_by_uid(suid); const habitat_t *hb = habitat_by_uid(huid);
        if (!sp || !hb || !tank_set_packs(&s_t, hb->id, sp->id, why, sizeof why)) PF("a tank refused the installed pair: %s", why); }
    static uint8_t a[4096], b[4096];
    for (int k = 0; k < 90; k++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (s_t.packs_missing) PF("a tank on installed packs is waiting");
    progression_save(&s_t);                                                       /* the tank as it lives, its pair named by uid */
    if (file_read(a, sizeof a) != SAVE_BYTES) PF("the save is %ld bytes", file_read(a, sizeof a));
    if (pf_rd32(a + 2104) != suid || pf_rd32(a + 2108) != huid || a[1957] != 0 || a[1958] != 0) PF("the save does not name the installed pair by uid (species %08x habitat %08x one-byte ids %d %d)", (unsigned)pf_rd32(a + 2104), (unsigned)pf_rd32(a + 2108), a[1957], a[1958]);
    /* the PACKS page's REMOVE: the tank is told first (the pair is read from the registry as it stands), then the pack goes */
    if (progression_pack_leaving(&s_t, 0x12345678u)) PF("a pack the tank does not live on paused it");
    if (!progression_pack_leaving(&s_t, suid)) PF("the tank did not wait when the pack it lives on was about to go");
    if (progression_pack_leaving(&s_t, suid)) PF("a tank already waiting was told again");
    if (!library_remove(suid, why, sizeof why)) PF("the species did not remove: %s", why);
    if (!s_t.packs_missing || s_t.pack_id != SPECIES_DEFAULT || s_t.habitat_id != HABITAT_DEFAULT) PF("the tank is not waiting on the defaults");
    const double clock0 = s_t.clock;
    for (int k = 0; k < 600; k++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (s_t.clock != clock0 || !s_t.packs_missing) PF("a tank whose pack is gone moved on (clock %.2f -> %.2f)", clock0, s_t.clock);
    notice_sync(&s_t);
    for (int k = 0; k < 120 && !notice_current(); k++) notice_tick(&s_t, 1.0f / 60, false);
    if (!notice_current() || notice_current()->kind != NOTICE_PACK_MISSING) PF("no PACK MISSING card for the removed pack");
    if (!progression_packs_missing_name(&s_t)[0] || strcmp(progression_packs_missing_name(&s_t), species_get(SPECIES_DEFAULT)->name) == 0) PF("the card does not name the removed pack (\"%s\")", progression_packs_missing_name(&s_t));
    progression_save(&s_t);
    if (file_read(b, sizeof b) != SAVE_BYTES || memcmp(a + 1956, b + 1956, SAVE_BYTES - 1956) != 0) PF("a save while the pack is gone changed the pair or the tail");
    /* a reboot without it: the saved tank loads waiting; the pack installed again from the card and the tick's own look (once a second) resumes it */
    reboot();
    tank_init(&s_t, 32); progression_wake(&s_t, 0);
    if (!s_t.packs_missing) PF("a tank saved on a removed pack loaded as if nothing were missing");
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("s4.ptpk", LIB_INSTALLABLE);
    if (install_file("s4.ptpk") != LIB_DONE_INSTALLED) PF("the pack did not install again: %s", s_msg);
    progression_tick(&s_t, 0.3f);
    if (!s_t.packs_missing) PF("the tank resumed before the tick's look (a second)");
    progression_tick(&s_t, 1.0f);
    if (s_t.packs_missing || tank_species(&s_t)->uid != suid || tank_habitat(&s_t)->uid != huid) PF("the tank did not resume on the reinstalled pack (missing %d)", s_t.packs_missing);
    notice_tick(&s_t, 1.0f / 60, false);
    if (notice_current() || notice_pending()) PF("the PACK MISSING card stayed after the pack came back");
    const double clock1 = s_t.clock;
    for (int k = 0; k < 300; k++) { tank_tick(&s_t, 1.0f / 30, NULL); progression_tick(&s_t, 1.0f / 30); }
    if (!(s_t.clock > clock1 + 9)) PF("a resumed tank does not live (clock %.2f -> %.2f)", clock1, s_t.clock);
    /* an UPDATE of the pack it lives on: told first, the new version in, and it resumes at once */
    card_put("s4b.ptpk", &s4b);
    if (!scan()) PF("scan: %s", s_why);
    EXPECT("s4b.ptpk", LIB_UPDATE);
    if (!progression_pack_leaving(&s_t, suid)) PF("the tank did not wait for its pack's update");
    if (install_file("s4b.ptpk") != LIB_DONE_UPDATED) PF("the update failed: %s", s_msg);
    progression_pack_arrived(&s_t);
    if (s_t.packs_missing || !strstr(tank_species(&s_t)->name, "U2")) PF("the tank did not come back on the updated pack (\"%s\")", tank_species(&s_t)->name);
    progression_save(&s_t);
    /* a reboot with the pack installed and the card long gone: the saved tank loads as itself */
    unsetenv("POCKET_CARD_DIR"); rm_rf(s_card);
    reboot();
    tank_init(&s_t, 33); progression_wake(&s_t, 0);
    if (s_t.packs_missing || tank_species(&s_t)->uid != suid || tank_habitat(&s_t)->uid != huid) PF("a tank saved on installed packs did not load on them after a reboot with no card");
    setenv("POCKET_TANK_SAVE", sim_tmp("tank.sav"), 1);
    printf("selftest-library: a tank on an installed pair: the save names it by uid (one-byte ids 0); REMOVE of the pack it lives on pauses it first (PACK MISSING, the defaults drawn, the clock unmoved over 20 s of frames, the pair and the tail written back unchanged); "
           "a reboot without the pack loads it waiting; installed again from the card it resumes at the tick's next look, and the card goes; an update of the pack it lives on pauses it and resumes it at once on the new version; a reboot with the pack installed and no card loads it whole\n");
    return 0;
}

/* ================================================================ */
int selftest_library(void) {
    snprintf(s_card, sizeof s_card, "%s", sim_tmp("lib-card"));
    snprintf(s_packs, sizeof s_packs, "%s/%s", s_card, CARD_FOLDER);
    snprintf(s_store, sizeof s_store, "%s", sim_tmp("lib-packstore.bin"));
    setenv("POCKET_TANK_PACKSTORE", s_store, 1);
    packs_drop_loaded();
    if (!pf_builtin(PTPK_KIND_SPECIES, &S0) || !pf_builtin(PTPK_KIND_HABITAT, &H0) || !pf_builtin(PTPK_KIND_MODEL, &M0)) PF("this world's built-in species, habitat or manifest file is not there");
    W = card_mem_alloc(sizeof *W);
    if (!W) PF("no working block");
    const int r = t_names() || t_scan() || t_install() || t_update_remove() || t_full() || t_no_module() || t_cuts() || t_hostile_store() || t_fuzz() || t_tank();
    firmware_caps_override(false, 0); shelf_port_sim_cut(-1); g_sim_pstore_tear = 0;
    unsetenv("POCKET_CARD_DIR");
    packs_drop_loaded();
    remove(s_store);
    library_boot();
    card_mem_free(W); W = NULL;
    if (r) return 1;
    printf("selftest-library: sizes: the work block %zu B (in PSRAM on the board), the store %d sectors of %d B, PACKS_SPARE %d species + %d habitats and MANIFEST_SPARE %d here (the firmware's with the card library: 2 + 2 + 1), the installed table %zu B\n",
           sizeof(lib_work_t), PSTORE_SECTORS, PSTORE_SECTOR_BYTES, PACKS_SPARE, PACKS_SPARE, MANIFEST_SPARE, sizeof(lib_inst_t) * PSTORE_SECTORS);
    printf("selftest-library OK\n");
    return 0;
}
#else
int selftest_library(void) { return 0; }
#endif
