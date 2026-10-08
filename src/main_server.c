// rts-server: runs the sim headless, in real time, and streams it to clients.
//
//   rts-server [--scenario NAME | --script PATH] [--port N] [--ticks N]
//              [--zone NAME --zones A,B,... --coord HOST:PORT]
//              [--state PATH] [--save-every SECONDS] [--logout hold|entrench|bot]
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
// With --coord, this is one zone of a larger match (coord.h): units leaving
// through its gates are reported to the coordinator, and units it delivers
// come in through this zone's gates as ARRIVE commands, so the zone's replay
// still verifies on its own. --zones names the match's zones, in the order
// gate commands number them.
//
// --state makes the zone durable: it resumes from PATH if it exists, saves
// there every --save-every seconds, after any tick where units left or
// arrived (before telling the coordinator), and on SIGTERM.
//
// --logout: when a faction's last player leaves, its combat units entrench
// as turrets (and mobilize when a player returns), or its bot takes over,
// or nothing happens (hold, the default).
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
#include "coord.c"

enum { MAX_CLIENTS = 8, MAX_COMMANDS = 512 };
static constexpr ptrdiff_t CLIENT_OUT = (ptrdiff_t)32 << 20;  // power of two
static constexpr ptrdiff_t CLIENT_IN = (ptrdiff_t)1 << 20;

// A departure the coordinator hasn't acknowledged yet. Kept in the zone's
// saved state and re-sent after a restart; the coordinator recognises a
// repeat by its number.
typedef struct {
	uint64_t seq;
	uint8_t faction;
	char dst[ZONE_NAME];
	uint8_t seed;
	int32_t counts[UNIT_TYPE_COUNT];
} Outgoing;

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
	FILE *record;                // the replay streams here, flushed at each save
	Command queued[MAX_COMMANDS];
	ptrdiff_t queued_count;

	double step_ms_avg;
	double forced_dilation;   // 0: automatic
	double speed;             // clock multiplier; 1 is real time
	double dilation;
	// One zone of many (with --coord).
	const char *zone_name;
	char zones[COORD_MAX_ZONES][ZONE_NAME];
	int zone_count;
	char coord_host[64];
	int coord_port, port;
	Conn coord;
	double coord_retry_at;
	Outgoing outgoing[256];      // departures awaiting the coordinator's ack
	uint64_t depart_seq;         // the last departure numbered
	int outgoing_count;
	uint64_t applied[1024];      // transfers recently brought in
	int applied_count;
	// Logging out (spike 6): what a faction does when its last player leaves.
	enum { LOGOUT_HOLD, LOGOUT_ENTRENCH, LOGOUT_BOT } logout;
	int players[FACTION_COUNT];
	// Persistence (spike 6).
	const char *state_path;
	double save_every, next_save;
	int64_t saves;
	ptrdiff_t save_bytes;
	double save_ms;
	uint64_t confirm[64];        // brought in this tick: confirm after the step
	int confirm_count;
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

// Logging out and back in, as commands in the next tick so replays have them.
static void queue_faction_command(Server *s, uint8_t faction, CommandKind kind)
{
	if (s->queued_count < MAX_COMMANDS) s->queued[s->queued_count++] = (Command){.faction = {faction}, .kind = kind};
}

static void last_player_leaves(Server *s, uint8_t faction)
{
	if (s->logout == LOGOUT_ENTRENCH) queue_faction_command(s, faction, COMMAND_ENTRENCH);
	if (s->logout == LOGOUT_BOT) s->run.bots[faction].enabled = true;
	if (s->logout != LOGOUT_HOLD)
		printf("server %s: %s's last player left: %s\n", s->zone_name, faction ? "red" : "blue",
		       s->logout == LOGOUT_ENTRENCH ? "entrenching" : "the bot takes over");
}

