#include "bot.h"

Bot bot_create(FactionId faction, uint64_t seed)
{
	return (Bot){
		.faction = faction,
		.rng = rng_seed(seed ^ (0xb07u + faction.v)),
		.period = 30,
		.group_size = 128,
		.enabled = true,
	};
}

// Every period, idle units are sent in groups to attack-move on the enemy
// base from a random angle of approach.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	if (!bot->enabled) return;
	if ((sim->tick.v + bot->faction.v * 15) % bot->period != 0) return;
	const Faction *f = &sim->factions[bot->faction.v];
	const Faction *enemy = &sim->factions[faction_enemy(bot->faction).v];
	const Units *u = &f->units;

	UnitHandle *group = nullptr;
	ptrdiff_t in_group = 0;
	for (ptrdiff_t i = 0; i <= u->count; i++) {
		bool flush = in_group > 0 && (i == u->count || in_group == bot->group_size);
		if (flush) {
			if (*count == cap) return;
			V2 target = v2(enemy->base.x + rng_range(&bot->rng, -40.0f, 40.0f),
			               enemy->base.z + rng_range(&bot->rng, -120.0f, 120.0f));
			out[(*count)++] = (Command){bot->faction, ORDER_ATTACK_MOVE, target, in_group, group};
			in_group = 0;
		}
		if (i == u->count) break;
		if (u->order[i] != ORDER_IDLE || u->engaged[i]) continue;
		if (in_group == 0) group = PUSH(arena, UnitHandle, bot->group_size);
		group[in_group++] = units_handle(u, i);
	}
}
