/* persist_port_esp.c — progression ports on the device: NVS blob + wall clock.
 * Wall clock: esp time (set from the PCF85063 RTC at boot in Track 4; until
 * then it is 0 on a cold boot, which simply disables the ravenous rule). */
#include "progression.h"
#include "collection.h"
#include "gift.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "persist";

/* A save LONGER than this build's save_t is a newer build's (a rollback, or
 * the installer's older release): its head is our whole layout (tails only
 * ever append), so the head is loaded and *got reports the whole length -
 * progression reads it again whole to keep the newer tail and write it back
 * (newer_keep), so the newer build finds it when it comes back.
 * A blob that exists but won't read blocks every save until a reset, so a
 * tank we could not load is never overwritten by a fresh one. */
static bool s_save_blocked;
bool persist_port_load(void *buf, size_t max, size_t *got) {
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0; esp_err_t e = nvs_get_blob(h, SAVE_NVS_KEY, NULL, &len);   /* the stored length first */
    if (e == ESP_OK && len == 0) e = ESP_ERR_NVS_NOT_FOUND;         /* an empty blob is nothing saved */
    if (e == ESP_OK && len <= max) e = nvs_get_blob(h, SAVE_NVS_KEY, buf, &len);
    else if (e == ESP_OK) {                                /* a newer build's save: keep its head */
        uint8_t *all = malloc(len);
        if (!all) e = ESP_ERR_NO_MEM;
        else if ((e = nvs_get_blob(h, SAVE_NVS_KEY, all, &len)) == ESP_OK) {
            memcpy(buf, all, max);
            ESP_LOGW(TAG, "a newer build's save (%u bytes): loading the first %u", (unsigned)len, (unsigned)max);
        }
        free(all);
    }
    nvs_close(h);
    if (e == ESP_OK) *got = len;
    else if (e != ESP_ERR_NVS_NOT_FOUND) {
        s_save_blocked = true;
        ESP_LOGE(TAG, "load failed: %s (%u bytes) - saving is OFF so the stored tank stays", esp_err_to_name(e), (unsigned)len);
    }
    return e == ESP_OK;
}
bool persist_port_save(const void *buf, size_t len) {
    if (s_save_blocked) return false;
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, SAVE_NVS_KEY, buf, len); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); if (e != ESP_OK) ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(e));
    return e == ESP_OK;
}
/* the keeper's reset: the tank goes - "save" and the director's parked "bk"
 * alike, so nothing can bring it back. Since the tank switcher (D13) the
 * board's own keys in the namespace stay: brightness, volume, the battery's
 * size, the touch fit, the bedtime log. (It was nvs_erase_all until 0.3.9.3.) */
bool persist_port_erase(void) {
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, SAVE_NVS_KEY); if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    esp_err_t e2 = nvs_erase_key(h, "bk"); if (e == ESP_OK && e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) e = e2;
    static const char *const OWN[7] = { COLL_NVS_KEY, KIN_NVS_KEY, GIFT_NVS_KEY,   /* the tank's own blobs beside its save (0.4): the visitor
                                                                                   collection, the KIN book, the gift notes */
                                        "bkvis", "bkkin", "bkgft", "bkbooks" };    /* ... and the director's parked ones (director.c books_park), as "bk" */
    for (int k = 0; k < 7; k++) { e2 = nvs_erase_key(h, OWN[k]); if (e == ESP_OK && e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) e = e2; }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "erase failed: %s", esp_err_to_name(e));
    else { ESP_LOGI(TAG, "the tank on the glass erased (the shelf and the board's settings kept)"); s_save_blocked = false; }
    return e == ESP_OK;
}
bool persist_port_blob_load(const char *key, void *buf, size_t max, size_t *got) {
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0; esp_err_t e = nvs_get_blob(h, key, NULL, &len);
    if (e == ESP_OK && (len == 0 || len > max)) e = ESP_ERR_INVALID_SIZE;
    if (e == ESP_OK) e = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (e == ESP_OK) *got = len;
    return e == ESP_OK;
}
bool persist_port_blob_save(const char *key, const void *buf, size_t len) {
    if (s_save_blocked) return false;
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, key, buf, len); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); if (e != ESP_OK) ESP_LOGW(TAG, "%s save failed: %s", key, esp_err_to_name(e));
    return e == ESP_OK;
}
bool persist_port_blob_forget(const char *key) {
    if (s_save_blocked) return false;
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, key); if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); if (e != ESP_OK) ESP_LOGW(TAG, "%s forget failed: %s", key, esp_err_to_name(e));
    return e == ESP_OK;
}
int64_t clock_port_now_unix(void) {
    time_t now = time(NULL); return now > 1700000000 ? (int64_t)now : 0;   /* 0 until the RTC sets it */
}

