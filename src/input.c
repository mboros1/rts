#include "input.h"

#include "keymap.h"
#include "platform.h"

void input_from_raylib(Input *in)
{
	*in = (Input){.mouse = GetMousePosition(), .mouse_delta = GetMouseDelta(), .wheel = GetMouseWheelMove(),
	              .width = GetScreenWidth(), .height = GetScreenHeight(), .dt = GetFrameTime()};
	for (int k = 0; k < INPUT_KEYS; k++) {
		in->pressed[k] = IsKeyPressed(k);
		in->down[k] = IsKeyDown(k);
	}
	const int buttons[3] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE};
	for (int b = 0; b < 3; b++) {
		in->click[b] = IsMouseButtonPressed(buttons[b]);
		in->held[b] = IsMouseButtonDown(buttons[b]);
		in->release[b] = IsMouseButtonReleased(buttons[b]);
	}
}

// ---------------------------------------------------------------- scripts

// Orders by name, for `expect sent NAME`.
static const char *const command_names[COMMAND_KIND_COUNT] = {
	[COMMAND_STOP] = "stop", [COMMAND_MOVE] = "move", [COMMAND_ATTACK_MOVE] = "fight", [COMMAND_SPAWN] = "spawn",
	[COMMAND_BUILD] = "build", [COMMAND_PRODUCE] = "produce", [COMMAND_SEND] = "send", [COMMAND_ARRIVE] = "arrive",
	[COMMAND_ENTRENCH] = "entrench", [COMMAND_MOBILIZE] = "mobilize", [COMMAND_GIVE] = "give",
	[COMMAND_OUTPUT] = "output", [COMMAND_SHARE] = "share", [COMMAND_AI_SLOTS] = "ai-slots", [COMMAND_TAKE] = "take",
	[COMMAND_SET_REPEAT] = "repeat", [COMMAND_WARP] = "warp",
};

static bool parse_xy(const char *t, Vector2 *out)
{
	float x, y;
	char end;
	if (sscanf(t, "%f,%f%c", &x, &y, &end) != 2) return false;
	*out = (Vector2){x, y};
	return true;
}

static const char *keep(Arena *a, const char *text)
{
	size_t n = strlen(text) + 1;
	char *out = PUSH(a, char, (ptrdiff_t)n);
	memcpy(out, text, n);
	return out;
}

static int compare_steps(const void *a, const void *b)
{
	const InputStep *x = a, *y = b;
	if (x->at != y->at) return x->at < y->at ? -1 : 1;
	return (x->line > y->line) - (x->line < y->line);
}

bool input_script_load(InputScript *s, Arena *arena, const char *path, char *err, size_t err_len)
{
	*s = (InputScript){0};
	Str text;
	if (!platform_read_file(arena, path, &text)) {
		snprintf(err, err_len, "%s: cannot read", path);
		return false;
	}
	ptrdiff_t lines = 1;
	for (ptrdiff_t i = 0; i < text.len; i++) lines += text.ptr[i] == '\n';
	s->steps = PUSH(arena, InputStep, lines);
	int line_no = 0;
	for (ptrdiff_t i = 0; i < text.len;) {
		ptrdiff_t j = i;
		while (j < text.len && text.ptr[j] != '\n') j++;
		line_no++;
		char line[256];
		snprintf(line, sizeof line, "%.*s", (int)(j - i), text.ptr + i);
		i = j + 1;
		char *hash = strchr(line, '#');
		if (hash) *hash = 0;
		char copy[256];
		snprintf(copy, sizeof copy, "%s", line);
		char *tok[8], *save;
		int n = 0;
		for (char *t = strtok_r(line, " \t\r", &save); t && n < 8; t = strtok_r(nullptr, " \t\r", &save)) tok[n++] = t;
		if (n == 0) continue;
		InputStep st = {.line = line_no, .text = keep(arena, copy)};
		char *end;
		if (n < 3 || strcmp(tok[0], "at") || (st.at = strtod(tok[1], &end), *end) || st.at < 0.0) goto bad;
		const char *verb = tok[2];
		if (!strcmp(verb, "key") && n >= 4) {
			st.kind = STEP_KEY;
			st.key = keymap_key_of((Str){tok[3], (ptrdiff_t)strlen(tok[3])});
			if (st.key < 0) goto bad;
			for (int k = 4; k < n; k++) {
				if (!strcmp(tok[k], "shift")) st.shift = true;
				else if (!strcmp(tok[k], "ctrl")) st.ctrl = true;
				else goto bad;
			}
		} else if ((!strcmp(verb, "click") || !strcmp(verb, "move")) && n >= 4) {
			st.kind = !strcmp(verb, "click") ? STEP_CLICK : STEP_MOVE;
			if (!parse_xy(tok[3], &st.a)) goto bad;
			for (int k = 4; k < n; k++) {
				if (!strcmp(tok[k], "right")) st.key = 1;
				else if (!strcmp(tok[k], "middle")) st.key = 2;
				else if (!strcmp(tok[k], "shift")) st.shift = true;
				else if (!strcmp(tok[k], "ctrl")) st.ctrl = true;
				else goto bad;
			}
		} else if (!strcmp(verb, "drag") && (n == 5 || (n == 6 && !strcmp(tok[5], "shift")))) {
			st.kind = STEP_DRAG;
			st.shift = n == 6;
			if (!parse_xy(tok[3], &st.a) || !parse_xy(tok[4], &st.b)) goto bad;
		} else if (!strcmp(verb, "shot") && n == 4 && strlen(tok[3]) < sizeof st.name && !strchr(tok[3], '/')) {
			st.kind = STEP_SHOT;
			snprintf(st.name, sizeof st.name, "%s", tok[3]);
		} else if (!strcmp(verb, "expect") && n >= 6) {
			st.kind = STEP_EXPECT;
			int at = 3;
			if (!strcmp(tok[at], "sent") && n == 7) {
				st.watch = WATCH_SENT;
				st.command = -1;
				at++;
				if (strcmp(tok[at], "any")) {
					st.command = -2;
					for (int k = 0; k < COMMAND_KIND_COUNT; k++)
						if (command_names[k] && !strcmp(tok[at], command_names[k])) st.command = k;
					if (st.command == -2) goto bad;
				}
			} else if (!strcmp(tok[at], "selected") && n == 6) {
				st.watch = WATCH_SELECTED;
			} else if (!strcmp(tok[at], "busy") && n == 6) {
				st.watch = WATCH_BUSY;
			} else {
				goto bad;
			}
			static const char *ops[] = {"<", "<=", ">", ">=", "==", "!="};
			st.op = -1;
			for (int k = 0; k < 6; k++)
				if (!strcmp(tok[n - 2], ops[k])) st.op = k;
			st.value = strtod(tok[n - 1], &end);
			if (st.op < 0 || *end) goto bad;
		} else if (!strcmp(verb, "quit") && n == 3) {
			st.kind = STEP_QUIT;
		} else {
			goto bad;
		}
		s->steps[s->count++] = st;
		continue;
	bad:
		snprintf(err, err_len, "%s:%d: not a step (see input.h): %s", path, line_no, copy);
		return false;
	}
	qsort(s->steps, (size_t)s->count, sizeof *s->steps, compare_steps);
	return true;
}

