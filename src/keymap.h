// Which key does what, from a JSON file (config/keys.json): a flat object of
// action names to key names,
//
//   {"stop": "Space", "move": "M", "build_1": "Q", "camera_up": "Up"}
//
// Actions it leaves out keep their defaults (keymap_default). A file with an
// unknown action or key, or one key bound to two actions, is refused whole.
// Fixed, not in the file: the digits (choosing a zone or a player), Enter,
// and shift and ctrl as modifiers.
//
// Pure: no raylib. Key codes are raylib's values (KeyboardKey), checked
// against raylib where it's included (controls.c).
#pragma once

#include "arena.h"
#include "base.h"

typedef enum : uint8_t {
	// Orders.
	BIND_MOVE,
	BIND_FIGHT,
	BIND_STOP,
	BIND_ENTRENCH,
	BIND_REPEAT,
	BIND_CLEAR_QUEUE,
	BIND_SEND,
	BIND_GIVE,
	BIND_TAKE,
	BIND_WARP_HOME,
	BIND_CANCEL_BUILD,    // unfinished structures selected: they go, refunded
	BIND_IDLE_BUILDER,    // select the next idle builder, and look at it
	// The build grid: items, row by row from the top left, then tabs.
	BIND_BUILD_1, BIND_BUILD_2, BIND_BUILD_3, BIND_BUILD_4,
	BIND_BUILD_5, BIND_BUILD_6, BIND_BUILD_7, BIND_BUILD_8,
	BIND_TAB_1, BIND_TAB_2, BIND_TAB_3, BIND_TAB_4,
	// The camera and the game.
	BIND_CAMERA_UP,
	BIND_CAMERA_DOWN,
	BIND_CAMERA_LEFT,
	BIND_CAMERA_RIGHT,
	BIND_MAP,             // back to the strategic map
	BIND_CANCEL,          // a placement or a prompt; with none, the map
	BIND_OVERLAY,         // network and timing details
	// The dev sandbox's own.
	BIND_PAUSE,
	BIND_SANDBOX_BOT,
	BIND_SANDBOX_CLIP,
	BIND_COUNT,
} KeyBinding;

// raylib's key codes for the keys a keymap can name.
enum {
	KEYCODE_SPACE = 32,
	KEYCODE_0 = 48,          // through 9: 57
	KEYCODE_A = 65,          // through Z: 90
	KEYCODE_ESCAPE = 256,
	KEYCODE_ENTER = 257,
	KEYCODE_TAB = 258,
	KEYCODE_BACKSPACE = 259,
	KEYCODE_RIGHT = 262,
	KEYCODE_LEFT = 263,
	KEYCODE_DOWN = 264,
	KEYCODE_UP = 265,
	KEYCODE_F1 = 290,        // through F12: 301
};

typedef struct {
	int key[BIND_COUNT];   // a raylib key code per action
} Keymap;

Keymap keymap_default(void);

// Apply a JSON keymap over km (usually the defaults). False, with why in err,
// and km untouched, on anything malformed, unknown or bound twice.
bool keymap_parse(Keymap *km, Str json, char *err, size_t err_len);

// The defaults, then the file at path over them if there is one. A file
// that's there but refused is reported on stderr (the defaults stand). With
// `required` (--keys), a missing file is reported too.
Keymap keymap_load(const char *path, bool required, Arena *scratch);

// The action named (as in the file), or -1; and the other way.
int keymap_action_of(Str name);
const char *keymap_action_name(KeyBinding a);

// A key's name (as in the file, and as drawn on a button), or -1 / "".
int keymap_key_of(Str name);
const char *keymap_key_name(int key);
