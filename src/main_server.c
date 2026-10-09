// rts-server: runs the sim headless, in real time, and streams it to clients.
//
//   rts-server [--scenario NAME | --script PATH] [--port N] [--ticks N]
//              [--zone NAME (--world PATH | --zones A,B,...) --coord HOST:PORT]
//              [--state PATH] [--save-every SECONDS]
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
// through its transporters are reported to the coordinator, and units it
// delivers come in as ARRIVE commands, so the zone's replay still verifies on
// its own. --world (world.h) names the zones, in the order sends number them,
// and the links between them; --zones only names them (tests).
//
// With a world, every send is routed here before the step, whoever ordered
// it (a player, a script, a bot), from the world's links and the
// coordinator's directory (which zones have our transporters):
//   - to a zone with our transporter: paired, hop by hop along the route
//     given or the shortest one through our transporters;
//   - to an adjacent zone without one: unpaired, landing in our strip;
//   - otherwise refused.
// Arrivals wait while their landing area is full (LANDING_ROOM).
//
// Each client plays one player (a slot) of its side, and its commands are
// that player's. A player away (logged out of the world, or never in it) has
// their bot play their units, unless the zone's setup turned it off; see
// update_presence.
//
// In a world, setup scripts only set up: spawning after tick 0 is refused.
//
// --state makes the zone durable: it resumes from PATH if it exists, saves
// there every --save-every seconds, after any tick where units left or
// arrived (before telling the coordinator), and on SIGTERM.
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
#include "world.c"

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
	Leg leg;
	int32_t counts[UNIT_TYPE_COUNT];
} Outgoing;

typedef enum { CLIENT_FREE, CLIENT_HELLO, CLIENT_READY } ClientState;

