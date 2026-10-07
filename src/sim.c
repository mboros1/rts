#include "sim.h"

const UnitDef unit_defs[UNIT_TYPE_COUNT] = {
	[UNIT_RAIDER] = {
		.name = "raider", .speed = 9.0f, .accel = 30.0f, .radius = 0.8f,
		.height = 1.2f, .hp = 60.0f, .range = 24.0f, .reload = 15,
		.shell_speed = 45.0f, .damage = 12.0f, .splash = 0.0f,
		.spread = 0.6f, .high_arc = false, .weight = 6,
	},
	[UNIT_TANK] = {
		.name = "tank", .speed = 5.0f, .accel = 12.0f, .radius = 1.4f,
		.height = 1.8f, .hp = 240.0f, .range = 32.0f, .reload = 36,
		.shell_speed = 42.0f, .damage = 48.0f, .splash = 1.5f,
		.spread = 0.8f, .high_arc = false, .weight = 3,
	},
	[UNIT_ARTILLERY] = {
		.name = "artillery", .speed = 3.5f, .accel = 8.0f, .radius = 1.1f,
		.height = 1.4f, .hp = 90.0f, .range = 78.0f, .reload = 90,
		.shell_speed = 40.0f, .damage = 60.0f, .splash = 4.0f,
		.spread = 2.0f, .high_arc = true, .weight = 2,
	},
};

static constexpr float MAX_UNIT_RADIUS = 1.4f;
static constexpr float GRID_CELL = 8.0f;
static constexpr int32_t RETARGET_PERIOD = 8;
static constexpr float ARRIVE_RADIUS = 3.0f;
static constexpr int32_t PROJECTILE_TTL = 10 * 30;
static constexpr float SEPARATION_MAX = 0.5f;   // per-tick push, units
static constexpr uint32_t NO_SLOT = UINT32_MAX;

static constexpr UnitHandle NO_UNIT = {NO_SLOT, 0};

SimConfig sim_config_default(void)
{
	return (SimConfig){
		.seed = 1,
		.initial_units = 5000,
		.population = 5000,
		.spawn_interval = 1,
		.max_units = 8192,
		.max_projectiles = 65536,
		.type_weight = {
			unit_defs[UNIT_RAIDER].weight,
			unit_defs[UNIT_TANK].weight,
			unit_defs[UNIT_ARTILLERY].weight,
		},
	};
}

// ---------------------------------------------------------------- units

static void units_init(Units *u, Arena *a, ptrdiff_t cap)
{
	*u = (Units){.cap = cap};
	u->slot_gen = PUSH(a, uint32_t, cap);
	u->slot_dense = PUSH(a, int32_t, cap);
	u->free_slots = PUSH(a, int32_t, cap);
	u->dense_slot = PUSH(a, int32_t, cap);
	u->px = PUSH(a, float, cap);
	u->py = PUSH(a, float, cap);
	u->pz = PUSH(a, float, cap);
	u->vx = PUSH(a, float, cap);
	u->vz = PUSH(a, float, cap);
	u->hp = PUSH(a, float, cap);
	u->type = PUSH(a, UnitType, cap);
	u->order = PUSH(a, OrderKind, cap);
	u->tx = PUSH(a, float, cap);
	u->tz = PUSH(a, float, cap);
	u->cooldown = PUSH(a, int32_t, cap);
	u->target = PUSH(a, UnitHandle, cap);
	u->engaged = PUSH(a, uint8_t, cap);
	// Free list pops from the end, so the first spawn gets slot 0.
	for (ptrdiff_t s = 0; s < cap; s++) {
		u->slot_dense[s] = -1;
		u->free_slots[s] = (int32_t)(cap - 1 - s);
	}
	u->free_count = cap;
}

ptrdiff_t units_find(const Units *u, UnitHandle h)
{
	if (h.index >= (uint32_t)u->cap) return -1;
	int32_t d = u->slot_dense[h.index];
	if (d < 0 || u->slot_gen[h.index] != h.generation) return -1;
	return d;
}

UnitHandle units_handle(const Units *u, ptrdiff_t dense)
{
	ASSERT(dense >= 0 && dense < u->count);
	uint32_t s = (uint32_t)u->dense_slot[dense];
	return (UnitHandle){s, u->slot_gen[s]};
}

