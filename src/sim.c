#include "sim.h"

const UnitDef unit_defs[UNIT_TYPE_COUNT] = {
	[UNIT_RAIDER] = {
		.name = "raider", .speed = 9.0f, .accel = 30.0f, .radius = 0.8f, .sight = 48.0f,
		.height = 1.2f, .hp = 60.0f, .range = 24.0f, .reload = 15,
		.shell_speed = 45.0f, .damage = 12.0f, .splash = 0.0f,
		.spread = 0.6f, .high_arc = false, .weight = 6,
		.metal_cost = 50.0f, .energy_cost = 300.0f, .build_time = 60.0f,
	},
	[UNIT_TANK] = {
		.name = "tank", .speed = 5.0f, .accel = 12.0f, .radius = 1.4f, .sight = 32.0f,
		.height = 1.8f, .hp = 240.0f, .range = 32.0f, .reload = 36,
		.shell_speed = 42.0f, .damage = 48.0f, .splash = 1.5f,
		.spread = 0.8f, .high_arc = false, .weight = 3,
		.metal_cost = 150.0f, .energy_cost = 700.0f, .build_time = 150.0f,
	},
	[UNIT_ARTILLERY] = {
		// Outranges its own sight: artillery needs spotters.
		.name = "artillery", .speed = 3.5f, .accel = 8.0f, .radius = 1.1f, .sight = 26.0f,
		.height = 1.4f, .hp = 90.0f, .range = 78.0f, .reload = 90,
		.shell_speed = 40.0f, .damage = 60.0f, .splash = 4.0f,
		.spread = 2.0f, .high_arc = true, .weight = 2,
		.metal_cost = 120.0f, .energy_cost = 900.0f, .build_time = 140.0f,
	},
	// Outranges everything, slowly: for lines that have dug in. Close up it
	// can't fire at all, so it needs an army in front of it.
	[UNIT_SIEGE] = {
		.name = "siege", .speed = 2.5f, .accel = 5.0f, .radius = 1.3f, .sight = 30.0f,
		.height = 1.6f, .hp = 150.0f, .range = 140.0f, .min_range = 35.0f, .reload = 180,
		.shell_speed = 55.0f, .damage = 140.0f, .splash = 6.0f,
		.spread = 3.0f, .high_arc = true, .weight = 0,
		.metal_cost = 260.0f, .energy_cost = 2200.0f, .build_time = 260.0f,
	},
	[UNIT_ENGINEER] = {
		.name = "engineer", .speed = 6.0f, .accel = 20.0f, .radius = 0.9f, .sight = 30.0f,
		.height = 1.0f, .hp = 120.0f, .build_power = 10.0f,
		.metal_cost = 60.0f, .energy_cost = 400.0f, .build_time = 70.0f,
	},
	// A combat engineer: builds at twice an engineer's pace, takes a beating,
	// shoots back a little. Small enough to go unpaired: a beachhead's crew.
	[UNIT_SAPPER] = {
		.name = "sapper", .speed = 5.5f, .accel = 16.0f, .radius = 1.0f, .sight = 32.0f,
		.height = 1.2f, .hp = 320.0f, .range = 22.0f, .reload = 30,
		.shell_speed = 60.0f, .damage = 8.0f, .spread = 0.6f, .build_power = 20.0f,
		.metal_cost = 140.0f, .energy_cost = 1000.0f, .build_time = 120.0f,
	},
	// Eyes: fast, far-sighted, fragile, unarmed. Cheap enough to lose.
	[UNIT_SCOUT] = {
		.name = "scout", .speed = 14.0f, .accel = 40.0f, .radius = 0.7f, .sight = 70.0f,
		.height = 0.8f, .hp = 40.0f,
		.metal_cost = 20.0f, .energy_cost = 150.0f, .build_time = 20.0f,
	},
	[UNIT_EXTRACTOR] = {
		.name = "extractor", .radius = 1.6f, .sight = 20.0f, .height = 1.6f, .hp = 300.0f,
		.metal_cost = 50.0f, .energy_cost = 500.0f, .build_time = 60.0f, .metal_income = 3.0f,
	},
	[UNIT_GENERATOR] = {
		.name = "generator", .radius = 1.8f, .sight = 16.0f, .height = 3.0f, .hp = 200.0f,
		.metal_cost = 40.0f, .energy_cost = 0.0f, .build_time = 50.0f, .energy_income = 40.0f,
	},
	[UNIT_FACTORY] = {
		.name = "factory", .radius = 4.0f, .sight = 30.0f, .height = 3.5f, .hp = 1500.0f,
		.build_power = 15.0f, .metal_cost = 300.0f, .energy_cost = 1500.0f, .build_time = 300.0f,
	},
	[UNIT_TRANSPORTER] = {
		.name = "transporter", .radius = 5.0f, .sight = 30.0f, .height = 6.0f, .hp = 2500.0f,
		.metal_cost = 600.0f, .energy_cost = 6000.0f, .build_time = 600.0f,
	},
	// Outranges the units it's made from, and is sturdier: a held line.
	[UNIT_TURRET] = {
		.name = "turret", .radius = 1.6f, .sight = 34.0f, .height = 2.2f, .hp = 360.0f,
		.range = 38.0f, .reload = 24, .shell_speed = 46.0f, .damage = 30.0f, .splash = 1.0f, .spread = 0.6f,
		.metal_cost = 120.0f, .energy_cost = 900.0f, .build_time = 120.0f,
	},
};

static constexpr float KIT_SPACING = 30.0f;        // between players' starting bases
static constexpr float MAX_SIGHT = 70.0f;          // the furthest any unit sees (scouts): checked in sim_init
static constexpr float MAX_RADIUS = 5.0f;           // transporters; separation reach

// The economy's constants: what a faction starts with and earns for free
// (its headquarters), and how close a builder must be.
static constexpr float START_METAL = 600.0f;
static constexpr float START_ENERGY = 3000.0f;
static constexpr float METAL_CAP = 2000.0f;
static constexpr float ENERGY_CAP = 10000.0f;
static constexpr float BASE_METAL_INCOME = 1.0f;
static constexpr float BASE_ENERGY_INCOME = 30.0f;
static constexpr float BUILD_RANGE = 6.0f;          // beyond the structure's radius
static constexpr float GRID_CELL = 8.0f;
static constexpr int32_t RETARGET_PERIOD = 8;
static constexpr float ARRIVE_RADIUS = 3.0f;
static constexpr float SEPARATION_MAX = 0.5f;   // per-tick push, units
static constexpr uint32_t NO_SLOT = UINT32_MAX;

static constexpr UnitHandle NO_UNIT = {NO_SLOT, 0};

SimConfig sim_config_default(void)
{
	return (SimConfig){
		.seed = 1,
		.bases = (1 << FACTION_COUNT) - 1,
		.initial_units = 5000,
		.population = 5000,
		.spawn_interval = 1,
		.max_units = 8192,
		.max_projectiles = 65536,
		.players = {1, 1},
		.type_weight = {
			unit_defs[UNIT_RAIDER].weight,
			unit_defs[UNIT_TANK].weight,
			unit_defs[UNIT_ARTILLERY].weight,
		},
	};
}

// ---------------------------------------------------------------- metal spots

int32_t spot_at(const Spots *spots, V2 at)
{
	for (int32_t k = 0; k < spots->count; k++)
		if (v2_len2(v2_sub(spots->at[k], at)) <= SPOT_RADIUS * SPOT_RADIUS) return k;
	return -1;
}