typedef struct {
	ClientState state;
	Conn conn;
	uint8_t faction;          // 0, 1 or SPECTATOR
	PlayerSlot player;        // which of the faction's players: its commands are theirs
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
	World world;                 // with --world; zone_count 0 without
	int here;                    // this zone's index in the world
	Directory dir;               // the coordinator's, refreshed every few seconds
	bool have_dir;
	BotWorld bot_world[FACTION_COUNT]; // each side's bot's view of the map
	// What each side has seen of the other here (saved with the zone):
	// the last tick it saw enemy units, and enemy transporters (how many).
	int64_t contact_tick[FACTION_COUNT], transporter_tick[FACTION_COUNT];
	uint8_t transporters_seen[FACTION_COUNT];
	bool contact[FACTION_COUNT];
	double next_dir;
	int64_t sends_routed, sends_refused, arrivals_held;
	char coord_host[64];
	int coord_port, port;
	Conn coord;
	double coord_retry_at, next_status;
	Outgoing outgoing[256];      // departures awaiting the coordinator's ack
	uint64_t depart_seq;         // the last departure numbered
	int outgoing_count;
	uint64_t applied[1024];      // transfers recently brought in
	int applied_count;
	bool bot_allowed[FACTION_COUNT][MAX_PLAYERS]; // a player's bot may play them while they're away
	int64_t last_here[FACTION_COUNT][MAX_PLAYERS];  // the last tick each player was logged in; -1: never
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

// Who's playing. In a world, a player is here while logged in anywhere in
// it (the coordinator's directory says), in this zone or not: their units
// here stay theirs, holding, while they play elsewhere. A zone of its own
// counts the clients connected to it. A player gone for AWAY_GRACE (a
// dropped connection, a restart) is away: their bot plays their units, where
// the zone allows bots, and a teammate may take them; a player never seen is
// away from the start. Recorded as COMMAND_AI_SLOTS by the script runner.
static constexpr int64_t AWAY_GRACE = 30 * 30;   // ticks: 30 game seconds

static void update_presence(Server *s)
{
	int64_t now = s->sim.tick.v;
	for (int f = 0; f < FACTION_COUNT; f++) {
		uint8_t here = 0;
		if (s->world.zone_count) {
			if (!s->have_dir) continue; // not knowing isn't leaving
			here = s->dir.online[f];
		} else {
			for (int k = 0; k < MAX_CLIENTS; k++)
				if (s->clients[k].state == CLIENT_READY && s->clients[k].faction == f)
					here |= (uint8_t)(1u << s->clients[k].player.v);
		}
		uint8_t away = 0;
		for (int p = 0; p < s->sim.config.players[f]; p++) {
			if (here >> p & 1) s->last_here[f][p] = now;
			bool gone = s->last_here[f][p] < 0 || now - s->last_here[f][p] >= AWAY_GRACE;
			away |= (uint8_t)(gone << p);
			bool was = s->run.away[f] >> p & 1;
			if (gone != was)
				printf("server %s: %s.%d %s\n", s->zone_name, f ? "red" : "blue", p,
				       gone ? (s->bot_allowed[f][p] ? "is away: its bot plays" : "is away") : "is back");
			s->run.bots[f][p].enabled = gone && s->bot_allowed[f][p];
		}
		s->run.away[f] = away;
	}
	s->run.presence_set = true;
}

static void handle_hello(Server *s, Client *c, ByteReader *r, double now)
{
	uint32_t version = get32(r);
	uint8_t faction = get8(r);
	PlayerSlot player = {get8(r)};
	bool slot_ok = faction == SPECTATOR || (faction < FACTION_COUNT && player.v < s->sim.config.players[faction]);
	if (r->bad || version != WIRE_VERSION || !slot_ok) {
		conn_close(&c->conn, "bad hello");
		return;
	}
	c->faction = faction;
	c->player = player;
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
	bool plays = faction < FACTION_COUNT;
	if (plays) wire_encode_faction_snapshot(&s->sim, (FactionId){faction}, &snap);
	else wire_encode_snapshot(&s->mirror, &snap);
	send_or_drop(s, c, plays ? MSG_FACTION_SNAPSHOT : MSG_SNAPSHOT, &snap, now);
	arena_rewind(m);
	printf("server: client joined as %s.%d at tick %lld (snapshot %td bytes)\n",
	       faction == SPECTATOR ? "spectator" : faction == 0 ? "blue" : "red", player.v,
	       (long long)s->sim.tick.v, snap.len);
}

// Commands are untrusted: only orders, only as the client's own player of its
// own faction, with sane targets; handles (and whose units they are) are
// checked by the sim like anyone's.
static void handle_command(Server *s, Client *c, ByteReader *r)
{
	PlayerSlot to = {get8(r)};
	uint8_t kind = get8(r);
	V2 target = {getf(r), getf(r)};
	uint8_t unit_type = get8(r);
	int32_t amount = (int32_t)get32(r);
	uint8_t flags = get8(r);
	Route route = get32(r);
	uint32_t n = get32(r);
	// Players order, build, produce, send and entrench; spawning is for scripts only.
	// Arriving comes from the coordinator, never players. Whether a send is
	// paired is the server's to say (route_send).
	bool kind_ok = kind <= COMMAND_ATTACK_MOVE || kind == COMMAND_BUILD || kind == COMMAND_PRODUCE || kind == COMMAND_SEND
	            || kind == COMMAND_ENTRENCH || kind == COMMAND_MOBILIZE || kind == COMMAND_GIVE || kind == COMMAND_OUTPUT
	            || kind == COMMAND_SHARE || kind == COMMAND_TAKE;
	bool ok = !r->bad && c->faction < FACTION_COUNT && kind_ok && to.v < s->sim.config.players[c->faction] && unit_type < UNIT_TYPE_COUNT
	       && amount >= 0 && amount <= (kind == COMMAND_PRODUCE ? FACTORY_QUEUE : ZONE_MAX)
	       && flags <= COMMAND_REPEAT && route_ok(route)
	       && n <= (uint32_t)s->sim.config.max_units && (ptrdiff_t)n * 8 == r->len - r->pos
	       && s->queued_count < MAX_COMMANDS;
	if (!ok) {
		s->commands_refused++;
		return;
	}
	UnitHandle *h = PUSH(&s->tick, UnitHandle, n);
	for (uint32_t i = 0; i < n; i++) h[i] = (UnitHandle){get32(r), get32(r)};
	s->queued[s->queued_count++] = (Command){.faction = {c->faction}, .player = c->player, .to = to, .kind = (CommandKind)kind,
	                                         .target = target, .count = n, .units = h,
	                                         .unit_type = (UnitType)unit_type, .amount = amount, .flags = flags,
	                                         .route = route};
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

// ---------------------------------------------------------------- zones

static void send_departure(Server *s, const Outgoing *o, double now)
{
	if (!s->coord.open) return;
	ArenaMark m = arena_mark(&s->encode);
	ByteBuf b = bytebuf_make(&s->encode, 160);
	put64(&b, o->seq);
	put8(&b, o->faction);
	put_name(&b, o->dst);
	put_leg(&b, &o->leg);
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

// How many of a faction's units stand within LANDING_RADIUS of at.
static int32_t landing_crowd(const Faction *f, V2 at)
{
	const Units *u = &f->units;
	int32_t n = 0;
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (!unit_is_structure(u->type[i]) && v2_len2(v2_sub(v2(u->px[i], u->pz[i]), at)) <= LANDING_RADIUS * LANDING_RADIUS)
			n++;
	return n;
}

// Units for this zone, as ARRIVE commands in the next tick: paired ones at
// one of the faction's transporters with room, unpaired ones at their point
// in its strip. No transporter, or no room: hold, and the coordinator offers
// them again later. The arrival is confirmed only once the zone has saved a
// state that holds it.
static void on_deliver(Server *s, ByteReader *r, double now)
{
	uint64_t id = get64(r);
	uint8_t faction = get8(r);
	Leg leg;
	int32_t counts[UNIT_TYPE_COUNT];
	if (!get_leg(r, &leg) || !get_counts(r, counts) || faction >= FACTION_COUNT) return;
	if (was_applied(s, id)) {
		// Already in: confirm (again) unless it's still waiting for its tick.
		for (int k = 0; k < s->applied_count; k++)
			if (s->applied[k] == id) send_id(s, COORD_DELIVERED, id, now);
		return;
	}
	const Faction *f = &s->sim.factions[faction];
	int32_t batch = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) batch += counts[k];
	// Room for every one of them, with what's already arriving this tick: the
	// sim can't spawn past its capacity, and a confirmed arrival must arrive.
	ptrdiff_t arriving = 0;
	for (ptrdiff_t k = 0; k < s->queued_count; k++)
		if (s->queued[k].kind == COMMAND_ARRIVE && s->queued[k].faction.v == faction) arriving += s->queued[k].amount;
	if (f->units.count + arriving + batch > f->units.cap - 16) {
		if (s->arrivals_held++ == 0)
			printf("server %s: transfer %llu waits: no room for more units\n", s->zone_name, (unsigned long long)id);
		send_id(s, COORD_HOLD, id, now);
		return;
	}
	V2 at = v2(0.0f, 0.0f);
	bool room = false, has_landing = false;
	if (leg.unpaired) {
		at = sim_strip_point(&s->sim.terrain, (FactionId){faction}, leg.landing);
		room = landing_crowd(f, at) + batch <= LANDING_ROOM;
	} else {
		for (ptrdiff_t i = 0; i < f->units.count && !room; i++) {
			if (f->units.type[i] != UNIT_TRANSPORTER || f->units.progress[i] < 1.0f) continue;
			has_landing = true;
			at = v2(f->units.px[i], f->units.pz[i]);
			room = landing_crowd(f, at) + batch <= LANDING_ROOM;
		}
	}
	if (!room || s->confirm_count == (int)countof(s->confirm)) {
		if (s->arrivals_held++ == 0)
			printf("server %s: transfer %llu waits: %s\n", s->zone_name, (unsigned long long)id,
			       leg.unpaired || has_landing ? "no room where they'd land" : "no transporter here");
		send_id(s, COORD_HOLD, id, now);
		return;
	}
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		if (!counts[k] || s->queued_count == MAX_COMMANDS) continue;
		s->queued[s->queued_count++] = (Command){.faction = {faction}, .kind = COMMAND_ARRIVE, .target = at,
		                                         .player = leg.owner,
		                                         .unit_type = (UnitType)k, .amount = counts[k], .route = leg.route,
		                                         .flags = leg.unpaired ? COMMAND_UNPAIRED : 0};
	}
	s->confirm[s->confirm_count++] = id;
	printf("server %s: transfer %llu arrives (%s, %s%s)\n", s->zone_name, (unsigned long long)id, faction ? "red" : "blue",
	       leg.unpaired ? "unpaired" : "paired", leg.route ? ", going on" : "");
}

static void on_depart_ack(Server *s, uint64_t seq)
{
	int kept = 0;
	for (int k = 0; k < s->outgoing_count; k++)
		if (s->outgoing[k].seq > seq) s->outgoing[kept++] = s->outgoing[k];
	s->outgoing_count = kept;
}

static uint32_t age_of(const Server *s, int64_t tick);

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
	if (s->world.zone_count && now >= s->next_dir) {
		// Where our transporters are, for routing sends.
		s->next_dir = now + 0.5;
		conn_send(&s->coord, COORD_DIRECTORY_ASK, "", 0, now);
	}
	if (now >= s->next_status) {
		// For the strategic map: who's here, in what strength.
		s->next_status = now + 1.0;
		ZoneStatus st = {.tick = s->sim.tick.v};
		for (int f = 0; f < FACTION_COUNT; f++) {
			const Units *u = &s->sim.factions[f].units;
			int32_t units = 0, structures = 0, transporters = 0;
			for (ptrdiff_t i = 0; i < u->count; i++) {
				if (unit_defs[u->type[i]].speed > 0.0f) units += unit_is_combat(u->type[i]);
				else structures++;
				transporters += u->type[i] == UNIT_TRANSPORTER && u->progress[i] >= 1.0f;
			}
			st.units[f] = (uint16_t)(units < 65535 ? units : 65535);
			st.structures[f] = (uint16_t)(structures < 65535 ? structures : 65535);
			st.transporters[f] = (uint8_t)(transporters < 255 ? transporters : 255);
			st.intel[f] = (Intel){.contact = s->contact[f], .seen_age = age_of(s, s->contact_tick[f]),
			                      .transporters = s->transporters_seen[f],
			                      .transporter_age = age_of(s, s->transporter_tick[f])};
		}
		ArenaMark m = arena_mark(&s->encode);
		ByteBuf b = bytebuf_make(&s->encode, 64);
		put_zone_status(&b, &st);
		conn_send(&s->coord, COORD_ZONE_STATUS, b.data, b.len, now);
		arena_rewind(m);
	}
	conn_pump(&s->coord, now);
	Frame f;
	while (s->coord.open && conn_next(&s->coord, &f)) {
		ByteReader r = byte_reader(f.data, f.len);
		if (f.kind == COORD_DELIVER) on_deliver(s, &r, now);
		else if (f.kind == COORD_DEPART_ACK) on_depart_ack(s, get64(&r));
		else if (f.kind == COORD_DIRECTORY) s->have_dir = get_directory(&r, &s->dir) || s->have_dir;
	}
}

