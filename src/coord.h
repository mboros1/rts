// The coordinator: zones, transfers between them, and each faction's pool.
//
// Every change is a line in an append-only log, written and fsynced before
// anyone is told, so a restarted coordinator rebuilds exactly what it had:
//
//   transfer ID SRC DST FACTION seed|units SEQ TYPE:N ...   units left SRC for DST
//                                                            (SEQ: SRC's own number for it)
//   delivered ID                                         DST confirmed them
//   draw ID FACTION ZONE TYPE:N ...                      the pool sent units to ZONE
//
// DST may be "pool": the units join the faction's pool at once. A draw is a
// transfer from the pool. A transfer is pending until delivered; while its
// zone is down or has no gate for the faction, it waits and is retried.
//
// A transfer is safe from the moment its line is logged. A zone keeps its
// departures in its saved state until they're acknowledged and re-sends them
// after a restart; SEQ lets the coordinator recognise one it already has.
#pragma once

#include "base.h"
#include "bytes.h"
#include "sim.h"

enum { COORD_MAX_ZONES = 8, COORD_MAX_TRANSFERS = 4096, ZONE_NAME = 24 };

// Messages between zones, players and the coordinator (framed by net.h).
typedef enum : uint8_t {
	COORD_ZONE_HELLO = 100, // zone: name, u16 player port
	COORD_DEPART,           // zone: u64 seq, u8 faction, dst name, u8 seed, counts
	COORD_DRAW,             // zone: u8 faction, counts (to this zone)
	COORD_DELIVERED,        // zone: u64 id
	COORD_HOLD,             // zone: u64 id: no gate for that faction yet
	COORD_DELIVER,          // coord: u64 id, u8 faction, u8 seed, counts
	COORD_DIRECTORY_ASK,    // player
	COORD_DIRECTORY,        // coord: u8 n, n x (name, host, u16 port, u8 up)
	COORD_DEPART_ACK,       // coord: u64 seq: logged (or already had it)
} CoordMsg;

// Names travel as u8 length + bytes; counts as UNIT_TYPE_COUNT x u32.
void put_name(ByteBuf *b, const char *name);
bool get_name(ByteReader *r, char out[ZONE_NAME]);
void put_counts(ByteBuf *b, const int32_t counts[UNIT_TYPE_COUNT]);
bool get_counts(ByteReader *r, int32_t counts[UNIT_TYPE_COUNT]);

typedef struct {
	uint64_t id;
	char src[ZONE_NAME], dst[ZONE_NAME];
	uint8_t faction;
	bool seed;
	int32_t counts[UNIT_TYPE_COUNT];
	bool delivered;
	double retry_at;        // not logged: when to try delivering again
} Transfer;

typedef struct {
	Transfer transfers[COORD_MAX_TRANSFERS];
	int32_t transfer_count;
	uint64_t next_id;
	int32_t pool[FACTION_COUNT][UNIT_TYPE_COUNT];
	// The highest departure number seen from each zone.
	char seq_zone[COORD_MAX_ZONES * 2][ZONE_NAME];
	uint64_t seq_max[COORD_MAX_ZONES * 2];
	int32_t seq_zones;
	FILE *log;              // nullptr: changes aren't recorded (tests)
} Ledger;

// Rebuild from a log's text. False, with the line number, on a bad line.
bool ledger_replay(Ledger *l, Str text, int32_t *bad_line);

// Each records its change in the log (fsynced) before applying it.
// nullptr if this departure (src, seq) is one the ledger already has.
Transfer *ledger_depart(Ledger *l, const char *src, uint64_t seq, const char *dst, uint8_t faction, bool seed,
                        const int32_t counts[UNIT_TYPE_COUNT]);
// Takes what the pool has, up to what was asked; nullptr if it has none.
Transfer *ledger_draw(Ledger *l, uint8_t faction, const char *zone, const int32_t want[UNIT_TYPE_COUNT]);
void ledger_delivered(Ledger *l, uint64_t id);

Transfer *ledger_find(Ledger *l, uint64_t id);
int32_t ledger_pending(const Ledger *l);
