// Scripts: timed actions for players and bots, written as text.
//
// A script never touches sim state. Each action at tick T becomes ordinary
// commands for tick T (or a check of the state at T), so scripted runs record
// and replay like any other and clips of them are still hash-verified.
//
//   title Scripted flank              # shown on the clip's title card
//   about Raiders swing north and hit red's side
//   scenario skirmish                 # starting config and bot behaviour
//   seed 7 | units 400 | weights 6 3 2 | respawn off | economy on
//   players blue 3                    # a side's players (1..8), each with their own base
//   economy rich cost=0.9             # economy: the zone's rules, and its environment
//   environment barren generator=2    #   (context.h: presets and fields)
//   map large                         # the zone's size and metal spots (context.h)
//   commanders on                     # economy: each player starts with only a commander
//                                     #   (and a side with a base, its headquarters); see sim.h
//   bases blue | bases red | bases none | bases both  # economy: who starts with a base
//   doctrine red turtle army_cap=500  # how a side's bot plays (bot.h); default balanced
//   bot red off                       # at tick 0; bot red.1 off: one player's
//
// A side is blue or red, or blue.N for its player N (0..7); plain blue is
// player 0. Units belong to a player, and only their owner's orders move
// them: a group's orders are given by whoever selected it.
//   clip 200..700                     # default clip window
//   speed 4                           # time-lapse clips: a frame every 4 ticks
//   camera director | camera 256,256 180
//   end 900                           # default: after the last action/clip
//
//   at 0   spawn blue tank 20 at 120,256 radius 10
//   at 1   select blue tank within 30 of 120,256 max 20 as column
//   at 30  move column 256,90         # also: attack-move, stop
//   at 31  move column 300,90 queued  # after its current orders (shift)
//   at 40  build crew extractor at 92,226   # engineers; also generator, factory
//   at 40  queue crew generator at 40,280   # a build after its current orders (shift)
//   zones alpha beta                  # names for zone numbers, in order
//   at 50  send army to gamma via beta at 60,300  # through our nearest transporter;
//                                     #   via: the hops before; at: an unpaired landing
//                                     #   point; unpaired: say so (no server to decide)
//   at 50  arrive blue tank 10        # as if through blue's transporter (tests)
//   at 40  produce plant raider 2 repeat    # factories: queue N (0 clears)
//   at 55  give column to 1           # the units now belong to player 1 of their side
//   at 55  output plant to 1          # factories: what they make goes to player 1
//   at 56  give blue.0 metal 300 to 1 # resources (metal or energy), as much as
//                                     #   blue.0 has and player 1 has room for
//   at 58  show crew tab 2            # clips draw the controls as if its player had
//                                     #   selected the group (tab: the build grid's); show none
//   at 57  take blue.1                # blue.1 takes every unit of away teammates
//                                     #   (a script's player is away while its bot is on)
//   at 60  send army to gamma for 2   # they arrive belonging to player 2
//   at 65  warp boss                  # the group's commanders warp home (paid, takes time)
//   at 70  entrench line              # combat units dig in as turrets (paid, takes time);
//   at 90  mobilize line              #   mobilize turns entrenched turrets back
//   at 60  bot red on
//   at 600 expect red lost > 50       # side fields: units lost fired shells spawned
//                                     #   metal energy metal_income energy_income
//                                     #   and finished counts: raiders tanks artillery
//                                     #   engineers scouts sappers metal-trucks energy-trucks extractors
//                                     #   generators factories transporters turrets metal-vaults energy-vaults
//                                     #   heavy-tanks howitzers advanced-engineers advanced-extractors
//                                     #   advanced-factories commanders headquarters sieges
//                                     #   departed arrived won (1 once the game's over and it won)
//   at 600 expect blue.1 tanks >= 5   # a player's: units and finished counts
//   at 600 expect column alive >= 15  # group field: alive
//
// A side whose headquarters falls loses: the script says the game is over
// (COMMAND_GAME_OVER), and nothing happens in the zone after that.
//
// Selections are queries resolved at their tick: SIDE (a player's units
// only), TYPE (raider, tank, artillery, any), optionally within R of X,Z, optionally the nearest max N.
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
	ACTION_SHARE,           // resources from a player to a teammate
	ACTION_TAKE,            // a player takes away teammates' units
	ACTION_SHOW,            // clips: draw the controls as if this group were selected
} ActionKind;

typedef enum : uint8_t {
	FIELD_UNITS,
	FIELD_LOST,
	FIELD_FIRED,
	FIELD_SHELLS,
	FIELD_SPAWNED,
	FIELD_ALIVE,      // groups only
	FIELD_METAL,
	FIELD_ENERGY,
	FIELD_METAL_INCOME,
	FIELD_ENERGY_INCOME,
	FIELD_COUNT,      // finished units of `type`
	FIELD_DEPARTED,
	FIELD_ARRIVED,
	FIELD_WON,        // 1 once the game is over and this side won
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
	float metal, energy;    // share: how much
	V2 at;                  // select area centre, spawn point, order target
	int32_t limit;          // select max (0 for all), spawn amount
	CommandKind command;    // order; build and produce use type and limit
	uint8_t flags;          // produce: COMMAND_REPEAT; send: COMMAND_UNPAIRED
	Route route;            // send: the hops after the first (limit)
	PlayerSlot player;      // select, spawn, arrive, bot, expect: SIDE.N (0 if just SIDE)
	bool player_given;      //   ... and whether .N was written
	PlayerSlot to;          // give, output; send: who it arrives for
	bool to_given;          // send: `for N` was written
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
	bool bots_on[FACTION_COUNT][MAX_PLAYERS]; // at tick 0
	Doctrine doctrine[FACTION_COUNT]; // how each side's bot plays (bot.h)
	ScriptAction *actions;         // sorted by tick, then by line
	ptrdiff_t action_count;
	const char **groups;
	ptrdiff_t group_count;
	int64_t clip_from, clip_to;
	int32_t speed;                 // clip time-lapse: one frame every N ticks
	const char *zones[8];          // zone numbers' names
	int32_t zone_count;
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
	Bot bots[FACTION_COUNT][MAX_PLAYERS]; // each player's; those past config.players never run
	// Who's away (bit per player), recorded as COMMAND_AI_SLOTS when it
	// changes. A script's players are away while their bots are on; a zone
	// server sets `away` itself, from who is logged in (presence_set).
	bool presence_set;
	uint8_t away[FACTION_COUNT];
	uint8_t away_sent[FACTION_COUNT]; // what the sim was last told (it starts with everyone away)
	UnitHandle **group_units;
	ptrdiff_t *group_len;
	FactionId *group_side;
	PlayerSlot *group_player;      // who selected it: the group's orders are theirs
	int32_t show_group;            // clips draw the controls as if it were selected; -1: none
	int32_t show_tab;              // ... with this build tab open
	// Queued builds per group, issued one at a time as the group goes idle.
	int32_t expects;
	int32_t failures;
	FILE *log;                     // expect results go here; may be nullptr
} ScriptRun;

void script_run_init(ScriptRun *r, const Script *s, Arena *arena, FILE *log);

// Call once per tick before sim_step: runs the actions at the current tick
// (checking expects against the state as it stands) and returns the commands
// for this tick, scripted first, then the bots'.
ptrdiff_t script_tick(ScriptRun *r, const Sim *sim, Arena *tick_arena, Command *out, ptrdiff_t cap);
