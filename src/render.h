// Drawing with raylib. Reads sim state; never writes it.
//
// Units and shells are drawn instanced: one draw call per (faction, unit
// type, selected) bucket and one for all shells, with transforms built in
// the frame arena each frame.
#pragma once

#include <raylib.h>

#include "arena.h"
#include "sim.h"

typedef struct {
	Shader shader;          // instanced, with a fixed directional light
	Material material;
	Mesh unit_mesh[UNIT_TYPE_COUNT];
	Mesh shell_mesh;
	Model terrain;
	// Render-only state, indexed by unit slot: the heading a unit last moved
	// in, so stopped units keep facing the way they were going.
	float *heading[FACTION_COUNT];
	ptrdiff_t heading_cap;
} Renderer;

void renderer_init(Renderer *r, const Sim *sim, Arena *program);
void renderer_unload(Renderer *r);

// alpha in [0, 1): how far between the last tick and the next to draw,
// extrapolated along velocity so 60 fps doesn't step at 30 Hz.
// selected marks faction 0 units by dense index (may be nullptr).
void renderer_draw_world(Renderer *r, const Sim *sim, float alpha, Arena *frame,
                         const uint8_t *selected);
