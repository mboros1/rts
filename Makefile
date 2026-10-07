# Unity builds: each program is one translation unit (src/main_*.c).
#
#   make            release game and headless runner
#   make check      local CI: clean ASan/UBSan tests, replay determinism,
#                   then a gcc-15 -Werror -fanalyzer build
#   make bench      time the sim at 5000 units per side
#   make determinism-x86   arm64 record, x86-64 (Rosetta) verify, and back
#   make cross      x86-64 Linux (run in Docker) and Windows builds
#   make clips      every scenario as build/clips/NAME.mp4 + .png sheet + .txt
#   make setup      once per clone: hooks, and LFS told not to fetch demo videos
#   make demos      render demos/*.script whose clip is missing or stale
#   make demos-restore   re-render demo videos that are only LFS pointers
#   make share DEMO=NAME   full-quality copy of a demo, on the clipboard

CC      := clang
GCC     := gcc-15
B       := build

WARN    := -Wall -Wextra -Wconversion -Wshadow -Wstrict-prototypes -Wno-unused-function
# Determinism (docs/conventions.md): never fuse multiply-adds, never fast-math.
SIMFP   := -ffp-contract=off -fno-fast-math
BASE    := -std=c23 $(WARN) $(SIMFP) -Isrc
DEBUG   := -O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined
RELEASE := -O2 -g

RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib)

