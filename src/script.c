#include "script.h"

#include "platform.h"

enum { MAX_TOKENS = 64, MAX_LINE = 512, MAX_GROUPS = 64 };

// ---------------------------------------------------------------- parsing

typedef struct {
	Script *s;
	Arena *arena;
	const char *path;
	int32_t line;
	char *err;
	ptrdiff_t err_len;
	ScriptAction *actions;
	ptrdiff_t action_cap;
	bool explicit_clip, explicit_end;
} Parser;

static bool bad(Parser *p, const char *why, const char *what)
{
	snprintf(p->err, (size_t)p->err_len, "%s:%d: %s%s%s", p->path, p->line, why,
	         what ? ": " : "", what ? what : "");
	return false;
}

static const char *copy_str(Arena *a, const char *s)
{
	ptrdiff_t n = (ptrdiff_t)strlen(s);
	char *c = arena_push(a, n + 1, 1);
	memcpy(c, s, (size_t)n);
	return c;
}

static bool parse_i64(const char *t, int64_t *out)
{
	char *end;
	long long v = strtoll(t, &end, 10);
	if (end == t || *end) return false;
	*out = v;
	return true;
}

static bool parse_f(const char *t, float *out)
{
	char *end;
	float v = strtof(t, &end);
	if (end == t || *end || !(v == v) || v - v != 0.0f) return false;
	*out = v;
	return true;
}

static bool parse_point(const char *t, V2 *out)
{
	char buf[64];
	const char *comma = strchr(t, ',');
	if (!comma || comma - t >= (ptrdiff_t)sizeof buf) return false;
	memcpy(buf, t, (size_t)(comma - t));
	buf[comma - t] = 0;
	return parse_f(buf, &out->x) && parse_f(comma + 1, &out->z);
}

static bool parse_side(const char *t, FactionId *out)
{
	if (!strcmp(t, "blue")) *out = (FactionId){0};
	else if (!strcmp(t, "red")) *out = (FactionId){1};
	else return false;
	return true;
}

// -1 for "any".
static bool parse_type(const char *t, int32_t *out, bool allow_any)
{
	if (allow_any && !strcmp(t, "any")) {
		*out = -1;
		return true;
	}
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) {
		if (!strcmp(t, unit_defs[k].name)) {
			*out = k;
			return true;
		}
	}
	return false;
}

static bool parse_on(const char *t, bool *out)
{
	if (!strcmp(t, "on")) *out = true;
	else if (!strcmp(t, "off")) *out = false;
	else return false;
	return true;
}

// A zone by name, as numbered by the script's `zones` line, or the pool.
static bool parse_zone(Parser *p, const char *name, int32_t *out)
{
	if (!strcmp(name, "pool")) {
		*out = ZONE_POOL;
		return true;
	}
	for (int32_t k = 0; k < p->s->zone_count; k++)
		if (!strcmp(p->s->zones[k], name)) {
			*out = k;
			return true;
		}
	return false;
}

static int32_t group_index(Parser *p, const char *name, bool create)
{
	Script *s = p->s;
	for (ptrdiff_t g = 0; g < s->group_count; g++)
		if (!strcmp(s->groups[g], name)) return (int32_t)g;
	if (!create || s->group_count == MAX_GROUPS) return -1;
	s->groups[s->group_count] = copy_str(p->arena, name);
	return (int32_t)s->group_count++;
}

static ScriptAction *new_action(Parser *p, int64_t tick, ActionKind kind, const char *text)
{
	if (p->s->action_count == p->action_cap) return nullptr;
	ScriptAction *a = &p->actions[p->s->action_count++];
	*a = (ScriptAction){.tick = tick, .line = p->line, .kind = kind, .group = -1,
	                    .text = copy_str(p->arena, text)};
	return a;
}

