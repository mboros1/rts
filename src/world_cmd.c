// rts world: the game. A persistent world of zones (world.h), each an
// rts-server process, with an rts-coord between them, all running in the
// background so the world keeps going when the window closes.
//
//   rts world                       continue: start whatever isn't running,
//                                   then open the strategic map
//   rts world --new FILE            a new world from a world file (worlds/)
//   rts world --stop                save every zone and stop
//   rts world --status [--as SIDE]  the map as text (as SIDE sees it: through its fog)
//   rts world --save NAME           stop, copy the world, start it again
//   rts world --load NAME           stop, replace the world with a save, start
//   rts world --saves               list saves
//
//   --dir DIR        where worlds live (default: world/): the running one in
//                    DIR/live, saves in DIR/saves/NAME
//   --headless       start the world but open no window
//   --speed X        the zones' clocks run X times real time (tests, demos)
//   --faction F      who you play (blue, the default, or red)
//   anything else goes to the client: --journey, --bot, --netlog ...
//
// Each zone runs the economy scenario on its own terrain, seeded from its
// name (DIR/live/ZONE.script); only a side's home zone starts with its base.
// Bots play every player nobody is playing. While you're logged in (on the
// map or in any zone) your units everywhere are yours and hold where they
// are; 30 game seconds after you log out, your bot plays them.
//
// In DIR/live: world.txt (the world file it was made from), coord.log and
// ZONE.state (the persistent state), ZONE-NNN.replay (one segment per run of
// a zone), *.out (logs), *.pid.

#include <stdarg.h>

#include "net.h"
#include "platform.h"
#include "world.h"

enum { WORLD_PATH = 512 };

// Paths and arguments: formatted into a ring deep enough for one argv.
static const char *fmt(const char *f, ...)
{
	static char bufs[32][WORLD_PATH];
	static int next;
	char *out = bufs[next++ % 32];
	va_list ap;
	va_start(ap, f);
	int n = vsnprintf(out, WORLD_PATH, f, ap);
	va_end(ap);
	if (n < 0 || n >= WORLD_PATH) fatal("path too long: %s", out);
	return out;
}

typedef struct {
	char dir[WORLD_PATH / 2], live[WORLD_PATH];
	double speed;
	bool headless;
	World world;
	Arena arena;
} WorldCmd;

static const char *live_path(WorldCmd *w, const char *name)
{
	return fmt("%s/%s", w->live, name);
}

static int read_pid(WorldCmd *w, const char *name)
{
	Str text;
	ArenaMark m = arena_mark(&w->arena);
	int pid = platform_read_file(&w->arena, live_path(w, fmt("%s.pid", name)), &text)
	              ? atoi(fmt("%.*s", (int)text.len, text.ptr)) : 0;
	arena_rewind(m);
	return pid;
}

static bool running(WorldCmd *w, const char *name) { return platform_alive(read_pid(w, name)); }

// The formats a world's saved state is in; a world made by another build
// can't be continued, only replaced.
static const char *formats(void) { return fmt("zone-state %d wire %d\n", ZONE_STATE_VERSION, WIRE_VERSION); }

static bool load_world(WorldCmd *w)
{
	Str text;
	char err[256];
	if (!platform_read_file(&w->arena, live_path(w, "world.txt"), &text)) return false;
	if (!world_parse(&w->world, text, live_path(w, "world.txt"), err, sizeof err)) fatal("%s", err);
	return true;
}

static bool same_build(WorldCmd *w)
{
	Str text;
	const char *want = formats();
	return platform_read_file(&w->arena, live_path(w, "formats"), &text) && text.len == (ptrdiff_t)strlen(want)
	    && !memcmp(text.ptr, want, (size_t)text.len);
}

static void start_process(WorldCmd *w, const char *name, const char *const argv[])
{
	int pid = platform_spawn_detached(argv, live_path(w, fmt("%s.out", name)));
	if (!pid) fatal("cannot start %s", argv[0]);
	const char *text = fmt("%d\n", pid);
	if (!platform_write_file(live_path(w, fmt("%s.pid", name)), text, (ptrdiff_t)strlen(text)))
		fatal("cannot write %s.pid", name);
}

