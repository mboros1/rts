// The playable sandbox: you command faction 0 (blue), bots play both sides.
//
//   WASD / arrows, middle-drag   pan          wheel        zoom
//   left-drag / click            select       shift        add to selection
//   right-click                  attack-move  ctrl+right   move
//   X  stop    B  toggle your side's bot    space  pause    F1  overlay
//   C  clip the last 10 seconds, as you saw them, to the clipboard
//
//   rts [--scenario NAME]    play (default: battle)
//   rts --connect HOST:PORT  play on a server (rts-server); see client.c
//   rts clip ...             render clips offline; see clip.h, `rts clip`
//
// Every admitted command, yours and the bots', is recorded; on exit the run
// is written to build/last.replay for `rts-headless verify`.
//
// Unity build: this file is the whole program.
#include "base.h"

#include <math.h>

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"
#include "scenario.c"
#include "script.c"
#include "wire.c"
#include "scene.c"
#include "render.c"
#include "view.c"
#include "net.c"
#include "client.c"
#include "clip.c"

static constexpr ptrdiff_t MAX_COMMANDS_PER_TICK = 256;
static constexpr int TIMING_WINDOW = 64; // a power of two
static constexpr int CAMERA_RING = 2048;  // a power of two; ~68 s of ticks
static constexpr double CLIP_SECONDS = 10.0;


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

	// Derived data for clips: where the camera was each tick, a ring.
	CameraKey camera_ring[CAMERA_RING];
	int64_t camera_keys;

	// A clip rendering in the background, finished when `clip_done` exists.
	char clip_done[512];
	char clip_video[512];
	char toast[512];
	double toast_until;
} Game;

static void start_clip(Game *g);

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

static void queue_command(Game *g, CommandKind kind, V2 target)
{
	if (g->selection_count == 0 || g->queued_count == MAX_COMMANDS_PER_TICK) return;
	const UnitHandle *units = arena_copy(&g->commands, g->selection,
	                                     g->selection_count * (ptrdiff_t)sizeof(UnitHandle),
	                                     alignof(UnitHandle));
	g->queued[g->queued_count++] = (Command){.faction = {0}, .kind = kind, .target = target,
	                                       .count = g->selection_count, .units = units};
}

