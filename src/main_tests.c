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
#include "world.c"

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
	ptrdiff_t d0 = units_spawn(&u, UNIT_RAIDER, (PlayerSlot){0}, 1, 1, 0);
	ptrdiff_t d1 = units_spawn(&u, UNIT_TANK, (PlayerSlot){0}, 2, 2, 0);
	ptrdiff_t d2 = units_spawn(&u, UNIT_ARTILLERY, (PlayerSlot){0}, 3, 3, 0);
	UnitHandle h0 = units_handle(&u, d0), h1 = units_handle(&u, d1), h2 = units_handle(&u, d2);
	CHECK(h0.index == 0 && h1.index == 1 && h2.index == 2);

	units_kill(&u, d1);
	CHECK(u.count == 2);
	CHECK(units_find(&u, h1) == -1);
	CHECK(units_find(&u, h0) == 0);
	ptrdiff_t moved = units_find(&u, h2);
	CHECK(moved == 1 && u.px[moved] == 3.0f && u.type[moved] == UNIT_ARTILLERY);

	// The freed slot comes back with a new generation; the old handle stays dead.
	ptrdiff_t d3 = units_spawn(&u, UNIT_RAIDER, (PlayerSlot){0}, 4, 4, 0);
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
	ptrdiff_t d = units_spawn(&f1->units, UNIT_TANK, (PlayerSlot){0}, tx, tz, terrain_height(t, tx, tz));
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
	CHECK(!s.bots_on[1][0] && s.config.initial_units == 0);

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
			for (int p = 0; p < MAX_PLAYERS; p++) {
				const Economy *e = &f->economy[p];
				bounded = bounded && e->metal >= 0.0f && e->metal <= e->metal_cap && e->energy >= 0.0f
				       && e->energy <= e->energy_cap && e->rate >= 0.0f && e->rate <= 1.0f;
			}
			owned[k] = f->spots;
			// Every owned spot has this faction's finished extractor on it.
			for (int32_t sp = 0; sp < server.spots.count; sp++) {
				if (!(f->spots >> sp & 1)) continue;
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
	Leg relay = {.route = route_push_front(0, 2)}, open = {.unpaired = true, .landing = v2(40.5f, 300.25f)};
	Transfer *t0 = ledger_depart(l, "alpha", 1, "beta", 0, &relay, tanks, &(Cargo){0});
	Transfer *t1 = ledger_depart(l, "alpha", 2, "beta", 0, &open, raiders, &(Cargo){250.5f, 1200.0f});
	CHECK(t0 && t1 && !t0->delivered && !t1->delivered);
	CHECK(ledger_depart(l, "alpha", 2, "beta", 0, &open, raiders, &(Cargo){0}) == nullptr); // re-sent: already had it
	ledger_delivered(l, t0->id);
	ledger_delivered(l, t0->id);                    // again: no second line
	CHECK(ledger_pending(l) == 1);
	fclose(log);

	// A fresh ledger from the log alone has the same transfers, legs and all.
	Str text;
	CHECK(platform_read_file(&a, path, &text));
	Ledger *again = PUSH(&a, Ledger, 1);
	int32_t bad;
	CHECK(ledger_replay(again, text, &bad));
	CHECK(again->transfer_count == 2 && ledger_pending(again) == 1 && again->next_id == 2);
	const Transfer *r0 = ledger_find(again, 0), *r1 = ledger_find(again, 1);
	CHECK(r0 && r0->delivered && !r0->leg.unpaired && r0->leg.route == relay.route && r0->counts[UNIT_TANK] == 10);
	CHECK(r1 && !r1->delivered && r1->leg.unpaired && r1->leg.landing.x == 40.5f && r1->leg.landing.z == 300.25f);
	CHECK(r1 && r1->cargo.metal == 250.5f && r1->cargo.energy == 1200.0f && r0 && r0->cargo.metal == 0.0f);
	CHECK(ledger_depart(again, "alpha", 2, "beta", 0, &open, raiders, &(Cargo){0}) == nullptr); // remembered across a restart
	int lines = 0;
	for (ptrdiff_t i = 0; i < text.len; i++) lines += text.ptr[i] == '\n';
	CHECK(lines == 3);

	// Compacted: the pending transfer, the ids and departure numbers; nothing else.
	CHECK(ledger_compact(again, path, &a));
	CHECK(again->transfer_count == 1 && again->lines == 0);
	CHECK(platform_read_file(&a, path, &text));
	Ledger *compact = PUSH(&a, Ledger, 1);
	CHECK(ledger_replay(compact, text, &bad));
	CHECK(compact->transfer_count == 1 && ledger_pending(compact) == 1 && compact->next_id == 2);
	const Transfer *c1 = ledger_find(compact, 1);
	CHECK(c1 && c1->leg.unpaired && c1->counts[UNIT_RAIDER] == 4);
	CHECK(ledger_depart(compact, "alpha", 2, "beta", 0, &open, raiders, &(Cargo){0}) == nullptr); // still a repeat
	Transfer *t3 = ledger_depart(compact, "alpha", 3, "beta", 0, &open, raiders, &(Cargo){0});
	CHECK(t3 && t3->id == 2);                       // ids carry on, never reused
	if (again->log) fclose(again->log);

	const char *broken[] = {"transfer 9 a b 7 paired 1 0 0,0 tank:1\n", "delivered 42\n",
	                        "transfer 1 a b 0 paired 1 0 0,0 nonsense:3\n", "explode 1\n", "transfer 1 a b 0 paired\n",
	                        "transfer 1 a b 0 sideways 1 0 0,0 tank:1\n", "transfer 1 a b 0 unpaired 1 3 0,0 tank:1\n",
	                        "transfer 1 a b 0 paired 1 0 nowhere tank:1\n", "draw 1 0 z tank:5\n"};
	for (int k = 0; k < (int)countof(broken); k++) {
		Ledger *x = PUSH(&a, Ledger, 1);
		CHECK(!ledger_replay(x, (Str){broken[k], (ptrdiff_t)strlen(broken[k])}, &bad));
	}
	remove(path);
	arena_destroy(&a);
}

