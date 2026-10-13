#include "controls.h"

#include <math.h>

#include "render.h"

// The keymap's codes are raylib's.
static_assert((int)KEYCODE_SPACE == (int)KEY_SPACE && (int)KEYCODE_A == (int)KEY_A && (int)KEYCODE_0 == (int)KEY_ZERO
              && (int)KEYCODE_ESCAPE == (int)KEY_ESCAPE && (int)KEYCODE_ENTER == (int)KEY_ENTER && (int)KEYCODE_TAB == (int)KEY_TAB
              && (int)KEYCODE_BACKSPACE == (int)KEY_BACKSPACE && (int)KEYCODE_RIGHT == (int)KEY_RIGHT
              && (int)KEYCODE_LEFT == (int)KEY_LEFT && (int)KEYCODE_DOWN == (int)KEY_DOWN && (int)KEYCODE_UP == (int)KEY_UP
              && (int)KEYCODE_F1 == (int)KEY_F1 && (int)KEYCODE_F1 + 11 == (int)KEY_F12,
              "keymap.h's key codes are raylib's");

// Each order's action in the keymap.
static const KeyBinding order_action[MENU_ORDER_COUNT] = {
	[MENU_ORDER_MOVE] = BIND_MOVE, [MENU_ORDER_FIGHT] = BIND_FIGHT, [MENU_ORDER_STOP] = BIND_STOP,
	[MENU_ORDER_ENTRENCH] = BIND_ENTRENCH, [MENU_ORDER_REPEAT] = BIND_REPEAT, [MENU_ORDER_CLEAR] = BIND_CLEAR_QUEUE,
	[MENU_ORDER_SEND] = BIND_SEND, [MENU_ORDER_GIVE] = BIND_GIVE, [MENU_ORDER_TAKE] = BIND_TAKE,
	[MENU_ORDER_WARP] = BIND_WARP_HOME, [MENU_ORDER_CANCEL] = BIND_CANCEL_BUILD,
};

static int key_of(const Controls *c, KeyBinding a) { return c->keys ? c->keys->key[a] : keymap_default().key[a]; }
static bool action_pressed(const Controls *c, const Input *in, KeyBinding a) { return in->pressed[key_of(c, a)]; }
static const char *action_key(const Controls *c, KeyBinding a) { return keymap_key_name(key_of(c, a)); }
static KeyBinding build_action(int slot) { return (KeyBinding)(slot < BUILD_ITEMS ? BIND_BUILD_1 + slot : BIND_TAB_1 + slot - BUILD_ITEMS); }

void controls_init(Controls *c, FactionId me, Arena *a, ptrdiff_t max_units)
{
	*c = (Controls){.me = me, .selection = PUSH(a, UnitHandle, max_units), .placing = -1, .aiming = -1,
	                .landing_for = -1, .send_for = -1, .team_count = 1};
}

void controls_clear(Controls *c)
{
	c->selection_count = 0;
	c->dragging = false;
	c->placing = -1;
	c->placing_drag = false;
	c->aiming = -1;
	c->sending = false;
	c->landing_for = -1;
	c->send_for = -1;
	c->giving = 0;
}

static const Units *own_units(const Controls *c, const Sim *sim) { return &sim->factions[c->me.v].units; }

// ---------------------------------------------------------------- layout

// The panels, in a 1600 x 900 design scaled to the screen: anchored to its
// bottom left, bottom right and top centre. Input and drawing share it, so a
// click lands on what was drawn.
enum { TILE_COLS = 4, TILE_ROWS = 4, TILES = TILE_COLS * TILE_ROWS };

typedef struct {
	float s;                           // design units to pixels
	Rectangle build, info, orders, groups, idle, players, minimap, metal, energy;
	Rectangle item[BUILD_ITEMS], tab[BUILD_TABS], order[ORDER_SLOTS], tile[TILES];
} Layout;

static Layout layout(int w, int h)
{
	Layout L = {.s = fminf((float)w / 1600.0f, (float)h / 900.0f)};
	float s = L.s, pad = 6.0f * s, gap = 3.0f * s;
	float bw = 104.0f * s, bh = 72.0f * s, th = 30.0f * s;   // a build item; a tab's height
	float left = 4.0f * s, bottom = (float)h - 4.0f * s;
	float grid_w = 4.0f * bw + 3.0f * gap + 2.0f * pad;
	L.info = (Rectangle){left, bottom - 210.0f * s, grid_w, 210.0f * s};
	L.build = (Rectangle){left, L.info.y - gap - (2.0f * bh + th + 2.0f * gap + 2.0f * pad), grid_w,
	                      2.0f * bh + th + 2.0f * gap + 2.0f * pad};
	for (int k = 0; k < BUILD_ITEMS; k++)
		L.item[k] = (Rectangle){L.build.x + pad + (float)(k % 4) * (bw + gap), L.build.y + pad + (float)(k / 4) * (bh + gap), bw, bh};
	for (int k = 0; k < BUILD_TABS; k++)
		L.tab[k] = (Rectangle){L.build.x + pad + (float)k * (bw + gap), L.build.y + pad + 2.0f * (bh + gap), bw, th};
	float ow = 88.0f * s, oh = 38.0f * s;
	L.orders = (Rectangle){L.info.x + L.info.width + gap, bottom - (3.0f * oh + 2.0f * gap + 2.0f * pad),
	                       5.0f * ow + 4.0f * gap + 2.0f * pad, 3.0f * oh + 2.0f * gap + 2.0f * pad};
	for (int k = 0; k < ORDER_SLOTS; k++)
		L.order[k] = (Rectangle){L.orders.x + pad + (float)(k % ORDER_COLS) * (ow + gap),
		                         L.orders.y + pad + (float)(k / ORDER_COLS) * (oh + gap), ow, oh};
	L.groups = (Rectangle){L.orders.x + L.orders.width + gap, L.orders.y, 90.0f * s, L.orders.height};
	L.idle = (Rectangle){L.groups.x + L.groups.width + gap, L.orders.y, 90.0f * s, L.orders.height};
	L.players = (Rectangle){(float)w - 4.0f * s - 300.0f * s, bottom - 150.0f * s, 300.0f * s, 150.0f * s};
	L.minimap = (Rectangle){4.0f * s, 4.0f * s, 240.0f * s, 240.0f * s};
	float rw = 300.0f * s;
	L.metal = (Rectangle){(float)w * 0.5f - rw - 8.0f * s, 6.0f * s, rw, 34.0f * s};
	L.energy = (Rectangle){(float)w * 0.5f + 8.0f * s, 6.0f * s, rw, 34.0f * s};
	float tw = (grid_w - 2.0f * pad - 3.0f * gap) / 4.0f, tile_h = (L.info.height - 2.0f * pad - 3.0f * gap) / 4.0f;
	for (int k = 0; k < TILES; k++)
		L.tile[k] = (Rectangle){L.info.x + pad + (float)(k % TILE_COLS) * (tw + gap),
		                        L.info.y + pad + (float)(k / TILE_COLS) * (tile_h + gap), tw, tile_h};
	return L;
}

