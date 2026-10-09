#include "bot.h"

// ---------------------------------------------------------------- doctrines

static const Doctrine presets[] = {
	{.name = "balanced", .mix = {2, 1, 1, 0}, .army_cap = 300, .garrison = 10, .send_min = 12, .assault_army = 40,
	 .crew_builder = UNIT_ENGINEER, .crew_builders = 4, .crew_escort = 4, .scouts = 2, .transporters = 1, .hunt = true},
	{.name = "rush", .mix = {4, 1, 0, 0}, .army_cap = 180, .garrison = 5, .send_min = 8, .assault_army = 20,
	 .crew_builder = UNIT_SAPPER, .crew_builders = 2, .crew_escort = 6, .scouts = 1, .transporters = 1, .hunt = true},
	{.name = "turtle", .mix = {0, 2, 2, 1}, .army_cap = 400, .garrison = 40, .send_min = 30, .assault_army = 150,
	 .crew_builder = UNIT_ENGINEER, .crew_builders = 4, .crew_escort = 4, .scouts = 2, .transporters = 1,
	 .entrench = true},
	{.name = "raider", .mix = {5, 0, 0, 0}, .army_cap = 250, .garrison = 10, .send_min = 10, .assault_army = 30,
	 .crew_builder = UNIT_SAPPER, .crew_builders = 2, .crew_escort = 6, .scouts = 3, .transporters = 3, .raids = true,
	 .hunt = true},
	{.name = "siege", .mix = {0, 2, 1, 2}, .army_cap = 300, .garrison = 15, .send_min = 20, .assault_army = 80,
	 .crew_builder = UNIT_SAPPER, .crew_builders = 4, .crew_escort = 4, .scouts = 2, .transporters = 1, .hunt = true},
};

static bool bad_doctrine(char *err, size_t len, const char *why, const char *what)
{
	snprintf(err, len, "doctrine: %s: %s", why, what);
	return false;
}

