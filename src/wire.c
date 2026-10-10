#include "wire.h"

enum : uint8_t { UNIT_SPAWN = 1, UNIT_MOVE, UNIT_HP, UNIT_DEATH };

// ---------------------------------------------------------------- mirror

void mirror_init(Mirror *m, Arena *a, ptrdiff_t max_units, ptrdiff_t max_shells)
{
	*m = (Mirror){.dilation = 1000};
	for (int f = 0; f < FACTION_COUNT; f++) {
		MirrorUnits *u = &m->units[f];
		u->cap = max_units;
		u->live = PUSH(a, uint8_t, max_units);
		u->gen = PUSH(a, uint32_t, max_units);
		u->type = PUSH(a, UnitType, max_units);
		u->qx = PUSH(a, uint16_t, max_units);
		u->qz = PUSH(a, uint16_t, max_units);
		u->qhp = PUSH(a, uint16_t, max_units);
		u->qvx = PUSH(a, int16_t, max_units);
		u->qvz = PUSH(a, int16_t, max_units);
		u->sent = PUSH(a, int64_t, max_units);
		MirrorShells *s = &m->shells[f];
		s->cap = max_shells;
		s->index = idmap_make(a, max_shells);
		s->id = PUSH(a, uint32_t, max_shells);
		s->px = PUSH(a, float, max_shells);
		s->py = PUSH(a, float, max_shells);
		s->pz = PUSH(a, float, max_shells);
		s->vx = PUSH(a, float, max_shells);
		s->vy = PUSH(a, float, max_shells);
		s->vz = PUSH(a, float, max_shells);
		s->weapon = PUSH(a, UnitType, max_shells);
		s->seen = PUSH(a, int64_t, max_shells);
	}
}

void mirror_clear(Mirror *m)
{
	m->tick = 0;
	m->dilation = 1000;
	for (int f = 0; f < FACTION_COUNT; f++) {
		memset(m->units[f].live, 0, (size_t)m->units[f].cap);
		m->shells[f].count = 0;
		idmap_clear(&m->shells[f].index);
	}
}

static Hash hash_shell(const MirrorShells *s, ptrdiff_t i)
{
	Hash h = hash_begin();
	h = hash_u32(h, s->id[i]);
	h = hash_f32(h, s->px[i]);
	h = hash_f32(h, s->py[i]);
	h = hash_f32(h, s->pz[i]);
	h = hash_f32(h, s->vx[i]);
	h = hash_f32(h, s->vy[i]);
	h = hash_f32(h, s->vz[i]);
	return hash_u32(h, s->weapon[i]);
}

Hash mirror_digest(const Mirror *m)
{
	Hash h = hash_u64(hash_begin(), (uint64_t)m->tick);
	for (int f = 0; f < FACTION_COUNT; f++) {
		const MirrorUnits *u = &m->units[f];
		for (ptrdiff_t s = 0; s < u->cap; s++) {
			if (!u->live[s]) continue;
			h = hash_u32(h, (uint32_t)s);
			h = hash_u32(h, u->gen[s]);
			h = hash_u32(h, u->type[s]);
			h = hash_u32(h, (uint32_t)u->qx[s] << 16 | u->qz[s]);
			h = hash_u32(h, (uint32_t)(uint16_t)u->qvx[s] << 16 | (uint16_t)u->qvz[s]);
			h = hash_u32(h, u->qhp[s]);
			h = hash_u64(h, (uint64_t)u->sent[s]);
		}
		// Shells are unordered on each side: combine their hashes by sum.
		const MirrorShells *sh = &m->shells[f];
		uint64_t sum = 0;
		for (ptrdiff_t i = 0; i < sh->count; i++) sum += hash_shell(sh, i).v;
		h = hash_u64(hash_u64(h, (uint64_t)sh->count), sum);
	}
	return h;
}

static ptrdiff_t shell_add(MirrorShells *s, uint32_t id)
{
	ptrdiff_t i = s->count++;
	s->id[i] = id;
	idmap_put(&s->index, id, (int32_t)i);
	return i;
}