// Mirrored pairs, so neither side's half is richer: two close to each base,
// the rest scattered and spaced apart. Deterministic from the seed.
static void spots_generate(Spots *s, const Terrain *t, uint64_t seed, const V2 base[FACTION_COUNT])
{
	*s = (Spots){0};
	Rng rng = rng_seed(seed ^ 0x5b075u);
	float mid = t->size_x * 0.5f;
	V2 near[2] = {v2(base[0].x + 28.0f, base[0].z - 30.0f), v2(base[0].x + 28.0f, base[0].z + 30.0f)};
	for (int k = 0; k < 2; k++) {
		s->at[s->count++] = near[k];
		s->at[s->count++] = v2(t->size_x - near[k].x, near[k].z);
	}
	for (int tries = 0; tries < 4000 && s->count + 2 <= MAX_SPOTS - 8; tries++) {
		V2 p = v2(rng_range(&rng, 40.0f, mid - 20.0f), rng_range(&rng, 40.0f, t->size_z - 40.0f));
		bool apart = true;
		for (int32_t k = 0; k < s->count && apart; k++) apart = v2_len2(v2_sub(s->at[k], p)) > 40.0f * 40.0f;
		if (!apart) continue;
		s->at[s->count++] = p;
		s->at[s->count++] = v2(t->size_x - p.x, p.z);
	}
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
	u->progress = PUSH(a, float, cap);
	u->build_type = PUSH(a, UnitType, cap);
	u->building = PUSH(a, UnitHandle, cap);
	u->work = PUSH(a, float, cap);
	u->queue = PUSH(a, uint64_t, cap);
	u->queue_len = PUSH(a, uint8_t, cap);
	u->repeat = PUSH(a, uint8_t, cap);
	u->dest = PUSH(a, uint8_t, cap);
	u->route = PUSH(a, uint32_t, cap);
	u->seeded = PUSH(a, uint8_t, cap);
	u->owner = PUSH(a, PlayerSlot, cap);
	u->output = PUSH(a, PlayerSlot, cap);
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

// A unit `player` may order: live, and theirs. -1 otherwise.
static ptrdiff_t units_find_own(const Units *u, UnitHandle h, PlayerSlot player)
{
	ptrdiff_t d = units_find(u, h);
	return d >= 0 && player_eq(u->owner[d], player) ? d : -1;
}

// Callers check the population rule first; a full pool here is a bug.
static ptrdiff_t units_spawn(Units *u, UnitType type, PlayerSlot owner, float x, float z, float y)
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
	u->progress[d] = 1.0f;
	u->build_type[d] = UNIT_EXTRACTOR;
	u->building[d] = NO_UNIT;
	u->work[d] = 0.0f;
	u->queue[d] = 0;
	u->queue_len[d] = 0;
	u->repeat[d] = 0;
	u->dest[d] = 0;
	u->route[d] = 0;
	u->seeded[d] = 0;
	u->owner[d] = owner;
	u->output[d] = owner;
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
		MOVE(progress); MOVE(build_type); MOVE(building); MOVE(work);
		MOVE(queue); MOVE(queue_len); MOVE(repeat); MOVE(dest); MOVE(route); MOVE(seeded);
		MOVE(owner); MOVE(output);
#undef MOVE
		u->slot_dense[u->dense_slot[d]] = (int32_t)d;
	}
	u->slot_dense[dead_slot] = -1;
	u->slot_gen[dead_slot]++;
	u->free_slots[u->free_count++] = dead_slot;
}

// ---------------------------------------------------------------- grid

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

static void perception_init(Perception *p, Arena *a, ptrdiff_t cap)
{
	*p = (Perception){.cap = cap};
	p->live = PUSH(a, uint8_t, cap);
	p->gen = PUSH(a, uint32_t, cap);
	p->type = PUSH(a, UnitType, cap);
	p->qx = PUSH(a, uint16_t, cap);
	p->qz = PUSH(a, uint16_t, cap);
	p->qvx = PUSH(a, int16_t, cap);
	p->qvz = PUSH(a, int16_t, cap);
	p->sent = PUSH(a, int64_t, cap);
}


// Is (x, z) within sight of any of these units? `grid` covers them.
static bool in_sight(const Units *own, const Grid *grid, float x, float z)
{
	CellRange r = grid_range(grid, x - MAX_SIGHT, z - MAX_SIGHT, x + MAX_SIGHT, z + MAX_SIGHT);
	int32_t j;
	GRID_FOR(grid, r, j) {
		float sight = unit_defs[own->type[j]].sight;
		if (v2_len2(v2_sub(v2(own->px[j], own->pz[j]), v2(x, z))) < sight * sight) return true;
	}
	return false;
}

// What faction `f` perceives of its enemy this tick, as entries against its
// current perception: units entering sight, drifting from their dead
// reckoning, or leaving sight. Reads both factions; writes neither.
static void sense(Sim *sim, FactionId f, FactionEffects *fx)
{
	const Faction *me = &sim->factions[f.v];
	const Units *enemy = &sim->factions[faction_enemy(f).v].units;
	const Perception *p = &me->seen;
	Grid grid;
	grid_build(&grid, &sim->interaction, &sim->terrain, me->units.px, me->units.pz, me->units.count);
	fx->seen = PUSH(&sim->interaction, SeenEntry, enemy->cap);
	fx->seen_count = 0;
	for (ptrdiff_t s = 0; s < enemy->cap; s++) {
		int32_t d = enemy->slot_dense[s];
		bool visible = d >= 0 && in_sight(&me->units, &grid, enemy->px[d], enemy->pz[d]);
		if (!visible) {
			if (p->live[s]) fx->seen[fx->seen_count++] = (SeenEntry){.tag = SEEN_LOST, .slot = (uint16_t)s};
			continue;
		}
		SeenEntry e = {
			.slot = (uint16_t)s,
			.gen = enemy->slot_gen[s],
			.type = enemy->type[d],
			.qx = q_pos(enemy->px[d]),
			.qz = q_pos(enemy->pz[d]),
			.qvx = q_vel(enemy->vx[d]),
			.qvz = q_vel(enemy->vz[d]),
		};
		if (!p->live[s] || p->gen[s] != e.gen) {
			e.tag = SEEN_ENTER;
			fx->seen[fx->seen_count++] = e;
			continue;
		}
		V2 guess = perception_at(p, s, sim->tick);
		float dx = guess.x - enemy->px[d], dz = guess.z - enemy->pz[d];
		bool drifted = dx * dx + dz * dz > RESEND_DISTANCE * RESEND_DISTANCE;
		bool turned = abs(e.qvx - p->qvx[s]) > RESEND_VELOCITY || abs(e.qvz - p->qvz[s]) > RESEND_VELOCITY;
		bool stale = sim->tick.v - p->sent[s] > RESEND_AGE;
		if (drifted || turned || stale) {
			e.tag = SEEN_MOVE;
			fx->seen[fx->seen_count++] = e;
		}
	}
}

// A target handle's index in the view, or -1 if it isn't perceived (any more).
static ptrdiff_t view_find(const EnemyView *v, UnitHandle h)
{
	if (h.index >= (uint32_t)v->slot_cap) return -1;
	int32_t i = v->slot_index[h.index];
	if (i < 0 || v->slot_gen[h.index] != h.generation) return -1;
	return i;
}

// Each metal spot belongs to whichever faction has a finished extractor on
// it. A holder keeps it while its extractor stands; a spot both claim at
// once, with no holder, belongs to nobody.
static void contest_spots(Sim *sim)
{
	bool claim[FACTION_COUNT][MAX_SPOTS] = {0};
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Units *u = &sim->factions[f].units;
		for (ptrdiff_t i = 0; i < u->count; i++) {
			if (u->type[i] != UNIT_EXTRACTOR || u->progress[i] < 1.0f) continue;
			int32_t k = spot_at(&sim->spots, v2(u->px[i], u->pz[i]));
			if (k >= 0) claim[f][k] = true;
		}
	}
	uint64_t owned[FACTION_COUNT] = {0};
	for (int32_t k = 0; k < sim->spots.count; k++) {
		int8_t o = sim->spot_owner[k];
		if (o < 0 || !claim[o][k]) o = claim[0][k] && !claim[1][k] ? 0 : claim[1][k] && !claim[0][k] ? 1 : -1;
		sim->spot_owner[k] = o;
		if (o >= 0) owned[o] |= (uint64_t)1 << k;
	}
	for (int f = 0; f < FACTION_COUNT; f++) sim->effects[f].spots = owned[f];
}

static void interaction_phase(Sim *sim)
{
	arena_reset(&sim->interaction);
	contest_spots(sim);
	for (int t = 0; t < FACTION_COUNT; t++) {
		// Shells fired at t last tick arrive now, in firing order.
		const Faction *from = &sim->factions[faction_enemy((FactionId){(uint32_t)t}).v];
		FactionEffects *fx = &sim->effects[t];
		fx->arrived_count = from->outbox_count;
		fx->arrived = arena_copy(&sim->interaction, from->outbox,
		                         from->outbox_count * (ptrdiff_t)sizeof(Launch), alignof(Launch));
		sense(sim, (FactionId){(uint32_t)t}, fx);
	}
}

// ---------------------------------------------------------------- faction phase

static V2 clamp_to_map(const Terrain *t, V2 p, float margin)
{
	return v2(f_clamp(p.x, margin, t->size_x - margin), f_clamp(p.z, margin, t->size_z - margin));
}

static bool finite_f(float x) { return x == x && x - x == 0.0f; }

static void spawn_at(Faction *f, const Terrain *t, UnitType type, PlayerSlot owner, V2 at, float radius)
{
	V2 p = v2(at.x + rng_range(&f->rng, -radius, radius), at.z + rng_range(&f->rng, -radius, radius));
	p = clamp_to_map(t, p, 2.0f);
	units_spawn(&f->units, type, owner, p.x, p.z, terrain_height(t, p.x, p.z));
	f->stats.units_spawned++;
}

static void apply_spawn(Faction *f, const Terrain *t, const Command *c)
{
	if (c->unit_type >= UNIT_TYPE_COUNT || c->amount < 0 || c->amount > SPAWN_MAX) return;
	if (!finite_f(c->radius) || c->radius < 0.0f || c->radius > SPAWN_MAX_RADIUS) return;
	V2 at = clamp_to_map(t, c->target, 2.0f);
	// The pool's capacity is the limit, checked before asking it.
	for (int32_t k = 0; k < c->amount && f->units.count < f->units.cap; k++)
		spawn_at(f, t, c->unit_type, c->player, at, c->radius);
}

