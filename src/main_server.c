// rts-server: runs the sim headless, in real time, and streams it to clients.
//
//   rts-server [--scenario NAME | --script PATH] [--port N] [--ticks N]
//              [--record PATH] [--stats PATH] [--bot blue|red on|off]
//              [--latency MS] [--jitter MS] [--dilation F] [--speed X]
//
// The server owns the tick. Players get their faction's snapshot, then each
// tick that faction's commands and effects, and simulate it themselves;
// nothing about an enemy they can't see is sent. Spectators get the full
// mirror instead (wire.h). Bots and scripts run inside the tick, and
// every admitted command, theirs and clients', is recorded, so a run can be
// verified with `rts-headless verify`.
//
// --latency/--jitter delay everything this server sends, to every client.
// --dilation forces time dilation; otherwise it follows the step time.
// --speed runs the clock X times faster than real time, for tests.
//
// Unity build: this file is the whole program.
#include "base.h"

#include <signal.h>

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "bot.c"
#include "replay.c"
#include "scenario.c"
#include "script.c"
#include "wire.c"
#include "net.c"

enum { MAX_CLIENTS = 8, MAX_COMMANDS = 512 };
static constexpr ptrdiff_t CLIENT_OUT = (ptrdiff_t)32 << 20;  // power of two
static constexpr ptrdiff_t CLIENT_IN = (ptrdiff_t)1 << 20;

typedef enum { CLIENT_FREE, CLIENT_HELLO, CLIENT_READY } ClientState;

typedef struct {
	ClientState state;
	Conn conn;
	uint8_t faction;          // 0, 1 or SPECTATOR
	int64_t joined_tick;
	int64_t commands;
} Client;

typedef struct {
	Sim sim;
	Script script;
	ScriptRun run;
	Mirror mirror;
	Client clients[MAX_CLIENTS];
	int listen_fd;
	double latency, jitter;

	Arena program;            // process lifetime
	Arena tick;               // one tick: commands, client command handles
	Arena encode;             // one tick: the encoded delta
	Arena replay;

	ReplayWriter writer;
	bool recording;
	Command queued[MAX_COMMANDS];
	ptrdiff_t queued_count;

	double step_ms_avg;
	double forced_dilation;   // 0: automatic
	double speed;             // clock multiplier; 1 is real time
	double dilation;
	WireStats wire;
	int64_t faction_bytes[FACTION_COUNT];   // each faction's stream, encoded
	int64_t bytes_out, bytes_out_last;
	int64_t joins, commands_admitted, commands_refused;
} Server;

static double arg_f(const char *s, double lo, double hi, const char *what)
{
	char *end;
	double v = strtod(s, &end);
	if (end == s || *end || v < lo || v > hi) fatal("%s out of range: %s", what, s);
	return v;
}

// ---------------------------------------------------------------- clients

static void send_or_drop(Server *s, Client *c, uint8_t kind, const ByteBuf *b, double now)
{
	if (b->overflow) fatal("message buffer overflow (kind %d)", kind);
	if (!conn_send(&c->conn, kind, b->data, b->len, now))
		fprintf(stderr, "server: client dropped: %s\n", c->conn.why_closed);
	(void)s;
}

static void handle_hello(Server *s, Client *c, ByteReader *r, double now)
{
	uint32_t version = get32(r);
	uint8_t faction = get8(r);
	if (r->bad || version != WIRE_VERSION || (faction >= FACTION_COUNT && faction != SPECTATOR)) {
		conn_close(&c->conn, "bad hello");
		return;
	}
	c->faction = faction;
	c->state = CLIENT_READY;
	c->joined_tick = s->sim.tick.v;
	s->joins++;

	ArenaMark m = arena_mark(&s->encode);
	ByteBuf b = bytebuf_make(&s->encode, 4096);
	put32(&b, WIRE_VERSION);
	put8(&b, faction);
	wire_put_config(&b, &s->sim.config);
	for (int f = 0; f < FACTION_COUNT; f++) {
		putf(&b, s->sim.factions[f].base.x);
		putf(&b, s->sim.factions[f].base.z);
	}
	send_or_drop(s, c, MSG_WELCOME, &b, now);
	ByteBuf snap = bytebuf_make(&s->encode, FRAME_MAX);
	bool player = faction < FACTION_COUNT;
	if (player) wire_encode_faction_snapshot(&s->sim, (FactionId){faction}, &snap);
	else wire_encode_snapshot(&s->mirror, &snap);
	send_or_drop(s, c, player ? MSG_FACTION_SNAPSHOT : MSG_SNAPSHOT, &snap, now);
	arena_rewind(m);
	printf("server: client joined as %s at tick %lld (snapshot %td bytes)\n",
	       faction == SPECTATOR ? "spectator" : faction == 0 ? "blue" : "red",
	       (long long)s->sim.tick.v, snap.len);
}