static void handle_input(Game *g)
{
	const Terrain *t = &g->sim.terrain;
	if (IsKeyPressed(KEY_SPACE)) g->paused = !g->paused;
	if (IsKeyPressed(KEY_F1)) g->overlay = !g->overlay;
	if (IsKeyPressed(KEY_B)) g->bots[0].enabled = !g->bots[0].enabled;
	if (IsKeyPressed(KEY_C)) start_clip(g);
	if (IsKeyPressed(KEY_X)) {
		const Units *u = &g->sim.factions[0].units;
		ptrdiff_t d = g->selection_count ? units_find(u, g->selection[0]) : -1;
		if (d >= 0) queue_command(g, COMMAND_STOP, v2(u->px[d], u->pz[d]));
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
			queue_command(g, move ? COMMAND_MOVE : COMMAND_ATTACK_MOVE, at);
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
	g->camera_ring[g->camera_keys++ & (CAMERA_RING - 1)] =
		(CameraKey){g->sim.tick.v, g->view.focus.x, g->view.focus.z, g->view.distance};
	g->queued_count = 0;
	arena_reset(&g->commands);
}

// ---------------------------------------------------------------- clips

static void show_toast(Game *g, const char *text, double seconds)
{
	snprintf(g->toast, sizeof g->toast, "%s", text);
	g->toast_until = GetTime() + seconds;
}

// Hand the run so far and the camera's recent path to `rts clip` in the
// background; it re-renders the last seconds exactly and fills the clipboard.
static void start_clip(Game *g)
{
	if (g->clip_done[0]) {
		show_toast(g, "still rendering the last clip", 2.0);
		return;
	}
	int64_t now = g->sim.tick.v;
	if (now < 2) return;
	const char *dir = TextFormat("%sclips", GetApplicationDirectory());
	if (!platform_make_dir(dir)) {
		show_toast(g, "cannot create the clips directory", 3.0);
		return;
	}
	// A truncated path would point the clip somewhere else: refuse instead.
	char base[400], replay[512], camera[512], sheet[512];
	int n = snprintf(base, sizeof base, "%s/clip-%06lld", dir, (long long)now);
	bool fits = n > 0 && n < (int)sizeof base;
	fits = fits && snprintf(replay, sizeof replay, "%s.replay", base) < (int)sizeof replay;
	fits = fits && snprintf(camera, sizeof camera, "%s.camera", base) < (int)sizeof camera;
	fits = fits && snprintf(sheet, sizeof sheet, "%s.png", base) < (int)sizeof sheet;
	fits = fits && snprintf(g->clip_video, sizeof g->clip_video, "%s.mp4", base) < (int)sizeof g->clip_video;
	fits = fits && snprintf(g->clip_done, sizeof g->clip_done, "%s.done", base) < (int)sizeof g->clip_done;
	if (!fits) {
		show_toast(g, "clip path too long", 3.0);
		g->clip_done[0] = 0;
		return;
	}

	Str bytes = replay_bytes(&g->writer);
	int64_t keys = g->camera_keys < CAMERA_RING ? g->camera_keys : CAMERA_RING;
	CameraKey *ordered = PUSH(&g->frame, CameraKey, keys);
	for (int64_t k = 0; k < keys; k++)
		ordered[k] = g->camera_ring[(g->camera_keys - keys + k) & (CAMERA_RING - 1)];
	if (!platform_write_file(replay, bytes.ptr, bytes.len) || !camera_track_write(camera, ordered, keys)) {
		show_toast(g, "cannot write the clip's inputs", 3.0);
		g->clip_done[0] = 0;
		return;
	}
	char last[32], to[32];
	snprintf(last, sizeof last, "%.1f", CLIP_SECONDS);
	snprintf(to, sizeof to, "%lld", (long long)now);
	const char *self = TextFormat("%srts", GetApplicationDirectory());
	const char *argv[] = {self, "clip", "--replay", replay, "--camera", camera, "--to", to,
	                      "--last", last, "--out", g->clip_video, "--sheet", sheet,
	                      "--clipboard", "--done", g->clip_done, nullptr};
	if (!platform_spawn(argv)) {
		show_toast(g, "cannot start the clip renderer", 3.0);
		g->clip_done[0] = 0;
		return;
	}
	show_toast(g, "clipping the last 10 seconds...", 60.0);
}

static void poll_clip(Game *g)
{
	platform_reap_children();
	if (!g->clip_done[0] || !platform_file_exists(g->clip_done)) return;
	bool ok = platform_file_exists(g->clip_video);
	show_toast(g, ok ? TextFormat("clip on the clipboard: %s", GetFileName(g->clip_video))
	                 : "clip failed; run `rts clip` by hand to see why", 5.0);
	g->clip_done[0] = 0;
}

static void draw_toast(const Game *g)
{
	if (GetTime() > g->toast_until) return;
	int w = MeasureText(g->toast, 20);
	int x = (GetScreenWidth() - w) / 2, y = GetScreenHeight() - 60;
	DrawRectangle(x - 12, y - 8, w + 24, 36, (Color){0, 0, 0, 180});
	DrawText(g->toast, x, y, 20, RAYWHITE);
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
	DrawText("drag select | right: attack-move | ctrl+right: move | X stop | B bot | C clip | space pause",
	         12, y, 14, GRAY);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "clip")) return clip_main(argc, argv);
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--connect")) return client_main(argc, argv);
	const Scenario *scenario = scenario_find("battle");
	if (argc > 2 && !strcmp(argv[1], "--scenario") && !(scenario = scenario_find(argv[2]))) {
		fprintf(stderr, "unknown scenario %s; `rts clip --list` lists them\n", argv[2]);
		return 2;
	}

	static Game g;
	SimConfig config = scenario_config(scenario);
	sim_init(&g.sim, config);
	scenario_bots(scenario, g.bots);

	g.program = arena_create("program", (ptrdiff_t)64 << 20);
	g.frame = arena_create("frame", (ptrdiff_t)256 << 20);
	g.commands = arena_create("commands", (ptrdiff_t)16 << 20);
	g.replay = arena_create("replay", (ptrdiff_t)4 << 30);
	replay_begin(&g.writer, &g.replay, config);
	g.selection = PUSH(&g.program, UnitHandle, config.max_units);
	g.overlay = true;

	SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
	InitWindow(1600, 900, TextFormat("rts: %s", scenario->name));
	SetTargetFPS(120);
	renderer_init(&g.renderer, &g.sim.terrain, config.max_units, &g.program);
	g.view = (View){
		.focus = v2(g.sim.terrain.size_x * 0.5f, g.sim.terrain.size_z * 0.5f),
		.distance = 420.0f,
	};

	double accumulator = 0.0;
	while (!WindowShouldClose()) {
		arena_reset(&g.frame);
		view_update(&g.view, &g.sim.terrain);
		handle_input(&g);
		poll_clip(&g);

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
		Scene scene = scene_from_sim(&g.sim);
		renderer_draw_scene(&g.renderer, &scene, g.paused ? 0.0f : (float)(accumulator / SIM_DT),
		                    &g.frame, selected, 0);
		EndMode3D();
		if (g.dragging) {
			Vector2 m = GetMousePosition();
			Rectangle box = {fminf(g.drag_start.x, m.x), fminf(g.drag_start.y, m.y),
			                 fabsf(m.x - g.drag_start.x), fabsf(m.y - g.drag_start.y)};
			DrawRectangleLinesEx(box, 1.0f, SELECTED_COLOR);
		}
		if (g.overlay) draw_overlay(&g);
		draw_toast(&g);
		EndDrawing();
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
