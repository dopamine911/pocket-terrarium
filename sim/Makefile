# pocket-tank PC simulator (LVGL v9 + SDL2)
#
# macOS: built as x86_64 by default because the Homebrew SDL2 here is the
# Intel build (/usr/local); the sim runs fine under Rosetta. `make ARCH=` for
# a native build against an arm64 SDL2. Elsewhere (Linux, CI) ARCH is empty
# and the host compiler's own architecture is used.

CC      := cc
ifeq ($(shell uname -s),Darwin)
ARCH    ?= -arch x86_64
endif
ARCH    ?=
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

# `make ROUND=1`: the round 466 px bowl (the Waveshare 1.75C's world) as
# ./fishsim-round, built beside the rectangle's in build-round/.
# `make WATCH=1`: the watch's 410 x 502 portrait tank (the Waveshare 2.06) as ./fishsim-watch
# `make SQUARE=1`: the square's 480 x 480 tank (the Waveshare 2.16) as ./fishsim-square
ifdef ROUND
BUILD   := build-round
OUT     := fishsim-round
ROUNDF  := -DTANK_ROUND
else ifdef WATCH
BUILD   := build-watch
OUT     := fishsim-watch
ROUNDF  := -DTANK_WATCH
else ifdef SQUARE
BUILD   := build-square
OUT     := fishsim-square
ROUNDF  := -DTANK_SQUARE
else
BUILD   := build
OUT     := fishsim
ROUNDF  :=
endif

# THE WORLD (pocket-terrarium P2b, common/world.h; PLAN.md D11): one core, a bin per world. `make` builds the
# terrarium's (the default: lizards, walking) as today's ./fishsim*; `make WORLD=aquarium` the aquarium's
# (pocket-tank's fish, swimming) as ./fishsim*-aquarium in build*-aquarium/ - so both worlds of every board build
# and run side by side. The world picks the model files, the packs, the goal set, the locomotion and the selftests.
WORLD   ?= terrarium
ifeq ($(WORLD),terrarium)
WORLDF  := -DPT_WORLD=2
else ifeq ($(WORLD),aquarium)
WORLDF  := -DPT_WORLD=1
BUILD   := $(BUILD)-aquarium
OUT     := $(OUT)-aquarium
else
$(error WORLD is terrarium or aquarium, not "$(WORLD)")
endif

# THE NETWORK (pocket-terrarium P3b, common/version.h PT_NET; PLAN.md D4): off in both worlds' bins - nothing of the tank
# link, gifts, passports, the visitor collection or updates is compiled. `make NET=1` builds the networked bin
# (-DPT_NET=1) as ./fishsim*-net in build*-net/, beside the others, for the networking tests (`make check-net`).
NET     ?= 0
ifeq ($(NET),1)
NETF    := -DPT_NET=1
BUILD   := $(BUILD)-net
OUT     := $(OUT)-net
else ifeq ($(NET),0)
NETF    := -DPT_NET=0
else
$(error NET is 0 or 1, not "$(NET)")
endif

# THE CARD LIBRARY (issue #4 phase 4, common/world.h PT_CARD; docs/card-library.md): built by default - the simulator's card is a
# folder (POCKET_CARD_DIR; card_port_sim.c), its pack store a file (POCKET_TANK_PACKSTORE), its PACKS page under ADVANCED. `make CARD=0`
# builds without it, as ./fishsim*-nocard in build*-nocard/: the configuration of a firmware that leaves CONFIG_POCKET_TANK_CARD off
# (no PACKS page, no library, its pack pools' size unchanged) - `make CARD=0 check` holds that configuration to every test it still has.
CARD    ?= 1
ifeq ($(CARD),1)
CARDF   := -DPT_CARD=1
else ifeq ($(CARD),0)
CARDF   := -DPT_CARD=0
BUILD   := $(BUILD)-nocard
OUT     := $(OUT)-nocard
else
$(error CARD is 0 or 1, not "$(CARD)")
endif

