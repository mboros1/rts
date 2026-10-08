// rts-coord: the coordinator (coord.h). Zones register here, report units
// leaving through their gates, and receive units arriving; players ask it
// which zones exist. Its log is its memory: kill it, restart it, and it
// picks up every transfer still owed.
//
//   rts-coord [--port N] [--log PATH] [--retry SECONDS]
//
// Unity build: this file is the whole program.
#include "base.h"

#include <signal.h>

#include "platform.c"
#include "arena.c"
#include "terrain.c"
#include "sim.c"
#include "coord.c"
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
} Zone;

typedef struct {
	Ledger ledger;
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
		put8(&b, t->seed);
		put_counts(&b, t->counts);
		send_to(c, &c->peers[z->peer], COORD_DELIVER, &b, now);
		t->retry_at = now + c->retry;
		printf("coord: offered transfer %llu to %s\n", (unsigned long long)t->id, t->dst);
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
		if (!z && c->zone_count < COORD_MAX_ZONES) {
			z = &c->zones[c->zone_count++];
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
		uint8_t seed = get8(&r);
		if (!from || !ok || faction >= FACTION_COUNT || !get_counts(&r, counts)) break;
		Transfer *t = ledger_depart(&c->ledger, from->name, seq, name, faction, seed, counts);
		if (t) printf("coord: transfer %llu %s -> %s (faction %u%s)\n", (unsigned long long)t->id, from->name, name,
		              faction, seed ? ", a gate seed" : "");
		else printf("coord: %s re-sent departure %llu, already logged\n", from->name, (unsigned long long)seq);
		// Logged now, or before: either way the zone can forget it.
		arena_reset(&c->out);
		ByteBuf b = bytebuf_make(&c->out, 16);
		put64(&b, seq);
		send_to(c, p, COORD_DEPART_ACK, &b, now);
		break;
	}
	case COORD_DRAW: {
		uint8_t faction = get8(&r);
		if (!from || faction >= FACTION_COUNT || !get_counts(&r, counts)) break;
		Transfer *t = ledger_draw(&c->ledger, faction, from->name, counts);
		if (t) printf("coord: draw %llu from faction %u's pool to %s\n", (unsigned long long)t->id, faction, from->name);
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
		if (t) t->retry_at = now + c->retry; // no gate there yet: ask again later
		break;
	}
	case COORD_DIRECTORY_ASK: {
		arena_reset(&c->out);
		ByteBuf b = bytebuf_make(&c->out, 1024);
		put8(&b, (uint8_t)c->zone_count);
		for (int k = 0; k < c->zone_count; k++) {
			put_name(&b, c->zones[k].name);
			put_name(&b, "127.0.0.1");
			put16(&b, (uint16_t)c->zones[k].port);
			put8(&b, c->zones[k].peer >= 0);
		}
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
	const char *log_path = "coord.log";
	c.retry = 2.0;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--log")) log_path = argv[i + 1];
		else if (!strcmp(argv[i], "--retry")) c.retry = atof(argv[i + 1]);
		else fatal("usage: rts-coord [--port N] [--log PATH] [--retry SECONDS]");
	}
	signal(SIGPIPE, SIG_IGN);
	c.program = arena_create("coord", (ptrdiff_t)256 << 20);
	c.out = arena_create("coord out", (ptrdiff_t)1 << 20);

	// Rebuild from the log, then keep appending to it.
	Str text;
	if (platform_read_file(&c.program, log_path, &text)) {
		int32_t bad;
		if (!ledger_replay(&c.ledger, text, &bad)) fatal("%s:%d: can't replay this line", log_path, bad);
		printf("coord: replayed %s: %d transfers, %d pending\n", log_path, c.ledger.transfer_count,
		       ledger_pending(&c.ledger));
	}
	if (!(c.ledger.log = fopen(log_path, "a"))) fatal("cannot open %s", log_path);
	for (int k = 0; k < COORD_MAX_ZONES; k++) c.zones[k].peer = -1;
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
		for (int k = 0; k < MAX_PEERS; k++)
			if (c.peers[k].used) conn_pump(&c.peers[k].conn, now);
		fflush(stdout);
		net_sleep(0.005);
	}
}
