/* card_port_sh8601.c - the CARD PORT (common/card.h, issue #4 phase 4, docs/card-library.md) of the Waveshare 1.8: its TF slot.
 * COMPILE-ONLY: nothing in this file has run on a board. What is sourced and what is not is docs/card-library.md's;
 * docs/board-amoled-1.8-card.md is the bench test that would settle it. Built only with CONFIG_POCKET_TANK_CARD (the 1.8 only).
 *
 * THE WIRING (the vendor's schematic, SD-CARD block - ESP32-S3-Touch-AMOLED-1.8.pdf - and its BSP, Waveshare-ESP32-components
 * bsp/esp32_s3_touch_amoled_1_8: bsp_sdcard_mount(), BSP_SD_CLK / BSP_SD_CMD / BSP_SD_D0): CLK (SCK) on GPIO 2, CMD (MOSI) on
 * GPIO 1, D0 (MISO) on GPIO 3; D1 and D2 are not connected; DAT3 / CS is NOT a GPIO - it is the TCA9554 expander's EXIO7, bit 7
 * of its output register at I2C 0x20 (board_pins.h); the slot's card-detect switch goes to ground, not to a pin, so a card is
 * found only by trying; 10k pull-ups to 3V3 on CS, MOSI, SCK and MISO. The slot is NOT on the display's QSPI lines.
 *
 * WHICH BOARD: card_slot() is board_has_expander() - the 1.8, positively (its expander answered and no ES7210) - and nothing
 * else. GPIO 1, 2 and 3 are other boards' pins: the 1.75C's panel reset, touch reset and PWR-key sense (board_pins.h
 * R_PIN_LCD_RST, R_PIN_TP_RST, R_PIN_PWR_SENSE); card_open never touches them anywhere but on the 1.8. On the 1.8 itself nothing
 * else in firmware/ uses them (grep, 2026-11-06; its pins: display 4-7 / 11 / 12, I2C 14 / 15, audio 8 / 9 / 16 / 45 / 46, BOOT 0).
 *
 * TWO WAYS TO REACH THE CARD, and why EXIO7 matters: a card whose DAT3 is LOW when CMD0 arrives goes into SPI mode and stays
 * there until its power is cycled. So
 *   SDMMC, 1 bit (the default; the vendor's own way: its BSP mounts the slot so): EXIO7 is HIGH before the first clock
 *     (board_sd_select(false)) and after it - this build never drives it low.
 *   SPI (CONFIG_POCKET_TANK_CARD_SPI, an alternative to try if SDMMC does not mount): EXIO7 is LOW, as the chip select, for the
 *     whole visit - the sdspi driver is given no CS pin (SDSPI_SLOT_NO_CS), it has nothing to toggle - on SPI3_HOST (the display
 *     has SPI2). A card that has been through it will not answer SDMMC until it has lost power: take it out and put it back.
 *
 * NEVER WRITES: fopen "rb", directory reads and stat, nothing else; format_if_mount_failed is false - a keeper's card is never
 * formatted. FAT12/16/32 only (ESP-IDF's FatFS has exFAT off: a 64 GB card in its factory exFAT is CARD NOT READABLE) and with
 * long file names (a .ptpk name has a four-letter extension, which 8.3 cannot hold: the build stops without them).
 *
 * WHAT A VISIT COSTS - ESTIMATED from ESP-IDF 5.5.2's sources and this tree's sdkconfig, not measured (nothing here has run):
 *   internal heap while mounted: the sdmmc_card_t (~0.3 KB), the SDMMC host's event queue (32 x 8 B), its two semaphores and its
 *     interrupt (~0.6 KB): about 1 KB. At the peak a sector read adds a 512 B DMA-capable bounce (sdmmc_read_sectors: the S3's
 *     SDMMC DMA cannot reach PSRAM, and FatFS's buffers are PSRAM's), a directory handle (~0.6 KB) and the stdio buffer (128 B:
 *     CONFIG_FATFS_VFS_FSTAT_BLKSIZE 0 = newlib's BUFSIZ here): about 1.3 KB more. The SPI build adds the bus, the device, the DMA
 *     descriptors and sdspi's 516 B block buffer: about 1.5 KB.
 *   FatFS's own memory goes through ff_memalloc, PSRAM first (CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y): the volume - a FATFS with a
 *     4096 B window (CONFIG_FATFS_SECTOR_4096 makes FF_MAX_SS 4096) and the VFS's path buffers, ~4.2 + ~4.4 KB - and a FIL with a
 *     4096 B file cache (CONFIG_FATFS_PER_FILE_CACHE) per max_files: ~13 KB for CARD_MAX_FILES 1 (3 would be ~21 KB). The
 *     internal heap pays that only if PSRAM refuses it.
 *   stack: no large local here (a 60 B path and a struct stat at most); the callee chain (f_mount, the SDMMC driver) is IDF's,
 *     est. 1.5 KB. Everything is freed by card_close. */
