// Named setups for automated play: a sim config plus how the bots behave.
// Clips, benchmarks and the game all start from one of these.
#pragma once

#include "bot.h"
#include "sim.h"

typedef struct {
	const char *name;
	const char *about;
	uint64_t seed;
	int32_t units;                         // per faction, start and population
	int32_t type_weight[COMBAT_TYPE_COUNT];
	int32_t bot_period;                    // ticks between bot decisions
	int32_t bot_group_size;                // units per bot attack command
	int64_t clip_from;                     // a good default clip window, ticks
	int64_t clip_to;
	bool economy;                          // build everything from a factory
} Scenario;

extern const Scenario scenarios[];
extern const ptrdiff_t scenario_count;

const Scenario *scenario_find(const char *name); // nullptr if unknown

SimConfig scenario_config(const Scenario *s);

// Bots for both factions, configured by the scenario.
void scenario_bots(const Scenario *s, Bot bots[FACTION_COUNT]);
