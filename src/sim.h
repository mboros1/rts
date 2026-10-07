// The simulation: a pure function from state + commands to the next state.
//
// State is laid out by faction from the start (docs/roadmap.md, spike 3).
// Each tick has two phases:
//
//   interaction — the only code that reads more than one faction. Delivers
//                 last tick's launches to their target faction and builds
//                 each faction's view of its enemies.
//   faction     — per faction, reading only that faction's state, its
//                 commands, its incoming projectiles and its enemy view.
//                 Moves units, flies incoming shells against its own units,
//                 aims and fires into its outbox.
//
// Nothing here includes raylib, reads a clock or touches a file.
#pragma once

#include "arena.h"
#include "base.h"
#include "hash.h"
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
} Projectiles;

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

// What a faction may know about its enemies this tick. Built by the
// interaction phase; spike 3 is where it stops containing everything.
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
	int64_t launches_dropped;  // target faction's projectile pool was full
} FactionStats;

typedef struct {
	FactionId id;
	V2 base;
	Rng rng;
	Units units;
	Projectiles incoming;
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
	EnemyView views[FACTION_COUNT];
	Arena zone;                // zone lifetime: pools and terrain
	Arena interaction;         // tick lifetime: views
} Sim;

void sim_init(Sim *sim, SimConfig config);
void sim_destroy(Sim *sim);

// Advance one tick, applying commands stamped for the current tick.
void sim_step(Sim *sim, const Command *commands, ptrdiff_t count);

// Hash of the state as it stands now (not a cached value).
Hash sim_hash_now(const Sim *sim);

// Dense index of a live unit, or -1 if the handle is stale.
ptrdiff_t units_find(const Units *u, UnitHandle h);
UnitHandle units_handle(const Units *u, ptrdiff_t dense);

static inline FactionId faction_enemy(FactionId f) { return (FactionId){1 - f.v}; }

// Ballistic aim: the launch velocity that carries a shell from `from` to `to`
// at `speed` under gravity. Lob picks the high arc. False if out of reach.
bool ballistic_solve(V3 from, V3 to, float speed, bool lob, V3 *vel, float *time);
