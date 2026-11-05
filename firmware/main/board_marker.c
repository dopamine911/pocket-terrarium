/* board_marker.c - the board an image is for, inside the image (0.3.0: one image per board, docs/BOARDS.md).
 * In .rodata_custom_desc, which the linker places right after esp_app_desc_t: a fixed offset in the image
 * (version.h PT_BOARD_MARKER_OFFSET). tools/pt_boards.py reads it back from the .bin (make_installer.py,
 * make_ota_manifest.py), and the update's download stops on another board's (net_port_esp.c). Every image carries
 * it, networked or not (version.h PT_NET); nothing references it, so main/CMakeLists.txt keeps it by name. */
#include "version.h"

const __attribute__((section(".rodata_custom_desc"), used)) pt_board_marker_t pt_board_marker = { PT_BOARD_MAGIC, PT_BOARD };
