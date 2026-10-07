// Headless runner: no window, no raylib.
//
//   rts-headless bench  [ticks] [units]  time the sim with bots on both sides
//   rts-headless record PATH [ticks] [units]  run bots and record a replay
//   rts-headless verify PATH             replay and compare every tick's hash
//
// Unity build: this file is the whole program.
#include "base.h"

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"

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

static int cmd_verify(const char *path)
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
	sim_init(&sim, r.config);
	int64_t ticks = 0;
	for (;;) {
		arena_reset(&decode);
		Tick tick;
		Command *cmds;
		ptrdiff_t n;
		Hash want;
		st = replay_next(&r, &decode, &tick, &cmds, &n, &want);
		if (st == REPLAY_END) break;
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
	}
	printf("verified %lld ticks of %s: every hash matches, final %016llx\n", (long long)ticks,
	       path, (unsigned long long)sim_hash_now(&sim).v);
	sim_destroy(&sim);
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	if (!strcmp(cmd, "bench")) return cmd_bench(parse_long(argc > 2 ? argv[2] : nullptr, 1800),
	                                           parse_long(argc > 3 ? argv[3] : nullptr, 5000));
	if (!strcmp(cmd, "record") && argc > 2)
		return cmd_record(argv[2], parse_long(argc > 3 ? argv[3] : nullptr, 1800),
		                  parse_long(argc > 4 ? argv[4] : nullptr, 5000));
	if (!strcmp(cmd, "verify") && argc > 2) return cmd_verify(argv[2]);
	fprintf(stderr, "usage: %s bench [ticks] [units] | record PATH [ticks] [units] | verify PATH\n", argv[0]);
	return 2;
}
