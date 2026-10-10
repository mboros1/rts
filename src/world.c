#include "world.h"

int world_zone(const World *w, const char *name)
{
	for (int k = 0; k < w->zone_count; k++)
		if (!strcmp(w->zones[k].name, name)) return k;
	return -1;
}

bool world_adjacent(const World *w, int a, int b)
{
	for (int k = 0; k < w->link_count; k++)
		if ((w->links[k][0] == a && w->links[k][1] == b) || (w->links[k][0] == b && w->links[k][1] == a)) return true;
	return false;
}

static bool world_bad(char *err, ptrdiff_t err_len, const char *path, int line, const char *why)
{
	snprintf(err, (size_t)err_len, "%s:%d: %s", path, line, why);
	return false;
}

static bool parse_unit(const char *t, float *out)
{
	char *end;
	float v = strtof(t, &end);
	if (end == t || *end || !(v >= 0.0f && v <= 1.0f)) return false;
	*out = v;
	return true;
}

bool world_parse(World *w, Str text, const char *path, char *err, ptrdiff_t err_len)
{
	*w = (World){.port = 7700, .players = {1, 1}};
	char line[512];
	int line_no = 0;
	for (ptrdiff_t i = 0; i < text.len;) {
		ptrdiff_t j = i;
		while (j < text.len && text.ptr[j] != '\n') j++;
		line_no++;
		if (j - i >= (ptrdiff_t)sizeof line) return world_bad(err, err_len, path, line_no, "line too long");
		memcpy(line, text.ptr + i, (size_t)(j - i));
		line[j - i] = 0;
		i = j + 1;
		char *hash = strchr(line, '#');
		if (hash) *hash = 0;
		char *tok[32], *save;
		int n = 0;
		for (char *t = strtok_r(line, " \t\r", &save); t && n < 32; t = strtok_r(nullptr, " \t\r", &save)) tok[n++] = t;
		if (n == 0) continue;
		if (!strcmp(tok[0], "port") && n == 2) {
			long p = strtol(tok[1], nullptr, 10);
			if (p < 1024 || p > 65000) return world_bad(err, err_len, path, line_no, "port out of range");
			w->port = (int)p;
		} else if (!strcmp(tok[0], "zone") && (n == 4 || n == 6)) {
			if (w->zone_count == COORD_MAX_ZONES) return world_bad(err, err_len, path, line_no, "too many zones");
			if (strlen(tok[1]) >= ZONE_NAME || world_zone(w, tok[1]) >= 0 || !strcmp(tok[1], "pool"))
				return world_bad(err, err_len, path, line_no, "bad or repeated zone name");
			WorldZone *z = &w->zones[w->zone_count];
			*z = (WorldZone){.home = -1};
			snprintf(z->name, sizeof z->name, "%s", tok[1]);
			if (!parse_unit(tok[2], &z->x) || !parse_unit(tok[3], &z->y))
				return world_bad(err, err_len, path, line_no, "position is two numbers from 0 to 1");
			if (n == 6) {
				if (strcmp(tok[4], "home")) return world_bad(err, err_len, path, line_no, "expected: home blue|red");
				if (!strcmp(tok[5], "blue")) z->home = 0;
				else if (!strcmp(tok[5], "red")) z->home = 1;
				else return world_bad(err, err_len, path, line_no, "home is blue or red");
			}
			w->zone_count++;
		} else if (!strcmp(tok[0], "link") && n == 3) {
			int a = world_zone(w, tok[1]), b = world_zone(w, tok[2]);
			if (a < 0 || b < 0 || a == b) return world_bad(err, err_len, path, line_no, "link needs two different zones");
			if (world_adjacent(w, a, b)) return world_bad(err, err_len, path, line_no, "repeated link");
			if (w->link_count == WORLD_MAX_LINKS) return world_bad(err, err_len, path, line_no, "too many links");
			w->links[w->link_count][0] = (uint8_t)a;
			w->links[w->link_count++][1] = (uint8_t)b;
		} else if (!strcmp(tok[0], "players") && n == 3) {
			int side = !strcmp(tok[1], "blue") ? 0 : !strcmp(tok[1], "red") ? 1 : -1;
			char *end;
			long k = strtol(tok[2], &end, 10);
			if (side < 0 || *end || k < 1 || k > MAX_PLAYERS)
				return world_bad(err, err_len, path, line_no, "players blue|red N (1..8)");
			w->players[side] = (uint8_t)k;
		} else if (!strcmp(tok[0], "doctrine") && n >= 3) {
			// Checked when a zone's setup reads it; here only where it goes.
			int side = !strcmp(tok[1], "blue") ? 0 : !strcmp(tok[1], "red") ? 1 : -1;
			if (side < 0) return world_bad(err, err_len, path, line_no, "doctrine blue|red [in ZONE] PRESET [FIELD=VALUE...]");
			int first = 2, zone = -1;
			if (!strcmp(tok[2], "in")) {
				if (n < 5 || (zone = world_zone(w, tok[3])) < 0)
					return world_bad(err, err_len, path, line_no, "doctrine SIDE in ZONE: a zone named before this line");
				first = 4;
			}
			char spec[192] = "";
			for (int k = first; k < n; k++) snprintf(spec + strlen(spec), sizeof spec - strlen(spec), "%s%s", k > first ? " " : "", tok[k]);
			char *to = zone < 0 ? w->doctrine[side] : w->doctrine_in[zone][side];
			snprintf(to, sizeof w->doctrine[side], "%s", spec);
		} else if ((!strcmp(tok[0], "economy") || !strcmp(tok[0], "environment") || !strcmp(tok[0], "map")) && n >= 2) {
			// Checked when the world starts (rts world --new); here only where it goes.
			bool economy = !strcmp(tok[0], "economy"), map = !strcmp(tok[0], "map");
			int first = 1, zone = -1;
			if (!strcmp(tok[1], "in")) {
				if (n < 4 || (zone = world_zone(w, tok[2])) < 0)
					return world_bad(err, err_len, path, line_no, "economy|environment|map in ZONE: a zone named before this line");
				first = 3;
			}
			char *to = economy ? (zone < 0 ? w->economy : w->economy_in[zone])
			         : map     ? (zone < 0 ? w->map : w->map_in[zone])
			                   : (zone < 0 ? w->environment : w->environment_in[zone]);
			to[0] = 0;
			for (int k = first; k < n; k++) snprintf(to + strlen(to), sizeof w->economy - strlen(to), "%s%s", k > first ? " " : "", tok[k]);
		} else if (!strcmp(tok[0], "setup") && n >= 3) {
			// setup ZONE LINE: a script line the zone starts with (a fixture:
			// what's there at the start; checked when the zone reads its script).
			int zone = world_zone(w, tok[1]);
			if (zone < 0) return world_bad(err, err_len, path, line_no, "setup ZONE LINE: a zone named before this line");
			char *to = w->setup[zone];
			for (int k = 2; k < n; k++) snprintf(to + strlen(to), sizeof w->setup[zone] - strlen(to), "%s%s", tok[k], k + 1 < n ? " " : "\n");
			if (strlen(to) + 1 >= sizeof w->setup[zone]) return world_bad(err, err_len, path, line_no, "too much setup for one zone");
		} else {
			return world_bad(err, err_len, path, line_no,
			                 "expected: port N | zone NAME X Y [home SIDE] | link A B | players SIDE N | doctrine ... | economy ... | environment ... | map ... | setup ZONE ...");
		}
	}
	if (w->zone_count == 0) return world_bad(err, err_len, path, line_no, "no zones");
	for (int f = 0; f < FACTION_COUNT; f++) {
		int homes = 0;
		for (int k = 0; k < w->zone_count; k++) homes += w->zones[k].home == f;
		if (homes != 1) return world_bad(err, err_len, path, line_no, "each side needs exactly one home zone");
	}
	return true;
}

