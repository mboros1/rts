// A world: the zones of one persistent match, where they sit on the
// strategic map, and the links between them. Units only travel along links.
//
//   # three.world
//   port 7700                        coordinator; zone k listens on port+1+k
//   zone west   0.2 0.5 home blue    name, map position (0..1), whose home
//   zone middle 0.5 0.5
//   zone east   0.8 0.5 home red
//   link west middle                 both ways
//   link middle east
//   players blue 3                   a side's players (1..8; default 1), every zone
//   doctrine red turtle              how a side's bots play (bot.h), every zone
//   doctrine blue in middle raider   ... or one zone (after its zone line)
//   economy lean                     a zone's rules and environment (context.h):
//   environment in east barren       every zone, or one (home zones start `home`)
//   map in west large                the zone's size and metal (context.h)
//   setup west at 0 spawn red tank 9 at 72,256   a script line (script.h) a zone
//                                    starts with: fixtures, what's where at the start
//
// A world file only sets up a match. Everything after that is the game's.
#pragma once

#include "base.h"
#include "bot.h"
#include "coord.h"

enum { WORLD_MAX_LINKS = 32 };

// A zone's saved state's format (rts-server --state); a world's live
// directory records it, so `rts world` can tell a world an older build made.
enum { ZONE_STATE_VERSION = 14 };

typedef struct {
	char name[ZONE_NAME];
	float x, y;                 // on the strategic map, 0..1, y down
	int8_t home;                // the faction whose home zone this is, or -1
} WorldZone;

typedef struct {
	WorldZone zones[COORD_MAX_ZONES];
	int zone_count;
	uint8_t links[WORLD_MAX_LINKS][2];
	int link_count;
	int port;
	uint8_t players[FACTION_COUNT];                        // slots per side
	char doctrine[FACTION_COUNT][192];                     // every zone ("": the default)
	char doctrine_in[COORD_MAX_ZONES][FACTION_COUNT][192]; // one zone, over that
	char setup[COORD_MAX_ZONES][1024];                     // script lines a zone starts with (`setup ZONE ...`)
	// The zones' contexts (context.h): every zone's, then one zone's over
	// it; home zones start from the `home` economy.
	char economy[192], economy_in[COORD_MAX_ZONES][192];
	char environment[192], environment_in[COORD_MAX_ZONES][192];
	char map[192], map_in[COORD_MAX_ZONES][192];
} World;

// The doctrine a side's bots play in zone z ("": the default).
static inline const char *world_doctrine(const World *w, int z, int side)
{
	return w->doctrine_in[z][side][0] ? w->doctrine_in[z][side] : w->doctrine[side];
}

// False with "PATH:LINE: why" in err on a bad file.
bool world_parse(World *w, Str text, const char *path, char *err, ptrdiff_t err_len);
int world_zone(const World *w, const char *name);   // index, or -1
bool world_adjacent(const World *w, int a, int b);

// ---------------------------------------------------------------- the directory
//
// What the coordinator tells players about the world: the strategic map.

// What a side knows of its enemy in a zone: only what its units have seen.
// Ages are in game seconds; INTEL_NEVER: never seen.
enum : uint32_t { INTEL_NEVER = 0xffffffffu };
typedef struct {
	bool contact;                // sees enemy units there now
	uint32_t seen_age;           // since it last did
	uint8_t transporters;        // enemy transporters it saw, when it last saw any
	uint32_t transporter_age;    // since then
} Intel;

// A zone's report of itself, sent to the coordinator every second.
typedef struct {
	int64_t tick;
	uint16_t units[FACTION_COUNT];       // combat units
	uint16_t structures[FACTION_COUNT];  // everything that doesn't move
	uint8_t transporters[FACTION_COUNT]; // finished transporters
	uint8_t short_metal[FACTION_COUNT];  // bit per player: under half the base storage's metal
	uint8_t short_energy[FACTION_COUNT]; // ... or energy
	Intel intel[FACTION_COUNT];          // each side's, of the other
	uint8_t fallen;                      // bit per faction: its headquarters fell here (it lost)
} ZoneStatus;

typedef struct {
	char name[ZONE_NAME];
	char host[64];
	int port;
	bool up;
	float x, y;                 // map position; both 0 when the world doesn't say
	int8_t home;                // -1: nobody's
	bool reported;              // has sent a status since the coordinator started
	ZoneStatus status;
} DirectoryZone;

// Units on their way from one zone to another (pending transfers, summed).
typedef struct {
	uint8_t src, dst, faction;
	int32_t units;
} DirectoryTransit;

enum { DIRECTORY_MAX_TRANSIT = 64 };

typedef struct {
	DirectoryZone zones[COORD_MAX_ZONES];
	int zone_count;
	uint8_t links[WORLD_MAX_LINKS][2];
	int link_count;
	DirectoryTransit transit[DIRECTORY_MAX_TRANSIT];
	int transit_count;
	// Each side's players: how many slots, and who plays each ("": nobody
	// yet, so its bots do).
	uint8_t players[FACTION_COUNT];
	char player[FACTION_COUNT][MAX_PLAYERS][ZONE_NAME];
	uint8_t online[FACTION_COUNT];  // bit per slot: logged in now (anywhere in the world)
	uint8_t ended;                  // 0: the game goes on; else 1 + the side that won, and every zone stops
	uint8_t ended_in;               // the zone the losing headquarters fell in (when ended)
} Directory;

// Who won, or -1 while the game goes on.
static inline int directory_winner(const Directory *d) { return d->ended ? d->ended - 1 : -1; }

// A side's view of a directory (fog of war on the map): its own presence,
// its own transits, its own players, and its intel; nothing of the enemy's
// numbers or who they are.
// SPECTATOR (wire.h): the whole of it.
void directory_fog(Directory *d, uint8_t viewer);

void put_zone_status(ByteBuf *b, const ZoneStatus *s);
bool get_zone_status(ByteReader *r, ZoneStatus *s);
void put_directory(ByteBuf *b, const Directory *d);
bool get_directory(ByteReader *r, Directory *d);

// The map as text, for terminals and tests:
//   west    up   blue's home  0:12:31   blue 23 units 5 bldg 1 transporters | red 0 units 0 bldg
//   links: west-middle middle-east
//   transit: blue 12 west->middle
void directory_print(const Directory *d, FILE *out);

// What a bot of side f in zone `here` knows of the world, from a directory
// already fogged for f; then, once the caller has put in what it knows of
// `here` first-hand, the links (which depend on what's believed where).
void bot_world_build(BotWorld *w, const Directory *d, int here, uint8_t f);
void bot_world_links(BotWorld *w, const Directory *d);