static const OrderKind order_for_command[] = {
	[COMMAND_STOP] = ORDER_IDLE,
	[COMMAND_MOVE] = ORDER_MOVE,
	[COMMAND_ATTACK_MOVE] = ORDER_ATTACK_MOVE,
	[COMMAND_BUILD] = ORDER_BUILD,
	[COMMAND_SEND] = ORDER_SEND,
};

static UnitType queue_front(const Units *u, ptrdiff_t d) { return (UnitType)(u->queue[d] & 0xff); }

static void queue_push(Units *u, ptrdiff_t d, UnitType t)
{
	if (u->queue_len[d] == FACTORY_QUEUE) return;
	u->queue[d] |= (uint64_t)t << (8 * u->queue_len[d]);
	u->queue_len[d]++;
}

static void queue_pop(Units *u, ptrdiff_t d)
{
	u->queue[d] >>= 8;
	u->queue_len[d]--;
}

// Factories queue units; anything else ignores a produce command.
static void apply_produce(Faction *f, const Command *c)
{
	if (c->unit_type >= UNIT_EXTRACTOR || c->amount < 0 || c->amount > FACTORY_QUEUE) return;
	Units *u = &f->units;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0 || u->type[d] != UNIT_FACTORY || u->progress[d] < 1.0f) continue;
		if (c->amount == 0) {
			u->queue[d] = 0;
			u->queue_len[d] = 0;
			u->work[d] = 0.0f;
		}
		for (int32_t n = 0; n < c->amount; n++) queue_push(u, d, c->unit_type);
		u->repeat[d] = (c->flags & COMMAND_REPEAT) != 0;
	}
}

// The faction's finished transporter nearest `at`, within `within`, or -1.
static ptrdiff_t nearest_transporter(const Faction *f, V2 at, float within)
{
	const Units *u = &f->units;
	ptrdiff_t best = -1;
	float best_d2 = within * within;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_TRANSPORTER || u->progress[i] < 1.0f) continue;
		float d2 = v2_len2(v2_sub(v2(u->px[i], u->pz[i]), at));
		if (d2 < best_d2) {
			best = i;
			best_d2 = d2;
		}
	}
	return best;
}

// A finished transform: a combat unit becomes a turret where it stands, or
// an entrenched turret becomes what it was. In place, so handles survive and
// selections still work. Health keeps its proportion.
static void finish_transform(Units *u, ptrdiff_t i)
{
	UnitType from = u->type[i];
	bool dig_in = unit_is_combat(from);
	UnitType to = dig_in ? UNIT_TURRET : u->build_type[i];
	u->hp[i] *= unit_defs[to].hp / unit_defs[from].hp;
	u->type[i] = to;
	u->build_type[i] = dig_in ? from : UNIT_EXTRACTOR;
	u->seeded[i] = dig_in;
	u->order[i] = ORDER_IDLE;
	u->work[i] = 0.0f;
	u->vx[i] = u->vz[i] = 0.0f;
	u->cooldown[i] = 0;
	u->tx[i] = u->px[i];
	u->tz[i] = u->pz[i];
}

// Entrench: these combat units start digging in. Mobilize: these entrenched
// turrets start packing up. Either way the economy pays as it goes.
static void apply_transform(Faction *f, const Command *c)
{
	Units *u = &f->units;
	bool dig_in = c->kind == COMMAND_ENTRENCH;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0 || u->order[d] == ORDER_TRANSFORM) continue;
		bool can = dig_in ? unit_is_combat(u->type[d]) : u->type[d] == UNIT_TURRET && u->seeded[d];
		if (!can) continue;
		u->order[d] = ORDER_TRANSFORM;
		u->work[d] = 0.0f;
		u->building[d] = NO_UNIT;
		u->tx[d] = u->px[d];
		u->tz[d] = u->pz[d];
	}
}

V2 sim_strip_point(const Terrain *t, FactionId f, V2 at)
{
	float w = t->size_x * STRIP;
	float lo = f.v == 0 ? 0.0f : t->size_x - w, hi = f.v == 0 ? w : t->size_x;
	at = clamp_to_map(t, at, 8.0f);
	at.x = f_clamp(at.x, lo + 8.0f, hi - 8.0f);
	return at;
}

// A send: the units go to our transporter nearest them and wait there for
// it to charge and fire. Unpaired, only raiders and engineers go, and the
// transporter remembers where they're to land (its tx, tz: structures never
// move, so they're free).
static void apply_send(Faction *f, const Sim *sim, const Command *c)
{
	Units *u = &f->units;
	bool unpaired = c->flags & COMMAND_UNPAIRED;
	if (c->amount < 0 || c->amount >= ZONE_MAX || !route_ok(c->route) || (unpaired && c->route) || !player_ok(c->to))
		return;
	V2 sum = v2(0.0f, 0.0f);
	ptrdiff_t live = 0;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0 || unit_is_structure(u->type[d])) continue;
		sum = v2_add(sum, v2(u->px[d], u->pz[d]));
		live++;
	}
	if (live == 0) return;
	// The player's nearest transporter with nobody waiting at it, or failing
	// that the nearest: with several, sends go out side by side. Only a
	// transporter's owner sends through it.
	V2 centre = v2_scale(sum, 1.0f / (float)live);
	ptrdiff_t g = -1;
	float best = 1e30f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_TRANSPORTER || u->progress[i] < 1.0f || !player_eq(u->owner[i], c->player)) continue;
		UnitHandle h = units_handle(u, i);
		bool busy = false;
		for (ptrdiff_t j = 0; j < u->count && !busy; j++) busy = u->order[j] == ORDER_SEND && unit_handle_eq(u->building[j], h);
		float d2 = v2_len2(v2_sub(v2(u->px[i], u->pz[i]), centre)) + (busy ? 1e8f : 0.0f);
		if (d2 < best) {
			best = d2;
			g = i;
		}
	}
	if (g < 0) return;
	if (unpaired) {
		V2 land = clamp_to_map(&sim->terrain, c->target, 8.0f);
		u->tx[g] = land.x;
		u->tz[g] = land.z;
	}
	UnitHandle gh = units_handle(u, g);
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0 || unit_is_structure(u->type[d]) || (unpaired && !unpaired_can_send(u->type[d]))) continue;
		u->order[d] = ORDER_SEND;
		u->output[d] = c->to;
		u->tx[d] = u->px[g];
		u->tz[d] = u->pz[g];
		u->building[d] = gh;
		u->dest[d] = (uint8_t)c->amount;
		u->route[d] = c->route | (unpaired ? ROUTE_UNPAIRED : 0u);
	}
}

// Units come in: paired, around our transporter nearest target; unpaired
// (or with no transporter left), spread around target in our strip. Those
// with hops still to go wait at the transporter to go on; the rest clear
// the landing area, so the next batch can come in.
static void apply_arrive(Faction *f, const Sim *sim, const Command *c)
{
	const Terrain *t = &sim->terrain;
	Units *u = &f->units;
	if (c->unit_type >= UNIT_TYPE_COUNT || unit_is_structure(c->unit_type) || c->amount < 0 || c->amount > SPAWN_MAX
	    || !route_ok(c->route) || !player_ok(c->player))
		return;
	bool unpaired = c->flags & COMMAND_UNPAIRED;
	ptrdiff_t g = unpaired ? -1 : nearest_transporter(f, c->target, 1e4f);
	V2 at = g >= 0 ? v2(u->px[g], u->pz[g]) : sim_strip_point(t, f->id, c->target);
	float ring = g >= 0 ? unit_defs[UNIT_TRANSPORTER].radius + 4.0f : 6.0f;
	bool onward = g >= 0 && route_first(c->route) >= 0;
	// Clear of the landing area, toward the middle of the map.
	V2 inward = v2(t->size_x * 0.5f - at.x, t->size_z * 0.5f - at.z);
	float len = v2_len(inward);
	V2 clear = len > 1.0f ? v2_add(at, v2_scale(inward, (LANDING_RADIUS + 8.0f) / len)) : at;
	for (int32_t k = 0; k < c->amount && u->count < u->cap; k++) {
		V2 p = clamp_to_map(t, v2(at.x + rng_range(&f->rng, -ring, ring), at.z + rng_range(&f->rng, -ring, ring)), 2.0f);
		ptrdiff_t d = units_spawn(u, c->unit_type, c->player, p.x, p.z, terrain_height(t, p.x, p.z));
		f->stats.units_spawned++;
		f->stats.units_arrived++;
		if (onward) {
			u->order[d] = ORDER_SEND;
			u->tx[d] = u->px[g];
			u->tz[d] = u->pz[g];
			u->building[d] = units_handle(u, g);
			u->dest[d] = (uint8_t)route_first(c->route);
			u->route[d] = route_rest(c->route);
		} else if (g >= 0) {
			V2 to = clamp_to_map(t, v2(clear.x + rng_range(&f->rng, -6.0f, 6.0f), clear.z + rng_range(&f->rng, -6.0f, 6.0f)), 2.0f);
			u->order[d] = ORDER_MOVE;
			u->tx[d] = to.x;
			u->tz[d] = to.z;
		}
	}
}