static void shell_remove(MirrorShells *s, ptrdiff_t i)
{
	idmap_del(&s->index, s->id[i]);
	ptrdiff_t last = --s->count;
	if (i == last) return;
	s->id[i] = s->id[last];
	s->px[i] = s->px[last];
	s->py[i] = s->py[last];
	s->pz[i] = s->pz[last];
	s->vx[i] = s->vx[last];
	s->vy[i] = s->vy[last];
	s->vz[i] = s->vz[last];
	s->weapon[i] = s->weapon[last];
	s->seen[i] = s->seen[last];
	idmap_put(&s->index, s->id[i], (int32_t)i);
}

// ---------------------------------------------------------------- encoding

static void put_header(ByteBuf *b, int64_t tick, uint16_t dilation, bool digest, Hash h)
{
	put64(b, (uint64_t)tick);
	put16(b, dilation);
	put8(b, digest);
	if (digest) put64(b, h.v);
}

static void put_shell(ByteBuf *b, const MirrorShells *s, ptrdiff_t i)
{
	put32(b, s->id[i]);
	putf(b, s->px[i]);
	putf(b, s->py[i]);
	putf(b, s->pz[i]);
	putf(b, s->vx[i]);
	putf(b, s->vy[i]);
	putf(b, s->vz[i]);
	put8(b, s->weapon[i]);
}

static void put_spawn(ByteBuf *b, const MirrorUnits *u, ptrdiff_t s, int64_t tick)
{
	put8(b, UNIT_SPAWN);
	put16(b, (uint16_t)s);
	put32(b, u->gen[s]);
	put8(b, u->type[s]);
	put16(b, u->qx[s]);
	put16(b, u->qz[s]);
	put16(b, (uint16_t)u->qvx[s]);
	put16(b, (uint16_t)u->qvz[s]);
	put16(b, u->qhp[s]);
	int64_t age = tick - u->sent[s];
	put16(b, (uint16_t)(age < 0 ? 0 : age > 65535 ? 65535 : age));
}

// Counts are written after the entries, so reserve their place first.
static ptrdiff_t reserve32(ByteBuf *b)
{
	ptrdiff_t at = b->len;
	put32(b, 0);
	return at;
}

static void patch32(ByteBuf *b, ptrdiff_t at, uint32_t x)
{
	if (b->overflow) return;
	uint8_t v[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)};
	memcpy(b->data + at, v, 4);
}

