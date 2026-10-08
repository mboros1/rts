// rts-coord: the coordinator (coord.h). Zones register here, report units
// leaving through their transporters, and receive units arriving; players ask it
// which zones exist. Its log is its memory: kill it, restart it, and it
// picks up every transfer still owed.
//
//   rts-coord [--port N] [--log PATH] [--retry SECONDS] [--world PATH]
//
// With --world (world.h) it knows the zones, their places on the strategic
// map and the links between them before any zone is up. Either way, players
// get the directory: each zone with its last status report, and the units
// in transit between zones.
//
// Unity build: this file is the whole program.
#include "base.h"

#include <signal.h>

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "coord.c"
#include "world.c"
#include "net.c"

enum { MAX_PEERS = 32 };

typedef struct {
	Conn conn;
	bool used;
	int zone;                   // index into zones, or -1 until it says hello
} Peer;

typedef struct {
	char name[ZONE_NAME];
	int port;                   // where its players connect
	int peer;                   // -1 while down
	float x, y;                 // from the world, if there is one
	int8_t home;
	bool reported;
	ZoneStatus status;
} Zone;

typedef struct {
	Ledger ledger;
	World world;                // link_count 0 without --world
	Zone zones[COORD_MAX_ZONES];
	int zone_count;
	Peer peers[MAX_PEERS];
	Arena program, out;
	double retry;
} Coord;

static Zone *zone_named(Coord *c, const char *name)
{
	for (int k = 0; k < c->zone_count; k++)
		if (!strcmp(c->zones[k].name, name)) return &c->zones[k];
	return nullptr;
}

static void send_to(Coord *c, Peer *p, uint8_t kind, const ByteBuf *b, double now)
{
	(void)c;
	conn_send(&p->conn, kind, b->data, b->len, now);
}

// Offer every pending transfer to its zone, if the zone is up and the
// transfer is due: new, held, or not confirmed in time.
static void deliver(Coord *c, double now)
{
	for (int32_t k = 0; k < c->ledger.transfer_count; k++) {
		Transfer *t = &c->ledger.transfers[k];
		if (t->delivered || now < t->retry_at) continue;
		Zone *z = zone_named(c, t->dst);
		if (!z || z->peer < 0) continue;
		arena_reset(&c->out);
		ByteBuf b = bytebuf_make(&c->out, 256);
		put64(&b, t->id);
		put8(&b, t->faction);
		put_leg(&b, &t->leg);
		put_counts(&b, t->counts);
		send_to(c, &c->peers[z->peer], COORD_DELIVER, &b, now);
		t->retry_at = now + c->retry;
		printf("coord: offered transfer %llu to %s\n", (unsigned long long)t->id, t->dst);
	}
}

static void directory(const Coord *c, Directory *d)
{
	*d = (Directory){.zone_count = c->zone_count, .link_count = c->world.link_count};
	for (int k = 0; k < c->zone_count; k++) {
		const Zone *z = &c->zones[k];
		d->zones[k] = (DirectoryZone){.port = z->port, .up = z->peer >= 0, .x = z->x, .y = z->y, .home = z->home,
		                              .reported = z->reported, .status = z->status};
		snprintf(d->zones[k].name, ZONE_NAME, "%s", z->name);
		snprintf(d->zones[k].host, sizeof d->zones[k].host, "127.0.0.1");
		if (!c->world.zone_count) { // no world file: a row, in order of arrival
			d->zones[k].x = (float)(k + 1) / (float)(c->zone_count + 1);
			d->zones[k].y = 0.5f;
		}
	}
	memcpy(d->links, c->world.links, sizeof d->links);
	// Pending transfers between zones, summed by route and side.
	for (int32_t k = 0; k < c->ledger.transfer_count; k++) {
		const Transfer *t = &c->ledger.transfers[k];
		if (t->delivered) continue;
		int src = -1, dst = -1;
		for (int i = 0; i < c->zone_count; i++) {
			if (!strcmp(c->zones[i].name, t->src)) src = i;
			if (!strcmp(c->zones[i].name, t->dst)) dst = i;
		}
		if (src < 0 || dst < 0) continue;
		int32_t units = 0;
		for (int u = 0; u < UNIT_TYPE_COUNT; u++) units += t->counts[u];
		int i = 0;
		while (i < d->transit_count && !(d->transit[i].src == src && d->transit[i].dst == dst
		                                 && d->transit[i].faction == t->faction)) i++;
		if (i == d->transit_count) {
			if (i == DIRECTORY_MAX_TRANSIT) continue;
			d->transit[d->transit_count++] = (DirectoryTransit){(uint8_t)src, (uint8_t)dst, t->faction, 0};
		}
		d->transit[i].units += units;
	}
}

