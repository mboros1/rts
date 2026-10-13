// Unit tests. Unity build, run under ASan and UBSan by `make check`.
#include "base.h"

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"
#include "scenario.c"
#include "context.c"
#include "script.c"
#include "wire.c"
#include "menu.c"
#include "keymap.c"
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
	mirror_init(&server, &a, config.max_units, config.max_projectiles, config.context.size);
	mirror_init(&early, &a, config.max_units, config.max_projectiles, config.context.size);
	mirror_init(&late, &a, config.max_units, config.max_projectiles, config.context.size);

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
		SpotSet owned[FACTION_COUNT];
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
				if (!spotset_has(&f->spots, sp)) continue;
				bool found = false;
				for (ptrdiff_t i = 0; i < f->units.count && !found; i++)
					found = unit_is_extractor(f->units.type[i]) && f->units.progress[i] >= 1.0f
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
		for (int w = 0; w < MAX_SPOTS / 64; w++) exclusive = exclusive && (owned[0].w[w] & owned[1].w[w]) == 0;
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
	ledger_end(l, 1, "beta");
	ledger_end(l, 0, "alpha");                      // the first word stands: no line
	CHECK(l->ended == 2 && !strcmp(l->ended_in, "beta"));
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
	CHECK(lines == 4 && again->ended == 2 && !strcmp(again->ended_in, "beta"));

	// Compacted: the pending transfer, the ids and departure numbers; nothing else.
	CHECK(ledger_compact(again, path, &a));
	CHECK(again->transfer_count == 1 && again->lines == 0);
	CHECK(platform_read_file(&a, path, &text));
	Ledger *compact = PUSH(&a, Ledger, 1);
	CHECK(ledger_replay(compact, text, &bad));
	CHECK(compact->transfer_count == 1 && ledger_pending(compact) == 1 && compact->next_id == 2);
	CHECK(compact->ended == 2 && !strcmp(compact->ended_in, "beta"));
	const Transfer *c1 = ledger_find(compact, 1);
	CHECK(c1 && c1->leg.unpaired && c1->counts[UNIT_RAIDER] == 4);
	CHECK(ledger_depart(compact, "alpha", 2, "beta", 0, &open, raiders, &(Cargo){0}) == nullptr); // still a repeat
	Transfer *t3 = ledger_depart(compact, "alpha", 3, "beta", 0, &open, raiders, &(Cargo){0});
	CHECK(t3 && t3->id == 2);                       // ids carry on, never reused
	if (again->log) fclose(again->log);

	const char *broken[] = {"transfer 9 a b 7 paired 1 0 0,0 tank:1\n", "delivered 42\n",
	                        "transfer 1 a b 0 paired 1 0 0,0 nonsense:3\n", "explode 1\n", "transfer 1 a b 0 paired\n",
	                        "transfer 1 a b 0 sideways 1 0 0,0 tank:1\n", "transfer 1 a b 0 unpaired 1 3 0,0 tank:1\n",
	                        "transfer 1 a b 0 paired 1 0 nowhere tank:1\n", "draw 1 0 z tank:5\n", "ended 2 west\n",
	                        "ended 1\n", "ended 1 west\nended 0 east\n"};
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
	d.ended = 2; // red won; blue's headquarters fell in east
	d.ended_in = 1;
	d.zones[1].status.fallen = 1;
	ByteBuf b = bytebuf_make(&a, 4096);
	put_directory(&b, &d);
	ByteReader r = byte_reader(b.data, b.len);
	Directory back;
	CHECK(get_directory(&r, &back) && r.pos == r.len);
	CHECK(directory_winner(&back) == 1 && back.ended_in == 1 && back.zones[1].status.fallen == 1);
	CHECK(directory_winner(&(Directory){0}) == -1);
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
	e0->metal = e0->metal_cap;
	float before = e1->metal;
	sim_step(&sim, nullptr, 0);
	CHECK(e0->metal == e0->metal_cap);
	float base = 1.0f * SIM_DT;   // the default base_metal, a tick of it
	CHECK(e1->metal > before + 2.0f * base - 1e-3f && e1->metal < before + 2.0f * base + 1e-3f);
	// Shares: what's asked, up to what the giver has and the taker's room.
	Command share = share_command((FactionId){0}, (PlayerSlot){1}, (PlayerSlot){0}, 100.0f, 0.0f);
	before = e1->metal;
	sim_step(&sim, &share, 1);    // 0 is full: nothing moves (but its income spills)
	CHECK(e1->metal >= before);
	e0->metal = 0.0f;
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
	e0->metal = 0.0f;
	e1->metal = 500.0f;
	e0->energy = e1->energy = 3000.0f;   // power for both: only metal differs
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

