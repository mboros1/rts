// Drawing with raylib. Reads sim state; never writes it.
//
// Units and shells are drawn instanced: one draw call per (faction, unit
// type, selected) bucket and one for all shells, with transforms built in
// the frame arena each frame.
#pragma once

// raylib.h defines its allocator macros in terms of malloc, which base.h
// poisons. Defining them first keeps raylib's out; our code never uses them.
#define RL_MALLOC(sz) RL_MALLOC_is_banned_use_an_arena
#define RL_CALLOC(n, sz) RL_CALLOC_is_banned_use_an_arena
#define RL_REALLOC(p, sz) RL_REALLOC_is_banned_use_an_arena
#define RL_FREE(p) RL_FREE_is_banned_use_an_arena

#include <raylib.h>

#include "arena.h"
#include "scene.h"
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
	// The terrain's lit colours, kept so fog can dim a copy and restore.
	unsigned char *terrain_colors;
	unsigned char *fogged_colors;
	ptrdiff_t terrain_vertices;
} Renderer;

// The RTS camera: looking down at focus from a fixed pitch, distance away.
// Shared by the game, the clip director and recorded camera tracks.
Camera3D render_camera(const Terrain *t, V2 focus, float distance);

void renderer_init(Renderer *r, const Terrain *terrain, ptrdiff_t max_units, Arena *program);
void renderer_unload(Renderer *r);

// Dim the terrain where visible[vertex] is 0 (one byte per heightfield
// sample, as remote_fog makes). nullptr clears the fog.
void renderer_set_fog(Renderer *r, const uint8_t *visible);

// alpha in [0, 1): how far between the last tick and the next to draw,
// extrapolated along velocity so 60 fps doesn't step at 30 Hz.
// selected marks the units of selected_faction by their index in the scene
// (may be nullptr).
void renderer_draw_scene(Renderer *r, const Scene *scene, float alpha, Arena *frame,
                         const uint8_t *selected, int selected_faction);
