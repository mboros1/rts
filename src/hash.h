// State hashing. Hashes cover fields in a declared order, never raw struct
// bytes, so padding and layout cannot change a hash.
#pragma once

#include "base.h"

typedef struct {
	uint64_t v;
} Hash;

static inline Hash hash_begin(void) { return (Hash){0xcbf29ce484222325u}; }

// Word-at-a-time FNV-style mix: the sim hashes every field every tick, so a
// byte loop would cost a millisecond at 10k units.
static inline Hash hash_u64(Hash h, uint64_t x)
{
	h.v = (h.v ^ x) * 0x100000001b3u;
	h.v ^= h.v >> 29;
	return h;
}

static inline Hash hash_u32(Hash h, uint32_t x) { return hash_u64(h, x); }

// Floats hash by bit pattern: two engines agree only if the bits agree.
static inline Hash hash_f32(Hash h, float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof bits);
	return hash_u64(h, bits);
}
