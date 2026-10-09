// The network client: draws what the server sends, sends what you order.
//
//   rts --connect HOST:PORT [--faction blue|red|spectator] [--slot N]
//       [--latency MS] [--jitter MS]       delay what this client sends
//       [--netlog PATH] [--stats PATH]
//       [--headless] [--auto-orders]       no window; order idle units forward
//       [--disconnect-at TICK] [--reconnect-after SECONDS] [--quit-at TICK]
//   rts --coord HOST:PORT [--zone NAME]   a world, through its coordinator
//       [--faction blue|red|spectator] [--name NAME]   who you are (default $USER):
//                                          the coordinator gives NAME a player slot
//       [--switch-to NAME --switch-at N] [--quit-after N]   (N: ticks received)
//       [--journey STEP,STEP,...]          scripted: map:SECONDS or ZONE:SECONDS
//       [--bot] [--doctrine SPEC]          the bot plays your side, as a player (bot.h)
//
// With a coordinator and no --zone, the window opens on the strategic map
// (map.h): click a zone to launch into it; Tab, or Esc when not placing a
// structure, goes back to the map. Each zone is its own match: launching
// joins it from its snapshot. --journey scripts the same for demos and tests:
// "map:3,west:30,map:3,east:20" looks at the map for 3 s, plays west for 30 s,
// and so on, then quits.
//
// --bot runs the game's bot on this client's own view of its faction and
// sends its orders like a player's: a stand-in player for demos and tests
// that can only do what a player can.
//
// A player simulates its own faction (remote.h): from a faction snapshot and
// each tick's commands and effects it runs the faction's phase itself and
// checks the faction hash against the server's every second. It only ever
// knows the enemies its faction can see, and draws the rest as fog.
//
// A spectator keeps a Mirror of everything instead (wire.h), dead-reckoned.
//
// Either way it reconnects on its own and starts again from a snapshot.

#include <math.h>

#include "bot.h"
#include "controls.h"
#include "coord.h"
#include "map.h"
#include "net.h"
#include "netlog.h"
#include "platform.h"
#include "remote.h"
#include "render.h"
#include "scene.h"
#include "view.h"
#include "wire.h"

typedef struct {
	const char *host;
	int port;
	uint8_t faction;
	const char *name;              // --name: who plays, in a world
	PlayerSlot slot;               // --slot, or the coordinator's answer
	double latency, jitter;
	const char *netlog_path, *stats_path;
	bool headless, auto_orders;
	int64_t disconnect_at, quit_at;
	double reconnect_after;
	const char *coord, *zone, *switch_to;
	int64_t switch_at, quit_after;
	bool bot;
	const char *doctrine;          // --doctrine, for --bot
	char journey[16][ZONE_NAME];   // "map" or a zone name, each for journey_s seconds
	double journey_s[16];
	int journey_len;
} ClientOptions;

typedef struct {
	ClientOptions opt;
	Conn conn;
	bool ever_connected;
	double disconnected_since;  // or since the last failed retry
	double lost_at;             // when the connection went, for giving up
	bool forced_done;

	bool have_world;
	SimConfig config;
	Terrain terrain;
	V2 base[FACTION_COUNT];
	bool player;                // simulates its faction; else a spectator
	Remote remote;              // players
	Mirror mirror;              // spectators
	bool ready;                 // has a snapshot
	double last_delta_at;

	Arena program, frame, out;
	Arena world;                // this zone's terrain and state: reset on a switch
	Directory dir;              // the world, as the coordinator last told it
	int zone_at;
	int64_t zones_visited;
	bool switched;
	bool on_map;                // looking at the strategic map, in no zone
	double next_directory;
	int journey_at;             // step of --journey in progress
	double journey_until;
	Bot bot;                    // --bot
	BotWorld bot_world;
	int64_t bot_tick;
	FILE *netlog;
	double t0;
	Rng rng;
	int64_t next_orders;

	int64_t deltas, digests, digest_mismatches, reconnects, gaps, malformed, commands_sent;
	double rtt, next_ping;
	double rate_since, down_rate;
	int64_t rate_bytes;

	Renderer renderer;
	bool renderer_ready;
	View view;
	Controls controls;
	bool overlay;
} Client;

// Monotonic seconds on this machine's clock, so netlogs from two clients on
// one machine line up when replayed side by side.
static double client_now(const Client *c)
{
	(void)c;
	return platform_seconds();
}

static void client_log(Client *c, uint8_t kind, const void *data, ptrdiff_t len)
{
	netlog_record(c->netlog, client_now(c), kind, data, len);
}

static void client_event(Client *c, uint8_t kind, const char *text)
{
	client_log(c, kind, text, (ptrdiff_t)strlen(text));
	if (c->opt.headless) printf("client: %s%s%s\n", kind == NETLOG_CONNECTED ? "connected" : "disconnected",
	                            text[0] ? ": " : "", text);
}

