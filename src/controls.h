// The player's controls: selecting your units and ordering them, building
// and producing, and the HUD that goes with it. The one set the game has:
// the network client and the dev sandbox both use it, so a control added
// here works in the game, not in a side copy.
//
//   left-drag / click   select (shift adds)      right-click  attack-move
//   ctrl+right-click    move                     X            stop
//   engineers: E / G / F / R then click to place an extractor, generator,
//   factory or transporter (shift keeps placing; right-click or esc cancels)
//   factories: 1-9 queue raider, tank, artillery, engineer, scout, sapper, siege,
//   metal truck, energy truck
//   (shift: repeat),
//   0 clears
//   V: selected combat units entrench as turrets, trucks as vaults;
//   selected entrenched turrets and vaults mobilize (both paid for as they
//   go, and helpless while they do)
//   T then a zone's number: send the selection there through our nearest
//   transporter. Where we have no transporter (a neighbouring zone, only
//   raiders and engineers), click where in our strip they should land.
//   With teammates, a player's number then says who they arrive for
//   (Enter: yourself).
//   H then a player's number: give the selection to that teammate. With only
//   factories selected, it points their output there instead (shift+H gives
//   the factories themselves). With nothing selected, it gives them a quarter
//   of your metal and energy (as much as they have room for).
//   K: take every unit here of teammates who are away (BAR's /take).
//
// You select and order only your own units; teammates' are drawn in their
// own shades of the side's colour.
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
	PlayerSlot to;          // give, output; send: who they arrive for
	float metal, energy;    // share: how much
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
	PlayerSlot player;      // who you are on your side
	// Your side's players, by slot: names ("" for a bot), team_count of them.
	const char *team[MAX_PLAYERS];
	int team_count;
	UnitHandle *selection;  // may hold stale handles until controls_selected
	ptrdiff_t selection_count;
	bool dragging;
	Vector2 drag_start;
	int placing;            // a structure type being placed, or -1
	bool sending;           // T pressed: waiting for a zone's number
	int landing_for;        // an unpaired send's zone, waiting for a click; -1
	int send_for;           // a paired send's zone, waiting for who it's for; -1
	int giving;             // H pressed: 1 give, 2 output, 3 give factories too, 4 resources; 0 not
	SendTargets targets;    // set by the client each frame
} Controls;

// You play player 0 of `me` with nobody else on the side until told otherwise
// (player, team, team_count).
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
