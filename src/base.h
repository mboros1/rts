// Common definitions every module includes first.
//
// System headers are included here, before the allocator poison below, so a
// header that mentions malloc is read before the name becomes an error. No
// module includes a system header of its own except platform.c.
#pragma once

// Strict -std=c23 hides POSIX (clock_gettime, MAP_ANON) in glibc and musl;
// this brings it back. It must precede the first system header.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdalign.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// All memory comes from arenas (arena.h), backed by platform_reserve.
#pragma GCC poison malloc calloc realloc free

#define countof(a) ((ptrdiff_t)(sizeof(a) / sizeof(*(a))))

// Assertions stay on in every build: the sim is worth more correct than fast.
[[noreturn]] void assert_fail(const char *file, int line, const char *expr);
#define ASSERT(c) ((c) ? (void)0 : assert_fail(__FILE__, __LINE__, #c))

// Unrecoverable condition with a formatted reason, e.g. arena exhaustion.
[[noreturn]] void fatal(const char *fmt, ...);

// A slice of bytes. NUL-terminated only at the boundary with C libraries.
typedef struct {
	const char *ptr;
	ptrdiff_t len;
} Str;

#define S(lit) ((Str){(lit), countof(lit) - 1})

// Domain values are thin types, never bare integers (docs/conventions.md).
typedef struct {
	int64_t v;
} Tick;

typedef struct {
	uint32_t v;
} FactionId;

// A player: a slot within a side, 0 to MAX_PLAYERS - 1. Who is in the slot
// (a person's name, or a bot) is the coordinator's to say; units only carry
// the slot, so it holds in every zone and across sends and saves.
enum { MAX_PLAYERS = 8 };
typedef struct {
	uint8_t v;
} PlayerSlot;

static inline bool player_eq(PlayerSlot a, PlayerSlot b) { return a.v == b.v; }
static inline bool player_ok(PlayerSlot p) { return p.v < MAX_PLAYERS; }

// A unit, addressed through its faction's handle pool. Stale once the unit
// dies: the slot's generation moves on and lookups refuse the old handle.
typedef struct {
	uint32_t index;
	uint32_t generation;
} UnitHandle;

static inline bool unit_handle_eq(UnitHandle a, UnitHandle b)
{
	return a.index == b.index && a.generation == b.generation;
}

// Bit tricks go through the compiler: <stdbit.h> is missing from macOS libc.
static inline bool is_pow2(ptrdiff_t x) { return x > 0 && (x & (x - 1)) == 0; }