// Trucks: storage that moves. Their room adds to their owner's cap where
// they are; leaving or changing hands, one takes a full load (or what there
// is) and brings it, with its room, to where it goes; storage lost loses
// what's past the cap. Dug in as a vault, a truck's room stays.
static void test_trucks(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 0;
	c.players[0] = 2;
	Sim sim;
	sim_init(&sim, c);
	Faction *f = &sim.factions[0];
	Units *u = &f->units;
	Economy *e = &f->economy[0], *e1 = &f->economy[1];
	Command setup[3] = {
		{.faction = {0}, .player = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_ENERGY_TRUCK, .amount = 2, .target = {100, 256}, .radius = 2},
		{.faction = {0}, .player = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_METAL_TRUCK, .amount = 1, .target = {100, 260}, .radius = 0},
		{.faction = {0}, .player = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_TRANSPORTER, .amount = 1, .target = {110, 256}, .radius = 0},
	};
	sim_step(&sim, setup, 3);
	sim_step(&sim, nullptr, 0);
	CHECK(e->energy_cap == zone_context_default().energy_cap + 2.0f * zone_context_default().truck_energy && e->metal_cap == zone_context_default().metal_cap + zone_context_default().truck_metal);
	CHECK(e1->energy_cap == zone_context_default().energy_cap);
	// Room is room: past the base, income stays with player 0, not spilling to 1.
	e->energy = zone_context_default().energy_cap;
	float e1_before = e1->energy;
	for (int t = 0; t < 30; t++) sim_step(&sim, nullptr, 0);
	CHECK(e->energy > zone_context_default().energy_cap + 29.0f && e1->energy < e1_before + 31.0f);
	UnitHandle truck[3];
	for (ptrdiff_t i = 0, n = 0; i < u->count; i++)
		if (unit_is_truck(u->type[i])) truck[n++] = units_handle(u, i);
	// Given to player 1, an energy truck takes a full load and its room with it.
	e->energy = 18000.0f;
	ptrdiff_t first = units_find(u, truck[0]);
	UnitHandle energy_truck = u->type[first] == UNIT_ENERGY_TRUCK ? truck[0] : truck[1];
	float e1_had = e1->energy;
	Command give = {.faction = {0}, .player = {0}, .to = {1}, .kind = COMMAND_GIVE, .count = 1, .units = &energy_truck};
	sim_step(&sim, &give, 1);
	CHECK(e->energy_cap == zone_context_default().energy_cap + zone_context_default().truck_energy && e1->energy_cap == zone_context_default().energy_cap + zone_context_default().truck_energy);
	CHECK(e->energy > 12990.0f && e->energy < 13100.0f);           // 18000 less a load, plus a tick's income
	CHECK(e1->energy > e1_had + zone_context_default().truck_energy - 1.0f);
	// Dug in as a vault, the room stays.
	UnitHandle other = unit_handle_eq(energy_truck, truck[0]) ? truck[1] : truck[0];
	if (u->type[units_find(u, other)] != UNIT_ENERGY_TRUCK) other = truck[2];
	Command dig = {.faction = {0}, .player = {0}, .kind = COMMAND_ENTRENCH, .count = 1, .units = &other};
	sim_step(&sim, &dig, 1);
	for (int t = 0; t < 200; t++) sim_step(&sim, nullptr, 0);
	CHECK(u->type[units_find(u, other)] == UNIT_ENERGY_VAULT && e->energy_cap == zone_context_default().energy_cap + zone_context_default().truck_energy);
	// The metal truck sent through the transporter, for player 1: a full load
	// leaves, even from below the base (the player chose to send it).
	UnitHandle metal_truck = truck[0];
	for (int k = 0; k < 3; k++)
		if (u->type[units_find(u, truck[k])] == UNIT_METAL_TRUCK) metal_truck = truck[k];
	e->metal = 1500.0f;
	Command send = {.faction = {0}, .player = {0}, .to = {1}, .kind = COMMAND_SEND, .amount = 1, .count = 1, .units = &metal_truck};
	sim_step(&sim, &send, 1);
	float left_m = -1.0f;
	for (int t = 0; t < 600 && left_m < 0.0f; t++) {
		sim_step(&sim, nullptr, 0);
		for (ptrdiff_t k = 0; k < f->departure_count; k++)
			if (f->departures[k].type == UNIT_METAL_TRUCK) {
				left_m = f->departures[k].metal;
				CHECK(player_eq(f->departures[k].owner, (PlayerSlot){1}) && f->departures[k].energy == 0.0f);
			}
	}
	CHECK(left_m == zone_context_default().truck_metal);
	CHECK(e->metal < 600.0f);                                        // 1500 less the load, plus income
	// Arriving: two metal trucks for player 1 bring their room and 1500 metal.
	sim_step(&sim, nullptr, 0);
	float m1 = e1->metal;
	Command arrive = {.faction = {0}, .player = {1}, .kind = COMMAND_ARRIVE, .unit_type = UNIT_METAL_TRUCK, .amount = 2,
	                  .target = {110, 256}, .metal = 1500.0f};
	sim_step(&sim, &arrive, 1);
	CHECK(e1->metal_cap == zone_context_default().metal_cap + 2.0f * zone_context_default().truck_metal && e1->metal > m1 + 1499.0f);
	// The vault destroyed: the room goes, and what's past the cap with it.
	e->energy = e->energy_cap;
	u->hp[units_find(u, other)] = 0.0f;
	sim_step(&sim, nullptr, 0);
	sim_step(&sim, nullptr, 0);
	CHECK(e->energy_cap == zone_context_default().energy_cap && e->energy <= zone_context_default().energy_cap);
	sim_destroy(&sim);
}

// The command menus (menu.h): what a selection can do, on BAR's grids.
static void test_menu(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 1;
	Sim sim;
	sim_init(&sim, c);
	Units *u = &sim.factions[0].units;
	UnitHandle engineers[2], factory = NO_UNIT;
	int ne = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] == UNIT_ENGINEER && ne < 2) engineers[ne++] = units_handle(u, i);
		if (u->type[i] == UNIT_FACTORY) factory = units_handle(u, i);
	}
	// Builders: categories on the tabs, the open one's structures as items.
	SelectionSummary s = menu_summarize(u, engineers, 2);
	CHECK(s.total == 2 && s.builders && menu_build_open(&s));
	BuildMenu b = menu_build(&s, 0, 2);
	CHECK(b.builders && b.tab[0].active && b.tab[0].enabled && !b.tab[1].enabled); // combat: nothing yet
	CHECK(b.item[0].shown && b.item[0].type == UNIT_EXTRACTOR && b.item[1].type == UNIT_GENERATOR && !b.item[3].shown);
	// T2's: shown, but only an advanced engineer builds the advanced extractor.
	CHECK(b.item[2].type == UNIT_ADVANCED_EXTRACTOR && !b.item[2].enabled && b.item[0].enabled);
	b = menu_build(&s, 3, 2);
	CHECK(b.tab[3].active && b.item[0].type == UNIT_FACTORY && b.item[1].type == UNIT_ADVANCED_FACTORY && b.item[1].enabled);
	// A factory: pages, and what's queued.
	Command q = {.faction = {0}, .kind = COMMAND_PRODUCE, .count = 1, .units = &factory, .unit_type = UNIT_TANK, .amount = 3};
	sim_step(&sim, &q, 1);
	s = menu_summarize(u, &factory, 1);
	CHECK(s.factories && !s.builders);
	b = menu_build(&s, 0, 2);
	CHECK(!b.builders && b.item[0].type == UNIT_RAIDER && b.item[1].type == UNIT_TANK && b.item[1].queued == 3);
	CHECK(b.tab[0].shown && b.tab[1].shown && !b.tab[2].shown);
	b = menu_build(&s, 1, 2);
	CHECK(b.item[0].type == UNIT_METAL_TRUCK && b.item[1].type == UNIT_ENERGY_TRUCK);
	// Its orders: repeat (off) and clear; no move for a structure.
	MenuContext alone = {.economy = true};
	OrdersMenu o = menu_orders(&s, &alone);
	CHECK(o.slot[0].order == MENU_ORDER_REPEAT && o.slot[0].toggle && !o.slot[0].active && o.slot[1].order == MENU_ORDER_CLEAR);
	CHECK(!o.slot[6].shown);
	// Builders with the factory: the builders' grid.
	UnitHandle both[3] = {engineers[0], engineers[1], factory};
	s = menu_summarize(u, both, 3);
	CHECK(menu_build(&s, 0, 2).builders);
	// Orders for mobile units: move, fight, stop, entrench; send needs a world,
	// give and take a team, take someone away.
	s = menu_summarize(u, engineers, 2);
	o = menu_orders(&s, &alone);
	CHECK(o.slot[5].order == MENU_ORDER_STOP && o.slot[6].order == MENU_ORDER_MOVE && o.slot[7].order == MENU_ORDER_FIGHT);
	CHECK(!o.slot[8].shown && !o.slot[10].shown && !o.slot[11].shown && !o.slot[12].shown); // engineers don't dig in
	MenuContext team = {.economy = true, .team = true, .world = true};
	o = menu_orders(&s, &team);
	CHECK(o.slot[10].order == MENU_ORDER_SEND && o.slot[11].enabled && o.slot[12].shown && !o.slot[12].enabled);
	team.takeable = true;
	CHECK(menu_orders(&s, &team).slot[12].enabled);
	// Nothing selected: give (resources) and take, with a team.
	SelectionSummary none = {0};
	o = menu_orders(&none, &team);
	CHECK(!o.slot[5].shown && o.slot[11].shown && o.slot[11].order == MENU_ORDER_GIVE);
	sim_destroy(&sim);
}