bool doctrine_parse(Doctrine *d, const char *spec, char *err, size_t err_len)
{
	char buf[256], *save;
	snprintf(buf, sizeof buf, "%s", spec);
	char *name = strtok_r(buf, " \t", &save);
	if (!name) return bad_doctrine(err, err_len, "no preset named", spec);
	int k = 0;
	while (k < (int)countof(presets) && strcmp(presets[k].name, name)) k++;
	if (k == (int)countof(presets)) return bad_doctrine(err, err_len, "no such preset (balanced rush turtle raider siege)", name);
	*d = presets[k];
	for (char *t = strtok_r(nullptr, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
		char *eq = strchr(t, '=');
		if (!eq) return bad_doctrine(err, err_len, "expected FIELD=VALUE", t);
		*eq = 0;
		const char *key = t, *v = eq + 1;
		char *end;
		long n = strtol(v, &end, 10);
		bool number = *v && !*end && n >= 0 && n <= 10000;
		bool on = !strcmp(v, "on"), off = !strcmp(v, "off");
		struct { const char *key; int32_t *field; } ints[] = {
			{"army_cap", &d->army_cap}, {"garrison", &d->garrison}, {"send_min", &d->send_min},
			{"assault_army", &d->assault_army}, {"crew_builders", &d->crew_builders}, {"crew_escort", &d->crew_escort},
			{"scouts", &d->scouts}, {"transporters", &d->transporters},
		};
		struct { const char *key; bool *field; } flags[] = {
			{"entrench", &d->entrench}, {"raids", &d->raids}, {"hunt", &d->hunt},
		};
		bool done = false;
		for (int i = 0; i < (int)countof(ints) && !done; i++)
			if (!strcmp(key, ints[i].key)) {
				if (!number) return bad_doctrine(err, err_len, "expected a number 0..10000 for", key);
				*ints[i].field = (int32_t)n;
				done = true;
			}
		for (int i = 0; i < (int)countof(flags) && !done; i++)
			if (!strcmp(key, flags[i].key)) {
				if (!on && !off) return bad_doctrine(err, err_len, "expected on or off for", key);
				*flags[i].field = on;
				done = true;
			}
		for (int i = 0; i < COMBAT_TYPE_COUNT && !done; i++)
			if (!strcmp(key, unit_defs[i].name)) {
				if (!number || n > 20) return bad_doctrine(err, err_len, "expected 0..20 a round for", key);
				d->mix[i] = (int32_t)n;
				done = true;
			}
		if (!done && !strcmp(key, "crew_builder")) {
			if (strcmp(v, "engineer") && strcmp(v, "sapper")) return bad_doctrine(err, err_len, "engineer or sapper, not", v);
			d->crew_builder = strcmp(v, "engineer") ? UNIT_SAPPER : UNIT_ENGINEER;
			done = true;
		}
		if (!done) return bad_doctrine(err, err_len, "no such field", key);
	}
	int32_t round = 0;
	for (int i = 0; i < COMBAT_TYPE_COUNT; i++) round += d->mix[i];
	if (round == 0 || round > 5) return bad_doctrine(err, err_len, "the factory cycle is 1 to 5 units a round", spec);
	if (d->transporters < 1 || d->crew_builders < 1 || d->crew_builders + d->crew_escort > UNPAIRED_BATCH)
		return bad_doctrine(err, err_len, "transporters and crew builders at least 1, a crew at most 8", spec);
	return true;
}

Bot bot_create(FactionId faction, uint64_t seed)
{
	return bot_create_player(faction, (PlayerSlot){0}, seed);
}

Bot bot_create_player(FactionId faction, PlayerSlot player, uint64_t seed)
{
	return (Bot){
		.faction = faction,
		.player = player,
		.rng = rng_seed(seed ^ (0xb07u + faction.v + 0x100u * player.v)),
		.period = 30,
		.group_size = 128,
		.enabled = true,
		.doctrine = presets[0],
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

// The units this bot plays: its player's. Teammates' are friendly, not its own.
static bool mine(const Bot *bot, const Units *u, ptrdiff_t i) { return player_eq(u->owner[i], bot->player); }

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

// A site near the player's base where `type` fits, spiralling outward.
static bool site_near_base(const Sim *sim, const Bot *bot, UnitType type, const Promised *promised, V2 *out)
{
	FactionId f = bot->faction;
	V2 base = sim_player_base(sim, f, bot->player);
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

static bool own_transporter(const Bot *bot, const Units *u, bool finished_only)
{
	for (ptrdiff_t i = 0; i < u->count; i++)
		if (mine(bot, u, i) && u->type[i] == UNIT_TRANSPORTER && (u->progress[i] >= 1.0f || !finished_only)) return true;
	return false;
}


enum { BOT_TRUCKS = 2 };   // of each kind kept waiting in a zone, at most

// An economy, kept simply: factories cycle a mix of units (with engineers
// while there are too few); idle engineers take the most useful job.
static void bot_economy(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	FactionId f = bot->faction;
	const Faction *me = &sim->factions[f.v];
	const Units *u = &me->units;
	// Trucks' room is for shipping, not a budget: plan on the base storage.
	Economy plan = me->economy[bot->player.v];
	plan.metal_cap = BASE_METAL_CAP;
	plan.energy_cap = BASE_ENERGY_CAP;
	const Economy *e = &plan;
	const Doctrine *d = &bot->doctrine;
	int engineers = 0, factories = 0, army = 0, scouts = 0, sappers = 0, metal_trucks = 0, energy_trucks = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!mine(bot, u, i)) continue;
		metal_trucks += u->type[i] == UNIT_METAL_TRUCK && u->order[i] != ORDER_SEND;
		energy_trucks += u->type[i] == UNIT_ENERGY_TRUCK && u->order[i] != ORDER_SEND;
		engineers += u->type[i] == UNIT_ENGINEER;
		sappers += u->type[i] == UNIT_SAPPER;
		scouts += u->type[i] == UNIT_SCOUT;
		factories += u->type[i] == UNIT_FACTORY;
		army += unit_is_combat(u->type[i]);
	}
	int want_engineers = 3 + (int)(e->metal_income / 6.0f);
	if (want_engineers > 8) want_engineers = 8;

	// Opening: factories make engineers and nothing else until there's metal
	// coming in, or none left to take (teammates have it); then the
	// doctrine's cycle of units, with engineers while there are few, scouts
	// and sappers while it wants them.
	bool free_metal = false;
	for (int32_t k = 0; k < sim->spots.count && !free_metal; k++)
		free_metal = placement_ok(sim, f, UNIT_EXTRACTOR, sim->spots.at[k]);
	bool opening = e->metal_income < 12.0f && free_metal;
	int32_t amount[UNIT_TYPE_COUNT] = {0};
	amount[UNIT_ENGINEER] = engineers < want_engineers;
	amount[UNIT_SCOUT] = bot->world && scouts < d->scouts;
	amount[UNIT_SAPPER] = bot->world && d->crew_builder == UNIT_SAPPER && sappers < d->crew_builders;
	// A resource's base storage nearly full: trucks of it, to ship loads to
	// the front (bot_strategy).
	amount[UNIT_METAL_TRUCK] = bot->world && !opening && e->metal >= 0.9f * e->metal_cap && metal_trucks < BOT_TRUCKS;
	amount[UNIT_ENERGY_TRUCK] = bot->world && !opening && e->energy >= 0.9f * e->energy_cap && energy_trucks < BOT_TRUCKS;
	// Enough here (spend it first), or enough everywhere: a zone that sends
	// its army away would otherwise make one forever.
	int32_t everywhere = 0;
	for (int z = 0; bot->world && z < bot->world->count; z++) everywhere += bot->world->mine[z];
	if (!opening && army < d->army_cap && everywhere < 4 * d->army_cap)
		for (int k = 0; k < COMBAT_TYPE_COUNT; k++) amount[k] = d->mix[k];
	uint32_t want = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) want |= (uint32_t)(amount[k] > 0) << k;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!mine(bot, u, i) || u->type[i] != UNIT_FACTORY || u->progress[i] < 1.0f) continue;
		// A factory making units for a teammate makes them an army: what we
		// need ourselves comes from our other factories.
		bool for_us = player_eq(u->output[i], bot->player);
		uint32_t army_types = 0;
		for (int k = 0; k < COMBAT_TYPE_COUNT; k++) army_types |= (uint32_t)(d->mix[k] > 0) << k;
		uint32_t wanted = for_us ? want : army_types;
		uint32_t has = 0;
		for (int q = 0; q < u->queue_len[i]; q++) has |= 1u << (u->queue[i] >> (8 * q) & 0xff);
		if (has == wanted) continue; // already making what we want
		UnitHandle *h = PUSH(arena, UnitHandle, 1);
		*h = units_handle(u, i);
		Command c = {.faction = f, .kind = COMMAND_PRODUCE, .count = 1, .units = h, .flags = COMMAND_REPEAT};
		c.amount = 0; // clear, then queue the cycle
		emit(out, count, cap, c);
		// Builders and eyes first, then the army.
		const UnitType order[] = {UNIT_ENGINEER, UNIT_SAPPER, UNIT_SCOUT, UNIT_METAL_TRUCK, UNIT_ENERGY_TRUCK, UNIT_RAIDER,
		                          UNIT_TANK, UNIT_ARTILLERY, UNIT_SIEGE};
		for (int k = 0; k < (int)countof(order); k++) {
			int32_t n = for_us ? amount[order[k]] : unit_is_combat(order[k]) ? d->mix[order[k]] : 0;
			if (!n) continue;
			c.unit_type = order[k];
			c.amount = n;
			emit(out, count, cap, c);
		}
	}

	// Spots we hold or stand on or are walking to, and sites we're walking to:
	// teammates' too, so two players never race for one.
	uint64_t taken = me->spots;
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
	// In a world, a zone with no transporter of ours yet: the builders are
	// for that (bot_strategy), before anything else.
	if (bot->world && !own_transporter(bot, u, false)) return;
	int want_factories = 1 + (int)(e->metal_income / 10.0f);
	if (want_factories > 4) want_factories = 4;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!mine(bot, u, i) || u->type[i] != UNIT_ENGINEER || u->order[i] != ORDER_IDLE) continue;
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
		bool near = spot >= 0 && v2_len(v2_sub(sim->spots.at[spot], sim_player_base(sim, f, bot->player))) < 220.0f;
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
		} else if (!site_near_base(sim, bot, type, &promised, &site)) {
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

enum { STRATEGY_PERIOD = 300, FRONT_MARGIN = 100, HUNT_RECENT = 120, RECON_STALE = 300, RAID = 8 };

// How far forward a zone of ours is: where we see the enemy, then where
// we've seen a transporter of theirs, then next to a zone we believe is
// theirs; the weaker we are there, the more it needs.
static int32_t front_score(const BotWorld *w, int z)
{
	return (w->contact[z] ? 100000 : 0) + (w->transporter_age[z] < 1800 ? 80000 : 0)
	     + (w->neighbours_enemy[z] ? 50000 : 0) - w->mine[z];
}

// Up to n of the bot's idle units of a type (any combat type: UNIT_TYPE_COUNT), appended to h.
static int32_t pick_idle(const Bot *bot, const Units *u, UnitType type, int32_t n, UnitHandle *h, int32_t at)
{
	for (ptrdiff_t i = 0; i < u->count && n > 0; i++) {
		if (!mine(bot, u, i) || u->order[i] != ORDER_IDLE || u->engaged[i]) continue;
		bool match = type == UNIT_TYPE_COUNT ? unit_is_combat(u->type[i]) : u->type[i] == type;
		if (!match) continue;
		bool taken = false;
		for (int32_t k = 0; k < at && !taken; k++) taken = unit_handle_eq(h[k], units_handle(u, i));
		if (taken) continue;
		h[at++] = units_handle(u, i);
		n--;
	}
	return at;
}

static void bot_send(const Bot *bot, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap, int zone, UnitHandle *h, int32_t n)
{
	(void)arena;
	if (n > 0) emit(out, count, cap, (Command){.faction = bot->faction, .kind = COMMAND_SEND, .amount = zone, .count = n, .units = h});
}

// The map, as the doctrine plays it: transporters here, crews to open
// neighbours, raids, a dug-in garrison at the front, and what can be spared
// sent forward. Sends name the destination only; the server routes.
static void bot_strategy(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	const BotWorld *w = bot->world;
	const Doctrine *d = &bot->doctrine;
	FactionId f = bot->faction;
	const Faction *me = &sim->factions[f.v];
	const Units *u = &me->units;
	const Economy *e = &me->economy[bot->player.v];
	int here = w->here;
	bool sending = false;
	int32_t idle_army = 0, idle_builders = 0, idle_crew = 0, idle_raiders = 0, scouts = 0, turrets = 0;
	int32_t transporters = 0;
	ptrdiff_t free_scout = -1;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!mine(bot, u, i)) continue;
		sending = sending || u->order[i] == ORDER_SEND;
		bool idle = u->order[i] == ORDER_IDLE && !u->engaged[i];
		idle_army += idle && unit_is_combat(u->type[i]);
		idle_builders += idle && unit_is_builder(u->type[i]);
		idle_crew += idle && u->type[i] == d->crew_builder;
		idle_raiders += idle && u->type[i] == UNIT_RAIDER;
		scouts += u->type[i] == UNIT_SCOUT;
		turrets += u->type[i] == UNIT_TURRET && u->seeded[i];
		transporters += u->type[i] == UNIT_TRANSPORTER;
		if (u->type[i] == UNIT_SCOUT && u->order[i] != ORDER_SEND) free_scout = i; // patrolling will do
	}
	bool front = w->contact[here] || w->neighbours_enemy[here];

	// Transporters: the first with every idle builder (the sooner it's up, the
	// sooner help comes); more, one at a time, up to the doctrine's number.
	if (transporters == 0 || (own_transporter(bot, u, true) && transporters < d->transporters)) {
		if (idle_builders == 0 || e->metal < 0.5f * unit_defs[UNIT_TRANSPORTER].metal_cost) return;
		Promised none = {0};
		V2 site;
		if (!site_near_base(sim, bot, UNIT_TRANSPORTER, &none, &site)) return;
		int32_t take = transporters == 0 ? idle_builders : 1;
		UnitHandle *h = PUSH(arena, UnitHandle, idle_builders);
		int32_t n = pick_idle(bot, u, UNIT_SAPPER, take, h, 0);
		n = pick_idle(bot, u, UNIT_ENGINEER, take - n, h, n);
		emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_BUILD, .target = site, .count = n,
		                                .units = h, .unit_type = UNIT_TRANSPORTER});
		if (transporters == 0) return;
	}
	if (!own_transporter(bot, u, true) || sending) return; // one send at a time

	// Dig in at the front: the garrison, as turrets (it can't be sent then).
	if (d->entrench && front && turrets < d->garrison && idle_army > 0) {
		int32_t n = d->garrison - turrets;
		UnitHandle *h = PUSH(arena, UnitHandle, n);
		int32_t got = pick_idle(bot, u, UNIT_TANK, n, h, 0);
		got = pick_idle(bot, u, UNIT_RAIDER, n - got, h, got);
		if (got) emit(out, count, cap, (Command){.faction = f, .kind = COMMAND_ENTRENCH, .count = got, .units = h});
	}

	// Open a neighbour where we have no transporter: a crew of builders to
	// raise one, with raiders to guard them. One we don't believe the enemy
	// holds first; one we do only with an army here to follow the crew in.
	int32_t crew = d->crew_builders + d->crew_escort;
	float crew_energy = UNPAIRED_ENERGY + UNPAIRED_ENERGY_PER * (float)crew;
	int32_t spare_builders = idle_crew - (d->crew_builder == UNIT_ENGINEER ? 1 : 0); // an engineer stays home
	if (spare_builders >= d->crew_builders && e->energy >= crew_energy) {
		int target = -1;
		for (int z = 0; z < w->count; z++)
			if (z != here && w->linked[z] && !w->ours[z] && !bot_believes_enemy(w, z) && (target < 0 || rng_below(&bot->rng, 2)))
				target = z;
		for (int z = 0; z < w->count && target < 0 && idle_army >= d->assault_army; z++)
			if (z != here && w->linked[z] && !w->ours[z]) target = z;
		if (target >= 0) {
			UnitHandle *h = PUSH(arena, UnitHandle, crew);
			int32_t n = pick_idle(bot, u, d->crew_builder, d->crew_builders, h, 0);
			n = pick_idle(bot, u, UNIT_RAIDER, d->crew_escort, h, n);
			bot_send(bot, arena, out, count, cap, target, h, n);
			return;
		}
	}

	// Raids: a pack of raiders, unpaired, into a neighbour we believe is theirs.
	if (d->raids && idle_raiders >= RAID && e->energy >= UNPAIRED_ENERGY + UNPAIRED_ENERGY_PER * RAID) {
		for (int z = 0; z < w->count; z++) {
			if (z == here || !w->linked[z] || w->ours[z] || !bot_believes_enemy(w, z)) continue;
			UnitHandle *h = PUSH(arena, UnitHandle, RAID);
			bot_send(bot, arena, out, count, cap, z, h, pick_idle(bot, u, UNIT_RAIDER, RAID, h, 0));
			return;
		}
	}

	// Recon: a scout, unpaired, to a neighbour we know nothing fresh about
	// and hold no transporter in. One stays home.
	if (free_scout >= 0 && scouts > 1 && e->energy >= UNPAIRED_ENERGY + UNPAIRED_ENERGY_PER) {
		for (int z = 0; z < w->count; z++) {
			if (z == here || !w->linked[z] || w->ours[z] || w->contact[z]) continue;
			if (w->seen_age[z] != BOT_NEVER && w->seen_age[z] < RECON_STALE) continue;
			UnitHandle *h = PUSH(arena, UnitHandle, 1);
			*h = units_handle(u, free_scout);
			bot_send(bot, arena, out, count, cap, z, h, 1);
			return;
		}
	}

	// Send what can be spared toward the fight: a zone of ours where the
	// enemy is (a beachhead, or one under attack) first, then one next to
	// them; only ever to a zone further forward than this one, so a front
	// holds and a beachhead draws the army behind it.
	int target = -1;
	int32_t best = front_score(w, here) + FRONT_MARGIN; // clearly further forward, or stay
	for (int z = 0; z < w->count; z++) {
		if (z == here || !w->ours[z] || w->mine[z] >= 2 * d->army_cap) continue; // full up there
		int32_t score = front_score(w, z);
		if (score > best) {
			best = score;
			target = z;
		}
	}
	// Trucks, each kind by its own resource: one waiting here goes, with a
	// full load, to the zone of ours furthest forward where this player is
	// short of that resource, if there's a full load to take; one too many
	// in a zone that isn't home goes home when there's little to take.
	int home = -1;
	for (int z = 0; z < w->count; z++)
		if (w->home[z]) home = z;
	for (int r = 0; r < 2; r++) {
		UnitType kind = r ? UNIT_ENERGY_TRUCK : UNIT_METAL_TRUCK;
		float have = r ? e->energy : e->metal, base = r ? BASE_ENERGY_CAP : BASE_METAL_CAP;
		const uint8_t *short_of = r ? w->short_energy : w->short_metal;
		int32_t idle = 0;
		ptrdiff_t truck = -1;
		for (ptrdiff_t i = 0; i < u->count; i++) {
			if (!mine(bot, u, i) || u->type[i] != kind || u->order[i] != ORDER_IDLE) continue;
			idle++;
			truck = i;
		}
		if (truck < 0) continue;
		int dest = -1;
		if (have >= 0.9f * base) {
			int32_t best_short = front_score(w, here) + FRONT_MARGIN;
			for (int z = 0; z < w->count; z++) {
				if (z == here || !w->ours[z] || !(short_of[z] >> bot->player.v & 1) || front_score(w, z) <= best_short) continue;
				best_short = front_score(w, z);
				dest = z;
			}
		} else if (have < 0.5f * base && idle > BOT_TRUCKS && home >= 0 && home != here && w->ours[home]) {
			dest = home;
		}
		if (dest < 0) continue;
		UnitHandle *h = PUSH(arena, UnitHandle, 1);
		*h = units_handle(u, truck);
		bot_send(bot, arena, out, count, cap, dest, h, 1);
		return;
	}
	if (target < 0) return;
	if (w->contact[here] && !w->neighbours_enemy[here]) return; // a fight here: everyone stays
	int32_t spare = idle_army - (d->entrench ? 0 : d->garrison); // a dug-in garrison isn't idle
	if (spare < d->send_min) return;
	if (spare > PAIRED_BATCH) spare = PAIRED_BATCH;
	UnitHandle *h = PUSH(arena, UnitHandle, spare);
	bot_send(bot, arena, out, count, cap, target, h, pick_idle(bot, u, UNIT_TYPE_COUNT, spare, h, 0));
}

