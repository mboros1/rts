// The playable sandbox: you command faction 0 (blue), bots play both sides.
//
//   WASD / arrows, middle-drag   pan          wheel        zoom
//   left-drag / click            select       shift        add to selection
//   right-click                  attack-move  ctrl+right   move
//   X  stop    B  toggle your side's bot    space  pause    F1  overlay
//
// Every admitted command, yours and the bots', is recorded; on exit the run
// is written to build/last.replay for `rts-headless verify`.
//
// RTS_AUTOSHOT=path.png runs RTS_AUTOSHOT_TICKS sim ticks (default 600),
// saves a screenshot and exits: a way to look at the game without a person.
//
// Unity build: this file is the whole program.
#include <raylib.h>

#include "base.h"

#include <math.h>

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"
#include "render.c"

static constexpr ptrdiff_t MAX_COMMANDS_PER_TICK = 256;
static constexpr int TIMING_WINDOW = 64; // a power of two

typedef struct {
	Camera3D camera;
	V2 focus;
	float distance;
} View;

typedef struct {
	Sim sim;
	Bot bots[FACTION_COUNT];
	Renderer renderer;
	View view;

	Arena program;          // process lifetime: selection, render state
	Arena frame;            // one rendered frame
	Arena commands;         // until the next sim step: queued orders
	Arena replay;           // the replay file as it grows

	ReplayWriter writer;
	Command queued[MAX_COMMANDS_PER_TICK];
	ptrdiff_t queued_count;

	UnitHandle *selection;  // faction 0 units, may contain stale handles
	ptrdiff_t selection_count;

	bool paused;
	bool overlay;
	bool dragging;
	Vector2 drag_start;

	// Derived data for the overlay: recent sim step times, a ring.
	double step_ms[TIMING_WINDOW];
	int64_t steps;
} Game;

// ---------------------------------------------------------------- camera

static void view_update(View *v, const Terrain *t)
{
	float dt = GetFrameTime();
	float pan = v->distance * 1.2f * dt;
	if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) v->focus.z -= pan;
	if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) v->focus.z += pan;
	if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) v->focus.x -= pan;
	if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) v->focus.x += pan;
	if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
		Vector2 d = GetMouseDelta();
		float k = v->distance / (float)GetScreenHeight();
		v->focus.x -= d.x * k;
		v->focus.z -= d.y * k;
	}
	float wheel = GetMouseWheelMove();
	if (wheel != 0.0f) v->distance = f_clamp(v->distance * powf(0.88f, wheel), 25.0f, 700.0f);
	v->focus = v2(f_clamp(v->focus.x, 0.0f, t->size_x), f_clamp(v->focus.z, 0.0f, t->size_z));

	Vector3 target = {v->focus.x, terrain_height(t, v->focus.x, v->focus.z), v->focus.z};
	v->camera.target = target;
	v->camera.position = (Vector3){target.x, target.y + v->distance * 0.85f, target.z + v->distance * 0.55f};
}

// March the mouse ray down to the sim's terrain, then bisect.
static bool pick_ground(const View *v, const Terrain *t, Vector2 mouse, V2 *out)
{
	Ray ray = GetScreenToWorldRay(mouse, v->camera);
	float step = 1.0f, prev = 0.0f;
	for (float s = 0.0f; s < 3000.0f; s += step) {
		Vector3 p = {ray.position.x + ray.direction.x * s, ray.position.y + ray.direction.y * s,
		             ray.position.z + ray.direction.z * s};
		if (p.y <= terrain_height(t, p.x, p.z)) {
			float lo = prev, hi = s;
			for (int k = 0; k < 12; k++) {
				float mid = 0.5f * (lo + hi);
				Vector3 q = {ray.position.x + ray.direction.x * mid, ray.position.y + ray.direction.y * mid,
				             ray.position.z + ray.direction.z * mid};
				if (q.y <= terrain_height(t, q.x, q.z)) hi = mid; else lo = mid;
			}
			*out = v2(ray.position.x + ray.direction.x * hi, ray.position.z + ray.direction.z * hi);
			return true;
		}
		prev = s;
	}
	return false;
}

// ---------------------------------------------------------------- input

static void select_units(Game *g, Rectangle box, bool add, bool click)
{
	const Units *u = &g->sim.factions[0].units;
	if (!add) g->selection_count = 0;
	ptrdiff_t best = -1;
	float best_d2 = 14.0f * 14.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		Vector2 s = GetWorldToScreen((Vector3){u->px[i], u->py[i] + 0.5f, u->pz[i]}, g->view.camera);
		if (click) {
			float dx = s.x - box.x, dy = s.y - box.y;
			if (dx * dx + dy * dy < best_d2) {
				best = i;
				best_d2 = dx * dx + dy * dy;
			}
		} else if (CheckCollisionPointRec(s, box)) {
			g->selection[g->selection_count++] = units_handle(u, i);
		}
	}
	if (click && best >= 0) g->selection[g->selection_count++] = units_handle(u, best);
}

