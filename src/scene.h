// What gets drawn: units and shells as flat arrays, from wherever they came.
//
// The renderer and the clip director read a Scene, never a Sim, so the same
// drawing serves a local sim, a network client's mirror, and recordings of
// either. Arrays are borrowed: valid until the source changes.
#pragma once

#include "arena.h"
#include "sim.h"
#include "terrain.h"
#include "wire.h"

typedef struct {
	ptrdiff_t count;
	const float *px, *py, *pz, *vx, *vz;
	const UnitType *type;
	const int32_t *slot;        // stable per unit: keys render-only state
} SceneUnits;

typedef struct {
	ptrdiff_t count;
	const float *px, *py, *pz, *vx, *vy, *vz;
	const UnitType *weapon;
} SceneShells;

typedef struct {
	const Terrain *terrain;
	int64_t tick;
	V2 base[FACTION_COUNT];
	ptrdiff_t max_units;        // slot capacity per faction
	SceneUnits units[FACTION_COUNT];
	SceneShells shells[FACTION_COUNT];
} Scene;

// Borrows the sim's arrays; no copying.
Scene scene_from_sim(const Sim *sim);

// Dead-reckons the mirror's units to its tick into dense arrays from arena.
Scene scene_from_mirror(const Mirror *m, const Terrain *t, const V2 base[FACTION_COUNT], Arena *arena);