// Give: the units change hands as they are, mid-order and all; the new owner
// carries on or re-orders them. Output: these factories' units go to `to`.
static void apply_give(Faction *f, const Command *c)
{
	if (!player_ok(c->to)) return;
	Units *u = &f->units;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0) continue;
		if (c->kind == COMMAND_GIVE) {
			u->owner[d] = c->to;
			// A factory's output stays where it was pointed; anything else
			// (a unit on its way out included) is now wholly theirs.
			if (u->type[d] != UNIT_FACTORY) u->output[d] = c->to;
		} else if (u->type[d] == UNIT_FACTORY) {
			u->output[d] = c->to;
		}
	}
}

// Orders keep the group's shape: each unit aims at the target plus its offset
// from the group's centre, squeezed so the group doesn't stay spread out.
static void apply_command(Faction *f, const Sim *sim, const Command *c)
{
	const Terrain *t = &sim->terrain;
	if (c->kind >= COMMAND_KIND_COUNT) return;
	if (!finite_f(c->target.x) || !finite_f(c->target.z)) return;
	if (c->kind == COMMAND_SPAWN) {
		apply_spawn(f, t, c);
		return;
	}
	if (c->kind == COMMAND_PRODUCE) {
		apply_produce(f, c);
		return;
	}
	if (c->kind == COMMAND_ARRIVE) {
		apply_arrive(f, sim, c);
		return;
	}
	if (c->kind == COMMAND_SEND) {
		apply_send(f, sim, c);
		return;
	}
	if (c->kind == COMMAND_ENTRENCH || c->kind == COMMAND_MOBILIZE) {
		apply_transform(f, c);
		return;
	}
	if (c->kind == COMMAND_GIVE || c->kind == COMMAND_OUTPUT) {
		apply_give(f, c);
		return;
	}
	if (c->kind == COMMAND_BUILD && !unit_is_structure(c->unit_type)) return;
	Units *u = &f->units;
	V2 target = clamp_to_map(t, c->target, 1.0f);

	V2 sum = v2(0.0f, 0.0f);
	ptrdiff_t live = 0;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0) continue;
		sum = v2_add(sum, v2(u->px[d], u->pz[d]));
		live++;
	}
	if (live == 0) return;
	V2 centre = v2_scale(sum, 1.0f / (float)live);
	float spread = 0.0f;
	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0) continue;
		spread = f_max(spread, v2_len(v2_sub(v2(u->px[d], u->pz[d]), centre)));
	}
	float want = 1.8f * f_sqrt((float)live);
	float squeeze = spread > want ? want / spread : 1.0f;

	for (ptrdiff_t k = 0; k < c->count; k++) {
		ptrdiff_t d = units_find_own(u, c->units[k], c->player);
		if (d < 0 || unit_is_structure(u->type[d])) continue;
		// Builders all go to the one site; others keep the group's shape.
		bool build = c->kind == COMMAND_BUILD;
		if (build && !unit_is_builder(u->type[d])) continue;
		V2 offset = v2_scale(v2_sub(v2(u->px[d], u->pz[d]), centre), squeeze);
		V2 dest = build ? target : clamp_to_map(t, v2_add(target, offset), 1.0f);
		u->order[d] = order_for_command[c->kind];
		u->tx[d] = dest.x;
		u->tz[d] = dest.z;
		u->building[d] = NO_UNIT;
		if (build) u->build_type[d] = c->unit_type;
	}
}

static UnitType pick_type(Rng *rng, const int32_t weight[COMBAT_TYPE_COUNT])
{
	int32_t total = 0;
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) total += weight[k];
	int32_t roll = (int32_t)rng_below(rng, (uint32_t)total);
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) {
		roll -= weight[k];
		if (roll < 0) return (UnitType)k;
	}
	return UNIT_RAIDER;
}

static void spawn_near_base(Faction *f, const SimConfig *c, const Terrain *t, float radius)
{
	spawn_at(f, t, pick_type(&f->rng, c->type_weight), (PlayerSlot){0}, f->base, radius);
}

static void spawn(Faction *f, const Sim *sim)
{
	const SimConfig *c = &sim->config;
	if (c->economy || sim->tick.v % c->spawn_interval != 0) return;
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
		if (unit_is_structure(u->type[i])) {
			nvx[i] = nvz[i] = cx[i] = cz[i] = 0.0f;
			continue;
		}
		V2 p = v2(u->px[i], u->pz[i]);
		V2 desired = v2(0.0f, 0.0f);
		if (u->order[i] == ORDER_TRANSFORM) {
			// Digging in: stands still (the desired velocity stays zero).
		} else if (u->order[i] == ORDER_BUILD || u->order[i] == ORDER_SEND) {
			V2 to = v2_sub(v2(u->tx[i], u->tz[i]), p);
			float d = v2_len(to);
			float reach = u->order[i] == ORDER_SEND ? unit_defs[UNIT_TRANSPORTER].radius + SEND_REACH - 1.0f
			                                         : BUILD_RANGE + unit_defs[u->build_type[i]].radius;
			if (d > reach) desired = v2_scale(to, def->speed / d);
		} else if (u->order[i] != ORDER_IDLE) {
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
		float reach = def->radius + MAX_RADIUS;
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
			// Two movers share the correction; a structure doesn't budge.
			float share = unit_is_structure(u->type[j]) ? 1.0f : 0.5f;
			float d = f_sqrt(d2);
			push = v2_add(push, v2_scale(away, share * (min_d - d) / d));
		}
		push = v2_limit(push, SEPARATION_MAX);
		cx[i] = push.x;
		cz[i] = push.z;
	}

	for (ptrdiff_t i = 0; i < n; i++) {
		const UnitDef *def = &unit_defs[u->type[i]];
		if (unit_is_structure(u->type[i])) continue;
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

static ptrdiff_t nearest_enemy(const EnemyView *v, V2 from, float range, float min_range)
{
	CellRange r = grid_range(&v->grid, from.x - range, from.z - range, from.x + range, from.z + range);
	ptrdiff_t best = -1;
	float best_d2 = range * range;
	int32_t j;
	GRID_FOR(&v->grid, r, j) {
		float d2 = v2_len2(v2_sub(v2(v->px[j], v->pz[j]), from));
		if (d2 < min_range * min_range) continue; // too close to fire at
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
		if (def->range <= 0.0f || u->order[i] == ORDER_TRANSFORM) continue; // transforming: no firing

		V2 p = v2(u->px[i], u->pz[i]);
		ptrdiff_t e = view_find(v, u->target[i]);
		if (e >= 0) {
			float d2 = v2_len2(v2_sub(v2(v->px[e], v->pz[e]), p));
			if (d2 > def->range * def->range || d2 < def->min_range * def->min_range) e = -1;
		}
		// Retargeting is staggered by slot so the cost spreads across ticks.
		if (e < 0 && (tick.v + u->dense_slot[i]) % RETARGET_PERIOD == 0) {
			e = nearest_enemy(v, p, def->range, def->min_range);
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

// ---------------------------------------------------------------- economy

bool site_near(const Sim *sim, FactionId f, UnitType type, V2 near, V2 *out)
{
	// Eight directions as exact constants: no libm in the sim.
	static const V2 ring[8] = {
		{1.0f, 0.0f}, {0.70710678f, 0.70710678f}, {0.0f, 1.0f}, {-0.70710678f, 0.70710678f},
		{-1.0f, 0.0f}, {-0.70710678f, -0.70710678f}, {0.0f, -1.0f}, {0.70710678f, -0.70710678f},
	};
	if (placement_ok(sim, f, type, near)) {
		*out = near;
		return true;
	}
	for (int i = 0; i < 96; i++) {
		float r = 8.0f + 7.0f * (float)(i / 8);
		V2 at = v2_add(near, v2_scale(ring[(i + 3 * (i / 8)) % 8], r));
		if (placement_ok(sim, f, type, at)) {
			*out = at;
			return true;
		}
	}
	return false;
}

bool placement_ok(const Sim *sim, FactionId fid, UnitType type, V2 at)
{
	const Terrain *t = &sim->terrain;
	const Faction *f = &sim->factions[fid.v];
	const UnitDef *def = &unit_defs[type];
	if (!unit_is_structure(type)) return false;
	float margin = def->radius + 2.0f;
	if (at.x < margin || at.z < margin || at.x > t->size_x - margin || at.z > t->size_z - margin) return false;
	int32_t spot = spot_at(&sim->spots, at);
	if (type == UNIT_EXTRACTOR && spot < 0) return false;
	const Units *u = &f->units;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!unit_is_structure(u->type[i])) continue;
		float min_d = def->radius + unit_defs[u->type[i]].radius + 1.0f;
		if (v2_len2(v2_sub(v2(u->px[i], u->pz[i]), at)) < min_d * min_d) return false;
		if (type == UNIT_EXTRACTOR && u->type[i] == UNIT_EXTRACTOR && spot_at(&sim->spots, v2(u->px[i], u->pz[i])) == spot)
			return false;
	}
	const Perception *seen = &f->seen;
	for (ptrdiff_t s = 0; s < seen->cap; s++) {
		if (!seen->live[s] || !unit_is_structure(seen->type[s])) continue;
		float min_d = def->radius + unit_defs[seen->type[s]].radius + 1.0f;
		if (v2_len2(v2_sub(perception_at(seen, s, sim->tick), at)) < min_d * min_d) return false;
	}
	return true;
}

// A builder's part in this tick: whose progress it advances and how much
// work it offers. Factories build their queue's front item.
typedef struct {
	int32_t builder;     // dense index
	int32_t target;      // structure being built, or -1 for a factory's item
	UnitType type;       // what the work is for
	float work;
} Job;

// An engineer at its site starts the structure, or helps with one already
// started there. Returns the structure's dense index, or -1.
static ptrdiff_t engineer_site(Faction *f, const Sim *sim, ptrdiff_t e)
{
	Units *u = &f->units;
	ptrdiff_t s = units_find(u, u->building[e]);
	if (s >= 0 && u->progress[s] < 1.0f) return s;
	u->building[e] = NO_UNIT;
	UnitType type = u->build_type[e];
	V2 site = v2(u->tx[e], u->tz[e]);
	if (type == UNIT_EXTRACTOR) {
		int32_t k = spot_at(&sim->spots, site);
		if (k >= 0) site = sim->spots.at[k];
	}
	if (v2_len(v2_sub(v2(u->px[e], u->pz[e]), site)) > BUILD_RANGE + unit_defs[type].radius) return -1;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != type || u->progress[i] >= 1.0f) continue;
		if (v2_len2(v2_sub(v2(u->px[i], u->pz[i]), site)) < 9.0f) {
			u->building[e] = units_handle(u, i);
			return i;
		}
	}
	if (u->count == u->cap || !placement_ok(sim, f->id, type, site)) {
		u->order[e] = ORDER_IDLE; // nothing to do here: report back idle
		return -1;
	}
	s = units_spawn(u, type, u->owner[e], site.x, site.z, terrain_height(&sim->terrain, site.x, site.z));
	u->progress[s] = 0.0f;
	u->hp[s] = 0.1f * unit_defs[type].hp;
	f->stats.units_spawned++;
	u->building[e] = units_handle(u, s);
	return s;
}