// Stale handles (dead units) and duplicates drop out of the selection.
static void prune_selection(Game *g, uint8_t *mark)
{
	const Units *u = &g->sim.factions[0].units;
	ptrdiff_t kept = 0;
	for (ptrdiff_t k = 0; k < g->selection_count; k++) {
		ptrdiff_t d = units_find(u, g->selection[k]);
		if (d < 0 || mark[d]) continue;
		mark[d] = 1;
		g->selection[kept++] = g->selection[k];
	}
	g->selection_count = kept;
}

static void queue_command(Game *g, OrderKind kind, V2 target)
{
	if (g->selection_count == 0 || g->queued_count == MAX_COMMANDS_PER_TICK) return;
	const UnitHandle *units = arena_copy(&g->commands, g->selection,
	                                     g->selection_count * (ptrdiff_t)sizeof(UnitHandle),
	                                     alignof(UnitHandle));
	g->queued[g->queued_count++] = (Command){{0}, kind, target, g->selection_count, units};
}

static void handle_input(Game *g)
{
	const Terrain *t = &g->sim.terrain;
	if (IsKeyPressed(KEY_SPACE)) g->paused = !g->paused;
	if (IsKeyPressed(KEY_F1)) g->overlay = !g->overlay;
	if (IsKeyPressed(KEY_B)) g->bots[0].enabled = !g->bots[0].enabled;
	if (IsKeyPressed(KEY_X)) {
		const Units *u = &g->sim.factions[0].units;
		ptrdiff_t d = g->selection_count ? units_find(u, g->selection[0]) : -1;
		if (d >= 0) queue_command(g, ORDER_IDLE, v2(u->px[d], u->pz[d]));
	}

	Vector2 mouse = GetMousePosition();
	if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		g->dragging = true;
		g->drag_start = mouse;
	}
	if (g->dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
		g->dragging = false;
		Rectangle box = {fminf(g->drag_start.x, mouse.x), fminf(g->drag_start.y, mouse.y),
		                 fabsf(mouse.x - g->drag_start.x), fabsf(mouse.y - g->drag_start.y)};
		bool click = box.width < 4.0f && box.height < 4.0f;
		if (click) box = (Rectangle){mouse.x, mouse.y, 0, 0};
		select_units(g, box, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT), click);
	}
	if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
		V2 at;
		if (pick_ground(&g->view, t, mouse, &at)) {
			bool move = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
			queue_command(g, move ? ORDER_MOVE : ORDER_ATTACK_MOVE, at);
		}
	}
}

// ---------------------------------------------------------------- stepping

static void step(Game *g)
{
	ptrdiff_t n = g->queued_count;
	for (int k = 0; k < FACTION_COUNT; k++)
		bot_think(&g->bots[k], &g->sim, &g->commands, g->queued, &n, MAX_COMMANDS_PER_TICK);
	Tick tick = g->sim.tick;
	double t0 = platform_seconds();
	sim_step(&g->sim, g->queued, n);
	g->step_ms[g->steps++ & (TIMING_WINDOW - 1)] = 1e3 * (platform_seconds() - t0);
	replay_tick(&g->writer, tick, g->queued, n, sim_hash_now(&g->sim));
	g->queued_count = 0;
	arena_reset(&g->commands);
}

// ---------------------------------------------------------------- overlay

static void draw_overlay(const Game *g)
{
	const Sim *s = &g->sim;
	int window = g->steps < TIMING_WINDOW ? (int)g->steps : TIMING_WINDOW;
	double mean = 0.0, worst = 0.0;
	for (int k = 0; k < window; k++) {
		mean += g->step_ms[k];
		worst = fmax(worst, g->step_ms[k]);
	}
	mean = window ? mean / window : 0.0;

	int y = 10, line = 20;
	DrawRectangle(5, 5, 470, 12 * line + 10, (Color){0, 0, 0, 150});
	DrawText(TextFormat("%d fps   tick %lld%s", GetFPS(), (long long)s->tick.v, g->paused ? "   PAUSED" : ""),
	         12, y, 18, RAYWHITE);
	y += line;
	DrawText(TextFormat("sim %.2f ms mean, %.2f ms worst (budget %.1f)", mean, worst, 1e3 / SIM_HZ),
	         12, y, 18, RAYWHITE);
	y += line;
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Faction *fa = &s->factions[f];
		DrawText(TextFormat("%s: %td units, %td shells in, %lld lost%s", f == 0 ? "blue" : "red",
		                    fa->units.count, fa->incoming.count, (long long)fa->stats.units_lost,
		                    g->bots[f].enabled ? "" : "  (bot off)"),
		         12, y, 18, FACTION_COLOR[f]);
		y += line;
	}
	DrawText(TextFormat("selected %td   hash %016llx", g->selection_count,
	                    (unsigned long long)sim_hash_now(s).v), 12, y, 18, RAYWHITE);
	y += line;
	const Arena *arenas[] = {&s->zone, &s->interaction, &s->factions[0].scratch,
	                         &s->factions[1].scratch, &g->frame, &g->replay};
	for (ptrdiff_t k = 0; k < countof(arenas); k++) {
		DrawText(TextFormat("%-18s %6.2f MB high, %4.0f MB reserved", arenas[k]->name,
		                    (double)arenas[k]->high_water / (1 << 20),
		                    (double)arenas[k]->reserved / (1 << 20)),
		         12, y, 16, LIGHTGRAY);
		y += line;
	}
	DrawText("drag select | right: attack-move | ctrl+right: move | X stop | B bot | space pause",
	         12, y, 14, GRAY);
}