// The strategic map through fog: a side's view keeps its own presence, its
// transits and its intel, and loses everything numeric about the enemy.
static void test_fog(void)
{
	Arena a = arena_create("fog", 1 << 20);
	Directory d = {.zone_count = 2, .link_count = 1, .links = {{0, 1}}, .transit_count = 2,
	               .transit = {{0, 1, 0, 12}, {1, 0, 1, 9}}};
	for (int k = 0; k < 2; k++) {
		DirectoryZone *z = &d.zones[k];
		snprintf(z->name, sizeof z->name, "%s", k ? "east" : "west");
		snprintf(z->host, sizeof z->host, "127.0.0.1");
		z->up = z->reported = true;
		z->x = 0.25f + 0.5f * (float)k;
		z->y = 0.5f;
		z->home = (int8_t)k;
		z->status = (ZoneStatus){.tick = 900, .units = {(uint16_t)(10 + k), (uint16_t)(20 + k)}, .structures = {3, 4},
		                         .transporters = {1, 1},
		                         .intel = {{.contact = k == 1, .seen_age = 40, .transporters = 1, .transporter_age = 90},
		                                   {.seen_age = INTEL_NEVER, .transporter_age = INTEL_NEVER}}};
	}
	ByteBuf b = bytebuf_make(&a, 4096);
	put_directory(&b, &d);
	ByteReader r = byte_reader(b.data, b.len);
	Directory back;
	CHECK(get_directory(&r, &back) && r.pos == r.len);
	CHECK(back.zones[1].status.intel[0].contact && back.zones[1].status.intel[0].transporter_age == 90);
	CHECK(back.zones[0].status.intel[1].seen_age == INTEL_NEVER);

	Directory blue = back;
	directory_fog(&blue, 0);
	CHECK(blue.zones[1].status.units[1] == 0 && blue.zones[1].status.structures[1] == 0
	      && blue.zones[1].status.transporters[1] == 0);                         // nothing of red's numbers
	CHECK(blue.zones[1].status.units[0] == 11 && blue.zones[1].status.transporters[0] == 1); // all of ours
	CHECK(blue.zones[1].status.intel[0].contact && blue.zones[1].status.intel[0].seen_age == 40); // what we saw
	CHECK(blue.zones[0].status.intel[1].seen_age == INTEL_NEVER);                 // nothing of what red saw
	CHECK(blue.transit_count == 1 && blue.transit[0].faction == 0);               // only our units in transit
	Directory all = back;
	directory_fog(&all, SPECTATOR);
	CHECK(all.zones[1].status.units[1] == 21 && all.transit_count == 2);          // a spectator sees it all

	// What a bot of blue's makes of it: contact, the age, the transporter.
	BotWorld w;
	bot_world_build(&w, &blue, 0, 0);
	bot_world_links(&w, &blue);
	CHECK(w.contact[1] && w.seen_age[1] == 40 && w.transporter_age[1] == 90 && w.linked[1]);
	CHECK(bot_believes_enemy(&w, 1) && w.neighbours_enemy[0]);
	arena_destroy(&a);
}

// Siege: fires at what's far (if something of ours sees it: it sees less
// than its own minimum range), never at what's close.
static void test_siege(void)
{
	SimConfig c = sim_config_default();
	c.initial_units = c.population = 0;
	Command cmds[3] = {
		{.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_SIEGE, .amount = 2, .target = {100, 100}, .radius = 0},
		{.faction = {1}, .kind = COMMAND_SPAWN, .unit_type = UNIT_TANK, .amount = 1, .target = {120, 100}, .radius = 0},
		{.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_SCOUT, .amount = 1, .target = {150, 100}, .radius = 0},
	};
	for (int far = 0; far < 2; far++) {
		Sim sim;
		sim_init(&sim, c);
		cmds[1].target = far ? v2(200, 100) : v2(120, 100);
		sim_step(&sim, cmds, 3);
		for (int t = 0; t < 400; t++) sim_step(&sim, nullptr, 0);
		int64_t fired = sim.factions[0].stats.shots_fired;
		CHECK(far ? fired > 0 : fired == 0);
		sim_destroy(&sim);
	}
}