// ---------------------------------------------------------------- the zone's saved state
//
//   "RTSZ" u32 version, config (wire_put_config), u64 last departure number,
//   u32 n, n x outgoing departure, u32 m, m x u64 applied transfer, sim_save

enum { ZONE_STATE_VERSION = 6 };

static void put_outgoing(ByteBuf *b, const Outgoing *o)
{
	put64(b, o->seq);
	put8(b, o->faction);
	put_name(b, o->dst);
	put_leg(b, &o->leg);
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
	for (int f = 0; f < FACTION_COUNT; f++) {
		put64(&b, (uint64_t)s->contact_tick[f]);
		put64(&b, (uint64_t)s->transporter_tick[f]);
		put8(&b, s->transporters_seen[f]);
	}
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
		if (!get_name(&r, o->dst) || !get_leg(&r, &o->leg)) fatal("%s: bad departure", s->state_path);
		if (!get_counts(&r, o->counts) || o->faction >= FACTION_COUNT) fatal("%s: bad departure", s->state_path);
	}
	s->outgoing_count = (int)n;
	uint32_t m = get32(&r);
	if (m > countof(s->applied)) fatal("%s: too many applied transfers", s->state_path);
	for (uint32_t k = 0; k < m; k++) s->applied[k] = get64(&r);
	s->applied_count = (int)m;
	for (int f = 0; f < FACTION_COUNT; f++) {
		s->contact_tick[f] = (int64_t)get64(&r);
		s->transporter_tick[f] = (int64_t)get64(&r);
		s->transporters_seen[f] = get8(&r);
	}
	if (r.bad || !sim_load(&s->sim, &r) || r.pos != r.len) fatal("%s: the sim doesn't load", s->state_path);
	return true;
}

