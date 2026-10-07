#include "replay.h"

static constexpr uint32_t REPLAY_MAGIC = 0x52535452u; // "RTSR" read as little-endian
static constexpr uint32_t REPLAY_VERSION = 1;

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
	*w = (ReplayWriter){.buf = buf};
	put_u32(w, REPLAY_MAGIC);
	put_u32(w, REPLAY_VERSION);
	put_u64(w, c.seed);
	put_u32(w, (uint32_t)c.initial_units);
	put_u32(w, (uint32_t)c.population);
	put_u32(w, (uint32_t)c.spawn_interval);
	put_u32(w, (uint32_t)c.max_units);
	put_u32(w, (uint32_t)c.max_projectiles);
}

void replay_tick(ReplayWriter *w, Tick tick, const Command *cmds, ptrdiff_t count, Hash after)
{
	put_u64(w, (uint64_t)tick.v);
	put_u32(w, (uint32_t)count);
	for (ptrdiff_t k = 0; k < count; k++) {
		const Command *c = &cmds[k];
		put_u32(w, c->faction.v);
		put_u32(w, c->kind);
		put_f32(w, c->target.x);
		put_f32(w, c->target.z);
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
	if (!ok) return REPLAY_TRUNCATED;
	// sim_init asserts on these; a file must not be able to trip an assert.
	if (c->max_units <= 0 || c->max_projectiles <= 0 || c->spawn_interval <= 0
	    || c->population > c->max_units || c->initial_units > c->population
	    || c->initial_units < 0 || c->max_units > (1 << 16) || c->max_projectiles > (1 << 20))
		return REPLAY_BAD_CONFIG;
	return REPLAY_OK;
}

ReplayStatus replay_next(ReplayReader *r, Arena *arena, Tick *tick,
                         Command **cmds, ptrdiff_t *count, Hash *after)
{
	if (r->pos == r->data.len) return REPLAY_END;
	uint64_t t;
	uint32_t n;
	if (!get_u64(r, &t) || !get_u32(r, &n)) return REPLAY_TRUNCATED;
	// Each command takes at least 20 bytes, which bounds n before allocating.
	if ((ptrdiff_t)n > (r->data.len - r->pos) / 20) return REPLAY_TRUNCATED;
	Command *out = arena_try_push(arena, (ptrdiff_t)n * (ptrdiff_t)sizeof(Command), alignof(Command));
	if (n && !out) return REPLAY_TOO_LARGE;
	for (uint32_t k = 0; k < n; k++) {
		uint32_t faction, kind, units;
		V2 target;
		if (!get_u32(r, &faction) || !get_u32(r, &kind) || !get_f32(r, &target.x)
		    || !get_f32(r, &target.z) || !get_u32(r, &units))
			return REPLAY_TRUNCATED;
		if (faction >= FACTION_COUNT || kind >= ORDER_KIND_COUNT) return REPLAY_BAD_COMMAND;
		if ((ptrdiff_t)units > (r->data.len - r->pos) / 8) return REPLAY_TRUNCATED;
		UnitHandle *h = arena_try_push(arena, (ptrdiff_t)units * (ptrdiff_t)sizeof(UnitHandle), alignof(UnitHandle));
		if (units && !h) return REPLAY_TOO_LARGE;
		for (uint32_t i = 0; i < units; i++)
			if (!get_u32(r, &h[i].index) || !get_u32(r, &h[i].generation)) return REPLAY_TRUNCATED;
		out[k] = (Command){(FactionId){faction}, (OrderKind)kind, target, (ptrdiff_t)units, h};
	}
	uint64_t hash;
	if (!get_u64(r, &hash)) return REPLAY_TRUNCATED;
	*tick = (Tick){(int64_t)t};
	*cmds = out;
	*count = (ptrdiff_t)n;
	*after = (Hash){hash};
	return REPLAY_OK;
}
