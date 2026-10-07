// Unit tests. Unity build, run under ASan and UBSan by `make check`.
#include "base.h"

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"

static int failures;
static int checks;

#define CHECK(c)                                                              \
	do {                                                                      \
		checks++;                                                             \
		if (!(c)) {                                                           \
			failures++;                                                       \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
		}                                                                     \
	} while (0)

static void test_arena(void)
{
	Arena a = arena_create("test", 1 << 20);
	uint8_t *p = arena_push(&a, 100, 1);
	bool zero = true;
	for (int i = 0; i < 100; i++) zero = zero && p[i] == 0;
	CHECK(zero);
	double *d = PUSH(&a, double, 4);
	CHECK(((uintptr_t)d & (alignof(double) - 1)) == 0);

	ArenaMark m = arena_mark(&a);
	ptrdiff_t before = a.used;
	uint8_t *scratch = arena_push(&a, 1000, 1);
	scratch[0] = 7;
	arena_rewind(m);
	CHECK(a.used == before);
	CHECK(a.mark_depth == 0);
	// Memory handed out again after a rewind is zeroed, not stale.
	uint8_t *again = arena_push(&a, 1000, 1);
	CHECK(again[0] == 0);

	uint32_t gen = a.generation;
	arena_reset(&a);
	CHECK(a.used == 0 && a.generation == gen + 1);
	CHECK(a.high_water >= before + 1000);

	CHECK(arena_try_push(&a, (ptrdiff_t)2 << 20, 1) == nullptr);
	CHECK(arena_try_push(&a, PTRDIFF_MAX, 1) == nullptr);
	arena_destroy(&a);
}

static void test_units(void)
{
	Arena a = arena_create("units", 1 << 20);
	Units u;
	units_init(&u, &a, 4);
	ptrdiff_t d0 = units_spawn(&u, UNIT_RAIDER, 1, 1, 0);
	ptrdiff_t d1 = units_spawn(&u, UNIT_TANK, 2, 2, 0);
	ptrdiff_t d2 = units_spawn(&u, UNIT_ARTILLERY, 3, 3, 0);
	UnitHandle h0 = units_handle(&u, d0), h1 = units_handle(&u, d1), h2 = units_handle(&u, d2);
	CHECK(h0.index == 0 && h1.index == 1 && h2.index == 2);

	units_kill(&u, d1);
	CHECK(u.count == 2);
	CHECK(units_find(&u, h1) == -1);
	CHECK(units_find(&u, h0) == 0);
	ptrdiff_t moved = units_find(&u, h2);
	CHECK(moved == 1 && u.px[moved] == 3.0f && u.type[moved] == UNIT_ARTILLERY);

	// The freed slot comes back with a new generation; the old handle stays dead.
	ptrdiff_t d3 = units_spawn(&u, UNIT_RAIDER, 4, 4, 0);
	UnitHandle h3 = units_handle(&u, d3);
	CHECK(h3.index == h1.index && h3.generation != h1.generation);
	CHECK(units_find(&u, h1) == -1);
	CHECK(units_find(&u, (UnitHandle){99, 0}) == -1);
	arena_destroy(&a);
}

// Fly a shell with the sim's integration and see where it comes down.
static float landing_x(V3 vel)
{
	V3 p = v3(0, 0, 0);
	const float drop = 0.5f * SIM_GRAVITY * SIM_DT * SIM_DT;
	for (int t = 0; t < 10000; t++) {
		V3 next = v3(p.x + vel.x * SIM_DT, p.y + vel.y * SIM_DT - drop, p.z + vel.z * SIM_DT);
		vel.y -= SIM_GRAVITY * SIM_DT;
		if (next.y <= 0.0f) {
			float s = p.y / (p.y - next.y);
			return p.x + (next.x - p.x) * s;
		}
		p = next;
	}
	return -1.0f;
}