// After a step: what left goes into the outbox, applied arrivals become
// remembered; if either happened the zone saves before telling anyone, so a
// crash can't lose units or bring them twice. Then the coordinator hears.
static bool same_leg(const Departure *a, const Departure *b)
{
	return a->dest == b->dest && a->route == b->route && a->unpaired == b->unpaired && a->landing.x == b->landing.x
	    && a->landing.z == b->landing.z && player_eq(a->owner, b->owner);
}

static void report_to_coord(Server *s, double now)
{
	if (!s->coord_port) return;
	int first_new = s->outgoing_count;
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Faction *fa = &s->sim.factions[f];
		// One departure per destination and leg (units of every type together).
		for (ptrdiff_t k = 0; k < fa->departure_count; k++) {
			const Departure *d = &fa->departures[k];
			bool first = true;
			for (ptrdiff_t j = 0; j < k && first; j++) first = !same_leg(&fa->departures[j], d);
			if (!first) continue;
			const char *dst = d->dest < s->zone_count ? s->zones[d->dest] : nullptr;
			if (!dst) {
				fprintf(stderr, "server %s: units left for zone %d, which isn't in the world\n", s->zone_name, d->dest);
				continue;
			}
			if (s->outgoing_count == (int)countof(s->outgoing)) fatal("server %s: departure outbox full", s->zone_name);
			Outgoing *o = &s->outgoing[s->outgoing_count++];
			*o = (Outgoing){.seq = ++s->depart_seq, .faction = (uint8_t)f,
			                .leg = {.unpaired = d->unpaired, .owner = d->owner, .route = d->route, .landing = d->landing}};
			memcpy(o->dst, dst, strlen(dst) + 1); // zone names fit: world_parse saw to that
			for (ptrdiff_t j = k; j < fa->departure_count; j++)
				if (same_leg(&fa->departures[j], d)) o->counts[fa->departures[j].type] += fa->departures[j].count;
		}
	}
	int confirmed = s->confirm_count;
	for (int k = 0; k < confirmed; k++) remember_applied(s, s->confirm[k]);
	s->confirm_count = 0;
	if (first_new < s->outgoing_count || confirmed) save_zone(s);

	for (int k = first_new; k < s->outgoing_count; k++) send_departure(s, &s->outgoing[k], now);
	for (int k = 0; k < confirmed; k++) send_id(s, COORD_DELIVERED, s->applied[s->applied_count - confirmed + k], now);
}