// Keymaps: the defaults have no key twice and match config/keys.json; a file
// rebinds what it names; anything malformed, unknown, fixed or bound twice is
// refused whole.
static void test_keymap(void)
{
	Keymap d = keymap_default();
	bool named = true, distinct = true; // one check each: CHECK in a loop is costly for gcc's analyzer
	for (int a = 0; a < BIND_COUNT; a++) {
		const char *name = keymap_action_name((KeyBinding)a);
		named = named && d.key[a] > 0 && name[0] && keymap_action_of((Str){name, (ptrdiff_t)strlen(name)}) == a;
		for (int b = a + 1; b < BIND_COUNT; b++) distinct = distinct && d.key[a] != d.key[b];
	}
	CHECK(named && distinct);
	Arena a = arena_create("keymap", 1 << 20);
	Str text = {"", 0};
	bool read = platform_read_file(&a, "config/keys.json", &text);
	CHECK(read);
	Keymap file = keymap_default();
	for (int k = 0; k < BIND_COUNT; k++) file.key[k] = 0;
	char err[256] = "";
	bool ok = read && keymap_parse(&file, text, err, sizeof err);
	if (!ok) fprintf(stderr, "config/keys.json: %s\n", err);
	CHECK(ok && !memcmp(&file, &d, sizeof d));                 // names every action, as the defaults
	Keymap km = keymap_default();
	CHECK(keymap_parse(&km, S("{\"stop\": \"X\", \"tab_2\": \"F4\"}"), err, sizeof err));
	CHECK(km.key[BIND_STOP] == 'X' && km.key[BIND_TAB_2] == KEYCODE_F1 + 3 && km.key[BIND_MOVE] == 'M');
	Keymap before = km;
	const char *refused[] = {
		"{\"stop\": \"Q\"}",               // Q is build_1's
		"{\"jump\": \"J\"}",               // no such action
		"{\"stop\": \"Hyper\"}",           // no such key
		"{\"stop\": \"5\"}",               // digits choose zones and players
		"{\"stop\": \"X\",}",              // a trailing comma
		"{\"stop\": \"X\"} x",
		"[\"stop\"]",
	};
	// Through a volatile: gcc's analyzer would otherwise explore the parser
	// once per literal (45 s of make check).
	const char *volatile refused_one[countof(refused)];
	for (int k = 0; k < (int)countof(refused); k++) refused_one[k] = refused[k];
	bool all_refused = true;
	for (int k = 0; k < (int)countof(refused); k++)
		all_refused = all_refused && !keymap_parse(&km, (Str){refused_one[k], (ptrdiff_t)strlen(refused_one[k])}, err, sizeof err)
		           && !memcmp(&km, &before, sizeof km);          // refused whole
	CHECK(all_refused);
	CHECK(!strcmp(keymap_key_name(KEYCODE_SPACE), "Space") && !strcmp(keymap_key_name('Q'), "Q")
	      && !strcmp(keymap_key_name(KEYCODE_F1 + 11), "F12") && keymap_key_of(S("F13")) < 0);
	arena_destroy(&a);
}