// ---------------------------------------------------------------- connection

// Ask the coordinator at HOST:PORT for the directory. Blocks up to half a
// second. The raw frame (for netlogs) lives in a.
static bool directory_fetch(const char *coord, uint8_t viewer, Arena *a, Directory *d, Str *raw)
{
	const char *colon = strrchr(coord, ':');
	if (!colon) fatal("--coord HOST:PORT");
	char host[64];
	snprintf(host, sizeof host, "%.*s", (int)(colon - coord), coord);
	int fd = net_connect(host, atoi(colon + 1));
	if (fd < 0) return false;
	Conn q;
	conn_init(&q, a, fd, 1 << 16, 1 << 16);
	// As our side, through its fog; spectators (and tools) see it all.
	conn_send(&q, COORD_DIRECTORY_ASK, &viewer, viewer < FACTION_COUNT ? 1 : 0, platform_seconds());
	bool got = false;
	for (int i = 0; i < 100 && q.open && !got; i++) {
		conn_pump(&q, platform_seconds());
		Frame f;
		while (!got && conn_next(&q, &f)) {
			if (f.kind != COORD_DIRECTORY) continue;
			ByteReader r = byte_reader(f.data, f.len);
			if (!get_directory(&r, d)) break;
			if (raw) *raw = (Str){(const char *)f.data, f.len};
			got = true;
		}
		if (!got) net_sleep(0.005);
	}
	conn_close(&q, "done");
	return got;
}

// Ask the coordinator which of our side's players NAME is. Blocks up to half
// a second; SIDE_FULL if every place on the side is taken, -1 if no answer.
static int coord_join(const char *coord, uint8_t faction, const char *name, Arena *a)
{
	const char *colon = strrchr(coord, ':');
	char host[64];
	snprintf(host, sizeof host, "%.*s", (int)(colon - coord), coord);
	int fd = net_connect(host, atoi(colon + 1));
	if (fd < 0) return -1;
	Conn q;
	conn_init(&q, a, fd, 1 << 12, 1 << 12);
	ArenaMark m = arena_mark(a);
	ByteBuf b = bytebuf_make(a, 64);
	put8(&b, faction);
	put_name(&b, name);
	conn_send(&q, COORD_JOIN, b.data, b.len, platform_seconds());
	arena_rewind(m);
	int slot = -1;
	for (int i = 0; i < 100 && q.open && slot < 0; i++) {
		conn_pump(&q, platform_seconds());
		Frame f;
		while (slot < 0 && conn_next(&q, &f))
			if (f.kind == COORD_SLOT && f.len == 1) slot = f.data[0];
		if (slot < 0) net_sleep(0.005);
	}
	conn_close(&q, "done");
	return slot;
}

static bool client_directory(Client *c)
{
	arena_reset(&c->out);
	Directory d;
	Str raw;
	if (!directory_fetch(c->opt.coord, c->opt.faction, &c->out, &d, &raw)) return false;
	c->dir = d;
	// Logged: a clip of this client draws the map as it was then.
	client_log(c, COORD_DIRECTORY, raw.ptr, raw.len);
	return true;
}

// Point the client at a zone from the directory; it connects next round.
static bool client_choose_zone(Client *c, const char *name)
{
	for (int k = 0; k < c->dir.zone_count; k++)
		if (!name || !strcmp(c->dir.zones[k].name, name)) {
			c->zone_at = k;
			c->opt.host = c->dir.zones[k].host;
			c->opt.port = c->dir.zones[k].port;
			return true;
		}
	return false;
}

static bool client_connect(Client *c)
{
	int fd = net_connect(c->opt.host, c->opt.port);
	if (fd < 0) return false;
	conn_reopen(&c->conn, fd);
	if (c->ever_connected) c->reconnects++;
	c->ever_connected = true;
	c->ready = false;
	arena_reset(&c->out);
	ByteBuf b = bytebuf_make(&c->out, 16);
	put32(&b, WIRE_VERSION);
	put8(&b, c->opt.faction);
	put8(&b, c->opt.slot.v);
	conn_send(&c->conn, MSG_HELLO, b.data, b.len, client_now(c));
	client_event(c, NETLOG_CONNECTED, "");
	c->zones_visited++;
	if (c->dir.zone_count) {
		const char *info = TextFormat("zone %s", c->dir.zones[c->zone_at].name);
		client_log(c, NETLOG_INFO, info, (ptrdiff_t)strlen(info));
	}
	return true;
}

static int64_t client_tick(const Client *c)
{
	return c->player ? c->remote.sim.tick.v : c->mirror.tick;
}

static uint16_t client_dilation(const Client *c)
{
	return c->player ? c->remote.dilation : c->mirror.dilation;
}

static void client_lost(Client *c, const char *why)
{
	client_event(c, NETLOG_DISCONNECTED, why ? why : "closed");
	c->disconnected_since = c->lost_at = client_now(c);
	c->ready = false;
}