// ---------------------------------------------------------------- routing sends

// Does faction f have a finished transporter in zone z? Here, from the sim;
// elsewhere, as the coordinator last heard.
static bool has_transporter(const Server *s, int z, uint8_t f)
{
	if (z == s->here) {
		const Units *u = &s->sim.factions[f].units;
		for (ptrdiff_t i = 0; i < u->count; i++)
			if (u->type[i] == UNIT_TRANSPORTER && u->progress[i] >= 1.0f) return true;
		return false;
	}
	for (int k = 0; s->have_dir && k < s->dir.zone_count; k++)
		if (!strcmp(s->dir.zones[k].name, s->world.zones[z].name))
			return s->dir.zones[k].reported && s->dir.zones[k].status.transporters[f] > 0;
	return false;
}

// The shortest way from a to b along links, every zone on the way (not a,
// not b) one with our transporter. The zones after a go to path; 0 if none.
static int shortest(const Server *s, int a, int b, uint8_t f, int path[ZONE_MAX])
{
	int from[COORD_MAX_ZONES], queue[COORD_MAX_ZONES], head = 0, tail = 0;
	for (int k = 0; k < s->world.zone_count; k++) from[k] = -1;
	from[a] = a;
	queue[tail++] = a;
	while (head < tail && from[b] < 0) {
		int z = queue[head++];
		if (z != a && !has_transporter(s, z, f)) continue; // can't relay through here
		for (int k = 0; k < s->world.zone_count; k++)
			if (from[k] < 0 && world_adjacent(&s->world, z, k)) {
				from[k] = z;
				queue[tail++] = k;
			}
	}
	if (from[b] < 0) return 0;
	int n = 0;
	for (int z = b; z != a; z = from[z]) n++;
	int k = n;
	for (int z = b; z != a; z = from[z]) path[--k] = z;
	return n;
}

