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
	*w = (World){.port = 7700};
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
		char *tok[8], *save;
		int n = 0;
		for (char *t = strtok_r(line, " \t\r", &save); t && n < 8; t = strtok_r(nullptr, " \t\r", &save)) tok[n++] = t;
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
		} else {
			return world_bad(err, err_len, path, line_no, "expected: port N | zone NAME X Y [home SIDE] | link A B");
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
	}
}

bool get_zone_status(ByteReader *r, ZoneStatus *s)
{
	s->tick = (int64_t)get64(r);
	for (int f = 0; f < FACTION_COUNT; f++) {
		s->units[f] = get16(r);
		s->structures[f] = get16(r);
		s->transporters[f] = get8(r);
	}
	return !r->bad && s->tick >= 0;
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
			for (int f = 0; f < FACTION_COUNT; f++)
				fprintf(out, "%s %s %d units %d bldg %d transporters", f ? " |" : "", f ? "red" : "blue", z->status.units[f],
				        z->status.structures[f], z->status.transporters[f]);
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
}
