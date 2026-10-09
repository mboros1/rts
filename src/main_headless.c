// Headless runner: no window, no raylib.
//
//   rts-headless bench  [ticks] [units]  time the sim with bots on both sides
//   rts-headless bench-teams [ticks] [players]  an economy game, PLAYERS a side,
//                                         all bots: time the sim and the bots apart
//   rts-headless record PATH [ticks] [units]  run bots and record a replay
//   rts-headless verify PATH [--partial] [--hash-at T]
//                                         replay and compare every tick's hash;
//                                         --partial accepts a crash-cut tail;
//                                         --hash-at prints the state's hash at tick T
//                                         (to chain segments that overlap)
//   rts-headless script PATH...          run scripts and their expectations
//   rts-headless wire [ticks] [units]    bytes a spectator would receive
//   rts-headless split [ticks] [units]   both factions simulated as clients from
//                                         their streams, hashes checked every tick
//
// Unity build: this file is the whole program.
#include "base.h"

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"
#include "scenario.c"
#include "script.c"
#include "wire.c"

static constexpr ptrdiff_t MAX_COMMANDS_PER_TICK = 256;

typedef struct {
	Arena commands;        // tick lifetime: bot output
	Bot bots[FACTION_COUNT];
} Driver;

static Driver driver_create(uint64_t seed)
{
	Driver d = {.commands = arena_create("commands", (ptrdiff_t)16 << 20)};
	for (int k = 0; k < FACTION_COUNT; k++) d.bots[k] = bot_create((FactionId){(uint32_t)k}, seed);
	return d;
}

static ptrdiff_t driver_think(Driver *d, const Sim *sim, Command *out)
{
	arena_reset(&d->commands);
	ptrdiff_t n = 0;
	for (int k = 0; k < FACTION_COUNT; k++)
		bot_think(&d->bots[k], sim, &d->commands, out, &n, MAX_COMMANDS_PER_TICK);
	return n;
}

static long parse_long(const char *s, long fallback)
{
	if (!s) return fallback;
	char *end;
	long v = strtol(s, &end, 10);
	return (*end || v <= 0) ? fallback : v;
}

static void print_summary(const Sim *sim)
{
	for (int k = 0; k < FACTION_COUNT; k++) {
		const Faction *f = &sim->factions[k];
		printf("  faction %d: %td units, %td shells incoming, %lld fired, %lld lost, %lld dropped\n",
		       k, f->units.count, f->incoming.count, (long long)f->stats.shots_fired,
		       (long long)f->stats.units_lost, (long long)f->stats.launches_dropped);
	}
	const Arena *arenas[] = {&sim->zone, &sim->interaction, &sim->factions[0].scratch,
	                         &sim->factions[1].scratch};
	for (ptrdiff_t k = 0; k < countof(arenas); k++)
		printf("  arena %-18s high water %8.2f MB of %6.0f MB\n", arenas[k]->name,
		       (double)arenas[k]->high_water / (1 << 20), (double)arenas[k]->reserved / (1 << 20));
}

static int cmd_bench(long ticks, long units)
{
	SimConfig config = sim_config_default();
	config.initial_units = (int32_t)units;
	config.population = (int32_t)units;
	if (config.max_units < units) config.max_units = (int32_t)units;
	Sim sim;
	sim_init(&sim, config);
	Driver d = driver_create(config.seed);
	Command cmds[MAX_COMMANDS_PER_TICK];

	double total = 0.0, worst = 0.0;
	ptrdiff_t peak_units = 0, peak_shells = 0;
	for (long t = 0; t < ticks; t++) {
		ptrdiff_t n = driver_think(&d, &sim, cmds);
		double t0 = platform_seconds();
		sim_step(&sim, cmds, n);
		double dt = platform_seconds() - t0;
		total += dt;
		if (dt > worst) worst = dt;
		ptrdiff_t alive = sim.factions[0].units.count + sim.factions[1].units.count;
		ptrdiff_t shells = sim.factions[0].incoming.count + sim.factions[1].incoming.count;
		if (alive > peak_units) peak_units = alive;
		if (shells > peak_shells) peak_shells = shells;
		if ((t + 1) % 300 == 0)
			printf("tick %5ld: %5td units, %6td shells, mean %.2f ms, worst %.2f ms\n", t + 1,
			       alive, shells, 1e3 * total / (double)(t + 1), 1e3 * worst);
	}
	printf("%ld ticks, %ld units per side: mean %.2f ms/tick, worst %.2f ms (budget %.1f ms)\n",
	       ticks, units, 1e3 * total / (double)ticks, 1e3 * worst, 1e3 / SIM_HZ);
	printf("peak %td units, %td shells in flight; final hash %016llx\n", peak_units, peak_shells,
	       (unsigned long long)sim_hash_now(&sim).v);
	print_summary(&sim);
	sim_destroy(&sim);
	return 0;
}

