#include "replay.h"

static constexpr uint32_t REPLAY_MAGIC = 0x52535452u; // "RTSR" read as little-endian
static constexpr uint32_t REPLAY_VERSION = 12; // 12: who's away, and taking from them

// ---------------------------------------------------------------- writing

// The writer's arena holds only the file, so every push extends it in place.
static void put(ReplayWriter *w, const void *bytes, ptrdiff_t n)
{
	uint8_t *p = arena_push(w->buf, n, 1);
	if (!w->start) w->start = p;
	ASSERT(p == w->start + w->len); // something else allocated from the replay arena
	memcpy(p, bytes, (size_t)n);
	w->len += n;
}

static void put_u32(ReplayWriter *w, uint32_t x)
{
	uint8_t b[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)};
	put(w, b, 4);
}

static void put_u64(ReplayWriter *w, uint64_t x)
{
	put_u32(w, (uint32_t)x);
	put_u32(w, (uint32_t)(x >> 32));
}

static void put_f32(ReplayWriter *w, float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof bits);
	put_u32(w, bits);
}

void replay_begin(ReplayWriter *w, Arena *buf, SimConfig c)
{
	replay_begin_from(w, buf, c, (Str){"", 0});
}

void replay_begin_from(ReplayWriter *w, Arena *buf, SimConfig c, Str snapshot)
{
	*w = (ReplayWriter){.buf = buf};
	put_u32(w, REPLAY_MAGIC);
	put_u32(w, REPLAY_VERSION);
	put_u64(w, c.seed);
	put_u32(w, (uint32_t)c.initial_units);
	put_u32(w, (uint32_t)c.population);
	put_u32(w, (uint32_t)c.spawn_interval);
	put_u32(w, (uint32_t)c.max_units);
	put_u32(w, (uint32_t)c.max_projectiles);
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) put_u32(w, (uint32_t)c.type_weight[k]);
	put_u32(w, c.economy);
	put_u32(w, c.bases);
	for (int f = 0; f < FACTION_COUNT; f++) put_u32(w, c.players[f]);
	put_u32(w, (uint32_t)snapshot.len);
	if (snapshot.len) put(w, snapshot.ptr, snapshot.len);
}

bool replay_start(const ReplayReader *r, Sim *sim)
{
	sim_init(sim, r->config);
	if (!r->snapshot.len) return true;
	ByteReader br = byte_reader(r->snapshot.ptr, r->snapshot.len);
	return sim_load(sim, &br) && br.pos == br.len;
}

void replay_tick(ReplayWriter *w, Tick tick, const Command *cmds, ptrdiff_t count, Hash after)
{
	put_u64(w, (uint64_t)tick.v);
	put_u32(w, (uint32_t)count);
	for (ptrdiff_t k = 0; k < count; k++) {
		const Command *c = &cmds[k];
		put_u32(w, c->faction.v);
		put_u32(w, c->player.v);
		put_u32(w, c->to.v);
		put_u32(w, c->kind);
		put_f32(w, c->target.x);
		put_f32(w, c->target.z);
		put_u32(w, c->unit_type);
		put_u32(w, (uint32_t)c->amount);
		put_f32(w, c->radius);
		put_u32(w, c->flags);
		put_u32(w, c->route);
		put_u32(w, (uint32_t)c->count);
		for (ptrdiff_t i = 0; i < c->count; i++) {
			put_u32(w, c->units[i].index);
			put_u32(w, c->units[i].generation);
		}
	}
	put_u64(w, after.v);
}

Str replay_bytes(const ReplayWriter *w)
{
	return (Str){(const char *)w->start, w->len};
}

bool replay_flush(ReplayWriter *w, FILE *f)
{
	bool ok = w->len == 0 || (fwrite(w->start, 1, (size_t)w->len, f) == (size_t)w->len && fflush(f) == 0);
	arena_reset(w->buf);
	w->start = nullptr;
	w->len = 0;
	return ok;
}

// ---------------------------------------------------------------- reading

const char *replay_status_name(ReplayStatus s)
{
	switch (s) {
	case REPLAY_OK: return "ok";
	case REPLAY_END: return "end";
	case REPLAY_BAD_MAGIC: return "not a replay file";
	case REPLAY_BAD_VERSION: return "unsupported replay version";
	case REPLAY_TRUNCATED: return "truncated";
	case REPLAY_BAD_CONFIG: return "config out of range";
	case REPLAY_BAD_COMMAND: return "malformed command";
	case REPLAY_TOO_LARGE: return "too large to decode";
	}
	return "unknown";
}

static bool get_u32(ReplayReader *r, uint32_t *x)
{
	if (r->data.len - r->pos < 4) return false;
	const uint8_t *b = (const uint8_t *)r->data.ptr + r->pos;
	*x = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	r->pos += 4;
	return true;
}

static bool get_u64(ReplayReader *r, uint64_t *x)
{
	uint32_t lo, hi;
	if (!get_u32(r, &lo) || !get_u32(r, &hi)) return false;
	*x = (uint64_t)hi << 32 | lo;
	return true;
}

static bool get_f32(ReplayReader *r, float *f)
{
	uint32_t bits;
	if (!get_u32(r, &bits)) return false;
	memcpy(f, &bits, sizeof bits);
	return true;
}

static bool get_i32(ReplayReader *r, int32_t *x)
{
	uint32_t u;
	if (!get_u32(r, &u)) return false;
	*x = (int32_t)u;
	return true;
}