static bool over_panels(const Layout *L, Vector2 p)
{
	const Rectangle *r[] = {&L->build, &L->info, &L->orders, &L->groups, &L->idle, &L->players, &L->minimap, &L->metal, &L->energy};
	for (int k = 0; k < (int)countof(r); k++)
		if (CheckCollisionPointRec(p, *r[k])) return true;
	return false;
}

// The unit types selected, in type order, a tile each (as many as fit).
static int tile_types(const SelectionSummary *s, UnitType out[TILES])
{
	int n = 0;
	for (int t = 0; t < UNIT_TYPE_COUNT && n < TILES; t++)
		if (s->count[t]) out[n++] = (UnitType)t;
	return n;
}

// ---------------------------------------------------------------- selection

static void select_units(Controls *c, const Sim *sim, const View *view, Rectangle box, bool add, bool click)
{
	const Units *u = own_units(c, sim);
	if (!add) c->selection_count = 0;
	ptrdiff_t best = -1;
	float best_score = 1.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!player_eq(u->owner[i], c->player)) continue; // teammates' units aren't yours to order
		Vector2 s = GetWorldToScreen((Vector3){u->px[i], u->py[i] + 0.5f, u->pz[i]}, view->camera);
		if (click) {
			// Within its size on screen (a factory is big), or 14 px for a small
			// one; the nearest, relative to that, wins.
			Vector2 edge = GetWorldToScreen((Vector3){u->px[i] + unit_defs[u->type[i]].radius, u->py[i] + 0.5f, u->pz[i]},
			                                view->camera);
			float r = fmaxf(14.0f, hypotf(edge.x - s.x, edge.y - s.y));
			float dx = s.x - box.x, dy = s.y - box.y;
			float score = (dx * dx + dy * dy) / (r * r);
			if (score < best_score) {
				best = i;
				best_score = score;
			}
		} else if (CheckCollisionPointRec(s, box)) {
			c->selection[c->selection_count++] = units_handle(u, i);
		}
	}
	if (click && best >= 0) c->selection[c->selection_count++] = units_handle(u, best);
}

// Your side's unfinished structure under the mouse that a selected builder
// can finish, or -1. A right-click on one resumes building it, as in BAR.
static ptrdiff_t unfinished_at(const Controls *c, const Sim *sim, const View *view, Vector2 mouse)
{
	const Units *u = own_units(c, sim);
	ptrdiff_t best = -1;
	float best_d2 = 14.0f * 14.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!unit_is_structure(u->type[i]) || u->progress[i] >= 1.0f) continue;
		Vector2 s = GetWorldToScreen((Vector3){u->px[i], u->py[i] + 0.5f, u->pz[i]}, view->camera);
		float dx = s.x - mouse.x, dy = s.y - mouse.y;
		if (dx * dx + dy * dy < best_d2) {
			best = i;
			best_d2 = dx * dx + dy * dy;
		}
	}
	if (best < 0) return -1;
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t d = units_find(u, c->selection[k]);
		if (d >= 0 && builder_builds(u->type[d], u->type[best])) return best;
	}
	return -1;
}

// Keep only the selected units of `type` (or, `drop`, all but them).
static void narrow_selection(Controls *c, const Sim *sim, UnitType type, bool drop)
{
	const Units *u = own_units(c, sim);
	ptrdiff_t kept = 0;
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t d = units_find(u, c->selection[k]);
		if (d >= 0 && (u->type[d] == type) != drop) c->selection[kept++] = c->selection[k];
	}
	c->selection_count = kept;
}

uint8_t *controls_selected(Controls *c, const Units *own, Arena *frame)
{
	uint8_t *marked = PUSH(frame, uint8_t, own->count > 0 ? own->count : 1);
	ptrdiff_t kept = 0;
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t i = units_find(own, c->selection[k]);
		if (i < 0 || marked[i] || !player_eq(own->owner[i], c->player)) continue; // gone, or given away
		marked[i] = 1;
		c->selection[kept++] = c->selection[k];
	}
	c->selection_count = kept;
	return marked;
}

// Where a structure would go for this click: extractors snap to the spot.
static constexpr float GRID_GAP = 2.0f; // between structures laid out by a drag

// A drag while placing: sites filling the rectangle from `from` to `to`,
// a footprint and a gap apart, rows from `from` outward; one site for a
// click, or for an extractor (they go on spots). At most `cap`.
static ptrdiff_t grid_sites(UnitType type, V2 from, V2 to, V2 *out, ptrdiff_t cap)
{
	float step = 2.0f * unit_defs[type].radius + GRID_GAP;
	float dx = to.x - from.x, dz = to.z - from.z;
	int nx = unit_is_extractor(type) ? 1 : 1 + (int)(fabsf(dx) / step);
	int nz = unit_is_extractor(type) ? 1 : 1 + (int)(fabsf(dz) / step);
	float sx = dx < 0.0f ? -step : step, sz = dz < 0.0f ? -step : step;
	ptrdiff_t n = 0;
	for (int j = 0; j < nz; j++)
		for (int i = 0; i < nx && n < cap; i++) out[n++] = v2(from.x + sx * (float)i, from.z + sz * (float)j);
	return n;
}

static V2 placement_site(const Sim *sim, UnitType type, V2 at)
{
	if (!unit_is_extractor(type)) return at;
	int32_t best = -1;
	float best_d2 = 15.0f * 15.0f;
	for (int32_t k = 0; k < sim->spots.count; k++) {
		float d2 = v2_len2(v2_sub(sim->spots.at[k], at));
		if (d2 < best_d2) {
			best = k;
			best_d2 = d2;
		}
	}
	return best >= 0 ? sim->spots.at[best] : at;
}

// ---------------------------------------------------------------- orders

// A teammate's number key this frame (1 is player 0), or -1.
static int player_key(const Controls *c, const Input *in)
{
	for (int k = 0; k < c->team_count; k++)
		if (in->pressed[KEY_ONE + k]) return k;
	return -1;
}

static void order(Controls *c, Arena *frame, Order *out, ptrdiff_t cap, ptrdiff_t *n, Order o)
{
	if (c->selection_count == 0 || *n == cap) return;
	if (o.kind != COMMAND_GIVE && o.kind != COMMAND_OUTPUT && o.kind != COMMAND_SEND) o.to = c->player;
	o.units = arena_copy(frame, c->selection, c->selection_count * (ptrdiff_t)sizeof(UnitHandle), alignof(UnitHandle));
	o.count = c->selection_count;
	out[(*n)++] = o;
}