// Doctrines: presets by name, any field over them, nonsense refused; scripts
// and world files carry them to the bots.
static void test_doctrine(void)
{
	char err[160];
	Doctrine d;
	CHECK(doctrine_parse(&d, "turtle", err, sizeof err) && d.entrench && d.mix[UNIT_SIEGE] == 1 && d.army_cap == 400);
	CHECK(doctrine_parse(&d, "rush army_cap=120 raider=3 siege=1 crew_builder=engineer hunt=off", err, sizeof err));
	CHECK(d.army_cap == 120 && d.mix[UNIT_SIEGE] == 1 && d.mix[UNIT_RAIDER] == 3 && d.crew_builder == UNIT_ENGINEER && !d.hunt);
	const char *bad[] = {"", "blitz", "rush army_cap", "rush army_cap=-3", "rush speed=9", "rush raider=0 tank=0",
	                     "rush hunt=maybe", "rush crew_builder=tank", "rush crew_builders=4 crew_escort=6",
	                     "turtle raider=3 tank=3"};
	for (int k = 0; k < (int)countof(bad); k++) CHECK(!doctrine_parse(&d, bad[k], err, sizeof err));

	Arena a = arena_create("doctrine", 16 << 20);
	const char *text = "scenario economy\ndoctrine red raider transporters=2\n";
	Script sc;
	CHECK(script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	CHECK(!strcmp(sc.doctrine[0].name, "balanced") && !strcmp(sc.doctrine[1].name, "raider")
	      && sc.doctrine[1].transporters == 2 && sc.doctrine[1].raids);
	ScriptRun run;
	script_run_init(&run, &sc, &a, nullptr);
	CHECK(run.bots[1][0].doctrine.transporters == 2 && !strcmp(run.bots[0][0].doctrine.name, "balanced"));
	const char *nonsense = "scenario economy\ndoctrine red blitz\n";
	CHECK(!script_parse(&sc, &a, (Str){nonsense, (ptrdiff_t)strlen(nonsense)}, "t.script", err, sizeof err));

	const char *wt = "zone west 0.2 0.5 home blue\nzone mid 0.5 0.5\nzone east 0.8 0.5 home red\n"
	                 "doctrine red turtle\ndoctrine red in mid siege garrison=5\n";
	World w;
	CHECK(world_parse(&w, (Str){wt, (ptrdiff_t)strlen(wt)}, "t.world", err, sizeof err));
	CHECK(!strcmp(world_doctrine(&w, 0, 1), "turtle") && !strcmp(world_doctrine(&w, 1, 1), "siege garrison=5")
	      && world_doctrine(&w, 2, 0)[0] == 0);
	arena_destroy(&a);
}

// Transporters under the faction split: sends, relays and arrivals are all
// faction state, so a client fed only its stream still matches.
//   - a paired send of tanks leaves after its 10 s charge, paying 30 a unit;
//   - an unpaired send takes only the raiders and engineers, 8 at most;
//   - a relay arrival goes on through the transporter it came in at;
//   - an unpaired arrival lands in our strip;
//   - a batch is at most PAIRED_BATCH: the rest goes next charge;
//   - one transporter is one queue: sends go in turn;
//   - (and entrenching and mobilizing, under the same split.)
static void test_transporters(void)
{
	const char *text =
		"scenario economy\n"
		"bases none\n"
		"bot blue off\n"
		"bot red off\n"
		"zones alpha beta gamma\n"
		"at 0 spawn blue transporter 1 at 100,256 radius 0\n"
		"at 0 spawn blue generator 40 at 40,150 radius 30\n"
		"at 0 spawn blue tank 6 at 140,256 radius 5\n"
		"at 0 spawn blue raider 10 at 140,300 radius 5\n"
		"at 0 spawn blue engineer 2 at 140,300 radius 3\n"
		"at 0 spawn blue tank 4 at 300,100 radius 3\n"
		"at 1 select blue tank within 15 of 140,256 as tanks\n"
		"at 1 select blue any within 15 of 140,300 as mixed\n"
		"at 1 select blue tank within 10 of 300,100 as guard\n"
		"at 3 entrench guard\n"
		"at 900 mobilize guard\n"
		"at 2 send tanks to beta\n"
		"at 700 select blue tank within 30 of 140,256 as tanks\n"
		"at 700 send mixed to beta at 60,300 unpaired\n"
		"at 701 send tanks to beta at 60,300 unpaired\n";
	Arena a = arena_create("transporters", 64 << 20);
	Arena tick = arena_create("transporters tick", 16 << 20);
	Script sc;
	char err[512];
	CHECK(script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	if (!sc.action_count) fprintf(stderr, "%s\n", err);
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
	Departure left[64];
	int64_t left_at[64];
	int left_count = 0;
	int32_t raiders_in_strip = 0, relayed = 0;
	Command cmds[64];
	for (int t = 0; t < 4600; t++) {
		arena_reset(&tick);
		ptrdiff_t n = script_tick(&run, &server, &tick, cmds, countof(cmds) - 3);
		// What the coordinator would bring: tanks to relay on to gamma, raiders
		// unpaired into our strip, and 70 raiders at once to go on to gamma.
		if (t == 2000)
			cmds[n++] = (Command){.faction = {0}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_TANK, .amount = 5,
			                      .target = v2(100, 256), .route = route_push_front(0, 2)};
		if (t == 2000)
			cmds[n++] = (Command){.faction = {0}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_RAIDER, .amount = 4,
			                      .target = v2(300, 200), .flags = COMMAND_UNPAIRED};
		if (t == 2600)
			cmds[n++] = (Command){.faction = {0}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_RAIDER, .amount = 70,
			                      .target = v2(100, 256), .route = route_push_front(0, 2)};
		sim_step(&server, cmds, n);
		const Faction *f = &server.factions[0];
		for (ptrdiff_t k = 0; k < f->departure_count && left_count < 64; k++) {
			left_at[left_count] = server.tick.v;
			left[left_count++] = f->departures[k];
		}
		if (t == 2001) {
			const Units *u = &f->units;
			for (ptrdiff_t i = 0; i < u->count; i++) {
				raiders_in_strip += u->type[i] == UNIT_RAIDER && u->order[i] != ORDER_SEND
				                    && u->px[i] < server.terrain.size_x * STRIP;
				relayed += u->type[i] == UNIT_TANK && u->order[i] == ORDER_SEND && u->dest[i] == 2;
			}
		}
		buf.len = 0;
		wire_encode_faction_tick(&server, (FactionId){0}, cmds, n, true, 1000, &buf);
		r = byte_reader(buf.data, buf.len);
		FactionTick ft;
		bool ok = wire_read_faction_tick(&r, &tick, &sc.config, (FactionId){0}, &ft);
		if (ok) faction_tick(&client, (FactionId){0}, &ft.effects, ft.commands, ft.command_count);
		matched = matched && ok && faction_hash(&client.factions[0]).v == ft.hash.v;
	}
	// 0: the six tanks, paired, after a 10 s charge (and the walk over).
	CHECK(left_count >= 6);
	CHECK(left[0].type == UNIT_TANK && left[0].count == 6 && !left[0].unpaired && left[0].dest == 1);
	CHECK(left_at[0] >= (int64_t)(PAIRED_CHARGE * SIM_HZ) && left_at[0] < 700);
	// Then the unpaired send: raiders and engineers only (the tanks were
	// turned away), 8 at most, landing where asked; the other 4 next charge.
	int32_t unpaired_units = 0, unpaired_tanks = 0, first_batch = 0;
	int64_t first_unpaired = -1;
	for (int k = 0; k < left_count; k++) {
		if (!left[k].unpaired) continue;
		if (first_unpaired < 0) first_unpaired = left_at[k];
		if (left_at[k] == first_unpaired) first_batch += left[k].count;
		unpaired_units += left[k].count;
		unpaired_tanks += left[k].type == UNIT_TANK ? left[k].count : 0;
		CHECK(left[k].landing.x == 60.0f && left[k].landing.z == 300.0f && left[k].route == 0);
	}
	CHECK(unpaired_tanks == 0 && unpaired_units == 12 && first_batch == UNPAIRED_BATCH);
	CHECK(first_unpaired >= 700 + (int64_t)(UNPAIRED_CHARGE * SIM_HZ));
	// The relay: the five tanks went on to gamma; unpaired raiders came in in our strip.
	CHECK(relayed == 5 && raiders_in_strip == 4);
	int32_t to_gamma = 0, biggest = 0;
	for (int k = 0; k < left_count; k++)
		if (left[k].dest == 2) {
			to_gamma += left[k].count;
			biggest = left[k].count > biggest ? left[k].count : biggest;
		}
	CHECK(to_gamma == 75 && biggest <= PAIRED_BATCH);
	CHECK(matched);
	// A zone with transporters (charged ones included) saves and loads.
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

// Entrenching: an order for some units, paid for as it goes and taking
// ENTRENCH_TIME, during which they hold still and don't fire; then turrets,
// which hold a line; mobilizing turns them back.
static void test_entrench(void)
{
	const char *text =
		"scenario economy\n"
		"bases none\n"
		"bot blue off\n"
		"bot red off\n"
		"at 0 spawn blue tank 10 at 200,256 radius 6\n"
		"at 0 spawn blue tank 3 at 120,256 radius 3\n"   // not ordered: they stay tanks
		"at 0 spawn red raider 20 at 236,256 radius 6\n"  // inside a turret's reach, outside a raider's
		"at 0 spawn blue generator 10 at 40,150 radius 10\n"  // entrenching ten takes all the starting energy
		"at 1 select blue tank within 12 of 200,256 as line\n"
		"at 2 entrench line\n"
		"at 120 expect blue turrets == 0\n"               // still digging in
		"at 200 expect blue turrets == 10\n"
		"at 200 expect blue tanks == 3\n"
		"at 700 expect red lost >= 10\n"
		"at 700 expect blue lost == 0\n"
		"at 701 select blue turret as dug\n"
		"at 702 mobilize dug\n"
		"at 760 expect blue turrets == 10\n"              // still packing up
		"at 830 expect blue turrets == 0\n"
		"at 830 expect blue tanks == 13\n";
	Arena a = arena_create("entrench", 32 << 20);
	Script sc;
	char err[512];
	CHECK(script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err));
	Sim sim;
	sim_init(&sim, sc.config);
	ScriptRun run;
	script_run_init(&run, &sc, &a, nullptr);
	Arena tick = arena_create("tick", 8 << 20);
	Command cmds[32];
	int64_t fired_at_start = -1, fired_digging = -1;
	float metal_before = 0.0f, metal_after = 0.0f;
	while (sim.tick.v < 840) {
		arena_reset(&tick);
		if (sim.tick.v == 2) metal_before = sim.factions[0].economy[0].metal; // before the order
		if (sim.tick.v == 3) fired_at_start = sim.factions[0].stats.shots_fired;
		if (sim.tick.v == 170) fired_digging = sim.factions[0].stats.shots_fired;
		if (sim.tick.v == 200) metal_after = sim.factions[0].economy[0].metal;
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, countof(cmds));
		sim_step(&sim, cmds, n);
	}
	if (run.failures) fprintf(stderr, "entrench: %d of %d expectations failed\n", run.failures, run.expects);
	CHECK(run.expects == 8 && run.failures == 0);
	CHECK(fired_digging == fired_at_start);     // silent while transforming
	// Paid: ten units' worth of metal, less what came in meanwhile.
	float income = (200.0f - 2.0f) * SIM_DT * 1.0f;
	float diff = metal_before + income - 10.0f * ENTRENCH_METAL - metal_after;
	CHECK(diff > -2.0f && diff < 2.0f);
	sim_destroy(&sim);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// Each player's economy: income to the earner, past storage to teammates;
// each pays for their own (builders, factories) and stalls alone; shares
// move only what the giver has and the taker has room for.
// (A player's store is set through held_*: what they have is rebuilt from it
// and their containers each tick.)
static void test_economies(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 1;              // blue: a factory and two engineers for each player
	c.players[0] = 2;
	Sim sim;
	sim_init(&sim, c);
	Faction *f = &sim.factions[0];
	Economy *e0 = &f->economy[0], *e1 = &f->economy[1];
	Units *u = &f->units;
	// Player 0 is full of metal: its income spills to player 1.
	e0->metal = e0->held_metal = e0->metal_cap;
	float before = e1->metal;
	sim_step(&sim, nullptr, 0);
	CHECK(e0->metal == e0->metal_cap);
	float base = 1.0f * SIM_DT;   // BASE_METAL_INCOME, a tick of it
	CHECK(e1->metal > before + 2.0f * base - 1e-3f && e1->metal < before + 2.0f * base + 1e-3f);
	// Shares: what's asked, up to what the giver has and the taker's room.
	Command share = share_command((FactionId){0}, (PlayerSlot){1}, (PlayerSlot){0}, 100.0f, 0.0f);
	before = e1->metal;
	sim_step(&sim, &share, 1);    // 0 is full: nothing moves (but its income spills)
	CHECK(e1->metal >= before);
	e0->metal = e0->held_metal = 0.0f;
	before = e1->metal;
	share = share_command((FactionId){0}, (PlayerSlot){1}, (PlayerSlot){0}, 100.0f, 1e9f);
	sim_step(&sim, &share, 1);
	CHECK(e0->metal > 99.0f && e0->metal < 101.0f && e1->metal < before - 99.0f);
	// All the energy 1 had, or as much as 0 had room for.
	CHECK(e0->energy <= e0->energy_cap && (e0->energy == e0->energy_cap || e1->energy < 2.0f));
	// Each player's factory makes raiders; player 0 is broke and stalls, 1 doesn't.
	ptrdiff_t fac[2] = {-1, -1};
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (u->type[i] == UNIT_FACTORY) fac[u->owner[i].v] = i;
	CHECK(fac[0] >= 0 && fac[1] >= 0);
	UnitHandle h[2] = {units_handle(u, fac[0]), units_handle(u, fac[1])};
	Command produce[2];
	for (int p = 0; p < 2; p++)
		produce[p] = (Command){.faction = {0}, .player = {(uint8_t)p}, .kind = COMMAND_PRODUCE, .count = 1,
		                       .units = &h[p], .unit_type = UNIT_RAIDER, .amount = 1};
	e0->metal = e0->held_metal = 0.0f;
	e1->metal = e1->held_metal = 500.0f;
	e0->energy = e1->energy = e0->held_energy = e1->held_energy = 3000.0f;   // power for both: only metal differs
	sim_step(&sim, produce, 2);
	for (int t = 0; t < 30; t++) sim_step(&sim, nullptr, 0);
	float work0 = u->work[units_find(u, h[0])], work1 = u->work[units_find(u, h[1])];
	CHECK(work1 > 0.04f);              // a second of a 4 s raider: about a quarter
	CHECK(work0 < 0.25f * work1);      // only player 0's trickle of metal
	CHECK(e0->rate < 0.5f && e1->rate == 1.0f);
	CHECK(e1->metal_spent > 0.0f);     // the factory's owner pays
	sim_destroy(&sim);
}

// Away players: their bots play them, a teammate who's here takes their
// units (BAR's /take), and a player coming back keeps what wasn't taken.
static void test_take(void)
{
	const char *text =
		"scenario economy\n"
		"players blue 3\n"
		"bot red off\n"
		"bot blue.0 off\n"                   // blue.0 and blue.2 are people; blue.1 is away, its bot playing
		"bot blue.2 off\n"
		"at 1 expect blue.1 units == 3\n"
		"at 2 take blue.1\n"                 // the away player can't take
		"at 3 expect blue.1 units == 3\n"
		"at 3 take blue.0\n"
		"at 4 expect blue.0 units == 6\n"    // its own three and blue.1's
		"at 4 expect blue.1 units == 0\n"
		"at 4 expect blue.2 units == 3\n"    // blue.2 is here: nothing of theirs is taken
		"at 5 spawn blue.2 tank 2 at 100,300\n"
		"at 6 bot blue.2 on\n"               // blue.2 leaves
		"at 8 bot blue.1 off\n"              // blue.1 comes back to nothing here
		"at 9 take blue.1\n"
		"at 10 expect blue.1 units == 5\n"
		"at 10 expect blue.2 units == 0\n";
	Arena a = arena_create("take", 32 << 20);
	Script sc;
	char err[512];
	bool parsed = script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err);
	if (!parsed) fprintf(stderr, "%s\n", err);
	CHECK(parsed);
	Sim sim;
	sim_init(&sim, sc.config);
	ScriptRun run;
	script_run_init(&run, &sc, &a, nullptr);
	Arena tick = arena_create("tick", 8 << 20);
	uint8_t away_at_1 = 0, away_at_8 = 0, away_at_10 = 0;  // after the step before that tick
	while (sim.tick.v < 11) {
		arena_reset(&tick);
		Command cmds[64];
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, countof(cmds));
		sim_step(&sim, cmds, n);
		if (sim.tick.v == 1) away_at_1 = sim.factions[0].away;
		if (sim.tick.v == 8) away_at_8 = sim.factions[0].away; // a change at 6 is recorded at 7
		if (sim.tick.v == 10) away_at_10 = sim.factions[0].away;
	}
	if (run.failures) fprintf(stderr, "take: %d of %d expectations failed\n", run.failures, run.expects);
	CHECK(run.expects == 7 && run.failures == 0);
	CHECK(away_at_1 == 2 && away_at_8 == 6 && away_at_10 == 4);
	CHECK(sim.factions[1].away == 0);
	sim_destroy(&sim);
	arena_destroy(&tick);
	arena_destroy(&a);
}

