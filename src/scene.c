#include "scene.h"

Scene scene_from_sim(const Sim *sim)
{
	Scene s = {.terrain = &sim->terrain, .tick = sim->tick.v, .max_units = sim->config.max_units,
	           .economy = sim->config.economy, .spots = &sim->spots};
	memcpy(s.spot_owner, sim->spot_owner, sizeof s.spot_owner);
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Faction *fa = &sim->factions[f];
		const Units *u = &fa->units;
		const Projectiles *p = &fa->incoming;
		s.base[f] = fa->base;
		s.units[f] = (SceneUnits){u->count, u->px, u->py, u->pz, u->vx, u->vz, u->type, u->dense_slot, u->progress,
		                          u->order, u->work, u->owner};
		s.shells[f] = (SceneShells){p->count, p->px, p->py, p->pz, p->vx, p->vy, p->vz, p->weapon};
	}
	return s;
}

Scene scene_from_mirror(const Mirror *m, const Terrain *t, const V2 base[FACTION_COUNT], Arena *arena)
{
	// A spectator's mirror says nothing about spots or construction.
	Scene s = {.terrain = t, .tick = m->tick, .max_units = m->units[0].cap};
	memset(s.spot_owner, -1, sizeof s.spot_owner);
	for (int f = 0; f < FACTION_COUNT; f++) {
		s.base[f] = base[f];
		const MirrorUnits *u = &m->units[f];
		float *px = PUSH(arena, float, u->cap), *py = PUSH(arena, float, u->cap);
		float *pz = PUSH(arena, float, u->cap), *vx = PUSH(arena, float, u->cap);
		float *vz = PUSH(arena, float, u->cap);
		UnitType *type = PUSH(arena, UnitType, u->cap);
		int32_t *slot = PUSH(arena, int32_t, u->cap);
		ptrdiff_t n = 0;
		for (ptrdiff_t k = 0; k < u->cap; k++) {
			if (!u->live[k]) continue;
			V2 p = mirror_predict(u, k, m->tick);
			px[n] = p.x;
			pz[n] = p.z;
			py[n] = terrain_height(t, p.x, p.z);
			vx[n] = dq_vel(u->qvx[k]);
			vz[n] = dq_vel(u->qvz[k]);
			type[n] = u->type[k];
			slot[n] = (int32_t)k;
			n++;
		}
		s.units[f] = (SceneUnits){n, px, py, pz, vx, vz, type, slot, nullptr, nullptr, nullptr, nullptr};
		const MirrorShells *sh = &m->shells[f];
		s.shells[f] = (SceneShells){sh->count, sh->px, sh->py, sh->pz, sh->vx, sh->vy, sh->vz, sh->weapon};
	}
	return s;
}