#include "sdkconfig.h"
#include "card.h"

#if PT_CARD

#if defined(CONFIG_FATFS_LFN_NONE)
#error "the card library needs FAT long file names (a .ptpk name has a four-letter extension, which 8.3 cannot hold): set CONFIG_FATFS_LFN_HEAP (firmware/sdkconfig.defaults has it)"
#endif

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#if CONFIG_POCKET_TANK_CARD_SPI
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#endif
#include "board_pins.h"
#include "display_port.h"
#include "director.h"

static const char *TAG = "card";

#define CARD_MOUNT      "/card"                      /* where the volume is registered (ESP_VFS_PATH_MAX is 15) */
#define CARD_PACKS_DIR  CARD_MOUNT "/" CARD_FOLDER   /* "/card/packs": the one folder this port reads */
#define CARD_MAX_FILES  1                            /* files open at once: card_read's one. IDF allocates max_files x sizeof(FIL) at
                                                        mount, ~4.2 KB each in this tree (above): nothing here holds a second one */
#define CARD_PIN_CLK    CONFIG_POCKET_TANK_CARD_PIN_CLK
#define CARD_PIN_CMD    CONFIG_POCKET_TANK_CARD_PIN_CMD   /* MOSI in SPI mode */
#define CARD_PIN_D0     CONFIG_POCKET_TANK_CARD_PIN_D0    /* MISO in SPI mode */

/* the pins are the Kconfig's to move, but never onto the board's own: the display's QSPI and chip select, I2C, the touch INT
   (a wrong number in menuconfig must not take the glass down: closing a visit resets its pins) */
#define CARD_PIN_TAKEN(p) ((p) == PIN_LCD_CS || (p) == PIN_LCD_PCLK || ((p) >= PIN_LCD_DATA0 && (p) <= PIN_LCD_DATA3) || \
                           (p) == PIN_I2C_SDA || (p) == PIN_I2C_SCL || (p) == PIN_TP_INT)
_Static_assert(!CARD_PIN_TAKEN(CARD_PIN_CLK) && !CARD_PIN_TAKEN(CARD_PIN_CMD) && !CARD_PIN_TAKEN(CARD_PIN_D0),
               "a card pin is one the display, I2C or touch already uses (main/board_pins.h)");
_Static_assert(CARD_PIN_CLK != CARD_PIN_CMD && CARD_PIN_CLK != CARD_PIN_D0 && CARD_PIN_CMD != CARD_PIN_D0,
               "the card's three pins must be three pins");

static sdmmc_card_t *s_card;                         /* the mounted card; NULL = no visit is open */
#if CONFIG_POCKET_TANK_CARD_SPI
#define CARD_SPI_HOST   SPI3_HOST
static bool s_bus;                                   /* spi_bus_initialize done, spi_bus_free not yet */
#endif

static void card_say(char *why, size_t n, const char *s) { if (why && n) snprintf(why, n, "%s", s); }

