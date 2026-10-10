#include "terrain.h"

#include "vmath.h"

static constexpr float TERRAIN_CELL = 4.0f;

// Lattice value in [0, 1) from integer coordinates: no float state involved.
static float lattice(uint64_t seed, int32_t i, int32_t j)
{
	uint64_t x = seed ^ ((uint64_t)(uint32_t)i * 0x9e3779b97f4a7c15u)
	                  ^ ((uint64_t)(uint32_t)j * 0xc2b2ae3d27d4eb4fu);
	x = (x ^ (x >> 31)) * 0xbf58476d1ce4e5b9u;
	x = (x ^ (x >> 29)) * 0x94d049bb133111ebu;
	x ^= x >> 32;
	return (float)(x >> 40) * (1.0f / 16777216.0f);
}

static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise: smoothed interpolation of lattice values `spacing` apart.
static float value_noise(uint64_t seed, float x, float z, float spacing)
{
	float fx = x / spacing, fz = z / spacing;
	int32_t i = (int32_t)fx, j = (int32_t)fz;
	float tx = smooth(fx - (float)i), tz = smooth(fz - (float)j);
	float a = lattice(seed, i, j), b = lattice(seed, i + 1, j);
	float c = lattice(seed, i, j + 1), d = lattice(seed, i + 1, j + 1);
	float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
	return ab + (cd - ab) * tz;
}

void terrain_generate(Terrain *t, Arena *arena, uint64_t seed, float size)
{
	*t = (Terrain){
		.size_x = size,
		.size_z = size,
		.cell = TERRAIN_CELL,
		.cells_x = (int32_t)(size / TERRAIN_CELL),
		.cells_z = (int32_t)(size / TERRAIN_CELL),
	};
	int32_t sx = t->cells_x + 1, sz = t->cells_z + 1;
	t->heights = PUSH(arena, float, (ptrdiff_t)sx * sz);
	for (int32_t j = 0; j < sz; j++) {
		for (int32_t i = 0; i < sx; i++) {
			float x = (float)i * t->cell, z = (float)j * t->cell;
			float h = 14.0f * value_noise(seed, x, z, 96.0f)
			        + 4.0f * value_noise(seed + 1, x, z, 24.0f);
			t->heights[j * sx + i] = h;
			t->max_height = f_max(t->max_height, h);
		}
	}
}

float terrain_height(const Terrain *t, float x, float z)
{
	float fx = f_clamp(x, 0.0f, t->size_x) / t->cell;
	float fz = f_clamp(z, 0.0f, t->size_z) / t->cell;
	int32_t i = (int32_t)fx, j = (int32_t)fz;
	if (i >= t->cells_x) i = t->cells_x - 1;
	if (j >= t->cells_z) j = t->cells_z - 1;
	float tx = fx - (float)i, tz = fz - (float)j;
	int32_t sx = t->cells_x + 1;
	const float *row0 = t->heights + (ptrdiff_t)j * sx;
	const float *row1 = row0 + sx;
	float ab = row0[i] + (row0[i + 1] - row0[i]) * tx;
	float cd = row1[i] + (row1[i + 1] - row1[i]) * tx;
	return ab + (cd - ab) * tz;
}
