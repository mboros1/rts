// The player's controls: selecting your units and ordering them, building
// and producing, and the panels that go with it, laid out as BAR's are. The
// one set the game has: the network client and the dev sandbox both use it,
// so a control added here works in the game, not in a side copy.
//
//   top centre      metal and energy: what you have, your storage, income and spending
//   bottom left     the build grid (menu.h): two rows of items and a row of
//                   tabs (QWER ASDF, ZXCV): builders' structures by
//                   category, factories' units by page (shift: five)
//   below it        the info pane: a tile per type selected (click: only
//                   those; shift-click: not those), or one unit's details
//   beside them     the orders grid: repeat, clear; stop, move, fight,
//                   entrench; send, give, take; each on its own key, always
//                   (they never share one with the build grid), every button a click
//
// Every key is the keymap's (keymap.h, config/keys.json); the defaults are
// named here and drawn on the buttons.
//   bottom right    your side's players
//   top left, and right of the orders: the minimap, control groups and idle
//                   builders, under construction
//
// Then:
//   left-drag / click   select your units (shift adds)
//   right-click         fight (attack-move) there; ctrl+right-click move
//   move, fight         then click where
//   placing             click to put it down (shift keeps placing); right-click or esc cancels
//   send                a zone's number; with teammates, then a player's (Enter: yourself);
//                       where we have no transporter, click where in our strip to land
//   give                a player's number: the selection (only factories: their
//                       output; shift: the factories themselves), or with nothing
//                       selected, a quarter of your metal and energy
//   take                every unit here of teammates who are away (BAR's /take)
//
// You select and order only your own units; teammates' are drawn in their
// own shades of the side's colour.
#pragma once

#include <raylib.h>

#include "arena.h"
#include "input.h"
#include "keymap.h"
#include "menu.h"
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
	int tab[2];             // the build grid's open category (builders: 0) and page (factories: 1)
	int aiming;             // move or fight, waiting for a click: its CommandKind, or -1
	bool sending;           // send: waiting for a zone's number
	int landing_for;        // an unpaired send's zone, waiting for a click; -1
	int send_for;           // a paired send's zone, waiting for who it's for; -1
	int giving;             // give: 1 the selection, 2 output, 3 factories too, 4 resources; 0 not
	SendTargets targets;    // set by the client each frame
	const Keymap *keys;     // nullptr: the defaults
} Controls;

// You play player 0 of `me` with nobody else on the side until told otherwise
// (player, team, team_count).
void controls_init(Controls *c, FactionId me, Arena *a, ptrdiff_t max_units);
void controls_clear(Controls *c);

// This frame's mouse and keys (input.h), against sim's view of our own faction.
// Orders land in out (at most cap); returns how many.
ptrdiff_t controls_input(Controls *c, const Input *in, const Sim *sim, const View *view, Arena *frame, Order *out,
                         ptrdiff_t cap);

// A placement, a move or fight, or a prompt in progress (cancel cancels it
// rather than leaving for the map).
bool controls_busy(const Controls *c);

// Drops dead units from the selection; marks the rest by unit index.
uint8_t *controls_selected(Controls *c, const Units *own, Arena *frame);

// Inside BeginMode3D: the structure being placed, where it would go.
void controls_draw_world(const Controls *c, const Sim *sim, const View *view, Vector2 mouse);
// After EndMode3D: the panels, in a screen of w x h pixels: the game's
// window (with its input: the mouse hovers and drags), or a clip's frame
// (in: nullptr).
void controls_draw_screen(const Controls *c, const Sim *sim, int w, int h, const Input *in);