static MenuContext menu_context(const Controls *c, const Sim *sim)
{
	uint8_t away = sim->factions[c->me.v].away;
	return (MenuContext){.economy = sim->config.economy, .team = c->team_count > 1,
	                     .takeable = !(away >> c->player.v & 1) && (away & ~(1u << c->player.v)) != 0,
	                     .world = c->targets.count > 0};
}

// A build grid button pressed (by key or click).
static void press_build(Controls *c, const SelectionSummary *s, const BuildMenu *m, int slot, bool shift, Arena *frame,
                        Order *out, ptrdiff_t cap, ptrdiff_t *n)
{
	if (slot >= BUILD_ITEMS) {
		const MenuButton *b = &m->tab[slot - BUILD_ITEMS];
		if (b->shown && b->enabled) c->tab[!m->builders] = slot - BUILD_ITEMS;
		return;
	}
	const MenuButton *b = &m->item[slot];
	if (!b->shown || !b->enabled) return;
	if (m->builders) {
		c->placing = b->type;
		return;
	}
	// Factories: one, or five with shift (the queue holds eight).
	order(c, frame, out, cap, n, (Order){.kind = COMMAND_PRODUCE, .type = b->type, .amount = shift ? 5 : 1,
	                                     .flags = s->repeating ? COMMAND_REPEAT : 0});
}

// An orders grid button pressed.
static void press_order(Controls *c, const Sim *sim, const SelectionSummary *s, const MenuButton *b, bool shift,
                        Arena *frame, Order *out, ptrdiff_t cap, ptrdiff_t *n)
{
	if (!b->shown || !b->enabled) return;
	switch (b->order) {
	case MENU_ORDER_MOVE: c->aiming = COMMAND_MOVE; break;
	case MENU_ORDER_FIGHT: c->aiming = COMMAND_ATTACK_MOVE; break;
	case MENU_ORDER_STOP: order(c, frame, out, cap, n, (Order){.kind = COMMAND_STOP}); break;
	case MENU_ORDER_ENTRENCH:
		// Both at once: the sim takes what digs in for one, what's dug in for the other.
		order(c, frame, out, cap, n, (Order){.kind = COMMAND_ENTRENCH});
		order(c, frame, out, cap, n, (Order){.kind = COMMAND_MOBILIZE});
		break;
	case MENU_ORDER_REPEAT:
		order(c, frame, out, cap, n, (Order){.kind = COMMAND_SET_REPEAT, .flags = s->repeating ? 0 : COMMAND_REPEAT});
		break;
	case MENU_ORDER_CLEAR:
		order(c, frame, out, cap, n, (Order){.kind = COMMAND_PRODUCE, .amount = 0, .flags = s->repeating ? COMMAND_REPEAT : 0});
		break;
	case MENU_ORDER_SEND:
		c->sending = true;
		c->landing_for = -1;
		break;
	case MENU_ORDER_GIVE:
		c->giving = s->total == 0 ? 4 : shift ? 3 : s->factories && s->count[UNIT_FACTORY] + s->count[UNIT_ADVANCED_FACTORY] == s->total ? 2 : 1;
		break;
	case MENU_ORDER_TAKE:
		if (*n < cap) out[(*n)++] = (Order){.kind = COMMAND_TAKE, .to = c->player};
		break;
	case MENU_ORDER_WARP: order(c, frame, out, cap, n, (Order){.kind = COMMAND_WARP}); break;
	case MENU_ORDER_CANCEL: order(c, frame, out, cap, n, (Order){.kind = COMMAND_CANCEL}); break;
	case MENU_ORDER_NONE:
	case MENU_ORDER_COUNT: break;
	}
	(void)sim;
}

// Send's prompts: a zone's number; then, paired with teammates, who for;
// unpaired, where to land. The server decides the route.
static bool send_input(Controls *c, const Input *in, const Sim *sim, const View *view, Arena *frame, Order *out,
                       ptrdiff_t cap, ptrdiff_t *n)
{
	const SendTargets *z = &c->targets;
	if (c->sending) {
		if (action_pressed(c, in, BIND_CANCEL) || in->click[1]) c->sending = false;
		for (int k = 0; k < z->count && k < 9; k++) {
			if (!in->pressed[KEY_ONE + k] || k == z->here) continue;
			c->sending = false;
			if (z->transporter[k] && c->team_count > 1) c->send_for = k; // then: for whom
			else if (z->transporter[k]) order(c, frame, out, cap, n, (Order){.kind = COMMAND_SEND, .amount = k, .to = c->player});
			else if (z->neighbour[k]) c->landing_for = k;
		}
		return true;
	}
	if (c->send_for >= 0) {
		int p = player_key(c, in);
		bool self = in->pressed[KEY_ENTER] || in->pressed[KEY_KP_ENTER] || action_pressed(c, in, BIND_SEND);
		if (action_pressed(c, in, BIND_CANCEL) || in->click[1]) c->send_for = -1;
		else if (p >= 0 || self) {
			PlayerSlot to = p >= 0 ? (PlayerSlot){(uint8_t)p} : c->player;
			order(c, frame, out, cap, n, (Order){.kind = COMMAND_SEND, .amount = c->send_for, .to = to});
			c->send_for = -1;
		}
		return true;
	}
	if (c->landing_for >= 0) {
		V2 at;
		if (action_pressed(c, in, BIND_CANCEL) || in->click[1]) c->landing_for = -1;
		else if (in->click[0] && pick_ground(view, &sim->terrain, in->mouse, &at)) {
			at = sim_strip_point(&sim->terrain, c->me, at);
			order(c, frame, out, cap, n, (Order){.kind = COMMAND_SEND, .amount = c->landing_for, .target = at, .to = c->player});
			c->landing_for = -1;
		}
		return true;
	}
	return false;
}

// Give's prompt: a player's number.
static bool give_input(Controls *c, const Input *in, const Sim *sim, Arena *frame, Order *out, ptrdiff_t cap, ptrdiff_t *n)
{
	if (!c->giving) return false;
	int p = player_key(c, in);
	if (action_pressed(c, in, BIND_CANCEL) || in->click[1]) {
		c->giving = 0;
	} else if (p >= 0 && c->giving == 4) {
		const Economy *e = &sim->factions[c->me.v].economy[c->player.v];
		if (*n < cap && p != c->player.v)
			out[(*n)++] = (Order){.kind = COMMAND_SHARE, .metal = 0.25f * e->metal, .energy = 0.25f * e->energy, .to = {(uint8_t)p}};
		c->giving = 0;
	} else if (p >= 0) {
		order(c, frame, out, cap, n, (Order){.kind = c->giving == 2 ? COMMAND_OUTPUT : COMMAND_GIVE, .to = {(uint8_t)p}});
		c->giving = 0;
	}
	return true;
}