// ---------------------------------------------------------------- main

int main(void)
{
	static Game g;
	SimConfig config = sim_config_default();
	sim_init(&g.sim, config);
	for (int k = 0; k < FACTION_COUNT; k++) g.bots[k] = bot_create((FactionId){(uint32_t)k}, config.seed);

	g.program = arena_create("program", (ptrdiff_t)64 << 20);
	g.frame = arena_create("frame", (ptrdiff_t)256 << 20);
	g.commands = arena_create("commands", (ptrdiff_t)16 << 20);
	g.replay = arena_create("replay", (ptrdiff_t)4 << 30);
	replay_begin(&g.writer, &g.replay, config);
	g.selection = PUSH(&g.program, UnitHandle, config.max_units);
	g.overlay = true;

	SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
	InitWindow(1600, 900, "rts spike 1");
	SetTargetFPS(120);
	renderer_init(&g.renderer, &g.sim, &g.program);
	g.view = (View){
		.camera = {.up = {0, 1, 0}, .fovy = 45.0f, .projection = CAMERA_PERSPECTIVE},
		.focus = v2(g.sim.terrain.size_x * 0.5f, g.sim.terrain.size_z * 0.5f),
		.distance = 420.0f,
	};

	const char *autoshot = getenv("RTS_AUTOSHOT");
	const char *autoshot_ticks = getenv("RTS_AUTOSHOT_TICKS");
	int64_t shot_at = autoshot_ticks ? atoll(autoshot_ticks) : 600;
	int settled_frames = 0;
	if (autoshot) g.view.distance = 260.0f;

	double accumulator = 0.0;
	while (!WindowShouldClose()) {
		arena_reset(&g.frame);
		view_update(&g.view, &g.sim.terrain);
		handle_input(&g);

		// Autoshot runs the sim flat out to the target tick before drawing.
		if (autoshot && GetTime() > 1.0)
			while (g.sim.tick.v < shot_at) step(&g);
		if (!g.paused) {
			accumulator += fmin(GetFrameTime(), 0.25);
			while (accumulator >= SIM_DT) {
				step(&g);
				accumulator -= SIM_DT;
			}
		}
		uint8_t *selected = PUSH(&g.frame, uint8_t, g.sim.factions[0].units.cap);
		prune_selection(&g, selected);

		BeginDrawing();
		ClearBackground((Color){24, 28, 34, 255});
		BeginMode3D(g.view.camera);
		renderer_draw_world(&g.renderer, &g.sim, g.paused ? 0.0f : (float)(accumulator / SIM_DT),
		                    &g.frame, selected);
		EndMode3D();
		if (g.dragging) {
			Vector2 m = GetMousePosition();
			Rectangle box = {fminf(g.drag_start.x, m.x), fminf(g.drag_start.y, m.y),
			                 fabsf(m.x - g.drag_start.x), fabsf(m.y - g.drag_start.y)};
			DrawRectangleLinesEx(box, 1.0f, SELECTED_COLOR);
		}
		if (g.overlay) draw_overlay(&g);
		EndDrawing();

		// raylib reads the framebuffer after the swap, which can hold an older
		// frame: pause on the target tick and draw a few frames before reading.
		if (autoshot && g.sim.tick.v >= shot_at) {
			g.paused = true;
			if (++settled_frames == 4) {
				TakeScreenshot(autoshot);
				break;
			}
		}
	}

	// Next to the executable, so it lands in build/ wherever the game is run from.
	Str bytes = replay_bytes(&g.writer);
	const char *path = TextFormat("%slast.replay", GetApplicationDirectory());
	if (platform_write_file(path, bytes.ptr, bytes.len))
		printf("wrote %s: %lld ticks, %td bytes\n", path, (long long)g.sim.tick.v, bytes.len);
	else
		fprintf(stderr, "cannot write %s\n", path);
	renderer_unload(&g.renderer);
	CloseWindow();
	sim_destroy(&g.sim);
	return 0;
}
