#include "controls.h"

#include <math.h>

#include "render.h"

void controls_init(Controls *c, FactionId me, Arena *a, ptrdiff_t max_units)
{
	*c = (Controls){.me = me, .selection = PUSH(a, UnitHandle, max_units), .placing = -1, .landing_for = -1,
	                .send_for = -1, .team_count = 1};
}

void controls_clear(Controls *c)
{
	c->selection_count = 0;
	c->dragging = false;
	c->placing = -1;
	c->sending = false;
	c->landing_for = -1;
	c->send_for = -1;
	c->giving = 0;
}

static const Units *own_units(const Controls *c, const Sim *sim) { return &sim->factions[c->me.v].units; }

static void select_units(Controls *c, const Sim *sim, const View *view, Rectangle box, bool add, bool click)
{
	const Units *u = own_units(c, sim);
	if (!add) c->selection_count = 0;
	ptrdiff_t best = -1;
	float best_d2 = 14.0f * 14.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!player_eq(u->owner[i], c->player)) continue; // teammates' units aren't yours to order
		Vector2 s = GetWorldToScreen((Vector3){u->px[i], u->py[i] + 0.5f, u->pz[i]}, view->camera);
		if (click) {
			float dx = s.x - box.x, dy = s.y - box.y;
			if (dx * dx + dy * dy < best_d2) {
				best = i;
				best_d2 = dx * dx + dy * dy;
			}
		} else if (CheckCollisionPointRec(s, box)) {
			c->selection[c->selection_count++] = units_handle(u, i);
		}
	}
	if (click && best >= 0) c->selection[c->selection_count++] = units_handle(u, best);
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

static bool selection_has(const Controls *c, const Sim *sim, UnitType type)
{
	const Units *u = own_units(c, sim);
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t d = units_find(u, c->selection[k]);
		if (d >= 0 && u->type[d] == type) return true;
	}
	return false;
}

// Where a structure would go for this click: extractors snap to the spot.
static V2 placement_site(const Sim *sim, UnitType type, V2 at)
{
	if (type != UNIT_EXTRACTOR) return at;
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

static bool selection_only(const Controls *c, const Sim *sim, UnitType type)
{
	const Units *u = own_units(c, sim);
	bool any = false;
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t d = units_find(u, c->selection[k]);
		if (d < 0) continue;
		if (u->type[d] != type) return false;
		any = true;
	}
	return any;
}

// A teammate's number key this frame (1 is player 0), or -1.
static int player_key(const Controls *c)
{
	for (int k = 0; k < c->team_count; k++)
		if (IsKeyPressed(KEY_ONE + k)) return k;
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

// T, then a number: the zone. Paired goes at once; unpaired waits for a
// landing point. The server decides the route.
static bool send_input(Controls *c, const Sim *sim, const View *view, Arena *frame, Order *out, ptrdiff_t cap,
                       ptrdiff_t *n)
{
	const SendTargets *z = &c->targets;
	if (z->count == 0) return false;
	if (IsKeyPressed(KEY_T) && c->selection_count) {
		c->sending = true;
		c->landing_for = -1;
		return true;
	}
	if (c->sending) {
		if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) c->sending = false;
		for (int k = 0; k < z->count && k < 9; k++) {
			if (!IsKeyPressed(KEY_ONE + k) || k == z->here) continue;
			c->sending = false;
			if (z->transporter[k] && c->team_count > 1) c->send_for = k; // then: for whom
			else if (z->transporter[k]) order(c, frame, out, cap, n, (Order){.kind = COMMAND_SEND, .amount = k, .to = c->player});
			else if (z->neighbour[k]) c->landing_for = k;
		}
		return true;
	}
	if (c->send_for >= 0) {
		int p = player_key(c);
		bool self = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_T);
		if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) c->send_for = -1;
		else if (p >= 0 || self) {
			PlayerSlot to = p >= 0 ? (PlayerSlot){(uint8_t)p} : c->player;
			order(c, frame, out, cap, n, (Order){.kind = COMMAND_SEND, .amount = c->send_for, .to = to});
			c->send_for = -1;
		}
		return true;
	}
	if (c->landing_for >= 0) {
		V2 at;
		if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) c->landing_for = -1;
		else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && pick_ground(view, &sim->terrain, GetMousePosition(), &at)) {
			at = sim_strip_point(&sim->terrain, c->me, at);
			order(c, frame, out, cap, n, (Order){.kind = COMMAND_SEND, .amount = c->landing_for, .target = at, .to = c->player});
			c->landing_for = -1;
		}
		return true;
	}
	return false;
}