bool controls_busy(const Controls *c)
{
	return c->placing >= 0 || c->aiming >= 0 || c->sending || c->landing_for >= 0 || c->send_for >= 0 || c->giving;
}

ptrdiff_t controls_input(Controls *c, const Input *in, const Sim *sim, const View *view, Arena *frame, Order *out,
                         ptrdiff_t cap)
{
	ptrdiff_t n = 0;
	Vector2 mouse = in->mouse;
	bool shift = input_shift(in);
	Layout L = layout(in->width, in->height);
	SelectionSummary s = menu_summarize(own_units(c, sim), c->selection, c->selection_count);
	MenuContext ctx = menu_context(c, sim);
	BuildMenu build = menu_build(&s, c->tab[!s.builders], (int)sim->config.context.tier);
	OrdersMenu orders = menu_orders(&s, &ctx);

	// A prompt in progress takes the keys and clicks it needs.
	if (send_input(c, in, sim, view, frame, out, cap, &n) || give_input(c, in, sim, frame, out, cap, &n)) return n;
	if (c->aiming >= 0) {
		V2 at;
		if (action_pressed(c, in, BIND_CANCEL) || in->click[1]) c->aiming = -1;
		else if (in->click[0] && !over_panels(&L, mouse)
		         && pick_ground(view, &sim->terrain, mouse, &at)) {
			order(c, frame, out, cap, &n, (Order){.kind = (CommandKind)c->aiming, .target = at, .flags = shift ? COMMAND_QUEUED : 0});
			if (!shift) c->aiming = -1;
		}
		return n;
	}
	if (c->placing >= 0) {
		// A click that places or cancels a structure is only that: not a
		// selection too (it used to start a drag that emptied the selection).
		V2 at;
		if (action_pressed(c, in, BIND_CANCEL) || in->click[1]) {
			c->placing = -1;
			c->placing_drag = false;
			return n;
		}
		UnitType type = (UnitType)c->placing;
		if (in->click[0] && !over_panels(&L, mouse) && pick_ground(view, &sim->terrain, mouse, &at)) {
			c->place_from = placement_site(sim, type, at);
			c->placing_drag = true;
			return n;
		}
		// Let go: the site, or the grid dragged out (BAR lays out a line;
		// a rectangle fills, and a thin one is a line). The first replaces
		// the builders' orders unless shift; the rest queue after it.
		if (c->placing_drag && in->release[0]) {
			c->placing_drag = false;
			V2 to = c->place_from;
			if (pick_ground(view, &sim->terrain, mouse, &at)) to = at;
			V2 sites[LATER_MAX];
			ptrdiff_t count = grid_sites(type, c->place_from, to, sites, countof(sites));
			for (ptrdiff_t k = 0; k < count; k++)
				if (placement_ok(sim, c->me, type, sites[k]))
					order(c, frame, out, cap, &n, (Order){.kind = COMMAND_BUILD, .target = sites[k], .type = type,
					                                     .flags = shift || n > 0 ? COMMAND_QUEUED : 0});
			if (!shift) c->placing = -1;
			return n;
		}
	}

	// Keys: the build grid's while it's open; the orders' always (they never share a key: keymap.h).
	if (menu_build_open(&s))
		for (int k = 0; k < BUILD_ITEMS + BUILD_TABS; k++)
			if (action_pressed(c, in, build_action(k))) press_build(c, &s, &build, k, shift, frame, out, cap, &n);
	for (int k = 0; k < ORDER_SLOTS; k++)
		if (orders.slot[k].shown && action_pressed(c, in, order_action[orders.slot[k].order]))
			press_order(c, sim, &s, &orders.slot[k], shift, frame, out, cap, &n);

	// Clicks on the panels: buttons, and the selection's tiles.
	if (in->click[0] && over_panels(&L, mouse)) {
		for (int k = 0; k < BUILD_ITEMS; k++)
			if (CheckCollisionPointRec(mouse, L.item[k])) press_build(c, &s, &build, k, shift, frame, out, cap, &n);
		for (int k = 0; k < BUILD_TABS; k++)
			if (CheckCollisionPointRec(mouse, L.tab[k])) press_build(c, &s, &build, BUILD_ITEMS + k, shift, frame, out, cap, &n);
		for (int k = 0; k < ORDER_SLOTS; k++)
			if (CheckCollisionPointRec(mouse, L.order[k])) press_order(c, sim, &s, &orders.slot[k], shift, frame, out, cap, &n);
		UnitType types[TILES];
		int tiles = s.total > 1 ? tile_types(&s, types) : 0;
		for (int k = 0; k < tiles; k++)
			if (CheckCollisionPointRec(mouse, L.tile[k])) narrow_selection(c, sim, types[k], shift);
		return n;
	}

	if (c->placing >= 0) return n; // the world's clicks are the placement's
	if (in->click[0]) {
		c->dragging = true;
		c->drag_start = mouse;
	}
	if (c->dragging && in->release[0]) {
		c->dragging = false;
		Rectangle box = {fminf(c->drag_start.x, mouse.x), fminf(c->drag_start.y, mouse.y),
		                 fabsf(mouse.x - c->drag_start.x), fabsf(mouse.y - c->drag_start.y)};
		bool click = box.width < 4.0f && box.height < 4.0f;
		if (click) box = (Rectangle){mouse.x, mouse.y, 0, 0};
		select_units(c, sim, view, box, shift, click);
	}
	V2 at;
	ptrdiff_t site = in->click[1] && !over_panels(&L, mouse) ? unfinished_at(c, sim, view, mouse) : -1;
	if (site >= 0) {
		const Units *u = own_units(c, sim);
		order(c, frame, out, cap, &n, (Order){.kind = COMMAND_BUILD, .target = v2(u->px[site], u->pz[site]), .type = u->type[site],
		                                     .flags = shift ? COMMAND_QUEUED : 0});
	} else if (in->click[1] && !over_panels(&L, mouse) && pick_ground(view, &sim->terrain, mouse, &at)) {
		bool move = input_ctrl(in);
		order(c, frame, out, cap, &n, (Order){.kind = move ? COMMAND_MOVE : COMMAND_ATTACK_MOVE, .target = at,
		                                     .flags = shift ? COMMAND_QUEUED : 0});
	}
	return n;
}

// ---------------------------------------------------------------- drawing

static Color order_color(OrderKind kind)
{
	return kind == ORDER_BUILD ? (Color){240, 210, 70, 230} : kind == ORDER_ATTACK_MOVE ? (Color){235, 80, 60, 230}
	                                                         : (Color){120, 255, 140, 230};
}