// Units leave a factory on the side facing the middle of the map.
static void factory_release(Faction *f, const Sim *sim, ptrdiff_t fac, UnitType type)
{
	Units *u = &f->units;
	if (u->count == u->cap) return;
	float face = u->px[fac] < sim->terrain.size_x * 0.5f ? 1.0f : -1.0f;
	float off = unit_defs[UNIT_FACTORY].radius + unit_defs[type].radius + 1.5f;
	V2 at = clamp_to_map(&sim->terrain, v2(u->px[fac] + off * face, u->pz[fac]), 2.0f);
	units_spawn(u, type, u->output[fac], at.x, at.z, terrain_height(&sim->terrain, at.x, at.z));
	f->stats.units_spawned++;
}

// The batch waiting at transporter g: the units next in line (first by
// index), all for the same hop and route, up to the batch size. Their
// indices go to out, ascending, if it isn't null.
static ptrdiff_t waiting_batch(const Units *u, ptrdiff_t g, int32_t *out, bool *unpaired)
{
	UnitHandle gh = units_handle(u, g);
	float reach = unit_defs[UNIT_TRANSPORTER].radius + SEND_REACH;
	ptrdiff_t n = 0, first = -1, limit = 0;
	*unpaired = false;
	for (ptrdiff_t i = 0; i < u->count && (first < 0 || n < limit); i++) {
		if (u->order[i] != ORDER_SEND || !unit_handle_eq(u->building[i], gh)) continue;
		if (v2_len2(v2_sub(v2(u->px[i], u->pz[i]), v2(u->px[g], u->pz[g]))) > reach * reach) continue;
		if (first < 0) {
			first = i;
			*unpaired = u->route[i] & ROUTE_UNPAIRED;
			limit = *unpaired ? UNPAIRED_BATCH : PAIRED_BATCH;
		}
		if (u->dest[i] != u->dest[first] || u->route[i] != u->route[first]
		    || !player_eq(u->output[i], u->output[first]))
			continue;
		if (out) out[n] = (int32_t)i;
		n++;
	}
	return n;
}

// Income, then every builder's work at one shared rate the store can pay for.
static void run_economy(Faction *f, const Sim *sim)
{
	Economy *e = &f->economy;
	Units *u = &f->units;
	float income_m = BASE_METAL_INCOME, income_e = BASE_ENERGY_INCOME;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->progress[i] < 1.0f) continue;
		const UnitDef *def = &unit_defs[u->type[i]];
		income_e += def->energy_income;
		if (def->metal_income > 0.0f) {
			int32_t k = spot_at(&sim->spots, v2(u->px[i], u->pz[i]));
			if (k >= 0 && (e->spots >> k & 1)) income_m += def->metal_income;
		}
	}
	e->metal = f_min(e->metal + income_m * SIM_DT, e->metal_cap);
	e->energy = f_min(e->energy + income_e * SIM_DT, e->energy_cap);
	e->metal_income = income_m;
	e->energy_income = income_e;

	Job *jobs = PUSH(&f->scratch, Job, u->count);
	ptrdiff_t n = 0;
	float want_m = 0.0f, want_e = 0.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		Job j = {(int32_t)i, -1, UNIT_RAIDER, 0.0f};
		if (unit_is_builder(u->type[i]) && u->order[i] == ORDER_BUILD) {
			ptrdiff_t s = engineer_site(f, sim, i);
			if (s < 0) continue;
			j.target = (int32_t)s;
			j.type = u->type[s];
			j.work = unit_defs[u->type[i]].build_power * SIM_DT;
		} else if (u->type[i] == UNIT_FACTORY && u->progress[i] >= 1.0f && u->queue_len[i] > 0) {
			j.type = queue_front(u, i);
			j.work = unit_defs[UNIT_FACTORY].build_power * SIM_DT;
		} else {
			continue;
		}
		const UnitDef *def = &unit_defs[j.type];
		want_m += def->metal_cost * j.work / def->build_time;
		want_e += def->energy_cost * j.work / def->build_time;
		jobs[n++] = j;
	}
	// Transporters charge on energy for the batch waiting at them, like a
	// job that only costs energy.
	float *charge_step = PUSH(&f->scratch, float, u->count > 0 ? u->count : 1);
	float *charge_cost = PUSH(&f->scratch, float, u->count > 0 ? u->count : 1);
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_TRANSPORTER || u->progress[i] < 1.0f || u->work[i] >= 1.0f) continue;
		bool unpaired;
		ptrdiff_t waiting = waiting_batch(u, i, nullptr, &unpaired);
		if (waiting == 0) continue;
		charge_step[i] = SIM_DT / (unpaired ? UNPAIRED_CHARGE : PAIRED_CHARGE);
		charge_cost[i] = unpaired ? UNPAIRED_ENERGY + UNPAIRED_ENERGY_PER * (float)waiting : PAIRED_ENERGY * (float)waiting;
		want_e += charge_cost[i] * charge_step[i];
	}
	// Transforms, at their own time and price.
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->order[i] != ORDER_TRANSFORM) continue;
		bool dig_in = unit_is_combat(u->type[i]);
		float step = SIM_DT / (dig_in ? ENTRENCH_TIME : MOBILIZE_TIME);
		want_m += (dig_in ? ENTRENCH_METAL : MOBILIZE_METAL) * step;
		want_e += (dig_in ? ENTRENCH_ENERGY : MOBILIZE_ENERGY) * step;
	}
	float rate_m = want_m > e->metal ? e->metal / want_m : 1.0f;
	float rate_e = want_e > e->energy ? e->energy / want_e : 1.0f;
	e->rate = n ? f_min(rate_m, rate_e) : 1.0f;
	e->metal_wanted = want_m / SIM_DT;
	e->energy_wanted = want_e / SIM_DT;

	float spent_m = 0.0f, spent_e = 0.0f;
	for (ptrdiff_t k = 0; k < n; k++) {
		const Job *j = &jobs[k];
		const UnitDef *def = &unit_defs[j->type];
		float rate = 1.0f;
		if (def->metal_cost > 0.0f) rate = f_min(rate, rate_m);
		if (def->energy_cost > 0.0f) rate = f_min(rate, rate_e);
		float step = j->work * rate / def->build_time;
		if (j->target >= 0) {
			// Never past done, and never paying for more than was built.
			step = f_min(step, 1.0f - u->progress[j->target]);
			u->progress[j->target] += step;
			u->hp[j->target] += 0.9f * def->hp * step;
			if (u->progress[j->target] >= 1.0f) {
				u->progress[j->target] = 1.0f;
				u->order[j->builder] = ORDER_IDLE;
				u->building[j->builder] = NO_UNIT;
			}
		} else {
			step = f_min(step, 1.0f - u->work[j->builder]);
			u->work[j->builder] += step;
			if (u->work[j->builder] >= 1.0f) {
				u->work[j->builder] = 0.0f;
				queue_pop(u, j->builder);
				if (u->repeat[j->builder]) queue_push(u, j->builder, j->type);
				factory_release(f, sim, j->builder, j->type);
			}
		}
		spent_m += def->metal_cost * step;
		spent_e += def->energy_cost * step;
	}
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->order[i] != ORDER_TRANSFORM) continue;
		bool dig_in = unit_is_combat(u->type[i]);
		float rate = f_min(dig_in && ENTRENCH_METAL > 0.0f ? rate_m : 1.0f, rate_e);
		float step = f_min(SIM_DT / (dig_in ? ENTRENCH_TIME : MOBILIZE_TIME) * rate, 1.0f - u->work[i]);
		u->work[i] += step;
		spent_m += (dig_in ? ENTRENCH_METAL : MOBILIZE_METAL) * step;
		spent_e += (dig_in ? ENTRENCH_ENERGY : MOBILIZE_ENERGY) * step;
		if (u->work[i] >= 1.0f) finish_transform(u, i);
	}
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (charge_step[i] <= 0.0f) continue;
		float step = f_min(charge_step[i] * rate_e, 1.0f - u->work[i]);
		u->work[i] += step;
		spent_e += charge_cost[i] * step;
	}
	e->metal = f_max(0.0f, e->metal - spent_m);
	e->energy = f_max(0.0f, e->energy - spent_e);
	e->metal_spent = spent_m / SIM_DT;
	e->energy_spent = spent_e / SIM_DT;
}