// "at T ..." — tok[0] is the verb, n tokens.
static bool parse_action(Parser *p, int64_t tick, char **tok, int n, const char *text)
{
	const char *verb = tok[0];
	ScriptAction *a;
	if (!strcmp(verb, "select")) {
		// select SIDE TYPE [within R of X,Z] [max N] as NAME
		if (!(a = new_action(p, tick, ACTION_SELECT, text))) return bad(p, "too many actions", nullptr);
		if (n < 5) return bad(p, "select SIDE TYPE [within R of X,Z] [max N] as NAME", nullptr);
		if (!parse_side(tok[1], &a->side)) return bad(p, "side is blue or red", tok[1]);
		if (!parse_type(tok[2], &a->type, true)) return bad(p, "unknown unit type", tok[2]);
		int i = 3;
		if (i + 3 < n && !strcmp(tok[i], "within")) {
			if (!parse_f(tok[i + 1], &a->radius) || a->radius <= 0.0f || strcmp(tok[i + 2], "of")
			    || !parse_point(tok[i + 3], &a->at))
				return bad(p, "within R of X,Z", nullptr);
			a->area = true;
			i += 4;
		}
		if (i + 1 < n && !strcmp(tok[i], "max")) {
			int64_t m;
			if (!parse_i64(tok[i + 1], &m) || m <= 0 || m > 1 << 20) return bad(p, "max N", tok[i + 1]);
			a->limit = (int32_t)m;
			i += 2;
		}
		if (i + 2 != n || strcmp(tok[i], "as")) return bad(p, "select must end with: as NAME", nullptr);
		if ((a->group = group_index(p, tok[i + 1], true)) < 0) return bad(p, "too many groups", nullptr);
		return true;
	}
	if (!strcmp(verb, "move") || !strcmp(verb, "attack-move") || !strcmp(verb, "stop")) {
		bool stop = !strcmp(verb, "stop");
		if (n != (stop ? 2 : 3)) return bad(p, stop ? "stop NAME" : "move|attack-move NAME X,Z", nullptr);
		if (!(a = new_action(p, tick, ACTION_ORDER, text))) return bad(p, "too many actions", nullptr);
		if ((a->group = group_index(p, tok[1], false)) < 0) return bad(p, "no group selected as", tok[1]);
		a->command = stop ? COMMAND_STOP : !strcmp(verb, "move") ? COMMAND_MOVE : COMMAND_ATTACK_MOVE;
		if (!stop && !parse_point(tok[2], &a->at)) return bad(p, "target is X,Z", tok[2]);
		return true;
	}
	if (!strcmp(verb, "build") || !strcmp(verb, "queue")) {
		// build|queue GROUP TYPE at X,Z
		if (n != 5 || strcmp(tok[3], "at")) return bad(p, "build|queue GROUP TYPE at X,Z", nullptr);
		ActionKind kind = !strcmp(verb, "queue") ? ACTION_QUEUE : ACTION_ORDER;
		if (!(a = new_action(p, tick, kind, text))) return bad(p, "too many actions", nullptr);
		if ((a->group = group_index(p, tok[1], false)) < 0) return bad(p, "no group selected as", tok[1]);
		if (!parse_type(tok[2], &a->type, false) || !unit_is_structure((UnitType)a->type))
			return bad(p, "not a structure", tok[2]);
		if (!parse_point(tok[4], &a->at)) return bad(p, "target is X,Z", tok[4]);
		a->command = COMMAND_BUILD;
		return true;
	}
	if (!strcmp(verb, "produce")) {
		// produce GROUP TYPE [N] [repeat]
		if (n < 3 || n > 5) return bad(p, "produce GROUP TYPE [N] [repeat]", nullptr);
		if (!(a = new_action(p, tick, ACTION_ORDER, text))) return bad(p, "too many actions", nullptr);
		if ((a->group = group_index(p, tok[1], false)) < 0) return bad(p, "no group selected as", tok[1]);
		if (!parse_type(tok[2], &a->type, false) || unit_is_structure((UnitType)a->type))
			return bad(p, "factories make units, not structures", tok[2]);
		a->command = COMMAND_PRODUCE;
		a->limit = 1;
		for (int i = 3; i < n; i++) {
			int64_t v;
			if (!strcmp(tok[i], "repeat")) a->flags = COMMAND_REPEAT;
			else if (parse_i64(tok[i], &v) && v >= 0 && v <= FACTORY_QUEUE) a->limit = (int32_t)v;
			else return bad(p, "produce GROUP TYPE [N 0..8] [repeat]", tok[i]);
		}
		return true;
	}
	if (!strcmp(verb, "gate") || !strcmp(verb, "seed")) {
		// gate GROUP to ZONE via X,Z | seed GROUP to ZONE
		bool gate = !strcmp(verb, "gate");
		if (n != (gate ? 6 : 4) || strcmp(tok[2], "to") || (gate && strcmp(tok[4], "via")))
			return bad(p, gate ? "gate GROUP to ZONE via X,Z" : "seed GROUP to ZONE", nullptr);
		if (!(a = new_action(p, tick, ACTION_ORDER, text))) return bad(p, "too many actions", nullptr);
		if ((a->group = group_index(p, tok[1], false)) < 0) return bad(p, "no group selected as", tok[1]);
		if (!parse_zone(p, tok[3], &a->limit)) return bad(p, "unknown zone (list them with: zones A B ...)", tok[3]);
		if (!gate && a->limit == ZONE_POOL) return bad(p, "a seed needs a zone, not the pool", nullptr);
		if (gate && !parse_point(tok[5], &a->at)) return bad(p, "via X,Z", tok[5]);
		a->command = gate ? COMMAND_GATE : COMMAND_SEED;
		a->type = UNIT_GATE;
		return true;
	}
	if (!strcmp(verb, "arrive")) {
		// arrive SIDE TYPE N
		int64_t amount;
		if (n != 4) return bad(p, "arrive SIDE TYPE N", nullptr);
		if (!(a = new_action(p, tick, ACTION_SPAWN, text))) return bad(p, "too many actions", nullptr);
		if (!parse_side(tok[1], &a->side)) return bad(p, "side is blue or red", tok[1]);
		if (!parse_type(tok[2], &a->type, false) || unit_is_structure((UnitType)a->type))
			return bad(p, "units arrive, not structures", tok[2]);
		if (!parse_i64(tok[3], &amount) || amount <= 0 || amount > SPAWN_MAX) return bad(p, "amount out of range", tok[3]);
		a->limit = (int32_t)amount;
		a->flags = 0xff; // marks an arrival rather than a spawn
		return true;
	}
	if (!strcmp(verb, "entrench") || !strcmp(verb, "mobilize")) {
		// entrench SIDE | mobilize SIDE: what logging out and back in do
		if (n != 2) return bad(p, "entrench|mobilize SIDE", nullptr);
		if (!(a = new_action(p, tick, ACTION_SPAWN, text))) return bad(p, "too many actions", nullptr);
		if (!parse_side(tok[1], &a->side)) return bad(p, "side is blue or red", tok[1]);
		a->flags = !strcmp(verb, "entrench") ? 0xfe : 0xfd;
		return true;
	}
	if (!strcmp(verb, "spawn")) {
		// spawn SIDE TYPE N at X,Z [radius R]
		if (n != 6 && n != 8) return bad(p, "spawn SIDE TYPE N at X,Z [radius R]", nullptr);
		if (!(a = new_action(p, tick, ACTION_SPAWN, text))) return bad(p, "too many actions", nullptr);
		int64_t amount;
		if (!parse_side(tok[1], &a->side)) return bad(p, "side is blue or red", tok[1]);
		if (!parse_type(tok[2], &a->type, false)) return bad(p, "unknown unit type", tok[2]);
		if (!parse_i64(tok[3], &amount) || amount <= 0 || amount > SPAWN_MAX)
			return bad(p, "amount out of range", tok[3]);
		a->limit = (int32_t)amount;
		if (strcmp(tok[4], "at") || !parse_point(tok[5], &a->at)) return bad(p, "at X,Z", nullptr);
		a->radius = 8.0f;
		if (n == 8 && (strcmp(tok[6], "radius") || !parse_f(tok[7], &a->radius) || a->radius < 0.0f
		               || a->radius > SPAWN_MAX_RADIUS))
			return bad(p, "radius R", nullptr);
		return true;
	}
	if (!strcmp(verb, "bot")) {
		if (n != 3) return bad(p, "bot SIDE on|off", nullptr);
		if (!(a = new_action(p, tick, ACTION_BOT, text))) return bad(p, "too many actions", nullptr);
		if (!parse_side(tok[1], &a->side) || !parse_on(tok[2], &a->on)) return bad(p, "bot SIDE on|off", nullptr);
		return true;
	}
	if (!strcmp(verb, "expect")) {
		// expect (SIDE FIELD | GROUP alive) OP NUMBER
		if (n != 5) return bad(p, "expect SIDE|GROUP FIELD OP NUMBER", nullptr);
		if (!(a = new_action(p, tick, ACTION_EXPECT, text))) return bad(p, "too many actions", nullptr);
		static const char *fields[] = {"units", "lost", "fired", "shells", "spawned", "alive",
		                               "metal", "energy", "metal_income", "energy_income",
		                               "", "departed", "arrived"};
		static const char *counts[UNIT_TYPE_COUNT] = {"raiders", "tanks", "artillery", "engineers",
		                                              "extractors", "generators", "factories", "gates",
		                                              "turrets"};
		static const char *ops[] = {"<", "<=", ">", ">=", "==", "!="};
		int f = -1, o = -1;
		for (int k = 0; k < (int)countof(fields); k++) if (!strcmp(tok[2], fields[k])) f = k;
		for (int k = 0; k < UNIT_TYPE_COUNT; k++)
			if (!strcmp(tok[2], counts[k])) {
				f = FIELD_COUNT;
				a->type = k;
			}
		for (int k = 0; k < (int)countof(ops); k++) if (!strcmp(tok[3], ops[k])) o = k;
		if (f < 0) return bad(p, "unknown field (see script.h)", tok[2]);
		if (o < 0) return bad(p, "operator is < <= > >= == !=", tok[3]);
		a->field = (ExpectField)f;
		a->op = (ExpectOp)o;
		if (a->field == FIELD_ALIVE) {
			if ((a->group = group_index(p, tok[1], false)) < 0) return bad(p, "no group selected as", tok[1]);
		} else if (!parse_side(tok[1], &a->side)) {
			return bad(p, "side is blue or red (groups only have: alive)", tok[1]);
		}
		char *end;
		a->value = strtod(tok[4], &end);
		if (end == tok[4] || *end) return bad(p, "not a number", tok[4]);
		return true;
	}
	return bad(p, "unknown action", verb);
}