static void ghost(const Sim *sim, UnitType type, float x, float z, Color color)
{
	const UnitDef *def = &unit_defs[type];
	DrawCubeWires((Vector3){x, terrain_height(&sim->terrain, x, z) + def->height * 0.5f, z}, def->radius * 2.0f, def->height,
	              def->radius * 2.0f, color);
}

// What's planned: every site our builders are going to build (a ghost), the
// full size of what's being built (its outline), and for the units selected,
// the path through their orders, coloured by kind (move, fight, build).
static void draw_plans(const Controls *c, const Sim *sim)
{
	const Units *u = own_units(c, sim);
	const Terrain *t = &sim->terrain;
	Color planned = ColorAlpha(render_player_color((int)c->me.v, c->player), 0.75f), outline = {230, 230, 220, 160};
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (unit_is_structure(u->type[i]) && u->progress[i] < 1.0f) ghost(sim, u->type[i], u->px[i], u->pz[i], outline);
		if (!player_eq(u->owner[i], c->player) || !unit_is_builder(u->type[i])) continue;
		if (u->order[i] == ORDER_BUILD && units_find(u, u->building[i]) < 0) ghost(sim, u->build_type[i], u->tx[i], u->tz[i], planned);
		for (int32_t n = u->later_head[i]; n >= 0; n = u->later_next[n])
			if (u->later_kind[n] == ORDER_BUILD) ghost(sim, u->later_type[n], u->later_tx[n], u->later_tz[n], planned);
	}
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t i = units_find(u, c->selection[k]);
		if (i < 0 || unit_is_structure(u->type[i])) continue;
		bool going = u->order[i] == ORDER_MOVE || u->order[i] == ORDER_ATTACK_MOVE || u->order[i] == ORDER_BUILD;
		if (!going && u->later_len[i] == 0) continue;
		Vector3 from = {u->px[i], u->py[i] + 0.4f, u->pz[i]};
		if (going) {
			Vector3 to = {u->tx[i], terrain_height(t, u->tx[i], u->tz[i]) + 0.4f, u->tz[i]};
			DrawLine3D(from, to, order_color(u->order[i]));
			from = to;
		}
		for (int32_t n = u->later_head[i]; n >= 0; n = u->later_next[n]) {
			Vector3 to = {u->later_tx[n], terrain_height(t, u->later_tx[n], u->later_tz[n]) + 0.4f, u->later_tz[n]};
			DrawLine3D(from, to, order_color(u->later_kind[n]));
			DrawCircle3D(to, 1.0f, (Vector3){1, 0, 0}, 90.0f, order_color(u->later_kind[n]));
			from = to;
		}
	}
}

void controls_draw_world(const Controls *c, const Sim *sim, const View *view, Vector2 mouse)
{
	draw_plans(c, sim);
	if (c->landing_for >= 0) {
		// Where an unpaired send may land: our strip, its edge traced on the ground.
		const Terrain *t = &sim->terrain;
		float edge = c->me.v == 0 ? t->size_x * STRIP : t->size_x * (1.0f - STRIP);
		Color line = {120, 255, 140, 220};
		for (float z = 0.0f; z + 4.0f <= t->size_z; z += 4.0f)
			DrawLine3D((Vector3){edge, terrain_height(t, edge, z) + 0.5f, z},
			           (Vector3){edge, terrain_height(t, edge, z + 4.0f) + 0.5f, z + 4.0f}, line);
		V2 at;
		if (pick_ground(view, t, mouse, &at)) {
			at = sim_strip_point(t, c->me, at);
			DrawCircle3D((Vector3){at.x, terrain_height(t, at.x, at.z) + 0.6f, at.z}, 8.0f, (Vector3){1, 0, 0}, 90.0f, line);
		}
	}
	V2 ghost;
	if (c->placing < 0 || !pick_ground(view, &sim->terrain, mouse, &ghost)) return;
	UnitType type = (UnitType)c->placing;
	ghost = placement_site(sim, type, ghost);
	// Dragging: the grid it will lay out (red where a site can't go).
	V2 sites[LATER_MAX];
	ptrdiff_t count = c->placing_drag ? grid_sites(type, c->place_from, ghost, sites, countof(sites)) : 1;
	if (!c->placing_drag) sites[0] = ghost;
	const UnitDef *def = &unit_defs[type];
	for (ptrdiff_t k = 0; k < count; k++) {
		bool ok = placement_ok(sim, c->me, type, sites[k]);
		Vector3 at = {sites[k].x, terrain_height(&sim->terrain, sites[k].x, sites[k].z) + def->height * 0.5f, sites[k].z};
		DrawCubeWires(at, def->radius * 2.0f, def->height, def->radius * 2.0f, ok ? GREEN : RED);
	}
}

static const Color PANEL = {14, 16, 20, 215}, PANEL_EDGE = {60, 66, 78, 255}, BUTTON = {32, 36, 44, 255},
                   BUTTON_HOVER = {48, 54, 66, 255}, BUTTON_ACTIVE = {44, 70, 112, 255}, TEXT = {225, 228, 232, 255},
                   TEXT_DIM = {120, 126, 136, 255}, METAL = {200, 205, 215, 255}, ENERGY = {240, 210, 70, 255},
                   ON = {90, 200, 110, 255}, OFF = {210, 70, 60, 255};

static void panel(Rectangle r, float s)
{
	DrawRectangleRec(r, PANEL);
	DrawRectangleLinesEx(r, fmaxf(1.0f, s), PANEL_EDGE);
}

static void text(const char *t, float x, float y, float size, Color color) { DrawText(t, (int)x, (int)y, (int)size, color); }

// A box with its name and UNDER CONSTRUCTION: what has a place, but not yet.
static void under_construction(Rectangle r, const char *name, float s)
{
	panel(r, s);
	text(name, r.x + 8.0f * s, r.y + 6.0f * s, 14.0f * s, TEXT_DIM);
	// On one line if it fits, else two.
	Color amber = {230, 170, 60, 255};
	float size = 12.0f * s;
	const char *one = "UNDER CONSTRUCTION";
	if ((float)MeasureText(one, (int)size) <= r.width - 12.0f * s) {
		text(one, r.x + (r.width - (float)MeasureText(one, (int)size)) * 0.5f, r.y + r.height * 0.5f - size * 0.5f, size, amber);
		return;
	}
	const char *lines[2] = {"UNDER", "CONSTRUCTION"};
	while (size > 6.0f && (float)MeasureText(lines[1], (int)size) > r.width - 8.0f * s) size -= 1.0f;
	for (int k = 0; k < 2; k++)
		text(lines[k], r.x + (r.width - (float)MeasureText(lines[k], (int)size)) * 0.5f,
		     r.y + r.height * 0.5f - size * 1.1f + (float)k * size * 1.2f, size, amber);
}

