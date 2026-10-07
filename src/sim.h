// The simulation: a pure function from state + commands to the next state.
//
// State is split by faction. Each tick has two phases:
//
//   interaction — the only code that reads more than one faction. For each
//                 faction it produces that faction's effects: the shells now
//                 arriving at it, and what it sees of its enemies (perception
//                 entries: dead-reckoned and quantized, as on the wire).
//   faction     — per faction, from that faction's state, its commands and
//                 its effects alone. Applies the effects, then moves units,
//                 flies incoming shells against its own units, aims at the
//                 enemies it perceives and fires into its outbox.
//
// So a faction's state is a pure function of its commands and effects: a
// client given those runs faction_tick and matches faction_hash bit for bit,
// while never receiving anything about enemies it can't see.
//
// Nothing here includes raylib, reads a clock or touches a file.
#pragma once

#include "arena.h"
#include "base.h"
#include "bytes.h"
#include "hash.h"
#include "quant.h"
#include "rng.h"
#include "terrain.h"
#include "vmath.h"

enum { FACTION_COUNT = 2 };

static constexpr float SIM_HZ = 30.0f;
static constexpr float SIM_DT = 1.0f / 30.0f;
static constexpr float SIM_GRAVITY = 20.0f;

typedef enum : uint8_t {
	UNIT_RAIDER,
	UNIT_TANK,
	UNIT_ARTILLERY,
	UNIT_TYPE_COUNT,
} UnitType;

// What a unit type is. Content, not state: never changes during a run.
typedef struct {
	const char *name;
	float speed;        // max ground speed, units/s
	float accel;        // units/s^2
	float radius;       // collision radius
	float sight;        // sees enemies within this distance
	float height;       // drawn height; the hit sphere sits radius above ground
	float hp;
	float range;        // max horizontal firing range
	int32_t reload;     // ticks between shots
	float shell_speed;  // muzzle speed, units/s
	float damage;       // to the unit hit
	float splash;       // splash radius, 0 for none
	float spread;       // aim jitter at the target point, units
	bool high_arc;      // lob shells (artillery) instead of firing flat
	int32_t weight;     // default spawn weight; scenarios override it
} UnitDef;

extern const UnitDef unit_defs[UNIT_TYPE_COUNT];

typedef enum : uint8_t {
	ORDER_IDLE,
	ORDER_MOVE,
	ORDER_ATTACK_MOVE,
	ORDER_KIND_COUNT,
} OrderKind;

typedef struct {
	uint64_t seed;
	int32_t initial_units;    // per faction, spawned at tick 0
	int32_t population;       // per faction; the spawner tops up to this
	int32_t spawn_interval;   // ticks between spawns per faction
	int32_t max_units;        // unit pool capacity per faction
	int32_t max_projectiles;  // incoming projectile capacity per faction
	int32_t type_weight[UNIT_TYPE_COUNT]; // relative spawn chance per type
} SimConfig;

SimConfig sim_config_default(void);

// One faction's units, struct-of-arrays, addressed by handles.
//
// Invariants:
//   0 <= count <= cap. Dense arrays are valid in [0, count).
//   For live dense i: slot_dense[dense_slot[i]] == i.
//   For a free slot s: slot_dense[s] == -1, and s is in free_slots.
//   A handle {s, g} is live iff slot_dense[s] >= 0 and slot_gen[s] == g.
//   Dense order changes only by swap-remove on death, so iteration order is
//   a deterministic function of the operations applied.
typedef struct {
	ptrdiff_t cap;
	ptrdiff_t count;

	uint32_t *slot_gen;
	int32_t *slot_dense;
	int32_t *free_slots;
	ptrdiff_t free_count;

	int32_t *dense_slot;
	float *px, *py, *pz;   // py is ground height, kept in step with px/pz
	float *vx, *vz;
	float *hp;
	UnitType *type;
	OrderKind *order;
	float *tx, *tz;        // order target
	int32_t *cooldown;     // ticks until the weapon can fire
	UnitHandle *target;    // enemy handle, may be stale
	uint8_t *engaged;      // had a target in range last tick
} Units;

