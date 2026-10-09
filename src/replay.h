// Recorded runs: the config, every admitted command per tick, and the state
// hash after each tick. A replay must reproduce every hash exactly; the first
// tick that does not is the divergence.
//
// Format, all little-endian:
//   header  "RTSR" u32 version (9), config (u64 seed, 5 x i32,
//           COMBAT_TYPE_COUNT x i32 spawn weights, u32 economy, u32 bases),
//           u32 snapshot length, snapshot (sim_save; empty: from the config)
//
// A replay with a snapshot is a segment: a resumed zone's run from where it
// picked up, verifiable on its own.
//   tick    u64 tick, u32 command count, commands, u64 hash after the step
//   command u32 faction, u32 kind, f32 x, f32 z, u32 unit type, u32 amount,
//           f32 radius, u32 flags, u32 route, u32 n, n x (u32 index, u32 gen)
#pragma once

#include "arena.h"
#include "hash.h"
#include "sim.h"

typedef struct {
	Arena *buf;           // the file grows here; it owns nothing else
	uint8_t *start;
	ptrdiff_t len;
} ReplayWriter;

void replay_begin(ReplayWriter *w, Arena *buf, SimConfig config);
void replay_begin_from(ReplayWriter *w, Arena *buf, SimConfig config, Str snapshot);
void replay_tick(ReplayWriter *w, Tick tick, const Command *cmds, ptrdiff_t count, Hash after);
Str replay_bytes(const ReplayWriter *w);

// Append what's been written since the last flush to f, then empty the
// buffer: a long-running zone streams its replay instead of holding it.
bool replay_flush(ReplayWriter *w, FILE *f);

typedef enum {
	REPLAY_OK,
	REPLAY_END,
	REPLAY_BAD_MAGIC,
	REPLAY_BAD_VERSION,
	REPLAY_TRUNCATED,
	REPLAY_BAD_CONFIG,
	REPLAY_BAD_COMMAND,
	REPLAY_TOO_LARGE,     // the decode arena could not hold it
} ReplayStatus;

const char *replay_status_name(ReplayStatus s);

// The file is untrusted input: every read is bounds-checked and every
// allocation can fail into a status instead of aborting.
typedef struct {
	Str data;
	ptrdiff_t pos;
	SimConfig config;
	Str snapshot;          // empty unless the replay is a segment
} ReplayReader;

// sim_init with the replay's config, then the snapshot if there is one.
bool replay_start(const ReplayReader *r, Sim *sim);

ReplayStatus replay_open(ReplayReader *r, Str data);

// Commands and their handle lists are allocated from arena.
ReplayStatus replay_next(ReplayReader *r, Arena *arena, Tick *tick,
                         Command **cmds, ptrdiff_t *count, Hash *after);