// Commands are untrusted: only orders, only for the client's own faction,
// with sane targets; handles are checked by the sim like anyone's.
static void handle_command(Server *s, Client *c, ByteReader *r)
{
	uint8_t kind = get8(r);
	V2 target = {getf(r), getf(r)};
	uint8_t unit_type = get8(r);
	int32_t amount = (int32_t)get32(r);
	uint8_t flags = get8(r);
	uint32_t n = get32(r);
	// Players order, build and produce; spawning is for scripts only.
	bool kind_ok = kind <= COMMAND_ATTACK_MOVE || kind == COMMAND_BUILD || kind == COMMAND_PRODUCE;
	bool ok = !r->bad && c->faction < FACTION_COUNT && kind_ok && unit_type < UNIT_TYPE_COUNT
	       && amount >= 0 && amount <= FACTORY_QUEUE && flags <= COMMAND_REPEAT
	       && n <= (uint32_t)s->sim.config.max_units && (ptrdiff_t)n * 8 == r->len - r->pos
	       && s->queued_count < MAX_COMMANDS;
	if (!ok) {
		s->commands_refused++;
		return;
	}
	UnitHandle *h = PUSH(&s->tick, UnitHandle, n);
	for (uint32_t i = 0; i < n; i++) h[i] = (UnitHandle){get32(r), get32(r)};
	s->queued[s->queued_count++] = (Command){.faction = {c->faction}, .kind = (CommandKind)kind,
	                                         .target = target, .count = n, .units = h,
	                                         .unit_type = (UnitType)unit_type, .amount = amount, .flags = flags};
	c->commands++;
	s->commands_admitted++;
}

static void service_clients(Server *s, double now)
{
	int fd;
	while ((fd = net_accept(s->listen_fd)) >= 0) {
		Client *c = nullptr;
		for (int k = 0; k < MAX_CLIENTS && !c; k++)
			if (s->clients[k].state == CLIENT_FREE) c = &s->clients[k];
		if (!c) {
			net_close_fd(fd);
			continue;
		}
		conn_reopen(&c->conn, fd);
		conn_set_delay(&c->conn, s->latency, s->jitter, (uint64_t)fd * 7919u);
		c->state = CLIENT_HELLO;
		c->commands = 0;
	}
	for (int k = 0; k < MAX_CLIENTS; k++) {
		Client *c = &s->clients[k];
		if (c->state == CLIENT_FREE) continue;
		conn_pump(&c->conn, now);
		Frame f;
		while (c->conn.open && conn_next(&c->conn, &f)) {
			ByteReader r = byte_reader(f.data, f.len);
			if (f.kind == MSG_HELLO && c->state == CLIENT_HELLO) handle_hello(s, c, &r, now);
			else if (f.kind == MSG_COMMAND && c->state == CLIENT_READY) handle_command(s, c, &r);
			else if (f.kind == MSG_PING) conn_send(&c->conn, MSG_PONG, f.data, f.len, now);
			else conn_close(&c->conn, "unexpected message");
		}
		if (!c->conn.open) {
			printf("server: client left at tick %lld: %s\n", (long long)s->sim.tick.v,
			       c->conn.why_closed ? c->conn.why_closed : "closed");
			s->bytes_out += c->conn.bytes_sent;
			c->conn.bytes_sent = 0;
			c->state = CLIENT_FREE;
		}
	}
}

// ---------------------------------------------------------------- ticking