static void player_returns(Server *s, uint8_t faction)
{
	if (s->logout == LOGOUT_ENTRENCH) queue_faction_command(s, faction, COMMAND_MOBILIZE);
	if (s->logout == LOGOUT_BOT) s->run.bots[faction].enabled = false;
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
	if (faction < FACTION_COUNT && s->players[faction]++ == 0) player_returns(s, faction);
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
	// Arriving and spawning come from the coordinator and scripts, never players.
	bool kind_ok = kind <= COMMAND_ATTACK_MOVE || kind == COMMAND_BUILD || kind == COMMAND_PRODUCE
	            || kind == COMMAND_GATE || kind == COMMAND_SEED || kind == COMMAND_DRAW;
	bool ok = !r->bad && c->faction < FACTION_COUNT && kind_ok && unit_type < UNIT_TYPE_COUNT
	       && amount >= 0 && amount <= (kind == COMMAND_PRODUCE ? FACTORY_QUEUE : kind == COMMAND_DRAW ? SPAWN_MAX : ZONE_POOL)
	       && flags <= COMMAND_REPEAT
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
			if (c->state == CLIENT_READY && c->faction < FACTION_COUNT && --s->players[c->faction] == 0)
				last_player_leaves(s, c->faction);
			c->state = CLIENT_FREE;
		}
	}
}

// ---------------------------------------------------------------- ticking

// ---------------------------------------------------------------- zones

static void send_departure(Server *s, const Outgoing *o, double now)
{
	if (!s->coord.open) return;
	ArenaMark m = arena_mark(&s->encode);
	ByteBuf b = bytebuf_make(&s->encode, 160);
	put64(&b, o->seq);
	put8(&b, o->faction);
	put_name(&b, o->dst);
	put8(&b, o->seed);
	put_counts(&b, o->counts);
	conn_send(&s->coord, COORD_DEPART, b.data, b.len, now);
	arena_rewind(m);
}

static void send_id(Server *s, uint8_t kind, uint64_t id, double now)
{
	if (!s->coord.open) return;
	uint8_t b[8];
	for (int k = 0; k < 8; k++) b[k] = (uint8_t)(id >> (8 * k));
	conn_send(&s->coord, kind, b, 8, now);
}

static bool was_applied(const Server *s, uint64_t id)
{
	for (int k = 0; k < s->applied_count; k++)
		if (s->applied[k] == id) return true;
	for (int k = 0; k < s->confirm_count; k++)
		if (s->confirm[k] == id) return true;
	return false;
}

// Remember an applied transfer; the oldest are forgotten, since the
// coordinator stops offering a transfer once it's confirmed.
static void remember_applied(Server *s, uint64_t id)
{
	if (s->applied_count == (int)countof(s->applied)) {
		memmove(s->applied, s->applied + 1, sizeof s->applied - sizeof s->applied[0]);
		s->applied_count--;
	}
	s->applied[s->applied_count++] = id;
}

// Units for this zone: through the faction's gate, as ARRIVE commands in the
// next tick. A seed lands near the faction's base. No gate yet: hold. The
// arrival is confirmed only once the zone has saved a state that holds it.
static void on_deliver(Server *s, ByteReader *r, double now)
{
	uint64_t id = get64(r);
	uint8_t faction = get8(r), seed = get8(r);
	int32_t counts[UNIT_TYPE_COUNT];
	if (!get_counts(r, counts) || faction >= FACTION_COUNT) return;
	if (was_applied(s, id)) {
		// Already in: confirm (again) unless it's still waiting for its tick.
		for (int k = 0; k < s->applied_count; k++)
			if (s->applied[k] == id) send_id(s, COORD_DELIVERED, id, now);
		return;
	}
	const Faction *f = &s->sim.factions[faction];
	V2 at = f->base;
	bool has_gate = false;
	for (ptrdiff_t i = 0; i < f->units.count && !has_gate; i++)
		if (f->units.type[i] == UNIT_GATE && f->units.progress[i] >= 1.0f) {
			has_gate = true;
			at = v2(f->units.px[i], f->units.pz[i]);
		}
	if (!seed && !has_gate) {
		send_id(s, COORD_HOLD, id, now);
		return;
	}
	if (s->confirm_count == (int)countof(s->confirm)) return; // busy: the coordinator will offer again
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		if (!counts[k] || s->queued_count == MAX_COMMANDS) continue;
		s->queued[s->queued_count++] = (Command){.faction = {faction}, .kind = COMMAND_ARRIVE, .target = at,
		                                         .unit_type = (UnitType)k, .amount = counts[k],
		                                         .flags = seed ? COMMAND_SEED_GATE : 0};
	}
	s->confirm[s->confirm_count++] = id;
	printf("server %s: transfer %llu arrives (%s)\n", s->zone_name, (unsigned long long)id,
	       seed ? "a gate seed" : faction ? "red" : "blue");
}