static bool parse_line(Parser *p, char *line)
{
	char text[MAX_LINE];
	snprintf(text, sizeof text, "%s", line);
	char *hash = strchr(line, '#');
	if (hash) *hash = 0;
	char *tok[MAX_TOKENS];
	int n = 0;
	for (char *c = line; *c;) {
		while (*c == ' ' || *c == '\t' || *c == '\r') *c++ = 0;
		if (!*c) break;
		if (n == MAX_TOKENS) return bad(p, "too many words on the line", nullptr);
		tok[n++] = c;
		while (*c && *c != ' ' && *c != '\t' && *c != '\r') c++;
	}
	if (n == 0) return true;
	Script *s = p->s;
	const char *key = tok[0];
	int64_t v;

	if (!strcmp(key, "title") || !strcmp(key, "about")) {
		// The rest of the original line, comment and all trimmed.
		const char *rest = strstr(text, key) + strlen(key);
		while (*rest == ' ' || *rest == '\t') rest++;
		char buf[MAX_LINE];
		snprintf(buf, sizeof buf, "%s", rest);
		char *h = strchr(buf, '#');
		if (h) *h = 0;
		for (ptrdiff_t k = (ptrdiff_t)strlen(buf) - 1; k >= 0 && (buf[k] == ' ' || buf[k] == '\r'); k--) buf[k] = 0;
		if (!strcmp(key, "title")) s->title = copy_str(p->arena, buf);
		else s->about = copy_str(p->arena, buf);
		return true;
	}
	if (!strcmp(key, "scenario")) {
		if (n != 2 || !(s->scenario = scenario_find(tok[1]))) return bad(p, "unknown scenario", n > 1 ? tok[1] : nullptr);
		if (p->s->action_count > 0) return bad(p, "scenario must come before actions", nullptr);
		s->config = scenario_config(s->scenario);
		if (!p->explicit_clip) {
			s->clip_from = s->scenario->clip_from;
			s->clip_to = s->scenario->clip_to;
		}
		return true;
	}
	if (!strcmp(key, "seed")) {
		if (n != 2 || !parse_i64(tok[1], &v) || v < 0) return bad(p, "seed N", nullptr);
		s->config.seed = (uint64_t)v;
		return true;
	}
	if (!strcmp(key, "units")) {
		if (n != 2 || !parse_i64(tok[1], &v) || v < 0 || v > 1 << 16) return bad(p, "units N (0..65536)", nullptr);
		s->config.initial_units = s->config.population = (int32_t)v;
		if (s->config.max_units < v) s->config.max_units = (int32_t)v;
		return true;
	}
	if (!strcmp(key, "weights")) {
		if (n != 1 + COMBAT_TYPE_COUNT) return bad(p, "weights RAIDER TANK ARTILLERY", nullptr);
		int64_t sum = 0;
		for (int k = 0; k < COMBAT_TYPE_COUNT; k++) {
			if (!parse_i64(tok[1 + k], &v) || v < 0 || v > 1000) return bad(p, "weight 0..1000", tok[1 + k]);
			s->config.type_weight[k] = (int32_t)v;
			sum += v;
		}
		if (sum == 0) return bad(p, "weights can't all be zero", nullptr);
		return true;
	}
	if (!strcmp(key, "zones")) {
		if (n < 2 || n - 1 > (int)countof(s->zones)) return bad(p, "zones NAME... (up to 8)", nullptr);
		if (p->s->action_count > 0) return bad(p, "zones must come before actions", nullptr);
		for (int k = 1; k < n; k++) s->zones[k - 1] = copy_str(p->arena, tok[k]);
		s->zone_count = n - 1;
		return true;
	}
	if (!strcmp(key, "economy")) {
		bool on;
		if (n != 2 || !parse_on(tok[1], &on)) return bad(p, "economy on|off", nullptr);
		s->config.economy = on;
		return true;
	}
	if (!strcmp(key, "respawn")) {
		bool on;
		if (n != 2 || !parse_on(tok[1], &on)) return bad(p, "respawn on|off", nullptr);
		s->config.spawn_interval = on ? 1 : 1 << 30;
		return true;
	}
	if (!strcmp(key, "bot")) {
		FactionId side;
		bool on;
		if (n != 3 || !parse_side(tok[1], &side) || !parse_on(tok[2], &on)) return bad(p, "bot SIDE on|off", nullptr);
		s->bots_on[side.v] = on;
		return true;
	}
	if (!strcmp(key, "clip")) {
		const char *dots = n == 2 ? strstr(tok[1], "..") : nullptr;
		if (!dots) return bad(p, "clip FROM..TO", nullptr);
		*(char *)dots = 0;
		int64_t a, b;
		if (!parse_i64(tok[1], &a) || !parse_i64(dots + 2, &b) || a < 0 || b <= a)
			return bad(p, "clip FROM..TO with FROM < TO", nullptr);
		s->clip_from = a;
		s->clip_to = b;
		p->explicit_clip = true;
		return true;
	}
	if (!strcmp(key, "camera")) {
		if (n == 2 && !strcmp(tok[1], "director")) {
			s->fixed_camera = false;
			return true;
		}
		if (n != 3 || !parse_point(tok[1], &s->camera_at) || !parse_f(tok[2], &s->camera_distance)
		    || s->camera_distance < 10.0f)
			return bad(p, "camera director | camera X,Z DISTANCE", nullptr);
		s->fixed_camera = true;
		return true;
	}
	if (!strcmp(key, "speed")) {
		if (n != 2 || !parse_i64(tok[1], &v) || v < 1 || v > 64) return bad(p, "speed N (1..64)", nullptr);
		s->speed = (int32_t)v;
		return true;
	}
	if (!strcmp(key, "end")) {
		if (n != 2 || !parse_i64(tok[1], &v) || v <= 0) return bad(p, "end TICK", nullptr);
		s->end = v;
		p->explicit_end = true;
		return true;
	}
	if (!strcmp(key, "at")) {
		if (n < 3 || !parse_i64(tok[1], &v) || v < 0) return bad(p, "at TICK ACTION ...", nullptr);
		// The action as written, without the comment, for messages.
		char *action_text = strstr(text, tok[2]);
		if (!action_text) action_text = text;
		char *h = strchr(action_text, '#');
		if (h) *h = 0;
		for (ptrdiff_t k = (ptrdiff_t)strlen(action_text) - 1;
		     k >= 0 && (action_text[k] == ' ' || action_text[k] == '\t' || action_text[k] == '\r'); k--)
			action_text[k] = 0;
		return parse_action(p, v, tok + 2, n - 2, action_text);
	}
	return bad(p, "unknown directive", key);
}

