// A player's world, rebuilt from the faction stream (wire.h): its own
// faction simulated locally with faction_tick, its enemies as perceived.
//
// Used by the network client and by clips of its netlog, so both see exactly
// what the player saw. The faction's hash is checked against the server's
// whenever a tick carries one.
//
// One part is for drawing only: the faction's own shells after they leave
// its outbox. They fly in the enemy's state, which the player never gets, so
// they're flown here from the launch and dropped when they meet the ground or
// a perceived enemy. Nothing simulated depends on them.
#pragma once

#include "arena.h"
#include "scene.h"
#include "sim.h"
#include "wire.h"

typedef struct {
	ptrdiff_t cap, count;
	float *px, *py, *pz, *vx, *vy, *vz;
	UnitType *weapon;
	int32_t *ttl;
} OutgoingShells;

typedef struct {
	SimConfig config;
	FactionId faction;
	Sim sim;
	bool ready;                  // has a snapshot
	Arena scratch;               // one tick's decoded inputs
	OutgoingShells outgoing;
	uint16_t dilation;
	int64_t hashes, hash_mismatches;
} Remote;

void remote_init(Remote *r, const SimConfig *config, FactionId faction, Arena *program);
void remote_destroy(Remote *r);

// Untrusted input. False leaves the remote not ready, for a resync.
bool remote_snapshot(Remote *r, ByteReader *in);

typedef enum {
	REMOTE_OK,
	REMOTE_BAD,          // malformed: drop the connection
	REMOTE_GAP,          // not the next tick: resync
	REMOTE_DIVERGED,     // the hash didn't match: a bug, then resync
} RemoteResult;

RemoteResult remote_tick(Remote *r, ByteReader *in, bool *had_hash);

// Own units exact; enemies perceived, dead-reckoned to now; shells arriving
// exact; own shells outgoing as flown here.
Scene remote_scene(const Remote *r, const V2 base[FACTION_COUNT], Arena *arena);

// Which terrain vertices the faction can see: one byte per heightfield sample.
void remote_fog(const Remote *r, uint8_t *visible);
