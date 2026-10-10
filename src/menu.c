#include "menu.h"

SelectionSummary menu_summarize(const Units *u, const UnitHandle *selection, ptrdiff_t count)
{
	SelectionSummary s = {0};
	for (ptrdiff_t k = 0; k < count; k++) {
		ptrdiff_t d = units_find(u, selection[k]);
		if (d < 0) continue;
		UnitType t = u->type[d];
		s.count[t]++;
		s.total++;
		bool done = u->progress[d] >= 1.0f;
		s.builders = s.builders || unit_is_builder(t);
		s.factories = s.factories || (unit_is_factory(t) && done);
		s.mobile = s.mobile || !unit_is_structure(t);
		s.digs_in = s.digs_in || unit_digs_in(t);
		s.dug_in = s.dug_in || u->seeded[d];
		s.warp_ready = s.warp_ready || (t == UNIT_COMMANDER && u->recharge[d] == 0 && u->order[d] != ORDER_WARP);
		if (unit_is_factory(t) && done) {
			s.repeating = s.repeating || u->repeat[d];
			for (int q = 0; q < u->queue_len[d]; q++) s.queued[u->queue[d] >> (8 * q) & 0xff]++;
		}
	}
	return s;
}

// What builders put up, by category, in grid order; combat has nothing yet
// (turrets come from entrenching). UNIT_TYPE_COUNT: an empty slot.
static const char *const build_tabs[BUILD_TABS] = {"Economy", "Combat", "Utility", "Build"};
static const UnitType build_items[BUILD_TABS][BUILD_ITEMS] = {
	{UNIT_EXTRACTOR, UNIT_GENERATOR, UNIT_ADVANCED_EXTRACTOR, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TRANSPORTER, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_FACTORY, UNIT_ADVANCED_FACTORY, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
};

// What factories make, by page.
static const char *const factory_tabs[BUILD_TABS] = {"Army", "Logistics", nullptr, nullptr};
static const UnitType factory_items[BUILD_TABS][BUILD_ITEMS] = {
	{UNIT_RAIDER, UNIT_TANK, UNIT_ARTILLERY, UNIT_SIEGE, UNIT_ENGINEER, UNIT_SAPPER, UNIT_SCOUT, UNIT_TYPE_COUNT},
	{UNIT_METAL_TRUCK, UNIT_ENERGY_TRUCK, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
};

// What an advanced factory makes.
static const char *const advanced_tabs[BUILD_TABS] = {"Army", nullptr, nullptr, nullptr};
static const UnitType advanced_items[BUILD_TABS][BUILD_ITEMS] = {
	{UNIT_HEAVY_TANK, UNIT_HOWITZER, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_ADVANCED_ENGINEER, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
	{UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT, UNIT_TYPE_COUNT},
};

BuildMenu menu_build(const SelectionSummary *s, int tab)
{
	BuildMenu m = {.builders = s->builders};
	if (!menu_build_open(s)) return m;
	// Factories: advanced ones' menu when that's all there is selected.
	bool advanced = !s->builders && s->count[UNIT_ADVANCED_FACTORY] > 0 && s->count[UNIT_FACTORY] == 0;
	const char *const *tabs = s->builders ? build_tabs : advanced ? advanced_tabs : factory_tabs;
	const UnitType(*items)[BUILD_ITEMS] = s->builders ? build_items : advanced ? advanced_items : factory_items;
	if (tab < 0 || tab >= BUILD_TABS || !tabs[tab]) tab = 0;
	for (int t = 0; t < BUILD_TABS; t++) {
		if (!tabs[t]) continue;
		bool any = false;
		for (int k = 0; k < BUILD_ITEMS; k++) any = any || items[t][k] != UNIT_TYPE_COUNT;
		m.tab[t] = (MenuButton){.shown = true, .enabled = any, .active = t == tab, .label = tabs[t]};
	}
	for (int k = 0; k < BUILD_ITEMS; k++) {
		UnitType type = items[tab][k];
		if (type == UNIT_TYPE_COUNT) continue;
		// A builder's item: enabled if some builder selected can build it.
		bool can = !s->builders;
		for (int b = 0; b < UNIT_TYPE_COUNT && !can; b++) can = s->count[b] > 0 && builder_builds((UnitType)b, type);
		m.item[k] = (MenuButton){.shown = true, .enabled = can, .type = type, .queued = s->queued[type]};
	}
	return m;
}

const char *menu_order_label(MenuOrder o, bool active)
{
	switch (o) {
	case MENU_ORDER_MOVE: return "Move";
	case MENU_ORDER_FIGHT: return "Fight";
	case MENU_ORDER_STOP: return "Stop";
	case MENU_ORDER_ENTRENCH: return active ? "Mobilize" : "Entrench";
	case MENU_ORDER_REPEAT: return active ? "Repeat on" : "Repeat off";
	case MENU_ORDER_CLEAR: return "Clear queue";
	case MENU_ORDER_SEND: return "Send";
	case MENU_ORDER_GIVE: return "Give";
	case MENU_ORDER_TAKE: return "Take";
	case MENU_ORDER_WARP: return "Warp home";
	case MENU_ORDER_NONE:
	case MENU_ORDER_COUNT: break;
	}
	return "";
}

// The orders grid, as BAR's: states first (repeat), then the common orders,
// then the team's.
OrdersMenu menu_orders(const SelectionSummary *s, const MenuContext *ctx)
{
	OrdersMenu m = {0};
	bool any = s->total > 0;
	struct {
		int slot;
		MenuOrder order;
		bool shown, enabled, active, toggle;
	} items[] = {
		{0, MENU_ORDER_REPEAT, s->factories, true, s->repeating, true},
		{1, MENU_ORDER_CLEAR, s->factories, true, false, false},
		{5, MENU_ORDER_STOP, any, true, false, false},
		{6, MENU_ORDER_MOVE, s->mobile, true, false, false},
		{7, MENU_ORDER_FIGHT, s->mobile, true, false, false},
		{8, MENU_ORDER_ENTRENCH, ctx->economy && (s->digs_in || s->dug_in), true, s->dug_in && !s->digs_in, false},
		{10, MENU_ORDER_SEND, s->mobile && ctx->world, true, false, false},
		{11, MENU_ORDER_GIVE, ctx->team && (any || ctx->economy), true, false, false},
		{12, MENU_ORDER_TAKE, ctx->team, ctx->takeable, false, false},
		{9, MENU_ORDER_WARP, s->count[UNIT_COMMANDER] > 0, s->warp_ready, false, false},
	};
	for (int k = 0; k < (int)countof(items); k++)
		if (items[k].shown)
			m.slot[items[k].slot] = (MenuButton){.shown = true, .enabled = items[k].enabled, .active = items[k].active,
			                                     .toggle = items[k].toggle, .order = items[k].order,
			                                     .label = menu_order_label(items[k].order, items[k].active)};
	return m;
}
