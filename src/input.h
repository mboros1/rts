// One frame's input, as the controls, the camera and the client read it:
// from raylib (a person), or from an input script (input_script_*: anyone
// else, e.g. a test or an agent playing through the controls).
//
// An input script is text; times are seconds since the zone's first
// snapshot; keys are the keymap's names (keymap.h); positions are pixels in
// the client's window (1600 x 900 when scripted):
//
//   at 1.0 drag 300,250 1300,650      # left-drag from one point to another
//   at 1.5 key Q                      # a tap; also: key Q shift | key Q ctrl
//   at 2.0 click 800,450              # left; also: click X,Y right [shift] [ctrl]
//   at 2.0 move 800,450               # the mouse (no button, or the one pressed)
//   at 2.2 press 800,450              # the left button down, held while the mouse moves
//   at 2.8 release 900,500 [shift]    # ... and up: a slow drag
//   at 2.5 shot builders              # the frame, to SHOTS/builders.png
//   at 3.0 expect sent build >= 1     # orders sent of a kind (sent any: all kinds)
//   at 3.0 expect selected >= 2       # units selected
//   at 3.0 expect busy == 0           # a placement, a move or a prompt in progress (1/0)
//   at 4.0 quit
//
// Expectations print as scripts' do (ok / FAIL) and a failure makes the
// client exit non-zero.
#pragma once

#include <raylib.h>

#include "arena.h"
#include "base.h"
#include "sim.h"

enum { INPUT_KEYS = 512 };

typedef struct {
	bool pressed[INPUT_KEYS];   // went down this frame
	bool down[INPUT_KEYS];      // is down
	Vector2 mouse, mouse_delta;
	bool click[3], held[3], release[3]; // left, right, middle: went down, is down, came up
	float wheel;
	int width, height;          // the window
	float dt;                   // seconds since the last frame
} Input;

void input_from_raylib(Input *in);

static inline bool input_shift(const Input *in) { return in->down[KEY_LEFT_SHIFT] || in->down[KEY_RIGHT_SHIFT]; }
static inline bool input_ctrl(const Input *in) { return in->down[KEY_LEFT_CONTROL] || in->down[KEY_RIGHT_CONTROL]; }

typedef enum : uint8_t {
	STEP_KEY,
	STEP_CLICK,
	STEP_MOVE,
	STEP_DRAG,
	STEP_PRESS,                 // the left button goes down here and stays down
	STEP_RELEASE,               // ... and comes up here
	STEP_SHOT,
	STEP_EXPECT,
	STEP_QUIT,
} StepKind;

typedef enum : uint8_t { WATCH_SENT, WATCH_SELECTED, WATCH_BUSY } Watch;

typedef struct {
	double at;
	int line;
	StepKind kind;
	int key;                    // KEY; or the mouse button (CLICK)
	bool shift, ctrl;
	Vector2 a, b;               // CLICK, MOVE: a; DRAG: a to b
	char name[48];              // SHOT
	Watch watch;                // EXPECT
	int command;                // EXPECT sent: a CommandKind, or -1 for any
	int op;                     // < <= > >= == != : 0..5
	double value;
	const char *text;
} InputStep;

typedef struct {
	InputStep *steps;           // in time order
	ptrdiff_t count;
	ptrdiff_t next;
	int phase;                  // a drag or a click in progress: its frame
	bool holding;               // the left button is down (press, until release)
	Vector2 mouse;
	int expects, failures;
} InputScript;

bool input_script_load(InputScript *s, Arena *arena, const char *path, char *err, size_t err_len);

// This frame's input at `t` seconds: the steps due, one at a time (a click
// or a drag takes a few frames). Shots and expectations due are returned
// (one per frame) for the client to carry out; quit sets *quit.
void input_script_frame(InputScript *s, double t, int width, int height, float dt, Input *in, const InputStep **due,
                        bool *quit);

// Checks an expectation against what the client says, prints it, counts it.
void input_script_expect(InputScript *s, const InputStep *e, double actual);