static void client_welcome(Client *c, ByteReader *r)
{
	uint32_t version = get32(r);
	uint8_t faction = get8(r);
	SimConfig config;
	bool ok = wire_get_config(r, &config);
	V2 base[FACTION_COUNT];
	for (int f = 0; f < FACTION_COUNT; f++) base[f] = v2(getf(r), getf(r));
	if (!ok || r->bad || version != WIRE_VERSION || faction != c->opt.faction) {
		c->malformed++;
		conn_close(&c->conn, "bad welcome");
		return;
	}
	if (c->have_world) {
		// Same match: carry on. A different one (another zone): start over.
		if (sim_config_equal(&config, &c->config)) {
			memcpy(c->base, base, sizeof base);
			return;
		}
		if (c->player) remote_destroy(&c->remote);
		if (c->renderer_ready) renderer_unload(&c->renderer);
		c->renderer_ready = false;
		c->have_world = false;
		arena_reset(&c->world);
	}
	c->config = config;
	memcpy(c->base, base, sizeof base);
	terrain_generate(&c->terrain, &c->world, config.seed);
	c->player = faction < FACTION_COUNT;
	if (c->player) remote_init(&c->remote, &config, (FactionId){faction}, &c->world);
	else mirror_init(&c->mirror, &c->world, config.max_units, config.max_projectiles);
	controls_init(&c->controls, (FactionId){faction < FACTION_COUNT ? faction : 0}, &c->world, config.max_units);
	c->controls.player = c->opt.slot;
	c->controls.team_count = faction < FACTION_COUNT ? config.players[faction] : 1;
	c->bot = bot_create_player((FactionId){faction < FACTION_COUNT ? faction : 0}, c->opt.slot, config.seed ^ 0x5eedu);
	char why[160];
	if (c->opt.doctrine && !doctrine_parse(&c->bot.doctrine, c->opt.doctrine, why, sizeof why)) fatal("%s", why);
	c->bot_tick = -1;
	c->have_world = true;
}

static void client_frames(Client *c)
{
	Frame f;
	while (c->conn.open && conn_next(&c->conn, &f)) {
		client_log(c, f.kind, f.data, f.len);
		ByteReader r = byte_reader(f.data, f.len);
		if (f.kind == MSG_WELCOME) {
			client_welcome(c, &r);
		} else if (f.kind == MSG_FACTION_SNAPSHOT && c->have_world && c->player) {
			c->ready = remote_snapshot(&c->remote, &r);
			if (!c->ready) {
				c->malformed++;
				conn_close(&c->conn, "bad faction snapshot");
			}
			c->last_delta_at = client_now(c);
		} else if (f.kind == MSG_FACTION_TICK && c->have_world && c->player) {
			if (!c->ready) continue;
			bool had_hash;
			RemoteResult res = remote_tick(&c->remote, &r, &had_hash);
			c->digests += had_hash;
			if (res == REMOTE_OK) {
				c->deltas++;
				c->last_delta_at = client_now(c);
			} else if (res == REMOTE_DIVERGED) {
				c->digest_mismatches++;
				conn_close(&c->conn, "faction hash mismatch: resyncing");
			} else if (res == REMOTE_GAP) {
				c->gaps++;
				conn_close(&c->conn, "missed a tick: resyncing");
			} else {
				c->malformed++;
				conn_close(&c->conn, "malformed faction tick");
			}
		} else if ((f.kind == MSG_SNAPSHOT || f.kind == MSG_DELTA) && c->have_world && !c->player) {
			bool snapshot = f.kind == MSG_SNAPSHOT;
			if (!snapshot && !c->ready) continue;
			ApplyInfo info;
			ApplyResult res = wire_apply(&c->mirror, &r, snapshot, &info);
			if (res == APPLY_GAP) {
				c->gaps++;
				conn_close(&c->conn, "missed a tick: resyncing");
			} else if (res == APPLY_BAD) {
				c->malformed++;
				conn_close(&c->conn, "malformed update");
			} else {
				c->ready = true;
				c->deltas += !snapshot;
				c->last_delta_at = client_now(c);
				if (info.had_digest) {
					c->digests++;
					c->digest_mismatches += !info.digest_matched;
				}
			}
		} else if (f.kind == MSG_PONG && f.len == 8) {
			double sent;
			memcpy(&sent, f.data, sizeof sent);
			c->rtt = client_now(c) - sent;
		}
	}
}