static void test_ballistics(void)
{
	V3 vel;
	float time;
	CHECK(ballistic_solve(v3(0, 0, 0), v3(50, 0, 0), 40.0f, false, &vel, &time));
	float low = landing_x(vel);
	CHECK(low > 49.9f && low < 50.1f);
	float low_time = time;
	CHECK(ballistic_solve(v3(0, 0, 0), v3(50, 0, 0), 40.0f, true, &vel, &time));
	float high = landing_x(vel);
	CHECK(high > 49.9f && high < 50.1f);
	CHECK(time > low_time * 2.0f); // a lob stays up long enough to dodge
	CHECK(!ballistic_solve(v3(0, 0, 0), v3(200, 0, 0), 40.0f, false, &vel, &time));
	CHECK(!ballistic_solve(v3(0, 0, 0), v3(0, 5, 0), 40.0f, false, &vel, &time));
}

static SimConfig small_config(uint64_t seed)
{
	SimConfig c = sim_config_default();
	c.seed = seed;
	c.initial_units = 300;
	c.population = 300;
	c.max_units = 512;
	c.max_projectiles = 8192;
	return c;
}

static int64_t run_with_bots(Sim *sim, Bot bots[FACTION_COUNT], Arena *cmd_arena,
                             Command *cmds, ptrdiff_t cap)
{
	arena_reset(cmd_arena);
	ptrdiff_t n = 0;
	for (int k = 0; k < FACTION_COUNT; k++) bot_think(&bots[k], sim, cmd_arena, cmds, &n, cap);
	sim_step(sim, cmds, n);
	return n;
}

static void test_determinism(void)
{
	Sim a, b;
	sim_init(&a, small_config(7));
	sim_init(&b, small_config(7));
	Bot ba[FACTION_COUNT], bb[FACTION_COUNT];
	for (int k = 0; k < FACTION_COUNT; k++) {
		ba[k] = bot_create((FactionId){(uint32_t)k}, 7);
		bb[k] = bot_create((FactionId){(uint32_t)k}, 7);
	}
	Arena ca = arena_create("cmd a", 1 << 20), cb = arena_create("cmd b", 1 << 20);
	Command cmda[64], cmdb[64];
	bool same = true;
	int64_t shots = 0;
	for (int t = 0; t < 600 && same; t++) {
		run_with_bots(&a, ba, &ca, cmda, countof(cmda));
		run_with_bots(&b, bb, &cb, cmdb, countof(cmdb));
		same = sim_hash_now(&a).v == sim_hash_now(&b).v;
	}
	shots = a.factions[0].stats.shots_fired + a.factions[1].stats.shots_fired;
	CHECK(same);
	CHECK(shots > 0); // the run reached combat, so the check covered it
	CHECK(a.factions[0].stats.units_lost + a.factions[1].stats.units_lost > 0);

	Sim c;
	sim_init(&c, small_config(8));
	sim_step(&c, nullptr, 0);
	sim_step(&a, nullptr, 0);
	CHECK(sim_hash_now(&c).v != sim_hash_now(&a).v);
	sim_destroy(&a);
	sim_destroy(&b);
	sim_destroy(&c);
	arena_destroy(&ca);
	arena_destroy(&cb);
}