// Shells flying toward a faction, simulated by that faction.
// Unordered; removal is swap-remove. 0 <= count <= cap.
typedef struct {
	ptrdiff_t cap;
	ptrdiff_t count;
	float *px, *py, *pz;
	float *vx, *vy, *vz;
	UnitType *weapon;      // the firing unit's type: damage, splash
	int32_t *ttl;          // ticks before it is discarded unexploded
	uint32_t *id;          // unique per target faction, in delivery order
	uint32_t next_id;
} Projectiles;

static constexpr float MAX_UNIT_RADIUS = 1.4f;
static constexpr int32_t PROJECTILE_TTL = 10 * 30;   // ticks a shell may fly

// What a faction knows of its enemies, by enemy slot: the units it currently
// sees, as quantized, dead-reckoned motion. Units are targeted and aimed at
// through this, never through enemy state, and the same bytes reach the
// faction's client, so both sides perceive identically.
typedef struct {
	ptrdiff_t cap;
	uint8_t *live;
	uint32_t *gen;
	UnitType *type;
	uint16_t *qx, *qz;
	int16_t *qvx, *qvz;
	int64_t *sent;
} Perception;

// Where a perceived unit is now, by dead reckoning.
static inline V2 perception_at(const Perception *p, ptrdiff_t s, Tick tick)
{
	return q_predict(p->qx[s], p->qz[s], p->qvx[s], p->qvz[s], (float)(tick.v - p->sent[s]) * SIM_DT);
}

typedef enum : uint8_t {
	SEEN_ENTER = 1,     // came into sight (or a new unit in a seen slot)
	SEEN_MOVE,          // the dead reckoning drifted: new motion
	SEEN_LOST,          // out of sight, or dead
} SeenTag;

typedef struct {
	SeenTag tag;
	UnitType type;
	uint16_t slot;
	uint16_t qx, qz;
	int16_t qvx, qvz;
	uint32_t gen;
} SeenEntry;

// A shot, crossing from the firing faction to the target faction.
typedef struct {
	V3 pos;
	V3 vel;
	UnitType weapon;
	FactionId target;
} Launch;

// A spatial grid over a set of points, rebuilt each time it is used.
// Items in a cell are in ascending index order.
typedef struct {
	float cell;            // cell edge length
	int32_t w, h;          // cells
	int32_t *start;        // w*h + 1 prefix offsets into items
	int32_t *items;        // indices into the gridded arrays
} Grid;

// Cells overlapping a rectangle, clamped to the grid; iterate with GRID_FOR.
static inline int32_t grid_coord(float v, float cell, int32_t n)
{
	int32_t c = (int32_t)(v / cell);
	return c < 0 ? 0 : c >= n ? n - 1 : c;
}

typedef struct {
	int32_t i0, i1, j0, j1;
} CellRange;

static inline CellRange grid_range(const Grid *g, float x0, float z0, float x1, float z1)
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

// The enemies a faction can target this tick: its Perception, dead-reckoned
// to now, with a grid for range queries. Built at the start of its phase.
typedef struct {
	ptrdiff_t count;
	float *px, *py, *pz;
	float *vx, *vz;
	UnitHandle *handle;
	float *radius;
	// Enemy slot -> index in this view, or -1; with the slot's generation.
	ptrdiff_t slot_cap;
	int32_t *slot_index;
	uint32_t *slot_gen;
	Grid grid;
} EnemyView;

typedef struct {
	int64_t shots_fired;
	int64_t units_spawned;
	int64_t units_lost;
	int64_t launches_dropped;  // arriving shells this faction's pool had no room for
} FactionStats;

