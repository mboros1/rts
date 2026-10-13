#include "remote.h"

void remote_init(Remote *r, const SimConfig *config, FactionId faction, Arena *program)
{
	*r = (Remote){.config = *config, .faction = faction, .dilation = 1000};
	sim_init(&r->sim, *config);
	r->scratch = arena_create("remote tick", (ptrdiff_t)64 << 20);
	OutgoingShells *o = &r->outgoing;
	o->cap = config->max_projectiles;
	o->px = PUSH(program, float, o->cap);
	o->py = PUSH(program, float, o->cap);
	o->pz = PUSH(program, float, o->cap);
	o->vx = PUSH(program, float, o->cap);
	o->vy = PUSH(program, float, o->cap);
	o->vz = PUSH(program, float, o->cap);
	o->weapon = PUSH(program, UnitType, o->cap);
	o->ttl = PUSH(program, int32_t, o->cap);
}

void remote_destroy(Remote *r)
{
	arena_destroy(&r->scratch);
	sim_destroy(&r->sim);
}

bool remote_snapshot(Remote *r, ByteReader *in)
{
	int64_t tick = (int64_t)get64(in);
	r->ready = !in->bad && tick >= 0 && faction_read(&r->sim.factions[r->faction.v], in) && in->pos == in->len;
	r->sim.tick.v = tick;
	r->outgoing.count = 0;
	return r->ready;
}

// Own shells in flight, for drawing: advance them, drop the ones that came
// down or met an enemy we perceive, then add this tick's launches.
static void fly_outgoing(Remote *r)
{
	OutgoingShells *o = &r->outgoing;
	const Faction *f = &r->sim.factions[r->faction.v];
	const EnemyView *v = &f->view;
	const Terrain *t = &r->sim.terrain;
	for (ptrdiff_t i = o->count - 1; i >= 0; i--) {
		V3 p0 = v3(o->px[i], o->py[i], o->pz[i]), vel = v3(o->vx[i], o->vy[i], o->vz[i]), p1 = p0;
		shell_advance(&p1, &vel);
		bool gone = --o->ttl[i] <= 0 || p1.y <= terrain_height(t, p1.x, p1.z);
		CellRange cr = grid_range(&v->grid, f_min(p0.x, p1.x) - MAX_UNIT_RADIUS, f_min(p0.z, p1.z) - MAX_UNIT_RADIUS,
		                          f_max(p0.x, p1.x) + MAX_UNIT_RADIUS, f_max(p0.z, p1.z) + MAX_UNIT_RADIUS);
		int32_t j;
		GRID_FOR(&v->grid, cr, j) {
			if (gone) break;
			V3 c = v3(v->px[j], v->py[j] + v->radius[j], v->pz[j]);
			V3 off = v3_sub(c, p1);
			gone = v3_dot(off, off) < v->radius[j] * v->radius[j] * 2.0f;
		}
		if (gone) {
			ptrdiff_t last = --o->count;
			o->px[i] = o->px[last], o->py[i] = o->py[last], o->pz[i] = o->pz[last];
			o->vx[i] = o->vx[last], o->vy[i] = o->vy[last], o->vz[i] = o->vz[last];
			o->weapon[i] = o->weapon[last];
			o->ttl[i] = o->ttl[last];
			continue;
		}
		o->px[i] = p1.x, o->py[i] = p1.y, o->pz[i] = p1.z;
		o->vx[i] = vel.x, o->vy[i] = vel.y, o->vz[i] = vel.z;
	}
	for (ptrdiff_t k = 0; k < f->outbox_count && o->count < o->cap; k++) {
		const Launch *l = &f->outbox[k];
		ptrdiff_t i = o->count++;
		o->px[i] = l->pos.x, o->py[i] = l->pos.y, o->pz[i] = l->pos.z;
		o->vx[i] = l->vel.x, o->vy[i] = l->vel.y, o->vz[i] = l->vel.z;
		o->weapon[i] = l->weapon;
		o->ttl[i] = PROJECTILE_TTL;
	}
}

RemoteResult remote_tick(Remote *r, ByteReader *in, bool *had_hash)
{
	*had_hash = false;
	if (!r->ready) return REMOTE_GAP;
	arena_reset(&r->scratch);
	FactionTick ft;
	if (!wire_read_faction_tick(in, &r->scratch, &r->config, r->faction, &ft)) return REMOTE_BAD;
	if (ft.tick != r->sim.tick.v) {
		r->ready = false;
		return REMOTE_GAP;
	}
	faction_tick(&r->sim, r->faction, &ft.effects, ft.commands, ft.command_count);
	r->dilation = ft.dilation;
	fly_outgoing(r);
	if (!ft.has_hash) return REMOTE_OK;
	*had_hash = true;
	r->hashes++;
	if (faction_hash(&r->sim.factions[r->faction.v]).v == ft.hash.v) return REMOTE_OK;
	r->hash_mismatches++;
	r->ready = false;
	return REMOTE_DIVERGED;
}