// A team game from the start, everyone a bot: what a full zone costs. The
// sim's step and the bots' thinking are timed apart (bots run in the zone's
// tick too).
static int cmd_bench_teams(long ticks, long players)
{
	if (players > MAX_PLAYERS) players = MAX_PLAYERS;
	char text[128];
	snprintf(text, sizeof text, "scenario economy\nplayers blue %ld\nplayers red %ld\n", players, players);
	Arena a = arena_create("bench-teams", (ptrdiff_t)64 << 20);
	Arena tick = arena_create("bench-teams tick", (ptrdiff_t)64 << 20);
	Script sc;
	char err[256];
	if (!script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "bench-teams", err, sizeof err)) fatal("%s", err);
	Sim sim;
	sim_init(&sim, sc.config);
	ScriptRun run;
	script_run_init(&run, &sc, &a, nullptr);
	enum { CAP = 8192 };
	static Command cmds[CAP];
	double step_total = 0.0, step_worst = 0.0, bot_total = 0.0, bot_worst = 0.0;
	ptrdiff_t peak_units = 0, most_commands = 0;
	for (long t = 0; t < ticks; t++) {
		arena_reset(&tick);
		double t0 = platform_seconds();
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, CAP);
		double t1 = platform_seconds();
		sim_step(&sim, cmds, n);
		double t2 = platform_seconds();
		bot_total += t1 - t0;
		step_total += t2 - t1;
		bot_worst = t1 - t0 > bot_worst ? t1 - t0 : bot_worst;
		step_worst = t2 - t1 > step_worst ? t2 - t1 : step_worst;
		most_commands = n > most_commands ? n : most_commands;
		ptrdiff_t alive = sim.factions[0].units.count + sim.factions[1].units.count;
		if (alive > peak_units) peak_units = alive;
		if ((t + 1) % 1800 == 0)
			printf("minute %3ld: %5td v %5td units   step mean %.2f worst %.2f ms   bots mean %.2f worst %.2f ms\n",
			       (t + 1) / 1800, sim.factions[0].units.count, sim.factions[1].units.count,
			       1e3 * step_total / (double)(t + 1), 1e3 * step_worst, 1e3 * bot_total / (double)(t + 1),
			       1e3 * bot_worst);
	}
	printf("%ld ticks, %ld v %ld players: step mean %.2f ms, worst %.2f ms; bots mean %.2f ms, worst %.2f ms "
	       "(budget %.1f ms)\n",
	       ticks, players, players, 1e3 * step_total / (double)ticks, 1e3 * step_worst, 1e3 * bot_total / (double)ticks,
	       1e3 * bot_worst, 1e3 / SIM_HZ);
	printf("peak %td units; most commands in a tick %td; final hash %016llx\n", peak_units, most_commands,
	       (unsigned long long)sim_hash_now(&sim).v);
	print_summary(&sim);
	sim_destroy(&sim);
	return 0;
}

static int cmd_record(const char *path, long ticks, long units)
{
	SimConfig config = sim_config_default();
	config.initial_units = (int32_t)units;
	config.population = (int32_t)units;
	if (config.max_units < units) config.max_units = (int32_t)units;
	Sim sim;
	sim_init(&sim, config);
	Driver d = driver_create(config.seed);
	Arena file = arena_create("replay", (ptrdiff_t)1 << 30);
	ReplayWriter w;
	replay_begin(&w, &file, config);
	Command cmds[MAX_COMMANDS_PER_TICK];
	for (long t = 0; t < ticks; t++) {
		ptrdiff_t n = driver_think(&d, &sim, cmds);
		Tick tick = sim.tick;
		sim_step(&sim, cmds, n);
		replay_tick(&w, tick, cmds, n, sim_hash_now(&sim));
	}
	Str bytes = replay_bytes(&w);
	if (!platform_write_file(path, bytes.ptr, bytes.len)) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	printf("recorded %ld ticks to %s (%td bytes), final hash %016llx\n", ticks, path, bytes.len,
	       (unsigned long long)sim_hash_now(&sim).v);
	sim_destroy(&sim);
	return 0;
}

