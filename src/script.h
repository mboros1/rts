// Scripts: timed actions for players and bots, written as text.
//
// A script never touches sim state. Each action at tick T becomes ordinary
// commands for tick T (or a check of the state at T), so scripted runs record
// and replay like any other and clips of them are still hash-verified.
//
//   title Scripted flank              # shown on the clip's title card
//   about Raiders swing north and hit red's side
//   scenario skirmish                 # starting config and bot behaviour
//   seed 7 | units 400 | weights 6 3 2 | respawn off
//   bot red off                       # at tick 0
//   clip 200..700                     # default clip window
//   camera director | camera 256,256 180
//   end 900                           # default: after the last action/clip
//
//   at 0   spawn blue tank 20 at 120,256 radius 10
//   at 1   select blue tank within 30 of 120,256 max 20 as column
//   at 30  move column 256,90         # also: attack-move, stop
//   at 60  bot red on
//   at 600 expect red lost > 50       # side fields: units lost fired shells spawned
//   at 600 expect column alive >= 15  # group field: alive
//
// Selections are queries resolved at their tick: SIDE, TYPE (raider, tank,
// artillery, any), optionally within R of X,Z, optionally the nearest max N.
// Units spawned at tick T exist from tick T+1.
#pragma once

#include "arena.h"
#include "bot.h"
#include "scenario.h"
#include "sim.h"

typedef enum : uint8_t {
	ACTION_SELECT,
	ACTION_ORDER,
	ACTION_SPAWN,
	ACTION_BOT,
	ACTION_EXPECT,
} ActionKind;

typedef enum : uint8_t {
	FIELD_UNITS,
	FIELD_LOST,
	FIELD_FIRED,
	FIELD_SHELLS,
	FIELD_SPAWNED,
	FIELD_ALIVE,      // groups only
} ExpectField;

typedef enum : uint8_t { OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE } ExpectOp;

typedef struct {
	int64_t tick;
	int32_t line;
	ActionKind kind;
	FactionId side;
	int32_t group;          // select, order, expect on a group; else -1
	int32_t type;           // select (-1 any), spawn
	bool area;              // select: within radius of at
	float radius;           // select area, spawn radius
	V2 at;                  // select area centre, spawn point, order target
	int32_t limit;          // select max (0 for all), spawn amount
	CommandKind command;    // order
	bool on;                // bot
	ExpectField field;      // expect
	ExpectOp op;
	double value;
	const char *text;       // the source line, for messages
} ScriptAction;

typedef struct {
	const char *path;
	const char *title;
	const char *about;
	const Scenario *scenario;
	SimConfig config;
	bool bots_on[FACTION_COUNT];   // at tick 0
	ScriptAction *actions;         // sorted by tick, then by line
	ptrdiff_t action_count;
	const char **groups;
	ptrdiff_t group_count;
	int64_t clip_from, clip_to;
	int64_t end;
	bool fixed_camera;
	V2 camera_at;
	float camera_distance;
} Script;

// Parse a script. On failure returns false with "path:line: reason" in err.
bool script_parse(Script *s, Arena *arena, Str text, const char *path, char *err, ptrdiff_t err_len);
bool script_load(Script *s, Arena *arena, const char *path, char *err, ptrdiff_t err_len);

typedef struct {
	const Script *script;
	ptrdiff_t next;
	Bot bots[FACTION_COUNT];
	UnitHandle **group_units;
	ptrdiff_t *group_len;
	FactionId *group_side;
	int32_t expects;
	int32_t failures;
	FILE *log;                     // expect results go here; may be nullptr
} ScriptRun;

void script_run_init(ScriptRun *r, const Script *s, Arena *arena, FILE *log);

// Call once per tick before sim_step: runs the actions at the current tick
// (checking expects against the state as it stands) and returns the commands
// for this tick, scripted first, then the bots'.
ptrdiff_t script_tick(ScriptRun *r, const Sim *sim, Arena *tick_arena, Command *out, ptrdiff_t cap);
