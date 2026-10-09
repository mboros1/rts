#include "coord.h"

#include <unistd.h>

void put_name(ByteBuf *b, const char *name)
{
	size_t n = strlen(name);
	if (n >= ZONE_NAME) n = ZONE_NAME - 1;
	put8(b, (uint8_t)n);
	put_bytes(b, name, (ptrdiff_t)n);
}

bool get_name(ByteReader *r, char out[ZONE_NAME])
{
	uint8_t n = get8(r);
	const uint8_t *p = n < ZONE_NAME ? take(r, n) : nullptr;
	if (!p || r->bad) return false;
	memcpy(out, p, n);
	out[n] = 0;
	for (uint8_t i = 0; i < n; i++)
		if (out[i] <= ' ' || out[i] > '~') return false; // names are single printable words
	return n > 0;
}

void put_counts(ByteBuf *b, const int32_t counts[UNIT_TYPE_COUNT])
{
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) put32(b, (uint32_t)counts[k]);
}

bool get_counts(ByteReader *r, int32_t counts[UNIT_TYPE_COUNT])
{
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		counts[k] = (int32_t)get32(r);
		if (counts[k] < 0 || counts[k] > SPAWN_MAX) return false;
	}
	return !r->bad;
}

void put_cargo(ByteBuf *b, const Cargo *c)
{
	putf(b, c->metal);
	putf(b, c->energy);
}

bool get_cargo(ByteReader *r, Cargo *c)
{
	c->metal = getf(r);
	c->energy = getf(r);
	// Comparisons are false for NaN, so this refuses those too.
	return !r->bad && c->metal >= 0.0f && c->metal <= 1e9f && c->energy >= 0.0f && c->energy <= 1e9f;
}

void put_leg(ByteBuf *b, const Leg *l)
{
	put8(b, l->unpaired);
	put8(b, l->owner.v);
	put32(b, l->route);
	putf(b, l->landing.x);
	putf(b, l->landing.z);
}

bool get_leg(ByteReader *r, Leg *l)
{
	uint8_t unpaired = get8(r);
	l->owner = (PlayerSlot){get8(r)};
	l->route = get32(r);
	l->landing = v2(getf(r), getf(r));
	l->unpaired = unpaired;
	// Comparisons are false for NaN, so this refuses those too.
	bool sane = l->landing.x >= -1e5f && l->landing.x <= 1e5f && l->landing.z >= -1e5f && l->landing.z <= 1e5f;
	return !r->bad && unpaired <= 1 && player_ok(l->owner) && route_ok(l->route) && !(unpaired && l->route) && sane;
}

// ---------------------------------------------------------------- the log

static void log_line(Ledger *l, const char *line)
{
	if (!l->log) return;
	fputs(line, l->log);
	fputc('\n', l->log);
	l->lines++;
	// Durable before anyone hears of it: a restart replays exactly this.
	if (fflush(l->log) != 0 || fsync(fileno(l->log)) != 0) fatal("cannot write the coordinator's log");
}

static void format_counts(char *out, size_t size, const int32_t counts[UNIT_TYPE_COUNT], const Cargo *cargo)
{
	size_t used = 0;
	out[0] = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT && used < size; k++)
		if (counts[k]) used += (size_t)snprintf(out + used, size - used, " %s:%d", unit_defs[k].name, counts[k]);
	if ((cargo->metal > 0.0f || cargo->energy > 0.0f) && used < size)
		snprintf(out + used, size - used, " cargo:%.9g,%.9g", (double)cargo->metal, (double)cargo->energy);
}

static bool parse_counts(char **save, int32_t counts[UNIT_TYPE_COUNT], Cargo *cargo)
{
	memset(counts, 0, sizeof(int32_t) * UNIT_TYPE_COUNT);
	*cargo = (Cargo){0};
	for (char *tok; (tok = strtok_r(nullptr, " ", save));) {
		char *colon = strchr(tok, ':');
		if (!colon) return false;
		*colon = 0;
		if (!strcmp(tok, "cargo")) {
			if (sscanf(colon + 1, "%f,%f", &cargo->metal, &cargo->energy) != 2) return false;
			if (!(cargo->metal >= 0.0f && cargo->energy >= 0.0f)) return false;
			continue;
		}
		int k = 0;
		while (k < UNIT_TYPE_COUNT && strcmp(unit_defs[k].name, tok)) k++;
		long n = strtol(colon + 1, nullptr, 10);
		if (k == UNIT_TYPE_COUNT || n <= 0 || n > SPAWN_MAX) return false;
		counts[k] = (int32_t)n;
	}
	return true;
}

static uint64_t *seq_of(Ledger *l, const char *zone)
{
	for (int32_t k = 0; k < l->seq_zones; k++)
		if (!strcmp(l->seq_zone[k], zone)) return &l->seq_max[k];
	if (l->seq_zones == (int32_t)countof(l->seq_max)) fatal("coordinator: too many zones");
	snprintf(l->seq_zone[l->seq_zones], ZONE_NAME, "%s", zone);
	l->seq_max[l->seq_zones] = 0;
	return &l->seq_max[l->seq_zones++];
}

