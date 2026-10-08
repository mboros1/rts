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

// Directions around the base to try sites in, as exact constants: bots run
// inside the tick, so they stay off libm too.
static const V2 ring[8] = {
	{1.0f, 0.0f}, {0.70710678f, 0.70710678f}, {0.0f, 1.0f}, {-0.70710678f, 0.70710678f},
	{-1.0f, 0.0f}, {-0.70710678f, -0.70710678f}, {0.0f, -1.0f}, {0.70710678f, -0.70710678f},
};

static bool emit(Command *out, ptrdiff_t *count, ptrdiff_t cap, Command c)
{
	if (*count == cap) return false;
	out[(*count)++] = c;
	return true;
}

// Sites engineers are already walking to: not built yet, so placement_ok
// can't see them, but promised all the same.
typedef struct {
	int n;
	V2 at[64];
	UnitType type[64];
} Promised;

static bool clear_of_promises(const Promised *p, UnitType type, V2 at)
{
	for (int k = 0; k < p->n; k++) {
		float min_d = unit_defs[type].radius + unit_defs[p->type[k]].radius + 1.0f;
		if (v2_len2(v2_sub(p->at[k], at)) < min_d * min_d) return false;
	}
	return true;
}

static void promise(Promised *p, UnitType type, V2 at)
{
	if (p->n == (int)countof(p->at)) return;
	p->at[p->n] = at;
	p->type[p->n++] = type;
}

// A site near the base where `type` fits, spiralling outward.
static bool site_near_base(const Sim *sim, FactionId f, UnitType type, const Promised *promised, V2 *out)
{
	V2 base = sim->factions[f.v].base;
	for (int i = 0; i < 64; i++) {
		float r = 16.0f + 7.0f * (float)(i / 8);
		V2 at = v2_add(base, v2_scale(ring[(i + 3 * (i / 8)) % 8], r));
		if (placement_ok(sim, f, type, at) && clear_of_promises(promised, type, at)) {
			*out = at;
			return true;
		}
	}
	return false;
}

static bool queue_has(const Units *u, ptrdiff_t d, UnitType t)
{
	for (int q = 0; q < u->queue_len[d]; q++)
		if ((UnitType)(u->queue[d] >> (8 * q) & 0xff) == t) return true;
	return false;
}

