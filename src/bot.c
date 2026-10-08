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

static bool own_transporter(const Units *u, bool finished_only)
{
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (u->type[i] == UNIT_TRANSPORTER && (u->progress[i] >= 1.0f || !finished_only)) return true;
	return false;
}

enum { BOT_ARMY_CAP = 300, SCOUTS_WANTED = 2 };

// An economy, kept simply: factories cycle a mix of units (with engineers
// while there are too few); idle engineers take the most useful job.
static void bot_economy(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	FactionId f = bot->faction;
	const Faction *me = &sim->factions[f.v];
	const Units *u = &me->units;
	const Economy *e = &me->economy;
	int engineers = 0, factories = 0, army = 0, scouts = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		engineers += u->type[i] == UNIT_ENGINEER;
		scouts += u->type[i] == UNIT_SCOUT;
		factories += u->type[i] == UNIT_FACTORY;
		army += unit_is_combat(u->type[i]);
	}
	int want_engineers = 3 + (int)(e->metal_income / 6.0f);
	if (want_engineers > 8) want_engineers = 8;

	// Opening: factories make engineers and nothing else until there's metal
	// coming in; then a cycle of units, with engineers while there are few.
	bool opening = e->metal_income < 12.0f;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_FACTORY || u->progress[i] < 1.0f) continue;
		bool short_handed = engineers < want_engineers;
		bool want_scout = bot->world && scouts < SCOUTS_WANTED;
		bool want_army = !opening && army < BOT_ARMY_CAP; // enough here: spend it first
		bool has_army = queue_has(u, i, UNIT_RAIDER);
		if (u->queue_len[i] > 0 && queue_has(u, i, UNIT_ENGINEER) == short_handed && has_army == want_army
		    && queue_has(u, i, UNIT_SCOUT) == want_scout)
			continue;
		if (u->queue_len[i] == 0 && !short_handed && !want_army && !want_scout) continue;
		UnitHandle *h = PUSH(arena, UnitHandle, 1);
		*h = units_handle(u, i);
		Command c = {.faction = f, .kind = COMMAND_PRODUCE, .count = 1, .units = h, .flags = COMMAND_REPEAT};
		const struct { UnitType type; int32_t amount; } mix[] = {
			{UNIT_ENGINEER, short_handed ? 1 : 0},
			{UNIT_SCOUT, want_scout ? 1 : 0},
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
	// In a world, a zone with no transporter of ours yet: the engineers are
	// for that (bot_strategy), before anything else.
	if (bot->world && !own_transporter(u, false)) return;
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

enum { STRATEGY_PERIOD = 300, GARRISON = 10, SEND_MIN = 12, CREW_ENGINEERS = 4, CREW_RAIDERS = 4, ASSAULT_ARMY = 40,
       FRONT_MARGIN = 100, HUNT_RECENT = 120, RECON_STALE = 300 };


// How far forward a zone of ours is: where we see the enemy, then where
// we've seen a transporter of theirs, then next to a zone we believe is
// theirs; the weaker we are there, the more it needs.
static int32_t front_score(const BotWorld *w, int z)
{
	return (w->contact[z] ? 100000 : 0) + (w->transporter_age[z] < 1800 ? 80000 : 0)
	     + (w->neighbours_enemy[z] ? 50000 : 0) - w->mine[z];
}

// The map: a transporter here, crews to open empty neighbours, surplus to
// where it's needed. Sends name the destination only; the server routes.
static void bot_strategy(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	const BotWorld *w = bot->world;
	FactionId f = bot->faction;
	const Faction *me = &sim->factions[f.v];
	const Units *u = &me->units;
	const Economy *e = &me->economy;
	int here = w->here;
	bool sending = false;
	int32_t idle_army = 0, idle_engineers = 0, scouts = 0;
	ptrdiff_t idle_scout = -1;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		sending = sending || u->order[i] == ORDER_SEND;
		bool idle = u->order[i] == ORDER_IDLE && !u->engaged[i];
		idle_army += idle && unit_is_combat(u->type[i]);
		idle_engineers += idle && u->type[i] == UNIT_ENGINEER;
		scouts += u->type[i] == UNIT_SCOUT;
		if (u->type[i] == UNIT_SCOUT && u->order[i] != ORDER_SEND) idle_scout = i; // patrolling will do
	}

	// No transporter here: build one, with an idle engineer, once it's affordable.
	if (!own_transporter(u, false)) {
		if (idle_engineers == 0 || e->metal < 0.5f * unit_defs[UNIT_TRANSPORTER].metal_cost) return;
		Promised none = {0};
		V2 site;
		if (!site_near_base(sim, f, UNIT_TRANSPORTER, &none, &site)) return;
		// Every idle engineer on it: the sooner it's up, the sooner help comes.
		UnitHandle *h = PUSH(arena, UnitHandle, idle_engineers);
		int32_t n = 0;
		for (ptrdiff_t i = 0; i < u->count && n < idle_engineers; i++)
			if (u->type[i] == UNIT_ENGINEER && u->order[i] == ORDER_IDLE && !u->engaged[i]) h[n++] = units_handle(u, i);
		emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_BUILD, .target = site, .count = n,
		                                .units = h, .unit_type = UNIT_TRANSPORTER});
		return;
	}
	if (!own_transporter(u, true) || sending) return; // one send at a time

	// Open a neighbour where we have no transporter: a crew of engineers to
	// build one, with raiders to guard them. One the enemy isn't in first;
	// one they are in only with an army here to follow the crew in (it lands
	// in our strip, perhaps unseen, and has to get the transporter up).
	float crew_energy = UNPAIRED_ENERGY + UNPAIRED_ENERGY_PER * (CREW_ENGINEERS + CREW_RAIDERS);
	if (idle_engineers > CREW_ENGINEERS && e->energy >= crew_energy) {
		int target = -1;
		for (int z = 0; z < w->count; z++)
			if (z != here && w->linked[z] && !w->ours[z] && !bot_believes_enemy(w, z) && (target < 0 || rng_below(&bot->rng, 2)))
				target = z;
		for (int z = 0; z < w->count && target < 0 && idle_army >= ASSAULT_ARMY; z++)
			if (z != here && w->linked[z] && !w->ours[z]) target = z;
		if (target >= 0) {
			UnitHandle *h = PUSH(arena, UnitHandle, CREW_ENGINEERS + CREW_RAIDERS);
			int32_t n = 0, eng = 0, rai = 0;
			for (ptrdiff_t i = 0; i < u->count; i++) {
				if (u->order[i] != ORDER_IDLE || u->engaged[i]) continue;
				if (u->type[i] == UNIT_ENGINEER && eng < CREW_ENGINEERS) eng++, h[n++] = units_handle(u, i);
				else if (u->type[i] == UNIT_RAIDER && rai < CREW_RAIDERS) rai++, h[n++] = units_handle(u, i);
			}
			emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_SEND, .amount = target, .count = n, .units = h});
			return;
		}
	}

	// Recon: a scout, unpaired, to a neighbour we know nothing fresh about
	// and hold no transporter in. One stays home.
	if (idle_scout >= 0 && scouts > 1 && e->energy >= UNPAIRED_ENERGY + UNPAIRED_ENERGY_PER) {
		for (int z = 0; z < w->count; z++) {
			if (z == here || !w->linked[z] || w->ours[z] || w->contact[z]) continue;
			if (w->seen_age[z] != BOT_NEVER && w->seen_age[z] < RECON_STALE) continue;
			UnitHandle *h = PUSH(arena, UnitHandle, 1);
			*h = units_handle(u, idle_scout);
			emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_SEND, .amount = z, .count = 1, .units = h});
			return;
		}
	}

	// Send what can be spared toward the fight: a zone of ours where the
	// enemy is (a beachhead, or one under attack) first, then one next to
	// them; only ever to a zone further forward than this one, so a front
	// holds and a beachhead draws the army behind it.
	if (w->contact[here] && !w->neighbours_enemy[here]) return; // a fight here: everyone stays
	int32_t spare = idle_army - GARRISON;
	if (spare < SEND_MIN) return;
	int target = -1;
	int32_t best = front_score(w, here) + FRONT_MARGIN; // clearly further forward, or stay
	for (int z = 0; z < w->count; z++) {
		if (z == here || !w->ours[z]) continue;
		int32_t score = front_score(w, z);
		if (score > best) {
			best = score;
			target = z;
		}
	}
	if (target < 0) return;
	if (spare > PAIRED_BATCH) spare = PAIRED_BATCH;
	UnitHandle *h = PUSH(arena, UnitHandle, spare);
	int32_t n = 0;
	for (ptrdiff_t i = 0; i < u->count && n < spare; i++)
		if (u->order[i] == ORDER_IDLE && !u->engaged[i] && unit_is_combat(u->type[i])) h[n++] = units_handle(u, i);
	emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_SEND, .amount = target, .count = n, .units = h});
}