ReplayStatus replay_open(ReplayReader *r, Str data)
{
	*r = (ReplayReader){.data = data};
	uint32_t magic, version;
	if (!get_u32(r, &magic)) return REPLAY_TRUNCATED;
	if (magic != REPLAY_MAGIC) return REPLAY_BAD_MAGIC;
	if (!get_u32(r, &version)) return REPLAY_TRUNCATED;
	if (version != REPLAY_VERSION) return REPLAY_BAD_VERSION;
	SimConfig *c = &r->config;
	bool ok = get_u64(r, &c->seed) && get_i32(r, &c->initial_units) && get_i32(r, &c->population)
	       && get_i32(r, &c->spawn_interval) && get_i32(r, &c->max_units)
	       && get_i32(r, &c->max_projectiles);
	int32_t weights = 0;
	bool weights_ok = true;
	for (int k = 0; k < COMBAT_TYPE_COUNT && ok; k++) {
		ok = get_i32(r, &c->type_weight[k]);
		weights_ok = weights_ok && c->type_weight[k] >= 0 && c->type_weight[k] <= 1000;
		weights += ok && weights_ok ? c->type_weight[k] : 0;
	}
	uint32_t economy, bases;
	if (!ok || !get_u32(r, &economy) || !get_u32(r, &bases)) return REPLAY_TRUNCATED;
	if (!weights_ok || weights <= 0 || economy > 1 || bases >= 1u << FACTION_COUNT) return REPLAY_BAD_CONFIG;
	c->economy = economy;
	c->bases = (uint8_t)bases;
	for (int f = 0; f < FACTION_COUNT; f++) {
		uint32_t players;
		if (!get_u32(r, &players)) return REPLAY_TRUNCATED;
		if (players < 1 || players > MAX_PLAYERS) return REPLAY_BAD_CONFIG;
		c->players[f] = (uint8_t)players;
	}
	// sim_init asserts on these; a file must not be able to trip an assert.
	if (c->max_units <= 0 || c->max_projectiles <= 0 || c->spawn_interval <= 0
	    || c->population > c->max_units || c->initial_units > c->population
	    || c->initial_units < 0 || c->max_units > (1 << 16) || c->max_projectiles > (1 << 20))
		return REPLAY_BAD_CONFIG;
	uint32_t snap;
	if (!get_u32(r, &snap)) return REPLAY_TRUNCATED;
	if ((ptrdiff_t)snap > r->data.len - r->pos) return REPLAY_TRUNCATED;
	r->snapshot = (Str){r->data.ptr + r->pos, (ptrdiff_t)snap};
	r->pos += snap;
	return REPLAY_OK;
}

ReplayStatus replay_next(ReplayReader *r, Arena *arena, Tick *tick,
                         Command **cmds, ptrdiff_t *count, Hash *after)
{
	if (r->pos == r->data.len) return REPLAY_END;
	uint64_t t;
	uint32_t n;
	if (!get_u64(r, &t) || !get_u32(r, &n)) return REPLAY_TRUNCATED;
	// Each command takes at least 48 bytes, which bounds n before allocating.
	if ((ptrdiff_t)n > (r->data.len - r->pos) / 48) return REPLAY_TRUNCATED;
	Command *out = arena_try_push(arena, (ptrdiff_t)n * (ptrdiff_t)sizeof(Command), alignof(Command));
	if (n && !out) return REPLAY_TOO_LARGE;
	for (uint32_t k = 0; k < n; k++) {
		uint32_t faction, player, to, kind, unit_type, amount, flags, route, units;
		V2 target;
		float radius;
		if (!get_u32(r, &faction) || !get_u32(r, &player) || !get_u32(r, &to) || !get_u32(r, &kind) || !get_f32(r, &target.x)
		    || !get_f32(r, &target.z) || !get_u32(r, &unit_type) || !get_u32(r, &amount)
		    || !get_f32(r, &radius) || !get_u32(r, &flags) || !get_u32(r, &route) || !get_u32(r, &units))
			return REPLAY_TRUNCATED;
		if (faction >= FACTION_COUNT || player >= MAX_PLAYERS || to >= MAX_PLAYERS || kind >= COMMAND_KIND_COUNT || unit_type >= UNIT_TYPE_COUNT
		    || amount > SPAWN_MAX || flags > 0xff || !route_ok(route))
			return REPLAY_BAD_COMMAND;
		if ((ptrdiff_t)units > (r->data.len - r->pos) / 8) return REPLAY_TRUNCATED;
		UnitHandle *h = arena_try_push(arena, (ptrdiff_t)units * (ptrdiff_t)sizeof(UnitHandle), alignof(UnitHandle));
		if (units && !h) return REPLAY_TOO_LARGE;
		for (uint32_t i = 0; i < units; i++)
			if (!get_u32(r, &h[i].index) || !get_u32(r, &h[i].generation)) return REPLAY_TRUNCATED;
		out[k] = (Command){.faction = {faction}, .player = {(uint8_t)player}, .to = {(uint8_t)to},
		                   .kind = (CommandKind)kind, .target = target, .count = (ptrdiff_t)units, .units = h,
		                   .unit_type = (UnitType)unit_type, .amount = (int32_t)amount, .radius = radius,
		                   .flags = (uint8_t)flags, .route = route};
	}
	uint64_t hash;
	if (!get_u64(r, &hash)) return REPLAY_TRUNCATED;
	*tick = (Tick){(int64_t)t};
	*cmds = out;
	*count = (ptrdiff_t)n;
	*after = (Hash){hash};
	return REPLAY_OK;
}
