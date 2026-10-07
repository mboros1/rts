# Unity builds: each program is one translation unit (src/main_*.c).
#
#   make            release game and headless runner
#   make check      local CI: clean ASan/UBSan tests, replay determinism,
#                   then a gcc-15 -Werror -fanalyzer build
#   make bench      time the sim at 5000 units per side
#   make determinism-x86   arm64 record, x86-64 (Rosetta) verify, and back
#   make cross      x86-64 Linux (run in Docker) and Windows builds

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

.PHONY: all check bench determinism-x86 cross clean

all: $(B)/rts $(B)/rts-headless

$(B):
	@mkdir -p $(B)

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

clean:
	rm -rf $(B)