SRC      := $(wildcard src/*.c src/*.h)
SIM_SRC  := $(filter-out src/main_game.c src/render.c src/render.h,$(SRC))

.PHONY: all setup check bench determinism-x86 cross clips demos demos-restore share clean

all: $(B)/rts $(B)/rts-headless

$(B):
	@mkdir -p $(B)

# Clone with GIT_LFS_SKIP_SMUDGE=1 (demo videos are never on the server),
# then run this once.
setup:
	git config core.hooksPath tools/hooks
	git config lfs.fetchexclude 'demos/*.mp4'
	@echo "hooks and LFS configured; 'make demos-restore' rebuilds demo videos"

$(B)/rts: src/main_game.c $(SRC) | $(B)
	$(CC) $(BASE) $(RELEASE) $(RAYLIB_CFLAGS) $< -o $@ $(RAYLIB_LIBS)

$(B)/rts-debug: src/main_game.c $(SRC) | $(B)
	$(CC) $(BASE) $(DEBUG) $(RAYLIB_CFLAGS) $< -o $@ $(RAYLIB_LIBS)

$(B)/rts-headless: src/main_headless.c $(SIM_SRC) | $(B)
	$(CC) $(BASE) $(RELEASE) $< -o $@

bench: $(B)/rts-headless
	$(B)/rts-headless bench 1800 5000

# Local CI. Builds from scratch in its own directory: nothing stale is reused.
CHECK := $(B)/check
check:
	@rm -rf $(CHECK) && mkdir -p $(CHECK)
	@echo "== clang, ASan + UBSan: tests"
	$(CC) $(BASE) -Werror $(DEBUG) src/main_tests.c -o $(CHECK)/tests
	$(CHECK)/tests
	@echo "== clang release: replay determinism"
	$(CC) $(BASE) -Werror $(RELEASE) src/main_headless.c -o $(CHECK)/headless
	$(CHECK)/headless record $(CHECK)/run.replay 900 2000
	$(CHECK)/headless verify $(CHECK)/run.replay
	@echo "== clang: game builds"
	$(CC) $(BASE) -Werror $(RELEASE) $(RAYLIB_CFLAGS) src/main_game.c -o $(CHECK)/rts $(RAYLIB_LIBS)
	@echo "== gcc-15 -Werror -fanalyzer"
	$(GCC) $(BASE) -Werror -O1 -fanalyzer src/main_tests.c -o $(CHECK)/tests-gcc
	$(GCC) $(BASE) -Werror -O1 -fanalyzer src/main_headless.c -o $(CHECK)/headless-gcc
	$(GCC) $(BASE) -Werror -O1 $(RAYLIB_CFLAGS) -c src/main_game.c -o $(CHECK)/game-gcc.o
	@echo "== clip smoke test: contact sheet, and a replay clip that must reproduce"
	$(CHECK)/rts clip --scenario skirmish --to 600 --sheet $(CHECK)/skirmish.png --frames 4 \
		--save-replay $(CHECK)/skirmish.replay
	$(CHECK)/rts clip --replay $(CHECK)/skirmish.replay --last 3 --sheet $(CHECK)/replayed.png --frames 4
	@echo "== demo scripts: every expectation must hold"
	$(CHECK)/headless script demos/*.script
	@echo "== check passed"

# Same replay on both architectures, both directions. x86-64 runs under Rosetta.
X86 := $(B)/x86
determinism-x86:
	@rm -rf $(X86) && mkdir -p $(X86)
	$(CC) $(BASE) $(RELEASE) -arch arm64 src/main_headless.c -o $(X86)/headless-arm64
	$(CC) $(BASE) $(RELEASE) -arch x86_64 src/main_headless.c -o $(X86)/headless-x86_64
	$(X86)/headless-arm64 record $(X86)/arm.replay 1800 5000
	$(X86)/headless-x86_64 verify $(X86)/arm.replay
	$(X86)/headless-x86_64 record $(X86)/x86.replay 1800 5000
	$(X86)/headless-arm64 verify $(X86)/x86.replay
	@echo "== arm64 and x86-64 agree on every tick"

CROSS := $(B)/cross
cross: $(B)/rts-headless
	@rm -rf $(CROSS) && mkdir -p $(CROSS)
	zig cc -target x86_64-linux-musl $(BASE) $(RELEASE) src/main_headless.c -o $(CROSS)/headless-linux
	x86_64-w64-mingw32-gcc $(BASE) $(RELEASE) -static src/main_headless.c -o $(CROSS)/headless.exe
	docker run --rm --platform linux/amd64 -v "$(CURDIR)/$(CROSS):/w" -w /w alpine:3 \
		sh -c './headless-linux record linux.replay 900 2000 && ./headless-linux verify linux.replay'
	$(B)/rts-headless verify $(CROSS)/linux.replay
	@echo "== linux x86-64 replay verifies natively"

CLIPS := $(B)/clips
clips: $(B)/rts
	@mkdir -p $(CLIPS)
	@for s in $$($(B)/rts clip --list | cut -d' ' -f1); do \
		$(B)/rts clip --scenario $$s --out $(CLIPS)/$$s.mp4 --sheet $(CLIPS)/$$s.png \
			--save-replay $(CLIPS)/$$s.replay || exit 1; \
	done

# Demos: one per feature, committed with it. The script is the source of
# truth (the sim is deterministic); the small .mp4 and .jpg sheet are kept in
# git so history shows each change without building anything. Rendered only
# when the script changes, never because the binary did.
DEMO_SCRIPTS := $(wildcard demos/*.script)
demos: $(DEMO_SCRIPTS:.script=.mp4)

DEMO_RENDER = $(B)/rts clip --script demos/$(1).script --out demos/$(1).mp4 \
	--sheet $(B)/demo-$(1).png --size 960x540 --crf 30 --label "$(1)" && \
	ffmpeg -loglevel error -y -i $(B)/demo-$(1).png -q:v 6 demos/$(1).jpg

demos/%.mp4: demos/%.script | $(B)/rts
	$(call DEMO_RENDER,$*)

# Videos are reproducible from their scripts, so a clone that only has LFS
# pointers rebuilds them instead of downloading them.
demos-restore: $(B)/rts
	@for v in demos/*.mp4; do \
		if head -c 40 "$$v" | grep -q '^version https://git-lfs'; then \
			d=$$(basename "$$v" .mp4); echo "restoring $$d"; \
			$(call DEMO_RENDER,$$d) || exit 1; \
		fi; \
	done
	@# Rendering is deterministic down to the encoded bytes, so this only
	@# refreshes git's stat cache; the videos match their pointers.
	@git add --renormalize demos/ && git status --short demos/

# For posting: 720p at the default quality, labelled with the commit.
share: $(B)/rts
	@test -n "$(DEMO)" || { echo "usage: make share DEMO=02-scripted-flank"; exit 2; }
	@mkdir -p $(B)/share
	$(B)/rts clip --script demos/$(DEMO).script --out $(B)/share/$(DEMO).mp4 \
		--label "$$(git describe --always --dirty)" --clipboard

clean:
	rm -rf $(B)