// ---------------------------------------------------------------- the directory

void put_zone_status(ByteBuf *b, const ZoneStatus *s)
{
	put64(b, (uint64_t)s->tick);
	for (int f = 0; f < FACTION_COUNT; f++) {
		put16(b, s->units[f]);
		put16(b, s->structures[f]);
		put8(b, s->transporters[f]);
		put8(b, s->short_metal[f]);
		put8(b, s->short_energy[f]);
		put8(b, s->intel[f].contact);
		put32(b, s->intel[f].seen_age);
		put8(b, s->intel[f].transporters);
		put32(b, s->intel[f].transporter_age);
	}
	put8(b, s->fallen);
}

bool get_zone_status(ByteReader *r, ZoneStatus *s)
{
	s->tick = (int64_t)get64(r);
	for (int f = 0; f < FACTION_COUNT; f++) {
		s->units[f] = get16(r);
		s->structures[f] = get16(r);
		s->transporters[f] = get8(r);
		s->short_metal[f] = get8(r);
		s->short_energy[f] = get8(r);
		uint8_t contact = get8(r);
		s->intel[f].seen_age = get32(r);
		s->intel[f].transporters = get8(r);
		s->intel[f].transporter_age = get32(r);
		if (contact > 1) return false;
		s->intel[f].contact = contact;
	}
	s->fallen = get8(r);
	return !r->bad && s->tick >= 0 && s->fallen < 1u << FACTION_COUNT;
}

