#define _DEFAULT_SOURCE   /* nothing special; -std=c11 */
/* release_check.c - issue #4 phase 2: the pack semantics of a RELEASE IMAGE, which the simulator's own bins cannot show.
 *
 * Every fishsim carries both worlds' built-in packs (-DPT_ALL_PACKS=1) and a firmware_caps() that is both worlds' union; a firmware
 * image carries its own world's alone, its firmware_caps() is its own world's constant (caps.h CAPS_FW_*), and the other world's
 * pack is refused AT LOAD, naming the module it moves by. This program is that configuration, built from the common sources
 * alone (no SDL, no LVGL, no tank): `make check-release` builds and runs it once for each world (sim/Makefile).
 *
 *   release-check-<world>:  the registries hold this world's packs and model manifest only; firmware_caps() is this world's
 *   constant and a set the other world's engine would not recognise; model_provides() is this world's; the other world's
 *   built-in pack file (../packs/, `make packs` writes them) is refused at load naming the module - whether or not its habitat
 *   is loaded; the other world's habitat and manifest load (they need nothing); this world's pair is held and the other world's
 *   habitat refused for a reason; a pack for the tests is never in the table.
 *
 * Run from sim/ (the packs are read from ../packs). Exit 0: all of it holds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packs/ptpk.h"
#include "packs/compat.h"
#include "packs/manifest.h"
#include "packs/builtin.h"
#include "version.h"

#define PF(...) do { printf("FAIL: release-check (%s): ", PT_WORLD_NAME); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

#if PT_ALL_PACKS
#error "release_check.c is the release configuration: build it with -DPT_ALL_PACKS=0 (sim/Makefile check-release)"
#endif

static uint8_t *slurp(const char *name, size_t *n) {
    char path[300]; snprintf(path, sizeof path, "../packs/%s.ptpk", name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (b && fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}
static packs_result_t offer(const char *name, char *why, size_t why_n) {
    size_t n = 0; uint8_t *b = slurp(name, &n);
    if (!b) { snprintf(why, why_n, "cannot read ../packs/%s.ptpk", name); return PACKS_REFUSED; }
    why[0] = 0;
    const packs_result_t r = packs_add(b, n, PACKS_FILE, NULL, why, why_n);
    free(b);
    return r;
}

int main(void) {
    char why[300];
    const char *own_s = PT_TERRARIUM ? "pt.gecko.leopard" : "pt.fish.roster", *own_h = PT_TERRARIUM ? "pt.habitat.desert" : "pt.habitat.freshwater";
    const char *other_s = PT_TERRARIUM ? "pt.fish.roster" : "pt.gecko.leopard", *other_h = PT_TERRARIUM ? "pt.habitat.freshwater" : "pt.habitat.desert";
    const char *own_m = PT_TERRARIUM ? "pt.model.lizard_v1" : "pt.model.fish_v3m", *other_m = PT_TERRARIUM ? "pt.model.fish_v3m" : "pt.model.lizard_v1";
    const char *module = PT_TERRARIUM ? "SWIM" : "WALK";                       /* what the other world's species moves by and this firmware has not */
    const caps_t want_fw = PT_TERRARIUM ? CAPS_FW_TERRARIUM : CAPS_FW_AQUARIUM;

    /* what the image carries */
    if (species_count() != 1 || habitat_count() != 1 || manifest_count() != 1 || PTPK_BUILTIN_N != 3)
        PF("the registries hold %d species, %d habitats, %d manifests (table %d): one of each is a release image's", species_count(), habitat_count(), manifest_count(), PTPK_BUILTIN_N);
    if (firmware_caps() != want_fw || firmware_caps() != CAPS_FW) PF("firmware_caps() is %08x, this world's constant is %08x", (unsigned)firmware_caps(), (unsigned)want_fw);
    if (firmware_caps() & (PT_TERRARIUM ? CAPS_FW_AQUARIUM & ~CAPS_FW_TERRARIUM : CAPS_FW_TERRARIUM & ~CAPS_FW_AQUARIUM)) PF("firmware_caps() carries a capability only the other world's engine has");
    const model_manifest_t *m = model_provides();
    if (!m || strcmp(m->tag, PT_MODEL_TAG) != 0) PF("model_provides() is %s, the build's model is %s", m ? m->tag : "none", PT_MODEL_TAG);
    if (manifest_by_tag(PT_TERRARIUM ? PT_MODEL_TAG_AQUARIUM : PT_MODEL_TAG_TERRARIUM)) PF("the other world's model manifest is carried by this image");
    if (species_get(SPECIES_TEST_WARM) || habitat_get(HABITAT_TEST_WARM)) PF("a pack for the tests is carried by this image");
    const species_t *s = species_get(SPECIES_DEFAULT); const habitat_t *h = habitat_get(HABITAT_DEFAULT);
    if (!s || !h || !packs_compatible(s, h, why, sizeof why)) PF("the world's default pair is not held: %s", why);

    /* a release image has no spare slot (ptpk.h PACKS_SPARE 0: the card library's, phase 4): nothing found at run time is added, and the
       other world's species is refused - with no room (the slot is asked for first), never into the registry */
    const char *others[] = { other_s, other_h, other_m };
    for (int k = 0; k < 3; k++) {
        if (offer(others[k], why, sizeof why) != PACKS_REFUSED) PF("%s loaded into an image with no spare slot", others[k]);
        if (!strstr(why, "no room")) PF("%s was refused for \"%s\", want \"no room\" (no spare slot)", others[k], why);
    }
    if (species_count() != 1 || habitat_count() != 1 || manifest_count() != 1) PF("a refused pack changed the registry (%d / %d / %d)", species_count(), habitat_count(), manifest_count());
    if (offer(own_s, why, sizeof why) != PACKS_SAME || offer(own_h, why, sizeof why) != PACKS_SAME || offer(own_m, why, sizeof why) != PACKS_SAME) PF("the world's own packs, found again in a folder, were not the same files: %s", why);
    if (model_provides() != m) PF("another manifest took the place of this build's model's");
    if (!packs_compatible(s, h, why, sizeof why)) PF("the world's own pair is no longer held: %s", why);
    (void)module;

    packs_drop_loaded();
    if (species_count() != 1 || habitat_count() != 1 || manifest_count() != 1) PF("packs_drop_loaded left %d / %d / %d", species_count(), habitat_count(), manifest_count());
    printf("release-check (%s): one pack of each kind and the %s model's manifest only; firmware_caps %08x is this world's constant and the other's engine's capabilities are not in it; "
           "the other world's species, habitat and model manifest are all refused (no spare slot: the card library's, phase 4) and the registry is unchanged; the world's own pair held\n",
           PT_WORLD_NAME, PT_MODEL_TAG, (unsigned)firmware_caps());
    return 0;
}
