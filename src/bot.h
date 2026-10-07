// A bot plays one faction by issuing commands, the same way a player does.
//
// It is not part of the sim: it reads sim state and produces commands, which
// enter the same stream as the player's and are recorded with them. Replays
// feed the recorded commands back and never run bots, so bot state is not
// in the sim hash.
#pragma once

#include "arena.h"
#include "rng.h"
#include "sim.h"

typedef struct {
	FactionId faction;
	Rng rng;
	int32_t period;       // ticks between decisions
	int32_t group_size;   // units per attack command
	bool enabled;
} Bot;

Bot bot_create(FactionId faction, uint64_t seed);

// Appends up to cap - *count commands to out; handle lists come from arena.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap);