bool script_parse(Script *s, Arena *arena, Str text, const char *path, char *err, ptrdiff_t err_len)
{
	ptrdiff_t lines = 1;
	for (ptrdiff_t i = 0; i < text.len; i++) lines += text.ptr[i] == '\n';
	const Scenario *def = scenario_find("skirmish");
	ASSERT(def); // the default every script starts from
	*s = (Script){
		.path = path,
		.title = "",
		.about = "",
		.scenario = def,
		.config = scenario_config(def),
		.bots_on = {true, true},
		.groups = PUSH(arena, const char *, MAX_GROUPS),
		.clip_from = def->clip_from,
		.clip_to = def->clip_to,
	};
	Parser p = {.s = s, .arena = arena, .path = path, .err = err, .err_len = err_len,
	            .actions = PUSH(arena, ScriptAction, lines), .action_cap = lines};

	for (ptrdiff_t i = 0; i < text.len;) {
		ptrdiff_t j = i;
		while (j < text.len && text.ptr[j] != '\n') j++;
		p.line++;
		char line[MAX_LINE];
		if (j - i >= MAX_LINE) return bad(&p, "line too long", nullptr);
		memcpy(line, text.ptr + i, (size_t)(j - i));
		line[j - i] = 0;
		if (!parse_line(&p, line)) return false;
		i = j + 1;
	}

	// Stable sort by tick: actions at one tick run in file order.
	ScriptAction *a = p.actions;
	for (ptrdiff_t i = 1; i < s->action_count; i++) {
		ScriptAction x = a[i];
		ptrdiff_t j = i;
		for (; j > 0 && a[j - 1].tick > x.tick; j--) a[j] = a[j - 1];
		a[j] = x;
	}
	s->actions = a;
	if (!p.explicit_end) {
		int64_t last = s->clip_to;
		for (ptrdiff_t i = 0; i < s->action_count; i++) last = a[i].tick > last ? a[i].tick : last;
		s->end = last + 1;
	}
	return true;
}

