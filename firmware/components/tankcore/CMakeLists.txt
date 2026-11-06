# Platform-agnostic reflex layer + renderer + progression, shared verbatim with
# sim/ via ../../../common, plus this platform's implementation of the
# progression ports (NVS + wall clock). The ports live here, not in main, so
# the static-archive link order resolves (tankcore -> nvs_flash).
set(COMMON ${CMAKE_CURRENT_LIST_DIR}/../../../common)
# THE NETWORK (common/version.h PT_NET, read as firmware/main/CMakeLists.txt reads it): 0 = updates, the passport, the
# tank link, the visitor collection and GIVE's notes stay out of the build. (main/CMakeLists.txt alone lists version.h as a
# configure dependency - an edit re-runs the whole configure; a second listing here made ninja's build.ninja name it as an
# output twice and the configure fail)
file(STRINGS "${COMMON}/version.h" PT_NET_LINE REGEX "^#define[ \t]+PT_NET[ \t]+[01]")
string(REGEX REPLACE "^#define[ \t]+PT_NET[ \t]+([01]).*$" "\\1" PT_NET "${PT_NET_LINE}")
set(SRCS "${COMMON}/tank.c" "${COMMON}/advisor.c" "${COMMON}/render.c" "${COMMON}/icons.c" "${COMMON}/progression.c" "${COMMON}/setup.c"
         "${COMMON}/audio.c" "${COMMON}/notice.c" "${COMMON}/sounds.c" "${COMMON}/battery.c" "${COMMON}/advanced.c" "${COMMON}/shelf.c"
         "${COMMON}/species.c" "${COMMON}/habitat.c" "${COMMON}/terrain.c"
         "${COMMON}/packs/ptpk.c" "${COMMON}/packs/ptpk_write.c" "${COMMON}/packs/builtin.c"
         "${COMMON}/packs/caps.c" "${COMMON}/packs/manifest.c" "${COMMON}/packs/models.c" "${COMMON}/packs/compat.c"
         "persist_port_esp.c" "rtc_port_pcf85063.c")
# THE PACKS (docs/ptpk.md): the built-in packs' source is a const species_t / habitat_t / model_manifest_t in C (common/species,
# common/habitat, packs/models.c; capabilities and the pair check: packs/caps.c, compat.c, the registry of the model manifests: manifest.c);
# packs_load_builtin encodes each (packs/ptpk_write.c) into a PTPK file on the main task's stack at boot and loads it through
# the one loader (packs/ptpk.c, packs_add), as a file from a card would be. The table of them (packs/builtin.c) names only what
# this image links: a release image carries its own world's pair, test-only packs left out - the simulator carries them all
# (PT_ALL_PACKS). THE WORLD's owned const packs and renderer art; render.c preprocessor-excludes the other live renderer
# before link resolution.
if(CONFIG_POCKET_TANK_WORLD_AQUARIUM)
    list(APPEND SRCS "${COMMON}/species/fish.c" "${COMMON}/habitat/freshwater.c")
else()
    list(APPEND SRCS "${COMMON}/species/gecko.c" "${COMMON}/habitat/desert.c"
                     "${COMMON}/render_lizard.c" "${COMMON}/render_scene_desert.c")
endif()
# THE MODULES (issue #4 phase 3): this world's mechanics (common/worlds/<world>/), its body type's art (common/bodies/<body>/)
# and the movement modules (common/locomotion/), found by directory
if(CONFIG_POCKET_TANK_WORLD_AQUARIUM)
    set(PT_WORLD_DIR aquarium)
    set(PT_BODY_DIR fish)
else()
    set(PT_WORLD_DIR terrarium)
    set(PT_BODY_DIR lizard)
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)   # (the early scan of the component's requirements runs in script mode, where CONFIGURE_DEPENDS is invalid)
    file(GLOB_RECURSE MODULE_SRCS CONFIGURE_DEPENDS "${COMMON}/worlds/${PT_WORLD_DIR}/*.c" "${COMMON}/bodies/${PT_BODY_DIR}/*.c" "${COMMON}/locomotion/*.c")
    list(APPEND SRCS ${MODULE_SRCS})
endif()
if(PT_NET STREQUAL "1")
    list(APPEND SRCS "${COMMON}/update.c" "${COMMON}/passport.c" "${COMMON}/link.c" "${COMMON}/tank_link.c" "${COMMON}/collection.c" "${COMMON}/gift.c")
endif()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "${COMMON}" "."
    PRIV_REQUIRES nvs_flash esp_driver_i2c esp_partition
)
# POP_CAP (the keeper's 5-fish cap) is common/tank.h's, the same for the sim and every board: never defined here
# (tank.h stops a build that does). N_FISH_MAX stays 6, the array bound.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)
# the world's shape is compiled in (common/tank.h): the bowl for the round board.
# PUBLIC: every component that sees tank.h must see the same world.
if(CONFIG_POCKET_TANK_ROUND)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC TANK_ROUND)
endif()
# the frame in the panel's byte order on every board (2026-10-23, render.h PT_FB_SWAP): the SPI DMA
# sends it straight from PSRAM on the flush task; the sim builds without it (its pixel tests)
target_compile_definitions(${COMPONENT_LIB} PUBLIC PT_FB_SWAP=1)
if(CONFIG_POCKET_TANK_WATCH)   # the watch's 410 x 502 portrait tank
    target_compile_definitions(${COMPONENT_LIB} PUBLIC TANK_WATCH)
endif()
if(CONFIG_POCKET_TANK_SQUARE)  # the square's 480 x 480 tank
    target_compile_definitions(${COMPONENT_LIB} PUBLIC TANK_SQUARE)
endif()
# THE WORLD (common/world.h, PLAN.md D11): the bin's, from the Kconfig choice POCKET_TANK_WORLD (sdkconfig.aquarium picks
# the aquarium). PUBLIC, as the board's shape: every component that sees tank.h - main, llm - must see the same world.
if(CONFIG_POCKET_TANK_WORLD_AQUARIUM)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PT_WORLD=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PT_WORLD=2)
endif()
