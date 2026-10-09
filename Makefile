# Unity builds: each program is one translation unit (src/main_*.c).
#
#   make            release game and headless runner
#   make play       the game: continue the world in world/ (a new one from
#                   worlds/three.world the first time); WORLD=FILE starts over
#   make check      local CI: clean ASan/UBSan tests, replay determinism,
#                   then a gcc-15 -Werror -fanalyzer build
#   make bench      time the sim at 5000 units per side
#   make bench-teams   8 bots a side in one zone, ten game minutes
#   make determinism-x86   arm64 record, x86-64 (Rosetta) verify, and back
#   make cross      x86-64 Linux (run in Docker) and Windows builds
#   make clips      every scenario as build/clips/NAME.mp4 + .png sheet + .txt
#   make setup      once per clone: hooks, and LFS told not to fetch demo videos
#   make demos      render demos/*.script whose clip is missing or stale
#   make demos-restore   re-render demo videos that are only LFS pointers
#   make share DEMO=NAME   full-quality copy of a demo, on the clipboard
#   make soak MINUTES=N   zones, a coordinator and a player under random kills
#   make world-soak MINUTES=N [WORLD=FILE]   a self-playing world under random kills

CC      := clang
GCC     := gcc-15
B       := build

WARN    := -Wall -Wextra -Wconversion -Wshadow -Wstrict-prototypes -Wno-unused-function
# Determinism (docs/conventions.md): never fuse multiply-adds, never fast-math.
SIMFP   := -ffp-contract=off -fno-fast-math
BASE    := -std=c23 $(WARN) $(SIMFP) -Isrc
DEBUG   := -O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined
# Same sanitizers, optimized: CI's test run is ~4x faster than at -O0.
SAN     := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
RELEASE := -O2 -g

RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib)