bool script_load(Script *s, Arena *arena, const char *path, char *err, ptrdiff_t err_len)
{
	Str text;
	if (!platform_read_file(arena, path, &text)) {
		snprintf(err, (size_t)err_len, "%s: cannot read", path);
		return false;
	}
	return script_parse(s, arena, text, path, err, err_len);
}

// ---------------------------------------------------------------- running

void script_run_init(ScriptRun *r, const Script *s, Arena *arena, FILE *log)
{
	*r = (ScriptRun){.script = s, .log = log};
	scenario_bots(s->scenario, r->bots);
	for (int k = 0; k < FACTION_COUNT; k++) r->bots[k].enabled = s->bots_on[k];
	r->group_units = PUSH(arena, UnitHandle *, s->group_count);
	r->group_len = PUSH(arena, ptrdiff_t, s->group_count);
	r->group_side = PUSH(arena, FactionId, s->group_count);
	r->queued = PUSH(arena, const ScriptAction **, s->group_count);
	r->queued_head = PUSH(arena, int32_t, s->group_count);
	r->queued_len = PUSH(arena, int32_t, s->group_count);
	for (ptrdiff_t g = 0; g < s->group_count; g++) {
		r->group_units[g] = PUSH(arena, UnitHandle, s->config.max_units);
		r->queued[g] = PUSH(arena, const ScriptAction *, s->action_count > 0 ? s->action_count : 1);
	}
}

