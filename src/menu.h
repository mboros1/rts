// The command menus: what a selection can do, laid out as BAR's are. Pure:
// no raylib, no input; controls.c draws them, binds their keys and clicks,
// and turns a press into an order.
//
//   build grid (bottom left, above the info pane): two rows of items on
//   QWER and ASDF, and a row of tabs on ZXCV
//     builders:  the tabs are categories (economy, combat, utility, build);
//                the items, the open category's structures
//     factories: the tabs are pages (army, logistics); the items, the units
//                on the open page
//   orders grid (beside them): five wide, each order on its own key, which
//   works while the build grid isn't open (no builders or factories
//   selected); every button is a click too
#pragma once

#include "sim.h"

enum { BUILD_ITEMS = 8, BUILD_TABS = 4, ORDER_COLS = 5, ORDER_ROWS = 3, ORDER_SLOTS = ORDER_COLS * ORDER_ROWS };

// The build grid's keys: items, row by row from the top left; then tabs.
static const char BUILD_ITEM_KEYS[BUILD_ITEMS + 1] = "QWERASDF";
static const char BUILD_TAB_KEYS[BUILD_TABS + 1] = "ZXCV";

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
	MENU_ORDER_COUNT,
} MenuOrder;

// Each order's key ('\0': click only).
static const char MENU_ORDER_KEYS[MENU_ORDER_COUNT] = {
	[MENU_ORDER_MOVE] = 'M', [MENU_ORDER_FIGHT] = 'F', [MENU_ORDER_STOP] = 'S', [MENU_ORDER_ENTRENCH] = 'B',
	[MENU_ORDER_REPEAT] = 'Y', [MENU_ORDER_SEND] = 'T', [MENU_ORDER_GIVE] = 'H', [MENU_ORDER_TAKE] = 'K',
};

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

// The build grid is open (and has the grid letters) for builders or factories.
static inline bool menu_build_open(const SelectionSummary *s) { return s->builders || s->factories; }

// The build grid's slot for a key: items 0..7, tabs 8..11; -1 if none.
static inline int menu_build_slot_of_key(char key)
{
	for (int k = 0; k < BUILD_ITEMS; k++)
		if (BUILD_ITEM_KEYS[k] == key) return k;
	for (int k = 0; k < BUILD_TABS; k++)
		if (BUILD_TAB_KEYS[k] == key) return BUILD_ITEMS + k;
	return -1;
}