// Start whatever part of the world isn't running.
static void world_start(WorldCmd *w)
{
	const char *bin = GetApplicationDirectory();
	char coord[64], speed[32];
	snprintf(coord, sizeof coord, "127.0.0.1:%d", w->world.port);
	snprintf(speed, sizeof speed, "%g", w->speed);
	if (!running(w, "coord")) {
		char port[16], log[WORLD_PATH], world[WORLD_PATH];
		snprintf(port, sizeof port, "%d", w->world.port);
		snprintf(log, sizeof log, "%s", live_path(w, "coord.log"));
		snprintf(world, sizeof world, "%s", live_path(w, "world.txt"));
		const char *argv[] = {fmt("%srts-coord", bin), "--port", port, "--log", log, "--world", world, nullptr};
		start_process(w, "coord", argv);
		printf("world: started the coordinator on port %s\n", port);
	}
	char world[WORLD_PATH];
	snprintf(world, sizeof world, "%s", live_path(w, "world.txt"));
	for (int k = 0; k < w->world.zone_count; k++) {
		const char *name = w->world.zones[k].name;
		if (running(w, name)) continue;
		// A new replay segment for each run of the zone.
		int seg = 1;
		while (platform_file_exists(live_path(w, fmt("%s-%03d.replay", name, seg)))) seg++;
		char port[16], state[WORLD_PATH], record[WORLD_PATH], stats[WORLD_PATH], script[WORLD_PATH];
		snprintf(port, sizeof port, "%d", w->world.port + 1 + k);
		snprintf(state, sizeof state, "%s", live_path(w, fmt("%s.state", name)));
		snprintf(record, sizeof record, "%s", live_path(w, fmt("%s-%03d.replay", name, seg)));
		snprintf(stats, sizeof stats, "%s", live_path(w, fmt("%s.txt", name)));
		snprintf(script, sizeof script, "%s", live_path(w, fmt("%s.script", name)));
		const char *argv[] = {fmt("%srts-server", bin), "--script", script, "--zone", name, "--world", world,
		                      "--coord", coord, "--port", port, "--state", state,
		                      "--record", record, "--stats", stats, "--speed", speed, nullptr};
		start_process(w, name, argv);
		printf("world: started zone %s on port %s\n", name, port);
	}
}

static bool wait_gone(int pid, double seconds)
{
	for (double t = 0.0; t < seconds; t += 0.05) {
		if (!platform_alive(pid)) return true;
		net_sleep(0.05);
	}
	return !platform_alive(pid);
}

// Zones first (each saves on SIGTERM and reports to the coordinator), then
// the coordinator.
static bool world_stop(WorldCmd *w)
{
	bool ok = true;
	for (int k = 0; k < w->world.zone_count; k++) {
		int pid = read_pid(w, w->world.zones[k].name);
		if (platform_alive(pid)) platform_terminate(pid);
	}
	for (int k = 0; k < w->world.zone_count; k++) {
		int pid = read_pid(w, w->world.zones[k].name);
		if (platform_alive(pid) && !wait_gone(pid, 20.0)) {
			fprintf(stderr, "world: zone %s didn't stop\n", w->world.zones[k].name);
			ok = false;
		}
	}
	int pid = read_pid(w, "coord");
	if (platform_alive(pid)) {
		platform_terminate(pid);
		ok = wait_gone(pid, 5.0) && ok;
	}
	printf(ok ? "world: stopped\n" : "world: some processes didn't stop\n");
	return ok;
}

static bool any_running(WorldCmd *w)
{
	if (running(w, "coord")) return true;
	for (int k = 0; k < w->world.zone_count; k++)
		if (running(w, w->world.zones[k].name)) return true;
	return false;
}

static int run_tool(const char *const argv[])
{
	int status = platform_run(argv);
	if (status != 0) fatal("%s failed", argv[0]);
	return status;
}

static void print_status(WorldCmd *w, uint8_t viewer)
{
	Directory d;
	const char *coord = fmt("127.0.0.1:%d", w->world.port);
	if (!directory_fetch(coord, viewer, nullptr, false, &w->arena, &d, nullptr)) {
		printf("world: the coordinator isn't answering (rts world to start it)\n");
		return;
	}
	directory_print(&d, stdout);
}

