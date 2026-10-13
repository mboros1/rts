#include "keymap.h"

#include "platform.h"

static const char *const action_names[BIND_COUNT] = {
	[BIND_MOVE] = "move", [BIND_FIGHT] = "fight", [BIND_STOP] = "stop", [BIND_ENTRENCH] = "entrench",
	[BIND_REPEAT] = "repeat", [BIND_CLEAR_QUEUE] = "clear_queue", [BIND_SEND] = "send", [BIND_GIVE] = "give",
	[BIND_TAKE] = "take", [BIND_WARP_HOME] = "warp_home", [BIND_CANCEL_BUILD] = "cancel_build",
	[BIND_BUILD_1] = "build_1", [BIND_BUILD_2] = "build_2", [BIND_BUILD_3] = "build_3", [BIND_BUILD_4] = "build_4",
	[BIND_BUILD_5] = "build_5", [BIND_BUILD_6] = "build_6", [BIND_BUILD_7] = "build_7", [BIND_BUILD_8] = "build_8",
	[BIND_TAB_1] = "tab_1", [BIND_TAB_2] = "tab_2", [BIND_TAB_3] = "tab_3", [BIND_TAB_4] = "tab_4",
	[BIND_CAMERA_UP] = "camera_up", [BIND_CAMERA_DOWN] = "camera_down", [BIND_CAMERA_LEFT] = "camera_left",
	[BIND_CAMERA_RIGHT] = "camera_right", [BIND_MAP] = "map", [BIND_CANCEL] = "cancel", [BIND_OVERLAY] = "overlay",
	[BIND_PAUSE] = "pause", [BIND_SANDBOX_BOT] = "sandbox_bot", [BIND_SANDBOX_CLIP] = "sandbox_clip",
};

Keymap keymap_default(void)
{
	Keymap km;
	const int defaults[BIND_COUNT] = {
		[BIND_MOVE] = 'M', [BIND_FIGHT] = 'G', [BIND_STOP] = KEYCODE_SPACE, [BIND_ENTRENCH] = 'B',
		[BIND_REPEAT] = 'Y', [BIND_CLEAR_QUEUE] = 'J', [BIND_SEND] = 'T', [BIND_GIVE] = 'H', [BIND_TAKE] = 'K',
		[BIND_WARP_HOME] = 'U', [BIND_CANCEL_BUILD] = 'N',
		// BAR's grid keys.
		[BIND_BUILD_1] = 'Q', [BIND_BUILD_2] = 'W', [BIND_BUILD_3] = 'E', [BIND_BUILD_4] = 'R',
		[BIND_BUILD_5] = 'A', [BIND_BUILD_6] = 'S', [BIND_BUILD_7] = 'D', [BIND_BUILD_8] = 'F',
		[BIND_TAB_1] = 'Z', [BIND_TAB_2] = 'X', [BIND_TAB_3] = 'C', [BIND_TAB_4] = 'V',
		[BIND_CAMERA_UP] = KEYCODE_UP, [BIND_CAMERA_DOWN] = KEYCODE_DOWN, [BIND_CAMERA_LEFT] = KEYCODE_LEFT,
		[BIND_CAMERA_RIGHT] = KEYCODE_RIGHT, [BIND_MAP] = KEYCODE_TAB, [BIND_CANCEL] = KEYCODE_ESCAPE,
		[BIND_OVERLAY] = KEYCODE_F1,
		[BIND_PAUSE] = 'P', [BIND_SANDBOX_BOT] = KEYCODE_F1 + 1, [BIND_SANDBOX_CLIP] = KEYCODE_F1 + 2,
	};
	for (int a = 0; a < BIND_COUNT; a++) km.key[a] = defaults[a];
	return km;
}

static bool str_is(Str s, const char *t) { return s.len == (ptrdiff_t)strlen(t) && !memcmp(s.ptr, t, (size_t)s.len); }

int keymap_action_of(Str name)
{
	for (int a = 0; a < BIND_COUNT; a++)
		if (str_is(name, action_names[a])) return a;
	return -1;
}

const char *keymap_action_name(KeyBinding a) { return a < BIND_COUNT ? action_names[a] : ""; }

static const struct {
	int key;
	const char *name;
} named_keys[] = {
	{KEYCODE_SPACE, "Space"}, {KEYCODE_ESCAPE, "Escape"}, {KEYCODE_ENTER, "Enter"}, {KEYCODE_TAB, "Tab"},
	{KEYCODE_BACKSPACE, "Backspace"}, {KEYCODE_RIGHT, "Right"}, {KEYCODE_LEFT, "Left"}, {KEYCODE_DOWN, "Down"},
	{KEYCODE_UP, "Up"},
};