// Commanders: the start (a headquarters and a commander each), presence
// (base storage and income only where your commander is), respawning at
// the headquarters, warp home (its price, its cooldown, a hit stopping
// it), and leaving for home from a zone without the headquarters.
static void test_commanders(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.commanders = true;
	c.bases = 1;
	c.players[0] = 2;
	Sim sim;
	sim_init(&sim, c);
	Faction *f = &sim.factions[0];
	Units *u = &f->units;
	int hq = 0, cmd = 0, other = 0;
	UnitHandle boss[2] = {NO_UNIT, NO_UNIT};
	for (ptrdiff_t i = 0; i < u->count; i++) {
		hq += u->type[i] == UNIT_HEADQUARTERS;
		if (u->type[i] == UNIT_COMMANDER) boss[u->owner[i].v] = units_handle(u, i), cmd++;
		other += u->type[i] != UNIT_HEADQUARTERS && u->type[i] != UNIT_COMMANDER;
	}
	CHECK(hq == 1 && cmd == 2 && other == 0);
	sim_step(&sim, nullptr, 0);
	CHECK(f->economy[1].metal_cap == zone_context_default().metal_cap && f->economy[1].metal_income > 0.0f);
	// Can't be given away.
	Command give = {.faction = {0}, .player = {1}, .to = {0}, .kind = COMMAND_GIVE, .count = 1, .units = &boss[1]};
	sim_step(&sim, &give, 1);
	CHECK(u->owner[units_find(u, boss[1])].v == 1);
	// blue.1's commander lost: no base storage or income here until it's back, 60 s on.
	u->hp[units_find(u, boss[1])] = 0.0f;
	sim_step(&sim, nullptr, 0);
	sim_step(&sim, nullptr, 0);
	CHECK(f->economy[1].metal_cap == 0.0f && f->economy[1].metal == 0.0f && f->economy[1].metal_income == 0.0f);
	CHECK(f->respawn_at[1] > sim.tick.v);
	int64_t due = f->respawn_at[1];
	while (sim.tick.v <= due) sim_step(&sim, nullptr, 0);
	sim_step(&sim, nullptr, 0);
	int back = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) back += u->type[i] == UNIT_COMMANDER && u->owner[i].v == 1;
	CHECK(back == 1 && f->respawn_at[1] == -1 && f->economy[1].metal_cap == zone_context_default().metal_cap);
	// blue.0 warps home: 10 s at full rate, 5000 energy, then beside the
	// headquarters, recharging; it can't warp again meanwhile.
	ptrdiff_t b0 = units_find(u, boss[0]);
	u->px[b0] = u->tx[b0] = 200.0f;
	f->economy[0].energy = f->economy[0].energy_cap;
	float before = f->economy[0].energy;
	Command warp = {.faction = {0}, .player = {0}, .kind = COMMAND_WARP, .count = 1, .units = &boss[0]};
	sim_step(&sim, &warp, 1);
	CHECK(u->order[units_find(u, boss[0])] == ORDER_WARP);
	for (int t = 0; t < 298; t++) sim_step(&sim, nullptr, 0);
	CHECK(u->order[units_find(u, boss[0])] == ORDER_WARP);          // not quite
	sim_step(&sim, nullptr, 0);
	sim_step(&sim, nullptr, 0);
	b0 = units_find(u, boss[0]);
	CHECK(u->order[b0] == ORDER_IDLE && u->px[b0] < 120.0f && u->recharge[b0] > 0);
	float paid = before + 300.0f * 30.0f * SIM_DT - f->economy[0].energy; // less the base income meanwhile
	CHECK(paid > 4500.0f && paid < 5600.0f);
	sim_step(&sim, &warp, 1);
	CHECK(u->order[units_find(u, boss[0])] != ORDER_WARP);          // recharging
	sim_destroy(&sim);

	// A zone without the headquarters: a warp and a loss both leave for home.
	c.bases = 0;
	sim_init(&sim, c);
	f = &sim.factions[0];
	u = &f->units;
	Command spawn = {.faction = {0}, .player = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_COMMANDER, .amount = 1,
	                 .target = {200, 256}, .radius = 0};
	Command spawn1 = spawn;
	spawn1.player = (PlayerSlot){1};
	Command both[2] = {spawn, spawn1};
	sim_step(&sim, both, 2);
	UnitHandle away0 = units_handle(u, 0), away1 = units_handle(u, 1);
	if (u->owner[0].v == 1) away0 = units_handle(u, 1), away1 = units_handle(u, 0);
	Command warp0 = {.faction = {0}, .player = {0}, .kind = COMMAND_WARP, .flags = COMMAND_FREE, .count = 1, .units = &away0};
	sim_step(&sim, &warp0, 1);
	bool warped = false, died = false;
	for (int t = 0; t < 320 && !warped; t++) {
		if (t == 5) u->hp[units_find(u, away1)] = 0.0f;
		sim_step(&sim, nullptr, 0);
		for (ptrdiff_t k = 0; k < f->departure_count; k++) {
			const Departure *d = &f->departures[k];
			CHECK(d->dest == DEST_HOME && d->type == UNIT_COMMANDER);
			warped = warped || (d->why == LEAVE_WARP && d->owner.v == 0);
			died = died || (d->why == LEAVE_DIED && d->owner.v == 1);
		}
	}
	CHECK(warped && died && units_find(u, away0) < 0);
	// A hit stops a warp: red tanks beside a warping commander.
	Command again = spawn, tanks = {.faction = {1}, .kind = COMMAND_SPAWN, .unit_type = UNIT_TANK, .amount = 4,
	                                .target = {215, 256}, .radius = 2};
	Command setup[2] = {again, tanks};
	sim_step(&sim, setup, 2);
	UnitHandle hit = NO_UNIT;
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (u->type[i] == UNIT_COMMANDER) hit = units_handle(u, i);
	Command warp_hit = {.faction = {0}, .player = {0}, .kind = COMMAND_WARP, .count = 1, .units = &hit};
	sim_step(&sim, &warp_hit, 1);
	CHECK(u->order[units_find(u, hit)] == ORDER_WARP);
	for (int t = 0; t < 150 && u->order[units_find(u, hit)] == ORDER_WARP; t++) sim_step(&sim, nullptr, 0);
	ptrdiff_t h = units_find(u, hit);
	CHECK(h >= 0 && u->order[h] == ORDER_IDLE && u->hp[h] < unit_defs[UNIT_COMMANDER].hp && u->recharge[h] == 0);
	sim_destroy(&sim);
}

// The game's end: a headquarters destroyed marks its side fallen; told the
// game is over, a zone freezes for good (ticks pass, nothing changes), and
// a client joining then gets the result with the rest.
static void test_game_over(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.commanders = true;
	c.bases = 3;
	Sim sim;
	sim_init(&sim, c);
	CHECK(sim_winner_due(&sim) == -1 && !sim_over(&sim));
	Units *blue = &sim.factions[0].units, *red = &sim.factions[1].units;
	UnitHandle boss = NO_UNIT;
	for (ptrdiff_t i = 0; i < red->count; i++)
		if (red->type[i] == UNIT_COMMANDER) boss = units_handle(red, i);
	for (ptrdiff_t i = 0; i < blue->count; i++)
		if (blue->type[i] == UNIT_HEADQUARTERS) blue->hp[i] = 0.0f;
	sim_step(&sim, nullptr, 0);
	CHECK(sim.factions[0].fallen && !sim.factions[1].fallen && sim_winner_due(&sim) == 1 && !sim_over(&sim));
	Command over[2] = {game_over_command((FactionId){0}, 1), game_over_command((FactionId){1}, 1)};
	sim_step(&sim, over, 2);
	CHECK(sim_over(&sim) && sim.factions[0].winner == 1 && sim.factions[1].winner == 1 && sim_winner_due(&sim) == -1);
	// Frozen: an order changes nothing, nor does time, but ticks go on.
	ptrdiff_t b = units_find(red, boss);
	float x = red->px[b], e = sim.factions[1].economy[0].energy;
	Hash before = faction_hash(&sim.factions[1]);
	Command move = {.faction = {1}, .player = {0}, .kind = COMMAND_MOVE, .target = v2(x - 50.0f, red->pz[b]), .count = 1, .units = &boss};
	int64_t tick = sim.tick.v;
	sim_step(&sim, &move, 1);
	for (int t = 0; t < 60; t++) sim_step(&sim, nullptr, 0);
	CHECK(red->px[units_find(red, boss)] == x && sim.factions[1].economy[0].energy == e && sim.tick.v == tick + 61);
	CHECK(faction_hash(&sim.factions[1]).v == before.v);
	// A later word changes nothing.
	Command other = game_over_command((FactionId){0}, 0);
	sim_step(&sim, &other, 1);
	CHECK(sim.factions[0].winner == 1);
	// The result survives a faction snapshot.
	Arena a = arena_create("game over", 64 << 20);
	ByteBuf buf = bytebuf_make(&a, 32 << 20);
	faction_write(&sim.factions[0], &buf);
	Sim late;
	sim_init(&late, c);
	ByteReader r = byte_reader(buf.data, buf.len);
	CHECK(faction_read(&late.factions[0], &r) && late.factions[0].fallen && late.factions[0].winner == 1);
	sim_destroy(&late);
	arena_destroy(&a);
	sim_destroy(&sim);
}