// A send as ordered (amount: the first zone asked for, route: the rest, the
// last being the destination) becomes the send the sim carries out: the
// next hop, the hops after it, paired or not. False: it can't be done.
static bool route_send(Server *s, Command *c)
{
	if (!s->world.zone_count) return true; // no world (tests): as given
	uint8_t f = (uint8_t)c->faction.v;
	int asked[ROUTE_HOPS + 1], n = 0;
	if (c->amount < 0 || c->amount >= s->world.zone_count) return false;
	asked[n++] = c->amount;
	for (Route r = c->route; route_first(r) >= 0; r = route_rest(r)) {
		if (n == ROUTE_HOPS + 1 || route_first(r) >= s->world.zone_count) return false;
		asked[n++] = route_first(r);
	}
	int dst = asked[n - 1];
	if (dst == s->here) return false;
	int path[ZONE_MAX * (ROUTE_HOPS + 1)], len = 0;
	bool paired = has_transporter(s, dst, f);
	if (!paired) {
		// Without a transporter there: one hop, to a neighbour, and only
		// raiders and engineers go (the sim sees to that).
		if (n != 1 || !world_adjacent(&s->world, s->here, dst)) return false;
		path[len++] = dst;
	} else {
		for (int k = 0, at = s->here; k < n; at = asked[k++]) {
			if (k < n - 1 && !has_transporter(s, asked[k], f)) return false; // a relay needs our transporter
			int m = shortest(s, at, asked[k], f, path + len);
			if (m == 0) return false;
			len += m;
			if (len > ROUTE_HOPS + 1) return false;
		}
	}
	c->amount = path[0];
	c->route = 0;
	for (int k = len - 1; k >= 1; k--) c->route = route_push_front(c->route, path[k]);
	c->flags = paired ? 0 : COMMAND_UNPAIRED;
	if (!paired && c->target.x == 0.0f && c->target.z == 0.0f) {
		// No landing point given: the middle of our strip.
		const Terrain *t = &s->sim.terrain;
		c->target = v2(f == 0 ? t->size_x * STRIP * 0.5f : t->size_x * (1.0f - STRIP * 0.5f), t->size_z * 0.5f);
	}
	return true;
}

