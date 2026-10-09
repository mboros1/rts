// The coordinator: zones, and units in transit between them.
//
// Every change is a line in an append-only log, written and fsynced before
// anyone is told, so a restarted coordinator rebuilds exactly what it had:
//
//   transfer ID SRC DST FACTION PLAYER paired|unpaired SEQ ROUTE X,Z TYPE:N ... [cargo:M,E]
//       units left SRC for DST, to arrive owned by PLAYER (a slot of FACTION),
//       their trucks carrying M metal and E energy
//       (SEQ: SRC's own number for it), going on
//       along ROUTE (a Route, in hex) from there; unpaired ones land at X,Z
//   delivered ID                            DST took them in
//   player NAME FACTION SLOT                NAME plays slot SLOT of FACTION,
//                                           in every zone, from now on
//   next ID / seq ZONE N                    after compaction: the next transfer
//                                           id, and ZONE's highest departure number
//
// The log is compacted at startup and every COMPACT_EVERY lines: rewritten
// (atomically) as next, seq and player lines plus the transfers still pending.
//
// A transfer is pending until delivered; while DST is down, has no
// transporter for a paired transfer, or has no room where they'd land, it
// waits and is retried.
//
// A transfer is safe from the moment its line is logged. A zone keeps its
// departures in its saved state until they're acknowledged and re-sends them
// after a restart; SEQ lets the coordinator recognise one it already has.
#pragma once

#include "base.h"
#include "bytes.h"
#include "sim.h"

enum { COORD_MAX_ZONES = 8, COORD_MAX_TRANSFERS = 4096, ZONE_NAME = 24, COMPACT_EVERY = 2000 };

// Messages between zones, players and the coordinator (framed by net.h).
typedef enum : uint8_t {
	COORD_ZONE_HELLO = 100, // zone: name, u16 player port
	COORD_DEPART,           // zone: u64 seq, u8 faction, dst name, Leg, counts, Cargo
	COORD_DELIVERED,        // zone: u64 id
	COORD_HOLD,             // zone: u64 id: can't take them yet
	COORD_DELIVER,          // coord: u64 id, u8 faction, Leg, counts, Cargo
	COORD_DIRECTORY_ASK,    // player: [u8 faction: its fogged view [, name: and I'm online [, u8 1: logging out]]]
	COORD_DIRECTORY,        // coord: Directory (world.h)
	COORD_DEPART_ACK,       // coord: u64 seq: logged (or already had it)
	COORD_ZONE_STATUS,      // zone, every second: ZoneStatus (world.h)
	COORD_JOIN,             // player: u8 faction, name: which slot is mine?
	COORD_SLOT,             // coord: u8 slot, or SIDE_FULL: the side is full
} CoordMsg;

enum : uint8_t { SIDE_FULL = 255 };

// How a transfer goes on: unpaired or not, who the units arrive owned by,
// the route after its destination, and where unpaired ones land.
// On the wire: u8 unpaired, u8 owner, u32 route, f32 x, z.
typedef struct {
	bool unpaired;
	PlayerSlot owner;
	Route route;
	V2 landing;
} Leg;

void put_leg(ByteBuf *b, const Leg *l);
bool get_leg(ByteReader *r, Leg *l);

// What a transfer's trucks carry, all together. On the wire: f32 metal, energy.
typedef struct {
	float metal, energy;
} Cargo;

void put_cargo(ByteBuf *b, const Cargo *c);
bool get_cargo(ByteReader *r, Cargo *c);

// Names travel as u8 length + bytes; counts as UNIT_TYPE_COUNT x u32.
void put_name(ByteBuf *b, const char *name);
bool get_name(ByteReader *r, char out[ZONE_NAME]);
void put_counts(ByteBuf *b, const int32_t counts[UNIT_TYPE_COUNT]);
bool get_counts(ByteReader *r, int32_t counts[UNIT_TYPE_COUNT]);

typedef struct {
	uint64_t id;
	char src[ZONE_NAME], dst[ZONE_NAME];
	uint8_t faction;
	Leg leg;
	int32_t counts[UNIT_TYPE_COUNT];
	Cargo cargo;
	bool delivered;
	double retry_at;        // not logged: when to try delivering again
} Transfer;

typedef struct {
	Transfer transfers[COORD_MAX_TRANSFERS];
	int32_t transfer_count;
	uint64_t next_id;
	// The highest departure number seen from each zone.
	char seq_zone[COORD_MAX_ZONES * 2][ZONE_NAME];
	uint64_t seq_max[COORD_MAX_ZONES * 2];
	int32_t seq_zones;
	// Who plays each slot: a name, or "" while nobody has taken it. A name
	// keeps its slot for good (no accounts: a name is all there is).
	char player[FACTION_COUNT][MAX_PLAYERS][ZONE_NAME];
	FILE *log;              // nullptr: changes aren't recorded (tests)
	int32_t lines;          // appended since the last compaction
} Ledger;

// Rebuild from a log's text. False, with the line number, on a bad line.
bool ledger_replay(Ledger *l, Str text, int32_t *bad_line);

// Each records its change in the log (fsynced) before applying it.
// nullptr if this departure (src, seq) is one the ledger already has.
Transfer *ledger_depart(Ledger *l, const char *src, uint64_t seq, const char *dst, uint8_t faction, const Leg *leg,
                        const int32_t counts[UNIT_TYPE_COUNT], const Cargo *cargo);
void ledger_delivered(Ledger *l, uint64_t id);
// NAME's slot on a side with `slots` players: theirs from before, else the
// first free one (logged), else SIDE_FULL.
uint8_t ledger_join(Ledger *l, uint8_t faction, const char *name, int slots);

Transfer *ledger_find(Ledger *l, uint64_t id);

// Rewrite the log at path as what's needed to rebuild this ledger, drop
// delivered transfers, and go on appending to the new log. False (and the
// old log kept) if it can't be written.
bool ledger_compact(Ledger *l, const char *path, Arena *scratch);
int32_t ledger_pending(const Ledger *l);