/* the 1.8, positively: its expander answered and no ES7210 (the 1.75 has both). The other boards use GPIO 1, 2 and 3 for
   something else - never driven there. It does not say a card is in the slot: card_open does */
bool card_slot(void) { return board_has_expander(); }

/* end of a visit, or a failed start of one: the SPI bus back (its device is gone by now) and EXIO7 high - where it is whenever
   no SPI visit is open, and where an SDMMC card needs it. (board_sd_select is void: if the expander does not answer, nothing changes) */
static void card_release(void) {
#if CONFIG_POCKET_TANK_CARD_SPI
    if (s_bus) { spi_bus_free(CARD_SPI_HOST); s_bus = false; }
#endif
    board_sd_select(false);
}

/* the card's slot brought up and its FAT volume mounted at CARD_MOUNT. On any failure ESP-IDF has undone its own part (host,
   volume, VFS entry); the SPI bus is this file's to give back (card_release) */
static esp_err_t card_mount(sdmmc_card_t **card) {
    const esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,             /* a keeper's card is never formatted */
        .max_files = CARD_MAX_FILES,
    };
#if CONFIG_POCKET_TANK_CARD_SPI
    const spi_bus_config_t bus = {
        .mosi_io_num = CARD_PIN_CMD, .miso_io_num = CARD_PIN_D0, .sclk_io_num = CARD_PIN_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4000,                     /* as ESP-IDF's sdspi example: a block and its tokens with room to spare */
    };
    esp_err_t e = spi_bus_initialize(CARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) return e;
    s_bus = true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CARD_SPI_HOST;                       /* the default is SPI2: the display's */
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = CARD_SPI_HOST;
    dev.gpio_cs = SDSPI_SLOT_NO_CS;                  /* EXIO7 is the chip select and the expander's: board_sd_select(true) holds it */
    return esp_vfs_fat_sdspi_mount(CARD_MOUNT, &host, &dev, &mount, card);
#else
    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();  /* slot 1, 20 MHz at most, 3.3 V: the vendor's BSP takes the default too */
    const sdmmc_slot_config_t slot = {
        .clk = CARD_PIN_CLK, .cmd = CARD_PIN_CMD, .d0 = CARD_PIN_D0,
        .d1 = GPIO_NUM_NC, .d2 = GPIO_NUM_NC, .d3 = GPIO_NUM_NC,   /* 1 bit: DAT3 is the expander's EXIO7, D1 and D2 are not wired. NC - */
        .d4 = GPIO_NUM_NC, .d5 = GPIO_NUM_NC, .d6 = GPIO_NUM_NC, .d7 = GPIO_NUM_NC,   /* not the macro's GPIO 4, 12, 13 (the display's) */
        .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP,            /* the card-detect switch goes to ground, not to a pin */
        .width = 1,
        .flags = 0,                                  /* the board's own pull-ups, not the chip's */
    };
    return esp_vfs_fat_sdmmc_mount(CARD_MOUNT, &host, &slot, &mount, card);
#endif
}

/* the card names itself in a few bytes it chose: only printable ASCII goes to the log */
static void card_label(const sdmmc_card_t *c, char out[9]) {
    int k = 0;
    for (; k < 8 && c->cid.name[k]; k++) {
        const unsigned char ch = (unsigned char)c->cid.name[k];
        out[k] = ch >= 0x20 && ch < 0x7f ? (char)ch : '?';
    }
    out[k] = 0;
}