static int cmd_verify(const char *path, bool partial, int64_t hash_at)
{
	Arena file = arena_create("replay file", (ptrdiff_t)1 << 30);
	Arena decode = arena_create("replay decode", (ptrdiff_t)64 << 20);
	Str data;
	if (!platform_read_file(&file, path, &data)) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	ReplayReader r;
	ReplayStatus st = replay_open(&r, data);
	if (st != REPLAY_OK) {
		fprintf(stderr, "%s: %s\n", path, replay_status_name(st));
		return 1;
	}
	Sim sim;
	if (!replay_start(&r, &sim)) {
		fprintf(stderr, "%s: its snapshot doesn't load\n", path);
		return 1;
	}
	int64_t ticks = 0;
	int64_t start_tick = sim.tick.v;
	Hash start_hash = sim_hash_now(&sim);
	if (hash_at == start_tick) printf("hash at tick %lld: %016llx\n", (long long)hash_at, (unsigned long long)start_hash.v);
	for (;;) {
		arena_reset(&decode);
		Tick tick;
		Command *cmds;
		ptrdiff_t n;
		Hash want;
		st = replay_next(&r, &decode, &tick, &cmds, &n, &want);
		if (st == REPLAY_END || (partial && st == REPLAY_TRUNCATED)) break;
		if (st != REPLAY_OK) {
			fprintf(stderr, "%s: %s at byte %td\n", path, replay_status_name(st), r.pos);
			return 1;
		}
		if (tick.v != sim.tick.v) {
			fprintf(stderr, "%s: expected tick %lld, file has %lld\n", path,
			        (long long)sim.tick.v, (long long)tick.v);
			return 1;
		}
		sim_step(&sim, cmds, n);
		Hash got = sim_hash_now(&sim);
		if (got.v != want.v) {
			fprintf(stderr, "DIVERGED after tick %lld: recorded %016llx, replayed %016llx\n",
			        (long long)tick.v, (unsigned long long)want.v, (unsigned long long)got.v);
			return 1;
		}
		ticks++;
		if (sim.tick.v == hash_at) printf("hash at tick %lld: %016llx\n", (long long)hash_at, (unsigned long long)got.v);
	}
	printf("verified %lld ticks of %s: every hash matches, final %016llx\n", (long long)ticks,
	       path, (unsigned long long)sim_hash_now(&sim).v);
	printf("segment from tick %lld (%016llx) to tick %lld (%016llx)\n", (long long)start_tick,
	       (unsigned long long)start_hash.v, (long long)sim.tick.v, (unsigned long long)sim_hash_now(&sim).v);
	sim_destroy(&sim);
	return 0;
}