void put_directory(ByteBuf *b, const Directory *d)
{
	put8(b, (uint8_t)d->zone_count);
	for (int k = 0; k < d->zone_count; k++) {
		const DirectoryZone *z = &d->zones[k];
		put_name(b, z->name);
		put_name(b, z->host);
		put16(b, (uint16_t)z->port);
		put8(b, z->up);
		putf(b, z->x);
		putf(b, z->y);
		put8(b, (uint8_t)z->home);
		put8(b, z->reported);
		put_zone_status(b, &z->status);
	}
	put8(b, (uint8_t)d->link_count);
	for (int k = 0; k < d->link_count; k++) {
		put8(b, d->links[k][0]);
		put8(b, d->links[k][1]);
	}
	put8(b, (uint8_t)d->transit_count);
	for (int k = 0; k < d->transit_count; k++) {
		put8(b, d->transit[k].src);
		put8(b, d->transit[k].dst);
		put8(b, d->transit[k].faction);
		put32(b, (uint32_t)d->transit[k].units);
	}
	for (int f = 0; f < FACTION_COUNT; f++) {
		put8(b, d->players[f]);
		put8(b, d->online[f]);
		for (int k = 0; k < d->players[f]; k++) {
			put8(b, d->player[f][k][0] != 0);
			if (d->player[f][k][0]) put_name(b, d->player[f][k]);
		}
	}
	put8(b, d->ended);
	put8(b, d->ended_in);
}

bool get_directory(ByteReader *r, Directory *d)
{
	*d = (Directory){0};
	int n = get8(r);
	if (n > COORD_MAX_ZONES) return false;
	for (int k = 0; k < n; k++) {
		DirectoryZone *z = &d->zones[k];
		char host[ZONE_NAME];
		if (!get_name(r, z->name) || !get_name(r, host)) return false;
		snprintf(z->host, sizeof z->host, "%s", host);
		z->port = get16(r);
		z->up = get8(r);
		z->x = getf(r);
		z->y = getf(r);
		z->home = (int8_t)get8(r);
		z->reported = get8(r);
		if (!get_zone_status(r, &z->status)) return false;
		if (!(z->x >= 0.0f && z->x <= 1.0f && z->y >= 0.0f && z->y <= 1.0f)) return false;
		if (z->home < -1 || z->home >= FACTION_COUNT) return false;
	}
	d->zone_count = n;
	int links = get8(r);
	if (links > WORLD_MAX_LINKS) return false;
	for (int k = 0; k < links; k++) {
		d->links[k][0] = get8(r);
		d->links[k][1] = get8(r);
		if (d->links[k][0] >= n || d->links[k][1] >= n) return false;
	}
	d->link_count = links;
	int transit = get8(r);
	if (transit > DIRECTORY_MAX_TRANSIT) return false;
	for (int k = 0; k < transit; k++) {
		DirectoryTransit *t = &d->transit[k];
		t->src = get8(r);
		t->dst = get8(r);
		t->faction = get8(r);
		t->units = (int32_t)get32(r);
		if (t->src >= n || t->dst >= n || t->faction >= FACTION_COUNT || t->units < 0) return false;
	}
	d->transit_count = transit;
	for (int f = 0; f < FACTION_COUNT; f++) {
		d->players[f] = get8(r);
		d->online[f] = get8(r);
		if (d->players[f] > MAX_PLAYERS) return false;
		for (int k = 0; k < d->players[f]; k++) {
			uint8_t named = get8(r);
			if (named > 1 || (named && !get_name(r, d->player[f][k]))) return false;
		}
	}
	d->ended = get8(r);
	d->ended_in = get8(r);
	if (d->ended > FACTION_COUNT || (d->ended && d->ended_in >= n)) return false;
	return !r->bad;
}

