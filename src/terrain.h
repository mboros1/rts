// A heightfield over the map. Generated from the seed, then immutable.
//
// Invariants: heights has (cells_x + 1) * (cells_z + 1) samples, row-major
// by z; sample (i, j) sits at (i * cell, j * cell).
#pragma once

#include "arena.h"
#include "base.h"

typedef struct {
	float size_x, size_z;   // world extent
	float cell;             // spacing between samples
	int32_t cells_x, cells_z;
	float *heights;
	float max_height;
} Terrain;

void terrain_generate(Terrain *t, Arena *arena, uint64_t seed);

// Bilinear height at a world position, clamped to the map.
float terrain_height(const Terrain *t, float x, float z);
