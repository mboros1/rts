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
	// The terrain, in chunks of up to TERRAIN_CHUNK cells a side (a mesh's
	// 16-bit indices reach 65535 vertices): a 2048 zone is 4 x 4 of them.
	Model terrain[16];
	int32_t terrain_chunks;
	int32_t chunk_i[16], chunk_j[16], chunk_w[16], chunk_h[16]; // first cell and size, in cells
	int32_t samples_x;      // the heightfield's samples a row (cells_x + 1)
	// Render-only state, indexed by unit slot: the heading a unit last moved
	// in, so stopped units keep facing the way they were going.
	float *heading[FACTION_COUNT];
	ptrdiff_t heading_cap;
	// The terrain's lit colours, one per height sample, kept so fog can dim a
	// copy and restore; and a chunk's worth of colours to upload from.
	unsigned char *terrain_colors;
	unsigned char *fogged_colors;
	ptrdiff_t terrain_vertices;   // height samples
	unsigned char *chunk_colors;
} Renderer;

// The RTS camera: looking down at focus from a fixed pitch, distance away.
// Shared by the game, the clip director and recorded camera tracks.
Camera3D render_camera(const Terrain *t, V2 focus, float distance);

// A player's colour: a shade of their side's (player 0's is the side's own).
Color render_player_color(int faction, PlayerSlot p);

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
