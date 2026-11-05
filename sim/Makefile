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

# PT_ALL_PACKS is the simulator's cross-world validation inventory (world.h); release firmware links only its world's packs.
CFLAGS  := $(ROUNDF) $(WORLDF) $(NETF) $(EXTRA_CFLAGS) -DPT_ALL_PACKS=1 -O2 -std=c11 $(ARCH) -I. -I../common -I../common/llm -DLV_CONF_INCLUDE_SIMPLE -DSOUNDS_BIN='"../assets/sounds/sounds.bin"' -DPT_VERSION='"$(shell git describe --always --dirty --exclude='*' 2>/dev/null || echo sim)"' $(SDL_CFLAGS) \
           -Wall -Wextra -Wno-unused-parameter -MMD -MP
LDFLAGS := $(ARCH) $(SDL_LIBS) -lm

APP_SRCS  := main.c advisor_llm.c persist_port_sim.c ../common/tank.c ../common/advanced.c ../common/shelf.c ../common/boot_show.c ../common/advisor.c ../common/render.c ../common/icons.c ../common/progression.c ../common/setup.c ../common/audio.c ../common/notice.c ../common/sounds.c ../common/battery.c ../common/llm/q4_model.c ../common/llm/word_tok.c ../common/llm/advisor_core.c

ifeq ($(NET),1)   # the networked bin's: updates, the passport, the link engine and the doors, the visitor collection, GIVE
APP_SRCS  += ../common/update.c ../common/passport.c ../common/link.c ../common/tank_link.c ../common/collection.c ../common/gift.c
APP_SRCS  += net_port_sim.c selftest_link.c selftest_coll.c   # the pretend radio; the link's tests; the visitor collection's pages (0.4)
endif
APP_SRCS  += ../common/species.c ../common/species/gecko.c ../common/species/fish.c ../common/habitat.c ../common/habitat/desert.c ../common/habitat/freshwater.c   # the packs, both worlds' (P2a, P2b)
APP_SRCS  += ../common/terrain.c   # a habitat's ground laid out on a glass: the walk, the heat, the scene, the rules (P2c)
APP_SRCS  += selftest_packs.c selftest_l1.c   # the packs, heat, shed and mister (P2a); the L1 encoder against the Python fixture (R4)
APP_SRCS  += selftest_walk.c  # the terrarium's walk, its goals and a free-running census (P2b)
ifeq ($(WORLD),terrarium)
APP_SRCS  += ../common/render_lizard.c ../common/render_scene_desert.c sheet_lizard.c   # this world's lizard/desert art; --lizard-sheet (P3a)
APP_SRCS  += shot_terrarium.c   # the terrarium as render_tank draws it: --terr-shot, --terr-bench (P3b)
endif
APP_SRCS  += sim_tmp.c          # the run's own scratch directory: no selftest names a fixed /tmp path (P2c)
LVGL_SRCS := $(shell find lvgl/src -name '*.c')
OBJS      := $(patsubst %.c,$(BUILD)/%.o,$(filter-out ../common/%,$(APP_SRCS)) $(LVGL_SRCS)) $(patsubst ../common/%.c,$(BUILD)/common/%.o,$(filter ../common/%,$(APP_SRCS)))

$(OUT): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

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
             --selftest-saves --selftest-update --selftest-genes --selftest-advanced --selftest-model --selftest-restore --selftest-shelf --selftest-shelf-safe --selftest-shelf-away --selftest-temper --selftest-board --selftest-critter-nav --selftest-touch-lock --selftest-shelf-rename --selftest-bake-order --selftest-order-squares --selftest-clock --selftest-under-card
SELFTESTS_SHARED += --selftest-double-tap   # a double tap never buys: the shop, MY TANKS, a card's SELL (0.4 rc)
SELFTESTS_SHARED += --selftest-packs        # every pack of both worlds against its rules; this world's pairs held, the other's refused (P2a, P2b)
SELFTESTS_AQUARIUM := --selftest --selftest-hunger --selftest-pop --selftest-fry --selftest-sleep --selftest-tend --selftest-llm \
             --selftest-breather --selftest-hold --selftest-depth --selftest-depth-pop --selftest-hold-emerge --selftest-fish-pair
SELFTESTS_AQUARIUM += --selftest-shop   # the meals the fish eat at the surface, the pieces
SELFTESTS_AQUARIUM += --selftest-grasscache --selftest-floor --selftest-stack --selftest-pumpkin-light --selftest-grassloop --selftest-grasspatch \
             --selftest-footprint --selftest-place --square-boot --selftest-cave --selftest-roost --selftest-boot-show --selftest-front-keep \
             --selftest-seahorse-stack --selftest-castle-keep --selftest-seahorse-post   # the aquarium's scene as render_tank draws it (P3b: shared until then)
SELFTESTS_TERRARIUM := --selftest-heat --selftest-shed --selftest-mister --selftest-l1-encode   # P2a
SELFTESTS_TERRARIUM += --selftest-walk --selftest-goals --selftest-crickets --selftest-gestures --selftest-terr-pay --selftest-terrarium   # P2b/P2c: the floor, goals, food/hands, pay, census
SELFTESTS_TERRARIUM += --selftest-terr-render                                                  # P3b: the desert's scenes and the lizards' stack
SELFTESTS_TERRARIUM += --selftest-terr-setup                                                   # P5: the first run's MORPH pages, kept across a reboot
SELFTESTS_TERRARIUM += --selftest-terr-progress                                                # P5: its badges, a hatchling's gates and the pack's cap, the shop's three
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
check: $(OUT)
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

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build build-round build-watch build-square fishsim fishsim-round fishsim-watch fishsim-square
	rm -rf build-aquarium build-round-aquarium build-watch-aquarium build-square-aquarium fishsim-aquarium fishsim-round-aquarium fishsim-watch-aquarium fishsim-square-aquarium
	rm -rf build*-net fishsim*-net

-include $(OBJS:.o=.d)

.PHONY: check check-net check-worlds check-net-worlds check-all check-nomodel clean