// A zone's context: presets and fields, refused whole when wrong; the
// standard one is the sim as it was; it changes what a zone pays and earns;
// and it travels with the config (wire, replays).
static void test_context(void)
{
	ZoneContext c = zone_context_default();
	char err[200];
	CHECK(zone_context_ok(&c) && c.base_metal == 1.0f && c.metal_cap == 2000.0f && c.yield[SOURCE_GENERATOR] == 1.0f);
	CHECK(context_parse(&c, CONTEXT_ECONOMY, "home", err, sizeof err) && c.base_metal == 8.0f);
	CHECK(context_parse(&c, CONTEXT_ECONOMY, "cost=1.5 truck_metal=2000", err, sizeof err) && c.base_metal == 8.0f
	      && c.cost == 1.5f && c.truck_metal == 2000.0f);
	CHECK(context_parse(&c, CONTEXT_ENVIRONMENT, "barren generator=2", err, sizeof err)
	      && c.yield[SOURCE_METAL] == 0.5f && c.yield[SOURCE_GENERATOR] == 2.0f && c.cost == 1.5f); // parts apart
	CHECK(context_parse(&c, CONTEXT_ECONOMY, "lean", err, sizeof err) && c.cost == 1.25f && c.truck_metal == 1000.0f
	      && c.yield[SOURCE_GENERATOR] == 2.0f); // a preset sets its own part only
	ZoneContext before = c;
	const char *bad_economy[] = {"plenty", "cost=", "cost=-1", "cost=abc", "wind=2", "cost=1 home", "start_metal=5000",
	                             "build_time=0", "cost=200"};
	for (int k = 0; k < (int)countof(bad_economy); k++)
		CHECK(!context_parse(&c, CONTEXT_ECONOMY, bad_economy[k], err, sizeof err) && !memcmp(&c, &before, sizeof c));
	CHECK(!context_parse(&c, CONTEXT_ENVIRONMENT, "wind=0.3", err, sizeof err)); // no wind yet: a typo fails
	CHECK(!context_parse(&c, CONTEXT_ENVIRONMENT, "cost=2", err, sizeof err));   // an economy field
	CHECK(parse_fails("scenario economy\neconomy rich cost=nope\n", "expected a number"));
	CHECK(parse_fails("scenario economy\nenvironment marsh\n", "a preset"));

	// What a zone pays and earns: twice the cost, half the generators.
	SimConfig cfg = sim_config_default();
	cfg.economy = true;
	cfg.bases = 1;
	SimConfig lean = cfg;
	lean.context.cost = 2.0f;
	lean.context.yield[SOURCE_GENERATOR] = 0.5f;
	Sim a, b;
	sim_init(&a, cfg);
	sim_init(&b, lean);
	for (Sim *sim = &a; sim; sim = sim == &a ? &b : nullptr) {
		Command gen = {.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_GENERATOR, .amount = 4, .target = v2(60.0f, 200.0f), .radius = 8.0f};
		sim_step(sim, &gen, 1);
		Units *u = &sim->factions[0].units;
		UnitHandle plant = NO_UNIT;
		for (ptrdiff_t i = 0; i < u->count; i++)
			if (u->type[i] == UNIT_FACTORY) plant = units_handle(u, i);
		Command make = {.faction = {0}, .kind = COMMAND_PRODUCE, .unit_type = UNIT_TANK, .amount = 1, .count = 1, .units = &plant};
		sim_step(sim, &make, 1);
		for (int t = 0; t < 10; t++) sim_step(sim, nullptr, 0);
	}
	const Economy *ea = &a.factions[0].economy[0], *eb = &b.factions[0].economy[0];
	CHECK(ea->energy_income > eb->energy_income && eb->energy_income == 30.0f + 4 * 40.0f * 0.5f);
	CHECK(ea->metal_wanted > 0.0f && eb->metal_wanted > 1.99f * ea->metal_wanted && eb->metal_wanted < 2.01f * ea->metal_wanted);
	sim_destroy(&a);
	sim_destroy(&b);

	// It travels: the wire's config and a replay's header.
	Arena ar = arena_create("context", 4 << 20);
	ByteBuf buf = bytebuf_make(&ar, 1024);
	wire_put_config(&buf, &lean);
	ByteReader r = byte_reader(buf.data, buf.len);
	SimConfig back = sim_config_default();
	CHECK(wire_get_config(&r, &back) && r.pos == r.len && !memcmp(&back.context, &lean.context, sizeof lean.context));
	ReplayWriter w;
	replay_begin(&w, &ar, lean);
	ReplayReader rr;
	CHECK(replay_open(&rr, replay_bytes(&w)) == REPLAY_OK && !memcmp(&rr.config.context, &lean.context, sizeof lean.context));
	arena_destroy(&ar);
}

