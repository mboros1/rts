// Unit tests. Unity build, run under ASan and UBSan by `make check`.
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
#include "coord.c"

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

static bool parse_fails(const char *text, const char *needle)
{
	Arena a = arena_create("script test", 4 << 20);
	Script s;
	char err[512] = {0};
	bool ok = script_parse(&s, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err);
	bool matched = !ok && strstr(err, needle) != nullptr;
	if (!matched) fprintf(stderr, "  parse of [%s] gave: %s\n", text, ok ? "success" : err);
	arena_destroy(&a);
	return matched;
}

static void test_script(void)
{
	const char *text =
		"title Test  # comment\n"
		"scenario skirmish\n"
		"units 0\n"
		"bot red off\n"
		"bot blue off  # or blue's bot would send the idle units to attack\n"
		"at 5 expect blue units == 6\n"
		"at 0 spawn blue tank 6 at 100,100 radius 4\n"
		"at 1 select blue tank within 20 of 100,100 max 4 as g\n"
		"at 2 move g 140,100\n"
		"at 90 expect g alive == 4\n";
	Arena a = arena_create("script", 16 << 20);
	Script s;
	char err[512];
	CHECK(script_parse(&s, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	CHECK(!strcmp(s.title, "Test"));
	CHECK(s.action_count == 5 && s.group_count == 1);
	CHECK(s.actions[0].tick == 0 && s.actions[4].tick == 90); // sorted by tick
	CHECK(!s.bots_on[1] && s.config.initial_units == 0);

	Sim sim;
	sim_init(&sim, s.config);
	ScriptRun run;
	script_run_init(&run, &s, &a, nullptr);
	Arena tick = arena_create("tick", 4 << 20);
	Command cmds[64];
	while (sim.tick.v < s.end) {
		arena_reset(&tick);
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, countof(cmds));
		sim_step(&sim, cmds, n);
	}
	CHECK(run.expects == 2 && run.failures == 0);
	CHECK(sim.factions[0].units.count == 6);
	// The selected four moved toward 140,100; the other two stayed.
	ptrdiff_t moved = 0;
	for (ptrdiff_t i = 0; i < sim.factions[0].units.count; i++) moved += sim.factions[0].units.px[i] > 125.0f;
	CHECK(moved == 4);
	sim_destroy(&sim);
	arena_destroy(&tick);
	arena_destroy(&a);

	CHECK(parse_fails("at 0 explode blue\n", "t.script:1: unknown action"));
	CHECK(parse_fails("\nat 3 move nobody 1,1\n", "t.script:2: no group"));
	CHECK(parse_fails("at 0 spawn blue tank 99999 at 1,1\n", "amount"));
	CHECK(parse_fails("at 0 spawn green tank 1 at 1,1\n", "side"));
	CHECK(parse_fails("clip 9..3\n", "FROM < TO"));
	CHECK(parse_fails("at 0 expect blue lost > lots\n", "not a number"));
	CHECK(parse_fails("scenario nope\n", "unknown scenario"));
}


// The server's mirror and clients' mirrors must agree tick for tick, from the
// start or from a snapshot taken mid-game, and bad input must fail cleanly.
static void test_wire(void)
{
	Arena a = arena_create("wire", (ptrdiff_t)256 << 20);
	Arena tick = arena_create("wire tick", 16 << 20);
	SimConfig config = small_config(11);
	Sim sim;
	sim_init(&sim, config);
	Bot bots[FACTION_COUNT];
	for (int k = 0; k < FACTION_COUNT; k++) bots[k] = bot_create((FactionId){(uint32_t)k}, 11);
	Mirror server, early, late;
	mirror_init(&server, &a, config.max_units, config.max_projectiles);
	mirror_init(&early, &a, config.max_units, config.max_projectiles);
	mirror_init(&late, &a, config.max_units, config.max_projectiles);

	ByteBuf buf = bytebuf_make(&a, 8 << 20);
	ApplyInfo info;
	wire_encode_tick(&server, &sim, &buf, true, 1000, nullptr);
	buf.len = 0;
	wire_encode_snapshot(&server, &buf);
	ByteReader r = byte_reader(buf.data, buf.len);
	CHECK(wire_apply(&early, &r, true, &info) == APPLY_OK && info.digest_matched);

	bool all_ok = true, all_matched = true, late_joined = false;
	int64_t shells_seen = 0, bytes = 0;
	Command cmds[64];
	for (int t = 0; t < 800; t++) { // the armies meet around tick 600
		arena_reset(&tick);
		ptrdiff_t n = 0;
		for (int k = 0; k < FACTION_COUNT; k++) bot_think(&bots[k], &sim, &tick, cmds, &n, countof(cmds));
		sim_step(&sim, cmds, n);
		buf.len = 0;
		wire_encode_tick(&server, &sim, &buf, true, 1000, nullptr);
		bytes += buf.len;
		r = byte_reader(buf.data, buf.len);
		all_ok = all_ok && wire_apply(&early, &r, false, &info) == APPLY_OK;
		all_matched = all_matched && info.digest_matched;
		if (late_joined) {
			r = byte_reader(buf.data, buf.len);
			all_ok = all_ok && wire_apply(&late, &r, false, &info) == APPLY_OK;
			all_matched = all_matched && info.digest_matched;
		}
		if (t == 400) {
			// A second client joins: snapshot, then the same deltas.
			ByteBuf snap = bytebuf_make(&tick, 8 << 20);
			wire_encode_snapshot(&server, &snap);
			r = byte_reader(snap.data, snap.len);
			CHECK(wire_apply(&late, &r, true, &info) == APPLY_OK && info.digest_matched);
			late_joined = true;
		}
		shells_seen += server.shells[0].count + server.shells[1].count;

		if (t == 450) {
			// Replaying the same delta is a gap, not a silent double step.
			r = byte_reader(buf.data, buf.len);
			CHECK(wire_apply(&early, &r, false, &info) == APPLY_GAP);
		}
	}
	CHECK(all_ok);
	CHECK(all_matched);           // including every shell's position, bit for bit
	CHECK(shells_seen > 0);       // shells were in flight, so that was tested
	CHECK(bytes < 800 * 4096);    // dead reckoning keeps 600 units under 4 KB a tick

	// Corrupted snapshots: every outcome is OK or BAD, never a crash.
	buf.len = 0;
	wire_encode_snapshot(&server, &buf);
	uint8_t *copy = PUSH(&a, uint8_t, buf.len);
	Rng rng = rng_seed(99);
	int bad = 0;
	for (int k = 0; k < 300; k++) {
		memcpy(copy, buf.data, (size_t)buf.len);
		for (int flips = 0; flips < 4; flips++) copy[rng_below(&rng, (uint32_t)buf.len)] ^= (uint8_t)(1 + rng_below(&rng, 255));
		ptrdiff_t len = k % 3 == 0 ? (ptrdiff_t)rng_below(&rng, (uint32_t)buf.len) : buf.len;
		r = byte_reader(copy, len);
		ApplyResult res = wire_apply(&late, &r, true, &info);
		bad += res == APPLY_BAD || (res == APPLY_OK && !info.digest_matched);
	}
	CHECK(bad > 250); // nearly every corruption is caught, by validation or the digest

	sim_destroy(&sim);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// What each faction could see, from current positions: the set its
// perception must equal after the next step's sensing.
static void visible_set(Sim *sim, FactionId f, Arena *a, uint8_t *out)
{
	const Faction *me = &sim->factions[f.v];
	const Units *enemy = &sim->factions[faction_enemy(f).v].units;
	Grid grid;
	grid_build(&grid, a, &sim->terrain, me->units.px, me->units.pz, me->units.count);
	for (ptrdiff_t s = 0; s < enemy->cap; s++) {
		int32_t d = enemy->slot_dense[s];
		out[s] = d >= 0 && in_sight(&me->units, &grid, enemy->px[d], enemy->pz[d]);
	}
}

// Spike 3's claim: a client fed only its faction's commands and effects
// reproduces that faction exactly, from the start or from a mid-game
// snapshot, and perceives exactly the enemies in sight, no more.
static void test_split(void)
{
	Arena a = arena_create("split", (ptrdiff_t)256 << 20);
	Arena tick = arena_create("split tick", 64 << 20);
	SimConfig config = small_config(13);
	Sim server, clients[FACTION_COUNT], late;
	sim_init(&server, config);
	Bot bots[FACTION_COUNT];
	for (int k = 0; k < FACTION_COUNT; k++) bots[k] = bot_create((FactionId){(uint32_t)k}, 13);

	ByteBuf buf = bytebuf_make(&a, 8 << 20);
	for (int k = 0; k < FACTION_COUNT; k++) {
		sim_init(&clients[k], config);
		buf.len = 0;
		wire_encode_faction_snapshot(&server, (FactionId){(uint32_t)k}, &buf);
		ByteReader r = byte_reader(buf.data, buf.len);
		clients[k].tick.v = (int64_t)get64(&r);
		CHECK(faction_read(&clients[k].factions[k], &r) && r.pos == r.len);
	}
	CHECK(faction_hash(&clients[0].factions[0]).v == faction_hash(&server.factions[0]).v);

	bool decoded = true, matched = true, exact_fog = true, late_ok = true, joined = false;
	int64_t seen_max = 0, arrivals = 0, bytes = 0;
	Command cmds[64];
	for (int t = 0; t < 800; t++) {
		arena_reset(&tick);
		ptrdiff_t n = 0;
		for (int k = 0; k < FACTION_COUNT; k++) bot_think(&bots[k], &server, &tick, cmds, &n, countof(cmds));
		sim_step(&server, cmds, n);
		for (int k = 0; k < FACTION_COUNT; k++) {
			FactionId f = {(uint32_t)k};
			buf.len = 0;
			wire_encode_faction_tick(&server, f, cmds, n, true, 1000, &buf);
			bytes += buf.len;
			for (int who = 0; who < (k == 1 && joined ? 2 : 1); who++) {
				Sim *c = who ? &late : &clients[k];
				ByteReader r = byte_reader(buf.data, buf.len);
				FactionTick ft;
				bool ok = wire_read_faction_tick(&r, &tick, &config, f, &ft) && ft.tick == c->tick.v;
				decoded = decoded && ok;
				if (!ok) continue;
				faction_tick(c, f, &ft.effects, ft.commands, ft.command_count);
				bool same = faction_hash(&c->factions[k]).v == ft.hash.v;
				if (who) late_ok = late_ok && same;
				else matched = matched && same;
			}
			seen_max = server.effects[k].seen_count > seen_max ? server.effects[k].seen_count : seen_max;
			arrivals += server.effects[k].arrived_count;
		}
		if (t == 400) {
			sim_init(&late, config);
			buf.len = 0;
			wire_encode_faction_snapshot(&server, (FactionId){1}, &buf);
			ByteReader r = byte_reader(buf.data, buf.len);
			late.tick.v = (int64_t)get64(&r);
			CHECK(faction_read(&late.factions[1], &r));
			joined = true;
		}
	}
	CHECK(decoded);
	CHECK(matched);
	CHECK(late_ok);
	CHECK(seen_max > 0 && arrivals > 0); // enemies were seen and shells arrived

	// Exact fog, checked directly: sense from the current positions and
	// compare the perception the step produces with what was visible.
	for (int round = 0; round < 3; round++) {
		uint8_t *vis[FACTION_COUNT];
		for (int k = 0; k < FACTION_COUNT; k++) {
			vis[k] = PUSH(&a, uint8_t, config.max_units);
			visible_set(&server, (FactionId){(uint32_t)k}, &tick, vis[k]);
		}
		sim_step(&server, nullptr, 0);
		for (int k = 0; k < FACTION_COUNT; k++)
			for (ptrdiff_t sl = 0; sl < config.max_units; sl++)
				exact_fog = exact_fog && server.factions[k].seen.live[sl] == vis[k][sl];
	}
	CHECK(exact_fog);
	CHECK(bytes < 800 * 2 * 4096); // the faction stream stays small at 600 units

	// Corrupted snapshots are rejected or loaded, never crash.
	buf.len = 0;
	wire_encode_faction_snapshot(&server, (FactionId){0}, &buf);
	uint8_t *copy = PUSH(&a, uint8_t, buf.len);
	Rng rng = rng_seed(7);
	int rejected = 0;
	for (int k = 0; k < 200; k++) {
		memcpy(copy, buf.data, (size_t)buf.len);
		for (int flips = 0; flips < 3; flips++) copy[8 + rng_below(&rng, (uint32_t)(buf.len - 8))] ^= 0x5a;
		ByteReader r = byte_reader(copy + 8, k % 4 == 0 ? (ptrdiff_t)rng_below(&rng, (uint32_t)(buf.len - 8)) : buf.len - 8);
		rejected += !faction_read(&late.factions[0], &r);
	}
	CHECK(rejected > 0);

	for (int k = 0; k < FACTION_COUNT; k++) sim_destroy(&clients[k]);
	sim_destroy(&late);
	sim_destroy(&server);
	arena_destroy(&tick);
	arena_destroy(&a);
}


// An economy game between bots: resources stay in bounds, spots are never
// owned twice, both sides expand and produce, and the faction split still
// holds with everything the economy adds to a faction's state.
static void test_economy(void)
{
	Arena a = arena_create("economy", (ptrdiff_t)256 << 20);
	Arena tick = arena_create("economy tick", 64 << 20);
	const Scenario *sc = scenario_find("economy");
	SimConfig config = scenario_config(sc);
	Sim server, clients[FACTION_COUNT];
	sim_init(&server, config);
	Bot bots[FACTION_COUNT];
	scenario_bots(sc, bots);
	ByteBuf buf = bytebuf_make(&a, 8 << 20);
	for (int k = 0; k < FACTION_COUNT; k++) {
		sim_init(&clients[k], config);
		buf.len = 0;
		wire_encode_faction_snapshot(&server, (FactionId){(uint32_t)k}, &buf);
		ByteReader r = byte_reader(buf.data, buf.len);
		clients[k].tick.v = (int64_t)get64(&r);
		CHECK(faction_read(&clients[k].factions[k], &r));
	}

	bool bounded = true, exclusive = true, earned = true, split = true;
	Command cmds[256];
	for (int t = 0; t < 3000; t++) { // 100 s: both sides have expanded and fielded units
		arena_reset(&tick);
		ptrdiff_t n = 0;
		for (int k = 0; k < FACTION_COUNT; k++) bot_think(&bots[k], &server, &tick, cmds, &n, countof(cmds));
		sim_step(&server, cmds, n);
		uint64_t owned[FACTION_COUNT];
		for (int k = 0; k < FACTION_COUNT; k++) {
			const Faction *f = &server.factions[k];
			const Economy *e = &f->economy;
			bounded = bounded && e->metal >= 0.0f && e->metal <= e->metal_cap && e->energy >= 0.0f
			       && e->energy <= e->energy_cap && e->rate >= 0.0f && e->rate <= 1.0f;
			owned[k] = e->spots;
			// Every owned spot has this faction's finished extractor on it.
			for (int32_t sp = 0; sp < server.spots.count; sp++) {
				if (!(e->spots >> sp & 1)) continue;
				bool found = false;
				for (ptrdiff_t i = 0; i < f->units.count && !found; i++)
					found = f->units.type[i] == UNIT_EXTRACTOR && f->units.progress[i] >= 1.0f
					     && spot_at(&server.spots, v2(f->units.px[i], f->units.pz[i])) == sp;
				earned = earned && found;
			}
			FactionId fid = {(uint32_t)k};
			buf.len = 0;
			wire_encode_faction_tick(&server, fid, cmds, n, true, 1000, &buf);
			ByteReader r = byte_reader(buf.data, buf.len);
			FactionTick ft;
			bool ok = wire_read_faction_tick(&r, &tick, &config, fid, &ft);
			if (ok) faction_tick(&clients[k], fid, &ft.effects, ft.commands, ft.command_count);
			split = split && ok && faction_hash(&clients[k].factions[k]).v == ft.hash.v;
		}
		exclusive = exclusive && (owned[0] & owned[1]) == 0;
	}
	CHECK(bounded);
	CHECK(exclusive);
	CHECK(earned);
	CHECK(split);
	for (int k = 0; k < FACTION_COUNT; k++) {
		const Units *u = &server.factions[k].units;
		int extractors = 0, factories = 0, combat = 0;
		for (ptrdiff_t i = 0; i < u->count; i++) {
			extractors += u->type[i] == UNIT_EXTRACTOR && u->progress[i] >= 1.0f;
			factories += u->type[i] == UNIT_FACTORY && u->progress[i] >= 1.0f;
			combat += unit_is_combat(u->type[i]);
		}
		CHECK(extractors >= 5);  // both sides expanded
		CHECK(factories >= 1);
		CHECK(combat + server.factions[k].stats.units_lost >= 6); // and fielded units (observed 8 and 10)
	}
	for (int k = 0; k < FACTION_COUNT; k++) sim_destroy(&clients[k]);
	sim_destroy(&server);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// A player's economy, scripted: engineers take a queue of builds in turn and
// a factory produces on repeat. No bots: only the orders a player gives.
static void test_build_orders(void)
{
	const char *text =
		"scenario economy\n"
		"bot blue off\n"
		"bot red off\n"
		"at 1 select blue engineer as crew\n"
		"at 1 select blue factory as plant\n"
		"at 2 queue crew extractor at 46,229\n"
		"at 2 queue crew extractor at 92,226\n"
		"at 2 queue crew generator at 36,280\n"
		"at 2 produce plant raider 2 repeat\n"
		"at 1800 expect blue extractors == 2\n"
		"at 1800 expect blue generators == 1\n"
		"at 1800 expect blue raiders >= 4\n"
		"at 1800 expect red spawned == 3\n"; // red did nothing
	Arena a = arena_create("build orders", 64 << 20);
	Script s;
	char err[512];
	CHECK(script_parse(&s, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	Sim sim;
	sim_init(&sim, s.config);
	ScriptRun run;
	script_run_init(&run, &s, &a, stderr);
	Arena tick = arena_create("tick", 16 << 20);
	Command cmds[64];
	while (sim.tick.v < s.end) {
		arena_reset(&tick);
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, countof(cmds));
		sim_step(&sim, cmds, n);
	}
	CHECK(run.expects == 4 && run.failures == 0);
	sim_destroy(&sim);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// The coordinator's memory: what it logs, replayed, is what it had.
static void test_ledger(void)
{
	Arena a = arena_create("ledger", 16 << 20);
	char path[] = "build/test-ledger.log";
	FILE *log = fopen(path, "w");
	CHECK(log != nullptr);
	if (!log) return;
	Ledger *l = PUSH(&a, Ledger, 1);
	l->log = log;
	int32_t tanks[UNIT_TYPE_COUNT] = {[UNIT_TANK] = 10}, raiders[UNIT_TYPE_COUNT] = {[UNIT_RAIDER] = 4};
	Transfer *t0 = ledger_depart(l, "alpha", 1, "beta", 0, false, tanks);
	Transfer *t1 = ledger_depart(l, "alpha", 2, "pool", 0, false, raiders);
	CHECK(ledger_depart(l, "alpha", 2, "pool", 0, false, raiders) == nullptr); // re-sent: already had it
	CHECK(!t0->delivered && t1->delivered);        // the pool takes them at once
	CHECK(l->pool[0][UNIT_RAIDER] == 4);
	int32_t want[UNIT_TYPE_COUNT] = {[UNIT_RAIDER] = 6, [UNIT_TANK] = 1};
	Transfer *t2 = ledger_draw(l, 0, "gamma", want);  // asks for more than there is
	CHECK(t2 && t2->counts[UNIT_RAIDER] == 4 && t2->counts[UNIT_TANK] == 0);
	CHECK(l->pool[0][UNIT_RAIDER] == 0);
	CHECK(ledger_draw(l, 0, "gamma", want) == nullptr); // nothing left
	ledger_delivered(l, t0->id);
	ledger_delivered(l, t0->id);                    // again: no second line
	CHECK(ledger_pending(l) == 1);                  // the draw is still owed
	fclose(log);

	// A fresh ledger from the log alone has the same transfers and pool.
	Str text;
	CHECK(platform_read_file(&a, path, &text));
	Ledger *again = PUSH(&a, Ledger, 1);
	int32_t bad;
	CHECK(ledger_replay(again, text, &bad));
	CHECK(again->transfer_count == 3 && ledger_pending(again) == 1);
	CHECK(again->pool[0][UNIT_RAIDER] == 0 && again->next_id == 3);
	CHECK(ledger_find(again, 0)->delivered && !ledger_find(again, 2)->delivered);
	CHECK(ledger_depart(again, "alpha", 2, "pool", 0, false, raiders) == nullptr); // remembered across a restart
	int lines = 0;
	for (ptrdiff_t i = 0; i < text.len; i++) lines += text.ptr[i] == '\n';
	CHECK(lines == 4);

	const char *broken[] = {"transfer 9 a b 7 units 1 tank:1\n", "delivered 42\n", "draw 1 0 z tank:5\n",
	                        "transfer 1 a b 0 units 1 nonsense:3\n", "explode 1\n", "transfer 1 a b 0 units\n"};
	for (int k = 0; k < (int)countof(broken); k++) {
		Ledger *x = PUSH(&a, Ledger, 1);
		CHECK(!ledger_replay(x, (Str){broken[k], (ptrdiff_t)strlen(broken[k])}, &bad));
	}
	remove(path);
	arena_destroy(&a);
}

// Gates under the faction split: departures, arrivals and a seed unfolding
// are all faction state, so clients fed only their streams still match.
static void test_gates(void)
{
	const char *text =
		"scenario economy\n"
		"bot blue off\n"
		"bot red off\n"
		"zones alpha beta\n"
		"at 0 spawn blue gate 1 at 100,256 radius 0\n"
		"at 0 spawn blue tank 6 at 140,256 radius 5\n"
		"at 1 select blue tank as army\n"
		"at 2 gate army to beta via 100,256\n";
	Arena a = arena_create("gates", 64 << 20);
	Arena tick = arena_create("gates tick", 16 << 20);
	Script sc;
	char err[512];
	CHECK(script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	Sim server, client;
	sim_init(&server, sc.config);
	sim_init(&client, sc.config);
	ByteBuf buf = bytebuf_make(&a, 4 << 20);
	wire_encode_faction_snapshot(&server, (FactionId){0}, &buf);
	ByteReader r = byte_reader(buf.data, buf.len);
	client.tick.v = (int64_t)get64(&r);
	CHECK(faction_read(&client.factions[0], &r));
	ScriptRun run;
	script_run_init(&run, &sc, &a, nullptr);
	bool matched = true;
	int64_t departed = 0;
	Command cmds[64];
	for (int t = 0; t < 1500; t++) {
		arena_reset(&tick);
		ptrdiff_t n = script_tick(&run, &server, &tick, cmds, countof(cmds) - 2);
		// What the coordinator would do: send the tanks back, and a seed.
		if (t == 600) cmds[n++] = (Command){.faction = {0}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_RAIDER, .amount = 3};
		if (t == 650) cmds[n++] = (Command){.faction = {0}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_GATE, .amount = 1,
		                                    .flags = COMMAND_SEED_GATE, .target = v2(100, 360)};
		sim_step(&server, cmds, n);
		for (ptrdiff_t k = 0; k < server.factions[0].departure_count; k++) departed += server.factions[0].departures[k].count;
		buf.len = 0;
		wire_encode_faction_tick(&server, (FactionId){0}, cmds, n, true, 1000, &buf);
		r = byte_reader(buf.data, buf.len);
		FactionTick ft;
		bool ok = wire_read_faction_tick(&r, &tick, &sc.config, (FactionId){0}, &ft);
		if (ok) faction_tick(&client, (FactionId){0}, &ft.effects, ft.commands, ft.command_count);
		matched = matched && ok && faction_hash(&client.factions[0]).v == ft.hash.v;
	}
	const Units *u = &server.factions[0].units;
	int gates = 0, raiders = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		gates += u->type[i] == UNIT_GATE && u->progress[i] >= 1.0f;
		raiders += u->type[i] == UNIT_RAIDER;
	}
	CHECK(departed == 6);                    // all six tanks left, as one departure
	CHECK(server.factions[0].stats.units_arrived == 3 && raiders == 3);
	CHECK(gates == 2);                       // the seed unfolded beside the first
	CHECK(matched);
	// A zone with gates (charged ones included) saves and loads.
	ByteBuf snap = bytebuf_make(&a, 4 << 20);
	sim_save(&server, &snap);
	Sim loaded;
	sim_init(&loaded, sc.config);
	ByteReader sr = byte_reader(snap.data, snap.len);
	CHECK(sim_load(&loaded, &sr) && sim_hash_now(&loaded).v == sim_hash_now(&server).v);
	sim_destroy(&loaded);
	sim_destroy(&server);
	sim_destroy(&client);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// A zone saved and resumed is the same zone: a loaded sim steps exactly as
// the original, a replay segment from the snapshot verifies, and a damaged
// snapshot is refused.
static void test_snapshot(void)
{
	Arena a = arena_create("snapshot", (ptrdiff_t)128 << 20);
	Arena tick = arena_create("snapshot tick", 16 << 20);
	SimConfig config = small_config(21);
	Sim original, resumed;
	sim_init(&original, config);
	Bot bots[FACTION_COUNT];
	for (int k = 0; k < FACTION_COUNT; k++) bots[k] = bot_create((FactionId){(uint32_t)k}, 21);
	Command cmds[64];
	for (int t = 0; t < 650; t++) { // into the fighting: shells in flight, units dead
		arena_reset(&tick);
		ptrdiff_t n = 0;
		for (int k = 0; k < FACTION_COUNT; k++) bot_think(&bots[k], &original, &tick, cmds, &n, countof(cmds));
		sim_step(&original, cmds, n);
	}
	ByteBuf snap = bytebuf_make(&a, 8 << 20);
	sim_save(&original, &snap);
	sim_init(&resumed, config);
	ByteReader r = byte_reader(snap.data, snap.len);
	CHECK(sim_load(&resumed, &r) && r.pos == r.len);
	CHECK(sim_hash_now(&resumed).v == sim_hash_now(&original).v);

	// Record the resumed run as a segment while checking it keeps step.
	Arena file = arena_create("segment", 16 << 20);
	ReplayWriter w;
	replay_begin_from(&w, &file, config, (Str){(const char *)snap.data, snap.len});
	bool same = true;
	for (int t = 0; t < 300; t++) {
		arena_reset(&tick);
		ptrdiff_t n = 0;
		for (int k = 0; k < FACTION_COUNT; k++) bot_think(&bots[k], &original, &tick, cmds, &n, countof(cmds));
		Tick at = original.tick;
		sim_step(&original, cmds, n);
		sim_step(&resumed, cmds, n);
		replay_tick(&w, at, cmds, n, sim_hash_now(&resumed));
		same = same && sim_hash_now(&resumed).v == sim_hash_now(&original).v;
	}
	CHECK(same);
	CHECK(original.factions[0].stats.units_lost + original.factions[1].stats.units_lost > 0);

	ReplayReader rr;
	CHECK(replay_open(&rr, replay_bytes(&w)) == REPLAY_OK && rr.snapshot.len == snap.len);
	Sim again;
	CHECK(replay_start(&rr, &again));
	Arena decode = arena_create("decode", 4 << 20);
	bool verified = true;
	int ticks = 0;
	for (;;) {
		arena_reset(&decode);
		Tick t;
		Command *rc;
		ptrdiff_t n;
		Hash want;
		if (replay_next(&rr, &decode, &t, &rc, &n, &want) != REPLAY_OK) break;
		verified = verified && t.v == again.tick.v;
		sim_step(&again, rc, n);
		verified = verified && sim_hash_now(&again).v == want.v;
		ticks++;
	}
	CHECK(verified && ticks == 300);

	// Damaged snapshots: refused or loaded, never a crash.
	uint8_t *copy = PUSH(&a, uint8_t, snap.len);
	Rng rng = rng_seed(3);
	int refused = 0;
	for (int k = 0; k < 120; k++) {
		memcpy(copy, snap.data, (size_t)snap.len);
		for (int f = 0; f < 3; f++) copy[rng_below(&rng, (uint32_t)snap.len)] ^= 0x33;
		Sim scratch;
		sim_init(&scratch, config);
		ByteReader cr = byte_reader(copy, k % 3 ? snap.len : (ptrdiff_t)rng_below(&rng, (uint32_t)snap.len));
		refused += !sim_load(&scratch, &cr);
		sim_destroy(&scratch);
	}
	CHECK(refused > 0);
	sim_destroy(&again);
	sim_destroy(&original);
	sim_destroy(&resumed);
	arena_destroy(&decode);
	arena_destroy(&file);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// Logging out and in: units entrench as turrets, which hold a line, and
// mobilize back into what they were.
static void test_entrench(void)
{
	const char *text =
		"scenario skirmish\n"
		"units 0\n"
		"bot blue off\n"
		"bot red off\n"
		"at 0 spawn blue tank 10 at 200,256 radius 6\n"
		"at 0 spawn red raider 20 at 236,256 radius 6\n"  // inside a turret's reach, outside a raider's
		"at 1 entrench blue\n"
		"at 2 expect blue turrets == 10\n"
		"at 2 expect blue tanks == 0\n"
		"at 600 expect red lost >= 10\n"
		"at 600 expect blue lost == 0\n"
		"at 601 mobilize blue\n"
		"at 602 expect blue turrets == 0\n"
		"at 602 expect blue tanks == 10\n";
	Arena a = arena_create("entrench", 32 << 20);
	Script sc;
	char err[512];
	CHECK(script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	Sim sim;
	sim_init(&sim, sc.config);
	ScriptRun run;
	script_run_init(&run, &sc, &a, stderr);
	Arena tick = arena_create("tick", 8 << 20);
	Command cmds[32];
	while (sim.tick.v < 610) {
		arena_reset(&tick);
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, countof(cmds));
		sim_step(&sim, cmds, n);
	}
	CHECK(run.expects == 6 && run.failures == 0);
	sim_destroy(&sim);
	arena_destroy(&tick);
	arena_destroy(&a);
}

typedef struct {
	const char *name;
	void (*run)(void);
} Test;

static const Test tests[] = {
	{"arena", test_arena},
	{"units", test_units},
	{"ballistics", test_ballistics},
	{"determinism", test_determinism},
	{"replay", test_replay},
	{"shell_hits", test_shell_hits},
	{"script", test_script},
	{"wire", test_wire},
	{"split", test_split},
	{"economy", test_economy},
	{"build_orders", test_build_orders},
	{"ledger", test_ledger},
	{"gates", test_gates},
	{"snapshot", test_snapshot},
	{"entrench", test_entrench},
};

// tests            run all, in order
// tests --list     names, one per line (make check runs them in parallel)
// tests NAME...    run those
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--list")) {
		for (ptrdiff_t i = 0; i < countof(tests); i++) printf("%s\n", tests[i].name);
		return 0;
	}
	for (ptrdiff_t i = 0; i < countof(tests); i++) {
		bool wanted = argc == 1;
		for (int k = 1; k < argc; k++) wanted = wanted || !strcmp(argv[k], tests[i].name);
		if (wanted) tests[i].run();
	}
	if (argc == 2) printf("%-12s ", argv[1]);
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