// Callers check the population rule first; a full pool here is a bug.
static ptrdiff_t units_spawn(Units *u, UnitType type, float x, float z, float y)
{
	ASSERT(u->count < u->cap && u->free_count > 0);
	int32_t s = u->free_slots[--u->free_count];
	ptrdiff_t d = u->count++;
	u->slot_dense[s] = (int32_t)d;
	u->dense_slot[d] = s;
	u->px[d] = x;
	u->py[d] = y;
	u->pz[d] = z;
	u->vx[d] = 0.0f;
	u->vz[d] = 0.0f;
	u->hp[d] = unit_defs[type].hp;
	u->type[d] = type;
	u->order[d] = ORDER_IDLE;
	u->tx[d] = x;
	u->tz[d] = z;
	u->cooldown[d] = 0;
	u->target[d] = NO_UNIT;
	u->engaged[d] = 0;
	return d;
}

// Swap-remove: the last unit moves into the hole.
static void units_kill(Units *u, ptrdiff_t d)
{
	ASSERT(d >= 0 && d < u->count);
	int32_t dead_slot = u->dense_slot[d];
	ptrdiff_t last = --u->count;
	if (d != last) {
#define MOVE(f) u->f[d] = u->f[last]
		MOVE(dense_slot); MOVE(px); MOVE(py); MOVE(pz); MOVE(vx); MOVE(vz);
		MOVE(hp); MOVE(type); MOVE(order); MOVE(tx); MOVE(tz);
		MOVE(cooldown); MOVE(target); MOVE(engaged);
#undef MOVE
		u->slot_dense[u->dense_slot[d]] = (int32_t)d;
	}
	u->slot_dense[dead_slot] = -1;
	u->slot_gen[dead_slot]++;
	u->free_slots[u->free_count++] = dead_slot;
}

// ---------------------------------------------------------------- grid

static int32_t grid_coord(float v, float cell, int32_t n)
{
	int32_t c = (int32_t)(v / cell);
	return c < 0 ? 0 : c >= n ? n - 1 : c;
}

static void grid_build(Grid *g, Arena *a, const Terrain *t,
                       const float *px, const float *pz, ptrdiff_t n)
{
	g->cell = GRID_CELL;
	g->w = (int32_t)(t->size_x / GRID_CELL) + 1;
	g->h = (int32_t)(t->size_z / GRID_CELL) + 1;
	ptrdiff_t cells = (ptrdiff_t)g->w * g->h;
	g->start = PUSH(a, int32_t, cells + 1);
	g->items = PUSH(a, int32_t, n);
	int32_t *cell_of = PUSH(a, int32_t, n);
	for (ptrdiff_t i = 0; i < n; i++) {
		int32_t c = grid_coord(pz[i], g->cell, g->h) * g->w + grid_coord(px[i], g->cell, g->w);
		cell_of[i] = c;
		g->start[c + 1]++;
	}
	for (ptrdiff_t c = 0; c < cells; c++) g->start[c + 1] += g->start[c];
	int32_t *cursor = arena_copy(a, g->start, cells * (ptrdiff_t)sizeof(int32_t), alignof(int32_t));
	for (ptrdiff_t i = 0; i < n; i++) g->items[cursor[cell_of[i]]++] = (int32_t)i;
}

typedef struct {
	int32_t i0, i1, j0, j1;
} CellRange;

static CellRange grid_range(const Grid *g, float x0, float z0, float x1, float z1)
{
	return (CellRange){
		grid_coord(x0, g->cell, g->w), grid_coord(x1, g->cell, g->w),
		grid_coord(z0, g->cell, g->h), grid_coord(z1, g->cell, g->h),
	};
}

#define GRID_FOR(g, r, k)                                                   \
	for (int32_t gj_ = (r).j0; gj_ <= (r).j1; gj_++)                        \
		for (int32_t gi_ = (r).i0; gi_ <= (r).i1; gi_++)                    \
			for (int32_t gk_ = (g)->start[gj_ * (g)->w + gi_],              \
			             ge_ = (g)->start[gj_ * (g)->w + gi_ + 1];          \
			     gk_ < ge_ && ((k) = (g)->items[gk_], true); gk_++)

// ---------------------------------------------------------------- ballistics