ptrdiff_t controls_input(Controls *c, const Sim *sim, const View *view, Arena *frame, Order *out, ptrdiff_t cap)
{
	ptrdiff_t n = 0;
	Vector2 mouse = GetMousePosition();
	bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
	if (IsKeyPressed(KEY_X)) order(c, frame, out, cap, &n, (Order){.kind = COMMAND_STOP});
	if (IsKeyPressed(KEY_V)) {
		// Both at once: the sim takes the combat units for one, turrets for the other.
		order(c, frame, out, cap, &n, (Order){.kind = COMMAND_ENTRENCH});
		order(c, frame, out, cap, &n, (Order){.kind = COMMAND_MOBILIZE});
	}
	if (send_input(c, sim, view, frame, out, cap, &n)) return n;

	// H, then a teammate: give them the selection (or point factories at
	// them); with nothing selected, a quarter of your metal and energy.
	if (IsKeyPressed(KEY_H) && c->team_count > 1) {
		c->giving = !c->selection_count ? (sim->config.economy ? 4 : 0) : shift ? 3 : selection_only(c, sim, UNIT_FACTORY) ? 2 : 1;
		return n;
	}
	if (c->giving) {
		int p = player_key(c);
		if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) c->giving = 0;
		else if (p >= 0 && c->giving == 4) {
			const Economy *e = &sim->factions[c->me.v].economy[c->player.v];
			if (n < cap && p != c->player.v)
				out[n++] = (Order){.kind = COMMAND_SHARE, .target = {0.25f * e->metal, 0.25f * e->energy}, .to = {(uint8_t)p}};
			c->giving = 0;
		} else if (p >= 0) {
			CommandKind kind = c->giving == 2 ? COMMAND_OUTPUT : COMMAND_GIVE;
			order(c, frame, out, cap, &n, (Order){.kind = kind, .to = {(uint8_t)p}});
			c->giving = 0;
		}
		return n;
	}

	if (sim->config.economy) {
		if (selection_has(c, sim, UNIT_ENGINEER) || selection_has(c, sim, UNIT_SAPPER)) {
			if (IsKeyPressed(KEY_E)) c->placing = UNIT_EXTRACTOR;
			if (IsKeyPressed(KEY_G)) c->placing = UNIT_GENERATOR;
			if (IsKeyPressed(KEY_F)) c->placing = UNIT_FACTORY;
			if (IsKeyPressed(KEY_R)) c->placing = UNIT_TRANSPORTER;
		}
		if (selection_has(c, sim, UNIT_FACTORY)) {
			const UnitType keys[] = {UNIT_RAIDER, UNIT_TANK, UNIT_ARTILLERY, UNIT_ENGINEER, UNIT_SCOUT, UNIT_SAPPER, UNIT_SIEGE};
			for (int k = 0; k < (int)countof(keys); k++)
				if (IsKeyPressed(KEY_ONE + k))
					order(c, frame, out, cap, &n, (Order){.kind = COMMAND_PRODUCE, .type = keys[k], .amount = 1,
					                                      .flags = shift ? COMMAND_REPEAT : 0});
			if (IsKeyPressed(KEY_ZERO)) order(c, frame, out, cap, &n, (Order){.kind = COMMAND_PRODUCE, .amount = 0});
		}
		if (c->placing >= 0) {
			// A click that places or cancels a structure isn't a selection.
			if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
				c->placing = -1;
				return n;
			}
			V2 at;
			if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && pick_ground(view, &sim->terrain, mouse, &at)) {
				UnitType type = (UnitType)c->placing;
				order(c, frame, out, cap, &n, (Order){.kind = COMMAND_BUILD, .target = placement_site(sim, type, at), .type = type});
				if (!shift) c->placing = -1;
			}
			return n;
		}
	}

	if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		c->dragging = true;
		c->drag_start = mouse;
	}
	if (c->dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
		c->dragging = false;
		Rectangle box = {fminf(c->drag_start.x, mouse.x), fminf(c->drag_start.y, mouse.y),
		                 fabsf(mouse.x - c->drag_start.x), fabsf(mouse.y - c->drag_start.y)};
		bool click = box.width < 4.0f && box.height < 4.0f;
		if (click) box = (Rectangle){mouse.x, mouse.y, 0, 0};
		select_units(c, sim, view, box, shift, click);
	}
	V2 at;
	if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && pick_ground(view, &sim->terrain, mouse, &at)) {
		bool move = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
		order(c, frame, out, cap, &n, (Order){.kind = move ? COMMAND_MOVE : COMMAND_ATTACK_MOVE, .target = at});
	}
	return n;
}