/* ---- the shelf's ports (the tank switcher, common/shelf.h): the `storage`
 * partition past its first 64 KB (the NVS rescue copy, main.c nvs_start,
 * sits at 0 and must never be written over) and the NVS namespace `frame`.
 * A `tank` reset (persist_port_erase) never touches either: the reset is the
 * tank on the glass only (D13). ---- */
#include "shelf.h"
#include "esp_partition.h"
#include "esp_memory_utils.h"
#define SHELF_BASE 0x10000
static const esp_partition_t *shelf_part(void) {
    static const esp_partition_t *p; static bool looked;
    if (!looked) { p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage"); looked = true; }
    return p;
}
size_t shelf_port_size(void) { const esp_partition_t *p = shelf_part(); return p && p->size > SHELF_BASE ? p->size - SHELF_BASE : 0; }
/* a read into PSRAM (the page's snapshots, the shelf's scratch) comes through 1 KB on the stack. Flash reads
   straight into PSRAM take a bounce buffer from INTERNAL RAM for the read (esp_flash_read: up to 16 KB, or the
   whole largest internal block when that is smaller) - MY TANKS's open took the square's internal low mark
   19 -> 7 KB, one snapshot at a time (0.4); on the watch / pendant (~20 KB free) the largest block went whole */
#define SHELF_READ_BOUNCE 1024
bool shelf_port_read(uint32_t off, void *buf, size_t n) {
    const esp_partition_t *p = shelf_part();
    if (!p) return false;
    if (esp_ptr_in_dram(buf)) return esp_partition_read(p, SHELF_BASE + off, buf, n) == ESP_OK;
    uint8_t b[SHELF_READ_BOUNCE];
    for (size_t o = 0; o < n; o += sizeof b) {
        size_t k = n - o < sizeof b ? n - o : sizeof b;
        if (esp_partition_read(p, SHELF_BASE + off + o, b, k) != ESP_OK) return false;
        memcpy((uint8_t *)buf + o, b, k);
    }
    return true;
}
bool shelf_port_write(uint32_t off, const void *buf, size_t n) { const esp_partition_t *p = shelf_part(); return p && esp_partition_write(p, SHELF_BASE + off, buf, n) == ESP_OK; }
bool shelf_port_erase(uint32_t off, size_t n) { const esp_partition_t *p = shelf_part(); return p && esp_partition_erase_range(p, SHELF_BASE + off, n) == ESP_OK; }
#define SHELF_FRAME_NS "terrframe"                /* pocket-terrarium's (pocket-tank's journal is "frame") */
bool shelf_port_frame_get(const char *key, void *buf, size_t max, size_t *got) {
    nvs_handle_t h; if (nvs_open(SHELF_FRAME_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = max; esp_err_t e = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (e == ESP_OK) *got = len;
    return e == ESP_OK;
}
bool shelf_port_frame_set(const char *key, const void *buf, size_t n) {
    nvs_handle_t h; if (nvs_open(SHELF_FRAME_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, key, buf, n); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); if (e != ESP_OK) ESP_LOGW(TAG, "frame %s: %s", key, esp_err_to_name(e));
    return e == ESP_OK;
}
bool shelf_port_frame_del(const char *key) {
    nvs_handle_t h; if (nvs_open(SHELF_FRAME_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, key); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}
bool persist_port_forget(void) {                  /* the save on the glass only: a fresh tank comes in */
    if (s_save_blocked) return false;             /* (a save we could not load is never thrown away for a fresh one) */
    nvs_handle_t h; if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, SAVE_NVS_KEY); if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}

#if PT_CARD
/* ---- THE PACK STORE's flash (packs/pstore.h, issue #4 phase 4): the 40 KB of the `storage` partition between the NVS rescue
 * copy (main.c nvs_start: NVS_RESCUE_SIZE 0x6000 bytes at storage + 0) and the shelf's first byte (SHELF_BASE) - PSTORE_SECTORS
 * sectors of 4 KB, one installed pack to a sector. Nothing here knows what a pack is: the store (common/packs/pstore.c) writes
 * a sector's file first and its header last, so a power cut leaves a sector that reads as nothing. Only the card-library build
 * has it (CONFIG_POCKET_TANK_CARD); the shelf's frame and the NVS copy do not move. Offsets are within the store. ---- */
#include "packs/pstore.h"
#define PSTORE_BYTES ((size_t)PSTORE_SECTORS * PSTORE_SECTOR_BYTES)
_Static_assert(PSTORE_BASE >= 0x6000, "the pack store must start past the NVS rescue copy (main.c nvs_start: 0x6000 bytes at the start of storage)");
_Static_assert(PSTORE_BASE + PSTORE_BYTES <= SHELF_BASE, "the pack store must end before the shelf's first byte (SHELF_BASE)");
_Static_assert(PSTORE_BASE % 4096 == 0 && PSTORE_SECTOR_BYTES % 4096 == 0, "esp_partition_erase_range wants the flash's 4 KB sectors, aligned");
/* as shelf_port_read: a destination outside DRAM (the library's block in PSRAM) is read through a small stack bounce - a flash
   read straight into PSRAM takes a bounce buffer from internal RAM (esp_flash_read), and the library runs on a page's budget */
bool pstore_port_read(uint32_t off, void *buf, size_t n) {
    const esp_partition_t *p = shelf_part();
    if (!p || !buf || off > PSTORE_BYTES || n > PSTORE_BYTES - off) return false;
    if (n == 0) return true;
    if (esp_ptr_in_dram(buf)) return esp_partition_read(p, PSTORE_BASE + off, buf, n) == ESP_OK;
    uint8_t b[SHELF_READ_BOUNCE];
    for (size_t o = 0; o < n; o += sizeof b) {
        size_t k = n - o < sizeof b ? n - o : sizeof b;
        if (esp_partition_read(p, PSTORE_BASE + off + o, b, k) != ESP_OK) return false;
        memcpy((uint8_t *)buf + o, b, k);
    }
    return true;
}
/* programs erased flash (bits only fall): the store erases the sector first. false when the flash refused it */
bool pstore_port_write(uint32_t off, const void *buf, size_t n) {
    const esp_partition_t *p = shelf_part();
    if (!p || !buf || off > PSTORE_BYTES || n > PSTORE_BYTES - off) return false;
    if (n == 0) return true;
    esp_err_t e = esp_partition_write(p, PSTORE_BASE + off, buf, n);
    if (e != ESP_OK) ESP_LOGW(TAG, "pack store: write of %u bytes at %u: %s", (unsigned)n, (unsigned)off, esp_err_to_name(e));
    return e == ESP_OK;
}
/* one sector of the store back to 0xFF */
bool pstore_port_erase(int sector) {
    const esp_partition_t *p = shelf_part();
    if (!p || sector < 0 || sector >= PSTORE_SECTORS) return false;
    esp_err_t e = esp_partition_erase_range(p, PSTORE_BASE + (size_t)sector * PSTORE_SECTOR_BYTES, PSTORE_SECTOR_BYTES);
    if (e != ESP_OK) ESP_LOGW(TAG, "pack store: erase of sector %d: %s", sector, esp_err_to_name(e));
    return e == ESP_OK;
}
#endif
