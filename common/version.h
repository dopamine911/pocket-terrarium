/* version.h - the tank's release number, the one place it lives (2026-09-29).
 *
 * Strato: "officially the tank should have a true version number so people
 * can easily understand what version they are on ... I would still consider
 * this alpha, pre 1.0". Everything shipped from the 09-11 launch to 09-29 is
 * the 0.1 series (the changelog keeps its git ids); 0.2.0 is the first
 * release under the batched schedule.
 *
 *   0.MINOR.0   a batched release (the number people talk about)
 *   0.x.PATCH   a fix that has to ship between releases
 *   1.0         Strato's call: stable saves, the core loop settled
 *
 * Bump it in the release commit, and write the changelog entry
 * (site/changelog.json) with the same number. The build id - the git
 * describe of the tree it was built from - rides beside it everywhere
 * (version_port_string), so a report still pins the exact build. The save
 * records PT_RELEASE_NUM of the build that wrote it (progression.c). */
#ifndef VERSION_H
#define VERSION_H
#include <string.h>
#include "world.h"   /* PT_TERRARIUM: the model identity is per world */

/* main is the 0.4 line (2026-10-05, Strato: "bump main to 0.4.0 dev"): the
 * number sits above every 0.3.x, so the Wi-Fi update never offers a 0.3.x
 * release (a 0.3.3 included) to a board on a main build (update.c newer()).
 * 0.3.x fixes live on branch release-0.3.2 and merge forward - keep THIS
 * number in that merge. At the 0.4.0 release the stage goes back to "alpha". */
#define PT_RELEASE_MAJOR 0
#define PT_RELEASE_MINOR 4
#define PT_RELEASE_PATCH 0
#define PT_RELEASE_HOTFIX 0                    /* a fourth part (2026-10-21): a fix between patches - 0.3.8.1 - that
                                                  must not burn the next number. 0 = none, and the string has three parts */
#define PT_RELEASE       "0.4.0"
#define PT_RELEASE_STAGE "alpha"               /* pre-1.0: shown beside the number ("alpha" in a release; "dev" on main between them) */
/* the packed number the tank compares (update.c newer(), the save's stamp):
 * a byte each, major.minor.patch.hotfix. Until 0.3.8 it was three bytes
 * (major << 16 | minor << 8 | patch): every four-byte number is larger than
 * every three-byte one, so a tank on 0.3.8 (0x000308) sees 0.3.8.1
 * (0x00030801) as newer, and 0.3.9 (0x00030900) ranks above both. A manifest's
 * min_from under 0x10000 is read as the old packing (update.c). Any line that
 * merges this forward (0.4) takes the four bytes with it, or its 0.4.0 would
 * rank below 0.3.8.1. tools/make_ota_manifest.py packs the same way. */
#define PT_RELEASE_NUM   ((PT_RELEASE_MAJOR << 24) | (PT_RELEASE_MINOR << 16) | (PT_RELEASE_PATCH << 8) | PT_RELEASE_HOTFIX)

/* the model this build ships with (world.h PT_MODEL_FILE_<WORLD>): its tag,
 * byte length and sha256, per world. The tank's model trailer
 * (firmware/main/model_trailer.c) is healed from these at boot when a cable
 * install left none, and an update manifest naming another model tag says
 * "needs the cable" until the model path lands (docs/OTA.md, phase two).
 * tools/model_trailer.py --check refuses a build whose world's model file
 * disagrees with these: update them when a world's model changes. */
#define PT_MODEL_TAG_AQUARIUM     "v3m"
#define PT_MODEL_LEN_AQUARIUM     7557640u
#define PT_MODEL_SHA256_AQUARIUM  "50bed3e16a6b11f3496dde21d8784da9067f40ec9f0a77cf97f60a95acb80f6b"
#define PT_MODEL_TAG_TERRARIUM    "lizard_v1"   /* docs/retrain-lizard.md: the focused repair's seed 2 */
#define PT_MODEL_LEN_TERRARIUM    7558660u
#define PT_MODEL_SHA256_TERRARIUM "d86e2f6194a2c5aa302b46502a2e20d57e06ba465d2d9f77dd85c40f9643d017"
#define PT_MODEL_TAG     PT_PICK(PT_MODEL_TAG_)       /* this build's world's (world.h PT_PICK) */
#define PT_MODEL_LEN     PT_PICK(PT_MODEL_LEN_)
#define PT_MODEL_SHA256  PT_PICK(PT_MODEL_SHA256_)

/* the board this build is for (2026-10-02: 0.3.0 ships three - docs/BOARDS.md).
 * One source tree, one release number, but an image per board: a tank fetches
 * its own board's update manifest (latest-<PT_BOARD>.json), refuses a manifest
 * that names another board, and refuses an image whose board marker
 * (firmware/main/net_port_esp.c, right after the app descriptor) is not its
 * own. The installer offers one manifest per board. PT_BOARD is a file-name
 * safe id; PT_BOARD_NAME is the maker's name for it. */
#if defined(TANK_ROUND)
#define PT_BOARD         "round175c"
#define PT_BOARD_NAME    "ESP32-S3-Touch-AMOLED-1.75C"
#elif defined(TANK_WATCH)
#define PT_BOARD         "watch206"
#define PT_BOARD_NAME    "ESP32-S3-Touch-AMOLED-2.06"
#elif defined(TANK_SQUARE)
#define PT_BOARD         "square216"
#define PT_BOARD_NAME    "ESP32-S3-Touch-AMOLED-2.16"
#else
#define PT_BOARD         "amoled18"
#define PT_BOARD_NAME    "ESP32-S3-Touch-AMOLED-1.8"
#endif
/* a board id in the keeper's words, for the glass (0.3.9.3: the WRONG VERSION
 * card, a board's own version offered by CHECK FOR UPDATES); NULL = unknown */
static inline const char *pt_board_words(const char *id) {
    if (!id) return NULL;
    return !strcmp(id, "amoled18") ? "1.8 INCH" : !strcmp(id, "round175c") ? "1.75 ROUND" :
           !strcmp(id, "watch206") ? "2.06 WATCH" : !strcmp(id, "square216") ? "2.16 SQUARE" : NULL;
}
#define PT_BOARD_MAGIC   "TRBOARD"             /* the image's board marker begins with these 8 bytes (with the NUL); pocket-terrarium's,
                                                  so a pocket-tank refuses our image over the air and we refuse its ("PTBOARD") */
#define PT_BOARD_MARKER_OFFSET 0x120           /* ... at this offset in the app image: the image header (24) + the
                                                  first segment's header (8) + esp_app_desc_t (256) */
typedef struct { char magic[8]; char board[24]; } pt_board_marker_t;   /* the marker (firmware/main/board_marker.c): every image
                                                  carries one; tools/pt_boards.py reads it, the update's download checks it */

/* pocket-terrarium: the networked features, off (PLAN.md D4: the port is a
 * proof of concept). 0 = compiled out: no Wi-Fi, no update check, no Improv
 * provisioning, no clock from the internet; no tank link (ESP-NOW), so no
 * visits, gifts or passports; no VISITORS square on the keeper page. The
 * networking sources leave the build (sim/Makefile NET, the firmware's
 * CMakeLists read this line) and the linker drops the Wi-Fi / ESP-NOW / lwip /
 * TLS / HTTP / OTA stacks. `make NET=1` builds the networked bin for the
 * networking tests (sim/Makefile check-net). Turning it on for a board needs
 * our own OTA manifest + key and a link identity of our own, or a terrarium
 * would update into a pocket-tank and trade with one. */
#ifndef PT_NET
#define PT_NET 0
#endif

#endif