// Containers: storage that moves. Their room adds to their owner's, income
// past the held store fills them, spending empties them first (the held
// store stays full while it can); a dead one loses its cargo; dug in as a
// vault it keeps it; sent, its cargo leaves with it, and arriving, unloads
// into the owner's store there.
static void test_containers(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 0;
	c.players[0] = 2;
	Sim sim;
	sim_init(&sim, c);
	Faction *f = &sim.factions[0];
	Units *u = &f->units;
	Economy *e = &f->economy[0];
	Command spawn = {.faction = {0}, .player = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_CONTAINER, .amount = 2,
	                 .target = {100, 256}, .radius = 2};
	Command gen = {.faction = {0}, .player = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_TRANSPORTER, .amount = 1,
	               .target = {110, 256}, .radius = 0};
	Command both[2] = {spawn, gen};
	sim_step(&sim, both, 2);
	CHECK(e->metal_cap == 2000.0f + 2.0f * CONTAINER_METAL && e->energy_cap == 10000.0f + 2.0f * CONTAINER_ENERGY);
	// Full held store: income goes into the containers, not to the teammate.
	e->held_metal = 2000.0f;
	float teammate = f->economy[1].metal;
	for (int t = 0; t < 30; t++) sim_step(&sim, nullptr, 0);
	float cargo = 0.0f;
	ptrdiff_t box[2] = {-1, -1};
	for (ptrdiff_t i = 0, k = 0; i < u->count; i++)
		if (u->type[i] == UNIT_CONTAINER) {
			cargo += u->cargo_metal[i];
			box[k++] = i;
		}
	CHECK(cargo > 0.9f && cargo < 1.1f);                // a second of base income
	CHECK(f->economy[1].metal < teammate + 1.01f + 0.01f); // only the teammate's own trickle
	CHECK(e->metal > 2000.9f && e->held_metal == 2000.0f);
	// Spending empties the containers first: give 500 away.
	Command share = share_command((FactionId){0}, (PlayerSlot){0}, (PlayerSlot){1}, 500.0f, 0.0f);
	sim_step(&sim, &share, 1);
	CHECK(e->held_metal < 1600.0f && u->cargo_metal[box[0]] == 0.0f);
	// Fill the first container, then dig it in: a vault, cargo and all.
	e->held_metal = 2000.0f;
	u->cargo_metal[box[0]] = 800.0f;
	UnitHandle h0 = units_handle(u, box[0]), h1 = units_handle(u, box[1]);
	Command dig = {.faction = {0}, .player = {0}, .kind = COMMAND_ENTRENCH, .count = 1, .units = &h0};
	sim_step(&sim, &dig, 1);
	for (int t = 0; t < 200; t++) sim_step(&sim, nullptr, 0);
	ptrdiff_t v = units_find(u, h0);
	CHECK(v >= 0 && u->type[v] == UNIT_VAULT && u->cargo_metal[v] >= 750.0f); // less the digging in, paid from it
	// The other, filled and sent through the transporter: its cargo goes with it.
	ptrdiff_t b1 = units_find(u, h1);
	u->cargo_metal[b1] = 300.0f;
	u->cargo_energy[b1] = 2000.0f;
	e->held_energy = 10000.0f;
	Command send = {.faction = {0}, .player = {0}, .to = {1}, .kind = COMMAND_SEND, .amount = 1, .count = 1, .units = &h1};
	sim_step(&sim, &send, 1);
	float left_m = 0.0f, left_e = 0.0f;
	for (int t = 0; t < 600 && left_m == 0.0f; t++) {
		sim_step(&sim, nullptr, 0);
		for (ptrdiff_t k = 0; k < f->departure_count; k++)
			if (f->departures[k].type == UNIT_CONTAINER) {
				left_m += f->departures[k].metal;
				left_e += f->departures[k].energy;
				CHECK(player_eq(f->departures[k].owner, (PlayerSlot){1}));
			}
	}
	CHECK(left_m >= 299.0f && left_m <= 301.0f && left_e >= 1900.0f); // less the transporter's charge
	sim_step(&sim, nullptr, 0);                          // what a player has is counted as each tick starts
	CHECK(e->metal_cap == 2000.0f + CONTAINER_METAL);    // the vault's room only
	// Arriving: two containers for player 1, carrying 1200 metal between them.
	float before = f->economy[1].metal;
	Command arrive = {.faction = {0}, .player = {1}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_CONTAINER, .amount = 2,
	                  .target = {110, 256}, .metal = 1200.0f, .energy = 0.0f};
	sim_step(&sim, &arrive, 1);
	sim_step(&sim, nullptr, 0);
	CHECK(f->economy[1].metal > before + 1199.0f);
	// A vault destroyed: its cargo is gone with it.
	e->held_metal = HELD_METAL_CAP;                      // full: the vault can't unload
	sim_step(&sim, nullptr, 0);
	ptrdiff_t vault = units_find(u, h0);
	float had = e->metal, in_vault = u->cargo_metal[vault];
	u->hp[vault] = 0.0f;
	sim_step(&sim, nullptr, 0);                          // it dies
	sim_step(&sim, nullptr, 0);                          // and isn't counted
	CHECK(in_vault > 100.0f && e->metal < had - in_vault + 1.0f);
	sim_destroy(&sim);
}

