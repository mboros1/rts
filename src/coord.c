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

// ---------------------------------------------------------------- the log

static void log_line(Ledger *l, const char *line)
{
	if (!l->log) return;
	fputs(line, l->log);
	fputc('\n', l->log);
	// Durable before anyone hears of it: a restart replays exactly this.
	if (fflush(l->log) != 0 || fsync(fileno(l->log)) != 0) fatal("cannot write the coordinator's log");
}

static void format_counts(char *out, size_t size, const int32_t counts[UNIT_TYPE_COUNT])
{
	size_t used = 0;
	out[0] = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT && used < size; k++)
		if (counts[k]) used += (size_t)snprintf(out + used, size - used, " %s:%d", unit_defs[k].name, counts[k]);
}

static bool parse_counts(char **save, int32_t counts[UNIT_TYPE_COUNT])
{
	memset(counts, 0, sizeof(int32_t) * UNIT_TYPE_COUNT);
	for (char *tok; (tok = strtok_r(nullptr, " ", save));) {
		char *colon = strchr(tok, ':');
		if (!colon) return false;
		*colon = 0;
		int k = 0;
		while (k < UNIT_TYPE_COUNT && strcmp(unit_defs[k].name, tok)) k++;
		long n = strtol(colon + 1, nullptr, 10);
		if (k == UNIT_TYPE_COUNT || n <= 0 || n > SPAWN_MAX) return false;
		counts[k] = (int32_t)n;
	}
	return true;
}

static Transfer *add_transfer(Ledger *l, uint64_t id, const char *src, const char *dst, uint8_t faction, bool seed,
                              const int32_t counts[UNIT_TYPE_COUNT])
{
	if (l->transfer_count == COORD_MAX_TRANSFERS) fatal("coordinator: too many transfers");
	Transfer *t = &l->transfers[l->transfer_count++];
	*t = (Transfer){.id = id, .faction = faction, .seed = seed};
	snprintf(t->src, sizeof t->src, "%s", src);
	snprintf(t->dst, sizeof t->dst, "%s", dst);
	memcpy(t->counts, counts, sizeof t->counts);
	if (id >= l->next_id) l->next_id = id + 1;
	// Into the pool: arrived the moment it's recorded.
	if (!strcmp(dst, "pool")) {
		for (int k = 0; k < UNIT_TYPE_COUNT; k++) l->pool[faction][k] += counts[k];
		t->delivered = true;
	}
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
		if (!strcmp(verb, "transfer")) {
			char *src = strtok_r(nullptr, " ", &save), *dst = strtok_r(nullptr, " ", &save);
			char *fac = strtok_r(nullptr, " ", &save), *kind = strtok_r(nullptr, " ", &save);
			if (!src || !dst || !fac || !kind || !parse_counts(&save, counts)) return false;
			long faction = strtol(fac, nullptr, 10);
			if (faction < 0 || faction >= FACTION_COUNT) return false;
			add_transfer(l, id, src, dst, (uint8_t)faction, !strcmp(kind, "seed"), counts);
		} else if (!strcmp(verb, "delivered")) {
			Transfer *t = ledger_find(l, id);
			if (!t) return false;
			t->delivered = true;
		} else if (!strcmp(verb, "draw")) {
			char *fac = strtok_r(nullptr, " ", &save), *zone = strtok_r(nullptr, " ", &save);
			if (!fac || !zone || !parse_counts(&save, counts)) return false;
			long faction = strtol(fac, nullptr, 10);
			if (faction < 0 || faction >= FACTION_COUNT) return false;
			for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
				if (counts[k] > l->pool[faction][k]) return false; // the log can't overdraw
				l->pool[faction][k] -= counts[k];
			}
			add_transfer(l, id, "pool", zone, (uint8_t)faction, false, counts);
		} else {
			return false;
		}
	}
	return true;
}

Transfer *ledger_depart(Ledger *l, const char *src, const char *dst, uint8_t faction, bool seed,
                        const int32_t counts[UNIT_TYPE_COUNT])
{
	char line[512], list[384];
	format_counts(list, sizeof list, counts);
	snprintf(line, sizeof line, "transfer %llu %s %s %u %s%s", (unsigned long long)l->next_id, src, dst, faction,
	         seed ? "seed" : "units", list);
	log_line(l, line);
	return add_transfer(l, l->next_id, src, dst, faction, seed, counts);
}

Transfer *ledger_draw(Ledger *l, uint8_t faction, const char *zone, const int32_t want[UNIT_TYPE_COUNT])
{
	int32_t take_n[UNIT_TYPE_COUNT];
	int32_t total = 0;
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		take_n[k] = want[k] < l->pool[faction][k] ? want[k] : l->pool[faction][k];
		total += take_n[k];
	}
	if (total == 0) return nullptr;
	char line[512], list[384];
	format_counts(list, sizeof list, take_n);
	snprintf(line, sizeof line, "draw %llu %u %s%s", (unsigned long long)l->next_id, faction, zone, list);
	log_line(l, line);
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) l->pool[faction][k] -= take_n[k];
	return add_transfer(l, l->next_id, "pool", zone, faction, false, take_n);
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