int keymap_key_of(Str name)
{
	if (name.len == 1 && name.ptr[0] >= 'A' && name.ptr[0] <= 'Z') return name.ptr[0];
	if (name.len == 1 && name.ptr[0] >= '0' && name.ptr[0] <= '9') return name.ptr[0];
	if ((name.len == 2 || name.len == 3) && name.ptr[0] == 'F') {
		int n = name.ptr[1] - '0';
		if (name.len == 3) n = n * 10 + (name.ptr[2] - '0');
		bool digits = name.ptr[1] >= '1' && name.ptr[1] <= '9' && (name.len == 2 || (name.ptr[2] >= '0' && name.ptr[2] <= '9'));
		if (digits && n >= 1 && n <= 12) return KEYCODE_F1 + n - 1;
	}
	for (int k = 0; k < (int)countof(named_keys); k++)
		if (str_is(name, named_keys[k].name)) return named_keys[k].key;
	return -1;
}

const char *keymap_key_name(int key)
{
	static const char *const letters[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
	                                        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
	static const char *const digits[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
	static const char *const f[12] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
	if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
	if (key >= '0' && key <= '9') return digits[key - '0'];
	if (key >= KEYCODE_F1 && key < KEYCODE_F1 + 12) return f[key - KEYCODE_F1];
	for (int k = 0; k < (int)countof(named_keys); k++)
		if (named_keys[k].key == key) return named_keys[k].name;
	return "";
}

// ---------------------------------------------------------------- JSON
//
// Only what a keymap is: one object of strings to strings. No escapes.

typedef struct {
	Str text;
	ptrdiff_t at;
} Json;

static void skip_space(Json *j)
{
	while (j->at < j->text.len && (j->text.ptr[j->at] == ' ' || j->text.ptr[j->at] == '\t' || j->text.ptr[j->at] == '\n'
	                               || j->text.ptr[j->at] == '\r'))
		j->at++;
}

static bool expect(Json *j, char c)
{
	skip_space(j);
	if (j->at < j->text.len && j->text.ptr[j->at] == c) {
		j->at++;
		return true;
	}
	return false;
}

static bool string(Json *j, Str *out)
{
	if (!expect(j, '"')) return false;
	ptrdiff_t start = j->at;
	while (j->at < j->text.len && j->text.ptr[j->at] != '"') {
		if (j->text.ptr[j->at] == '\\' || (unsigned char)j->text.ptr[j->at] < ' ') return false;
		j->at++;
	}
	if (j->at == j->text.len) return false;
	*out = (Str){j->text.ptr + start, j->at - start};
	j->at++;
	return true;
}

static bool refuse(char *err, size_t len, const char *why, Str what)
{
	snprintf(err, len, "%s%s%.*s%s", why, what.len ? ": \"" : "", (int)what.len, what.ptr, what.len ? "\"" : "");
	return false;
}

bool keymap_parse(Keymap *km, Str json, char *err, size_t err_len)
{
	Keymap next = *km;
	Json j = {json, 0};
	Str none = {"", 0};
	if (!expect(&j, '{')) return refuse(err, err_len, "expected a JSON object", none);
	bool first = true;
	while (!expect(&j, '}')) {
		if (!first && !expect(&j, ',')) return refuse(err, err_len, "expected , or }", none);
		first = false;
		Str action, key;
		if (!string(&j, &action)) return refuse(err, err_len, "expected an action name in quotes", none);
		if (!expect(&j, ':')) return refuse(err, err_len, "expected : after", action);
		if (!string(&j, &key)) return refuse(err, err_len, "expected a key name in quotes for", action);
		int a = keymap_action_of(action), k = keymap_key_of(key);
		if (a < 0) return refuse(err, err_len, "no such action", action);
		if (k < 0) return refuse(err, err_len, "no such key (A-Z, F1-F12, Space, Escape, Tab, Backspace, Up, Down, Left, Right)", key);
		if ((k >= KEYCODE_0 && k <= KEYCODE_0 + 9) || k == KEYCODE_ENTER)
			return refuse(err, err_len, "the digits and Enter are fixed (they choose zones and players)", key);
		next.key[a] = k;
	}
	skip_space(&j);
	if (j.at != json.len) return refuse(err, err_len, "something after the object", none);
	for (int a = 0; a < BIND_COUNT; a++)
		for (int b = a + 1; b < BIND_COUNT; b++)
			if (next.key[a] == next.key[b]) {
				snprintf(err, err_len, "%s is bound to both %s and %s", keymap_key_name(next.key[a]), action_names[a],
				         action_names[b]);
				return false;
			}
	*km = next;
	return true;
}

Keymap keymap_load(const char *path, bool required, Arena *scratch)
{
	Keymap km = keymap_default();
	ArenaMark m = arena_mark(scratch);
	Str text;
	char err[256];
	if (!platform_file_exists(path)) {
		if (required) fprintf(stderr, "keys: no %s; using the default keys\n", path);
	} else if (!platform_read_file(scratch, path, &text)) {
		fprintf(stderr, "keys: cannot read %s; using the default keys\n", path);
	} else if (!keymap_parse(&km, text, err, sizeof err)) {
		fprintf(stderr, "keys: %s: %s; using the default keys\n", path, err);
	}
	arena_rewind(m);
	return km;
}