// As our player; `to`: who a give or an output is for, or who a send arrives for.
static void client_send_order(Client *c, CommandKind kind, V2 target, UnitType type, int32_t amount, uint8_t flags,
                              PlayerSlot to, const UnitHandle *units, ptrdiff_t n)
{
	if (!c->conn.open || n == 0) return;
	arena_reset(&c->out);
	ByteBuf b = bytebuf_make(&c->out, 24 + n * 8);
	put8(&b, to.v);
	put8(&b, kind);
	putf(&b, target.x);
	putf(&b, target.z);
	put8(&b, type);
	put32(&b, (uint32_t)amount);
	put8(&b, flags);
	put32(&b, 0); // route: the server works it out
	put32(&b, (uint32_t)n);
	for (ptrdiff_t i = 0; i < n; i++) {
		put32(&b, units[i].index);
		put32(&b, units[i].generation);
	}
	if (conn_send(&c->conn, MSG_COMMAND, b.data, b.len, client_now(c))) c->commands_sent++;
}

static void client_send_command(Client *c, CommandKind kind, V2 target, const UnitHandle *units, ptrdiff_t n)
{
	client_send_order(c, kind, target, UNIT_RAIDER, 0, 0, c->opt.slot, units, n);
}

// A stand-in player for tests and demos: every three seconds, own units with
// nothing to do are sent toward the enemy base, in groups.
static void client_auto_orders(Client *c)
{
	if (!c->ready || !c->player || client_tick(c) < c->next_orders) return;
	c->next_orders = client_tick(c) + 90;
	const Units *u = &c->remote.sim.factions[c->opt.faction].units;
	V2 enemy = c->base[1 - c->opt.faction];
	UnitHandle group[128];
	ptrdiff_t n = 0;
	for (ptrdiff_t i = 0; i <= u->count; i++) {
		bool flush = n == countof(group) || (i == u->count && n > 0);
		if (flush) {
			V2 at = v2(enemy.x + rng_range(&c->rng, -30.0f, 30.0f), enemy.z + rng_range(&c->rng, -100.0f, 100.0f));
			client_send_command(c, COMMAND_ATTACK_MOVE, at, group, n);
			n = 0;
		}
		if (i == u->count) break;
		if (u->order[i] != ORDER_IDLE || u->engaged[i] || !player_eq(u->owner[i], c->opt.slot)) continue;
		group[n++] = units_handle(u, i);
	}
}

// ---------------------------------------------------------------- window

// Where a send could go, from the directory: our transporters, and the
// zones linked to this one.
static void client_targets(Client *c)
{
	SendTargets *t = &c->controls.targets;
	*t = (SendTargets){.here = c->zone_at};
	if (!c->opt.coord || !c->player) return;
	const Directory *d = &c->dir;
	for (int k = 0; k < MAX_PLAYERS; k++) c->controls.team[k] = d->player[c->opt.faction][k];
	for (int k = 0; k < d->zone_count && k < (int)countof(t->name); k++) {
		t->name[k] = d->zones[k].name;
		t->transporter[k] = d->zones[k].reported && d->zones[k].status.transporters[c->opt.faction] > 0;
		for (int l = 0; l < d->link_count; l++)
			t->neighbour[k] = t->neighbour[k] || (d->links[l][0] == k && d->links[l][1] == c->zone_at)
			                                  || (d->links[l][1] == k && d->links[l][0] == c->zone_at);
		t->count = k + 1;
	}
}

static void client_input(Client *c)
{
	if (!c->player) return;
	client_targets(c);
	Order orders[16];
	ptrdiff_t n = controls_input(&c->controls, &c->remote.sim, &c->view, &c->frame, orders, countof(orders));
	for (ptrdiff_t k = 0; k < n; k++)
		client_send_order(c, orders[k].kind, orders[k].target, orders[k].type, orders[k].amount, orders[k].flags,
		                  orders[k].to, orders[k].units, orders[k].count);
}

// --bot: the game's bot, on what this client knows, sending what a player can.
static void client_bot(Client *c)
{
	if (!c->ready || !c->player || c->remote.sim.tick.v == c->bot_tick) return;
	c->bot_tick = c->remote.sim.tick.v;
	if (c->opt.coord && c->dir.zone_count) {
		// The map as the directory has it; this zone's own side as simulated here.
		bot_world_build(&c->bot_world, &c->dir, c->zone_at, c->opt.faction);
		const Units *u = &c->remote.sim.factions[c->opt.faction].units;
		int32_t mine = 0;
		bool ours = false;
		for (ptrdiff_t i = 0; i < u->count; i++) {
			mine += unit_is_structure(u->type[i]) ? 2 : unit_is_combat(u->type[i]);
			ours = ours || (u->type[i] == UNIT_TRANSPORTER && u->progress[i] >= 1.0f);
		}
		c->bot_world.mine[c->zone_at] = mine;
		c->bot_world.ours[c->zone_at] = ours;
		c->bot_world.contact[c->zone_at] = c->remote.sim.factions[c->opt.faction].view.count > 0;
		bot_world_links(&c->bot_world, &c->dir);
		c->bot.world = &c->bot_world;
	}
	Command cmds[64];
	ptrdiff_t n = 0;
	bot_think(&c->bot, &c->remote.sim, &c->frame, cmds, &n, countof(cmds));
	for (ptrdiff_t k = 0; k < n; k++)
		client_send_order(c, cmds[k].kind, cmds[k].target, cmds[k].unit_type, cmds[k].amount, cmds[k].flags,
		                  cmds[k].to, cmds[k].units, cmds[k].count);
}