SRC      := $(wildcard src/*.c src/*.h)
SIM_SRC  := $(filter-out src/main_game.c src/render.c src/render.h src/view.c src/view.h src/client.c src/clip.c src/clip.h,$(SRC))
# The zone smoke test drives these through processes; rebuild them together.

.PHONY: play world-soak work-log demos-rerender all setup check check-zones check-world check-gcc-coord check-tests check-replay check-split check-demo-scripts check-game check-server check-net check-gcc check-gcc-tests check-gcc-headless check-gcc-server check-gcc-game bench bench-teams determinism-x86 soak cross clips demos demos-restore share clean

all: $(B)/rts $(B)/rts-headless $(B)/rts-server $(B)/rts-coord

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

$(B)/rts-server: src/main_server.c $(SIM_SRC) | $(B)
	$(CC) $(BASE) $(RELEASE) $< -o $@

$(B)/rts-coord: src/main_coord.c $(SIM_SRC) | $(B)
	$(CC) $(BASE) $(RELEASE) $< -o $@

# The game. The world keeps running when the window closes: `build/rts world
# --stop` stops it (every zone saves first).
play: all
	@if [ -n "$(WORLD)" ]; then $(B)/rts world --new $(WORLD); \
	elif [ -f world/live/world.txt ]; then $(B)/rts world; \
	else $(B)/rts world --new worlds/three.world; fi

bench: $(B)/rts-headless
	$(B)/rts-headless bench 1800 5000

# Eight players a side, all bots, ten game minutes: what a full team zone costs.
bench-teams: $(B)/rts-headless
	$(B)/rts-headless bench-teams 18000 8

# Local CI. Builds from scratch in its own directory: nothing stale is reused.
# Stages are independent and run in parallel, each timed by tools/stage
# (history in build/timings.log), so check costs about its slowest stage.
CHECK := $(B)/check
S := tools/stage
CHECK_STAGES := check-tests check-replay check-split check-demo-scripts check-game check-server check-gcc check-net check-zones check-world
check:
	@rm -rf $(CHECK) && mkdir -p $(CHECK)
	@$(S) "check (total)" $(MAKE) --no-print-directory -j8 $(CHECK_STAGES)
	@echo "== check passed"

check-tests:
	@$(S) "build tests (clang -O1, ASan+UBSan)" $(CC) $(BASE) -Werror $(SAN) src/main_tests.c -o $(CHECK)/tests
	@$(S) "run tests (in parallel)" sh -c '$(CHECK)/tests --list | xargs -P 8 -n 1 $(CHECK)/tests > $(CHECK)/tests.log 2>&1 || { cat $(CHECK)/tests.log; exit 1; }'

$(CHECK)/headless:
	@$(S) "build headless (clang -O2)" $(CC) $(BASE) -Werror $(RELEASE) src/main_headless.c -o $@

check-replay: $(CHECK)/headless
	@$(S) "replay: record 900 ticks, 2000 units" $(CHECK)/headless record $(CHECK)/run.replay 900 2000 > /dev/null
	@$(S) "replay: verify" $(CHECK)/headless verify $(CHECK)/run.replay > /dev/null

check-split: $(CHECK)/headless
	@$(S) "split: both factions rebuilt from their streams" $(CHECK)/headless split 600 1500 > /dev/null

# One process per demo script, side by side.
check-demo-scripts: $(CHECK)/headless
	@$(S) "demo scripts: expectations (in parallel)" sh -c 'ls demos/*.script | xargs -P 8 -n 1 $(CHECK)/headless script > $(CHECK)/demos.log 2>&1 || { cat $(CHECK)/demos.log; exit 1; }'

$(CHECK)/rts:
	@$(S) "build game (clang -O2)" $(CC) $(BASE) -Werror $(RELEASE) $(RAYLIB_CFLAGS) src/main_game.c -o $@ $(RAYLIB_LIBS)

check-game: $(CHECK)/rts
	@$(S) "clip: scenario contact sheet" $(CHECK)/rts clip --scenario skirmish --to 600 \
		--sheet $(CHECK)/skirmish.png --frames 4 --save-replay $(CHECK)/skirmish.replay > /dev/null
	@$(S) "clip: replay must reproduce" $(CHECK)/rts clip --replay $(CHECK)/skirmish.replay --last 3 \
		--sheet $(CHECK)/replayed.png --frames 4 > /dev/null

$(CHECK)/rts-server:
	@$(S) "build server (clang -O2)" $(CC) $(BASE) -Werror $(RELEASE) src/main_server.c -o $@

$(CHECK)/rts-coord:
	@$(S) "build coordinator (clang -O2)" $(CC) $(BASE) -Werror $(RELEASE) src/main_coord.c -o $@

# Three zones on a line and a coordinator: an unpaired send waits through a
# coordinator restart, a crew builds a transporter, tanks relay two hops and
# wait for room to land, a player switches zones.
check-zones: $(CHECK)/rts $(CHECK)/rts-server $(CHECK)/rts-coord
	@$(S) "zones: unpaired, restart, relay, push-back, switch" tools/zone-smoke $(CHECK) $(CHECK)/zones > $(CHECK)/zones.log 2>&1 \
		|| { cat $(CHECK)/zones.log; exit 1; }

# The game through `rts world`: start, a player's journey across the map and
# two zones, stop and continue, save and load, the journey's clip.
check-world: $(CHECK)/rts $(CHECK)/rts-server $(CHECK)/rts-coord
	@$(S) "world: journey, stop/continue, save/load" tools/world-smoke $(CHECK) $(CHECK)/world > $(CHECK)/world.log 2>&1 \
		|| { cat $(CHECK)/world.log; exit 1; }

check-server: $(CHECK)/rts-server

# A server and a headless client over a real socket, with latency, jitter and
# a forced rejoin: every digest must match and the server's run must verify.
NET := $(CHECK)/net
check-net: $(CHECK)/rts $(CHECK)/rts-server $(CHECK)/headless
	@mkdir -p $(NET)
	@$(S) "net: server + client, 10 s with a rejoin" tools/net-smoke $(CHECK) $(NET) 300
	@$(S) "net: server replay verifies" $(CHECK)/headless verify $(NET)/server.replay > /dev/null

# One stage per program: the analyzer is the slowest step, so run them side by side.
check-gcc: check-gcc-tests check-gcc-headless check-gcc-server check-gcc-game check-gcc-coord
check-gcc-coord:
	@$(S) "gcc -fanalyzer: coordinator" $(GCC) $(BASE) -Werror -O1 -fanalyzer src/main_coord.c -o $(CHECK)/coord-gcc
check-gcc-tests:
	@$(S) "gcc -fanalyzer: tests" $(GCC) $(BASE) -Werror -O1 -fanalyzer src/main_tests.c -o $(CHECK)/tests-gcc
check-gcc-headless:
	@$(S) "gcc -fanalyzer: headless" $(GCC) $(BASE) -Werror -O1 -fanalyzer src/main_headless.c -o $(CHECK)/headless-gcc
check-gcc-server:
	@$(S) "gcc -fanalyzer: server" $(GCC) $(BASE) -Werror -O1 -fanalyzer src/main_server.c -o $(CHECK)/server-gcc
check-gcc-game:
	@$(S) "gcc: game compiles" $(GCC) $(BASE) -Werror -O1 $(RAYLIB_CFLAGS) -c src/main_game.c -o $(CHECK)/game-gcc.o

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

# Persistence under chaos: alpha and the coordinator are SIGKILLed at random
# while units cross between zones and a player logs in and out. Checks no unit
# is lost or duplicated, replay segments chain, and beta's memory stays flat.
MINUTES ?= 5
soak: all
	@$(S) "soak: $(MINUTES) min" python3 tools/soak.py $(B) $(B)/soak --minutes $(MINUTES)

# The world playing itself: bots on both sides everywhere, zones and the
# coordinator SIGKILLed at random, players dropping in. Checks conservation,
# replays, memory, and that both sides expanded.
world-soak: all
	@$(S) "world soak: $(MINUTES) min" python3 tools/world-soak.py $(B) $(B)/world-soak --minutes $(MINUTES) \
		--world $(or $(WORLD),worlds/five.world)

# Copy every Claude Code session transcript for this repo into work_log/
# (gitignored but for its README), named by date and session id.
work-log:
	@mkdir -p work_log
	@for f in $(HOME)/.claude/projects/-Users-martinboros-git-rts/*.jsonl; do \
		id=$$(basename $$f .jsonl | cut -c1-8); d=$$(date -r $$f +%Y-%m-%d); \
		cp $$f work_log/$$(ls work_log | grep -m1 "session-$$id.jsonl" || echo "$$d-session-$$id.jsonl"); \
	done; ls -la work_log

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
# Retired demos (a "RETIRED" line at the top) keep their video and sheet.
DEMO_RUNS := $(shell grep -L "^. RETIRED" demos/*.sh)
demos: $(DEMO_SCRIPTS:.script=.mp4) $(DEMO_RUNS:.sh=.mp4)

DEMO_RENDER = $(B)/rts clip --script demos/$(1).script --out demos/$(1).mp4 \
	--sheet $(B)/demo-$(1).png --size 960x540 --crf 30 --label "$(1)" && \
	ffmpeg -loglevel error -y -i $(B)/demo-$(1).png -q:v 6 demos/$(1).jpg

demos/%.mp4: demos/%.script | $(B)/rts
	$(call DEMO_RENDER,$*)

# Network demos are real runs over sockets (see demos/04-*.sh).
demos/%.mp4: demos/%.sh | $(B)/rts $(B)/rts-server $(B)/rts-headless
	B=$(B) OUT=demos/$* SIZE=960x540 CRF=30 LABEL="$*" sh $<
	ffmpeg -loglevel error -y -i $(B)/demo-$*.png -q:v 6 demos/$*.jpg

# Videos are reproducible from their scripts, so a clone that only has LFS
# pointers rebuilds them instead of downloading them.
demos-restore: $(B)/rts
	@for v in demos/*.mp4; do \
		if head -c 40 "$$v" | grep -q '^version https://git-lfs'; then \
			d=$$(basename "$$v" .mp4); [ -f demos/$$d.script ] || { echo "skipping $$d (a run or retired)"; continue; }; \
			echo "restoring $$d"; \
			$(call DEMO_RENDER,$$d) || exit 1; \
		fi; \
	done
	@# Rendering is deterministic down to the encoded bytes, so this only
	@# refreshes git's stat cache; the videos match their pointers.
	@git add --renormalize demos/ && git status --short demos/

# For posting: 720p at the default quality, labelled with the commit.
# After any change to rendering or clip output: re-render every demo, in the
# same commit, so demos-restore keeps reproducing them byte for byte.
# Network demos re-render from their last netlog when there is one.
demos-rerender: $(B)/rts $(B)/rts-server $(B)/rts-headless
	@for s in demos/*.script; do d=$$(basename $$s .script); \
		$(S) "rerender $$d" sh -c '$(call DEMO_RENDER,'"$$d"') > /dev/null' || exit 1; done
	@for s in demos/*.sh; do d=$$(basename $$s .sh); grep -q '^# RETIRED' $$s && continue; \
		$(S) "rerender $$d" sh -c "B=$(B) OUT=demos/$$d SIZE=960x540 CRF=30 LABEL=$$d RENDER_ONLY=1 sh $$s > /dev/null && \
			ffmpeg -loglevel error -y -i $(B)/demo-$$d.png -q:v 6 demos/$$d.jpg" || exit 1; done

# A network demo re-renders from its last netlog rather than re-running.
share: $(B)/rts
	@test -n "$(DEMO)" || { echo "usage: make share DEMO=02-scripted-flank"; exit 2; }
	@mkdir -p $(B)/share
	@if [ -f demos/$(DEMO).sh ]; then \
		B=$(B) OUT=$(B)/share/$(DEMO) SIZE=1280x720 CRF=26 RENDER_ONLY=1 CLIPBOARD=1 \
			LABEL="$$(git describe --always --dirty)" sh demos/$(DEMO).sh; \
	else \
		$(B)/rts clip --script demos/$(DEMO).script --out $(B)/share/$(DEMO).mp4 \
			--label "$$(git describe --always --dirty)" --clipboard; \
	fi

clean:
	rm -rf $(B)
