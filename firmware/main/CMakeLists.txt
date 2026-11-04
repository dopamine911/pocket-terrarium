set(SRCS main.c director.c brightness.c batlog.c codec_port.c advisor_llm_esp.c battery_port_axp2101.c imu_port_qmi8658.c
         update_mode.c net_port_esp.c model_trailer.c link_spike.c link_port_espnow.c link_dev.c)
# sd_spike.c (the tank switcher's step 2 test, `sd test`) is left out of 0.3.9.3 (docs/release-0.3.9.3.md D2): it comes
# back with the packages (step 4) - list it here and restore its three hooks in main.c / director.c
if(CONFIG_POCKET_TANK_DISPLAY_SH8601)
    list(APPEND SRCS display_port_sh8601.c)
else()
    list(APPEND SRCS display_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_TOUCH_FT3168)
    list(APPEND SRCS touch_port_ft3168.c)
else()
    list(APPEND SRCS touch_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_AUDIO_ES8311)
    list(APPEND SRCS audio_port_es8311.c)
else()
    list(APPEND SRCS audio_port_stub.c)
endif()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES tankcore llm esp_partition spi_flash esp_timer esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_usb_serial_jtag esp_lcd nvs_flash esp_app_format
             esp_wifi esp_netif esp_event esp_http_client esp_https_ota app_update json mbedtls bootloader_support fatfs sdmmc esp_driver_sdspi
    EMBED_FILES "../../assets/sounds/sounds.bin"
)
# THE BRAIN's tokenizer, the world's (common/world.h THE BRAIN - the one place the lizard model's files are named):
# PT_FW_TOKENIZER_AQUARIUM / _TERRARIUM, a path from firmware/, read from world.h and embedded under the name
# tokenizer.bin whatever its own, so main.c's _binary_tokenizer_bin_start / _end hold in either world. (The model itself
# goes to the model partition: tools/flash*.sh.) An edit of world.h re-runs this.
if(CONFIG_POCKET_TANK_WORLD_AQUARIUM)
    set(PT_WORLD_TAG AQUARIUM)
else()
    set(PT_WORLD_TAG TERRARIUM)
endif()
set(PT_WORLD_H "${CMAKE_CURRENT_LIST_DIR}/../../common/world.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PT_WORLD_H}")
file(STRINGS "${PT_WORLD_H}" PT_TOK_LINE REGEX "^#define[ \t]+PT_FW_TOKENIZER_${PT_WORLD_TAG}[ \t]")
string(REGEX MATCH "\"[^\"]+\"" PT_TOK_FILE "${PT_TOK_LINE}")
string(REPLACE "\"" "" PT_TOK_FILE "${PT_TOK_FILE}")
if(NOT PT_TOK_FILE OR NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../${PT_TOK_FILE}")
    message(FATAL_ERROR "common/world.h's PT_FW_TOKENIZER_${PT_WORLD_TAG} names no file under firmware/ ('${PT_TOK_FILE}')")
endif()
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../${PT_TOK_FILE}" BINARY RENAME_TO tokenizer.bin)
# the board marker (net_port_esp.c, version.h PT_BOARD_MARKER_OFFSET): nothing
# references it - the download reads it back from flash - so it is kept by name
# or --gc-sections drops it (ESP-IDF's "custom structure" recipe, app_image_format)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pt_board_marker")