static void on_depart_ack(Server *s, uint64_t seq)
{
	int kept = 0;
	for (int k = 0; k < s->outgoing_count; k++)
		if (s->outgoing[k].seq > seq) s->outgoing[kept++] = s->outgoing[k];
	s->outgoing_count = kept;
}

static void coord_service(Server *s, double now)
{
	if (!s->coord_port) return;
	if (!s->coord.open && now >= s->coord_retry_at) {
		s->coord_retry_at = now + 0.5;
		int fd = net_connect(s->coord_host, s->coord_port);
		if (fd >= 0) {
			conn_reopen(&s->coord, fd);
			ArenaMark m = arena_mark(&s->encode);
			ByteBuf b = bytebuf_make(&s->encode, 64);
			put_name(&b, s->zone_name);
			put16(&b, (uint16_t)s->port);
			conn_send(&s->coord, COORD_ZONE_HELLO, b.data, b.len, now);
			arena_rewind(m);
			// Whatever it hasn't acknowledged, it may not have: send it all.
			for (int k = 0; k < s->outgoing_count; k++) send_departure(s, &s->outgoing[k], now);
			printf("server %s: registered with the coordinator (%d departures to confirm)\n", s->zone_name,
			       s->outgoing_count);
		}
	}
	if (!s->coord.open) return;
	conn_pump(&s->coord, now);
	Frame f;
	while (s->coord.open && conn_next(&s->coord, &f)) {
		ByteReader r = byte_reader(f.data, f.len);
		if (f.kind == COORD_DELIVER) on_deliver(s, &r, now);
		else if (f.kind == COORD_DEPART_ACK) on_depart_ack(s, get64(&r));
	}
}

// ---------------------------------------------------------------- the zone's saved state
//
//   "RTSZ" u32 version, config (wire_put_config), u64 last departure number,
//   u32 n, n x outgoing departure, u32 m, m x u64 applied transfer, sim_save

enum { ZONE_STATE_VERSION = 1 };

static void put_outgoing(ByteBuf *b, const Outgoing *o)
{
	put64(b, o->seq);
	put8(b, o->faction);
	put_name(b, o->dst);
	put8(b, o->seed);
	put_counts(b, o->counts);
}

static bool save_zone(Server *s)
{
	// The replay first: on disk it always reaches at least the saved tick.
	if (s->record && !replay_flush(&s->writer, s->record)) fprintf(stderr, "server: cannot write the replay\n");
	if (!s->state_path) return true;
	double t0 = platform_seconds();
	ArenaMark m = arena_mark(&s->encode);
	ByteBuf b = bytebuf_make(&s->encode, (ptrdiff_t)48 << 20);
	put_bytes(&b, "RTSZ", 4);
	put32(&b, ZONE_STATE_VERSION);
	wire_put_config(&b, &s->sim.config);
	put64(&b, s->depart_seq);
	put32(&b, (uint32_t)s->outgoing_count);
	for (int k = 0; k < s->outgoing_count; k++) put_outgoing(&b, &s->outgoing[k]);
	put32(&b, (uint32_t)s->applied_count);
	for (int k = 0; k < s->applied_count; k++) put64(&b, s->applied[k]);
	sim_save(&s->sim, &b);
	bool ok = !b.overflow && platform_write_file_atomic(s->state_path, b.data, b.len);
	if (!ok) fprintf(stderr, "server %s: cannot save %s\n", s->zone_name, s->state_path);
	s->saves++;
	s->save_bytes = b.len;
	s->save_ms = 1e3 * (platform_seconds() - t0);
	arena_rewind(m);
	return ok;
}