// An economy, kept simply: factories cycle a mix of units (with engineers
// while there are too few); idle engineers take the most useful job.
static void bot_economy(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	FactionId f = bot->faction;
	const Faction *me = &sim->factions[f.v];
	const Units *u = &me->units;
	const Economy *e = &me->economy;
	int engineers = 0, factories = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		engineers += u->type[i] == UNIT_ENGINEER;
		factories += u->type[i] == UNIT_FACTORY;
	}
	int want_engineers = 3 + (int)(e->metal_income / 6.0f);
	if (want_engineers > 8) want_engineers = 8;

	// Opening: factories make engineers and nothing else until there's metal
	// coming in; then a cycle of units, with engineers while there are few.
	bool opening = e->metal_income < 12.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_FACTORY || u->progress[i] < 1.0f) continue;
		bool short_handed = engineers < want_engineers;
		bool want_army = !opening;
		bool has_army = queue_has(u, i, UNIT_RAIDER);
		if (u->queue_len[i] > 0 && queue_has(u, i, UNIT_ENGINEER) == short_handed && has_army == want_army) continue;
		if (u->queue_len[i] == 0 && !short_handed && !want_army) continue;
		UnitHandle *h = PUSH(arena, UnitHandle, 1);
		*h = units_handle(u, i);
		Command c = {.faction = f, .kind = COMMAND_PRODUCE, .count = 1, .units = h, .flags = COMMAND_REPEAT};
		const struct { UnitType type; int32_t amount; } mix[] = {
			{UNIT_ENGINEER, short_handed ? 1 : 0},
			{UNIT_RAIDER, want_army ? 2 : 0},
			{UNIT_TANK, want_army ? 1 : 0},
			{UNIT_ARTILLERY, want_army ? 1 : 0},
		};
		c.amount = 0; // clear, then queue the cycle
		emit(out, count, cap, c);
		for (int k = 0; k < (int)countof(mix); k++) {
			if (!mix[k].amount) continue;
			c.unit_type = mix[k].type;
			c.amount = mix[k].amount;
			emit(out, count, cap, c);
		}
	}

	// Spots we hold or stand on or are walking to, and sites we're walking to.
	uint64_t taken = e->spots;
	Promised promised = {0};
	for (ptrdiff_t i = 0; i < u->count; i++) {
		V2 at = v2(u->px[i], u->pz[i]);
		if (u->type[i] == UNIT_ENGINEER && u->order[i] == ORDER_BUILD) {
			at = v2(u->tx[i], u->tz[i]);
			promise(&promised, u->build_type[i], at);
		} else if (u->type[i] != UNIT_EXTRACTOR) {
			continue;
		}
		int32_t k = spot_at(&sim->spots, at);
		if (k >= 0) taken |= (uint64_t)1 << k;
	}
	int want_factories = 1 + (int)(e->metal_income / 10.0f);
	if (want_factories > 4) want_factories = 4;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_ENGINEER || u->order[i] != ORDER_IDLE) continue;
		V2 p = v2(u->px[i], u->pz[i]);
		// The nearest free metal spot, and whether it's within reach of home.
		int32_t spot = -1;
		float best = 1e30f;
		for (int32_t k = 0; k < sim->spots.count; k++) {
			if (taken >> k & 1) continue;
			float d2 = v2_len2(v2_sub(sim->spots.at[k], p));
			if (d2 < best && placement_ok(sim, f, UNIT_EXTRACTOR, sim->spots.at[k])) {
				best = d2;
				spot = k;
			}
		}
		bool near = spot >= 0 && v2_len(v2_sub(sim->spots.at[spot], me->base)) < 220.0f;
		bool power_short = e->energy < 2000.0f && e->energy_income < 1.1f * e->energy_wanted;
		bool rich = e->metal > 400.0f && factories < want_factories;

		// Power first, then spend a surplus on a factory, then nearby metal,
		// then more factories or power, and far metal last.
		UnitType type;
		if (power_short) type = UNIT_GENERATOR;
		else if (rich) type = UNIT_FACTORY;
		else if (near) type = UNIT_EXTRACTOR;
		else if (factories < want_factories && e->metal > 250.0f) type = UNIT_FACTORY;
		else if (spot < 0 && e->energy > 0.5f * e->energy_cap) continue; // nothing worth building
		else if (e->energy_income < 1.5f * e->energy_wanted || spot < 0) type = UNIT_GENERATOR;
		else type = UNIT_EXTRACTOR;

		V2 site;
		if (type == UNIT_EXTRACTOR) {
			site = sim->spots.at[spot];
			taken |= (uint64_t)1 << spot;
		} else if (!site_near_base(sim, f, type, &promised, &site)) {
			continue;
		}
		promise(&promised, type, site);
		factories += type == UNIT_FACTORY;
		UnitHandle *h = PUSH(arena, UnitHandle, 1);
		*h = units_handle(u, i);
		emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_BUILD, .target = site, .count = 1,
		                                .units = h, .unit_type = type});
	}
}

// Every period: run the economy if there is one, and send idle combat units
// in groups to attack-move on the enemy base from a random angle.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	if (!bot->enabled) return;
	if ((sim->tick.v + bot->faction.v * 15) % bot->period != 0) return;
	if (sim->config.economy) bot_economy(bot, sim, arena, out, count, cap);
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
			out[(*count)++] = (Command){.faction = bot->faction, .kind = COMMAND_ATTACK_MOVE,
			                             .target = target, .count = in_group, .units = group};
			in_group = 0;
		}
		if (i == u->count) break;
		if (u->order[i] != ORDER_IDLE || u->engaged[i] || !unit_is_combat(u->type[i])) continue;
		if (in_group == 0) group = PUSH(arena, UnitHandle, bot->group_size);
		group[in_group++] = units_handle(u, i);
	}
}