static Transfer *add_transfer(Ledger *l, uint64_t id, const char *src, const char *dst, uint8_t faction, const Leg *leg,
                              const int32_t counts[UNIT_TYPE_COUNT], const Cargo *cargo)
{
	if (l->transfer_count == COORD_MAX_TRANSFERS) fatal("coordinator: too many transfers");
	Transfer *t = &l->transfers[l->transfer_count++];
	*t = (Transfer){.id = id, .faction = faction, .leg = *leg, .cargo = *cargo};
	snprintf(t->src, sizeof t->src, "%s", src);
	snprintf(t->dst, sizeof t->dst, "%s", dst);
	memcpy(t->counts, counts, sizeof t->counts);
	if (id >= l->next_id) l->next_id = id + 1;
	return t;
}

Transfer *ledger_find(Ledger *l, uint64_t id)
{
	for (int32_t k = 0; k < l->transfer_count; k++)
		if (l->transfers[k].id == id) return &l->transfers[k];
	return nullptr;
}

int32_t ledger_pending(const Ledger *l)
{
	int32_t n = 0;
	for (int32_t k = 0; k < l->transfer_count; k++) n += !l->transfers[k].delivered;
	return n;
}

bool ledger_replay(Ledger *l, Str text, int32_t *bad_line)
{
	*bad_line = 0;
	char line[1024];
	for (ptrdiff_t i = 0; i < text.len;) {
		ptrdiff_t j = i;
		while (j < text.len && text.ptr[j] != '\n') j++;
		++*bad_line;
		if (j - i >= (ptrdiff_t)sizeof line) return false;
		memcpy(line, text.ptr + i, (size_t)(j - i));
		line[j - i] = 0;
		i = j + 1;
		if (!line[0]) continue;
		char *save;
		char *verb = strtok_r(line, " ", &save);
		char *id_s = strtok_r(nullptr, " ", &save);
		if (!verb || !id_s) return false;
		uint64_t id = strtoull(id_s, nullptr, 10);
		int32_t counts[UNIT_TYPE_COUNT];
		Cargo cargo;
		if (!strcmp(verb, "transfer")) {
			char *src = strtok_r(nullptr, " ", &save), *dst = strtok_r(nullptr, " ", &save);
			char *fac = strtok_r(nullptr, " ", &save), *owner = strtok_r(nullptr, " ", &save);
			char *kind = strtok_r(nullptr, " ", &save);
			char *seq_s = strtok_r(nullptr, " ", &save), *route_s = strtok_r(nullptr, " ", &save);
			char *at_s = strtok_r(nullptr, " ", &save);
			if (!src || !dst || !fac || !owner || !kind || !seq_s || !route_s || !at_s || !parse_counts(&save, counts, &cargo)) return false;
			long faction = strtol(fac, nullptr, 10);
			if (faction < 0 || faction >= FACTION_COUNT) return false;
			long player = strtol(owner, nullptr, 10);
			if (player < 0 || player >= MAX_PLAYERS) return false;
			Leg leg = {.unpaired = !strcmp(kind, "unpaired"), .route = (Route)strtoul(route_s, nullptr, 16),
			           .owner = {(uint8_t)player}};
			if (!leg.unpaired && strcmp(kind, "paired")) return false;
			if (sscanf(at_s, "%f,%f", &leg.landing.x, &leg.landing.z) != 2) return false;
			if (!route_ok(leg.route) || (leg.unpaired && leg.route)) return false;
			uint64_t seq = strtoull(seq_s, nullptr, 10), *max = seq_of(l, src);
			if (seq > *max) *max = seq;
			add_transfer(l, id, src, dst, (uint8_t)faction, &leg, counts, &cargo);
		} else if (!strcmp(verb, "next")) {
			if (id > l->next_id) l->next_id = id;
		} else if (!strcmp(verb, "seq")) {
			// seq ZONE N: id_s is the zone's name here.
			char *n_s = strtok_r(nullptr, " ", &save);
			if (!n_s || strlen(id_s) >= ZONE_NAME) return false;
			uint64_t n = strtoull(n_s, nullptr, 10), *max = seq_of(l, id_s);
			if (n > *max) *max = n;
		} else if (!strcmp(verb, "player")) {
			// player NAME FACTION SLOT: id_s is the name here.
			char *fac = strtok_r(nullptr, " ", &save), *slot_s = strtok_r(nullptr, " ", &save);
			if (!fac || !slot_s || strlen(id_s) >= ZONE_NAME) return false;
			long faction = strtol(fac, nullptr, 10), slot = strtol(slot_s, nullptr, 10);
			if (faction < 0 || faction >= FACTION_COUNT || slot < 0 || slot >= MAX_PLAYERS) return false;
			snprintf(l->player[faction][slot], ZONE_NAME, "%s", id_s);
		} else if (!strcmp(verb, "delivered")) {
			Transfer *t = ledger_find(l, id);
			if (!t) return false;
			t->delivered = true;
		} else {
			return false;
		}
	}
	return true;
}

