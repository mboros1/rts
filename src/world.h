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
//
// A world file only sets up a match. Everything after that is the game's.
#pragma once

#include "base.h"
#include "coord.h"

enum { WORLD_MAX_LINKS = 32 };

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
} World;

// False with "PATH:LINE: why" in err on a bad file.
bool world_parse(World *w, Str text, const char *path, char *err, ptrdiff_t err_len);
int world_zone(const World *w, const char *name);   // index, or -1
bool world_adjacent(const World *w, int a, int b);

// ---------------------------------------------------------------- the directory
//
// What the coordinator tells players about the world: the strategic map.

// A zone's report of itself, sent to the coordinator every second.
typedef struct {
	int64_t tick;
	uint16_t units[FACTION_COUNT];       // combat units
	uint16_t structures[FACTION_COUNT];  // everything that doesn't move
	uint8_t gates[FACTION_COUNT];        // finished gates
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
} Directory;

void put_zone_status(ByteBuf *b, const ZoneStatus *s);
bool get_zone_status(ByteReader *r, ZoneStatus *s);
void put_directory(ByteBuf *b, const Directory *d);
bool get_directory(ByteReader *r, Directory *d);

// The map as text, for terminals and tests:
//   west    up   blue's home  0:12:31   blue 23 units 5 bldg 1 gate | red 0 units 0 bldg
//   links: west-middle middle-east
//   transit: blue 12 west->middle
void directory_print(const Directory *d, FILE *out);
