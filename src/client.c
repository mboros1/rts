// The network client: draws what the server sends, sends what you order.
//
//   rts --connect HOST:PORT [--faction blue|red|spectator]
//       [--latency MS] [--jitter MS]       delay what this client sends
//       [--netlog PATH] [--stats PATH]
//       [--headless] [--auto-orders]       no window; order idle units forward
//       [--disconnect-at TICK] [--reconnect-after SECONDS] [--quit-at TICK]
//
// The client never simulates (that's spike 3). It keeps a Mirror (wire.h)
// built from the server's snapshot and deltas, dead-reckons units between
// updates, flies shells itself, and checks the server's digest every second.
// It reconnects on its own and takes a fresh snapshot.

#include <math.h>

#include "net.h"
#include "netlog.h"
#include "platform.h"
#include "render.h"
#include "scene.h"
#include "view.h"
#include "wire.h"

typedef struct {
	const char *host;
	int port;
	uint8_t faction;
	double latency, jitter;
	const char *netlog_path, *stats_path;
	bool headless, auto_orders;
	int64_t disconnect_at, quit_at;
	double reconnect_after;
} ClientOptions;

typedef struct {
	ClientOptions opt;
	Conn conn;
	bool ever_connected;
	double disconnected_since;
	bool forced_done;

	bool have_world;
	SimConfig config;
	Terrain terrain;
	V2 base[FACTION_COUNT];
	Mirror mirror;
	bool mirror_ready;
	double last_delta_at;

	Arena program, frame, out;
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
	UnitHandle *selection;
	ptrdiff_t selection_count;
	bool dragging;
	Vector2 drag_start;
} Client;

static double client_now(const Client *c) { return platform_seconds() - c->t0; }

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

static bool client_connect(Client *c)
{
	int fd = net_connect(c->opt.host, c->opt.port);
	if (fd < 0) return false;
	conn_reopen(&c->conn, fd);
	if (c->ever_connected) c->reconnects++;
	c->ever_connected = true;
	c->mirror_ready = false;
	arena_reset(&c->out);
	ByteBuf b = bytebuf_make(&c->out, 16);
	put32(&b, WIRE_VERSION);
	put8(&b, c->opt.faction);
	conn_send(&c->conn, MSG_HELLO, b.data, b.len, client_now(c));
	client_event(c, NETLOG_CONNECTED, "");
	return true;
}