static Command build_command(const ScriptRun *r, const ScriptAction *a, Arena *tick_arena)
{
	return (Command){
		.faction = r->group_side[a->group],
		.kind = COMMAND_BUILD,
		.unit_type = (UnitType)a->type,
		.target = a->at,
		.count = r->group_len[a->group],
		.units = arena_copy(tick_arena, r->group_units[a->group],
		                    r->group_len[a->group] * (ptrdiff_t)sizeof(UnitHandle), alignof(UnitHandle)),
	};
}

// A group is busy while any living engineer in it is still on a build.
static bool group_building(const ScriptRun *r, int32_t g, const Sim *sim)
{
	const Units *u = &sim->factions[r->group_side[g].v].units;
	for (ptrdiff_t i = 0; i < r->group_len[g]; i++) {
		ptrdiff_t d = units_find(u, r->group_units[g][i]);
		if (d >= 0 && u->order[d] == ORDER_BUILD) return true;
	}
	return false;
}

typedef struct {
	float d2;
	int32_t dense;
} Candidate;

static int by_distance(const void *a, const void *b)
{
	const Candidate *x = a, *y = b;
	if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
	return (x->dense > y->dense) - (x->dense < y->dense);
}

static void run_select(ScriptRun *r, const ScriptAction *a, const Sim *sim, Arena *scratch)
{
	const Units *u = &sim->factions[a->side.v].units;
	Candidate *c = PUSH(scratch, Candidate, u->count);
	ptrdiff_t n = 0;
	for (ptrdiff_t i = 0; i < u->count; i++) {
		if (a->type >= 0 && u->type[i] != a->type) continue;
		float d2 = a->area ? v2_len2(v2_sub(v2(u->px[i], u->pz[i]), a->at)) : 0.0f;
		if (a->area && d2 > a->radius * a->radius) continue;
		c[n++] = (Candidate){d2, (int32_t)i};
	}
	if (a->limit > 0 && n > a->limit) {
		qsort(c, (size_t)n, sizeof *c, by_distance);
		n = a->limit;
	}
	for (ptrdiff_t i = 0; i < n; i++) r->group_units[a->group][i] = units_handle(u, c[i].dense);
	r->group_len[a->group] = n;
	r->group_side[a->group] = a->side;
	if (n == 0 && r->log)
		fprintf(r->log, "  note line %d at %lld: selection is empty: %s\n", a->line, (long long)a->tick, a->text);
}