static void step(Server *s, double now)
{
	ptrdiff_t n = s->queued_count;
	n += script_tick(&s->run, &s->sim, &s->tick, s->queued + n, MAX_COMMANDS - n);
	Tick tick = s->sim.tick;
	double t0 = platform_seconds();
	sim_step(&s->sim, s->queued, n);
	double ms = 1e3 * (platform_seconds() - t0);
	s->step_ms_avg = s->step_ms_avg == 0.0 ? ms : s->step_ms_avg * 0.95 + ms * 0.05;
	if (s->recording) replay_tick(&s->writer, tick, s->queued, n, sim_hash_now(&s->sim));

	// Time dilation: if a step takes more than 80% of the tick budget, slow
	// the clock so the server stays responsive instead of falling behind.
	double budget = 1e3 / SIM_HZ;
	double automatic = s->step_ms_avg > 0.8 * budget ? 0.8 * budget / s->step_ms_avg : 1.0;
	s->dilation = s->forced_dilation > 0.0 ? s->forced_dilation : automatic;

	arena_reset(&s->encode);
	uint16_t dilation = (uint16_t)(s->dilation * 1000.0 + 0.5);
	ByteBuf delta = bytebuf_make(&s->encode, FRAME_MAX);
	bool digest = s->sim.tick.v % DIGEST_EVERY == 0;
	wire_encode_tick(&s->mirror, &s->sim, &delta, digest, dilation, &s->wire);
	// Each faction's stream is encoded once, whoever is listening.
	ByteBuf stream[FACTION_COUNT];
	for (int f = 0; f < FACTION_COUNT; f++) {
		stream[f] = bytebuf_make(&s->encode, FRAME_MAX / 2);
		wire_encode_faction_tick(&s->sim, (FactionId){(uint32_t)f}, s->queued, n, digest, dilation, &stream[f]);
		s->faction_bytes[f] += stream[f].len;
	}
	for (int k = 0; k < MAX_CLIENTS; k++) {
		Client *c = &s->clients[k];
		if (c->state != CLIENT_READY) continue;
		if (c->faction < FACTION_COUNT) send_or_drop(s, c, MSG_FACTION_TICK, &stream[c->faction], now);
		else send_or_drop(s, c, MSG_DELTA, &delta, now);
	}

	s->queued_count = 0;
	arena_reset(&s->tick);
}

static void report(Server *s, double interval)
{
	int64_t sent = s->bytes_out;
	int connected = 0;
	for (int k = 0; k < MAX_CLIENTS; k++) {
		sent += s->clients[k].conn.bytes_sent;
		connected += s->clients[k].state == CLIENT_READY;
	}
	printf("server: tick %6lld  clients %d  out %7.1f KB/s  step %.2f ms  dilation %3.0f%%  units %td v %td\n",
	       (long long)s->sim.tick.v, connected, (double)(sent - s->bytes_out_last) / 1024.0 / interval,
	       s->step_ms_avg, s->dilation * 100.0, s->sim.factions[0].units.count, s->sim.factions[1].units.count);
	s->bytes_out_last = sent;
	fflush(stdout);
}

// A scenario runs as a script with no actions, so there's one driver.
static void load_driver(Server *s, const char *scenario, const char *script_path)
{
	char err[512];
	if (script_path) {
		if (!script_load(&s->script, &s->program, script_path, err, sizeof err)) fatal("%s", err);
		return;
	}
	const Scenario *sc = scenario_find(scenario);
	if (!sc) fatal("unknown scenario %s", scenario);
	char src[128];
	snprintf(src, sizeof src, "scenario %s\n", sc->name);
	if (!script_parse(&s->script, &s->program, (Str){src, (ptrdiff_t)strlen(src)}, scenario, err, sizeof err))
		fatal("%s", err);
}