void wire_encode_tick(Mirror *m, const Sim *sim, ByteBuf *out, bool digest, uint16_t dilation,
                      WireStats *stats)
{
	int64_t tick = sim->tick.v;
	m->tick = tick;
	m->dilation = dilation;

	// Units are diffed first; the header (with the digest of the updated
	// mirror) is written into a separate buffer below, so build the body in
	// place after leaving room for the largest header.
	ptrdiff_t header_at = out->len;
	put_bytes(out, (uint8_t[19]){0}, 19);

	for (int f = 0; f < FACTION_COUNT; f++) {
		const Units *su = &sim->factions[f].units;
		MirrorUnits *u = &m->units[f];
		ptrdiff_t count_at = reserve32(out);
		uint32_t n = 0;
		for (ptrdiff_t s = 0; s < su->cap; s++) {
			int32_t d = su->slot_dense[s];
			bool alive = d >= 0;
			if (!alive) {
				if (u->live[s]) {
					u->live[s] = 0;
					put8(out, UNIT_DEATH);
					put16(out, (uint16_t)s);
					n++;
				}
				continue;
			}
			uint16_t qhp = q_hp(su->hp[d]);
			int16_t qvx = q_vel(su->vx[d]), qvz = q_vel(su->vz[d]);
			if (!u->live[s] || u->gen[s] != su->slot_gen[s]) {
				// A new unit, or a slot reused since the client last saw it.
				u->live[s] = 1;
				u->gen[s] = su->slot_gen[s];
				u->type[s] = su->type[d];
				u->qx[s] = q_pos(su->px[d]);
				u->qz[s] = q_pos(su->pz[d]);
				u->qvx[s] = qvx;
				u->qvz[s] = qvz;
				u->qhp[s] = qhp;
				u->sent[s] = tick;
				put_spawn(out, u, s, tick);
				n++;
				continue;
			}
			V2 guess = mirror_predict(u, s, tick);
			float dx = guess.x - su->px[d], dz = guess.z - su->pz[d];
			bool drifted = dx * dx + dz * dz > RESEND_DISTANCE * RESEND_DISTANCE;
			bool turned = abs(qvx - u->qvx[s]) > RESEND_VELOCITY || abs(qvz - u->qvz[s]) > RESEND_VELOCITY;
			// Snapshots carry the age since sent in 16 bits: refresh before it wraps.
			bool stale = tick - u->sent[s] > RESEND_AGE;
			if (drifted || turned || stale) {
				u->qx[s] = q_pos(su->px[d]);
				u->qz[s] = q_pos(su->pz[d]);
				u->qvx[s] = qvx;
				u->qvz[s] = qvz;
				u->sent[s] = tick;
				put8(out, UNIT_MOVE);
				put16(out, (uint16_t)s);
				put16(out, u->qx[s]);
				put16(out, u->qz[s]);
				put16(out, (uint16_t)qvx);
				put16(out, (uint16_t)qvz);
				n++;
			}
			if (qhp != u->qhp[s]) {
				u->qhp[s] = qhp;
				put8(out, UNIT_HP);
				put16(out, (uint16_t)s);
				put16(out, qhp);
				n++;
			}
		}
		patch32(out, count_at, n);
		if (stats) stats->units_sent += n;
	}

	for (int f = 0; f < FACTION_COUNT; f++) {
		const Projectiles *p = &sim->factions[f].incoming;
		MirrorShells *s = &m->shells[f];
		ptrdiff_t starts_at = reserve32(out);
		uint32_t starts = 0;
		for (ptrdiff_t k = 0; k < p->count; k++) {
			int32_t i = idmap_get(&s->index, p->id[k]);
			bool fresh = i < 0;
			if (fresh) i = (int32_t)shell_add(s, p->id[k]);
			// Keep the exact state: snapshots send it, and the digest checks
			// clients flew theirs to the same bits.
			s->px[i] = p->px[k];
			s->py[i] = p->py[k];
			s->pz[i] = p->pz[k];
			s->vx[i] = p->vx[k];
			s->vy[i] = p->vy[k];
			s->vz[i] = p->vz[k];
			s->weapon[i] = p->weapon[k];
			s->seen[i] = tick;
			if (fresh) {
				put_shell(out, s, i);
				starts++;
			}
		}
		patch32(out, starts_at, starts);
		ptrdiff_t ends_at = reserve32(out);
		uint32_t ends = 0;
		for (ptrdiff_t i = s->count - 1; i >= 0; i--) {
			if (s->seen[i] == tick) continue;
			put32(out, s->id[i]);
			shell_remove(s, i);
			ends++;
		}
		patch32(out, ends_at, ends);
		if (stats) {
			stats->shells_started += starts;
			stats->shells_ended += ends;
		}
	}

	// Now the header, with the digest of the mirror as just updated. It was
	// given 19 bytes; without a digest it needs 11, so shift the body down.
	ByteBuf head = {(uint8_t[19]){0}, 19, 0, false};
	put_header(&head, tick, dilation, digest, digest ? mirror_digest(m) : (Hash){0});
	if (out->overflow) return;
	ptrdiff_t body_at = header_at + 19, gap = 19 - head.len;
	memcpy(out->data + header_at, head.data, (size_t)head.len);
	memmove(out->data + header_at + head.len, out->data + body_at, (size_t)(out->len - body_at));
	out->len -= gap;
}

void wire_encode_snapshot(const Mirror *m, ByteBuf *out)
{
	put_header(out, m->tick, m->dilation, true, mirror_digest(m));
	for (int f = 0; f < FACTION_COUNT; f++) {
		const MirrorUnits *u = &m->units[f];
		ptrdiff_t count_at = reserve32(out);
		uint32_t n = 0;
		for (ptrdiff_t s = 0; s < u->cap; s++) {
			if (!u->live[s]) continue;
			put_spawn(out, u, s, m->tick);
			n++;
		}
		patch32(out, count_at, n);
	}
	for (int f = 0; f < FACTION_COUNT; f++) {
		const MirrorShells *s = &m->shells[f];
		put32(out, (uint32_t)s->count);
		for (ptrdiff_t i = 0; i < s->count; i++) put_shell(out, s, i);
		put32(out, 0);
	}
}

// ---------------------------------------------------------------- applying

static bool finite(float x) { return x == x && x - x == 0.0f; }