static void client_overlay(const Client *c, bool connected)
{
	int y = 10, line = 20;
	DrawRectangle(5, 5, 520, 7 * line + 10, (Color){0, 0, 0, 150});
	const char *side = c->opt.faction == 0 ? "blue" : c->opt.faction == 1 ? "red" : "spectator";
	DrawText(TextFormat("%d fps   %s at %s:%d   tick %lld%s", GetFPS(), side, c->opt.host, c->opt.port,
	                    (long long)client_tick(c), c->player ? "   simulating locally" : ""), 12, y, 18, RAYWHITE);
	y += line;
	DrawText(TextFormat("down %.1f KB/s   rtt %.0f ms   time %.0f%%", c->down_rate / 1024.0, c->rtt * 1e3,
	                    client_dilation(c) / 10.0), 12, y, 18, RAYWHITE);
	y += line;
	DrawText(TextFormat("%s %lld ok, %lld mismatched   reconnects %lld", c->player ? "faction hashes" : "digests", (long long)(c->digests - c->digest_mismatches),
	                    (long long)c->digest_mismatches, (long long)c->reconnects),
	         12, y, 18, c->digest_mismatches ? RED : RAYWHITE);
	y += line;
	DrawText(TextFormat("sending with %.0f ms latency, %.0f ms jitter", c->opt.latency * 1e3, c->opt.jitter * 1e3),
	         12, y, 16, LIGHTGRAY);
	y += line;
	DrawText(TextFormat("commands sent %lld", (long long)c->commands_sent), 12, y, 16, LIGHTGRAY);
	y += line;
	DrawText("Tab or Esc: strategic map | F1: this overlay", 12, y, 14, GRAY);
	if (!connected) {
		const char *msg = "connection lost: reconnecting...";
		int w = MeasureText(msg, 30);
		DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){0, 0, 0, 120});
		DrawText(msg, (GetScreenWidth() - w) / 2, GetScreenHeight() / 2 - 15, 30, RAYWHITE);
	}
}

// The zone's name and how to get back to the map, top left; F1 has the rest.
static void client_banner(const Client *c, bool connected)
{
	const char *side = c->opt.faction == 0 ? "blue" : c->opt.faction == 1 ? "red" : "spectator";
	const char *zone = c->dir.zone_count ? c->dir.zones[c->zone_at].name : TextFormat("%s:%d", c->opt.host, c->opt.port);
	const char *text = TextFormat("zone %s   %s   %s", zone, side, c->opt.coord ? "Tab: strategic map   F1: details" : "F1: details");
	DrawRectangle(5, 5, MeasureText(text, 20) + 20, 32, (Color){0, 0, 0, 150});
	DrawText(text, 15, 11, 20, RAYWHITE);
	if (!connected) {
		const char *msg = "connection lost: reconnecting...";
		int w = MeasureText(msg, 30);
		DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){0, 0, 0, 120});
		DrawText(msg, (GetScreenWidth() - w) / 2, GetScreenHeight() / 2 - 15, 30, RAYWHITE);
	}
}

// Leave the zone (if in one) for the strategic map.
static void client_to_map(Client *c, double now)
{
	if (c->conn.open) {
		conn_close(&c->conn, "to the map");
		client_lost(c, "to the strategic map");
	}
	c->on_map = true;
	c->ready = false;
	c->next_directory = now; // fresh at once
	if (c->have_world) controls_clear(&c->controls);
	client_log(c, NETLOG_MAP, "", 0);
}

// Into a zone, from the map or another zone: it connects this round.
static bool client_launch(Client *c, const char *zone, double now)
{
	if (!client_choose_zone(c, zone)) return false;
	if (c->conn.open) {
		conn_close(&c->conn, "switching zones");
		client_lost(c, TextFormat("switching to zone %s", zone));
	}
	if (c->ever_connected) c->reconnects--;  // the next connect is a launch, not a failure
	c->on_map = false;
	c->disconnected_since = now - c->opt.reconnect_after; // connect right away
	c->lost_at = now;
	return true;
}

// ---------------------------------------------------------------- main

static uint8_t parse_faction(const char *s)
{
	if (!strcmp(s, "blue")) return 0;
	if (!strcmp(s, "red")) return 1;
	if (!strcmp(s, "spectator")) return SPECTATOR;
	fatal("--faction is blue, red or spectator");
}