// Resume from a saved state, if there is one. Its config must be this run's.
static bool load_zone(Server *s, Arena *scratch)
{
	Str file;
	if (!s->state_path || !platform_read_file(scratch, s->state_path, &file)) return false;
	ByteReader r = byte_reader(file.ptr, file.len);
	const uint8_t *magic = take(&r, 4);
	SimConfig config;
	if (!magic || memcmp(magic, "RTSZ", 4) || get32(&r) != ZONE_STATE_VERSION || !wire_get_config(&r, &config)
	    || !sim_config_equal(&config, &s->sim.config))
		fatal("%s isn't this zone's saved state", s->state_path);
	s->depart_seq = get64(&r);
	uint32_t n = get32(&r);
	if (n > countof(s->outgoing)) fatal("%s: too many departures", s->state_path);
	for (uint32_t k = 0; k < n; k++) {
		Outgoing *o = &s->outgoing[k];
		o->seq = get64(&r);
		o->faction = get8(&r);
		if (!get_name(&r, o->dst)) fatal("%s: bad departure", s->state_path);
		o->seed = get8(&r);
		if (!get_counts(&r, o->counts) || o->faction >= FACTION_COUNT) fatal("%s: bad departure", s->state_path);
	}
	s->outgoing_count = (int)n;
	uint32_t m = get32(&r);
	if (m > countof(s->applied)) fatal("%s: too many applied transfers", s->state_path);
	for (uint32_t k = 0; k < m; k++) s->applied[k] = get64(&r);
	s->applied_count = (int)m;
	if (r.bad || !sim_load(&s->sim, &r) || r.pos != r.len) fatal("%s: the sim doesn't load", s->state_path);
	return true;
}

// After a step: what left goes into the outbox, applied arrivals become
// remembered; if either happened the zone saves before telling anyone, so a
// crash can't lose units or bring them twice. Then the coordinator hears.
static void report_to_coord(Server *s, const Command *cmds, ptrdiff_t n, double now)
{
	if (!s->coord_port) return;
	int first_new = s->outgoing_count;
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Faction *fa = &s->sim.factions[f];
		// One departure per destination, and one per seed.
		for (ptrdiff_t k = 0; k < fa->departure_count; k++) {
			const Departure *d = &fa->departures[k];
			bool seed = d->type == UNIT_GATE, first = true;
			for (ptrdiff_t j = 0; j < k && first; j++)
				first = fa->departures[j].dest != d->dest || (fa->departures[j].type == UNIT_GATE) != seed;
			if (!first) continue;
			const char *dst = d->dest == ZONE_POOL ? "pool" : d->dest < s->zone_count ? s->zones[d->dest] : nullptr;
			if (!dst) {
				fprintf(stderr, "server %s: units left for zone %d, which isn't in --zones\n", s->zone_name, d->dest);
				continue;
			}
			if (s->outgoing_count == (int)countof(s->outgoing)) fatal("server %s: departure outbox full", s->zone_name);
			Outgoing *o = &s->outgoing[s->outgoing_count++];
			*o = (Outgoing){.seq = ++s->depart_seq, .faction = (uint8_t)f, .seed = seed};
			snprintf(o->dst, sizeof o->dst, "%s", dst);
			for (ptrdiff_t j = k; j < fa->departure_count; j++)
				if (fa->departures[j].dest == d->dest && (fa->departures[j].type == UNIT_GATE) == seed)
					o->counts[fa->departures[j].type] += fa->departures[j].count;
		}
	}
	int confirmed = s->confirm_count;
	for (int k = 0; k < confirmed; k++) remember_applied(s, s->confirm[k]);
	s->confirm_count = 0;
	if (first_new < s->outgoing_count || confirmed) save_zone(s);

	for (int k = first_new; k < s->outgoing_count; k++) send_departure(s, &s->outgoing[k], now);
	for (int k = 0; k < confirmed; k++) send_id(s, COORD_DELIVERED, s->applied[s->applied_count - confirmed + k], now);
	for (ptrdiff_t k = 0; k < n; k++) {
		if (cmds[k].kind != COMMAND_DRAW || cmds[k].unit_type >= UNIT_TYPE_COUNT || !s->coord.open) continue;
		int32_t counts[UNIT_TYPE_COUNT] = {0};
		counts[cmds[k].unit_type] = cmds[k].amount;
		ArenaMark m = arena_mark(&s->encode);
		ByteBuf b = bytebuf_make(&s->encode, 64);
		put8(&b, (uint8_t)cmds[k].faction.v);
		put_counts(&b, counts);
		conn_send(&s->coord, COORD_DRAW, b.data, b.len, now);
		arena_rewind(m);
	}
}

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
	report_to_coord(s, s->queued, n, now);

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