// Idle scouts wander the zone, the enemy's side of it most: our eyes.
static void bot_patrol(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	const Units *u = &sim->factions[bot->faction.v].units;
	const Terrain *t = &sim->terrain;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (!mine(bot, u, i) || u->type[i] != UNIT_SCOUT || u->order[i] != ORDER_IDLE) continue;
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
static void bot_think_side(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap);

// A bot's commands are its player's, and what it sends stays its own.
void bot_think(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	if (!bot->enabled) return;
	ptrdiff_t first = *count;
	bot_think_side(bot, sim, arena, out, count, cap);
	for (ptrdiff_t k = first; k < *count; k++) {
		out[k].player = bot->player;
		out[k].to = bot->player;
	}
}

static void bot_think_side(Bot *bot, const Sim *sim, Arena *arena, Command *out, ptrdiff_t *count, ptrdiff_t cap)
{
	// Teammates think on ticks of their own.
	int64_t t = sim->tick.v + bot->player.v * 4;
	if (bot->world && sim->config.economy && (t + bot->player.v * 33 + 7 + bot->faction.v * 150) % STRATEGY_PERIOD == 0)
		bot_strategy(bot, sim, arena, out, count, cap);
	if ((t + bot->faction.v * 15) % bot->period != 0) return;
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
		if (f->view.count == 0 && !(bot->doctrine.hunt && w->seen_age[w->here] < HUNT_RECENT)) return;
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
		if (!mine(bot, u, i) || u->order[i] != ORDER_IDLE || u->engaged[i] || !unit_is_combat(u->type[i])) continue;
		if (in_group == 0) group = PUSH(arena, UnitHandle, bot->group_size);
		group[in_group++] = units_handle(u, i);
	}
}
