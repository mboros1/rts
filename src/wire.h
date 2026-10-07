// The match stream between server and clients. One schema, both ends.
//
// The server keeps a Mirror of what it has told clients; every client keeps
// the same Mirror built from what it received. Each tick the server diffs the
// sim against its mirror and sends only what a client couldn't predict:
//
//   units   quantized position and velocity, resent only when the actual
//           position drifts from the dead-reckoned one, or velocity changes;
//           hp when it changes; spawns and deaths.
//   shells  start and end only. Clients fly shells with shell_advance from
//           the server's start state and get its positions bit for bit.
//
// Every DIGEST_EVERY ticks a delta carries the hash of the server's mirror;
// a client comparing it against its own proves the two agree, including
// every shell's position.
//
// That mirror is for spectators, who see everything. A player gets the
// faction stream instead and simulates its own faction (spike 3):
//
//   FACTION_SNAPSHOT  the faction's whole state (faction_write), to start from
//   FACTION_TICK      one step's inputs for that faction: its commands, what
//                     it perceives (SeenEntry), the shells arriving at it, and
//                     every DIGEST_EVERY ticks its faction_hash after the step
//
// Nothing in the faction stream describes an enemy the faction can't see.
//
// Frames (net.h) carry: u32 length, u8 kind, payload. Payloads, little-endian:
//   HELLO     u32 version, u8 faction (0, 1, or SPECTATOR)
//   WELCOME   u32 version, u8 faction, config: u64 seed, 5 x i32, 3 x i32 weights,
//             per faction: f32 base x, f32 base z
//   SNAPSHOT  body; replaces the client's mirror
//   DELTA     body for exactly the next tick
//   COMMAND   u8 kind, f32 x, f32 z, u32 n, n x (u32 slot, u32 generation)
//   FACTION_SNAPSHOT  i64 tick, faction_write
//   FACTION_TICK      i64 tick, u16 dilation, u8 has hash, [u64 hash],
//             u32 n, n x command: u8 kind, f32 x, f32 z, u8 unit type,
//                                 i32 amount, f32 radius, u32 m, m x handle
//             u32 n, n x seen: u8 tag, u16 slot, then ENTER: u32 gen, u8 type,
//                              u16 x, u16 z, i16 vx, i16 vz; MOVE: the last 4
//             u32 n, n x arrival: f32 x, y, z, vx, vy, vz, u8 weapon
//   PING/PONG f64 sender's clock, echoed
//   body      i64 tick, u16 dilation (per mille), u8 has digest, [u64 digest],
//             per faction: u32 n, n unit entries,
//             per faction: u32 n, n shell starts, u32 m, m x u32 shell id
//   unit      u8 tag, u16 slot, then by tag:
//               SPAWN  u32 gen, u8 type, u16 x, u16 z, i16 vx, i16 vz, u16 hp, u16 age
//               MOVE   u16 x, u16 z, i16 vx, i16 vz
//               HP     u16 hp
//               DEATH  (nothing)
//   shell     u32 id, f32 x, y, z, vx, vy, vz, u8 weapon
#pragma once

#include "arena.h"
#include "bytes.h"
#include "hash.h"
#include "idmap.h"
#include "quant.h"
#include "sim.h"

enum { WIRE_VERSION = 1, DIGEST_EVERY = 30 };
static constexpr uint8_t SPECTATOR = 255;

typedef enum : uint8_t {
	MSG_HELLO = 1,
	MSG_WELCOME,
	MSG_SNAPSHOT,
	MSG_DELTA,
	MSG_COMMAND,
	MSG_PING,
	MSG_PONG,
	MSG_FACTION_SNAPSHOT,
	MSG_FACTION_TICK,
	MSG_KIND_COUNT,
} MsgKind;

static inline uint16_t q_hp(float hp) { return (uint16_t)(f_clamp(hp, 0.0f, 4095.0f) * 16.0f + 0.5f); }

// One faction's units as the mirror knows them, indexed by slot.
typedef struct {
	ptrdiff_t cap;
	uint8_t *live;
	uint32_t *gen;
	UnitType *type;
	uint16_t *qx, *qz, *qhp;
	int16_t *qvx, *qvz;
	int64_t *sent;          // tick qx..qvz were sent; prediction runs from here
} MirrorUnits;

// Shells flying at one faction, dense, found by id.
typedef struct {
	ptrdiff_t cap;
	ptrdiff_t count;
	IdMap index;
	uint32_t *id;
	float *px, *py, *pz, *vx, *vy, *vz;
	UnitType *weapon;
	int64_t *seen;          // server only: last tick the sim had it
} MirrorShells;

typedef struct {
	int64_t tick;
	uint16_t dilation;      // per mille: 1000 is real time
	MirrorUnits units[FACTION_COUNT];
	MirrorShells shells[FACTION_COUNT];
} Mirror;

void mirror_init(Mirror *m, Arena *a, ptrdiff_t max_units, ptrdiff_t max_shells);
void mirror_clear(Mirror *m);
Hash mirror_digest(const Mirror *m);

// Dead-reckoned position of a unit at tick.
static inline V2 mirror_predict(const MirrorUnits *u, ptrdiff_t slot, int64_t tick)
{
	return q_predict(u->qx[slot], u->qz[slot], u->qvx[slot], u->qvz[slot], (float)(tick - u->sent[slot]) * SIM_DT);
}

typedef struct {
	int64_t units_sent;     // entries of any kind
	int64_t shells_started;
	int64_t shells_ended;
} WireStats;

// Server: bring the mirror up to the sim's tick and write the delta body.
void wire_encode_tick(Mirror *m, const Sim *sim, ByteBuf *out, bool digest, uint16_t dilation,
                      WireStats *stats);

// Server: the whole mirror as a snapshot body.
void wire_encode_snapshot(const Mirror *m, ByteBuf *out);

typedef enum {
	APPLY_OK,
	APPLY_BAD,              // malformed: drop the connection
	APPLY_GAP,              // not the next tick: resync
} ApplyResult;

typedef struct {
	bool had_digest;
	bool digest_matched;
} ApplyInfo;

// Client: apply a snapshot or delta body. Untrusted input: every field is
// range-checked, and a bad message leaves the mirror for a resync.
ApplyResult wire_apply(Mirror *m, ByteReader *r, bool snapshot, ApplyInfo *info);

// Server, after sim_step: faction f's inputs for the step just taken.
void wire_encode_faction_tick(const Sim *sim, FactionId f, const Command *cmds, ptrdiff_t count,
                              bool hash, uint16_t dilation, ByteBuf *out);
void wire_encode_faction_snapshot(const Sim *sim, FactionId f, ByteBuf *out);

typedef struct {
	int64_t tick;
	uint16_t dilation;
	bool has_hash;
	Hash hash;
	Command *commands;
	ptrdiff_t command_count;
	FactionEffects effects;
} FactionTick;

// Client: decode one step's inputs, allocating from arena. Untrusted input:
// false on anything malformed or out of range for this config.
bool wire_read_faction_tick(ByteReader *r, Arena *a, const SimConfig *c, FactionId f, FactionTick *out);

void wire_put_config(ByteBuf *b, const SimConfig *c);
bool wire_get_config(ByteReader *r, SimConfig *c);   // false if out of range