void directory_print(const Directory *d, FILE *out)
{
	for (int k = 0; k < d->zone_count; k++) {
		const DirectoryZone *z = &d->zones[k];
		fprintf(out, "%-10s %-4s %-12s", z->name, z->up ? "up" : "down",
		        z->home == 0 ? "blue's home" : z->home == 1 ? "red's home" : "");
		if (z->reported) {
			int64_t secs = z->status.tick / (int64_t)SIM_HZ;
			fprintf(out, " %lld:%02lld:%02lld ", (long long)(secs / 3600), (long long)(secs / 60 % 60), (long long)(secs % 60));
			for (int f = 0; f < FACTION_COUNT; f++) {
				const Intel *in = &z->status.intel[f];
				fprintf(out, "%s %s %d units %d bldg %d transporters", f ? " |" : "", f ? "red" : "blue", z->status.units[f],
				        z->status.structures[f], z->status.transporters[f]);
				if (in->contact) fprintf(out, " (contact)");
				else if (in->seen_age != INTEL_NEVER) fprintf(out, " (saw them %us ago)", in->seen_age);
				if (in->transporters) fprintf(out, " (saw %u of their transporters %us ago)", in->transporters, in->transporter_age);
			}
		}
		fprintf(out, "\n");
	}
	if (d->link_count) {
		fprintf(out, "links:");
		for (int k = 0; k < d->link_count; k++)
			fprintf(out, " %s-%s", d->zones[d->links[k][0]].name, d->zones[d->links[k][1]].name);
		fprintf(out, "\n");
	}
	for (int k = 0; k < d->transit_count; k++) {
		const DirectoryTransit *t = &d->transit[k];
		fprintf(out, "transit: %s %d %s->%s\n", t->faction ? "red" : "blue", t->units, d->zones[t->src].name,
		        d->zones[t->dst].name);
	}
	// players: blue 0 alice, 1 (bot) | red 0 (bot)
	fprintf(out, "players:");
	for (int f = 0; f < FACTION_COUNT; f++) {
		fprintf(out, "%s %s", f ? " |" : "", f ? "red" : "blue");
		for (int k = 0; k < d->players[f]; k++)
			fprintf(out, "%s %d %s%s", k ? "," : "", k, d->player[f][k][0] ? d->player[f][k] : "(bot)",
			        d->player[f][k][0] && !(d->online[f] >> k & 1) ? " (away)" : "");
	}
	fprintf(out, "\n");
	if (d->ended)
		fprintf(out, "game over: %s won (%s's headquarters fell in %s)\n", d->ended == 1 ? "blue" : "red",
		        d->ended == 1 ? "red" : "blue", d->zones[d->ended_in].name);
}

void bot_world_build(BotWorld *w, const Directory *d, int here, uint8_t f)
{
	*w = (BotWorld){.count = d->zone_count < ZONE_MAX ? d->zone_count : ZONE_MAX, .here = here};
	for (int z = 0; z < w->count; z++) {
		const DirectoryZone *dz = &d->zones[z];
		w->home[z] = dz->home == f;
		w->seen_age[z] = w->transporter_age[z] = BOT_NEVER;
		if (!dz->reported) continue;
		const Intel *in = &dz->status.intel[f];
		w->ours[z] = dz->status.transporters[f] > 0;
		w->short_metal[z] = dz->status.short_metal[f];
		w->short_energy[z] = dz->status.short_energy[f];
		w->mine[z] = dz->status.units[f] + 2 * dz->status.structures[f];
		w->contact[z] = in->contact;
		w->seen_age[z] = in->seen_age;
		w->transporter_age[z] = in->transporters ? in->transporter_age : BOT_NEVER;
	}
}

void bot_world_links(BotWorld *w, const Directory *d)
{
	for (int k = 0; k < d->link_count; k++) {
		int a = d->links[k][0], b = d->links[k][1];
		if (a >= w->count || b >= w->count) continue;
		if (a == w->here) w->linked[b] = true;
		if (b == w->here) w->linked[a] = true;
		if (bot_believes_enemy(w, a)) w->neighbours_enemy[b] = true;
		if (bot_believes_enemy(w, b)) w->neighbours_enemy[a] = true;
	}
}

void directory_fog(Directory *d, uint8_t viewer)
{
	if (viewer >= FACTION_COUNT) return;
	uint8_t enemy = (uint8_t)(1 - viewer);
	for (int k = 0; k < d->zone_count; k++) {
		ZoneStatus *s = &d->zones[k].status;
		s->units[enemy] = 0;
		s->structures[enemy] = 0;
		s->transporters[enemy] = 0;
		s->short_metal[enemy] = s->short_energy[enemy] = 0;
		s->intel[enemy] = (Intel){.seen_age = INTEL_NEVER, .transporter_age = INTEL_NEVER};
	}
	d->players[enemy] = 0;
	d->online[enemy] = 0;
	memset(d->player[enemy], 0, sizeof d->player[enemy]);
	int kept = 0;
	for (int k = 0; k < d->transit_count; k++)
		if (d->transit[k].faction == viewer) d->transit[kept++] = d->transit[k];
	d->transit_count = kept;
}