ApplyResult wire_apply(Mirror *m, ByteReader *r, bool snapshot, ApplyInfo *info)
{
	*info = (ApplyInfo){0};
	int64_t tick = (int64_t)get64(r);
	uint16_t dilation = get16(r);
	uint8_t has_digest = get8(r);
	Hash digest = {has_digest ? get64(r) : 0};
	if (r->bad || has_digest > 1 || dilation > 10000) return APPLY_BAD;
	if (snapshot) {
		mirror_clear(m);
	} else {
		if (tick != m->tick + 1) return APPLY_GAP;
		// Everything in flight moves one tick before this tick's changes.
		for (int f = 0; f < FACTION_COUNT; f++) {
			MirrorShells *s = &m->shells[f];
			for (ptrdiff_t i = 0; i < s->count; i++) {
				V3 p = v3(s->px[i], s->py[i], s->pz[i]), v = v3(s->vx[i], s->vy[i], s->vz[i]);
				shell_advance(&p, &v);
				s->px[i] = p.x, s->py[i] = p.y, s->pz[i] = p.z;
				s->vx[i] = v.x, s->vy[i] = v.y, s->vz[i] = v.z;
			}
		}
	}
	m->tick = tick;
	m->dilation = dilation;

	for (int f = 0; f < FACTION_COUNT; f++) {
		MirrorUnits *u = &m->units[f];
		uint32_t n = get32(r);
		if (r->bad || n > (uint32_t)(4 * u->cap)) return APPLY_BAD;
		for (uint32_t k = 0; k < n; k++) {
			uint8_t tag = get8(r);
			uint16_t s = get16(r);
			if (r->bad || s >= u->cap) return APPLY_BAD;
			switch (tag) {
			case UNIT_SPAWN: {
				u->gen[s] = get32(r);
				uint8_t type = get8(r);
				u->qx[s] = get16(r);
				u->qz[s] = get16(r);
				u->qvx[s] = (int16_t)get16(r);
				u->qvz[s] = (int16_t)get16(r);
				u->qhp[s] = get16(r);
				uint16_t age = get16(r);
				if (type >= UNIT_TYPE_COUNT) return APPLY_BAD;
				u->type[s] = (UnitType)type;
				u->sent[s] = tick - age;
				u->live[s] = 1;
				break;
			}
			case UNIT_MOVE:
				if (!u->live[s]) return APPLY_BAD;
				u->qx[s] = get16(r);
				u->qz[s] = get16(r);
				u->qvx[s] = (int16_t)get16(r);
				u->qvz[s] = (int16_t)get16(r);
				u->sent[s] = tick;
				break;
			case UNIT_HP:
				if (!u->live[s]) return APPLY_BAD;
				u->qhp[s] = get16(r);
				break;
			case UNIT_DEATH:
				if (!u->live[s]) return APPLY_BAD;
				u->live[s] = 0;
				break;
			default:
				return APPLY_BAD;
			}
		}
	}
	for (int f = 0; f < FACTION_COUNT; f++) {
		MirrorShells *s = &m->shells[f];
		uint32_t starts = get32(r);
		if (r->bad || starts > (uint32_t)(s->cap - s->count)) return APPLY_BAD;
		for (uint32_t k = 0; k < starts; k++) {
			uint32_t id = get32(r);
			float p[6];
			for (int j = 0; j < 6; j++) p[j] = getf(r);
			uint8_t weapon = get8(r);
			if (r->bad || weapon >= UNIT_TYPE_COUNT || id == UINT32_MAX || idmap_get(&s->index, id) >= 0)
				return APPLY_BAD;
			for (int j = 0; j < 6; j++)
				if (!finite(p[j])) return APPLY_BAD;
			ptrdiff_t i = shell_add(s, id);
			s->px[i] = p[0], s->py[i] = p[1], s->pz[i] = p[2];
			s->vx[i] = p[3], s->vy[i] = p[4], s->vz[i] = p[5];
			s->weapon[i] = (UnitType)weapon;
		}
		uint32_t ends = get32(r);
		if (r->bad || ends > (uint32_t)s->count) return APPLY_BAD;
		for (uint32_t k = 0; k < ends; k++) {
			int32_t i = idmap_get(&s->index, get32(r));
			if (r->bad || i < 0) return APPLY_BAD;
			shell_remove(s, i);
		}
	}
	if (r->pos != r->len) return APPLY_BAD;
	if (has_digest) {
		info->had_digest = true;
		info->digest_matched = mirror_digest(m).v == digest.v;
	}
	return APPLY_OK;
}

// ---------------------------------------------------------------- faction stream