static void on_frame(Coord *c, int pi, Frame *f, double now)
{
	Peer *p = &c->peers[pi];
	ByteReader r = byte_reader(f->data, f->len);
	Zone *from = p->zone >= 0 ? &c->zones[p->zone] : nullptr;
	char name[ZONE_NAME];
	int32_t counts[UNIT_TYPE_COUNT];
	switch (f->kind) {
	case COORD_ZONE_HELLO: {
		bool ok = get_name(&r, name);
		uint16_t port = get16(&r);
		if (!ok || r.bad) {
			conn_close(&p->conn, "bad zone hello");
			break;
		}
		Zone *z = zone_named(c, name);
		if (!z && c->world.zone_count) {
			conn_close(&p->conn, "no such zone in this world");
			break;
		}
		if (!z && c->zone_count < COORD_MAX_ZONES) {
			z = &c->zones[c->zone_count++];
			*z = (Zone){.peer = -1, .home = -1};
			snprintf(z->name, sizeof z->name, "%s", name);
		}
		if (!z) break;
		z->port = port;
		z->peer = pi;
		p->zone = (int)(z - c->zones);
		// Anything owed to it is due now.
		for (int32_t k = 0; k < c->ledger.transfer_count; k++)
			if (!strcmp(c->ledger.transfers[k].dst, z->name)) c->ledger.transfers[k].retry_at = 0.0;
		printf("coord: zone %s up (players on port %u)\n", z->name, port);
		break;
	}
	case COORD_DEPART: {
		uint64_t seq = get64(&r);
		uint8_t faction = get8(&r);
		bool ok = get_name(&r, name);
		Leg leg;
		if (!from || !ok || faction >= FACTION_COUNT || !get_leg(&r, &leg) || !get_counts(&r, counts)) break;
		if (!zone_named(c, name)) printf("coord: %s sent units to %s, which isn't a zone here\n", from->name, name);
		Transfer *t = ledger_depart(&c->ledger, from->name, seq, name, faction, &leg, counts);
		int32_t units = 0;
		for (int k = 0; k < UNIT_TYPE_COUNT; k++) units += counts[k];
		// The soak test counts units from these lines (the log is compacted).
		if (t) printf("coord: transfer %llu %s -> %s (faction %u, %s, seq %llu, %d units)\n", (unsigned long long)t->id,
		              from->name, name, faction, leg.unpaired ? "unpaired" : "paired", (unsigned long long)seq, units);
		else printf("coord: %s re-sent departure %llu, already logged\n", from->name, (unsigned long long)seq);
		// Logged now, or before: either way the zone can forget it.
		arena_reset(&c->out);
		ByteBuf b = bytebuf_make(&c->out, 16);
		put64(&b, seq);
		send_to(c, p, COORD_DEPART_ACK, &b, now);
		break;
	}
	case COORD_DELIVERED: {
		uint64_t id = get64(&r);
		if (!r.bad) ledger_delivered(&c->ledger, id);
		printf("coord: transfer %llu delivered\n", (unsigned long long)id);
		break;
	}
	case COORD_HOLD: {
		Transfer *t = ledger_find(&c->ledger, get64(&r));
		if (t) t->retry_at = now + c->retry; // it can't take them yet: ask again later
		break;
	}
	case COORD_ZONE_STATUS: {
		ZoneStatus st;
		if (!from || !get_zone_status(&r, &st)) break;
		from->status = st;
		from->reported = true;
		break;
	}
	case COORD_DIRECTORY_ASK: {
		// A side asking sees the map through its fog; anyone else (zones,
		// spectators, tools) sees all of it. Nothing checks who's asking yet.
		Directory d;
		directory(c, &d);
		if (f->len == 1) directory_fog(&d, f->data[0]);
		arena_reset(&c->out);
		ByteBuf b = bytebuf_make(&c->out, 8192);
		put_directory(&b, &d);
		send_to(c, p, COORD_DIRECTORY, &b, now);
		break;
	}
	default:
		conn_close(&p->conn, "unexpected message");
	}
}