// A zone's map: its size and metal spots, from the context. The standard
// map is the one there always was; a huge one is four times as wide, its
// spots inside it, mirrored, apart, its bases an eighth of the way in, and
// positions still quantize finely enough over it.
static void test_map(void)
{
	ZoneContext c = zone_context_default();
	char err[200];
	CHECK(c.size == 512.0f && c.spots == 24.0f);
	CHECK(context_parse(&c, CONTEXT_MAP, "huge", err, sizeof err) && c.size == 2048.0f && c.spots == 256.0f);
	CHECK(context_parse(&c, CONTEXT_MAP, "size=1536 spots=160", err, sizeof err) && c.size == 1536.0f);
	const char *bad[] = {"size=100", "size=4096", "size=1000", "spots=4", "spots=300", "spots=9.5", "acres=2", "vast"};
	for (int k = 0; k < (int)countof(bad); k++) CHECK(!context_parse(&c, CONTEXT_MAP, bad[k], err, sizeof err));
	CHECK(context_parse(&c, CONTEXT_ECONOMY, "lean", err, sizeof err) && c.size == 1536.0f); // the economy leaves the map be

	SimConfig cfg = sim_config_default();
	cfg.economy = true;
	Sim small, big;
	sim_init(&small, cfg);
	CHECK(small.terrain.size_x == 512.0f && small.spots.count == 24 && small.factions[0].base.x == 64.0f);
	cfg.context.size = 2048.0f;
	cfg.context.spots = 256.0f;
	sim_init(&big, cfg);
	CHECK(big.terrain.size_x == 2048.0f && big.terrain.cells_x == 512 && big.factions[0].base.x == 256.0f);
	CHECK(big.spots.count > 200 && big.spots.count <= 256 && big.spots.count % 2 == 0);
	bool inside = true, mirrored = true;
	for (int32_t k = 0; k < big.spots.count; k++) {
		V2 p = big.spots.at[k];
		inside = inside && p.x > 0.0f && p.x < 2048.0f && p.z > 0.0f && p.z < 2048.0f;
		mirrored = mirrored && spot_at(&big.spots, v2(2048.0f - p.x, p.z)) >= 0;
	}
	CHECK(inside && mirrored);
	// 1/32 unit over 2048.
	float worst = 0.0f;
	for (float x = 0.0f; x < 2048.0f; x += 7.31f) {
		float d = dq_pos(q_pos(x, 2048.0f), 2048.0f) - x;
		worst = f_max(worst, d < 0.0f ? -d : d);
	}
	CHECK(worst <= 1.0f / 64.0f + 1e-4f);
	// More than 64 spots: sets reach them all.
	SpotSet set = {0};
	spotset_add(&set, 200);
	CHECK(spotset_has(&set, 200) && !spotset_has(&set, 8) && spotset_any(&set) && spotset_words(256) == 4 && spotset_words(24) == 1);
	sim_destroy(&small);
	sim_destroy(&big);
}

// Cancel: an unfinished structure goes, what was spent on it comes back,
// and its builder stops (rather than putting the site back up). An
// unfinished factory takes a queue.
static void test_cancel_build(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 1;
	Sim sim;
	sim_init(&sim, c);
	Units *u = &sim.factions[0].units;
	UnitHandle eng = NO_UNIT;
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (u->type[i] == UNIT_ENGINEER && unit_handle_eq(eng, NO_UNIT)) eng = units_handle(u, i);
	ptrdiff_t e = units_find(u, eng);
	V2 at = v2_add(v2(u->px[e], u->pz[e]), v2(0.0f, 14.0f));
	Command build = {.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_FACTORY, .target = at, .count = 1, .units = &eng};
	sim_step(&sim, &build, 1);
	for (int t = 0; t < 300; t++) sim_step(&sim, nullptr, 0);
	UnitHandle site = NO_UNIT;
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (u->type[i] == UNIT_FACTORY && u->progress[i] < 1.0f) site = units_handle(u, i);
	CHECK(units_find(u, site) >= 0);
	Command produce = {.faction = {0}, .kind = COMMAND_PRODUCE, .unit_type = UNIT_TANK, .amount = 2, .count = 1, .units = &site};
	sim_step(&sim, &produce, 1);
	CHECK(u->queue_len[units_find(u, site)] == 2);
	float progress = u->progress[units_find(u, site)], metal = sim.factions[0].economy[0].metal;
	Command cancel = {.faction = {0}, .kind = COMMAND_CANCEL, .count = 1, .units = &site};
	sim_step(&sim, &cancel, 1);
	float back = sim.factions[0].economy[0].metal - metal;
	CHECK(units_find(u, site) < 0 && u->order[units_find(u, eng)] == ORDER_IDLE);
	CHECK(back > 0.8f * progress * unit_defs[UNIT_FACTORY].metal_cost && back < 1.2f * progress * unit_defs[UNIT_FACTORY].metal_cost + 5.0f);
	for (int t = 0; t < 60; t++) sim_step(&sim, nullptr, 0);
	int sites = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) sites += u->type[i] == UNIT_FACTORY && u->progress[i] < 1.0f;
	CHECK(sites == 0); // and nobody put it back up
	sim_destroy(&sim);
}