Scene remote_scene(const Remote *r, const V2 base[FACTION_COUNT], Arena *arena)
{
	const Sim *sim = &r->sim;
	FactionId me = r->faction, them = faction_enemy(me);
	const Faction *f = &sim->factions[me.v];
	Scene s = {.terrain = &sim->terrain, .tick = sim->tick.v, .max_units = sim->config.max_units,
	           .economy = sim->config.economy, .spots = &sim->spots};
	for (int k = 0; k < FACTION_COUNT; k++) s.base[k] = base[k];
	// A player knows which spots are its own, and nothing else about them.
	for (int k = 0; k < MAX_SPOTS; k++) s.spot_owner[k] = (int8_t)(spotset_has(&f->spots, k) ? (int)me.v : -1);

	const Units *u = &f->units;
	s.units[me.v] = (SceneUnits){u->count, u->px, u->py, u->pz, u->vx, u->vz, u->type, u->dense_slot, u->progress,
	                             u->order, u->work, u->owner};
	const Projectiles *p = &f->incoming;
	s.shells[me.v] = (SceneShells){p->count, p->px, p->py, p->pz, p->vx, p->vy, p->vz, p->weapon};

	// Enemies as perceived, dead-reckoned to the current tick.
	const Perception *seen = &f->seen;
	float *px = PUSH(arena, float, seen->cap), *py = PUSH(arena, float, seen->cap);
	float *pz = PUSH(arena, float, seen->cap), *vx = PUSH(arena, float, seen->cap);
	float *vz = PUSH(arena, float, seen->cap);
	UnitType *type = PUSH(arena, UnitType, seen->cap);
	int32_t *slot = PUSH(arena, int32_t, seen->cap);
	ptrdiff_t n = 0;
	for (ptrdiff_t k = 0; k < seen->cap; k++) {
		if (!seen->live[k]) continue;
		V2 at = perception_at(seen, k, sim->tick);
		px[n] = at.x;
		pz[n] = at.z;
		py[n] = terrain_height(&sim->terrain, at.x, at.z);
		vx[n] = dq_vel(seen->qvx[k]);
		vz[n] = dq_vel(seen->qvz[k]);
		type[n] = seen->type[k];
		slot[n] = (int32_t)k;
		n++;
	}
	s.units[them.v] = (SceneUnits){n, px, py, pz, vx, vz, type, slot, nullptr, nullptr, nullptr, nullptr};
	const OutgoingShells *o = &r->outgoing;
	s.shells[them.v] = (SceneShells){o->count, o->px, o->py, o->pz, o->vx, o->vy, o->vz, o->weapon};
	return s;
}

void remote_fog(const Remote *r, uint8_t *visible)
{
	const Terrain *t = &r->sim.terrain;
	const Units *u = &r->sim.factions[r->faction.v].units;
	int32_t sx = t->cells_x + 1, sz = t->cells_z + 1;
	memset(visible, 0, (size_t)sx * (size_t)sz);
	// Rasterize each unit's sight circle onto the heightfield's samples.
	for (ptrdiff_t i = 0; i < u->count; i++) {
		float sight = unit_defs[u->type[i]].sight;
		float cx = u->px[i] / t->cell, cz = u->pz[i] / t->cell, rc = sight / t->cell;
		int32_t z0 = (int32_t)f_max(0.0f, cz - rc), z1 = (int32_t)f_min((float)(sz - 1), cz + rc + 1.0f);
		for (int32_t j = z0; j <= z1; j++) {
			float dz = (float)j - cz, half2 = rc * rc - dz * dz;
			if (half2 < 0.0f) continue;
			float half = f_sqrt(half2);
			int32_t x0 = (int32_t)f_max(0.0f, cx - half + 0.999f), x1 = (int32_t)f_min((float)(sx - 1), cx + half);
			if (x1 >= x0) memset(visible + (ptrdiff_t)j * sx + x0, 1, (size_t)(x1 - x0 + 1));
		}
	}
	// Jammed (2): out of sight, on our radar, and within the reach of an
	// enemy jammer we know of. It looks it: static, not plain fog.
	const Perception *seen = &r->sim.factions[r->faction.v].seen;
	for (ptrdiff_t s = 0; s < seen->cap; s++) {
		if (!seen->live[s] || seen->type[s] != UNIT_JAMMER) continue;
		V2 at = perception_at(seen, s, r->sim.tick);
		float rc = unit_defs[UNIT_JAMMER].jam / t->cell, cx = at.x / t->cell, cz = at.z / t->cell;
		for (int32_t j = (int32_t)f_max(0.0f, cz - rc); j <= (int32_t)f_min((float)(sz - 1), cz + rc); j++)
			for (int32_t i = (int32_t)f_max(0.0f, cx - rc); i <= (int32_t)f_min((float)(sx - 1), cx + rc); i++) {
				uint8_t *v = visible + (ptrdiff_t)j * sx + i;
				float dx = (float)i - cx, dz = (float)j - cz;
				if (*v || dx * dx + dz * dz > rc * rc) continue;
				float x = (float)i * t->cell, z = (float)j * t->cell;
				for (ptrdiff_t k = 0; k < u->count && !*v; k++) {
					float reach = unit_defs[u->type[k]].radar;
					if (reach <= 0.0f || u->progress[k] < 1.0f) continue;
					float ex = u->px[k] - x, ez = u->pz[k] - z;
					if (ex * ex + ez * ez < reach * reach) *v = 2;
				}
			}
	}
}