Transfer *ledger_depart(Ledger *l, const char *src, uint64_t seq, const char *dst, uint8_t faction, const Leg *leg,
                        const int32_t counts[UNIT_TYPE_COUNT], const Cargo *cargo)
{
	uint64_t *max = seq_of(l, src);
	if (seq <= *max) return nullptr; // a zone re-sending after a restart
	*max = seq;
	char line[512], list[384];
	format_counts(list, sizeof list, counts, cargo);
	snprintf(line, sizeof line, "transfer %llu %s %s %u %u %s %llu %x %.9g,%.9g%s", (unsigned long long)l->next_id, src, dst,
	         faction, leg->owner.v, leg->unpaired ? "unpaired" : "paired", (unsigned long long)seq, leg->route, (double)leg->landing.x,
	         (double)leg->landing.z, list);
	log_line(l, line);
	return add_transfer(l, l->next_id, src, dst, faction, leg, counts, cargo);
}

void ledger_delivered(Ledger *l, uint64_t id)
{
	Transfer *t = ledger_find(l, id);
	if (!t || t->delivered) return; // a repeated confirmation changes nothing
	char line[64];
	snprintf(line, sizeof line, "delivered %llu", (unsigned long long)id);
	log_line(l, line);
	t->delivered = true;
}

uint8_t ledger_join(Ledger *l, uint8_t faction, const char *name, int slots)
{
	ASSERT(faction < FACTION_COUNT && slots <= MAX_PLAYERS);
	for (int k = 0; k < slots; k++)
		if (!strcmp(l->player[faction][k], name)) return (uint8_t)k;
	for (int k = 0; k < slots; k++) {
		if (l->player[faction][k][0]) continue;
		char line[64];
		snprintf(line, sizeof line, "player %s %u %d", name, faction, k);
		log_line(l, line);
		snprintf(l->player[faction][k], ZONE_NAME, "%s", name);
		return (uint8_t)k;
	}
	return SIDE_FULL;
}

static void format_transfer(char *line, size_t size, const Transfer *t, uint64_t seq)
{
	char list[384];
	format_counts(list, sizeof list, t->counts, &t->cargo);
	snprintf(line, size, "transfer %llu %s %s %u %u %s %llu %x %.9g,%.9g%s", (unsigned long long)t->id, t->src, t->dst,
	         t->faction, t->leg.owner.v, t->leg.unpaired ? "unpaired" : "paired", (unsigned long long)seq, t->leg.route,
	         (double)t->leg.landing.x, (double)t->leg.landing.z, list);
}

bool ledger_compact(Ledger *l, const char *path, Arena *scratch)
{
	ArenaMark m = arena_mark(scratch);
	ByteBuf b = bytebuf_make(scratch, (ptrdiff_t)l->transfer_count * 512 + 64 * (COORD_MAX_ZONES * 2)
	                                    + 64 * FACTION_COUNT * MAX_PLAYERS + 64);
	char line[512];
	snprintf(line, sizeof line, "next %llu\n", (unsigned long long)l->next_id);
	put_bytes(&b, line, (ptrdiff_t)strlen(line));
	for (int32_t k = 0; k < l->seq_zones; k++) {
		snprintf(line, sizeof line, "seq %s %llu\n", l->seq_zone[k], (unsigned long long)l->seq_max[k]);
		put_bytes(&b, line, (ptrdiff_t)strlen(line));
	}
	for (int f = 0; f < FACTION_COUNT; f++)
		for (int k = 0; k < MAX_PLAYERS; k++) {
			if (!l->player[f][k][0]) continue;
			snprintf(line, sizeof line, "player %s %d %d\n", l->player[f][k], f, k);
			put_bytes(&b, line, (ptrdiff_t)strlen(line));
		}
	int32_t kept = 0;
	for (int32_t k = 0; k < l->transfer_count; k++) {
		const Transfer *t = &l->transfers[k];
		if (t->delivered) continue;
		// The departure number only mattered for spotting repeats, which the
		// seq lines now cover.
		format_transfer(line, sizeof line, t, 0);
		put_bytes(&b, line, (ptrdiff_t)strlen(line));
		put_bytes(&b, "\n", 1);
		l->transfers[kept++] = *t;
	}
	bool ok = !b.overflow && platform_write_file_atomic(path, b.data, b.len);
	if (ok) {
		l->transfer_count = kept;
		if (l->log) fclose(l->log);
		l->log = fopen(path, "a");
		if (!l->log) fatal("cannot reopen %s", path);
		l->lines = 0;
	}
	arena_rewind(m);
	return ok;
}