enum { MAX_DEPARTURES = 64 };

static void add_departure(Faction *f, const Departure *dep)
{
	for (ptrdiff_t k = 0; k < f->departure_count; k++) {
		Departure *d = &f->departures[k];
		if (d->dest == dep->dest && d->type == dep->type && d->route == dep->route && d->unpaired == dep->unpaired
		    && d->landing.x == dep->landing.x && d->landing.z == dep->landing.z && player_eq(d->owner, dep->owner)) {
			d->count++;
			return;
		}
	}
	f->departures[f->departure_count++] = *dep;
}

// Charged transporters fire: the batch waiting, all for one hop and route.
static void fire_transporters(Faction *f)
{
	Units *u = &f->units;
	f->departures = PUSH(&f->scratch, Departure, MAX_DEPARTURES);
	f->departure_count = 0;
	int32_t leaving[PAIRED_BATCH];
	// Waiting at a transporter that's gone: stand down.
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (u->order[i] == ORDER_SEND && units_find(u, u->building[i]) < 0) u->order[i] = ORDER_IDLE;
	for (ptrdiff_t g = 0; g < u->count && f->departure_count + UNIT_TYPE_COUNT <= MAX_DEPARTURES; g++) {
		if (u->type[g] != UNIT_TRANSPORTER || u->progress[g] < 1.0f || u->work[g] < 1.0f) continue;
		bool unpaired;
		ptrdiff_t n = waiting_batch(u, g, leaving, &unpaired);
		if (n == 0) continue;
		Departure dep = {.dest = u->dest[leaving[0]], .route = u->route[leaving[0]] & ~ROUTE_UNPAIRED, .unpaired = unpaired,
		                 .landing = unpaired ? v2(u->tx[g], u->tz[g]) : v2(0.0f, 0.0f), .count = 1,
		                 .owner = u->output[leaving[0]]};
		for (ptrdiff_t k = 0; k < n; k++) {
			dep.type = u->type[leaving[k]];
			add_departure(f, &dep);
		}
		UnitHandle gh = units_handle(u, g);
		// Leaving units are removed highest index first, so swap-remove
		// never moves one still to be removed.
		for (ptrdiff_t k = n - 1; k >= 0; k--) {
			units_kill(u, leaving[k]);
			f->stats.units_departed++;
		}
		// Removal may have moved the transporter: find it again by handle. If
		// the loop now skips one, it fires next tick; none fires twice, as
		// firing empties the charge.
		u->work[units_find(u, gh)] = 0.0f;
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

// The start of a faction's phase: its effects, applied. The outbox was
// delivered by the interaction phase, so it starts the tick empty.
static void apply_effects(Faction *f, const FactionEffects *fx, Tick tick)
{
	f->outbox_count = 0;
	f->economy.spots = fx->spots;
	Projectiles *p = &f->incoming;
	for (ptrdiff_t k = 0; k < fx->arrived_count; k++) {
		const Launch *l = &fx->arrived[k];
		if (p->count == p->cap) {
			f->stats.launches_dropped++;
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
	Perception *seen = &f->seen;
	for (ptrdiff_t k = 0; k < fx->seen_count; k++) {
		const SeenEntry *e = &fx->seen[k];
		ptrdiff_t s = e->slot;
		if (e->tag == SEEN_LOST) {
			seen->live[s] = 0;
			continue;
		}
		if (e->tag == SEEN_ENTER) {
			seen->live[s] = 1;
			seen->gen[s] = e->gen;
			seen->type[s] = e->type;
		}
		seen->qx[s] = e->qx;
		seen->qz[s] = e->qz;
		seen->qvx[s] = e->qvx;
		seen->qvz[s] = e->qvz;
		seen->sent[s] = tick.v;
	}
}

// The enemies a faction can target this tick: its perception, dead-reckoned
// to now. Everything here is reproducible from the faction's effects.
static void build_view(Faction *f, const Terrain *t, Tick tick)
{
	const Perception *p = &f->seen;
	EnemyView *v = &f->view;
	Arena *a = &f->scratch;
	*v = (EnemyView){.slot_cap = p->cap};
	v->px = PUSH(a, float, p->cap);
	v->py = PUSH(a, float, p->cap);
	v->pz = PUSH(a, float, p->cap);
	v->vx = PUSH(a, float, p->cap);
	v->vz = PUSH(a, float, p->cap);
	v->handle = PUSH(a, UnitHandle, p->cap);
	v->radius = PUSH(a, float, p->cap);
	v->slot_index = PUSH(a, int32_t, p->cap);
	v->slot_gen = p->gen;
	ptrdiff_t n = 0;
	for (ptrdiff_t s = 0; s < p->cap; s++) {
		v->slot_index[s] = -1;
		if (!p->live[s]) continue;
		V2 at = perception_at(p, s, tick);
		v->px[n] = at.x;
		v->pz[n] = at.z;
		v->py[n] = terrain_height(t, at.x, at.z);
		v->vx[n] = dq_vel(p->qvx[s]);
		v->vz[n] = dq_vel(p->qvz[s]);
		v->handle[n] = (UnitHandle){(uint32_t)s, p->gen[s]};
		v->radius[n] = unit_defs[p->type[s]].radius;
		v->slot_index[s] = (int32_t)n;
		n++;
	}
	v->count = n;
	grid_build(&v->grid, a, t, v->px, v->pz, n);
}

static void faction_phase(Sim *sim, Faction *f, const FactionEffects *fx, const Command *commands,
                          ptrdiff_t count)
{
	arena_reset(&f->scratch);
	apply_effects(f, fx, sim->tick);
	build_view(f, &sim->terrain, sim->tick);
	for (ptrdiff_t k = 0; k < count; k++)
		if (commands[k].faction.v == f->id.v) apply_command(f, sim, &commands[k]);
	spawn(f, sim);
	move_units(f, &sim->terrain);
	if (sim->config.economy) {
		run_economy(f, sim);
		fire_transporters(f);
	} else {
		f->departure_count = 0;
		// No economy to pay from: transforms just take their time.
		Units *u = &f->units;
		for (ptrdiff_t i = 0; i < u->count; i++) {
			if (u->order[i] != ORDER_TRANSFORM) continue;
			u->work[i] = f_min(1.0f, u->work[i] + SIM_DT / (unit_is_combat(u->type[i]) ? ENTRENCH_TIME : MOBILIZE_TIME));
			if (u->work[i] >= 1.0f) finish_transform(u, i);
		}
	}
	fly_projectiles(f, &sim->terrain);
	fire_weapons(f, &f->view, &sim->terrain, sim->tick);
	remove_dead(f);
}

// ---------------------------------------------------------------- lifecycle

V2 sim_player_base(const Sim *sim, FactionId f, PlayerSlot p)
{
	V2 base = sim->factions[f.v].base;
	float dz = (float)((p.v + 1) / 2) * KIT_SPACING * (p.v % 2 ? -1.0f : 1.0f);
	return v2(base.x, base.z + dz);
}

void sim_init(Sim *sim, SimConfig config)
{
	ASSERT(config.max_units > 0 && config.max_projectiles > 0);
	ASSERT(config.population <= config.max_units && config.initial_units <= config.population);
	ASSERT(config.spawn_interval > 0);
	for (int k = 0; k < FACTION_COUNT; k++) ASSERT(config.players[k] >= 1 && config.players[k] <= MAX_PLAYERS);
	int32_t weights = 0;
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) {
		ASSERT(config.type_weight[k] >= 0 && config.type_weight[k] <= 1000);
		weights += config.type_weight[k];
	}
	ASSERT(weights > 0);
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) ASSERT(unit_defs[k].sight <= MAX_SIGHT); // or sight searches miss it
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
		perception_init(&f->seen, &sim->zone, config.max_units);
		f->outbox_cap = config.max_units;
		f->outbox = PUSH(&sim->zone, Launch, f->outbox_cap);
		f->scratch = arena_create(scratch_names[k], (ptrdiff_t)64 << 20);

		f->economy = (Economy){START_METAL, START_ENERGY, METAL_CAP, ENERGY_CAP, .rate = 1.0f};
		if (config.economy) {
			// Each player: a factory facing the middle, and two engineers
			// beside it; the first at the base, the others above and below it
			// in turn. In a world, only in the faction's home zone.
			if (!(config.bases >> k & 1)) continue;
			float face = k == 0 ? 1.0f : -1.0f;
			for (int pl = 0; pl < config.players[k]; pl++) {
				PlayerSlot owner = {(uint8_t)pl};
				V2 at = v2_add(sim_player_base(sim, f->id, owner), v2(10.0f * face, 0.0f));
				units_spawn(&f->units, UNIT_FACTORY, owner, at.x, at.z, terrain_height(t, at.x, at.z));
				for (int e = 0; e < 2; e++) {
					V2 q = v2(at.x + 9.0f * face, at.z + (e ? 6.0f : -6.0f));
					units_spawn(&f->units, UNIT_ENGINEER, owner, q.x, q.z, terrain_height(t, q.x, q.z));
				}
				f->stats.units_spawned += 3;
			}
			continue;
		}
		float spread = 1.6f * f_sqrt((float)config.initial_units);
		for (int32_t n = 0; n < config.initial_units; n++) spawn_near_base(f, &config, t, spread);
	}
	spots_generate(&sim->spots, t, config.seed, bases);
	for (int k = 0; k < MAX_SPOTS; k++) sim->spot_owner[k] = -1;
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
		faction_phase(sim, &sim->factions[k], &sim->effects[k], commands, count);
	sim->tick.v++;
}

void faction_tick(Sim *sim, FactionId f, const FactionEffects *fx, const Command *commands, ptrdiff_t count)
{
	faction_phase(sim, &sim->factions[f.v], fx, commands, count);
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
		h = hash_f32(h, u->progress[i]);
		h = hash_u32(h, u->build_type[i]);
		h = hash_u32(h, u->building[i].index);
		h = hash_u32(h, u->building[i].generation);
		h = hash_f32(h, u->work[i]);
		h = hash_u64(h, u->queue[i]);
		h = hash_u32(h, (uint32_t)u->queue_len[i] << 24 | (uint32_t)u->repeat[i] << 16 | (uint32_t)u->dest[i] << 8 | u->seeded[i]);
		h = hash_u32(h, u->route[i]);
		h = hash_u32(h, (uint32_t)u->owner[i].v << 8 | u->output[i].v);
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

static Hash hash_perception(Hash h, const Perception *p)
{
	for (ptrdiff_t s = 0; s < p->cap; s++) {
		if (!p->live[s]) continue;
		h = hash_u32(h, (uint32_t)s);
		h = hash_u32(h, p->gen[s]);
		h = hash_u32(h, p->type[s]);
		h = hash_u32(h, (uint32_t)p->qx[s] << 16 | p->qz[s]);
		h = hash_u32(h, (uint32_t)(uint16_t)p->qvx[s] << 16 | (uint16_t)p->qvz[s]);
		h = hash_u64(h, (uint64_t)p->sent[s]);
	}
	return h;
}

Hash faction_hash(const Faction *f)
{
	Hash h = hash_u32(hash_begin(), f->id.v);
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
	h = hash_u64(h, (uint64_t)f->stats.units_departed);
	h = hash_u64(h, (uint64_t)f->stats.units_arrived);
	const Economy *e = &f->economy;
	float ef[] = {e->metal, e->energy, e->metal_cap, e->energy_cap, e->metal_income, e->energy_income,
	              e->metal_spent, e->energy_spent, e->metal_wanted, e->energy_wanted, e->rate};
	for (int k = 0; k < (int)countof(ef); k++) h = hash_f32(h, ef[k]);
	h = hash_u64(h, e->spots);
	return hash_perception(h, &f->seen);
}

Hash sim_hash_now(const Sim *sim)
{
	Hash h = hash_u64(hash_begin(), (uint64_t)sim->tick.v);
	for (int k = 0; k < FACTION_COUNT; k++) h = hash_u64(h, faction_hash(&sim->factions[k]).v);
	for (int k = 0; k < MAX_SPOTS; k++) h = hash_u32(h, (uint32_t)(int32_t)sim->spot_owner[k]);
	return h;
}

// ---------------------------------------------------------------- faction snapshots

#define PUT_ARRAY(b, arr, n, put)                                     \
	for (ptrdiff_t i_ = 0; i_ < (n); i_++) put((b), (arr)[i_])

void faction_write(const Faction *f, ByteBuf *b)
{
	const Units *u = &f->units;
	put64(b, f->rng.state);
	put32(b, (uint32_t)u->cap);
	put32(b, (uint32_t)u->count);
	put32(b, (uint32_t)u->free_count);
	PUT_ARRAY(b, u->slot_gen, u->cap, put32);
	for (ptrdiff_t i = 0; i < u->free_count; i++) put32(b, (uint32_t)u->free_slots[i]);
	for (ptrdiff_t i = 0; i < u->count; i++) {
		put32(b, (uint32_t)u->dense_slot[i]);
		putf(b, u->px[i]);
		putf(b, u->py[i]);
		putf(b, u->pz[i]);
		putf(b, u->vx[i]);
		putf(b, u->vz[i]);
		putf(b, u->hp[i]);
		put8(b, u->type[i]);
		put8(b, u->order[i]);
		putf(b, u->tx[i]);
		putf(b, u->tz[i]);
		put32(b, (uint32_t)u->cooldown[i]);
		put32(b, u->target[i].index);
		put32(b, u->target[i].generation);
		put8(b, u->engaged[i]);
		putf(b, u->progress[i]);
		put8(b, u->build_type[i]);
		put32(b, u->building[i].index);
		put32(b, u->building[i].generation);
		putf(b, u->work[i]);
		put64(b, u->queue[i]);
		put8(b, u->queue_len[i]);
		put8(b, u->repeat[i]);
		put8(b, u->dest[i]);
		put32(b, u->route[i]);
		put8(b, u->seeded[i]);
		put8(b, u->owner[i].v);
		put8(b, u->output[i].v);
	}
	const Projectiles *p = &f->incoming;
	put32(b, (uint32_t)p->count);
	put32(b, p->next_id);
	for (ptrdiff_t i = 0; i < p->count; i++) {
		putf(b, p->px[i]);
		putf(b, p->py[i]);
		putf(b, p->pz[i]);
		putf(b, p->vx[i]);
		putf(b, p->vy[i]);
		putf(b, p->vz[i]);
		put8(b, p->weapon[i]);
		put32(b, (uint32_t)p->ttl[i]);
		put32(b, p->id[i]);
	}
	put32(b, (uint32_t)f->outbox_count);
	for (ptrdiff_t i = 0; i < f->outbox_count; i++) {
		const Launch *l = &f->outbox[i];
		putf(b, l->pos.x);
		putf(b, l->pos.y);
		putf(b, l->pos.z);
		putf(b, l->vel.x);
		putf(b, l->vel.y);
		putf(b, l->vel.z);
		put8(b, l->weapon);
	}
	put64(b, (uint64_t)f->stats.shots_fired);
	put64(b, (uint64_t)f->stats.units_spawned);
	put64(b, (uint64_t)f->stats.units_lost);
	put64(b, (uint64_t)f->stats.launches_dropped);
	put64(b, (uint64_t)f->stats.units_departed);
	put64(b, (uint64_t)f->stats.units_arrived);
	const Economy *e = &f->economy;
	float ef[] = {e->metal, e->energy, e->metal_cap, e->energy_cap, e->metal_income, e->energy_income,
	              e->metal_spent, e->energy_spent, e->metal_wanted, e->energy_wanted, e->rate};
	for (int k = 0; k < (int)countof(ef); k++) putf(b, ef[k]);
	put64(b, e->spots);
	const Perception *seen = &f->seen;
	uint32_t live = 0;
	for (ptrdiff_t s = 0; s < seen->cap; s++) live += seen->live[s];
	put32(b, live);
	for (ptrdiff_t s = 0; s < seen->cap; s++) {
		if (!seen->live[s]) continue;
		put32(b, (uint32_t)s);
		put32(b, seen->gen[s]);
		put8(b, seen->type[s]);
		put16(b, seen->qx[s]);
		put16(b, seen->qz[s]);
		put16(b, (uint16_t)seen->qvx[s]);
		put16(b, (uint16_t)seen->qvz[s]);
		put64(b, (uint64_t)seen->sent[s]);
	}
}

static bool ok_float(float x) { return x == x && x - x == 0.0f; }

// Reads into f, which must have been set up by sim_init with the same config.
// Every invariant of Units, Projectiles and Perception is checked, because a
// snapshot arrives over the network.
bool faction_read(Faction *f, ByteReader *r)
{
	Units *u = &f->units;
	f->rng.state = get64(r);
	uint32_t cap = get32(r), count = get32(r), free_count = get32(r);
	if (r->bad || cap != (uint32_t)u->cap || count > cap || free_count != cap - count) return false;
	u->count = count;
	u->free_count = free_count;
	for (ptrdiff_t s = 0; s < u->cap; s++) {
		u->slot_gen[s] = get32(r);
		u->slot_dense[s] = -1;
	}
	for (ptrdiff_t i = 0; i < u->free_count; i++) {
		uint32_t s = get32(r);
		if (s >= cap || u->slot_dense[s] != -1) return false;
		u->slot_dense[s] = -2; // marks free while checking for duplicates
		u->free_slots[i] = (int32_t)s;
	}
	for (ptrdiff_t i = 0; i < u->count; i++) {
		uint32_t s = get32(r);
		if (r->bad || s >= cap || u->slot_dense[s] != -1) return false;
		u->slot_dense[s] = (int32_t)i;
		u->dense_slot[i] = (int32_t)s;
		u->px[i] = getf(r);
		u->py[i] = getf(r);
		u->pz[i] = getf(r);
		u->vx[i] = getf(r);
		u->vz[i] = getf(r);
		u->hp[i] = getf(r);
		uint8_t type = get8(r), order = get8(r);
		u->tx[i] = getf(r);
		u->tz[i] = getf(r);
		u->cooldown[i] = (int32_t)get32(r);
		u->target[i] = (UnitHandle){get32(r), get32(r)};
		u->engaged[i] = get8(r);
		u->progress[i] = getf(r);
		uint8_t build_type = get8(r);
		u->building[i] = (UnitHandle){get32(r), get32(r)};
		u->work[i] = getf(r);
		u->queue[i] = get64(r);
		u->queue_len[i] = get8(r);
		u->repeat[i] = get8(r);
		u->dest[i] = get8(r);
		u->route[i] = get32(r);
		u->seeded[i] = get8(r);
		u->owner[i] = (PlayerSlot){get8(r)};
		u->output[i] = (PlayerSlot){get8(r)};
		if (!player_ok(u->owner[i]) || !player_ok(u->output[i])) return false;
		if (type >= UNIT_TYPE_COUNT || order >= ORDER_KIND_COUNT || u->engaged[i] > 1 || u->cooldown[i] < 0
		    || build_type >= UNIT_TYPE_COUNT || u->queue_len[i] > FACTORY_QUEUE || u->repeat[i] > 1 || u->seeded[i] > 1
		    || u->dest[i] >= ZONE_MAX || !route_ok(u->route[i] & ~ROUTE_UNPAIRED)
		    // work: a factory item's progress, below 1; a transporter's charge, up to 1.
		    || !(u->progress[i] >= 0.0f && u->progress[i] <= 1.0f) || !(u->work[i] >= 0.0f && u->work[i] <= 1.0f))
			return false;
		for (int q = 0; q < u->queue_len[i]; q++)
			if ((u->queue[i] >> (8 * q) & 0xff) >= UNIT_EXTRACTOR) return false;
		u->build_type[i] = (UnitType)build_type;
		u->type[i] = (UnitType)type;
		u->order[i] = (OrderKind)order;
		float fs[] = {u->px[i], u->py[i], u->pz[i], u->vx[i], u->vz[i], u->hp[i], u->tx[i], u->tz[i]};
		for (int k = 0; k < (int)countof(fs); k++)
			if (!ok_float(fs[k])) return false;
	}
	for (ptrdiff_t s = 0; s < u->cap; s++) {
		if (u->slot_dense[s] == -2) u->slot_dense[s] = -1;
		else if (u->slot_dense[s] == -1) return false; // neither live nor free
	}
	Projectiles *p = &f->incoming;
	uint32_t shells = get32(r);
	p->next_id = get32(r);
	if (r->bad || shells > (uint32_t)p->cap) return false;
	p->count = shells;
	for (ptrdiff_t i = 0; i < p->count; i++) {
		p->px[i] = getf(r);
		p->py[i] = getf(r);
		p->pz[i] = getf(r);
		p->vx[i] = getf(r);
		p->vy[i] = getf(r);
		p->vz[i] = getf(r);
		uint8_t weapon = get8(r);
		p->ttl[i] = (int32_t)get32(r);
		p->id[i] = get32(r);
		if (weapon >= UNIT_TYPE_COUNT || p->ttl[i] <= 0) return false;
		p->weapon[i] = (UnitType)weapon;
		float fs[] = {p->px[i], p->py[i], p->pz[i], p->vx[i], p->vy[i], p->vz[i]};
		for (int k = 0; k < (int)countof(fs); k++)
			if (!ok_float(fs[k])) return false;
	}
	uint32_t out = get32(r);
	if (r->bad || out > (uint32_t)f->outbox_cap) return false;
	f->outbox_count = out;
	for (ptrdiff_t i = 0; i < f->outbox_count; i++) {
		Launch *l = &f->outbox[i];
		l->pos = v3(getf(r), getf(r), getf(r));
		l->vel = v3(getf(r), getf(r), getf(r));
		uint8_t weapon = get8(r);
		if (weapon >= UNIT_TYPE_COUNT) return false;
		l->weapon = (UnitType)weapon;
		l->target = faction_enemy(f->id);
	}
	f->stats.shots_fired = (int64_t)get64(r);
	f->stats.units_spawned = (int64_t)get64(r);
	f->stats.units_lost = (int64_t)get64(r);
	f->stats.launches_dropped = (int64_t)get64(r);
	f->stats.units_departed = (int64_t)get64(r);
	f->stats.units_arrived = (int64_t)get64(r);
	Economy *e = &f->economy;
	float *ef[] = {&e->metal, &e->energy, &e->metal_cap, &e->energy_cap, &e->metal_income, &e->energy_income,
	               &e->metal_spent, &e->energy_spent, &e->metal_wanted, &e->energy_wanted, &e->rate};
	for (int k = 0; k < (int)countof(ef); k++) {
		*ef[k] = getf(r);
		if (!ok_float(*ef[k])) return false;
	}
	e->spots = get64(r);
	Perception *seen = &f->seen;
	memset(seen->live, 0, (size_t)seen->cap);
	uint32_t live = get32(r);
	if (r->bad || live > (uint32_t)seen->cap) return false;
	for (uint32_t k = 0; k < live; k++) {
		uint32_t s = get32(r);
		if (r->bad || s >= (uint32_t)seen->cap || seen->live[s]) return false;
		seen->gen[s] = get32(r);
		uint8_t type = get8(r);
		seen->qx[s] = get16(r);
		seen->qz[s] = get16(r);
		seen->qvx[s] = (int16_t)get16(r);
		seen->qvz[s] = (int16_t)get16(r);
		seen->sent[s] = (int64_t)get64(r);
		if (type >= UNIT_TYPE_COUNT) return false;
		seen->type[s] = (UnitType)type;
		seen->live[s] = 1;
	}
	return !r->bad;
}

// ---------------------------------------------------------------- zone snapshots

void sim_save(const Sim *sim, ByteBuf *b)
{
	put32(b, 0x5a4d4953u); // "SIMZ"
	put64(b, (uint64_t)sim->tick.v);
	for (int k = 0; k < MAX_SPOTS; k++) put8(b, (uint8_t)sim->spot_owner[k]);
	for (int f = 0; f < FACTION_COUNT; f++) faction_write(&sim->factions[f], b);
}

bool sim_load(Sim *sim, ByteReader *r)
{
	if (get32(r) != 0x5a4d4953u) return false;
	int64_t tick = (int64_t)get64(r);
	if (r->bad || tick < 0) return false;
	sim->tick.v = tick;
	for (int k = 0; k < MAX_SPOTS; k++) {
		int8_t o = (int8_t)get8(r);
		if (o < -1 || o >= FACTION_COUNT) return false;
		sim->spot_owner[k] = o;
	}
	for (int f = 0; f < FACTION_COUNT; f++)
		if (!faction_read(&sim->factions[f], r)) return false;
	return !r->bad;
}