// What the match stream costs, without sockets: encode every tick's delta
// for a bot battle and report the bytes.
static int cmd_wire(long ticks, long units)
{
	SimConfig config = sim_config_default();
	config.initial_units = config.population = (int32_t)units;
	if (config.max_units < units) config.max_units = (int32_t)units;
	Sim sim;
	sim_init(&sim, config);
	Driver d = driver_create(config.seed);
	Arena a = arena_create("wire", (ptrdiff_t)1 << 30);
	Mirror m;
	mirror_init(&m, &a, config.max_units, config.max_projectiles);
	ByteBuf buf = bytebuf_make(&a, (ptrdiff_t)64 << 20);
	wire_encode_tick(&m, &sim, &buf, false, 1000, nullptr);
	Command cmds[MAX_COMMANDS_PER_TICK];
	WireStats ws = {0};
	int64_t total = 0, worst = 0, window = 0;
	double encode = 0.0;
	for (long t = 0; t < ticks; t++) {
		sim_step(&sim, cmds, driver_think(&d, &sim, cmds));
		buf.len = 0;
		double t0 = platform_seconds();
		wire_encode_tick(&m, &sim, &buf, sim.tick.v % DIGEST_EVERY == 0, 1000, &ws);
		encode += platform_seconds() - t0;
		total += buf.len;
		window += buf.len;
		if (buf.len > worst) worst = buf.len;
		if ((t + 1) % 300 == 0) {
			printf("tick %5ld: %5td units, %6.1f KB/s over the last 10 s\n", t + 1,
			       sim.factions[0].units.count + sim.factions[1].units.count, (double)window / 10.0 / 1024.0);
			window = 0;
		}
	}
	buf.len = 0;
	wire_encode_snapshot(&m, &buf);
	printf("%ld ticks, %ld units a side: mean %.1f KB/s, worst tick %.1f KB, snapshot %.1f KB, "
	       "encode %.3f ms/tick\n", ticks, units, (double)total / (double)ticks * SIM_HZ / 1024.0,
	       (double)worst / 1024.0, (double)buf.len / 1024.0, 1e3 * encode / (double)ticks);
	printf("unit entries %lld (%.1f a tick), shells started %lld\n", (long long)ws.units_sent,
	       (double)ws.units_sent / (double)ticks, (long long)ws.shells_started);
	sim_destroy(&sim);
	return 0;
}

// Spike 3 at scale: the server's battle, and each faction rebuilt by a
// client sim from nothing but its encoded stream, hash-checked every tick.
static int cmd_split(long ticks, long units)
{
	SimConfig config = sim_config_default();
	config.initial_units = config.population = (int32_t)units;
	if (config.max_units < units) config.max_units = (int32_t)units;
	Sim server, client[FACTION_COUNT];
	sim_init(&server, config);
	Driver d = driver_create(config.seed);
	Arena a = arena_create("split", (ptrdiff_t)1 << 30), tick = arena_create("split tick", (ptrdiff_t)256 << 20);
	ByteBuf buf = bytebuf_make(&a, (ptrdiff_t)64 << 20);
	int64_t bytes[FACTION_COUNT] = {0}, worst[FACTION_COUNT] = {0}, seen[FACTION_COUNT] = {0};
	ptrdiff_t snapshot = 0;
	for (int f = 0; f < FACTION_COUNT; f++) {
		sim_init(&client[f], config);
		buf.len = 0;
		wire_encode_faction_snapshot(&server, (FactionId){(uint32_t)f}, &buf);
		snapshot = buf.len;
		ByteReader r = byte_reader(buf.data, buf.len);
		client[f].tick.v = (int64_t)get64(&r);
		if (!faction_read(&client[f].factions[f], &r)) fatal("snapshot didn't load");
	}
	Command cmds[MAX_COMMANDS_PER_TICK];
	double server_s = 0.0, client_s = 0.0;
	int64_t mismatches = 0;
	for (long t = 0; t < ticks; t++) {
		ptrdiff_t n = driver_think(&d, &server, cmds);
		double t0 = platform_seconds();
		sim_step(&server, cmds, n);
		server_s += platform_seconds() - t0;
		for (int f = 0; f < FACTION_COUNT; f++) {
			FactionId fid = {(uint32_t)f};
			arena_reset(&tick);
			buf.len = 0;
			wire_encode_faction_tick(&server, fid, cmds, n, true, 1000, &buf);
			bytes[f] += buf.len;
			if (buf.len > worst[f]) worst[f] = buf.len;
			seen[f] += server.effects[f].seen_count;
			ByteReader r = byte_reader(buf.data, buf.len);
			FactionTick ft;
			if (!wire_read_faction_tick(&r, &tick, &config, fid, &ft)) fatal("tick %ld didn't decode", t);
			t0 = platform_seconds();
			faction_tick(&client[f], fid, &ft.effects, ft.commands, ft.command_count);
			client_s += platform_seconds() - t0;
			mismatches += faction_hash(&client[f].factions[f]).v != ft.hash.v;
		}
		if ((t + 1) % 300 == 0)
			printf("tick %5ld: %5td units   blue's stream %6.1f KB/s   red's %6.1f KB/s   mismatches %lld\n", t + 1,
			       server.factions[0].units.count + server.factions[1].units.count,
			       (double)bytes[0] / (double)(t + 1) * SIM_HZ / 1024.0,
			       (double)bytes[1] / (double)(t + 1) * SIM_HZ / 1024.0, (long long)mismatches);
	}
	printf("%ld ticks, %ld units a side: faction hashes checked %ld times each, %lld mismatched\n", ticks, units,
	       ticks, (long long)mismatches);
	for (int f = 0; f < FACTION_COUNT; f++)
		printf("  %s stream: mean %.1f KB/s, worst tick %.1f KB, %.0f perception entries a tick\n",
		       f ? "red " : "blue", (double)bytes[f] / (double)ticks * SIM_HZ / 1024.0, (double)worst[f] / 1024.0,
		       (double)seen[f] / (double)ticks);
	printf("  faction snapshot %.1f KB; server step %.2f ms, client faction step %.2f ms\n",
	       (double)snapshot / 1024.0, 1e3 * server_s / (double)ticks, 1e3 * client_s / (double)ticks / FACTION_COUNT);
	return mismatches ? 1 : 0;
}