// What each side sees of the other, from its perception (the same it aims
// with, and its client gets): enemy units now, and enemy transporters.
static void note_sightings(Server *s)
{
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Perception *p = &s->sim.factions[f].seen;
		int32_t seen = 0, transporters = 0;
		for (ptrdiff_t k = 0; k < p->cap; k++) {
			if (!p->live[k]) continue;
			seen++;
			transporters += p->type[k] == UNIT_TRANSPORTER;
		}
		s->contact[f] = seen > 0;
		if (seen) s->contact_tick[f] = s->sim.tick.v;
		if (transporters) {
			s->transporter_tick[f] = s->sim.tick.v;
			s->transporters_seen[f] = (uint8_t)(transporters < 255 ? transporters : 255);
		}
	}
}

static uint32_t age_of(const Server *s, int64_t tick)
{
	if (tick < 0) return INTEL_NEVER;
	int64_t secs = (s->sim.tick.v - tick) / (int64_t)SIM_HZ;
	return (uint32_t)(secs < 0 ? 0 : secs > 0x7fffffff ? 0x7fffffff : secs);
}

// Each side's bot sees the map as the directory last told it, with this zone
// as it is now.
static void update_bot_worlds(Server *s)
{
	if (!s->world.zone_count || !s->have_dir) return;
	for (int f = 0; f < FACTION_COUNT; f++) {
		BotWorld *w = &s->bot_world[f];
		// Through this side's fog, like a player's map; this zone as it is now.
		Directory fogged = s->dir;
		directory_fog(&fogged, (uint8_t)f);
		bot_world_build(w, &fogged, s->here, (uint8_t)f);
		const Units *u = &s->sim.factions[f].units;
		int32_t mine = 0;
		for (ptrdiff_t i = 0; i < u->count; i++) mine += unit_is_structure(u->type[i]) ? 2 : unit_is_combat(u->type[i]);
		w->mine[s->here] = mine;
		w->ours[s->here] = has_transporter(s, s->here, (uint8_t)f);
		w->contact[s->here] = s->contact[f];
		w->seen_age[s->here] = age_of(s, s->contact_tick[f]);
		w->transporter_age[s->here] = s->transporters_seen[f] ? age_of(s, s->transporter_tick[f]) : BOT_NEVER;
		bot_world_links(w, &fogged);
		for (int p = 0; p < MAX_PLAYERS; p++) s->run.bots[f][p].world = w;
	}
}

