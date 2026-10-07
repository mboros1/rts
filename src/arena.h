// Regions: the only allocator. See docs/conventions.md, "Memory".
//
// Three layers, kept apart:
//   backing memory  — a virtual range reserved up front, committed as used;
//   region lifetime — create, reset, destroy, with a generation per reset;
//   allocation      — a plain bump.
//
// Invariants of Arena:
//   0 <= used <= committed <= reserved, and used <= high_water.
//   Every byte in [0, used) belongs to a live allocation and was zeroed
//   when pushed. In builds with ASan, [used, committed) is poisoned.
//   mark_depth counts marks taken and not yet rewound; reset requires 0.
#pragma once

#include "base.h"

typedef struct Arena {
	const char *name;
	uint8_t *base;
	ptrdiff_t reserved;
	ptrdiff_t committed;
	ptrdiff_t used;
	ptrdiff_t high_water;
	int64_t allocs;
	uint32_t generation;
	int32_t mark_depth;
} Arena;

// A point to rewind scratch work to. Valid only for the arena's current
// generation, and only in LIFO order with other marks on the same arena.
typedef struct {
	Arena *arena;
	ptrdiff_t used;
	uint32_t generation;
	int32_t depth;
} ArenaMark;

Arena arena_create(const char *name, ptrdiff_t reserve);
void arena_destroy(Arena *a);

// Zeroed, aligned memory. Running out is a bug: aborts naming the arena,
// the request and the high-water mark.
void *arena_push(Arena *a, ptrdiff_t size, ptrdiff_t align);

// As arena_push, but returns nullptr instead of aborting. Only for untrusted
// input, where running out means rejecting it.
void *arena_try_push(Arena *a, ptrdiff_t size, ptrdiff_t align);

void *arena_push_array(Arena *a, ptrdiff_t size, ptrdiff_t align, ptrdiff_t count);

#define PUSH(a, T, n) ((T *)arena_push_array((a), (ptrdiff_t)sizeof(T), (ptrdiff_t)alignof(T), (n)))

ArenaMark arena_mark(Arena *a);
void arena_rewind(ArenaMark m);

// Ends every allocation in the arena: poisons it and bumps the generation.
void arena_reset(Arena *a);

// Copy bytes into the arena: the one place a lifetime changes.
void *arena_copy(Arena *a, const void *src, ptrdiff_t size, ptrdiff_t align);