# PT_ALL_PACKS is the simulator's cross-world validation inventory (world.h); release firmware links only its world's packs.
CFLAGS  := $(ROUNDF) $(WORLDF) $(NETF) $(CARDF) $(EXTRA_CFLAGS) -DPT_ALL_PACKS=1 -O2 -std=c11 $(ARCH) -I. -I../common -I../common/llm -DLV_CONF_INCLUDE_SIMPLE -DSOUNDS_BIN='"../assets/sounds/sounds.bin"' -DPT_VERSION='"$(shell git describe --always --dirty --exclude='*' 2>/dev/null || echo sim)"' $(SDL_CFLAGS) \
           -Wall -Wextra -Wno-unused-parameter -MMD -MP
LDFLAGS := $(ARCH) $(SDL_LIBS) -lm

APP_SRCS  := main.c advisor_llm.c persist_port_sim.c ../common/tank.c ../common/advanced.c ../common/shelf.c ../common/advisor.c ../common/render.c ../common/icons.c ../common/progression.c ../common/setup.c ../common/audio.c ../common/notice.c ../common/sounds.c ../common/battery.c ../common/llm/q4_model.c ../common/llm/word_tok.c ../common/llm/advisor_core.c

ifeq ($(NET),1)   # the networked bin's: updates, the passport, the link engine and the doors, the visitor collection, GIVE
APP_SRCS  += ../common/update.c ../common/passport.c ../common/link.c ../common/tank_link.c ../common/collection.c ../common/gift.c
APP_SRCS  += net_port_sim.c selftest_link.c selftest_coll.c   # the pretend radio; the link's tests; the visitor collection's pages (0.4)
endif
APP_SRCS  += ../common/species.c ../common/habitat.c ../common/packs/ptpk.c ../common/packs/ptpk_write.c ../common/packs/builtin.c packs_dir.c   # the pack registry and the PTPK loader (P2a, P2b; issue #4 phase 1), the encoder and the built-in table; the simulator's folder of packs
APP_SRCS  += ../common/packs/caps.c ../common/packs/manifest.c ../common/packs/models.c ../common/packs/compat.c   # capabilities, model manifests and the pair check (issue #4 phase 2)
APP_SRCS  += ../common/species/gecko.c ../common/species/fish.c ../common/habitat/desert.c ../common/habitat/freshwater.c   # the built-in packs' source: both worlds' const species_t / habitat_t, the test packs too (PT_ALL_PACKS)
APP_SRCS  += ../common/terrain.c   # a habitat's ground laid out on a glass: the walk, the heat, the scene, the rules (P2c)
APP_SRCS  += selftest_packs.c selftest_ptpk.c selftest_caps.c selftest_l1.c   # the packs, heat, shed and mister (P2a); the PTPK format, loader and a tank without its packs (issue #4 phase 1); the capabilities, the model manifests and a pair that cannot run (phase 2); the L1 encoder against the Python fixture (R4)
APP_SRCS  += selftest_walk.c  # the terrarium's walk, its goals and a free-running census (P2b)
ifeq ($(CARD),1)   # THE CARD LIBRARY (issue #4 phase 4): the card port, the pack store, the library, the PACKS page; the simulator's card is a folder, its store a file; the tests' fixtures
APP_SRCS  += ../common/card.c ../common/packs/pstore.c ../common/packs/library.c ../common/packs_page.c card_port_sim.c
APP_SRCS  += packs_fixture.c selftest_library.c selftest_packs_page.c
endif
ifeq ($(WORLD),terrarium)
APP_SRCS  += sheet_lizard.c   # --lizard-sheet (P3a; the lizard and desert art are the modules' below)
APP_SRCS  += shot_terrarium.c   # the terrarium as render_tank draws it: --terr-shot, --terr-bench (P3b)
endif
# THE MODULES (issue #4 phase 3): this world's mechanics (common/worlds/<world>/, critters/ below it), its body type's art
# (common/bodies/<body>/) and the movement modules (common/locomotion/) are found by directory, so a new file needs no list
BODY      := $(if $(filter terrarium,$(WORLD)),lizard,fish)
APP_SRCS  += $(sort $(wildcard ../common/worlds/$(WORLD)/*.c ../common/worlds/$(WORLD)/*/*.c ../common/bodies/$(BODY)/*.c ../common/locomotion/*.c))
APP_SRCS  += sim_tmp.c          # the run's own scratch directory: no selftest names a fixed /tmp path (P2c)
LVGL_SRCS := $(shell find lvgl/src -name '*.c')
OBJS      := $(patsubst %.c,$(BUILD)/%.o,$(filter-out ../common/%,$(APP_SRCS)) $(LVGL_SRCS)) $(patsubst ../common/%.c,$(BUILD)/common/%.o,$(filter ../common/%,$(APP_SRCS)))

# THE BUILT-IN PACKS' FILES (issue #4 phase 1, docs/ptpk.md "Authoring"): the const species_t / habitat_t of common/species and
# common/habitat are the packs' source; tools/ptpk_emit.c encodes them (common/packs/ptpk_write.c, the table in builtin.c) into
# packs/<uid_ns>.ptpk, the files a card gets. `make packs` builds that emitter and runs it - a file is rewritten only when its
# bytes differ - and the default build and `make check` run it, so those files cannot be stale against the sim's own built-ins.
# Its objects are the sim's own, compiled with its CFLAGS (every build here is -DPT_ALL_PACKS=1: the table holds all eight
# packs): the encoder, the table, the four const definitions and the two model manifests, no registry.
EMIT      := $(BUILD)/ptpk_emit
EMIT_OBJS := $(BUILD)/tools/ptpk_emit.o $(patsubst %,$(BUILD)/common/%.o,packs/ptpk_write packs/builtin packs/models species/gecko species/fish habitat/desert habitat/freshwater)

all: $(OUT) packs

$(OUT): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(EMIT): $(EMIT_OBJS)
	$(CC) -o $@ $(EMIT_OBJS) $(ARCH) -lm

packs: $(EMIT)
	@$(EMIT) ../packs

# THE SELFTESTS, by world (P2b): the shared ones hold in both bins; the aquarium's assert the fish - the swim, the
# grass and the pieces they swim among, the shop's meals and pieces, the fish brain's census - and stay as pocket-tank
# wrote them; the terrarium's assert the lizards (the heat, the shed, the mister, the L1 line, the walk). A shared test
# with one fish-only step runs that step in the aquarium alone (PT_TERRARIUM: the card's staged fish swimming to its
# stage - a lizard walks under it -, the clock's door visit). `make check` runs the world built.
# THE RENDER, by world (P3b): render_tank draws the aquarium's water, grass, pieces and critters only in the aquarium's
# bin (the terrarium's draws the desert and its lizards: render.c THE TERRARIUM), so the tests of that scene - the grass
# loop and cache, the pieces' and critters' stack, the castle, the cave and the roost, the boot show - are the
# aquarium's; the terrarium's own is --selftest-terr-render (its two scenes, the switch, the moisture, the hides' fronts).
SELFTESTS_SHARED := --selftest-battery --selftest-card --selftest-keeper \
             --selftest-saves --selftest-update --selftest-genes --selftest-advanced --selftest-model --selftest-restore --selftest-shelf --selftest-shelf-safe --selftest-shelf-away --selftest-temper --selftest-board --selftest-touch-lock --selftest-shelf-rename --selftest-bake-order --selftest-order-squares --selftest-clock --selftest-under-card
SELFTESTS_SHARED += --selftest-double-tap   # a double tap never buys: the shop, MY TANKS, a card's SELL (0.4 rc)
SELFTESTS_SHARED += --selftest-packs        # every pack of both worlds against its rules; this world's pairs held, the other's refused (P2a, P2b)
SELFTESTS_SHARED += --selftest-ptpk         # the pack file format and its loader: every built-in pack encoded from its const struct, read back equal, the file in packs/ the same; hostile files refused, a folder, a tank without its pack (issue #4 phase 1)
SELFTESTS_SHARED += --selftest-caps         # capabilities (issue #4 phase 2): what a species needs, a habitat, the firmware and the model provide; the pair check and its reasons, the model manifests, a tank whose pair cannot run
ifeq ($(CARD),1)
SELFTESTS_SHARED += --selftest-library     # the card library (issue #4 phase 4): scan, check, install, the pack store across reboots and power cuts, update, remove, a hostile card, a tank on an installed pack
SELFTESTS_SHARED += --selftest-packs-page  # the PACKS page under ADVANCED on this board: layout inside the glass, taps, installs and removals through the page, paging, the confirm
endif
SELFTESTS_AQUARIUM := --selftest --selftest-hunger --selftest-pop --selftest-fry --selftest-sleep --selftest-tend --selftest-llm \
             --selftest-breather --selftest-hold --selftest-depth --selftest-depth-pop --selftest-hold-emerge --selftest-fish-pair --selftest-critter-nav
SELFTESTS_AQUARIUM += --selftest-shop   # the meals the fish eat at the surface, the pieces
SELFTESTS_AQUARIUM += --selftest-grasscache --selftest-floor --selftest-stack --selftest-pumpkin-light --selftest-grassloop --selftest-grasspatch \
             --selftest-footprint --selftest-place --square-boot --selftest-cave --selftest-roost --selftest-boot-show --selftest-front-keep \
             --selftest-seahorse-stack --selftest-castle-keep --selftest-seahorse-post   # the aquarium's scene as render_tank draws it (P3b: shared until then)
SELFTESTS_TERRARIUM := --selftest-heat --selftest-shed --selftest-mister --selftest-l1-encode   # P2a
SELFTESTS_TERRARIUM += --selftest-walk --selftest-goals --selftest-crickets --selftest-gestures --selftest-terr-pay --selftest-terrarium   # P2b/P2c: the floor, goals, food/hands, pay, census
SELFTESTS_TERRARIUM += --selftest-terr-render                                                  # P3b: the desert's scenes and the lizards' stack
SELFTESTS_TERRARIUM += --selftest-terr-setup                                                   # P5: the first run's MORPH pages, kept across a reboot
SELFTESTS_TERRARIUM += --selftest-terr-progress                                                # P5: its badges, a hatchling's gates and the pack's cap, the shop's three
SELFTESTS_TERRARIUM += --selftest-terr-deco                                                    # P5b: the decorations - bought, kept clear of hides/dish/ramps/bask, saved, shelved, sold
# THE NETWORK's selftests (version.h PT_NET), in the networked bin alone (NET=1): the passport, the link engine under a
# hostile radio, GIVE and the visitor collection's pages in both worlds; the visits through the doors and a cross-tank
# fry's cap in the aquarium's (a lizard never takes a door). The shared tests with networked steps of their own (#if PT_NET
# inside: the KIN book, a fish away at the save, the UPDATES page and the wizard, the GIVE modal and the gift card, the
# visitor badge, CHECK FOR UPDATES, a visit at a late clock) run those steps there too.
SELFTESTS_NET_SHARED   := --selftest-passport --selftest-link --selftest-give --selftest-coll-pages
SELFTESTS_NET_AQUARIUM := --selftest-doors --selftest-fry-cap
SELFTESTS_NET_PARTS    := --selftest-shelf --selftest-saves --selftest-update --selftest-under-card --selftest-keeper --selftest-board --selftest-clock
ifeq ($(WORLD),aquarium)
SELFTESTS := $(SELFTESTS_SHARED) $(SELFTESTS_AQUARIUM)
SELFTESTS_NET_ONLY := $(SELFTESTS_NET_SHARED) $(SELFTESTS_NET_AQUARIUM)
else
SELFTESTS := $(SELFTESTS_SHARED) $(SELFTESTS_TERRARIUM)
SELFTESTS_NET_ONLY := $(SELFTESTS_NET_SHARED)
endif
SELFTESTS_NET := $(SELFTESTS_NET_ONLY) $(SELFTESTS_NET_PARTS)
ifeq ($(NET),1)
SELFTESTS += $(SELFTESTS_NET_ONLY)   # (`make NET=1 check`: every test the networked bin has)
endif
# the dev repo does not track model/out (the public one does): NOMODEL=1 runs
# the same checks without the LLM selftest, for a checkout with no model
ifdef NOMODEL
SELFTESTS := $(filter-out --selftest-llm --selftest-model,$(SELFTESTS))
endif

# `make check` tests the rectangle in the world built (the terrarium unless WORLD=aquarium); `make ROUND=1 check` /
# `make WATCH=1 check` / `make SQUARE=1 check` the same selftests in that board's world; `make check-worlds` both
# worlds on one board; `make check-net` the network's tests (SELFTESTS_NET) in the networked bin of the world built;
# `make check-all` both worlds on all four boards, and the network's tests in both worlds' networked bins on each -
# the gate for anything in common/ (docs/BOARDS.md)
check: $(OUT) packs
	@set -e; for test in $(SELFTESTS); do echo "RUN $(OUT) $$test"; ./$(OUT) "$$test"; done

ifeq ($(NET),1)
check-net: $(OUT)
	@set -e; for test in $(SELFTESTS_NET); do echo "RUN $(OUT) $$test"; ./$(OUT) "$$test"; done
else
check-net:
	$(MAKE) NET=1 check-net
endif

check-worlds:
	$(MAKE) WORLD=terrarium check
	$(MAKE) WORLD=aquarium check
	$(MAKE) check-release

# THE RELEASE IMAGE's packs (issue #4 phase 2): every fishsim above is built -DPT_ALL_PACKS=1 - both worlds' built-in packs, a
# firmware_caps() that is both worlds' union - and a firmware image is not: its own world's packs, its own world's capabilities,
# the other world's pack refused at load naming the module it moves by. release_check.c is that configuration from the common
# sources alone (no SDL, no LVGL), once for each world; run from here so that it finds ../packs.
RELEASE_COMMON := ../common/species.c ../common/habitat.c ../common/terrain.c ../common/packs/ptpk.c ../common/packs/ptpk_write.c \
                  ../common/packs/builtin.c ../common/packs/caps.c ../common/packs/manifest.c ../common/packs/models.c ../common/packs/compat.c
RELEASE_WORLD_2 := ../common/species/gecko.c ../common/habitat/desert.c
RELEASE_WORLD_1 := ../common/species/fish.c ../common/habitat/freshwater.c
check-release: packs
	@mkdir -p build-release
	@set -e; for w in 2 1; do \
	  echo "RUN release-check PT_WORLD=$$w"; \
	  $(CC) $(ARCH) -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -DPT_WORLD=$$w -DPT_ALL_PACKS=0 -DPT_NET=0 -I. -I../common -I../common/llm \
	    release_check.c $(RELEASE_COMMON) $$(if [ $$w = 2 ]; then echo $(RELEASE_WORLD_2); else echo $(RELEASE_WORLD_1); fi) -o build-release/release-check-$$w -lm; \
	  ./build-release/release-check-$$w; \
	done

check-net-worlds:
	$(MAKE) WORLD=terrarium check-net
	$(MAKE) WORLD=aquarium check-net

check-all:
	$(MAKE) check-worlds
	$(MAKE) ROUND=1 check-worlds
	$(MAKE) WATCH=1 check-worlds
	$(MAKE) SQUARE=1 check-worlds
	$(MAKE) check-net-worlds
	$(MAKE) ROUND=1 check-net-worlds
	$(MAKE) WATCH=1 check-net-worlds
	$(MAKE) SQUARE=1 check-net-worlds

check-nomodel:
	$(MAKE) NOMODEL=1 check

$(BUILD)/common/llm/%.o: ../common/llm/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

$(BUILD)/common/%.o: ../common/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tools/%.o: ../tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build build-round build-watch build-square fishsim fishsim-round fishsim-watch fishsim-square
	rm -rf build-aquarium build-round-aquarium build-watch-aquarium build-square-aquarium fishsim-aquarium fishsim-round-aquarium fishsim-watch-aquarium fishsim-square-aquarium
	rm -rf build*-net fishsim*-net build*-nocard fishsim*-nocard build-release

-include $(OBJS:.o=.d) $(EMIT_OBJS:.o=.d)

.PHONY: all packs check check-net check-worlds check-net-worlds check-all check-nomodel check-release clean