static void client_lost(Client *c, const char *why)
{
	client_event(c, NETLOG_DISCONNECTED, why ? why : "closed");
	c->disconnected_since = client_now(c);
	c->mirror_ready = false;
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
		if (memcmp(&config, &c->config, sizeof config) != 0) fatal("server changed its config on reconnect");
		return;
	}
	c->config = config;
	memcpy(c->base, base, sizeof base);
	terrain_generate(&c->terrain, &c->program, config.seed);
	mirror_init(&c->mirror, &c->program, config.max_units, config.max_projectiles);
	c->selection = PUSH(&c->program, UnitHandle, config.max_units);
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
		} else if ((f.kind == MSG_SNAPSHOT || f.kind == MSG_DELTA) && c->have_world) {
			bool snapshot = f.kind == MSG_SNAPSHOT;
			if (!snapshot && !c->mirror_ready) continue;
			ApplyInfo info;
			ApplyResult res = wire_apply(&c->mirror, &r, snapshot, &info);
			if (res == APPLY_GAP) {
				c->gaps++;
				conn_close(&c->conn, "missed a tick: resyncing");
			} else if (res == APPLY_BAD) {
				c->malformed++;
				conn_close(&c->conn, "malformed update");
			} else {
				c->mirror_ready = true;
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

static void client_send_command(Client *c, CommandKind kind, V2 target, const UnitHandle *units, ptrdiff_t n)
{
	if (!c->conn.open || n == 0) return;
	arena_reset(&c->out);
	ByteBuf b = bytebuf_make(&c->out, 16 + n * 8);
	put8(&b, kind);
	putf(&b, target.x);
	putf(&b, target.z);
	put32(&b, (uint32_t)n);
	for (ptrdiff_t i = 0; i < n; i++) {
		put32(&b, units[i].index);
		put32(&b, units[i].generation);
	}
	if (conn_send(&c->conn, MSG_COMMAND, b.data, b.len, client_now(c))) c->commands_sent++;
}

// A stand-in player for tests and demos: every three seconds, own units
// standing still are sent toward the enemy base, in groups.
static void client_auto_orders(Client *c)
{
	if (!c->mirror_ready || c->opt.faction >= FACTION_COUNT || c->mirror.tick < c->next_orders) return;
	c->next_orders = c->mirror.tick + 90;
	const MirrorUnits *u = &c->mirror.units[c->opt.faction];
	V2 enemy = c->base[1 - c->opt.faction];
	UnitHandle group[128];
	ptrdiff_t n = 0;
	for (ptrdiff_t s = 0; s <= u->cap; s++) {
		bool flush = n == countof(group) || (s == u->cap && n > 0);
		if (flush) {
			V2 at = v2(enemy.x + rng_range(&c->rng, -30.0f, 30.0f), enemy.z + rng_range(&c->rng, -100.0f, 100.0f));
			client_send_command(c, COMMAND_ATTACK_MOVE, at, group, n);
			n = 0;
		}
		if (s == u->cap) break;
		if (!u->live[s] || abs(u->qvx[s]) > 64 || abs(u->qvz[s]) > 64) continue;
		group[n++] = (UnitHandle){(uint32_t)s, u->gen[s]};
	}
}

// ---------------------------------------------------------------- window

static void client_select(Client *c, const Scene *scene, Rectangle box, bool add, bool click)
{
	if (c->opt.faction >= FACTION_COUNT) return;
	const SceneUnits *u = &scene->units[c->opt.faction];
	const MirrorUnits *m = &c->mirror.units[c->opt.faction];
	if (!add) c->selection_count = 0;
	ptrdiff_t best = -1;
	float best_d2 = 14.0f * 14.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		Vector2 s = GetWorldToScreen((Vector3){u->px[i], u->py[i] + 0.5f, u->pz[i]}, c->view.camera);
		UnitHandle h = {(uint32_t)u->slot[i], m->gen[u->slot[i]]};
		if (click) {
			float dx = s.x - box.x, dy = s.y - box.y;
			if (dx * dx + dy * dy < best_d2) {
				best = i;
				best_d2 = dx * dx + dy * dy;
			}
		} else if (CheckCollisionPointRec(s, box) && c->selection_count < c->config.max_units) {
			c->selection[c->selection_count++] = h;
		}
	}
	if (click && best >= 0) c->selection[c->selection_count++] = (UnitHandle){(uint32_t)u->slot[best], m->gen[u->slot[best]]};
}

// Marks selected units by scene index and drops dead ones from the selection.
static uint8_t *client_selected(Client *c, const Scene *scene)
{
	if (c->opt.faction >= FACTION_COUNT) return nullptr;
	const SceneUnits *u = &scene->units[c->opt.faction];
	const MirrorUnits *m = &c->mirror.units[c->opt.faction];
	int32_t *index_of_slot = PUSH(&c->frame, int32_t, m->cap);
	for (ptrdiff_t s = 0; s < m->cap; s++) index_of_slot[s] = -1;
	for (ptrdiff_t i = 0; i < u->count; i++) index_of_slot[u->slot[i]] = (int32_t)i;
	uint8_t *marked = PUSH(&c->frame, uint8_t, u->count);
	ptrdiff_t kept = 0;
	for (ptrdiff_t k = 0; k < c->selection_count; k++) {
		UnitHandle h = c->selection[k];
		int32_t i = h.index < (uint32_t)m->cap ? index_of_slot[h.index] : -1;
		if (i < 0 || m->gen[h.index] != h.generation || marked[i]) continue;
		marked[i] = 1;
		c->selection[kept++] = h;
	}
	c->selection_count = kept;
	return marked;
}

static void client_input(Client *c, const Scene *scene)
{
	Vector2 mouse = GetMousePosition();
	if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		c->dragging = true;
		c->drag_start = mouse;
	}
	if (c->dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
		c->dragging = false;
		Rectangle box = {fminf(c->drag_start.x, mouse.x), fminf(c->drag_start.y, mouse.y),
		                 fabsf(mouse.x - c->drag_start.x), fabsf(mouse.y - c->drag_start.y)};
		bool click = box.width < 4.0f && box.height < 4.0f;
		if (click) box = (Rectangle){mouse.x, mouse.y, 0, 0};
		client_select(c, scene, box, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT), click);
	}
	V2 at;
	if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && pick_ground(&c->view, &c->terrain, mouse, &at)) {
		bool move = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
		client_send_command(c, move ? COMMAND_MOVE : COMMAND_ATTACK_MOVE, at, c->selection, c->selection_count);
	}
	if (IsKeyPressed(KEY_X) && c->selection_count)
		client_send_command(c, COMMAND_STOP, v2(0, 0), c->selection, c->selection_count);
}