void wire_encode_faction_tick(const Sim *sim, FactionId f, const Command *cmds, ptrdiff_t count,
                              bool hash, uint16_t dilation, ByteBuf *out)
{
	const Faction *fa = &sim->factions[f.v];
	put64(out, (uint64_t)(sim->tick.v - 1));
	put16(out, dilation);
	put8(out, hash);
	if (hash) put64(out, faction_hash(fa).v);
	put64(out, sim->effects[f.v].spots);

	ptrdiff_t count_at = reserve32(out);
	uint32_t n = 0;
	for (ptrdiff_t k = 0; k < count; k++) {
		const Command *c = &cmds[k];
		if (c->faction.v != f.v) continue;
		put8(out, c->player.v);
		put8(out, c->to.v);
		put8(out, c->kind);
		putf(out, c->target.x);
		putf(out, c->target.z);
		put8(out, c->unit_type);
		put32(out, (uint32_t)c->amount);
		putf(out, c->radius);
		put8(out, c->flags);
		put32(out, c->route);
		putf(out, c->metal);
		putf(out, c->energy);
		put32(out, (uint32_t)c->count);
		for (ptrdiff_t i = 0; i < c->count; i++) {
			put32(out, c->units[i].index);
			put32(out, c->units[i].generation);
		}
		n++;
	}
	patch32(out, count_at, n);

	const FactionEffects *fx = &sim->effects[f.v];
	put32(out, (uint32_t)fx->seen_count);
	for (ptrdiff_t k = 0; k < fx->seen_count; k++) {
		const SeenEntry *e = &fx->seen[k];
		put8(out, e->tag);
		put16(out, e->slot);
		if (e->tag == SEEN_LOST) continue;
		if (e->tag == SEEN_ENTER) {
			put32(out, e->gen);
			put8(out, e->type);
		}
		put16(out, e->qx);
		put16(out, e->qz);
		put16(out, (uint16_t)e->qvx);
		put16(out, (uint16_t)e->qvz);
	}
	put32(out, (uint32_t)fx->arrived_count);
	for (ptrdiff_t k = 0; k < fx->arrived_count; k++) {
		const Launch *l = &fx->arrived[k];
		putf(out, l->pos.x);
		putf(out, l->pos.y);
		putf(out, l->pos.z);
		putf(out, l->vel.x);
		putf(out, l->vel.y);
		putf(out, l->vel.z);
		put8(out, l->weapon);
	}
}

void wire_encode_faction_snapshot(const Sim *sim, FactionId f, ByteBuf *out)
{
	put64(out, (uint64_t)sim->tick.v);
	faction_write(&sim->factions[f.v], out);
}