static void button_frame(Rectangle r, const MenuButton *b, bool hover, float s)
{
	Color bg = !b->enabled ? (Color){24, 26, 30, 255} : b->active && !b->toggle ? BUTTON_ACTIVE : hover ? BUTTON_HOVER : BUTTON;
	DrawRectangleRec(r, bg);
	DrawRectangleLinesEx(r, fmaxf(1.0f, s), PANEL_EDGE);
}

static void draw_build_item(Rectangle r, const MenuButton *b, const ZoneContext *zc, const char *key, bool keys_live, bool hover,
                            float s)
{
	if (!b->shown) {
		DrawRectangleRec(r, (Color){22, 24, 28, 255});
		return;
	}
	button_frame(r, b, hover, s);
	const UnitDef *def = &unit_defs[b->type];
	text(def->name, r.x + 6.0f * s, r.y + 6.0f * s, 14.0f * s, TEXT);
	// Costs, as BAR shows them: metal over energy, bottom right.
	// The zone's: its cost multiplier.
	const char *m = TextFormat("%.0f", def->metal_cost * zc->cost), *e = TextFormat("%.0f", def->energy_cost * zc->cost);
	text(m, r.x + r.width - (float)MeasureText(m, (int)(14.0f * s)) - 6.0f * s, r.y + r.height - 36.0f * s, 14.0f * s, METAL);
	text(e, r.x + r.width - (float)MeasureText(e, (int)(14.0f * s)) - 6.0f * s, r.y + r.height - 19.0f * s, 14.0f * s, ENERGY);
	text(key, r.x + 6.0f * s, r.y + r.height - 19.0f * s, 14.0f * s, keys_live ? TEXT : TEXT_DIM);
	if (b->queued) text(TextFormat("%d", b->queued), r.x + r.width - 26.0f * s, r.y + 6.0f * s, 16.0f * s, ON);
}

static void draw_text_button(Rectangle r, const MenuButton *b, const char *key, bool keys_live, bool hover, float s)
{
	if (!b->shown) {
		DrawRectangleRec(r, (Color){22, 24, 28, 255});
		return;
	}
	button_frame(r, b, hover, s);
	// The label low in the button; its key small in the top right, clear of it.
	Color color = b->enabled ? TEXT : TEXT_DIM;
	float size = 14.0f * s, key_size = 10.0f * s;
	const char *label = b->label ? b->label : "";
	bool keyed = key && key[0];
	float label_y = keyed && r.height > 32.0f * s ? r.y + r.height - size - 7.0f * s : r.y + (r.height - size) * 0.5f;
	text(label, r.x + (r.width - (float)MeasureText(label, (int)size)) * 0.5f, label_y, size, color);
	if (keyed)
		text(key, r.x + r.width - (float)MeasureText(key, (int)key_size) - 4.0f * s, r.y + 3.0f * s, key_size,
		     keys_live ? ENERGY : TEXT_DIM);
	if (b->toggle) DrawRectangle((int)(r.x + 10.0f * s), (int)(r.y + r.height - 5.0f * s), (int)(r.width - 20.0f * s),
		                         (int)fmaxf(2.0f, 2.0f * s), b->active ? ON : OFF);
}

static void draw_resource(Rectangle r, const char *name, float have, float cap, float income, float spent, Color color, float s)
{
	panel(r, s);
	Rectangle bar = {r.x + 70.0f * s, r.y + 8.0f * s, r.width - 78.0f * s, 8.0f * s};
	DrawRectangleRec(bar, (Color){40, 44, 52, 255});
	DrawRectangleRec((Rectangle){bar.x, bar.y, bar.width * (cap > 0.0f ? fminf(1.0f, have / cap) : 0.0f), bar.height}, color);
	text(name, r.x + 8.0f * s, r.y + 9.0f * s, 16.0f * s, color);
	text(TextFormat("%.0f / %.0f", have, cap), bar.x, r.y + 19.0f * s, 12.0f * s, TEXT);
	const char *rate = TextFormat("+%.1f  -%.1f", income, spent);
	text(rate, r.x + r.width - (float)MeasureText(rate, (int)(12.0f * s)) - 8.0f * s, r.y + 19.0f * s, 12.0f * s,
	     income >= spent ? ON : OFF);
}

static const char *order_name(OrderKind o)
{
	static const char *names[ORDER_KIND_COUNT] = {"idle", "moving", "fighting", "building", "waiting to send", "digging in",
	                                              "warping home"};
	return o < ORDER_KIND_COUNT ? names[o] : "";
}

// What an order is, in a few words: "build generator", "move", "fight".
static const char *order_words(OrderKind kind, UnitType type)
{
	return kind == ORDER_BUILD ? TextFormat("build %s", unit_defs[type].name) : kind == ORDER_MOVE ? "move"
	     : kind == ORDER_ATTACK_MOVE ? "fight" : order_name(kind);
}

// One unit's orders: what it's doing, then what's queued, runs of the same
// together ("then build generator x6"), as many lines as there's room for.
static void draw_order_list(const Units *u, ptrdiff_t d, float x, float y, float bottom, float sc)
{
	if (u->order[d] == ORDER_IDLE && u->later_len[d] == 0) return;
	float line = 15.0f * sc;
	const char *now = order_words(u->order[d], u->build_type[d]);
	ptrdiff_t site = u->order[d] == ORDER_BUILD ? units_find(u, u->building[d]) : -1;
	if (site >= 0) now = TextFormat("%s (%.0f%%)", now, u->progress[site] * 100.0f);
	text(TextFormat("now: %s", now), x, y, 13.0f * sc, TEXT);
	y += line;
	int shown = 0;
	for (int32_t n = u->later_head[d]; n >= 0;) {
		if (y + 2.0f * line > bottom && u->later_next[n] >= 0) { // room for one more line: say what's left
			text(TextFormat("+%d more", u->later_len[d] - shown), x, y, 13.0f * sc, TEXT_DIM);
			return;
		}
		int run = 1;
		int32_t m = u->later_next[n];
		while (m >= 0 && u->later_kind[m] == u->later_kind[n]
		       && (u->later_kind[n] != ORDER_BUILD || u->later_type[m] == u->later_type[n])) {
			run++;
			m = u->later_next[m];
		}
		text(TextFormat("then: %s%s", order_words(u->later_kind[n], u->later_type[n]), run > 1 ? TextFormat(" x%d", run) : ""),
		     x, y, 13.0f * sc, TEXT_DIM);
		y += line;
		shown += run;
		n = m;
	}
}