static ptrdiff_t group_alive(const ScriptRun *r, int32_t g, const Sim *sim)
{
	const Units *u = &sim->factions[r->group_side[g].v].units;
	ptrdiff_t alive = 0;
	for (ptrdiff_t i = 0; i < r->group_len[g]; i++) alive += units_find(u, r->group_units[g][i]) >= 0;
	return alive;
}

static void run_expect(ScriptRun *r, const ScriptAction *a, const Sim *sim)
{
	double actual = 0.0;
	if (a->field == FIELD_ALIVE) {
		actual = (double)group_alive(r, a->group, sim);
	} else {
		const Faction *f = &sim->factions[a->side.v];
		switch (a->field) {
		case FIELD_UNITS: actual = (double)f->units.count; break;
		case FIELD_LOST: actual = (double)f->stats.units_lost; break;
		case FIELD_FIRED: actual = (double)f->stats.shots_fired; break;
		case FIELD_SHELLS: actual = (double)f->incoming.count; break;
		case FIELD_SPAWNED: actual = (double)f->stats.units_spawned; break;
		case FIELD_METAL: actual = (double)f->economy.metal; break;
		case FIELD_ENERGY: actual = (double)f->economy.energy; break;
		case FIELD_METAL_INCOME: actual = (double)f->economy.metal_income; break;
		case FIELD_ENERGY_INCOME: actual = (double)f->economy.energy_income; break;
		case FIELD_COUNT:
			for (ptrdiff_t i = 0; i < f->units.count; i++)
				actual += f->units.type[i] == a->type && f->units.progress[i] >= 1.0f;
			break;
		case FIELD_DEPARTED: actual = (double)f->stats.units_departed; break;
		case FIELD_ARRIVED: actual = (double)f->stats.units_arrived; break;
		case FIELD_ALIVE: break;
		}
	}
	bool ok = false;
	switch (a->op) {
	case OP_LT: ok = actual < a->value; break;
	case OP_LE: ok = actual <= a->value; break;
	case OP_GT: ok = actual > a->value; break;
	case OP_GE: ok = actual >= a->value; break;
	case OP_EQ: ok = actual == a->value; break;
	case OP_NE: ok = actual != a->value; break;
	}
	r->expects++;
	r->failures += !ok;
	if (r->log)
		fprintf(r->log, "  %s line %d at %lld: %s (actual %g)\n", ok ? "ok  " : "FAIL", a->line,
		        (long long)a->tick, a->text, actual);
}