// Everything a faction's phase takes from outside the faction in one tick,
// besides its commands. Built by the interaction phase on the server; sent to
// the faction's client, which feeds it to faction_tick.
typedef struct {
	ptrdiff_t seen_count;
	SeenEntry *seen;
	ptrdiff_t arrived_count;   // shells arriving, in delivery order
	Launch *arrived;
} FactionEffects;

typedef struct {
	FactionId id;
	V2 base;
	Rng rng;
	Units units;
	Projectiles incoming;
	Perception seen;           // its enemies, as it perceives them
	EnemyView view;            // built from `seen` each tick, in scratch
	Launch *outbox;            // launches fired this tick, delivered next tick
	ptrdiff_t outbox_count;
	ptrdiff_t outbox_cap;
	FactionStats stats;
	Arena scratch;             // tick lifetime, owned by this faction's phase
} Faction;

// What a command does. The first three order units; spawn creates them and
// is for scripts and setup only (admission will refuse it from players).
typedef enum : uint8_t {
	COMMAND_STOP,
	COMMAND_MOVE,
	COMMAND_ATTACK_MOVE,
	COMMAND_SPAWN,
	COMMAND_KIND_COUNT,
} CommandKind;

static constexpr int32_t SPAWN_MAX = 4096;      // units per spawn command
static constexpr float SPAWN_MAX_RADIUS = 200.0f;

// An input to the sim for one faction. Handles that are stale or not that
// faction's are ignored. Spawn uses unit_type, amount and radius around
// target, and no handles.
typedef struct {
	FactionId faction;
	CommandKind kind;
	V2 target;
	ptrdiff_t count;
	const UnitHandle *units;
	UnitType unit_type;
	int32_t amount;
	float radius;
} Command;

typedef struct {
	SimConfig config;
	Tick tick;
	Terrain terrain;
	Faction factions[FACTION_COUNT];
	FactionEffects effects[FACTION_COUNT]; // this tick's, until the next step
	Arena zone;                // zone lifetime: pools and terrain
	Arena interaction;         // tick lifetime: effects
} Sim;

void sim_init(Sim *sim, SimConfig config);
void sim_destroy(Sim *sim);

// Advance one tick, applying commands stamped for the current tick.
void sim_step(Sim *sim, const Command *commands, ptrdiff_t count);

// Hash of the state as it stands now (not a cached value).
Hash sim_hash_now(const Sim *sim);

// One faction's state: what its client must reproduce exactly.
Hash faction_hash(const Faction *f);

// A client's step: advance only faction f, from its commands and effects.
// The other faction's state is unused and may be empty.
void faction_tick(Sim *sim, FactionId f, const FactionEffects *fx, const Command *commands, ptrdiff_t count);

// A faction's whole state, for a client joining mid-game. faction_read
// treats its input as untrusted and checks every invariant.
void faction_write(const Faction *f, ByteBuf *b);
bool faction_read(Faction *f, ByteReader *r);

// Dense index of a live unit, or -1 if the handle is stale.
ptrdiff_t units_find(const Units *u, UnitHandle h);
UnitHandle units_handle(const Units *u, ptrdiff_t dense);

static inline FactionId faction_enemy(FactionId f) { return (FactionId){1 - f.v}; }

// One tick of shell flight: exact for constant gravity, so shells land where
// the aim solved. Clients fly shells with this too, from the server's start
// state, and get the server's positions bit for bit.
static inline void shell_advance(V3 *pos, V3 *vel)
{
	const float dt = SIM_DT;
	const float drop = 0.5f * SIM_GRAVITY * dt * dt;
	*pos = v3(pos->x + vel->x * dt, pos->y + vel->y * dt - drop, pos->z + vel->z * dt);
	vel->y -= SIM_GRAVITY * dt;
}

// Ballistic aim: the launch velocity that carries a shell from `from` to `to`
// at `speed` under gravity. Lob picks the high arc. False if out of reach.
bool ballistic_solve(V3 from, V3 to, float speed, bool lob, V3 *vel, float *time);