bool card_open(char *why, size_t n) {
    if (!card_slot()) { card_say(why, n, "NO CARD SLOT"); return false; }   /* not the 1.8: its GPIO 1, 2 and 3 are not ours */
    if (s_card) { card_say(why, n, "CARD BUSY"); return false; }           /* one visit at a time: every true is answered by one card_close */
#if CONFIG_POCKET_TANK_CARD_SPI
    ESP_LOGI(TAG, "visit open (SPI mode CLK %d MOSI %d MISO %d, EXIO7 low)", CARD_PIN_CLK, CARD_PIN_CMD, CARD_PIN_D0);
    board_sd_select(true);                           /* the chip select, held for the whole visit */
#else
    ESP_LOGI(TAG, "visit open (SDMMC 1-bit CLK %d CMD %d D0 %d, EXIO7 high)", CARD_PIN_CLK, CARD_PIN_CMD, CARD_PIN_D0);
    board_sd_select(false);                          /* DAT3 high before the first clock, or the card falls into SPI mode */
#endif
    device_wdt_feed();                               /* the visit runs on the tank task: a mount is seconds at the worst, the watchdog's 20 s are not */
    sdmmc_card_t *card = NULL;
    const esp_err_t e = card_mount(&card);
    device_wdt_feed();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s (0x%x)", esp_err_to_name(e), (unsigned)e);
        card_release();
        /* no card answers with a timeout (or, from the host, not-found); no room is its own word; a card that answered but has no
           FAT volume IDF can mount (ESP_FAIL, FatFS's errors, exFAT) is one the keeper must reformat */
        card_say(why, n, e == ESP_ERR_NOT_FOUND || e == ESP_ERR_TIMEOUT ? "NO CARD" : e == ESP_ERR_NO_MEM ? "NO MEMORY" : "CARD NOT READABLE");
        return false;
    }
    s_card = card;
    char nm[9];
    card_label(card, nm);
    ESP_LOGI(TAG, "mounted: %s, %llu MB", nm, (unsigned long long)(unsigned)card->csd.capacity * (unsigned)card->csd.sector_size >> 20);
    return true;
}

void card_close(void) {
    if (!s_card) return;                             /* idempotent */
    const esp_err_t e = esp_vfs_fat_sdcard_unmount(CARD_MOUNT, s_card);   /* frees the card and the host's slot */
    if (e != ESP_OK) ESP_LOGW(TAG, "unmount: %s (0x%x)", esp_err_to_name(e), (unsigned)e);
    s_card = NULL;
    card_release();
    ESP_LOGI(TAG, "visit closed");
}

/* where `name` goes in out[0..n), kept in strcmp order (FAT keeps the case it was given: A before a), or -1 when it is there */
static int list_pos(const card_entry_t *out, int n, const char *name) {
    int at = n;
    while (at > 0) {
        const int c = strcmp(name, out[at - 1].name);
        if (c == 0) return -1;
        if (c > 0) break;
        at--;
    }
    return at;
}

/* name into out[at]: the names from there up move one place, and in a list that is already full the last one falls out */
static void list_put(card_entry_t *out, int *n, int max, int at, const char *name, uint32_t size) {
    const int last = *n < max ? *n : max - 1;
    memmove(&out[at + 1], &out[at], (size_t)(last - at) * sizeof out[0]);
    const size_t l = strnlen(name, sizeof out[at].name - 1);   /* (card_name_ok has taken the name: at most CARD_NAME_MAX - 1 characters) */
    memcpy(out[at].name, name, l); out[at].name[l] = 0;
    out[at].size = size;
    if (*n < max) (*n)++;
}