// Orders for later (shift): queued moves run in order; a plain order
// replaces the list; a queued build list is built in turn; lists survive a
// snapshot (same hash), and a dead unit's nodes go back to the pool.
static void test_later_orders(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 1;
	Sim sim;
	sim_init(&sim, c);
	Faction *f = &sim.factions[0];
	Units *u = &f->units;
	f->economy[0].metal = f->economy[0].metal_cap = 1e6f;
	f->economy[0].energy = f->economy[0].energy_cap = 1e7f;
	Command spawn = {.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_TANK, .amount = 1, .target = v2(100.0f, 200.0f)};
	sim_step(&sim, &spawn, 1);
	UnitHandle tank = NO_UNIT, eng = NO_UNIT;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] == UNIT_TANK) tank = units_handle(u, i);
		if (u->type[i] == UNIT_ENGINEER && unit_handle_eq(eng, NO_UNIT)) eng = units_handle(u, i);
	}
	ptrdiff_t free0 = u->later_free_count;
	Command moves[] = {
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(120.0f, 200.0f), .count = 1, .units = &tank},
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(120.0f, 230.0f), .count = 1, .units = &tank, .flags = COMMAND_QUEUED},
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(100.0f, 230.0f), .count = 1, .units = &tank, .flags = COMMAND_QUEUED},
	};
	sim_step(&sim, moves, countof(moves));
	ptrdiff_t d = units_find(u, tank);
	CHECK(u->order[d] == ORDER_MOVE && u->later_len[d] == 2 && u->later_free_count == free0 - 2);

	// The lists survive a snapshot: same hash, same list.
	Arena a = arena_create("later", 16 << 20);
	ByteBuf buf = bytebuf_make(&a, 8 << 20);
	faction_write(f, &buf);
	Sim copy;
	sim_init(&copy, c);
	ByteReader r = byte_reader(buf.data, buf.len);
	CHECK(faction_read(&copy.factions[0], &r));
	CHECK(faction_hash(&copy.factions[0]).v == faction_hash(f).v);
	CHECK(copy.factions[0].units.later_len[units_find(&copy.factions[0].units, tank)] == 2);
	sim_destroy(&copy);

	// Corner by corner: it passes the second corner, then ends at the third.
	bool passed = false;
	for (int t = 0; t < 1200; t++) {
		sim_step(&sim, nullptr, 0);
		d = units_find(u, tank);
		passed = passed || v2_len(v2_sub(v2(u->px[d], u->pz[d]), v2(120.0f, 230.0f))) < 4.0f;
	}
	CHECK(passed && v2_len(v2_sub(v2(u->px[d], u->pz[d]), v2(100.0f, 230.0f))) < 4.0f && u->later_len[d] == 0);
	CHECK(u->later_free_count == free0);

	// A plain order replaces the list.
	Command again[] = {
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(140.0f, 200.0f), .count = 1, .units = &tank},
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(140.0f, 240.0f), .count = 1, .units = &tank, .flags = COMMAND_QUEUED},
	};
	sim_step(&sim, again, countof(again));
	Command plain = {.faction = {0}, .kind = COMMAND_MOVE, .target = v2(90.0f, 200.0f), .count = 1, .units = &tank};
	sim_step(&sim, &plain, 1);
	d = units_find(u, tank);
	CHECK(u->later_len[d] == 0 && u->tx[d] == 90.0f && u->later_free_count == free0);

	// Queued builds: an engineer puts up two generators, one after the other.
	int before = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) before += u->type[i] == UNIT_GENERATOR;
	ptrdiff_t e = units_find(u, eng);
	V2 at = v2(u->px[e], u->pz[e]);
	Command builds[] = {
		{.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_GENERATOR, .target = v2_add(at, v2(0.0f, 10.0f)), .count = 1, .units = &eng},
		{.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_GENERATOR, .target = v2_add(at, v2(10.0f, 10.0f)), .count = 1, .units = &eng, .flags = COMMAND_QUEUED},
	};
	sim_step(&sim, builds, countof(builds));
	for (int t = 0; t < 3000; t++) sim_step(&sim, nullptr, 0);
	int after = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) after += u->type[i] == UNIT_GENERATOR && u->progress[i] >= 1.0f;
	CHECK(after == before + 2);

	// Steering: a second site straight behind the first; the engineer goes
	// round the first generator (it used to stand against it).
	e = units_find(u, eng);
	at = v2(u->px[e], u->pz[e]);
	Command behind[] = {
		{.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_GENERATOR, .target = v2_add(at, v2(0.0f, 10.0f)), .count = 1, .units = &eng},
		{.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_GENERATOR, .target = v2_add(at, v2(0.0f, 18.0f)), .count = 1, .units = &eng, .flags = COMMAND_QUEUED},
	};
	sim_step(&sim, behind, countof(behind));
	for (int t = 0; t < 3000; t++) sim_step(&sim, nullptr, 0);
	int behind_done = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) behind_done += u->type[i] == UNIT_GENERATOR && u->progress[i] >= 1.0f;
	CHECK(behind_done == after + 2);

	// A dead unit's list goes back to the pool.
	Command more[] = {
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(140.0f, 200.0f), .count = 1, .units = &tank},
		{.faction = {0}, .kind = COMMAND_MOVE, .target = v2(140.0f, 240.0f), .count = 1, .units = &tank, .flags = COMMAND_QUEUED},
	};
	sim_step(&sim, more, countof(more));
	CHECK(u->later_free_count == free0 - 1);
	units_kill(u, units_find(u, tank));
	CHECK(u->later_free_count == free0);
	arena_destroy(&a);
	sim_destroy(&sim);
}

// A commander-only start builds on either side: red's sites mirror blue's
// (red's commander once walked into its headquarters toward a site behind
// it, and stood there for the rest of the game).
static void test_bot_commander_start(void)
{
	for (int side = 0; side < FACTION_COUNT; side++) {
		SimConfig c = sim_config_default();
		c.economy = true;
		c.commanders = true;
		c.bases = (uint8_t)(1 << side);
		Sim sim;
		sim_init(&sim, c);
		Bot bot = bot_create((FactionId){(uint32_t)side}, 1);
		Arena cmd_arena = arena_create("cmds", 1 << 20);
		Command cmds[64];
		for (int t = 0; t < 3000; t++) {
			arena_reset(&cmd_arena);
			ptrdiff_t n = 0;
			bot_think(&bot, &sim, &cmd_arena, cmds, &n, countof(cmds));
			sim_step(&sim, cmds, n);
		}
		const Units *u = &sim.factions[side].units;
		int factories = 0;
		for (ptrdiff_t i = 0; i < u->count; i++) factories += u->type[i] == UNIT_FACTORY && u->progress[i] >= 1.0f;
		CHECK(factories >= 1);
		arena_destroy(&cmd_arena);
		sim_destroy(&sim);
	}
}

// T2 turned off (the default, tier 1): no advanced factory goes up, and one
// that's there anyway makes nothing; T1 goes on as before.
static void test_tier_off(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.bases = 1;
	CHECK(c.context.tier == 1.0f);
	Sim sim;
	sim_init(&sim, c);
	Units *u = &sim.factions[0].units;
	Command setup = {.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_ADVANCED_FACTORY, .amount = 1, .target = v2(90.0f, 330.0f)};
	sim_step(&sim, &setup, 1);
	sim_step(&sim, nullptr, 0);
	UnitHandle eng = NO_UNIT, plant = NO_UNIT;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] == UNIT_ENGINEER && unit_handle_eq(eng, NO_UNIT)) eng = units_handle(u, i);
		if (u->type[i] == UNIT_ADVANCED_FACTORY) plant = units_handle(u, i);
	}
	CHECK(units_find(u, eng) >= 0 && units_find(u, plant) >= 0);
	Command no[] = {
		{.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_ADVANCED_FACTORY, .target = v2(120.0f, 330.0f), .count = 1, .units = &eng},
		{.faction = {0}, .kind = COMMAND_PRODUCE, .unit_type = UNIT_HEAVY_TANK, .amount = 1, .count = 1, .units = &plant},
	};
	sim_step(&sim, no, countof(no));
	CHECK(u->order[units_find(u, eng)] == ORDER_IDLE && u->queue_len[units_find(u, plant)] == 0);
	Command yes = {.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_FACTORY, .target = v2(120.0f, 330.0f), .count = 1, .units = &eng};
	sim_step(&sim, &yes, 1);
	CHECK(u->order[units_find(u, eng)] == ORDER_BUILD);
	sim_destroy(&sim);
}

