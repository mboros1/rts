// A bot plays one faction by issuing commands, the same way a player does.
//
// In a world it also plays the map, every STRATEGY_PERIOD ticks: it builds a
// transporter where its side has none, opens empty neighbouring zones with an
// unpaired crew (engineers and raiders), and sends what its zone can spare,
// paired, to where its side is under most pressure or nearest the enemy (the
// zone server finds the way). Within a zone it attacks only once it sees an
// enemy; otherwise its army waits for a send.
//
// It is not part of the sim: it reads sim state and produces commands, which
// enter the same stream as the player's and are recorded with them. Replays
// feed the recorded commands back and never run bots, so bot state is not
// in the sim hash.
#pragma once

#include "arena.h"
#include "rng.h"
#include "sim.h"

// What a bot knows of the world beyond its zone, through its side's fog,
// from the coordinator's directory (world.h): the zones linked to this one,
// where its side has transporters, its own strength in each zone (units, and
// twice the structures), and what it has seen of the enemy: contact now, how
// long since it last saw them, and since it saw an enemy transporter. No
// enemy numbers. Kept current by whoever runs the bot.
enum : uint32_t { BOT_NEVER = 0xffffffffu };   // an age: never seen
typedef struct {
	int count, here;
	bool linked[ZONE_MAX];     // to here
	bool ours[ZONE_MAX];       // our transporter there: a paired send can go
	bool home[ZONE_MAX];       // our home zone
	int32_t mine[ZONE_MAX];
	bool contact[ZONE_MAX];    // we see them there now
	uint32_t seen_age[ZONE_MAX], transporter_age[ZONE_MAX]; // game seconds, or BOT_NEVER
	bool neighbours_enemy[ZONE_MAX]; // linked to a zone we believe is theirs
} BotWorld;

// Do we believe the enemy holds zone z: seen there lately, or a transporter of
// theirs seen there not too long ago? Stale sightings count for less, then
// not at all.
static inline bool bot_believes_enemy(const BotWorld *w, int z)
{
	return w->contact[z] || w->seen_age[z] < 600 || w->transporter_age[z] < 1800;
}

typedef struct {
	FactionId faction;
	Rng rng;
	int32_t period;       // ticks between decisions
	int32_t group_size;   // units per attack command
	bool enabled;
	const BotWorld *world; // nullptr: one zone, nothing beyond it
} Bot;

Bot bot_create(FactionId faction, uint64_t seed);

// Appends up to cap - *count commands to out; handle lists come from arena.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap);
