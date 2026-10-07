// Seeded randomness that lives inside the state it affects.
// The sim never calls rand(); each stream is a field of hashed state.
#pragma once

#include "base.h"

typedef struct {
	uint64_t state;
} Rng;

static inline Rng rng_seed(uint64_t seed) { return (Rng){seed ^ 0x9e3779b97f4a7c15u}; }

// splitmix64
static inline uint64_t rng_next(Rng *r)
{
	uint64_t z = (r->state += 0x9e3779b97f4a7c15u);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
	return z ^ (z >> 31);
}

// Uniform in [0, n).
static inline uint32_t rng_below(Rng *r, uint32_t n)
{
	return (uint32_t)(((rng_next(r) >> 32) * (uint64_t)n) >> 32);
}

// Uniform float in [lo, hi), built from integer bits so it is exact everywhere.
static inline float rng_range(Rng *r, float lo, float hi)
{
	float unit = (float)(rng_next(r) >> 40) * (1.0f / 16777216.0f);
	return lo + (hi - lo) * unit;
}