bool ballistic_solve(V3 from, V3 to, float speed, bool lob, V3 *vel, float *time)
{
	V2 flat = v2(to.x - from.x, to.z - from.z);
	float d = v2_len(flat);
	if (d < 1e-3f) return false;
	float h = to.y - from.y;
	float g = SIM_GRAVITY;
	float v2s = speed * speed;
	float disc = v2s * v2s - g * (g * d * d + 2.0f * h * v2s);
	if (disc < 0.0f) return false;
	float root = f_sqrt(disc);
	float tan_t = (lob ? v2s + root : v2s - root) / (g * d);
	float cos_t = 1.0f / f_sqrt(1.0f + tan_t * tan_t);
	float sin_t = tan_t * cos_t;
	V2 dir = v2_scale(flat, 1.0f / d);
	*vel = v3(dir.x * speed * cos_t, speed * sin_t, dir.z * speed * cos_t);
	*time = d / (speed * cos_t);
	return true;
}

// ---------------------------------------------------------------- interaction phase

static void deliver_launches(Sim *sim)
{
	for (int f = 0; f < FACTION_COUNT; f++) {
		Faction *from = &sim->factions[f];
		for (ptrdiff_t k = 0; k < from->outbox_count; k++) {
			const Launch *l = &from->outbox[k];
			Projectiles *p = &sim->factions[l->target.v].incoming;
			if (p->count == p->cap) {
				from->stats.launches_dropped++;
				continue;
			}
			ptrdiff_t i = p->count++;
			p->px[i] = l->pos.x;
			p->py[i] = l->pos.y;
			p->pz[i] = l->pos.z;
			p->vx[i] = l->vel.x;
			p->vy[i] = l->vel.y;
			p->vz[i] = l->vel.z;
			p->weapon[i] = l->weapon;
			p->ttl[i] = PROJECTILE_TTL;
			p->id[i] = p->next_id++;
		}
		from->outbox_count = 0;
	}
}

static void build_view(EnemyView *v, Arena *a, const Faction *enemy, const Terrain *t)
{
	const Units *u = &enemy->units;
	ptrdiff_t n = u->count;
	*v = (EnemyView){.count = n, .slot_cap = u->cap};
	v->px = arena_copy(a, u->px, n * (ptrdiff_t)sizeof(float), alignof(float));
	v->py = arena_copy(a, u->py, n * (ptrdiff_t)sizeof(float), alignof(float));
	v->pz = arena_copy(a, u->pz, n * (ptrdiff_t)sizeof(float), alignof(float));
	v->vx = arena_copy(a, u->vx, n * (ptrdiff_t)sizeof(float), alignof(float));
	v->vz = arena_copy(a, u->vz, n * (ptrdiff_t)sizeof(float), alignof(float));
	v->handle = PUSH(a, UnitHandle, n);
	v->radius = PUSH(a, float, n);
	v->slot_index = PUSH(a, int32_t, u->cap);
	v->slot_gen = arena_copy(a, u->slot_gen, u->cap * (ptrdiff_t)sizeof(uint32_t), alignof(uint32_t));
	for (ptrdiff_t s = 0; s < u->cap; s++) v->slot_index[s] = -1;
	for (ptrdiff_t i = 0; i < n; i++) {
		v->handle[i] = units_handle(u, i);
		v->radius[i] = unit_defs[u->type[i]].radius;
		v->slot_index[u->dense_slot[i]] = (int32_t)i;
	}
	grid_build(&v->grid, a, t, v->px, v->pz, n);
}

static ptrdiff_t view_find(const EnemyView *v, UnitHandle h)
{
	if (h.index >= (uint32_t)v->slot_cap) return -1;
	if (v->slot_gen[h.index] != h.generation) return -1;
	return v->slot_index[h.index];
}

static void interaction_phase(Sim *sim)
{
	arena_reset(&sim->interaction);
	deliver_launches(sim);
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Faction *enemy = &sim->factions[faction_enemy((FactionId){(uint32_t)f}).v];
		build_view(&sim->views[f], &sim->interaction, enemy, &sim->terrain);
	}
}

// ---------------------------------------------------------------- faction phase

static V2 clamp_to_map(const Terrain *t, V2 p, float margin)
{
	return v2(f_clamp(p.x, margin, t->size_x - margin), f_clamp(p.z, margin, t->size_z - margin));
}

static bool finite_f(float x) { return x == x && x - x == 0.0f; }