// T2: any builder puts up an advanced factory, which makes only T2 (and a
// factory only T1); only an advanced engineer builds an advanced extractor,
// over our extractor, which it replaces when done, earning more; T2 can't
// pioneer (unpaired sends).
static void test_t2(void)
{
	SimConfig c = sim_config_default();
	c.economy = true;
	c.context.tier = 2.0f;
	c.bases = 1;
	Sim sim;
	sim_init(&sim, c);
	Faction *f = &sim.factions[0];
	Units *u = &f->units;
	f->economy[0].metal = f->economy[0].metal_cap = 1e6f; // not what this is about
	f->economy[0].energy = f->economy[0].energy_cap = 1e7f;
	V2 spot = sim.spots.at[0];
	Command setup[] = {
		{.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_EXTRACTOR, .amount = 1, .target = spot},
		{.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_ADVANCED_ENGINEER, .amount = 1, .target = v2_add(spot, v2(5.0f, 0.0f))},
		{.faction = {0}, .kind = COMMAND_SPAWN, .unit_type = UNIT_ADVANCED_FACTORY, .amount = 1, .target = v2(90.0f, 330.0f)},
	};
	sim_step(&sim, setup, countof(setup));
	sim_step(&sim, nullptr, 0);
	UnitHandle eng = NO_UNIT, adv = NO_UNIT, plant = NO_UNIT, t1 = NO_UNIT;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] == UNIT_ENGINEER && unit_handle_eq(eng, NO_UNIT)) eng = units_handle(u, i);
		if (u->type[i] == UNIT_ADVANCED_ENGINEER) adv = units_handle(u, i);
		if (u->type[i] == UNIT_ADVANCED_FACTORY) plant = units_handle(u, i);
		if (u->type[i] == UNIT_FACTORY) t1 = units_handle(u, i);
	}
	CHECK(units_find(u, adv) >= 0 && units_find(u, plant) >= 0 && units_find(u, t1) >= 0);
	CHECK(builder_builds(UNIT_ENGINEER, UNIT_ADVANCED_FACTORY) && builder_builds(UNIT_COMMANDER, UNIT_ADVANCED_FACTORY));
	CHECK(!builder_builds(UNIT_ENGINEER, UNIT_ADVANCED_EXTRACTOR) && builder_builds(UNIT_ADVANCED_ENGINEER, UNIT_ADVANCED_EXTRACTOR));
	CHECK(!builder_builds(UNIT_ENGINEER, UNIT_HEADQUARTERS) && !builder_builds(UNIT_ENGINEER, UNIT_TURRET));
	CHECK(placement_ok(&sim, (FactionId){0}, UNIT_ADVANCED_EXTRACTOR, spot) && !placement_ok(&sim, (FactionId){0}, UNIT_EXTRACTOR, spot));

	// An engineer can't take the order; the advanced engineer can.
	Command no = {.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_ADVANCED_EXTRACTOR, .target = spot, .count = 1, .units = &eng};
	sim_step(&sim, &no, 1);
	CHECK(u->order[units_find(u, eng)] == ORDER_IDLE);
	// Factories: each its own tier.
	Command wrong[] = {
		{.faction = {0}, .kind = COMMAND_PRODUCE, .unit_type = UNIT_HEAVY_TANK, .amount = 1, .count = 1, .units = &t1},
		{.faction = {0}, .kind = COMMAND_PRODUCE, .unit_type = UNIT_RAIDER, .amount = 1, .count = 1, .units = &plant},
	};
	sim_step(&sim, wrong, 2);
	CHECK(u->queue_len[units_find(u, t1)] == 0 && u->queue_len[units_find(u, plant)] == 0);
	Command right[] = {
		{.faction = {0}, .kind = COMMAND_PRODUCE, .unit_type = UNIT_HEAVY_TANK, .amount = 1, .count = 1, .units = &plant},
		{.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_ADVANCED_EXTRACTOR, .target = spot, .count = 1, .units = &adv},
	};
	sim_step(&sim, right, 2);
	CHECK(u->queue_len[units_find(u, plant)] == 1);
	float before = 0.0f;
	for (int t = 0; t < 3000; t++) {
		sim_step(&sim, nullptr, 0);
		if (t == 5) before = f->economy[0].metal_income;
	}
	int heavy = 0, deep = 0, shallow = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		heavy += u->type[i] == UNIT_HEAVY_TANK;
		deep += u->type[i] == UNIT_ADVANCED_EXTRACTOR && u->progress[i] >= 1.0f;
		shallow += u->type[i] == UNIT_EXTRACTOR;
	}
	CHECK(heavy == 1 && deep == 1 && shallow == 0);
	CHECK(f->economy[0].metal_income > before + 4.0f);
	CHECK(f->stats.units_lost == 0); // replaced, not lost
	// A teammate's extractor isn't ours to deepen: the order comes to nothing.
	sim.config.players[0] = 2;
	V2 other = sim.spots.at[1];
	Command theirs[] = {
		{.faction = {0}, .player = {1}, .kind = COMMAND_SPAWN, .unit_type = UNIT_EXTRACTOR, .amount = 1, .target = other},
	};
	sim_step(&sim, theirs, 1);
	Command take = {.faction = {0}, .kind = COMMAND_BUILD, .unit_type = UNIT_ADVANCED_EXTRACTOR, .target = other, .count = 1, .units = &adv};
	sim_step(&sim, &take, 1);
	for (int t = 0; t < 1500; t++) sim_step(&sim, nullptr, 0);
	int theirs_left = 0, deep_there = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		bool there = spot_at(&sim.spots, v2(u->px[i], u->pz[i])) == 1;
		theirs_left += there && u->type[i] == UNIT_EXTRACTOR && u->owner[i].v == 1;
		deep_there += there && u->type[i] == UNIT_ADVANCED_EXTRACTOR;
	}
	CHECK(theirs_left == 1 && deep_there == 0);
	// No pioneering in T2.
	CHECK(unpaired_can_send(UNIT_RAIDER) && unpaired_can_send(UNIT_ENGINEER));
	CHECK(!unpaired_can_send(UNIT_HEAVY_TANK) && !unpaired_can_send(UNIT_HOWITZER) && !unpaired_can_send(UNIT_ADVANCED_ENGINEER));
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
	{"trucks", test_trucks},
	{"menu", test_menu},
	{"keymap", test_keymap},
	{"commanders", test_commanders},
	{"game-over", test_game_over},
	{"context", test_context},
	{"t2", test_t2},
	{"tier-off", test_tier_off},
	{"bot-commander-start", test_bot_commander_start},
	{"later-orders", test_later_orders},
	{"cancel-build", test_cancel_build},
	{"map", test_map},
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