int card_list(card_entry_t *out, int max, bool *more, char *why, size_t why_n) {
    if (more) *more = false;
    if (!s_card || !out || max <= 0) { card_say(why, why_n, "NO CARD"); return -1; }
    DIR *d = opendir(CARD_PACKS_DIR);
    if (!d) {
        const int err = errno;
        ESP_LOGW(TAG, "/" CARD_FOLDER ": not opened (errno %d)", err);
        card_say(why, why_n, err == ENOENT || err == ENOTDIR ? "NO PACKS FOLDER" : err == ENOMEM ? "NO MEMORY" : "CARD NOT READABLE");
        return -1;
    }
    /* EVERY entry is looked at and the `max` smallest names kept, sorted, in out itself: a card with 1000 files lists the same
       first 32 every time whatever order its directory holds them in, and `over` says there were more. A folder is as big as
       the card's owner made it: the watchdog is fed on the way. */
    int n = 0;
    bool over = false;
    for (unsigned seen = 1;; seen++) {
        errno = 0;
        const struct dirent *e = readdir(d);
        if (!e) {
            const int err = errno;                   /* the end of the folder leaves errno alone; a read that failed sets it */
            if (!err) break;
            ESP_LOGW(TAG, "/" CARD_FOLDER ": read stopped (errno %d)", err);
            closedir(d);
            card_say(why, why_n, "CARD NOT READABLE");
            return -1;                               /* never a half-read folder passed off as the whole one */
        }
        if (!(seen & 63)) device_wdt_feed();
        const bool reg = e->d_type == DT_REG;
        if (!reg && e->d_type != DT_UNKNOWN) continue;     /* a directory, or anything else the volume names: not listed */
        if (!card_name_ok(e->d_name)) continue;            /* a name this port would not open is never listed */
        const int at = list_pos(out, n, e->d_name);
        if (at < 0) continue;
        if (reg && n == max && at == max) { over = true; continue; }   /* a file, too far down the order for the list: no stat */
        char path[sizeof CARD_PACKS_DIR + CARD_NAME_MAX];  /* "/card/packs/" and a name of at most 47 characters */
        const int k = snprintf(path, sizeof path, CARD_PACKS_DIR "/%s", e->d_name);
        struct stat st;
        if (k < 0 || (size_t)k >= sizeof path || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == max) {                                    /* a full list: this name is the one more */
            over = true;
            if (at == max) continue;
        }
        /* FAT sizes are 32-bit: a file of 2 GB or more shows as a negative 32-bit off_t, and the cast gives its size back. The
           library refuses it by its size, before a byte of it is read */
        list_put(out, &n, max, at, e->d_name, (uint32_t)st.st_size);
    }
    closedir(d);
    if (more) *more = over;
    ESP_LOGI(TAG, "/" CARD_FOLDER ": %d .ptpk files listed%s", n, over ? " (more on the card, not listed)" : "");
    return n;
}

int card_read(const char *name, uint8_t *buf, size_t cap) {
    if (!s_card || !buf || !cap || cap > (size_t)INT_MAX) return -1;
    if (!name || !card_name_ok(name)) { ESP_LOGW(TAG, "read: a name this port does not open - refused"); return -1; }
    char path[sizeof CARD_PACKS_DIR + CARD_NAME_MAX];
    const int k = snprintf(path, sizeof path, CARD_PACKS_DIR "/%s", name);
    if (k < 0 || (size_t)k >= sizeof path) return -1;
    FILE *f = fopen(path, "rb");                     /* read only: nothing in this file opens a card file any other way */
    if (!f) { ESP_LOGW(TAG, "%s: not opened (errno %d)", name, errno); return -1; }
    const size_t got = fread(buf, 1, cap, f);        /* never more than cap bytes into the caller's block */
    bool bad = ferror(f) != 0;
    bool longer = false;
    if (!bad && got == cap) {                        /* the block is full: one byte more tells a file that fits exactly from one that does not */
        longer = fgetc(f) != EOF;
        if (!longer) bad = ferror(f) != 0;
    }
    fclose(f);
    device_wdt_feed();
    if (longer) { ESP_LOGW(TAG, "%s: longer than %u bytes - refused", name, (unsigned)cap); return -1; }
    if (bad) { ESP_LOGW(TAG, "%s: read failed (errno %d)", name, errno); return -1; }
    return (int)got;
}

/* the library's working area (10460 B + the page's 768 B, measured in the simulator) while its page is up: PSRAM, never .bss and never the internal heap the tank lives on.
   NULL when there is no PSRAM (heap_caps does not fall back to anything else) */
void *card_mem_alloc(size_t bytes) { return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void card_mem_free(void *p) { heap_caps_free(p); }

#endif