static void spawn_at(Faction *f, const Terrain *t, UnitType type, V2 at, float radius)
{
	V2 p = v2(at.x + rng_range(&f->rng, -radius, radius), at.z + rng_range(&f->rng, -radius, radius));
	p = clamp_to_map(t, p, 2.0f);
	units_spawn(&f->units, type, p.x, p.z, terrain_height(t, p.x, p.z));
	f->stats.units_spawned++;
}

static void apply_spawn(Faction *f, const Terrain *t, const Command *c)
{
	if (c->unit_type >= UNIT_TYPE_COUNT || c->amount < 0 || c->amount > SPAWN_MAX) return;
	if (!finite_f(c->radius) || c->radius < 0.0f || c->radius > SPAWN_MAX_RADIUS) return;
	V2 at = clamp_to_map(t, c->target, 2.0f);
	// The pool's capacity is the limit, checked before asking it.
	for (int32_t k = 0; k < c->amount && f->units.count < f->units.cap; k++)
		spawn_at(f, t, c->unit_type, at, c->radius);
}

static const OrderKind order_for_command[] = {
	[COMMAND_STOP] = ORDER_IDLE,
	[COMMAND_MOVE] = ORDER_MOVE,
	[COMMAND_ATTACK_MOVE] = ORDER_ATTACK_MOVE,
};

// Orders keep the group's shape: each unit aims at the target plus its offset
// from the group's centre, squeezed so the group doesn't stay spread out.
static void apply_command(Faction *f, const Terrain *t, const Command *c)
{
	if (c->kind >= COMMAND_KIND_COUNT) return;
	if (!finite_f(c->target.x) || !finite_f(c->target.z)) return;
	if (c->kind == COMMAND_SPAWN) {
		apply_spawn(f, t, c);
		return;
	}
	Units *u = &f->units;
	V2 target = clamp_to_map(t, c->target, 1.0f);

	V2 sum = v2(0.0f, 0.0f);
	ptrdiff_t live = 0;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find(u, c->units[k]);
		if (d < 0) continue;
		sum = v2_add(sum, v2(u->px[d], u->pz[d]));
		live++;
	}
	if (live == 0) return;
	V2 centre = v2_scale(sum, 1.0f / (float)live);
	float spread = 0.0f;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find(u, c->units[k]);
		if (d < 0) continue;
		spread = f_max(spread, v2_len(v2_sub(v2(u->px[d], u->pz[d]), centre)));
	}
	float want = 1.8f * f_sqrt((float)live);
	float squeeze = spread > want ? want / spread : 1.0f;

	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find(u, c->units[k]);
		if (d < 0) continue;
		V2 offset = v2_scale(v2_sub(v2(u->px[d], u->pz[d]), centre), squeeze);
		V2 dest = clamp_to_map(t, v2_add(target, offset), 1.0f);
		u->order[d] = order_for_command[c->kind];
		u->tx[d] = dest.x;
		u->tz[d] = dest.z;
	}
}

static UnitType pick_type(Rng *rng, const int32_t weight[UNIT_TYPE_COUNT])
{
	int32_t total = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) total += weight[k];
	int32_t roll = (int32_t)rng_below(rng, (uint32_t)total);
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		roll -= weight[k];
		if (roll < 0) return (UnitType)k;
	}
	return UNIT_RAIDER;
}

static void spawn_near_base(Faction *f, const SimConfig *c, const Terrain *t, float radius)
{
	spawn_at(f, t, pick_type(&f->rng, c->type_weight), f->base, radius);
}

static void spawn(Faction *f, const Sim *sim)
{
	const SimConfig *c = &sim->config;
	if (sim->tick.v % c->spawn_interval != 0) return;
	// The population limit is a game rule, checked before the pool is asked.
	if (f->units.count >= c->population || f->units.count >= f->units.cap) return;
	spawn_near_base(f, c, &sim->terrain, 20.0f);
}