// The info pane: a tile per type selected, or one unit's details.
static void draw_info(const Controls *c, const Sim *sim, const Layout *L, const SelectionSummary *s, Vector2 mouse,
                      bool live)
{
	float sc = L->s;
	panel(L->info, sc);
	const Units *u = own_units(c, sim);
	if (s->total == 0) {
		text("nothing selected", L->info.x + 10.0f * sc, L->info.y + 10.0f * sc, 16.0f * sc, TEXT_DIM);
		return;
	}
	if (s->total > 1) {
		UnitType types[TILES];
		int n = tile_types(s, types);
		for (int k = 0; k < n; k++) {
			Rectangle r = L->tile[k];
			bool hover = live && CheckCollisionPointRec(mouse, r);
			DrawRectangleRec(r, hover ? BUTTON_HOVER : BUTTON);
			DrawRectangleRec((Rectangle){r.x, r.y, 4.0f * sc, r.height}, render_player_color((int)c->me.v, c->player));
			text(unit_defs[types[k]].name, r.x + 8.0f * sc, r.y + 4.0f * sc, 12.0f * sc, TEXT);
			text(TextFormat("%d", s->count[types[k]]), r.x + 8.0f * sc, r.y + r.height - 18.0f * sc, 16.0f * sc, TEXT);
		}
		return;
	}
	// One unit: what it is, its health, what it's doing, and what's particular to it.
	ptrdiff_t d = -1;
	for (ptrdiff_t k = 0; k < c->selection_count && d < 0; k++) d = units_find(u, c->selection[k]);
	if (d < 0) return;
	const UnitDef *def = &unit_defs[u->type[d]];
	float x = L->info.x + 10.0f * sc, y = L->info.y + 8.0f * sc;
	text(def->name, x, y, 22.0f * sc, render_player_color((int)c->me.v, u->owner[d]));
	const char *who = c->team[u->owner[d].v] && c->team[u->owner[d].v][0] ? c->team[u->owner[d].v] : "you";
	// What it's doing: a factory with a queue is producing, whatever its order says.
	const char *doing = unit_is_factory(u->type[d]) && u->queue_len[d] ? "producing" : order_name(u->order[d]);
	if (u->progress[d] < 1.0f) {
		UnitHandle h = units_handle(u, d);
		int on = 0;
		for (ptrdiff_t i = 0; i < u->count; i++) on += u->order[i] == ORDER_BUILD && unit_handle_eq(u->building[i], h);
		doing = TextFormat("being built: %d builder%s on it", on, on == 1 ? "" : "s");
	}
	text(TextFormat("%s   %s", doing, u->owner[d].v == c->player.v ? "" : who), x, y + 26.0f * sc,
	     14.0f * sc, TEXT_DIM);
	Rectangle hp = {x, y + 46.0f * sc, L->info.width - 20.0f * sc, 8.0f * sc};
	DrawRectangleRec(hp, (Color){60, 30, 30, 255});
	DrawRectangleRec((Rectangle){hp.x, hp.y, hp.width * fminf(1.0f, u->hp[d] / def->hp), hp.height}, ON);
	text(TextFormat("%.0f / %.0f hp", u->hp[d], def->hp), x, y + 58.0f * sc, 13.0f * sc, TEXT);
	char line[160] = "";
	if (def->range > 0.0f)
		snprintf(line, sizeof line, "range %.0f   damage %.0f every %.1f s   sight %.0f   speed %.0f", def->range,
		         def->damage, (double)def->reload / SIM_HZ, def->sight, def->speed);
	else if (def->speed > 0.0f)
		snprintf(line, sizeof line, "sight %.0f   speed %.0f%s", def->sight, def->speed,
		         def->build_power > 0.0f ? TextFormat("   build power %.0f", def->build_power) : "");
	else
		snprintf(line, sizeof line, "sight %.0f%s%s", def->sight,
		         def->metal_income > 0.0f ? TextFormat("   +%.1f metal", def->metal_income * sim->config.context.yield[def->source]) : "",
		         def->energy_income > 0.0f ? TextFormat("   +%.0f energy", def->energy_income * sim->config.context.yield[def->source]) : "");
	text(line, x, y + 78.0f * sc, 13.0f * sc, TEXT);
	if (u->type[d] == UNIT_METAL_TRUCK || u->type[d] == UNIT_METAL_VAULT)
		text(TextFormat("storage +%.0f metal", sim->config.context.truck_metal), x, y + 96.0f * sc, 13.0f * sc, METAL);
	if (u->type[d] == UNIT_ENERGY_TRUCK || u->type[d] == UNIT_ENERGY_VAULT)
		text(TextFormat("storage +%.0f energy", sim->config.context.truck_energy), x, y + 96.0f * sc, 13.0f * sc, ENERGY);
	if (unit_is_factory(u->type[d]) && u->progress[d] >= 1.0f) {
		// In order, runs of a type together: "queue: raider, 5 tank".
		char q[160] = "queue:";
		int used = 6;
		for (int k = 0; k < u->queue_len[d] && used < (int)sizeof q;) {
			UnitType t = (UnitType)(u->queue[d] >> (8 * k) & 0xff);
			int run = 1;
			while (k + run < u->queue_len[d] && (UnitType)(u->queue[d] >> (8 * (k + run)) & 0xff) == t) run++;
			used += snprintf(q + used, sizeof q - (size_t)used, "%s %s%s", k ? "," : "", run > 1 ? TextFormat("%d ", run) : "",
			                 unit_defs[t].name);
			k += run;
		}
		if (!u->queue_len[d]) snprintf(q + used, sizeof q - (size_t)used, " empty");
		text(q, x, y + 96.0f * sc, 13.0f * sc, TEXT);
		if (u->queue_len[d]) {
			Rectangle bar = {x, y + 116.0f * sc, L->info.width - 20.0f * sc, 6.0f * sc};
			DrawRectangleRec(bar, (Color){40, 44, 52, 255});
			DrawRectangleRec((Rectangle){bar.x, bar.y, bar.width * u->work[d], bar.height}, ENERGY);
		}
	}
	if (u->progress[d] < 1.0f) text(TextFormat("under construction: %.0f%%", u->progress[d] * 100.0f), x, y + 96.0f * sc, 13.0f * sc, ORANGE);
	if (u->type[d] == UNIT_COMMANDER) {
		const char *warp = u->order[d] == ORDER_WARP ? TextFormat("warping home: %.0f%%", u->work[d] * 100.0f)
		                 : u->recharge[d] > 0      ? TextFormat("warp home in %.0f s", (double)u->recharge[d] / SIM_HZ)
		                                           : "warp home ready";
		text(warp, x, y + 96.0f * sc, 13.0f * sc, u->recharge[d] > 0 ? TEXT_DIM : ENERGY);
	}
	draw_order_list(u, d, x, y + 118.0f * sc, L->info.y + L->info.height - 6.0f * sc, sc);
	if (u->type[d] == UNIT_HEADQUARTERS) {
		// Who's coming back, and when.
		const Faction *fa = &sim->factions[c->me.v];
		char line2[160] = "";
		int used2 = 0;
		for (int p = 0; p < sim->config.players[c->me.v] && used2 < (int)sizeof line2; p++)
			if (fa->respawn_at[p] >= 0)
				used2 += snprintf(line2 + used2, sizeof line2 - (size_t)used2, "%s%s in %.0f s", used2 ? ", " : "respawning: ",
				                  c->team[p] && c->team[p][0] ? c->team[p] : TextFormat("player %d", p + 1),
				                  (double)(fa->respawn_at[p] - sim->tick.v) / SIM_HZ);
		text(used2 ? line2 : "no commanders to respawn", x, y + 96.0f * sc, 13.0f * sc, used2 ? ENERGY : TEXT_DIM);
	}
}