ptrdiff_t script_tick(ScriptRun *r, const Sim *sim, Arena *tick_arena, Command *out, ptrdiff_t cap)
{
	const Script *s = r->script;
	ptrdiff_t n = 0;
	// Actions behind the current tick (a run that started late) are skipped.
	while (r->next < s->action_count && s->actions[r->next].tick < sim->tick.v) r->next++;
	for (; r->next < s->action_count && s->actions[r->next].tick == sim->tick.v; r->next++) {
		const ScriptAction *a = &s->actions[r->next];
		switch (a->kind) {
		case ACTION_SELECT:
			run_select(r, a, sim, tick_arena);
			break;
		case ACTION_ORDER:
			if (n == cap || r->group_len[a->group] == 0) break;
			if (a->command == COMMAND_SEED) {
				// The seed fires from the gate the group's first live unit is.
				const Units *u = &sim->factions[r->group_side[a->group].v].units;
				ptrdiff_t d = -1;
				for (ptrdiff_t i = 0; i < r->group_len[a->group] && d < 0; i++) d = units_find(u, r->group_units[a->group][i]);
				if (d < 0) break;
				out[n++] = (Command){.faction = r->group_side[a->group], .kind = COMMAND_SEED,
				                     .target = v2(u->px[d], u->pz[d]), .amount = a->limit};
				break;
			}
			out[n++] = (Command){
				.faction = r->group_side[a->group],
				.kind = a->command,
				.unit_type = a->type >= 0 ? (UnitType)a->type : UNIT_RAIDER,
				.amount = a->limit,
				.flags = a->flags,
				.target = a->at,
				.count = r->group_len[a->group],
				.units = arena_copy(tick_arena, r->group_units[a->group],
				                    r->group_len[a->group] * (ptrdiff_t)sizeof(UnitHandle),
				                    alignof(UnitHandle)),
			};
			break;
		case ACTION_SPAWN:
			if (n == cap) break;
			if (a->flags == 0xfe || a->flags == 0xfd) {
				out[n++] = (Command){.faction = a->side, .kind = a->flags == 0xfe ? COMMAND_ENTRENCH : COMMAND_MOBILIZE};
				break;
			}
			if (a->flags == 0xff) {
				out[n++] = (Command){.faction = a->side, .kind = COMMAND_ARRIVE, .unit_type = (UnitType)a->type,
				                     .amount = a->limit};
				break;
			}
			out[n++] = (Command){.faction = a->side, .kind = COMMAND_SPAWN, .target = a->at,
			                     .unit_type = (UnitType)a->type, .amount = a->limit, .radius = a->radius};
			break;
		case ACTION_QUEUE:
			r->queued[a->group][r->queued_head[a->group] + r->queued_len[a->group]++] = a;
			break;
		case ACTION_BOT:
			r->bots[a->side.v].enabled = a->on;
			break;
		case ACTION_EXPECT:
			run_expect(r, a, sim);
			break;
		}
	}
	// Groups that finished their last build start the next queued one.
	for (ptrdiff_t g = 0; g < s->group_count; g++) {
		if (r->queued_len[g] == 0 || n == cap || r->group_len[g] == 0 || group_building(r, (int32_t)g, sim)) continue;
		out[n++] = build_command(r, r->queued[g][r->queued_head[g]], tick_arena);
		r->queued_head[g]++;
		r->queued_len[g]--;
	}
	for (int k = 0; k < FACTION_COUNT; k++) bot_think(&r->bots[k], sim, tick_arena, out, &n, cap);
	return n;
}