void controls_draw_world(const Controls *c, const Sim *sim, const View *view)
{
	if (c->landing_for >= 0) {
		// Where an unpaired send may land: our strip, its edge traced on the ground.
		const Terrain *t = &sim->terrain;
		float edge = c->me.v == 0 ? t->size_x * STRIP : t->size_x * (1.0f - STRIP);
		Color line = {120, 255, 140, 220};
		for (float z = 0.0f; z + 4.0f <= t->size_z; z += 4.0f)
			DrawLine3D((Vector3){edge, terrain_height(t, edge, z) + 0.5f, z},
			           (Vector3){edge, terrain_height(t, edge, z + 4.0f) + 0.5f, z + 4.0f}, line);
		V2 at;
		if (pick_ground(view, t, GetMousePosition(), &at)) {
			at = sim_strip_point(t, c->me, at);
			DrawCircle3D((Vector3){at.x, terrain_height(t, at.x, at.z) + 0.6f, at.z}, 8.0f, (Vector3){1, 0, 0}, 90.0f, line);
		}
	}
	V2 ghost;
	if (c->placing < 0 || !pick_ground(view, &sim->terrain, GetMousePosition(), &ghost)) return;
	UnitType type = (UnitType)c->placing;
	ghost = placement_site(sim, type, ghost);
	bool ok = placement_ok(sim, c->me, type, ghost);
	const UnitDef *def = &unit_defs[type];
	Vector3 at = {ghost.x, terrain_height(&sim->terrain, ghost.x, ghost.z) + def->height * 0.5f, ghost.z};
	DrawCubeWires(at, def->radius * 2.0f, def->height, def->radius * 2.0f, ok ? GREEN : RED);
}