static void move_units(Faction *f, const Terrain *t)
{
	Units *u = &f->units;
	ptrdiff_t n = u->count;
	Grid grid;
	grid_build(&grid, &f->scratch, t, u->px, u->pz, n);
	float *nvx = PUSH(&f->scratch, float, n);
	float *nvz = PUSH(&f->scratch, float, n);
	float *cx = PUSH(&f->scratch, float, n);
	float *cz = PUSH(&f->scratch, float, n);

	// Read positions, write into temporaries: the result doesn't depend on
	// the order units are visited.
	for (ptrdiff_t i = 0; i < n; i++) {
		const UnitDef *def = &unit_defs[u->type[i]];
		V2 p = v2(u->px[i], u->pz[i]);
		V2 desired = v2(0.0f, 0.0f);
		if (u->order[i] != ORDER_IDLE) {
			V2 to = v2_sub(v2(u->tx[i], u->tz[i]), p);
			float d = v2_len(to);
			if (d < ARRIVE_RADIUS) {
				u->order[i] = ORDER_IDLE;
			} else if (!(u->order[i] == ORDER_ATTACK_MOVE && u->engaged[i])) {
				desired = v2_scale(to, def->speed / d);
			}
		}
		V2 v = v2(u->vx[i], u->vz[i]);
		V2 steer = v2_limit(v2_sub(desired, v), def->accel * SIM_DT);
		v = v2_limit(v2_add(v, steer), def->speed);
		nvx[i] = v.x;
		nvz[i] = v.z;

		V2 push = v2(0.0f, 0.0f);
		float reach = def->radius + MAX_UNIT_RADIUS;
		CellRange r = grid_range(&grid, p.x - reach, p.z - reach, p.x + reach, p.z + reach);
		int32_t j;
		GRID_FOR(&grid, r, j) {
			if (j == i) continue;
			V2 away = v2_sub(p, v2(u->px[j], u->pz[j]));
			float min_d = def->radius + unit_defs[u->type[j]].radius;
			float d2 = v2_len2(away);
			if (d2 >= min_d * min_d) continue;
			if (d2 < 1e-8f) {
				// Stacked exactly: split by index so the result is still deterministic.
				push.x += (i < j ? 0.05f : -0.05f);
				continue;
			}
			float d = f_sqrt(d2);
			push = v2_add(push, v2_scale(away, 0.5f * (min_d - d) / d));
		}
		push = v2_limit(push, SEPARATION_MAX);
		cx[i] = push.x;
		cz[i] = push.z;
	}

	for (ptrdiff_t i = 0; i < n; i++) {
		const UnitDef *def = &unit_defs[u->type[i]];
		u->vx[i] = nvx[i];
		u->vz[i] = nvz[i];
		V2 p = v2(u->px[i] + nvx[i] * SIM_DT + cx[i], u->pz[i] + nvz[i] * SIM_DT + cz[i]);
		p = clamp_to_map(t, p, def->radius);
		u->px[i] = p.x;
		u->pz[i] = p.z;
		u->py[i] = terrain_height(t, p.x, p.z);
	}
}

static V3 unit_centre(const Units *u, ptrdiff_t i)
{
	return v3(u->px[i], u->py[i] + unit_defs[u->type[i]].radius, u->pz[i]);
}

static void splash(Units *u, const Grid *grid, V3 at, const UnitDef *weapon, ptrdiff_t except)
{
	if (weapon->splash <= 0.0f) return;
	float reach = weapon->splash + MAX_UNIT_RADIUS;
	CellRange r = grid_range(grid, at.x - reach, at.z - reach, at.x + reach, at.z + reach);
	int32_t j;
	GRID_FOR(grid, r, j) {
		if (j == except) continue;
		float rad = unit_defs[u->type[j]].radius;
		V3 off = v3_sub(unit_centre(u, j), at);
		float d = f_sqrt(v3_dot(off, off));
		float edge = f_max(0.0f, d - rad);
		if (edge >= weapon->splash) continue;
		u->hp[j] -= 0.5f * weapon->damage * (1.0f - edge / weapon->splash);
	}
}