// Idle scouts wander the zone, the enemy's side of it most: our eyes.
static void bot_patrol(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	const Units *u = &sim->factions[bot->faction.v].units;
	const Terrain *t = &sim->terrain;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (u->type[i] != UNIT_SCOUT || u->order[i] != ORDER_IDLE) continue;
		float x = rng_range(&bot->rng, 0.15f, 0.95f);
		if (bot->faction.v == 1) x = 1.0f - x;
		UnitHandle *h = PUSH(arena, UnitHandle, 1);
		*h = units_handle(u, i);
		emit(out, count, cap, (Command){.faction = bot->faction, .kind = COMMAND_MOVE, .count = 1, .units = h,
		                                .target = v2(x * t->size_x, rng_range(&bot->rng, 0.05f, 0.95f) * t->size_z)});
	}
}

// Every period: run the economy if there is one, and send idle combat units
// in groups to attack-move on the enemy base from a random angle (in a
// world, only once we see an enemy). Every STRATEGY_PERIOD, on
// a tick of its own, the map.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	if (!bot->enabled) return;
	if (bot->world && sim->config.economy && (sim->tick.v + 7 + bot->faction.v * 150) % STRATEGY_PERIOD == 0)
		bot_strategy(bot, sim, arena, out, count, cap);
	if ((sim->tick.v + bot->faction.v * 15) % bot->period != 0) return;
	if (sim->config.economy) bot_economy(bot, sim, arena, out, count, cap);
	const Faction *f = &sim->factions[bot->faction.v];
	const Faction *enemy = &sim->factions[faction_enemy(bot->faction).v];
	const Units *u = &f->units;
	// In a world, attack an enemy we see, or go after one seen here lately
	// that slipped out of sight. A landing nobody has spotted gets its
	// chance: scouts are how it gets spotted.
	const BotWorld *w = bot->world;
	if (w) {
		bot_patrol(bot, sim, arena, out, count, cap);
		if (f->view.count == 0 && !(w->seen_age[w->here] < HUNT_RECENT)) return;
	}

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