static void test_replay(void)
{
	Sim sim;
	SimConfig config = small_config(3);
	sim_init(&sim, config);
	Bot bots[FACTION_COUNT];
	for (int k = 0; k < FACTION_COUNT; k++) bots[k] = bot_create((FactionId){(uint32_t)k}, 3);
	Arena cmd_arena = arena_create("cmds", 1 << 20);
	Arena file = arena_create("file", 16 << 20);
	ReplayWriter w;
	replay_begin(&w, &file, config);
	Command cmds[64];
	for (int t = 0; t < 300; t++) {
		arena_reset(&cmd_arena);
		ptrdiff_t n = 0;
		for (int k = 0; k < FACTION_COUNT; k++)
			bot_think(&bots[k], &sim, &cmd_arena, cmds, &n, countof(cmds));
		Tick tick = sim.tick;
		sim_step(&sim, cmds, n);
		replay_tick(&w, tick, cmds, n, sim_hash_now(&sim));
	}
	Hash final = sim_hash_now(&sim);
	sim_destroy(&sim);

	Str bytes = replay_bytes(&w);
	ReplayReader r;
	CHECK(replay_open(&r, bytes) == REPLAY_OK);
	CHECK(r.config.seed == 3 && r.config.initial_units == 300);
	Sim again;
	sim_init(&again, r.config);
	Arena decode = arena_create("decode", 4 << 20);
	int ticks = 0;
	bool match = true;
	ReplayStatus st;
	for (;;) {
		arena_reset(&decode);
		Tick tick;
		Command *rc;
		ptrdiff_t n;
		Hash want;
		st = replay_next(&r, &decode, &tick, &rc, &n, &want);
		if (st != REPLAY_OK) break;
		sim_step(&again, rc, n);
		match = match && sim_hash_now(&again).v == want.v;
		ticks++;
	}
	CHECK(st == REPLAY_END);
	CHECK(ticks == 300);
	CHECK(match);
	CHECK(sim_hash_now(&again).v == final.v);
	sim_destroy(&again);

	// Damaged files fail with a status, never an assert or a crash.
	CHECK(replay_open(&r, (Str){bytes.ptr, 6}) == REPLAY_TRUNCATED);
	char bad[64];
	memcpy(bad, bytes.ptr, sizeof bad);
	bad[0] = 'X';
	CHECK(replay_open(&r, (Str){bad, sizeof bad}) == REPLAY_BAD_MAGIC);
	CHECK(replay_open(&r, (Str){bytes.ptr, bytes.len - 3}) == REPLAY_OK);
	for (st = REPLAY_OK; st == REPLAY_OK;) {
		arena_reset(&decode);
		Tick tick;
		Command *rc;
		ptrdiff_t n;
		Hash want;
		st = replay_next(&r, &decode, &tick, &rc, &n, &want);
	}
	CHECK(st == REPLAY_TRUNCATED);
	arena_destroy(&decode);
	arena_destroy(&file);
	arena_destroy(&cmd_arena);
}

// A shell fired by faction 0 crosses to faction 1 as a launch, and faction 1
// flies it into its own unit.
static void test_shell_hits(void)
{
	SimConfig c = small_config(5);
	c.initial_units = 0;
	c.population = 0;
	Sim sim;
	sim_init(&sim, c);
	Faction *f1 = &sim.factions[1];
	const Terrain *t = &sim.terrain;
	float tx = 256.0f, tz = 256.0f;
	ptrdiff_t d = units_spawn(&f1->units, UNIT_TANK, tx, tz, terrain_height(t, tx, tz));
	UnitHandle target = units_handle(&f1->units, d);

	V3 from = v3(tx - 40.0f, terrain_height(t, tx - 40.0f, tz) + 2.0f, tz);
	V3 to = v3(tx, terrain_height(t, tx, tz) + unit_defs[UNIT_TANK].radius, tz);
	V3 vel;
	float time;
	CHECK(ballistic_solve(from, to, 42.0f, true, &vel, &time));
	Faction *f0 = &sim.factions[0];
	f0->outbox[f0->outbox_count++] = (Launch){from, vel, UNIT_TANK, f1->id};

	float hp = unit_defs[UNIT_TANK].hp;
	for (int k = 0; k < 400 && f1->units.count && f1->units.hp[units_find(&f1->units, target)] == hp; k++)
		sim_step(&sim, nullptr, 0);
	ptrdiff_t now = units_find(&f1->units, target);
	CHECK(now >= 0 && f1->units.hp[now] < hp);
	CHECK(f1->incoming.count == 0);
	sim_destroy(&sim);
}

int main(void)
{
	test_arena();
	test_units();
	test_ballistics();
	test_determinism();
	test_replay();
	test_shell_hits();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
