// A bot plays one player's units in one faction by issuing commands, the
// same way a person in that slot would. Teammates (other slots, people or
// bots) are friendly: it never orders their units, and it keeps clear of
// sites and metal spots they're taking.
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

// How a bot plays: a doctrine. A preset by name, then any field by name:
//   "turtle army_cap=500 siege=3"
//   balanced  the default: a mixed army, crews of engineers, attacks what it sees
//   rush      raiders, early and often; sapper crews; small sends; hunts
//   turtle    tanks, artillery, siege; digs in at the front; assaults only in force
//   raider    raiders and scouts; three transporters; raids into enemy zones
//   siege     siege behind tanks; sapper crews; pushes once it has the guns
// Fields (unit names set the factory cycle: how many of each per round):
//   raider tank artillery siege   army_cap garrison send_min assault_army
//   crew_builder=engineer|sapper crew_builders crew_escort scouts transporters
//   entrench=on|off raids=on|off hunt=on|off
typedef struct {
	char name[16];
	int32_t mix[COMBAT_TYPE_COUNT];  // the factory cycle, per combat type
	int32_t army_cap;      // stops making army past this many in a zone
	int32_t garrison;      // kept back when sending; dug in, if it entrenches
	int32_t send_min;      // the smallest group worth sending
	int32_t assault_army;  // idle army needed before a crew goes into an enemy zone
	UnitType crew_builder; // engineer or sapper
	int32_t crew_builders, crew_escort; // a crew: builders, and raiders with them
	int32_t scouts;        // kept per zone
	int32_t transporters;  // per zone: more, more sends at once
	bool entrench;         // dig in the garrison in a zone at the front
	bool raids;            // send raiders unpaired into enemy zones
	bool hunt;             // go after an enemy seen lately
} Doctrine;

// False, with a message, on an unknown preset, field or value.
bool doctrine_parse(Doctrine *d, const char *spec, char *err, size_t err_len);

typedef struct {
	FactionId faction;
	PlayerSlot player;
	Rng rng;
	int32_t period;       // ticks between decisions
	int32_t group_size;   // units per attack command
	bool enabled;
	const BotWorld *world; // nullptr: one zone, nothing beyond it
	Doctrine doctrine;
} Bot;

Bot bot_create(FactionId faction, uint64_t seed);     // plays player 0
Bot bot_create_player(FactionId faction, PlayerSlot player, uint64_t seed);

// Appends up to cap - *count commands to out; handle lists come from arena.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap);