// What a prompt in progress is asking, above the orders grid.
static void draw_prompt(const Controls *c, const Layout *L)
{
	const SendTargets *z = &c->targets;
	char line[320] = "";
	int used = 0;
	if (c->placing >= 0)
		snprintf(line, sizeof line, "placing a %s: click to build, drag for a grid (shift: queue more), right-click or esc cancels", unit_defs[c->placing].name);
	else if (c->aiming >= 0)
		snprintf(line, sizeof line, "%s: click where (shift: queue more), right-click or esc cancels", c->aiming == COMMAND_MOVE ? "move" : "fight");
	else if (c->sending) {
		used = snprintf(line, sizeof line, "send to:");
		for (int k = 0; k < z->count && k < 9 && used < (int)sizeof line; k++)
			used += snprintf(line + used, sizeof line - (size_t)used, "   %d %s%s", k + 1, z->name[k],
			                 k == z->here          ? " (here)"
			                 : z->transporter[k]   ? ""
			                 : z->neighbour[k]     ? " (unpaired: raiders, builders, scouts)"
			                                       : " (no way)");
	} else if (c->landing_for >= 0) {
		snprintf(line, sizeof line, "landing in %s: click where, in your strip (esc cancels)", z->name[c->landing_for]);
	} else if (c->send_for >= 0 || c->giving) {
		used = snprintf(line, sizeof line, "%s",
		                c->send_for >= 0 ? TextFormat("send to %s, for:   Enter you", z->name[c->send_for])
		                : c->giving == 2 ? "factories make units for:"
		                : c->giving == 4 ? "give a quarter of your metal and energy to:"
		                                 : "give to:");
		for (int k = 0; k < c->team_count && used < (int)sizeof line; k++)
			if (k != c->player.v || c->giving == 2)
				used += snprintf(line + used, sizeof line - (size_t)used, "   %d %s", k + 1,
				                 c->team[k] && c->team[k][0] ? c->team[k] : "(bot)");
		if (used < (int)sizeof line) snprintf(line + used, sizeof line - (size_t)used, "   (esc cancels)");
	}
	if (!line[0]) return;
	float s = L->s, size = 16.0f * s;
	float w = (float)MeasureText(line, (int)size) + 16.0f * s;
	Rectangle r = {L->orders.x, L->orders.y - size - 16.0f * s, w, size + 10.0f * s};
	panel(r, s);
	text(line, r.x + 8.0f * s, r.y + 5.0f * s, size, TEXT);
}

void controls_draw_screen(const Controls *c, const Sim *sim, int w, int h, const Input *in)
{
	bool live = in != nullptr;
	Vector2 mouse = live ? in->mouse : (Vector2){-1.0f, -1.0f};
	if (live && c->dragging)
		DrawRectangleLinesEx((Rectangle){fminf(c->drag_start.x, mouse.x), fminf(c->drag_start.y, mouse.y),
		                                 fabsf(mouse.x - c->drag_start.x), fabsf(mouse.y - c->drag_start.y)},
		                     1.0f, SELECTED_COLOR);
	Layout L = layout(w, h);
	float s = L.s;
	SelectionSummary sum = menu_summarize(own_units(c, sim), c->selection, c->selection_count);
	MenuContext ctx = menu_context(c, sim);
	BuildMenu build = menu_build(&sum, c->tab[!sum.builders], (int)sim->config.context.tier);
	OrdersMenu orders = menu_orders(&sum, &ctx);
	bool build_keys = menu_build_open(&sum);

	if (sim->config.economy) {
		const Economy *e = &sim->factions[c->me.v].economy[c->player.v];
		draw_resource(L.metal, "metal", e->metal, e->metal_cap, e->metal_income, e->metal_spent, METAL, s);
		draw_resource(L.energy, "energy", e->energy, e->energy_cap, e->energy_income, e->energy_spent, ENERGY, s);
	}
	under_construction(L.minimap, "minimap", s);

	panel(L.build, s);
	for (int k = 0; k < BUILD_ITEMS; k++)
		draw_build_item(L.item[k], &build.item[k], &sim->config.context, action_key(c, build_action(k)), build_keys, CheckCollisionPointRec(mouse, L.item[k]), s);
	for (int k = 0; k < BUILD_TABS; k++)
		draw_text_button(L.tab[k], &build.tab[k], action_key(c, build_action(BUILD_ITEMS + k)), build_keys,
		                 CheckCollisionPointRec(mouse, L.tab[k]), s);
	draw_info(c, sim, &L, &sum, mouse, live);

	panel(L.orders, s);
	for (int k = 0; k < ORDER_SLOTS; k++)
		draw_text_button(L.order[k], &orders.slot[k], orders.slot[k].shown ? action_key(c, order_action[orders.slot[k].order]) : "", true,
		                 CheckCollisionPointRec(mouse, L.order[k]), s);
	under_construction(L.groups, "groups", s);
	under_construction(L.idle, "idle builders", s);

	// Your side's players, bottom right, in their colours.
	panel(L.players, s);
	text("Your team", L.players.x + 10.0f * s, L.players.y + 8.0f * s, 16.0f * s, TEXT_DIM);
	uint8_t away = sim->factions[c->me.v].away;
	for (int k = 0; k < c->team_count; k++) {
		bool named = c->team[k] && c->team[k][0];
		const char *who = TextFormat("%d  %s%s%s", k + 1, named ? c->team[k] : "(bot)", k == c->player.v ? "  (you)" : "",
		                             away >> k & 1 && named ? "  away" : "");
		text(who, L.players.x + 14.0f * s, L.players.y + (32.0f + 18.0f * (float)k) * s, 16.0f * s,
		     render_player_color((int)c->me.v, (PlayerSlot){(uint8_t)k}));
	}
	draw_prompt(c, &L);
}