// Shells aimed at this faction, flown against this faction's units only.
static void fly_projectiles(Faction *f, const Terrain *t)
{
	Units *u = &f->units;
	Projectiles *p = &f->incoming;
	Grid grid;
	grid_build(&grid, &f->scratch, t, u->px, u->pz, u->count);

	// Descending, so swap-remove only moves shells already flown this tick.
	for (ptrdiff_t k = p->count - 1; k >= 0; k--) {
		const UnitDef *weapon = &unit_defs[p->weapon[k]];
		V3 p0 = v3(p->px[k], p->py[k], p->pz[k]);
		V3 vel = v3(p->vx[k], p->vy[k], p->vz[k]);
		V3 p1 = p0;
		shell_advance(&p1, &vel);
		V3 seg = v3_sub(p1, p0);
		float seg2 = v3_dot(seg, seg);

		ptrdiff_t hit = -1;
		float hit_t = 2.0f;
		float reach = MAX_UNIT_RADIUS;
		CellRange r = grid_range(&grid, f_min(p0.x, p1.x) - reach, f_min(p0.z, p1.z) - reach,
		                         f_max(p0.x, p1.x) + reach, f_max(p0.z, p1.z) + reach);
		int32_t j;
		GRID_FOR(&grid, r, j) {
			float rad = unit_defs[u->type[j]].radius;
			V3 c = unit_centre(u, j);
			float s = seg2 > 0.0f ? f_clamp(v3_dot(v3_sub(c, p0), seg) / seg2, 0.0f, 1.0f) : 0.0f;
			V3 off = v3_sub(c, v3_add(p0, v3_scale(seg, s)));
			if (v3_dot(off, off) < rad * rad && s < hit_t) {
				hit = j;
				hit_t = s;
			}
		}

		bool gone = false;
		if (hit >= 0) {
			u->hp[hit] -= weapon->damage;
			splash(u, &grid, v3_add(p0, v3_scale(seg, hit_t)), weapon, hit);
			gone = true;
		} else if (p1.y <= terrain_height(t, p1.x, p1.z)) {
			splash(u, &grid, p1, weapon, -1);
			gone = true;
		} else if (--p->ttl[k] <= 0) {
			gone = true;
		}

		if (gone) {
			ptrdiff_t last = --p->count;
			p->px[k] = p->px[last];
			p->py[k] = p->py[last];
			p->pz[k] = p->pz[last];
			p->vx[k] = p->vx[last];
			p->vy[k] = p->vy[last];
			p->vz[k] = p->vz[last];
			p->weapon[k] = p->weapon[last];
			p->ttl[k] = p->ttl[last];
			p->id[k] = p->id[last];
		} else {
			p->px[k] = p1.x;
			p->py[k] = p1.y;
			p->pz[k] = p1.z;
			p->vx[k] = vel.x;
			p->vy[k] = vel.y;
			p->vz[k] = vel.z;
		}
	}
}

static ptrdiff_t nearest_enemy(const EnemyView *v, V2 from, float range)
{
	CellRange r = grid_range(&v->grid, from.x - range, from.z - range, from.x + range, from.z + range);
	ptrdiff_t best = -1;
	float best_d2 = range * range;
	int32_t j;
	GRID_FOR(&v->grid, r, j) {
		float d2 = v2_len2(v2_sub(v2(v->px[j], v->pz[j]), from));
		if (d2 < best_d2 || (d2 == best_d2 && best >= 0 && j < best)) {
			best = j;
			best_d2 = d2;
		}
	}
	return best;
}

// Aim with lead: solve, predict where the target will be after the flight
// time, then solve again for that point.
static bool aim(const Faction *f, Rng *rng, const Terrain *t, const EnemyView *v,
                ptrdiff_t i, ptrdiff_t e, V3 *muzzle, V3 *vel)
{
	const Units *u = &f->units;
	const UnitDef *def = &unit_defs[u->type[i]];
	*muzzle = v3(u->px[i], u->py[i] + def->height, u->pz[i]);
	V3 target = v3(v->px[e], v->py[e] + v->radius[e], v->pz[e]);
	float time;
	if (!ballistic_solve(*muzzle, target, def->shell_speed, def->high_arc, vel, &time))
		return false;
	float lx = v->px[e] + v->vx[e] * time + rng_range(rng, -def->spread, def->spread);
	float lz = v->pz[e] + v->vz[e] * time + rng_range(rng, -def->spread, def->spread);
	V3 lead = v3(lx, terrain_height(t, lx, lz) + v->radius[e], lz);
	return ballistic_solve(*muzzle, lead, def->shell_speed, def->high_arc, vel, &time);
}