int main(int argc, char **argv)
{
	static Server s;
	const char *scenario = "skirmish", *script = nullptr, *record = nullptr, *stats = nullptr;
	int port = 7777;
	int64_t ticks = -1;
	int bot_override[FACTION_COUNT] = {-1, -1};
	for (int i = 1; i < argc;) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : nullptr;
		int used = 2;
		if (!v) fatal("%s needs a value", a);
		if (!strcmp(a, "--scenario")) scenario = v;
		else if (!strcmp(a, "--script")) script = v;
		else if (!strcmp(a, "--port")) port = (int)arg_f(v, 1, 65535, "port");
		else if (!strcmp(a, "--ticks")) ticks = (int64_t)arg_f(v, 1, 1e9, "ticks");
		else if (!strcmp(a, "--record")) record = v;
		else if (!strcmp(a, "--stats")) stats = v;
		else if (!strcmp(a, "--latency")) s.latency = arg_f(v, 0, 5000, "latency") / 1000.0;
		else if (!strcmp(a, "--jitter")) s.jitter = arg_f(v, 0, 5000, "jitter") / 1000.0;
		else if (!strcmp(a, "--dilation")) s.forced_dilation = arg_f(v, 0.05, 1.0, "dilation");
		else if (!strcmp(a, "--speed")) s.speed = arg_f(v, 0.1, 100.0, "speed");
		else if (!strcmp(a, "--bot") && i + 2 < argc) {
			int f = !strcmp(v, "blue") ? 0 : !strcmp(v, "red") ? 1 : -1;
			const char *on = argv[i + 2];
			if (f < 0 || (strcmp(on, "on") && strcmp(on, "off"))) fatal("--bot blue|red on|off");
			bot_override[f] = !strcmp(on, "on");
			used = 3;
		} else fatal("unknown option %s", a);
		i += used;
	}
	signal(SIGPIPE, SIG_IGN);
	if (s.speed == 0.0) s.speed = 1.0;

	s.program = arena_create("program", (ptrdiff_t)1 << 30);
	s.tick = arena_create("tick", (ptrdiff_t)64 << 20);
	s.encode = arena_create("encode", (ptrdiff_t)64 << 20);
	s.replay = arena_create("replay", (ptrdiff_t)4 << 30);
	load_driver(&s, scenario, script);
	sim_init(&s.sim, s.script.config);
	script_run_init(&s.run, &s.script, &s.program, stdout);
	for (int f = 0; f < FACTION_COUNT; f++)
		if (bot_override[f] >= 0) s.run.bots[f].enabled = bot_override[f];
	mirror_init(&s.mirror, &s.program, s.sim.config.max_units, s.sim.config.max_projectiles);
	{
		// Bring the mirror to tick 0 so the first snapshot has the starting armies.
		ByteBuf scratch = bytebuf_make(&s.encode, FRAME_MAX);
		wire_encode_tick(&s.mirror, &s.sim, &scratch, false, 1000, nullptr);
		arena_reset(&s.encode);
	}
	for (int k = 0; k < MAX_CLIENTS; k++)
		conn_init(&s.clients[k].conn, &s.program, -1, CLIENT_OUT, CLIENT_IN);
	if (record) {
		replay_begin(&s.writer, &s.replay, s.sim.config);
		s.recording = true;
	}
	s.listen_fd = net_listen(port);
	if (s.listen_fd < 0) fatal("cannot listen on port %d", port);
	printf("server: %s on port %d, sending with %.0f ms latency, %.0f ms jitter\n",
	       s.script.title[0] ? s.script.title : scenario, port, s.latency * 1e3, s.jitter * 1e3);
	fflush(stdout);

	double next = platform_seconds(), last_report = next;
	s.dilation = 1.0;
	while (ticks < 0 || s.sim.tick.v < ticks) {
		double now = platform_seconds();
		service_clients(&s, now);
		if (now >= next) {
			step(&s, now);
			next += SIM_DT / (s.dilation * s.speed);
			if (next < now - 0.25) next = now; // fell far behind: don't try to catch up
		}
		for (int k = 0; k < MAX_CLIENTS; k++)
			if (s.clients[k].state != CLIENT_FREE) conn_pump(&s.clients[k].conn, platform_seconds());
		if (now - last_report >= 5.0) {
			report(&s, now - last_report);
			last_report = now;
		}
		double wait = next - platform_seconds();
		net_sleep(wait > 0.002 ? 0.002 : wait > 0.0 ? wait : 0.0);
	}
	// Let queued deltas drain before closing.
	for (double end = platform_seconds() + s.latency + s.jitter + 0.5; platform_seconds() < end;) {
		for (int k = 0; k < MAX_CLIENTS; k++)
			if (s.clients[k].state != CLIENT_FREE) conn_pump(&s.clients[k].conn, platform_seconds());
		net_sleep(0.002);
	}
	report(&s, 5.0);

	int64_t sent = s.bytes_out;
	for (int k = 0; k < MAX_CLIENTS; k++) sent += s.clients[k].conn.bytes_sent;
	if (record) {
		Str bytes = replay_bytes(&s.writer);
		if (!platform_write_file(record, bytes.ptr, bytes.len)) fatal("cannot write %s", record);
		printf("server: recorded %lld ticks to %s\n", (long long)s.sim.tick.v, record);
	}
	if (stats) {
		FILE *f = fopen(stats, "w");
		if (!f) fatal("cannot write %s", stats);
		fprintf(f, "ticks %lld\njoins %lld\ncommands_admitted %lld\ncommands_refused %lld\n"
		           "bytes_sent %lld\nunit_entries %lld\nshells_started %lld\nshells_ended %lld\n"
		           "final_hash %016llx\nfaction_stream_bytes %lld %lld\n",
		        (long long)s.sim.tick.v, (long long)s.joins, (long long)s.commands_admitted,
		        (long long)s.commands_refused, (long long)sent, (long long)s.wire.units_sent,
		        (long long)s.wire.shells_started, (long long)s.wire.shells_ended,
		        (unsigned long long)sim_hash_now(&s.sim).v, (long long)s.faction_bytes[0],
		        (long long)s.faction_bytes[1]);
		fclose(f);
	}
	return 0;
}
