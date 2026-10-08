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

// What a bot knows of the world beyond its zone, from the coordinator's
// directory (world.h): the zones linked to this one, where its side has
// transporters, and each side's strength in each zone (units, and twice the
// structures). Kept current by whoever runs the bot.
typedef struct {
	int count, here;
	bool linked[ZONE_MAX];     // to here
	bool ours[ZONE_MAX];       // our transporter there: a paired send can go
	bool home[ZONE_MAX];       // our home zone
	int32_t mine[ZONE_MAX], theirs[ZONE_MAX];
	bool neighbours_enemy[ZONE_MAX]; // linked to a zone where they are
} BotWorld;

typedef struct {
	FactionId faction;
	Rng rng;
	int32_t period;       // ticks between decisions
	int32_t group_size;   // units per attack command
	bool enabled;
	const BotWorld *world; // nullptr: one zone, nothing beyond it
	int64_t enemy_since;   // tick the enemy was first known in this zone; -1: not here
} Bot;

Bot bot_create(FactionId faction, uint64_t seed);

// Appends up to cap - *count commands to out; handle lists come from arena.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap);