static void fire_weapons(Faction *f, const EnemyView *v, const Terrain *t, Tick tick)
{
	Units *u = &f->units;
	FactionId enemy = faction_enemy(f->id);
	for (ptrdiff_t i = 0; i < u->count; i++) {
		const UnitDef *def = &unit_defs[u->type[i]];
		if (u->cooldown[i] > 0) u->cooldown[i]--;
		u->engaged[i] = 0;

		V2 p = v2(u->px[i], u->pz[i]);
		ptrdiff_t e = view_find(v, u->target[i]);
		if (e >= 0 && v2_len2(v2_sub(v2(v->px[e], v->pz[e]), p)) > def->range * def->range) e = -1;
		// Retargeting is staggered by slot so the cost spreads across ticks.
		if (e < 0 && (tick.v + u->dense_slot[i]) % RETARGET_PERIOD == 0) {
			e = nearest_enemy(v, p, def->range);
			u->target[i] = e >= 0 ? v->handle[e] : NO_UNIT;
		}
		if (e < 0) continue;
		u->engaged[i] = 1;
		if (u->cooldown[i] > 0) continue;

		V3 muzzle, vel;
		if (!aim(f, &f->rng, t, v, i, e, &muzzle, &vel)) continue;
		ASSERT(f->outbox_count < f->outbox_cap); // each unit fires at most once a tick
		f->outbox[f->outbox_count++] = (Launch){muzzle, vel, u->type[i], enemy};
		u->cooldown[i] = def->reload;
		f->stats.shots_fired++;
	}
}

static void remove_dead(Faction *f)
{
	Units *u = &f->units;
	for (ptrdiff_t i = u->count - 1; i >= 0; i--) {
		if (u->hp[i] > 0.0f) continue;
		units_kill(u, i);
		f->stats.units_lost++;
	}
}

static void faction_phase(Sim *sim, Faction *f, const Command *commands, ptrdiff_t count)
{
	arena_reset(&f->scratch);
	for (ptrdiff_t k = 0; k < count; k++)
		if (commands[k].faction.v == f->id.v) apply_command(f, &sim->terrain, &commands[k]);
	spawn(f, sim);
	move_units(f, &sim->terrain);
	fly_projectiles(f, &sim->terrain);
	fire_weapons(f, &sim->views[f->id.v], &sim->terrain, sim->tick);
	remove_dead(f);
}

// ---------------------------------------------------------------- lifecycle

void sim_init(Sim *sim, SimConfig config)
{
	ASSERT(config.max_units > 0 && config.max_projectiles > 0);
	ASSERT(config.population <= config.max_units && config.initial_units <= config.population);
	ASSERT(config.spawn_interval > 0);
	int32_t weights = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		ASSERT(config.type_weight[k] >= 0 && config.type_weight[k] <= 1000);
		weights += config.type_weight[k];
	}
	ASSERT(weights > 0);
	*sim = (Sim){.config = config};
	sim->zone = arena_create("zone", (ptrdiff_t)256 << 20);
	sim->interaction = arena_create("interaction", (ptrdiff_t)64 << 20);
	terrain_generate(&sim->terrain, &sim->zone, config.seed);

	static const char *scratch_names[FACTION_COUNT] = {"faction 0 scratch", "faction 1 scratch"};
	const Terrain *t = &sim->terrain;
	V2 bases[FACTION_COUNT] = {v2(64.0f, t->size_z * 0.5f), v2(t->size_x - 64.0f, t->size_z * 0.5f)};
	for (int k = 0; k < FACTION_COUNT; k++) {
		Faction *f = &sim->factions[k];
		f->id = (FactionId){(uint32_t)k};
		f->base = bases[k];
		f->rng = rng_seed(config.seed * 2654435761u + (uint64_t)k);
		units_init(&f->units, &sim->zone, config.max_units);
		Projectiles *p = &f->incoming;
		p->cap = config.max_projectiles;
		p->px = PUSH(&sim->zone, float, p->cap);
		p->py = PUSH(&sim->zone, float, p->cap);
		p->pz = PUSH(&sim->zone, float, p->cap);
		p->vx = PUSH(&sim->zone, float, p->cap);
		p->vy = PUSH(&sim->zone, float, p->cap);
		p->vz = PUSH(&sim->zone, float, p->cap);
		p->weapon = PUSH(&sim->zone, UnitType, p->cap);
		p->ttl = PUSH(&sim->zone, int32_t, p->cap);
		p->id = PUSH(&sim->zone, uint32_t, p->cap);
		f->outbox_cap = config.max_units;
		f->outbox = PUSH(&sim->zone, Launch, f->outbox_cap);
		f->scratch = arena_create(scratch_names[k], (ptrdiff_t)64 << 20);

		float spread = 1.6f * f_sqrt((float)config.initial_units);
		for (int32_t n = 0; n < config.initial_units; n++) spawn_near_base(f, &config, t, spread);
	}
}