static void step(Server *s, double now)
{
	update_presence(s);
	update_bot_worlds(s);
	ptrdiff_t n = s->queued_count;
	n += script_tick(&s->run, &s->sim, &s->tick, s->queued + n, MAX_COMMANDS - n);
	// Every send, whoever ordered it, goes the way the world allows, or not at all.
	ptrdiff_t kept = 0;
	for (ptrdiff_t k = 0; k < n; k++) {
		Command c = s->queued[k];
		if (c.kind == COMMAND_SEND) {
			if (!route_send(s, &c)) {
				s->sends_refused++;
				continue;
			}
			s->sends_routed++;
		}
		s->queued[kept++] = c;
	}
	n = kept;
	Tick tick = s->sim.tick;
	double t0 = platform_seconds();
	sim_step(&s->sim, s->queued, n);
	note_sightings(s);
	double ms = 1e3 * (platform_seconds() - t0);
	s->step_ms_avg = s->step_ms_avg == 0.0 ? ms : s->step_ms_avg * 0.95 + ms * 0.05;
	if (s->recording) replay_tick(&s->writer, tick, s->queued, n, sim_hash_now(&s->sim));
	report_to_coord(s, now);

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
		else if (!strcmp(a, "--world")) {
			Str text;
			char err[256];
			static Arena world_text;
			world_text = arena_create("world file", 1 << 20);
			if (!platform_read_file(&world_text, v, &text)) fatal("cannot read %s", v);
			if (!world_parse(&s.world, text, v, err, sizeof err)) fatal("%s", err);
			for (int k = 0; k < s.world.zone_count; k++) snprintf(s.zones[k], ZONE_NAME, "%s", s.world.zones[k].name);
			s.zone_count = s.world.zone_count;
		} else if (!strcmp(a, "--zones")) {
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
	if (s.world.zone_count) {
		if (!s.zone_name || (s.here = world_zone(&s.world, s.zone_name)) < 0) fatal("--world needs --zone, one of its zones");
	}
	for (int f = 0; f < FACTION_COUNT; f++) s.contact_tick[f] = s.transporter_tick[f] = -1;
	load_driver(&s, scenario, script);
	if (s.world.zone_count) {
		// Fixtures set up; after that, only what a player could do.
		for (ptrdiff_t k = 0; k < s.script.action_count; k++)
			if (s.script.actions[k].kind == ACTION_SPAWN && s.script.actions[k].tick > 0)
				fatal("%s:%d: in a world, a script spawns only at tick 0 (it sets up; it doesn't play)",
				      s.script.path ? s.script.path : "script", s.script.actions[k].line);
	}
	sim_init(&s.sim, s.script.config);
	bool resumed = load_zone(&s, &s.encode);
	arena_reset(&s.encode);
	if (resumed)
		printf("server %s: resumed %s at tick %lld (%d departures to confirm)\n", s.zone_name ? s.zone_name : "zone",
		       s.state_path, (long long)s.sim.tick.v, s.outgoing_count);
	script_run_init(&s.run, &s.script, &s.program, stdout);
	for (int f = 0; f < FACTION_COUNT; f++) {
		for (int p = 0; p < MAX_PLAYERS; p++) {
			if (bot_override[f] >= 0) s.run.bots[f][p].enabled = bot_override[f];
			s.bot_allowed[f][p] = s.run.bots[f][p].enabled;
			// As the state was saved: who was away then is away now; who
			// wasn't gets the grace from here.
			bool away = s.sim.factions[f].away >> p & 1;
			s.last_here[f][p] = away ? -1 : s.sim.tick.v;
		}
		s.run.away[f] = s.run.away_sent[f] = s.sim.factions[f].away;
	}
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
			for (int t = 0; t < UNIT_TYPE_COUNT; t++) outbox += s.outgoing[k].counts[t];
		fprintf(f, "outbox_units %lld\n", (long long)outbox);
		// Each unacknowledged departure: it may already be in the coordinator's
		// log (the ack was lost), so a checker needs the sequence numbers.
		for (int k = 0; k < s.outgoing_count; k++) {
			int64_t n = 0;
			for (int t = 0; t < UNIT_TYPE_COUNT; t++) n += s.outgoing[k].counts[t];
			fprintf(f, "outbox %llu %lld\n", (unsigned long long)s.outgoing[k].seq, (long long)n);
		}
		for (int k = 0; k < FACTION_COUNT; k++) {
			const Units *u = &s.sim.factions[k].units;
			int transporters = 0;
			for (ptrdiff_t i = 0; i < u->count; i++) transporters += u->type[i] == UNIT_TRANSPORTER && u->progress[i] >= 1.0f;
			fprintf(f, "transporters_%s %d\n", k ? "red" : "blue", transporters);
		}
		fprintf(f, "sends_routed %lld\nsends_refused %lld\narrivals_held %lld\n", (long long)s.sends_routed,
		        (long long)s.sends_refused, (long long)s.arrivals_held);
		fclose(f);
	}
	return 0;
}
