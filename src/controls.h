// The player's controls: selecting your units and ordering them, building
// and producing, and the HUD that goes with it. The one set the game has:
// the network client and the dev sandbox both use it, so a control added
// here works in the game, not in a side copy.
//
//   left-drag / click   select (shift adds)      right-click  attack-move
//   ctrl+right-click    move                     X            stop
//   engineers: E / G / F / R then click to place an extractor, generator,
//   factory or transporter (shift keeps placing; right-click or esc cancels)
//   factories: 1-4 queue raider, tank, artillery, engineer (shift: repeat),
//   0 clears
//   T then a zone's number: send the selection there through our nearest
//   transporter. Where we have no transporter (a neighbouring zone, only
//   raiders and engineers), click where in our strip they should land.
#pragma once

#include <raylib.h>

#include "arena.h"
#include "sim.h"
#include "view.h"

// One order from the player, before it's a Command: the client sends it,
// the sandbox queues it. units points into the frame arena.
typedef struct {
	CommandKind kind;
	V2 target;
	UnitType type;
	int32_t amount;
	uint8_t flags;
	const UnitHandle *units;
	ptrdiff_t count;
} Order;

// The zones a send can go to, as the client knows them (world.h's
// directory). count 0: no world (the sandbox), no sends.
typedef struct {
	int count;
	int here;
	const char *name[16];
	bool transporter[16];   // ours, there: a paired send
	bool neighbour[16];     // linked to here: an unpaired send can go
} SendTargets;

typedef struct {
	FactionId me;
	UnitHandle *selection;  // may hold stale handles until controls_selected
	ptrdiff_t selection_count;
	bool dragging;
	Vector2 drag_start;
	int placing;            // a structure type being placed, or -1
	bool sending;           // T pressed: waiting for a zone's number
	int landing_for;        // an unpaired send's zone, waiting for a click; -1
	SendTargets targets;    // set by the client each frame
} Controls;

void controls_init(Controls *c, FactionId me, Arena *a, ptrdiff_t max_units);
void controls_clear(Controls *c);

// This frame's mouse and keys, against sim's view of our own faction.
// Orders land in out (at most cap); returns how many.
ptrdiff_t controls_input(Controls *c, const Sim *sim, const View *view, Arena *frame, Order *out, ptrdiff_t cap);

// Drops dead units from the selection; marks the rest by unit index.
uint8_t *controls_selected(Controls *c, const Units *own, Arena *frame);

// Inside BeginMode3D: the structure being placed, where it would go.
void controls_draw_world(const Controls *c, const Sim *sim, const View *view);
// After EndMode3D: the drag box and the HUD (economy, selection, keys).
void controls_draw_screen(const Controls *c, const Sim *sim);