// SIGTERM and SIGINT: finish the tick, save, and leave.
static volatile sig_atomic_t stopping;
static void on_stop(int sig)
{
	(void)sig;
	stopping = 1;
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
		} else if (!strcmp(a, "--zone")) s.zone_name = v;
		else if (!strcmp(a, "--state")) s.state_path = v;
		else if (!strcmp(a, "--save-every")) s.save_every = arg_f(v, 0.1, 3600, "save-every");
		else if (!strcmp(a, "--logout")) {
			if (!strcmp(v, "hold")) s.logout = LOGOUT_HOLD;
			else if (!strcmp(v, "entrench")) s.logout = LOGOUT_ENTRENCH;
			else if (!strcmp(v, "bot")) s.logout = LOGOUT_BOT;
			else fatal("--logout hold|entrench|bot");
		}
		else if (!strcmp(a, "--zones")) {
			for (const char *z = v; *z && s.zone_count < COORD_MAX_ZONES;) {
				const char *comma = strchr(z, ',');
				size_t len = comma ? (size_t)(comma - z) : strlen(z);
				if (len == 0 || len >= ZONE_NAME) fatal("bad --zones %s", v);
				memcpy(s.zones[s.zone_count], z, len);
				s.zones[s.zone_count++][len] = 0;
				z += len + (comma != nullptr);
			}
		} else if (!strcmp(a, "--coord")) {
			const char *colon = strrchr(v, ':');
			if (!colon || colon - v >= (ptrdiff_t)sizeof s.coord_host) fatal("--coord HOST:PORT");
			memcpy(s.coord_host, v, (size_t)(colon - v));
			s.coord_port = atoi(colon + 1);
		} else fatal("unknown option %s", a);
		i += used;
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_stop);
	signal(SIGINT, on_stop);
	if (s.save_every == 0.0) s.save_every = 10.0;
	if (s.speed == 0.0) s.speed = 1.0;

	s.program = arena_create("program", (ptrdiff_t)1 << 30);
	s.tick = arena_create("tick", (ptrdiff_t)64 << 20);
	s.encode = arena_create("encode", (ptrdiff_t)64 << 20);
	s.replay = arena_create("replay", (ptrdiff_t)4 << 30);
	load_driver(&s, scenario, script);
	sim_init(&s.sim, s.script.config);
	bool resumed = load_zone(&s, &s.encode);
	arena_reset(&s.encode);
	if (resumed)
		printf("server %s: resumed %s at tick %lld (%d departures to confirm)\n", s.zone_name ? s.zone_name : "zone",
		       s.state_path, (long long)s.sim.tick.v, s.outgoing_count);
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
		// A resumed zone records a segment starting from where it picked up.
		ByteBuf snap = bytebuf_make(&s.encode, (ptrdiff_t)48 << 20);
		if (resumed) sim_save(&s.sim, &snap);
		replay_begin_from(&s.writer, &s.replay, s.sim.config, (Str){(const char *)snap.data, snap.len});
		arena_reset(&s.encode);
		if (!(s.record = fopen(record, "wb"))) fatal("cannot write %s", record);
		replay_flush(&s.writer, s.record);
		s.recording = true;
	}
	s.port = port;
	if (s.coord_port) {
		if (!s.zone_name) fatal("--coord needs --zone NAME");
		conn_init(&s.coord, &s.program, -1, 1 << 20, 4 << 20);

	}
	if (!s.zone_name) s.zone_name = "zone";
	s.listen_fd = net_listen(port);
	if (s.listen_fd < 0) fatal("cannot listen on port %d", port);
	printf("server: %s on port %d, sending with %.0f ms latency, %.0f ms jitter\n",
	       s.script.title[0] ? s.script.title : scenario, port, s.latency * 1e3, s.jitter * 1e3);
	fflush(stdout);

	double next = platform_seconds(), last_report = next;
	s.dilation = 1.0;
	s.next_save = platform_seconds() + s.save_every;
	while ((ticks < 0 || s.sim.tick.v < ticks) && !stopping) {
		double now = platform_seconds();
		service_clients(&s, now);
		coord_service(&s, now);
		if (now >= next) {
			step(&s, now);
			next += SIM_DT / (s.dilation * s.speed);
			if (next < now - 0.25) next = now; // fell far behind: don't try to catch up
		}
		for (int k = 0; k < MAX_CLIENTS; k++)
			if (s.clients[k].state != CLIENT_FREE) conn_pump(&s.clients[k].conn, platform_seconds());
		if ((s.state_path || s.record) && now >= s.next_save) {
			save_zone(&s);
			s.next_save = now + s.save_every;
		}
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
		if (s.coord.open) conn_pump(&s.coord, platform_seconds());
		net_sleep(0.002);
	}
	report(&s, 5.0);
	save_zone(&s); // also flushes the replay

	int64_t sent = s.bytes_out;
	for (int k = 0; k < MAX_CLIENTS; k++) sent += s.clients[k].conn.bytes_sent;
	if (s.record) {
		replay_flush(&s.writer, s.record);
		fclose(s.record);
		printf("server: recorded to tick %lld in %s\n", (long long)s.sim.tick.v, record);
	}
	if (stats) {
		FILE *f = fopen(stats, "w");
		if (!f) fatal("cannot write %s", stats);
		fprintf(f, "ticks %lld\njoins %lld\ncommands_admitted %lld\ncommands_refused %lld\n"
		           "bytes_sent %lld\nunit_entries %lld\nshells_started %lld\nshells_ended %lld\n"
		           "final_hash %016llx\nfaction_stream_bytes %lld %lld\nunits_in %lld\nunits_out %lld\n",
		        (long long)s.sim.tick.v, (long long)s.joins, (long long)s.commands_admitted,
		        (long long)s.commands_refused, (long long)sent, (long long)s.wire.units_sent,
		        (long long)s.wire.shells_started, (long long)s.wire.shells_ended,
		        (unsigned long long)sim_hash_now(&s.sim).v, (long long)s.faction_bytes[0],
		        (long long)s.faction_bytes[1],
		        (long long)(s.sim.factions[0].stats.units_arrived + s.sim.factions[1].stats.units_arrived),
		        (long long)(s.sim.factions[0].stats.units_departed + s.sim.factions[1].stats.units_departed));
		fprintf(f, "saves %lld\nsave_bytes %td\nsave_ms %.2f\ntick %lld\n", (long long)s.saves, s.save_bytes,
		        s.save_ms, (long long)s.sim.tick.v);
		int64_t outbox = 0;
		for (int k = 0; k < s.outgoing_count; k++)
			for (int t = 0; t < UNIT_TYPE_COUNT; t++) outbox += s.outgoing[k].seed ? 0 : s.outgoing[k].counts[t];
		fprintf(f, "outbox_units %lld\n", (long long)outbox);
		// Each unacknowledged departure: it may already be in the coordinator's
		// log (the ack was lost), so a checker needs the sequence numbers.
		for (int k = 0; k < s.outgoing_count; k++) {
			int64_t n = 0;
			for (int t = 0; t < UNIT_TYPE_COUNT; t++) n += s.outgoing[k].seed ? 0 : s.outgoing[k].counts[t];
			fprintf(f, "outbox %llu %lld\n", (unsigned long long)s.outgoing[k].seq, (long long)n);
		}
		for (int k = 0; k < FACTION_COUNT; k++) {
			const Units *u = &s.sim.factions[k].units;
			int gates = 0;
			for (ptrdiff_t i = 0; i < u->count; i++) gates += u->type[i] == UNIT_GATE && u->progress[i] >= 1.0f;
			fprintf(f, "gates_%s %d\n", k ? "red" : "blue", gates);
		}
		fclose(f);
	}
	return 0;
}