bool wire_read_faction_tick(ByteReader *r, Arena *a, const SimConfig *c, FactionId f, FactionTick *out)
{
	*out = (FactionTick){0};
	out->tick = (int64_t)get64(r);
	out->dilation = get16(r);
	uint8_t has_hash = get8(r);
	if (r->bad || has_hash > 1 || out->dilation > 10000) return false;
	out->has_hash = has_hash;
	if (has_hash) out->hash = (Hash){get64(r)};
	out->effects.spots = get64(r);

	uint32_t n = get32(r);
	if (r->bad || n > 4096) return false;
	out->commands = PUSH(a, Command, n);
	out->command_count = n;
	for (uint32_t k = 0; k < n; k++) {
		Command *cmd = &out->commands[k];
		cmd->player = (PlayerSlot){get8(r)};
		cmd->to = (PlayerSlot){get8(r)};
		uint8_t kind = get8(r);
		cmd->target = v2(getf(r), getf(r));
		uint8_t type = get8(r);
		cmd->amount = (int32_t)get32(r);
		cmd->radius = getf(r);
		cmd->flags = get8(r);
		cmd->route = get32(r);
		cmd->metal = getf(r);
		cmd->energy = getf(r);
		uint32_t m = get32(r);
		if (r->bad || kind >= COMMAND_KIND_COUNT || type >= UNIT_TYPE_COUNT || m > (uint32_t)c->max_units
		    || !player_ok(cmd->player) || !player_ok(cmd->to)
		    || !route_ok(cmd->route)
		    || (ptrdiff_t)m * 8 > r->len - r->pos)
			return false;
		cmd->faction = f;
		cmd->kind = (CommandKind)kind;
		cmd->unit_type = (UnitType)type;
		UnitHandle *h = PUSH(a, UnitHandle, m);
		for (uint32_t i = 0; i < m; i++) h[i] = (UnitHandle){get32(r), get32(r)};
		cmd->units = h;
		cmd->count = m;
	}

	FactionEffects *fx = &out->effects;
	uint32_t seen = get32(r);
	if (r->bad || seen > (uint32_t)c->max_units) return false;
	fx->seen = PUSH(a, SeenEntry, seen);
	fx->seen_count = seen;
	for (uint32_t k = 0; k < seen; k++) {
		SeenEntry *e = &fx->seen[k];
		uint8_t tag = get8(r);
		e->slot = get16(r);
		if (r->bad || tag < SEEN_ENTER || tag > SEEN_LOST || e->slot >= c->max_units) return false;
		e->tag = (SeenTag)tag;
		if (e->tag == SEEN_LOST) continue;
		if (e->tag == SEEN_ENTER) {
			e->gen = get32(r);
			uint8_t type = get8(r);
			if (type >= UNIT_TYPE_COUNT) return false;
			e->type = (UnitType)type;
		}
		e->qx = get16(r);
		e->qz = get16(r);
		e->qvx = (int16_t)get16(r);
		e->qvz = (int16_t)get16(r);
	}
	uint32_t arrived = get32(r);
	if (r->bad || arrived > (uint32_t)c->max_units) return false;
	fx->arrived = PUSH(a, Launch, arrived);
	fx->arrived_count = arrived;
	for (uint32_t k = 0; k < arrived; k++) {
		Launch *l = &fx->arrived[k];
		float v[6];
		for (int j = 0; j < 6; j++) {
			v[j] = getf(r);
			if (!finite(v[j])) return false;
		}
		uint8_t weapon = get8(r);
		if (weapon >= UNIT_TYPE_COUNT) return false;
		*l = (Launch){v3(v[0], v[1], v[2]), v3(v[3], v[4], v[5]), (UnitType)weapon, f};
	}
	return !r->bad && r->pos == r->len;
}

// ---------------------------------------------------------------- config

void wire_put_config(ByteBuf *b, const SimConfig *c)
{
	put64(b, c->seed);
	put32(b, (uint32_t)c->initial_units);
	put32(b, (uint32_t)c->population);
	put32(b, (uint32_t)c->spawn_interval);
	put32(b, (uint32_t)c->max_units);
	put32(b, (uint32_t)c->max_projectiles);
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) put32(b, (uint32_t)c->type_weight[k]);
	put8(b, c->economy);
	put8(b, c->bases);
	for (int f = 0; f < FACTION_COUNT; f++) put8(b, c->players[f]);
	put8(b, c->commanders);
	ZoneContext zc = c->context;
	for (int k = 0; k < ZONE_CONTEXT_FIELDS; k++) putf(b, *zone_context_field(&zc, k));
}

bool wire_get_config(ByteReader *r, SimConfig *c)
{
	c->seed = get64(r);
	c->initial_units = (int32_t)get32(r);
	c->population = (int32_t)get32(r);
	c->spawn_interval = (int32_t)get32(r);
	c->max_units = (int32_t)get32(r);
	c->max_projectiles = (int32_t)get32(r);
	int32_t sum = 0;
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) {
		c->type_weight[k] = (int32_t)get32(r);
		if (c->type_weight[k] < 0 || c->type_weight[k] > 1000) return false;
		sum += c->type_weight[k];
	}
	uint8_t economy = get8(r), bases = get8(r);
	if (economy > 1 || bases >= 1u << FACTION_COUNT) return false;
	c->economy = economy;
	c->bases = bases;
	for (int f = 0; f < FACTION_COUNT; f++) {
		c->players[f] = get8(r);
		if (c->players[f] < 1 || c->players[f] > MAX_PLAYERS) return false;
	}
	uint8_t commanders = get8(r);
	if (commanders > 1) return false;
	c->commanders = commanders;
	for (int k = 0; k < ZONE_CONTEXT_FIELDS; k++) *zone_context_field(&c->context, k) = getf(r);
	return !r->bad && zone_context_ok(&c->context) && sum > 0 && c->max_units > 0 && c->max_units <= 1 << 16
	    && c->max_projectiles > 0 && c->max_projectiles <= 1 << 20 && c->spawn_interval > 0
	    && c->initial_units >= 0 && c->initial_units <= c->population && c->population <= c->max_units;
}