void controls_draw_screen(const Controls *c, const Sim *sim)
{
	if (c->dragging) {
		Vector2 m = GetMousePosition();
		DrawRectangleLinesEx((Rectangle){fminf(c->drag_start.x, m.x), fminf(c->drag_start.y, m.y),
		                                 fabsf(m.x - c->drag_start.x), fabsf(m.y - c->drag_start.y)},
		                     1.0f, SELECTED_COLOR);
	}
	// The HUD: a bar along the bottom.
	int h = GetScreenHeight(), y = h - 74;
	DrawRectangle(0, y - 8, GetScreenWidth(), 82, (Color){0, 0, 0, 150});
	if (sim->config.economy) {
		const Economy *e = &sim->factions[c->me.v].economy[c->player.v];
		DrawText(TextFormat("metal %.0f (+%.1f -%.1f/s)   energy %.0f (+%.0f -%.0f/s)%s", e->metal, e->metal_income,
		                    e->metal_spent, e->energy, e->energy_income, e->energy_spent,
		                    e->rate < 0.99f ? TextFormat("   STALL %.0f%%", e->rate * 100.0f) : ""),
		         14, y, 20, e->rate < 0.99f ? ORANGE : (Color){230, 220, 160, 255});
	}
	// Your side's players, above the bar, in their colours; you're marked.
	if (c->team_count > 1) {
		int x = 14;
		for (int k = 0; k < c->team_count; k++) {
			const char *who = TextFormat("%d %s%s", k + 1, c->team[k] && c->team[k][0] ? c->team[k] : "(bot)",
			                             k == c->player.v ? " (you)" : "");
			DrawText(who, x, y - 32, 18, render_player_color((int)c->me.v, (PlayerSlot){(uint8_t)k}));
			x += MeasureText(who, 18) + 24;
		}
	}
	// What's selected, by type.
	const Units *u = own_units(c, sim);
	int counts[UNIT_TYPE_COUNT] = {0};
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t d = units_find(u, c->selection[k]);
		if (d >= 0) counts[u->type[d]]++;
	}
	int transforming = 0;
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		ptrdiff_t d = units_find(u, c->selection[k]);
		transforming += d >= 0 && u->order[d] == ORDER_TRANSFORM;
	}
	char sel[256] = "nothing selected";
	int used = 0;
	for (int t = 0; t < UNIT_TYPE_COUNT; t++)
		if (counts[t] && used < (int)sizeof sel)
			used += snprintf(sel + used, sizeof sel - (size_t)used, "%s%d %s", used ? ", " : "", counts[t], unit_defs[t].name);
	if (transforming && used < (int)sizeof sel)
		snprintf(sel + used, sizeof sel - (size_t)used, "  (%d transforming)", transforming);
	if (c->placing >= 0) snprintf(sel, sizeof sel, "placing a %s: click to build, esc to cancel", unit_defs[c->placing].name);
	const SendTargets *z = &c->targets;
	if (c->sending) {
		// The zones, by number, and how a send there would go.
		used = snprintf(sel, sizeof sel, "send to:");
		for (int k = 0; k < z->count && k < 9 && used < (int)sizeof sel; k++)
			used += snprintf(sel + used, sizeof sel - (size_t)used, "   %d %s%s", k + 1, z->name[k],
			                 k == z->here          ? " (here)"
			                 : z->transporter[k]   ? ""
			                 : z->neighbour[k]     ? " (no transporter: raiders, engineers)"
			                                       : " (no way)");
	}
	if (c->landing_for >= 0)
		snprintf(sel, sizeof sel, "landing in %s: click where, in your strip (esc cancels)", z->name[c->landing_for]);
	if (c->send_for >= 0 || c->giving) {
		used = snprintf(sel, sizeof sel, "%s",
		                c->send_for >= 0 ? TextFormat("send to %s, for:   Enter you", z->name[c->send_for])
		                : c->giving == 2 ? "factories make units for:"
		                : c->giving == 4 ? "give a quarter of your metal and energy to:"
		                                 : "give to:");
		for (int k = 0; k < c->team_count && used < (int)sizeof sel; k++)
			if (k != c->player.v || c->giving == 2)
				used += snprintf(sel + used, sizeof sel - (size_t)used, "   %d %s", k + 1, c->team[k] && c->team[k][0] ? c->team[k] : "(bot)");
		if (used < (int)sizeof sel) snprintf(sel + used, sizeof sel - (size_t)used, "   (esc cancels)");
	}
	DrawText(sel, 14, y + 26, 18, RAYWHITE);
	DrawText(sim->config.economy
	             ? (c->targets.count ? "drag select | right attack-move | ctrl+right move | X stop | V entrench | engineers: E G F R | factories: 1-7, 0 clear | T send | H give"
	                                 : "drag select | right attack-move | ctrl+right move | X stop | V entrench | engineers: E G F R | factories: 1-7, 0 clear")
	             : "drag select | right attack-move | ctrl+right move | X stop | V entrench",
	         14, y + 50, 14, GRAY);
}