// Players: every unit has an owner and only the owner's orders move it;
// giving, factory output and sends name who units go to; each player
// starts a base of their own; ownership survives a snapshot.
static void test_ownership(void)
{
	const char *text =
		"scenario economy\n"
		"players blue 3\n"
		"bot blue off\n"
		"bot red off\n"
		"at 0 spawn blue.1 tank 5 at 150,300 radius 4\n"
		"at 0 spawn blue.1 transporter 1 at 120,330 radius 0\n"
		"at 1 select blue.1 tank as b1\n"
		"at 1 select blue factory as plant\n"     // player 0's: blue is blue.0
		"at 2 give b1 to 2\n"
		"at 2 output plant to 1\n"
		"at 2 produce plant raider 2\n"
		"at 3 expect blue.1 tanks == 0\n"
		"at 3 expect blue.2 tanks == 5\n"
		"at 3 expect blue factories == 3\n"       // one each
		"at 3 expect blue.2 factories == 1\n"
		"at 700 expect blue.1 raiders == 2\n"     // made by player 0's factory, for player 1
		"at 700 expect blue.0 raiders == 0\n";
	Arena a = arena_create("ownership", 32 << 20);
	Script sc;
	char err[512];
	bool parsed = script_parse(&sc, &a, (Str){text, (ptrdiff_t)strlen(text)}, "t.script", err, sizeof err);
	if (!parsed) fprintf(stderr, "%s\n", err);
	CHECK(parsed);
	Sim sim;
	sim_init(&sim, sc.config);
	ScriptRun run;
	script_run_init(&run, &sc, &a, nullptr);
	Arena tick = arena_create("tick", 8 << 20);
	Units *u = &sim.factions[0].units;
	UnitHandle tanks[5];
	int ignored_ok = 0, obeyed_ok = 0, foreign_send_ok = 0, own_send_ok = 0, departed_for_2 = 0, arrived_for_2 = 0;
	while (sim.tick.v < 701) {
		arena_reset(&tick);
		Command cmds[40];
		ptrdiff_t n = script_tick(&run, &sim, &tick, cmds, 32);
		if (sim.tick.v == 10) {
			int k = 0;
			for (ptrdiff_t i = 0; i < u->count && k < 5; i++)
				if (u->type[i] == UNIT_TANK) tanks[k++] = units_handle(u, i);
			// Player 0 orders player 2's tanks: nothing happens.
			cmds[n++] = (Command){.faction = {0}, .player = {0}, .to = {0}, .kind = COMMAND_MOVE,
			                      .target = v2(250, 300), .count = 5, .units = tanks};
		}
		if (sim.tick.v == 12) {
			// Player 2 can; and player 0 can't send them through player 1's transporter.
			cmds[n++] = (Command){.faction = {0}, .player = {2}, .to = {2}, .kind = COMMAND_MOVE,
			                      .target = v2(150, 320), .count = 2, .units = tanks};
			cmds[n++] = (Command){.faction = {0}, .player = {0}, .to = {0}, .kind = COMMAND_SEND,
			                      .amount = 1, .count = 5, .units = tanks};
		}
		if (sim.tick.v == 14) {
			// Player 2 gives three back to player 1, who sends them to arrive for player 2.
			cmds[n++] = (Command){.faction = {0}, .player = {2}, .to = {1}, .kind = COMMAND_GIVE, .count = 3, .units = tanks + 2};
			cmds[n++] = (Command){.faction = {0}, .player = {1}, .to = {2}, .kind = COMMAND_SEND,
			                      .amount = 1, .count = 3, .units = tanks + 2};
		}
		if (sim.tick.v == 20) {
			cmds[n++] = (Command){.faction = {0}, .player = {2}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_SCOUT,
			                      .amount = 2, .target = v2(120, 330)};
		}
		sim_step(&sim, cmds, n);
		ptrdiff_t d0 = units_find(u, tanks[0]), d2 = units_find(u, tanks[2]);
		if (sim.tick.v == 11) ignored_ok = d0 >= 0 && u->order[d0] == ORDER_IDLE;
		if (sim.tick.v == 13) {
			obeyed_ok = d0 >= 0 && u->order[d0] == ORDER_MOVE;
			foreign_send_ok = d2 >= 0 && u->order[d2] != ORDER_SEND;
		}
		if (sim.tick.v == 15) own_send_ok = d2 >= 0 && u->order[d2] == ORDER_SEND && player_eq(u->output[d2], (PlayerSlot){2})
		                                    && player_eq(u->owner[d2], (PlayerSlot){1});
		for (ptrdiff_t k = 0; k < sim.factions[0].departure_count; k++) {
			const Departure *d = &sim.factions[0].departures[k];
			if (d->type == UNIT_TANK && player_eq(d->owner, (PlayerSlot){2})) departed_for_2 += d->count;
		}
		if (sim.tick.v == 21)
			for (ptrdiff_t i = 0; i < u->count; i++) arrived_for_2 += u->type[i] == UNIT_SCOUT && player_eq(u->owner[i], (PlayerSlot){2});
	}
	if (run.failures) fprintf(stderr, "ownership: %d of %d expectations failed\n", run.failures, run.expects);
	CHECK(run.expects == 6 && run.failures == 0);
	CHECK(ignored_ok && obeyed_ok);
	CHECK(foreign_send_ok && own_send_ok);
	CHECK(departed_for_2 == 3);
	CHECK(arrived_for_2 == 2);
	// Each player's base where sim_player_base says.
	int kits = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_FACTORY) continue;
		V2 at = sim_player_base(&sim, (FactionId){0}, u->owner[i]);
		kits += v2_len(v2_sub(v2(u->px[i], u->pz[i]), at)) < 12.0f;
	}
	CHECK(kits == 3);
	// Owners and outputs survive a snapshot, and are in the hash.
	ByteBuf b = bytebuf_make(&a, (ptrdiff_t)8 << 20);
	faction_write(&sim.factions[0], &b);
	Sim copy;
	sim_init(&copy, sc.config);
	ByteReader r = byte_reader(b.data, b.len);
	CHECK(faction_read(&copy.factions[0], &r));
	CHECK(faction_hash(&copy.factions[0]).v == faction_hash(&sim.factions[0]).v);
	Units *cu = &copy.factions[0].units;
	cu->owner[0].v ^= 1;
	CHECK(faction_hash(&copy.factions[0]).v != faction_hash(&sim.factions[0]).v);
	sim_destroy(&copy);
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
	{"transporters", test_transporters},
	{"fog", test_fog},
	{"doctrine", test_doctrine},
	{"siege", test_siege},
	{"snapshot", test_snapshot},
	{"entrench", test_entrench},
	{"ownership", test_ownership},
	{"economies", test_economies},
	{"take", test_take},
	{"containers", test_containers},
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
