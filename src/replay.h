// Recorded runs: the config, every admitted command per tick, and the state
// hash after each tick. A replay must reproduce every hash exactly; the first
// tick that does not is the divergence.
//
// Format, all little-endian:
//   header  "RTSR" u32 version (2), config (u64 seed, 5 x i32,
//           UNIT_TYPE_COUNT x i32 spawn weights)
//   tick    u64 tick, u32 command count, commands, u64 hash after the step
//   command u32 faction, u32 kind, f32 x, f32 z, u32 n, n x (u32 index, u32 gen)
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
void replay_tick(ReplayWriter *w, Tick tick, const Command *cmds, ptrdiff_t count, Hash after);
Str replay_bytes(const ReplayWriter *w);

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
} ReplayReader;

ReplayStatus replay_open(ReplayReader *r, Str data);

// Commands and their handle lists are allocated from arena.
ReplayStatus replay_next(ReplayReader *r, Arena *arena, Tick *tick,
                         Command **cmds, ptrdiff_t *count, Hash *after);