int main(int argc, char **argv)
{
	static Coord c;
	int port = 7700;
	const char *log_path = "coord.log", *world_path = nullptr;
	c.retry = 2.0;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--log")) log_path = argv[i + 1];
		else if (!strcmp(argv[i], "--retry")) c.retry = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "--world")) world_path = argv[i + 1];
		else fatal("usage: rts-coord [--port N] [--log PATH] [--retry SECONDS] [--world PATH]");
	}
	signal(SIGPIPE, SIG_IGN);
	c.program = arena_create("coord", (ptrdiff_t)256 << 20);
	c.out = arena_create("coord out", (ptrdiff_t)1 << 20);
	for (int k = 0; k < COORD_MAX_ZONES; k++) c.zones[k] = (Zone){.peer = -1, .home = -1};
	if (world_path) {
		Str text;
		char err[256];
		if (!platform_read_file(&c.program, world_path, &text)) fatal("cannot read %s", world_path);
		if (!world_parse(&c.world, text, world_path, err, sizeof err)) fatal("%s", err);
		for (int k = 0; k < c.world.zone_count; k++) {
			const WorldZone *w = &c.world.zones[k];
			Zone *z = &c.zones[c.zone_count++];
			*z = (Zone){.peer = -1, .x = w->x, .y = w->y, .home = w->home,
			            .port = c.world.port + 1 + k};
			snprintf(z->name, sizeof z->name, "%s", w->name);
		}
	}

	// Rebuild from the log, then keep appending to it.
	Str text;
	if (platform_read_file(&c.program, log_path, &text)) {
		int32_t bad;
		if (!ledger_replay(&c.ledger, text, &bad)) fatal("%s:%d: can't replay this line", log_path, bad);
		printf("coord: replayed %s: %d transfers, %d pending\n", log_path, c.ledger.transfer_count,
		       ledger_pending(&c.ledger));
	}
	if (!(c.ledger.log = fopen(log_path, "a"))) fatal("cannot open %s", log_path);
	// Start from a compact log: what's pending, not everything that ever was.
	if (!ledger_compact(&c.ledger, log_path, &c.program)) fatal("cannot compact %s", log_path);
	printf("coord: compacted %s at startup: %d pending\n", log_path, c.ledger.transfer_count);
	for (int k = 0; k < MAX_PEERS; k++) conn_init(&c.peers[k].conn, &c.program, -1, 1 << 20, 1 << 20);
	int listen_fd = net_listen(port);
	if (listen_fd < 0) fatal("cannot listen on port %d", port);
	printf("coord: listening on port %d, log %s\n", port, log_path);
	fflush(stdout);

	for (;;) {
		double now = platform_seconds();
		int fd;
		while ((fd = net_accept(listen_fd)) >= 0) {
			int k = 0;
			while (k < MAX_PEERS && c.peers[k].used) k++;
			if (k == MAX_PEERS) {
				net_close_fd(fd);
				continue;
			}
			conn_reopen(&c.peers[k].conn, fd);
			c.peers[k].used = true;
			c.peers[k].zone = -1;
		}
		for (int k = 0; k < MAX_PEERS; k++) {
			Peer *p = &c.peers[k];
			if (!p->used) continue;
			conn_pump(&p->conn, now);
			Frame f;
			while (p->conn.open && conn_next(&p->conn, &f)) on_frame(&c, k, &f, now);
			if (!p->conn.open) {
				if (p->zone >= 0 && c.zones[p->zone].peer == k) {
					printf("coord: zone %s down\n", c.zones[p->zone].name);
					c.zones[p->zone].peer = -1;
				}
				p->used = false;
			}
		}
		deliver(&c, now);
		if (c.ledger.lines >= COMPACT_EVERY) {
			int32_t before = c.ledger.transfer_count;
			if (ledger_compact(&c.ledger, log_path, &c.out))
				printf("coord: compacted %s: %d transfers, %d pending\n", log_path, before, c.ledger.transfer_count);
			else
				fprintf(stderr, "coord: cannot compact %s; going on with it as it is\n", log_path);
		}
		for (int k = 0; k < MAX_PEERS; k++)
			if (c.peers[k].used) conn_pump(&c.peers[k].conn, now);
		fflush(stdout);
		net_sleep(0.005);
	}
}
