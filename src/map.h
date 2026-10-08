// The strategic map: every zone of the world, the links between them, who
// holds what, and units in transit. Drawn from the coordinator's directory
// (world.h), by the client and by clips alike. raylib, 2D.
#pragma once

#include "base.h"
#include "world.h"

// Draws into the current target, w x h pixels; sizes scale with h.
// viewer: the faction looking (-1: nobody in particular).
// hover: the zone under the mouse, or -1.
void map_draw(const Directory *d, int viewer, int hover, int w, int h);

// The zone at pixel (x, y) on a map drawn at w x h, or -1.
int map_pick(const Directory *d, float x, float y, int w, int h);
