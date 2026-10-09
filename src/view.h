// The player's camera and mouse picking, shared by the local game and the
// network client.
#pragma once

#include "input.h"
#include "keymap.h"
#include "render.h"
#include "terrain.h"
#include "vmath.h"

typedef struct {
	Camera3D camera;
	V2 focus;
	float distance;
} View;

// Pan (WASD, arrows, middle-drag) and zoom (wheel), then rebuild the camera.
void view_update(View *v, const Terrain *t, const Keymap *keys, const Input *in);

// Where the mouse ray meets the terrain. False if it doesn't.
bool pick_ground(const View *v, const Terrain *t, Vector2 mouse, V2 *out);