int world_main(int argc, char **argv)
{
	static WorldCmd w;
	w.speed = 1.0;
	w.arena = arena_create("world", (ptrdiff_t)64 << 20);
	snprintf(w.dir, sizeof w.dir, "world");
	const char *new_file = nullptr, *save = nullptr, *load = nullptr, *faction = "blue", *as = nullptr;
	bool stop = false, status = false, saves = false;
	// What isn't ours goes to the client.
	const char *client_args[64];
	int client_argc = 0;
	client_args[client_argc++] = argv[0];
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : nullptr;
		if (!strcmp(a, "--stop")) stop = true;
		else if (!strcmp(a, "--status")) status = true;
		else if (!strcmp(a, "--saves")) saves = true;
		else if (!strcmp(a, "--headless")) w.headless = true;
		else if (!strcmp(a, "--new") && v) new_file = v, i++;
		else if (!strcmp(a, "--save") && v) save = v, i++;
		else if (!strcmp(a, "--load") && v) load = v, i++;
		else if (!strcmp(a, "--dir") && v) snprintf(w.dir, sizeof w.dir, "%s", v), i++;
		else if (!strcmp(a, "--speed") && v) w.speed = atof(v), i++;
		else if (!strcmp(a, "--faction") && v) faction = v, i++;
		else if (!strcmp(a, "--as") && v) as = v, i++;
		else if (client_argc < (int)countof(client_args) - 6) client_args[client_argc++] = a;
	}
	if (!(w.speed > 0.0 && w.speed <= 100.0)) fatal("--speed is between 0 and 100");
	snprintf(w.live, sizeof w.live, "%s/live", w.dir);
	const char *saves_dir = fmt("%s/saves", w.dir);
	if (!platform_make_dir(w.dir) || !platform_make_dir(saves_dir)) fatal("cannot create %s", w.dir);
	if (saves) {
		const char *argv_ls[] = {"ls", "-1", saves_dir, nullptr};
		return platform_run(argv_ls) == 0 ? 0 : 1;
	}
	if (save && strchr(save, '/')) fatal("--save NAME: a plain name");
	if (load && strchr(load, '/')) fatal("--load NAME: a plain name");

	bool have = load_world(&w);
	if (new_file || load) {
		if (have && any_running(&w) && !world_stop(&w)) fatal("cannot stop the running world");
		const char *rm[] = {"rm", "-rf", w.live, nullptr};
		run_tool(rm);
		if (new_file) {
			Str text;
			World check;
			char err[256];
			if (!platform_read_file(&w.arena, new_file, &text)) fatal("cannot read %s", new_file);
			if (!world_parse(&check, text, new_file, err, sizeof err)) fatal("%s", err);
			if (!platform_make_dir(w.live)) fatal("cannot create %s", w.live);
			if (!platform_write_file(live_path(&w, "world.txt"), text.ptr, text.len)) fatal("cannot write world.txt");
			if (!platform_write_file(live_path(&w, "formats"), formats(), (ptrdiff_t)strlen(formats())))
				fatal("cannot write formats");
			// Each zone's setup: its own terrain and spots, from its name.
			for (int k = 0; k < check.zone_count; k++) {
				uint64_t seed = 1469598103934665603u;
				for (const char *c = check.zones[k].name; *c; c++) seed = (seed ^ (uint8_t)*c) * 1099511628211u;
				// Only a side's home zone starts with its base; elsewhere it has to get in.
				int8_t home = check.zones[k].home;
				const char *script = fmt("scenario economy\nseed %llu\nbases %s\nplayers blue %d\nplayers red %d\n%s%s%s%s%s%s",
				                         (unsigned long long)(seed % 1000000007u),
				                         home == 0 ? "blue" : home == 1 ? "red" : "none", check.players[0], check.players[1],
				                         world_doctrine(&check, k, 0)[0] ? "doctrine blue " : "", world_doctrine(&check, k, 0),
				                         world_doctrine(&check, k, 0)[0] ? "\n" : "",
				                         world_doctrine(&check, k, 1)[0] ? "doctrine red " : "", world_doctrine(&check, k, 1),
				                         world_doctrine(&check, k, 1)[0] ? "\n" : "");
				if (!platform_write_file(live_path(&w, fmt("%s.script", check.zones[k].name)), script, (ptrdiff_t)strlen(script)))
					fatal("cannot write the zone scripts");
			}
			printf("world: new world from %s\n", new_file);
		} else {
			const char *src = fmt("%s/%s", saves_dir, load);
			if (!platform_file_exists(fmt("%s/world.txt", src))) fatal("no save %s (rts world --saves)", load);
			char from[WORLD_PATH];
			snprintf(from, sizeof from, "%s", src);
			const char *cp[] = {"cp", "-R", from, w.live, nullptr};
			run_tool(cp);
			// The save's pid files are from processes long gone.
			printf("world: loaded save %s\n", load);
		}
		have = load_world(&w);
	}
	if (!have) fatal("no world in %s yet: rts world --new worlds/three.world", w.live);
	if (!same_build(&w) && !stop && !status) {
		fprintf(stderr, "world: %s was made by an older build of the game and can't be continued.\n"
		                "       Start a new one: make play WORLD=worlds/three.world (or rts world --new FILE)\n",
		        w.live);
		return 1;
	}
	if (stop) return world_stop(&w) ? 0 : 1;
	if (save) {
		bool was = any_running(&w);
		if (was && !world_stop(&w)) fatal("cannot stop the world to save it");
		char to[WORLD_PATH];
		snprintf(to, sizeof to, "%s/%s", saves_dir, save);
		const char *rm[] = {"rm", "-rf", to, nullptr};
		const char *cp[] = {"cp", "-R", w.live, to, nullptr};
		run_tool(rm);
		run_tool(cp);
		printf("world: saved as %s\n", save);
		if (!was) return 0;
	}
	if (status) {
		print_status(&w, !as ? SPECTATOR : !strcmp(as, "blue") ? 0 : !strcmp(as, "red") ? 1 : SPECTATOR);
		return 0;
	}

	world_start(&w);
	if (w.headless) return 0;
	// The coordinator needs a moment before it answers.
	static char coord[64];
	snprintf(coord, sizeof coord, "127.0.0.1:%d", w.world.port);
	client_args[client_argc++] = "--coord";
	client_args[client_argc++] = coord;
	client_args[client_argc++] = "--faction";
	client_args[client_argc++] = faction;
	client_args[client_argc] = nullptr;
	return client_main(client_argc, (char **)client_args);
}