static int cmd_script(int count, char **paths)
{
	Arena work = arena_create("script", (ptrdiff_t)1 << 30);
	Arena tick = arena_create("script tick", (ptrdiff_t)64 << 20);
	int failed = 0;
	for (int i = 0; i < count; i++) {
		arena_reset(&work);
		Script script;
		char err[512];
		if (!script_load(&script, &work, paths[i], err, sizeof err)) {
			fprintf(stderr, "%s\n", err);
			failed++;
			continue;
		}
		printf("%s: %s\n", paths[i], script.title[0] ? script.title : "(untitled)");
		Sim sim;
		sim_init(&sim, script.config);
		ScriptRun run;
		script_run_init(&run, &script, &work, stdout);
		Command cmds[MAX_COMMANDS_PER_TICK];
		while (sim.tick.v < script.end) {
			arena_reset(&tick);
			ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, MAX_COMMANDS_PER_TICK);
			sim_step(&sim, cmds, n);
		}
		printf("  %d of %d expectations held over %lld ticks\n", run.expects - run.failures,
		       run.expects, (long long)script.end);
		failed += run.failures > 0;
		sim_destroy(&sim);
	}
	return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	if (!strcmp(cmd, "bench-teams")) return cmd_bench_teams(parse_long(argc > 2 ? argv[2] : nullptr, 18000),
	                                                        parse_long(argc > 3 ? argv[3] : nullptr, 8));
	if (!strcmp(cmd, "bench")) return cmd_bench(parse_long(argc > 2 ? argv[2] : nullptr, 1800),
	                                           parse_long(argc > 3 ? argv[3] : nullptr, 5000));
	if (!strcmp(cmd, "record") && argc > 2)
		return cmd_record(argv[2], parse_long(argc > 3 ? argv[3] : nullptr, 1800),
		                  parse_long(argc > 4 ? argv[4] : nullptr, 5000));
	if (!strcmp(cmd, "verify") && argc > 2) {
		bool partial = false;
		int64_t hash_at = -1;
		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--partial")) partial = true;
			else if (!strcmp(argv[i], "--hash-at") && i + 1 < argc) hash_at = atoll(argv[++i]);
			else fatal("verify PATH [--partial] [--hash-at T]");
		}
		return cmd_verify(argv[2], partial, hash_at);
	}
	if (!strcmp(cmd, "script") && argc > 2) return cmd_script(argc - 2, argv + 2);
	if (!strcmp(cmd, "split")) return cmd_split(parse_long(argc > 2 ? argv[2] : nullptr, 1800),
	                                           parse_long(argc > 3 ? argv[3] : nullptr, 5000));
	if (!strcmp(cmd, "wire")) return cmd_wire(parse_long(argc > 2 ? argv[2] : nullptr, 1800),
	                                         parse_long(argc > 3 ? argv[3] : nullptr, 5000));
	fprintf(stderr, "usage: %s bench [ticks] [units] | bench-teams [ticks] [players] | record PATH [ticks] [units] | verify PATH | script PATH... | wire|split [ticks] [units]\n", argv[0]);
	return 2;
}
