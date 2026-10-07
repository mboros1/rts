// A fixed-capacity map from 32-bit ids to dense indices.
//
// Open addressing with linear probing; removal shifts later entries back, so
// there are no tombstones and lookups stay short. Keys are stored as id + 1,
// leaving 0 for empty, so UINT32_MAX can't be stored.
//
// Invariants: cap is a power of two, count < cap. Every stored key sits at or
// after its home slot with no empty slot between.
#pragma once

#include "arena.h"
#include "base.h"

typedef struct {
	uint32_t *keys;
	int32_t *vals;
	ptrdiff_t cap;
	ptrdiff_t count;
} IdMap;

// Room for at least `entries` while staying at most half full.
static inline IdMap idmap_make(Arena *a, ptrdiff_t entries)
{
	ptrdiff_t cap = 16;
	while (cap < entries * 2) cap *= 2;
	return (IdMap){PUSH(a, uint32_t, cap), PUSH(a, int32_t, cap), cap, 0};
}

static inline ptrdiff_t idmap_home(const IdMap *m, uint32_t id)
{
	return (ptrdiff_t)((id * 0x9e3779b1u) & (uint32_t)(m->cap - 1));
}

static inline int32_t idmap_get(const IdMap *m, uint32_t id)
{
	for (ptrdiff_t i = idmap_home(m, id);; i = (i + 1) & (m->cap - 1)) {
		if (m->keys[i] == 0) return -1;
		if (m->keys[i] == id + 1) return m->vals[i];
	}
}

// Inserts or overwrites. The caller keeps count below cap / 2.
static inline void idmap_put(IdMap *m, uint32_t id, int32_t val)
{
	ASSERT(id != UINT32_MAX && m->count < m->cap - 1);
	for (ptrdiff_t i = idmap_home(m, id);; i = (i + 1) & (m->cap - 1)) {
		if (m->keys[i] == id + 1) {
			m->vals[i] = val;
			return;
		}
		if (m->keys[i] == 0) {
			m->keys[i] = id + 1;
			m->vals[i] = val;
			m->count++;
			return;
		}
	}
}

static inline void idmap_del(IdMap *m, uint32_t id)
{
	ptrdiff_t mask = m->cap - 1, i = idmap_home(m, id);
	for (;; i = (i + 1) & mask) {
		if (m->keys[i] == 0) return;
		if (m->keys[i] == id + 1) break;
	}
	// Shift back any entry whose home is at or before the hole.
	for (ptrdiff_t j = (i + 1) & mask; m->keys[j]; j = (j + 1) & mask) {
		ptrdiff_t home = idmap_home(m, m->keys[j] - 1);
		bool movable = i <= j ? (home <= i || home > j) : (home <= i && home > j);
		if (movable) {
			m->keys[i] = m->keys[j];
			m->vals[i] = m->vals[j];
			i = j;
		}
	}
	m->keys[i] = 0;
	m->count--;
}

static inline void idmap_clear(IdMap *m)
{
	memset(m->keys, 0, (size_t)m->cap * sizeof *m->keys);
	m->count = 0;
}
