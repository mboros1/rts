// The command menus: what a selection can do, laid out as BAR's are. Pure:
// no raylib, no input; controls.c draws them, binds their keys and clicks,
// and turns a press into an order.
//
//   build grid (bottom left, above the info pane): two rows of items and a
//   row of tabs (on QWER, ASDF and ZXCV by default: keymap.h)
//     builders:  the tabs are categories (economy, combat, utility, build);
//                the items, the open category's structures
//     factories: the tabs are pages (army, logistics); the items, the units
//                on the open page
//   orders grid (beside them): five wide, each order on its own key
//   (keymap.h); every button is a click too
#pragma once

#include "sim.h"

enum { BUILD_ITEMS = 8, BUILD_TABS = 4, ORDER_COLS = 5, ORDER_ROWS = 3, ORDER_SLOTS = ORDER_COLS * ORDER_ROWS };

typedef enum : uint8_t {
	MENU_ORDER_NONE,
	MENU_ORDER_MOVE,
	MENU_ORDER_FIGHT,       // attack-move
	MENU_ORDER_STOP,
	MENU_ORDER_ENTRENCH,    // or mobilize, for what's dug in
	MENU_ORDER_REPEAT,      // factories: finished items requeue (a toggle)
	MENU_ORDER_CLEAR,       // factories: empty the queue
	MENU_ORDER_SEND,        // through a transporter, to a zone
	MENU_ORDER_GIVE,        // the selection to a teammate; with nothing selected, resources
	MENU_ORDER_TAKE,        // away teammates' units here
	MENU_ORDER_WARP,        // commanders: warp home
	MENU_ORDER_COUNT,
} MenuOrder;

typedef struct {
	bool shown;             // empty slots draw as blanks
	bool enabled;           // shown, but can't be used now (an empty category, no one to take from)
	bool active;            // the open tab; repeat that's on; mobilize rather than entrench
	bool toggle;            // draws a state bar (on: active)
	UnitType type;          // build items: what it builds or makes
	MenuOrder order;        // orders
	int32_t queued;         // factory items: queued of this type, over the selected factories
	const char *label;      // tabs and orders
} MenuButton;

typedef struct {
	bool builders;          // else factories (or nothing: an empty grid)
	MenuButton item[BUILD_ITEMS];
	MenuButton tab[BUILD_TABS];
} BuildMenu;

typedef struct {
	MenuButton slot[ORDER_SLOTS];
} OrdersMenu;

// What's selected, as the menus need it.
typedef struct {
	int32_t count[UNIT_TYPE_COUNT];   // selected units by type
	int32_t total;
	bool builders, factories, mobile, digs_in, dug_in;
	bool repeating;                   // a selected factory repeats
	bool warp_ready;                  // a selected commander can warp now
	int32_t queued[UNIT_TYPE_COUNT];  // over the selected factories
} SelectionSummary;

// From the selection's handles (stale ones skipped) in faction `units`.
SelectionSummary menu_summarize(const Units *units, const UnitHandle *selection, ptrdiff_t count);

typedef struct {
	bool economy;     // there's building at all
	bool team;        // the side has teammates: give, take
	bool takeable;    // a teammate is away: take does something
	bool world;       // there are zones to send to
} MenuContext;

// `tab`: the open category (builders) or page (factories).
BuildMenu menu_build(const SelectionSummary *s, int tab);
OrdersMenu menu_orders(const SelectionSummary *s, const MenuContext *ctx);

// The build grid is open (and its keys live) for builders or factories.
static inline bool menu_build_open(const SelectionSummary *s) { return s->builders || s->factories; }
