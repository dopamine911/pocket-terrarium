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

CFLAGS  := $(ROUNDF) $(WORLDF) $(EXTRA_CFLAGS) -O2 -std=c11 $(ARCH) -I. -I../common -I../common/llm -DLV_CONF_INCLUDE_SIMPLE -DSOUNDS_BIN='"../assets/sounds/sounds.bin"' -DPT_VERSION='"$(shell git describe --always --dirty --exclude='*' 2>/dev/null || echo sim)"' $(SDL_CFLAGS) \
           -Wall -Wextra -Wno-unused-parameter -MMD -MP
LDFLAGS := $(ARCH) $(SDL_LIBS) -lm

APP_SRCS  := main.c selftest_link.c advisor_llm.c persist_port_sim.c net_port_sim.c ../common/tank.c ../common/update.c ../common/advanced.c ../common/shelf.c ../common/boot_show.c ../common/advisor.c ../common/render.c ../common/icons.c ../common/progression.c ../common/setup.c ../common/audio.c ../common/notice.c ../common/sounds.c ../common/battery.c ../common/passport.c ../common/link.c ../common/tank_link.c ../common/collection.c ../common/gift.c ../common/llm/q4_model.c ../common/llm/word_tok.c ../common/llm/advisor_core.c

APP_SRCS  += selftest_coll.c   # the visitor collection's pages (0.4)
APP_SRCS  += ../common/species.c ../common/species/gecko.c ../common/species/fish.c ../common/habitat.c ../common/habitat/desert.c ../common/habitat/freshwater.c   # the packs, both worlds' (P2a, P2b)
APP_SRCS  += ../common/terrain.c   # a habitat's ground laid out on a glass: the walk, the heat, the scene, the rules (P2c)
APP_SRCS  += selftest_packs.c selftest_l1.c   # the packs, heat, shed and mister (P2a); the L1 encoder against the Python fixture (R4)
APP_SRCS  += selftest_walk.c  # the terrarium's walk, its goals and a free-running census (P2b)
APP_SRCS  += ../common/render_lizard.c ../common/render_scene_desert.c sheet_lizard.c   # the lizard and the desert, standalone; --lizard-sheet (P3a)
LVGL_SRCS := $(shell find lvgl/src -name '*.c')
OBJS      := $(patsubst %.c,$(BUILD)/%.o,$(filter-out ../common/%,$(APP_SRCS)) $(LVGL_SRCS)) $(patsubst ../common/%.c,$(BUILD)/common/%.o,$(filter ../common/%,$(APP_SRCS)))

$(OUT): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# THE SELFTESTS, by world (P2b): the shared ones hold in both bins; the aquarium's assert the fish - the swim, the
# grass and the pieces they swim among, the doors they visit through, the shop's meals and pieces, the fish brain's
# census - and stay as pocket-tank wrote them; the terrarium's assert the lizards (the heat, the shed, the mister, the
# L1 line, the walk). A shared test with one fish-only step runs that step in the aquarium alone (PT_TERRARIUM: the card's
# staged fish swimming to its stage - a lizard walks under it -, the clock's door visit). `make check` runs the world built.
SELFTESTS_SHARED := --selftest-battery --selftest-card --selftest-keeper \
             --selftest-saves --selftest-update --selftest-genes --selftest-passport --selftest-link --selftest-give --selftest-advanced --selftest-model --selftest-restore --selftest-grasscache --selftest-floor --selftest-stack --selftest-shelf --selftest-shelf-safe --selftest-shelf-away --selftest-temper --selftest-board --selftest-critter-nav --selftest-pumpkin-light --selftest-grassloop --selftest-grasspatch --selftest-touch-lock --selftest-shelf-rename --selftest-footprint --selftest-bake-order --selftest-place --selftest-order-squares --square-boot --selftest-cave --selftest-roost --selftest-boot-show --selftest-front-keep --selftest-seahorse-stack --selftest-castle-keep --selftest-seahorse-post --selftest-clock --selftest-under-card
SELFTESTS_SHARED += --selftest-coll-pages
SELFTESTS_SHARED += --selftest-double-tap   # a double tap never buys: the shop, MY TANKS, a card's SELL (0.4 rc)
SELFTESTS_SHARED += --selftest-packs        # every pack of both worlds against its rules; this world's pairs held, the other's refused (P2a, P2b)
SELFTESTS_AQUARIUM := --selftest --selftest-hunger --selftest-pop --selftest-fry --selftest-sleep --selftest-tend --selftest-llm \
             --selftest-breather --selftest-hold --selftest-depth --selftest-depth-pop --selftest-hold-emerge --selftest-fish-pair
SELFTESTS_AQUARIUM += --selftest-shop --selftest-doors --selftest-fry-cap   # the meals the fish eat at the surface, the pieces; visits through the doors
SELFTESTS_TERRARIUM := --selftest-heat --selftest-shed --selftest-mister --selftest-l1-encode   # P2a
SELFTESTS_TERRARIUM += --selftest-walk --selftest-goals --selftest-terrarium                   # P2b: the floor, the goals, a free run
ifeq ($(WORLD),aquarium)
SELFTESTS := $(SELFTESTS_SHARED) $(SELFTESTS_AQUARIUM)
else
SELFTESTS := $(SELFTESTS_SHARED) $(SELFTESTS_TERRARIUM)
endif
# the dev repo does not track model/out (the public one does): NOMODEL=1 runs
# the same checks without the LLM selftest, for a checkout with no model
ifdef NOMODEL
SELFTESTS := $(filter-out --selftest-llm --selftest-model,$(SELFTESTS))
endif

# `make check` tests the rectangle in the world built (the terrarium unless WORLD=aquarium); `make ROUND=1 check` /
# `make WATCH=1 check` / `make SQUARE=1 check` the same selftests in that board's world; `make check-worlds` both
# worlds on one board; `make check-all` both worlds on all four boards - the gate for anything in common/
# (docs/BOARDS.md)
check: $(OUT)
	@set -e; for test in $(SELFTESTS); do echo "RUN $(OUT) $$test"; ./$(OUT) "$$test"; done

check-worlds:
	$(MAKE) WORLD=terrarium check
	$(MAKE) WORLD=aquarium check

check-all:
	$(MAKE) check-worlds
	$(MAKE) ROUND=1 check-worlds
	$(MAKE) WATCH=1 check-worlds
	$(MAKE) SQUARE=1 check-worlds

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

-include $(OBJS:.o=.d)

.PHONY: check check-worlds check-all check-nomodel clean