static void client_overlay(const Client *c, bool connected)
{
	int y = 10, line = 20;
	DrawRectangle(5, 5, 520, 7 * line + 10, (Color){0, 0, 0, 150});
	const char *side = c->opt.faction == 0 ? "blue" : c->opt.faction == 1 ? "red" : "spectator";
	DrawText(TextFormat("%d fps   %s at %s:%d   tick %lld", GetFPS(), side, c->opt.host, c->opt.port,
	                    (long long)c->mirror.tick), 12, y, 18, RAYWHITE);
	y += line;
	DrawText(TextFormat("down %.1f KB/s   rtt %.0f ms   time %.0f%%", c->down_rate / 1024.0, c->rtt * 1e3,
	                    c->mirror.dilation / 10.0), 12, y, 18, RAYWHITE);
	y += line;
	DrawText(TextFormat("digests %lld ok, %lld mismatched   reconnects %lld", (long long)(c->digests - c->digest_mismatches),
	                    (long long)c->digest_mismatches, (long long)c->reconnects),
	         12, y, 18, c->digest_mismatches ? RED : RAYWHITE);
	y += line;
	DrawText(TextFormat("sending with %.0f ms latency, %.0f ms jitter", c->opt.latency * 1e3, c->opt.jitter * 1e3),
	         12, y, 16, LIGHTGRAY);
	y += line;
	DrawText(TextFormat("selected %td   commands sent %lld", c->selection_count, (long long)c->commands_sent),
	         12, y, 16, LIGHTGRAY);
	y += line;
	DrawText("drag select | right: attack-move | ctrl+right: move | X stop", 12, y, 14, GRAY);
	if (!connected) {
		const char *msg = "connection lost: reconnecting...";
		int w = MeasureText(msg, 30);
		DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){0, 0, 0, 120});
		DrawText(msg, (GetScreenWidth() - w) / 2, GetScreenHeight() / 2 - 15, 30, RAYWHITE);
	}
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
		else if (!strcmp(a, "--latency")) c.opt.latency = atof(v) / 1000.0;
		else if (!strcmp(a, "--jitter")) c.opt.jitter = atof(v) / 1000.0;
		else if (!strcmp(a, "--netlog")) c.opt.netlog_path = v;
		else if (!strcmp(a, "--stats")) c.opt.stats_path = v;
		else if (!strcmp(a, "--disconnect-at")) c.opt.disconnect_at = atoll(v);
		else if (!strcmp(a, "--reconnect-after")) c.opt.reconnect_after = atof(v);
		else if (!strcmp(a, "--quit-at")) c.opt.quit_at = atoll(v);
		else fatal("unknown option %s", a);
		i += used;
	}
	if (!c.opt.host || c.opt.port <= 0) fatal("--connect HOST:PORT");

	c.program = arena_create("client program", (ptrdiff_t)1 << 30);
	c.frame = arena_create("client frame", (ptrdiff_t)256 << 20);
	c.out = arena_create("client out", (ptrdiff_t)16 << 20);
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

	if (!c.opt.headless) {
		SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
		InitWindow(1600, 900, TextFormat("rts client: %s:%d", c.opt.host, c.opt.port));
		SetTargetFPS(120);
	}
	for (int tries = 0; !client_connect(&c); tries++) {
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
		} else if (now - c.disconnected_since >= c.opt.reconnect_after) {
			if (!client_connect(&c)) c.disconnected_since = now; // try again later
		}
		if (c.opt.disconnect_at >= 0 && !c.forced_done && c.mirror_ready && c.mirror.tick >= c.opt.disconnect_at) {
			c.forced_done = true;
			conn_close(&c.conn, "test disconnect");
			client_lost(&c, "test disconnect");
		}
		if (c.opt.auto_orders) client_auto_orders(&c);
		if (c.conn.open && now >= c.next_ping) {
			c.next_ping = now + 1.0;
			conn_send(&c.conn, MSG_PING, &now, sizeof now, now);
		}
		if (now - c.rate_since >= 1.0) {
			c.down_rate = (double)(c.conn.bytes_received - c.rate_bytes) / (now - c.rate_since);
			c.rate_bytes = c.conn.bytes_received;
			c.rate_since = now;
		}
		if (c.opt.quit_at >= 0 && c.mirror.tick >= c.opt.quit_at) break;
		if (!c.conn.open && now - c.disconnected_since > 15.0 && c.opt.headless) {
			fprintf(stderr, "client: server gone for 15 s, giving up\n");
			status = 1;
			break;
		}

		if (c.opt.headless) {
			net_sleep(0.002);
			continue;
		}
		if (WindowShouldClose()) break;
		if (c.have_world && !c.renderer_ready) {
			renderer_init(&c.renderer, &c.terrain, c.config.max_units, &c.program);
			c.view = (View){.focus = v2(c.terrain.size_x * 0.5f, c.terrain.size_z * 0.5f), .distance = 420.0f};
			c.renderer_ready = true;
		}
		BeginDrawing();
		ClearBackground((Color){24, 28, 34, 255});
		if (c.renderer_ready && c.mirror.tick > 0) {
			view_update(&c.view, &c.terrain);
			Scene scene = scene_from_mirror(&c.mirror, &c.terrain, c.base, &c.frame);
			client_input(&c, &scene);
			uint8_t *selected = client_selected(&c, &scene);
			// Extrapolate along velocity since the last update, a little past
			// one tick at most so a late packet doesn't fling units.
			float tick_s = SIM_DT * 1000.0f / (float)(c.mirror.dilation ? c.mirror.dilation : 1000);
			float alpha = f_clamp((float)(now - c.last_delta_at) / tick_s, 0.0f, 1.5f);
			BeginMode3D(c.view.camera);
			renderer_draw_scene(&c.renderer, &scene, c.conn.open ? alpha : 0.0f, &c.frame, selected,
			                    c.opt.faction < FACTION_COUNT ? c.opt.faction : -1);
			EndMode3D();
			if (c.dragging) {
				Vector2 m = GetMousePosition();
				DrawRectangleLinesEx((Rectangle){fminf(c.drag_start.x, m.x), fminf(c.drag_start.y, m.y),
				                                 fabsf(m.x - c.drag_start.x), fabsf(m.y - c.drag_start.y)},
				                     1.0f, SELECTED_COLOR);
			}
		}
		client_overlay(&c, c.conn.open);
		EndDrawing();
	}

	conn_close(&c.conn, "client quit");
	if (c.netlog) fclose(c.netlog);
	printf("client: %lld deltas, %lld digests (%lld mismatched), %lld reconnects, %lld gaps, "
	       "%lld malformed, %lld commands sent, final tick %lld\n",
	       (long long)c.deltas, (long long)c.digests, (long long)c.digest_mismatches, (long long)c.reconnects,
	       (long long)c.gaps, (long long)c.malformed, (long long)c.commands_sent, (long long)c.mirror.tick);
	if (c.opt.stats_path) {
		FILE *f = fopen(c.opt.stats_path, "w");
		if (!f) fatal("cannot write %s", c.opt.stats_path);
		fprintf(f, "deltas %lld\ndigests %lld\ndigest_mismatches %lld\nreconnects %lld\ngaps %lld\n"
		           "malformed %lld\ncommands_sent %lld\nfinal_tick %lld\nbytes_received %lld\n",
		        (long long)c.deltas, (long long)c.digests, (long long)c.digest_mismatches,
		        (long long)c.reconnects, (long long)c.gaps, (long long)c.malformed,
		        (long long)c.commands_sent, (long long)c.mirror.tick, (long long)c.conn.bytes_received);
		fclose(f);
	}
	if (c.renderer_ready) renderer_unload(&c.renderer);
	if (!c.opt.headless) CloseWindow();
	return status;
}