void sim_destroy(Sim *sim)
{
	for (int k = 0; k < FACTION_COUNT; k++) arena_destroy(&sim->factions[k].scratch);
	arena_destroy(&sim->interaction);
	arena_destroy(&sim->zone);
}

void sim_step(Sim *sim, const Command *commands, ptrdiff_t count)
{
	interaction_phase(sim);
	for (int k = 0; k < FACTION_COUNT; k++)
		faction_phase(sim, &sim->factions[k], commands, count);
	sim->tick.v++;
}

// ---------------------------------------------------------------- hashing

static Hash hash_units(Hash h, const Units *u)
{
	h = hash_u64(h, (uint64_t)u->count);
	h = hash_u64(h, (uint64_t)u->free_count);
	for (ptrdiff_t s = 0; s < u->free_count; s++) h = hash_u32(h, (uint32_t)u->free_slots[s]);
	for (ptrdiff_t s = 0; s < u->cap; s++) h = hash_u32(h, u->slot_gen[s]);
	for (ptrdiff_t i = 0; i < u->count; i++) {
		h = hash_u32(h, (uint32_t)u->dense_slot[i]);
		h = hash_f32(h, u->px[i]);
		h = hash_f32(h, u->py[i]);
		h = hash_f32(h, u->pz[i]);
		h = hash_f32(h, u->vx[i]);
		h = hash_f32(h, u->vz[i]);
		h = hash_f32(h, u->hp[i]);
		h = hash_u32(h, u->type[i]);
		h = hash_u32(h, u->order[i]);
		h = hash_f32(h, u->tx[i]);
		h = hash_f32(h, u->tz[i]);
		h = hash_u32(h, (uint32_t)u->cooldown[i]);
		h = hash_u32(h, u->target[i].index);
		h = hash_u32(h, u->target[i].generation);
		h = hash_u32(h, u->engaged[i]);
	}
	return h;
}

static Hash hash_projectiles(Hash h, const Projectiles *p)
{
	h = hash_u64(h, (uint64_t)p->count);
	h = hash_u32(h, p->next_id);
	for (ptrdiff_t i = 0; i < p->count; i++) {
		h = hash_f32(h, p->px[i]);
		h = hash_f32(h, p->py[i]);
		h = hash_f32(h, p->pz[i]);
		h = hash_f32(h, p->vx[i]);
		h = hash_f32(h, p->vy[i]);
		h = hash_f32(h, p->vz[i]);
		h = hash_u32(h, p->weapon[i]);
		h = hash_u32(h, (uint32_t)p->ttl[i]);
		h = hash_u32(h, p->id[i]);
	}
	return h;
}

Hash sim_hash_now(const Sim *sim)
{
	Hash h = hash_begin();
	h = hash_u64(h, (uint64_t)sim->tick.v);
	for (int k = 0; k < FACTION_COUNT; k++) {
		const Faction *f = &sim->factions[k];
		h = hash_u64(h, f->rng.state);
		h = hash_units(h, &f->units);
		h = hash_projectiles(h, &f->incoming);
		h = hash_u64(h, (uint64_t)f->outbox_count);
		for (ptrdiff_t i = 0; i < f->outbox_count; i++) {
			const Launch *l = &f->outbox[i];
			h = hash_f32(h, l->pos.x);
			h = hash_f32(h, l->pos.y);
			h = hash_f32(h, l->pos.z);
			h = hash_f32(h, l->vel.x);
			h = hash_f32(h, l->vel.y);
			h = hash_f32(h, l->vel.z);
			h = hash_u32(h, l->weapon);
			h = hash_u32(h, l->target.v);
		}
		h = hash_u64(h, (uint64_t)f->stats.shots_fired);
		h = hash_u64(h, (uint64_t)f->stats.units_spawned);
		h = hash_u64(h, (uint64_t)f->stats.units_lost);
		h = hash_u64(h, (uint64_t)f->stats.launches_dropped);
	}
	return h;
}