static int client_main(int argc, char **argv)
{
	static Client c;
	c.opt = (ClientOptions){.faction = 0, .disconnect_at = -1, .quit_at = -1, .reconnect_after = 2.0};
	for (int i = 1; i < argc;) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : nullptr;
		int used = 2;
		if (!strcmp(a, "--headless")) c.opt.headless = true, used = 1;
		else if (!strcmp(a, "--auto-orders")) c.opt.auto_orders = true, used = 1;
		else if (!strcmp(a, "--bot")) c.opt.bot = true, used = 1;
		else if (!v) fatal("%s needs a value", a);
		else if (!strcmp(a, "--connect")) {
			static char host[256];
			const char *colon = strrchr(v, ':');
			if (!colon || colon - v >= (ptrdiff_t)sizeof host) fatal("--connect HOST:PORT");
			memcpy(host, v, (size_t)(colon - v));
			host[colon - v] = 0;
			c.opt.host = host;
			c.opt.port = atoi(colon + 1);
		} else if (!strcmp(a, "--faction")) c.opt.faction = parse_faction(v);
		else if (!strcmp(a, "--name")) c.opt.name = v;
		else if (!strcmp(a, "--slot")) {
			int slot = atoi(v);
			if (slot < 0 || slot >= MAX_PLAYERS) fatal("--slot is 0..%d", MAX_PLAYERS - 1);
			c.opt.slot = (PlayerSlot){(uint8_t)slot};
		}
		else if (!strcmp(a, "--latency")) c.opt.latency = atof(v) / 1000.0;
		else if (!strcmp(a, "--jitter")) c.opt.jitter = atof(v) / 1000.0;
		else if (!strcmp(a, "--netlog")) c.opt.netlog_path = v;
		else if (!strcmp(a, "--stats")) c.opt.stats_path = v;
		else if (!strcmp(a, "--disconnect-at")) c.opt.disconnect_at = atoll(v);
		else if (!strcmp(a, "--reconnect-after")) c.opt.reconnect_after = atof(v);
		else if (!strcmp(a, "--quit-at")) c.opt.quit_at = atoll(v);
		else if (!strcmp(a, "--coord")) c.opt.coord = v;
		else if (!strcmp(a, "--zone")) c.opt.zone = v;
		else if (!strcmp(a, "--switch-to")) c.opt.switch_to = v;
		else if (!strcmp(a, "--switch-at")) c.opt.switch_at = atoll(v);
		else if (!strcmp(a, "--quit-after")) c.opt.quit_after = atoll(v);
		else if (!strcmp(a, "--doctrine")) c.opt.doctrine = v;
		else if (!strcmp(a, "--journey")) {
			static char steps[512];
			snprintf(steps, sizeof steps, "%s", v);
			char *save;
			for (char *t = strtok_r(steps, ",", &save); t; t = strtok_r(nullptr, ",", &save)) {
				char *colon = strchr(t, ':');
				if (!colon || c.opt.journey_len == (int)countof(c.opt.journey) || colon - t >= ZONE_NAME)
					fatal("--journey map:SECONDS,ZONE:SECONDS,...");
				*colon = 0;
				snprintf(c.opt.journey[c.opt.journey_len], ZONE_NAME, "%s", t);
				c.opt.journey_s[c.opt.journey_len++] = atof(colon + 1);
			}
		}
		else fatal("unknown option %s", a);
		i += used;
	}
	c.out = arena_create("client out", (ptrdiff_t)16 << 20);
	c.program = arena_create("client program", (ptrdiff_t)1 << 30);
	c.world = arena_create("client world", (ptrdiff_t)2 << 30);
	c.frame = arena_create("client frame", (ptrdiff_t)256 << 20);
	conn_init(&c.conn, &c.program, -1, (ptrdiff_t)4 << 20, (ptrdiff_t)32 << 20);
	conn_set_delay(&c.conn, c.opt.latency, c.opt.jitter, 0xc11e47u);
	c.rng = rng_seed(0xa070u + c.opt.faction);
	c.t0 = platform_seconds();
	if (c.opt.netlog_path) {
		c.netlog = fopen(c.opt.netlog_path, "wb");
		if (!c.netlog) fatal("cannot write %s", c.opt.netlog_path);
		netlog_begin(c.netlog);
		char info[160];
		snprintf(info, sizeof info, "%s, %.0f ms latency, %.0f ms jitter",
		         c.opt.faction == 0 ? "blue" : c.opt.faction == 1 ? "red" : "spectator",
		         c.opt.latency * 1e3, c.opt.jitter * 1e3);
		client_log(&c, NETLOG_INFO, info, (ptrdiff_t)strlen(info));
	}
	if (c.opt.coord) {
		bool listed = false;
		for (int tries = 0; tries < 50 && !(listed = client_directory(&c) && c.dir.zone_count > 0); tries++) net_sleep(0.1);
		if (!listed) fatal("no zones from the coordinator at %s", c.opt.coord);
		if (c.opt.faction < FACTION_COUNT) {
			// Who we are: a name, and the slot on our side that's theirs.
			const char *name = c.opt.name ? c.opt.name : getenv("USER");
			if (!name || !*name) name = "player";
			int slot = -1;
			for (int tries = 0; tries < 50 && slot < 0; tries++)
				if ((slot = coord_join(c.opt.coord, c.opt.faction, name, &c.out)) < 0) net_sleep(0.1);
			if (slot < 0) fatal("no answer from the coordinator at %s", c.opt.coord);
			if (slot == SIDE_FULL) { // not a fault: someone else got there first
				fprintf(stderr, "client: every place on %s is taken\n", c.opt.faction ? "red" : "blue");
				return 1;
			}
			c.opt.slot = (PlayerSlot){(uint8_t)slot};
			c.opt.name = name;
			printf("client: %s plays %s.%d\n", name, c.opt.faction ? "red" : "blue", slot);
		}
		// Where to start: a zone, or the map (in a window, or a journey's first step).
		const char *first = c.opt.journey_len ? c.opt.journey[0] : c.opt.zone;
		if (c.opt.journey_len) c.journey_until = platform_seconds() + c.opt.journey_s[0];
		if (first && strcmp(first, "map")) {
			if (!client_choose_zone(&c, first)) fatal("no zone %s at the coordinator", first);
		} else if (first || !c.opt.headless) {
			c.on_map = true;
			client_log(&c, NETLOG_MAP, "", 0);
		} else if (!client_choose_zone(&c, nullptr)) {
			fatal("no zones at the coordinator");
		}
	}
	if (!c.on_map && (!c.opt.host || c.opt.port <= 0)) fatal("--connect HOST:PORT or --coord HOST:PORT");

	if (!c.opt.headless) {
		SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
		InitWindow(1600, 900, c.opt.coord ? "rts" : TextFormat("rts client: %s:%d", c.opt.host, c.opt.port));
		SetExitKey(KEY_NULL); // Esc goes to the map; close the window to quit
		SetTargetFPS(120);
	}
	for (int tries = 0; !c.on_map && !client_connect(&c); tries++) {
		if (tries == 50) fatal("cannot connect to %s:%d", c.opt.host, c.opt.port);
		net_sleep(0.1);
	}

	int status = 0;
	for (;;) {
		double now = client_now(&c);
		arena_reset(&c.frame);
		bool connected = c.conn.open;
		if (connected) {
			conn_pump(&c.conn, now);
			client_frames(&c);
			if (!c.conn.open) client_lost(&c, c.conn.why_closed);
		} else if (!c.on_map && now - c.disconnected_since >= c.opt.reconnect_after) {
			if (!client_connect(&c)) c.disconnected_since = now; // try again later
		}
		if (c.opt.disconnect_at >= 0 && !c.forced_done && c.ready && client_tick(&c) >= c.opt.disconnect_at) {
			c.forced_done = true;
			conn_close(&c.conn, "test disconnect");
			client_lost(&c, "test disconnect");
		}
		if (c.opt.auto_orders) client_auto_orders(&c);
		if (c.opt.bot && !c.on_map) client_bot(&c);
		if (c.conn.open && now >= c.next_ping) {
			c.next_ping = now + 1.0;
			conn_send(&c.conn, MSG_PING, &now, sizeof now, now);
		}
		if (now - c.rate_since >= 1.0) {
			c.down_rate = (double)(c.conn.bytes_received - c.rate_bytes) / (now - c.rate_since);
			c.rate_bytes = c.conn.bytes_received;
			c.rate_since = now;
		}
		if (c.opt.coord && now >= c.next_directory) {
			// The map refreshes every second; in a zone, for sends, every few.
			c.next_directory = now + (c.on_map ? 1.0 : 3.0);
			client_directory(&c);
		}
		if (c.opt.quit_at >= 0 && client_tick(&c) >= c.opt.quit_at) break;
		if (c.opt.quit_after > 0 && c.deltas >= c.opt.quit_after) break;
		// A scripted journey: the next step when this one's time is up.
		if (c.opt.journey_len && now >= c.journey_until) {
			if (++c.journey_at == c.opt.journey_len) break;
			const char *step = c.opt.journey[c.journey_at];
			c.journey_until = now + c.opt.journey_s[c.journey_at];
			if (!strcmp(step, "map")) client_to_map(&c, now);
			else if (!client_launch(&c, step, now)) fatal("--journey: no zone %s", step);
		}
		// --switch-to: straight from one zone to another (the zone smoke test).
		if (c.opt.switch_to && !c.switched && c.deltas >= c.opt.switch_at) {
			c.switched = true;
			client_directory(&c);
			if (!client_launch(&c, c.opt.switch_to, now)) fatal("--switch-to: no zone %s", c.opt.switch_to);
		}
		if (!c.on_map && !c.conn.open && now - c.lost_at > 15.0 && c.opt.headless) {
			fprintf(stderr, "client: server gone for 15 s, giving up\n");
			status = 1;
			break;
		}

		if (c.opt.headless) {
			net_sleep(0.002);
			continue;
		}
		if (WindowShouldClose()) break;
		BeginDrawing();
		ClearBackground((Color){24, 28, 34, 255});
		if (c.on_map) {
			Vector2 m = GetMousePosition();
			int w = GetScreenWidth(), h = GetScreenHeight();
			int hover = map_pick(&c.dir, m.x, m.y, w, h);
			map_draw(&c.dir, c.opt.faction < FACTION_COUNT ? c.opt.faction : -1, hover, w, h);
			if (hover >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && c.dir.zones[hover].up)
				client_launch(&c, c.dir.zones[hover].name, now);
			if (IsKeyPressed(KEY_ESCAPE)) {
				EndDrawing();
				break;
			}
			EndDrawing();
			continue;
		}
		if (c.have_world && !c.renderer_ready) {
			renderer_init(&c.renderer, &c.terrain, c.config.max_units, &c.world);
			// Start over our own base.
			V2 home = c.player ? c.base[c.opt.faction] : v2(c.terrain.size_x * 0.5f, c.terrain.size_z * 0.5f);
			c.view = (View){.focus = home, .distance = c.player ? 220.0f : 420.0f};
			c.renderer_ready = true;
		}
		bool was_placing = c.controls.placing >= 0;
		if (c.renderer_ready && c.ready) {
			view_update(&c.view, &c.terrain);
			Scene scene = c.player ? remote_scene(&c.remote, c.base, &c.frame)
			                       : scene_from_mirror(&c.mirror, &c.terrain, c.base, &c.frame);
			if (c.player) {
				uint8_t *visible = PUSH(&c.frame, uint8_t, (ptrdiff_t)(c.terrain.cells_x + 1) * (c.terrain.cells_z + 1));
				remote_fog(&c.remote, visible);
				renderer_set_fog(&c.renderer, visible);
				client_input(&c);
			}
			uint8_t *selected = c.player ? controls_selected(&c.controls, &c.remote.sim.factions[c.opt.faction].units, &c.frame)
			                             : nullptr;
			// Extrapolate along velocity since the last update, a little past
			// one tick at most so a late packet doesn't fling units.
			uint16_t dil = client_dilation(&c);
			float tick_s = SIM_DT * 1000.0f / (float)(dil ? dil : 1000);
			float alpha = f_clamp((float)(now - c.last_delta_at) / tick_s, 0.0f, 1.5f);
			BeginMode3D(c.view.camera);
			renderer_draw_scene(&c.renderer, &scene, c.conn.open ? alpha : 0.0f, &c.frame, selected,
			                    c.opt.faction < FACTION_COUNT ? c.opt.faction : -1);
			if (c.player) controls_draw_world(&c.controls, &c.remote.sim, &c.view);
			EndMode3D();
			if (c.player) controls_draw_screen(&c.controls, &c.remote.sim);
		}
		if (IsKeyPressed(KEY_F1)) c.overlay = !c.overlay;
		if (c.overlay) client_overlay(&c, c.conn.open);
		else client_banner(&c, c.conn.open);
		EndDrawing();
		// To the map: Tab, or Esc unless it just cancelled a placement.
		if (c.opt.coord && (IsKeyPressed(KEY_TAB) || (IsKeyPressed(KEY_ESCAPE) && !was_placing))) client_to_map(&c, now);
		else if (!c.opt.coord && IsKeyPressed(KEY_ESCAPE) && !was_placing) break;
	}

	conn_close(&c.conn, "client quit");
	if (c.netlog) fclose(c.netlog);
	printf("client: %lld deltas, %lld digests (%lld mismatched), %lld reconnects, %lld gaps, "
	       "%lld malformed, %lld commands sent, final tick %lld\n",
	       (long long)c.deltas, (long long)c.digests, (long long)c.digest_mismatches, (long long)c.reconnects,
	       (long long)c.gaps, (long long)c.malformed, (long long)c.commands_sent, (long long)client_tick(&c));
	if (c.opt.stats_path) {
		FILE *f = fopen(c.opt.stats_path, "w");
		if (!f) fatal("cannot write %s", c.opt.stats_path);
		fprintf(f, "zones_visited %lld\n", (long long)c.zones_visited);
		fprintf(f, "deltas %lld\ndigests %lld\ndigest_mismatches %lld\nreconnects %lld\ngaps %lld\n"
		           "malformed %lld\ncommands_sent %lld\nfinal_tick %lld\nbytes_received %lld\n",
		        (long long)c.deltas, (long long)c.digests, (long long)c.digest_mismatches,
		        (long long)c.reconnects, (long long)c.gaps, (long long)c.malformed,
		        (long long)c.commands_sent, (long long)client_tick(&c), (long long)c.conn.bytes_received);
		fclose(f);
	}
	if (c.renderer_ready) renderer_unload(&c.renderer);
	if (!c.opt.headless) CloseWindow();
	return status;
}
