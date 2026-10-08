#include "scenario.h"

const Scenario scenarios[] = {
	{"battle", "5000 a side, mixed arms, waves of 128",
	 1, 5000, {6, 3, 2}, 30, 128, 450, 1350, false},
	{"skirmish", "400 a side in small groups, readable up close",
	 2, 400, {6, 3, 2}, 20, 24, 300, 900, false},
	{"artillery", "1500 a side, artillery heavy: long lobbed shells",
	 3, 1500, {2, 2, 6}, 45, 96, 450, 1350, false},
	{"raiders", "2500 a side, raiders only: fast swarms",
	 4, 2500, {1, 0, 0}, 15, 64, 240, 900, false},
	{"economy", "a factory and two engineers each: expand, produce, attack",
	 5, 0, {6, 3, 2}, 30, 24, 0, 5400, true},
};

const ptrdiff_t scenario_count = countof(scenarios);

const Scenario *scenario_find(const char *name)
{
	for (ptrdiff_t i = 0; i < scenario_count; i++)
		if (!strcmp(scenarios[i].name, name)) return &scenarios[i];
	return nullptr;
}

SimConfig scenario_config(const Scenario *s)
{
	ASSERT(s); // scenario_find returns nullptr for unknown names
	SimConfig c = sim_config_default();
	c.seed = s->seed;
	c.initial_units = s->units;
	c.population = s->units;
	if (c.max_units < s->units) c.max_units = s->units;
	for (int k = 0; k < COMBAT_TYPE_COUNT; k++) c.type_weight[k] = s->type_weight[k];
	c.economy = s->economy;
	return c;
}

void scenario_bots(const Scenario *s, Bot bots[FACTION_COUNT])
{
	ASSERT(s);
	for (int k = 0; k < FACTION_COUNT; k++) {
		bots[k] = bot_create((FactionId){(uint32_t)k}, s->seed);
		bots[k].period = s->bot_period;
		bots[k].group_size = s->bot_group_size;
	}
}