static void hold_modifiers(Input *in, const InputStep *st)
{
	in->down[KEY_LEFT_SHIFT] = st->shift;
	in->down[KEY_LEFT_CONTROL] = st->ctrl;
}

void input_script_frame(InputScript *s, double t, int width, int height, float dt, Input *in, const InputStep **due,
                        bool *quit)
{
	*in = (Input){.mouse = s->mouse, .width = width, .height = height, .dt = dt};
	*due = nullptr;
	*quit = false;
	if (s->next == s->count || s->steps[s->next].at > t) return;
	const InputStep *st = &s->steps[s->next];
	hold_modifiers(in, st);
	// A step at a time; a click and a drag take a few frames, as a hand's would.
	switch (st->kind) {
	case STEP_KEY:
		in->pressed[st->key] = in->down[st->key] = true;
		s->next++;
		break;
	case STEP_MOVE:
		in->mouse_delta = (Vector2){st->a.x - s->mouse.x, st->a.y - s->mouse.y};
		in->mouse = s->mouse = st->a;
		s->next++;
		break;
	case STEP_CLICK:
		in->mouse = s->mouse = st->a;
		if (s->phase == 0) {
			in->click[st->key] = in->held[st->key] = true;
			s->phase = 1;
		} else {
			in->release[st->key] = true;
			s->phase = 0;
			s->next++;
		}
		break;
	case STEP_DRAG:
		if (s->phase == 0) {
			in->mouse = s->mouse = st->a;
			in->click[0] = in->held[0] = true;
			s->phase = 1;
		} else if (s->phase == 1) {
			in->mouse_delta = (Vector2){st->b.x - s->mouse.x, st->b.y - s->mouse.y};
			in->mouse = s->mouse = st->b;
			in->held[0] = true;
			s->phase = 2;
		} else {
			in->mouse = s->mouse = st->b;
			in->release[0] = true;
			s->phase = 0;
			s->next++;
		}
		break;
	case STEP_SHOT:
	case STEP_EXPECT:
		*due = st;
		s->next++;
		break;
	case STEP_QUIT:
		*quit = true;
		s->next++;
		break;
	}
}

void input_script_expect(InputScript *s, const InputStep *e, double actual)
{
	bool ok = false;
	switch (e->op) {
	case 0: ok = actual < e->value; break;
	case 1: ok = actual <= e->value; break;
	case 2: ok = actual > e->value; break;
	case 3: ok = actual >= e->value; break;
	case 4: ok = actual == e->value; break;
	case 5: ok = actual != e->value; break;
	}
	s->expects++;
	s->failures += !ok;
	printf("  %s line %d at %.1f s: %s (actual %g)\n", ok ? "ok  " : "FAIL", e->line, e->at, e->text, actual);
}
