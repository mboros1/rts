#include "arena.h"

#include "platform.h"

#if defined(__SANITIZE_ADDRESS__)
#define ARENA_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ARENA_ASAN 1
#endif
#endif

#ifdef ARENA_ASAN
#include <sanitizer/asan_interface.h>
#define POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (size_t)(n))
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (size_t)(n))
#else
#define POISON(p, n) ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

// Freed bytes are filled with this so stale reads stand out in a debugger
// even without ASan.
enum { ARENA_DEAD_BYTE = 0xDD };

static constexpr ptrdiff_t ARENA_COMMIT_CHUNK = 1 << 16;

static ptrdiff_t round_up(ptrdiff_t x, ptrdiff_t to)
{
	return (x + to - 1) / to * to;
}

Arena arena_create(const char *name, ptrdiff_t reserve)
{
	ptrdiff_t page = platform_page_size();
	reserve = round_up(reserve, page);
	return (Arena){
		.name = name,
		.base = platform_reserve(reserve),
		.reserved = reserve,
	};
}

void arena_destroy(Arena *a)
{
	ASSERT(a->mark_depth == 0);
	platform_release(a->base, a->reserved);
	*a = (Arena){.name = a->name};
}

static bool grow_commit(Arena *a, ptrdiff_t need)
{
	if (need <= a->committed) return true;
	if (need > a->reserved) return false;
	ptrdiff_t chunk = round_up(ARENA_COMMIT_CHUNK, platform_page_size());
	ptrdiff_t target = round_up(need, chunk);
	if (target > a->reserved) target = a->reserved;
	platform_commit(a->base + a->committed, target - a->committed);
	POISON(a->base + a->committed, target - a->committed);
	a->committed = target;
	return true;
}

void *arena_try_push(Arena *a, ptrdiff_t size, ptrdiff_t align)
{
	ASSERT(size >= 0);
	ASSERT(is_pow2(align));
	ptrdiff_t start = round_up(a->used, align);
	ptrdiff_t end;
	if (ckd_add(&end, start, size)) return nullptr;
	if (!grow_commit(a, end)) return nullptr;
	uint8_t *p = a->base + start;
	UNPOISON(p, size);
	memset(p, 0, (size_t)size);
	a->used = end;
	if (end > a->high_water) a->high_water = end;
	a->allocs++;
	return p;
}

void *arena_push(Arena *a, ptrdiff_t size, ptrdiff_t align)
{
	void *p = arena_try_push(a, size, align);
	if (!p) {
		fatal("arena '%s' exhausted: asked %td bytes (align %td) with %td used, "
		      "%td reserved, high water %td, %lld allocations",
		      a->name, size, align, a->used, a->reserved, a->high_water,
		      (long long)a->allocs);
	}
	return p;
}

void *arena_push_array(Arena *a, ptrdiff_t size, ptrdiff_t align, ptrdiff_t count)
{
	ptrdiff_t total;
	ASSERT(count >= 0);
	if (ckd_mul(&total, size, count))
		fatal("arena '%s': array of %td x %td bytes overflows", a->name, count, size);
	return arena_push(a, total, align);
}

static void kill_range(Arena *a, ptrdiff_t from, ptrdiff_t to)
{
	if (to <= from) return;
	// Alignment padding between allocations was never unpoisoned.
	UNPOISON(a->base + from, to - from);
	memset(a->base + from, ARENA_DEAD_BYTE, (size_t)(to - from));
	POISON(a->base + from, to - from);
}

ArenaMark arena_mark(Arena *a)
{
	a->mark_depth++;
	return (ArenaMark){a, a->used, a->generation, a->mark_depth};
}

void arena_rewind(ArenaMark m)
{
	Arena *a = m.arena;
	ASSERT(m.generation == a->generation); // the arena was reset under the mark
	ASSERT(m.depth == a->mark_depth);      // marks rewound out of LIFO order
	ASSERT(m.used <= a->used);
	kill_range(a, m.used, a->used);
	a->used = m.used;
	a->mark_depth--;
}

void arena_reset(Arena *a)
{
	ASSERT(a->mark_depth == 0);
	kill_range(a, 0, a->used);
	a->used = 0;
	a->generation++;
}

void *arena_copy(Arena *a, const void *src, ptrdiff_t size, ptrdiff_t align)
{
	void *dst = arena_push(a, size, align);
	if (size) memcpy(dst, src, (size_t)size);
	return dst;
}
