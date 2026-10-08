// Netlog: every frame a client received, timestamped, plus connection events.
// A clip replays it to show exactly what the client saw: the latency, the
// freeze while disconnected, the snapshot on rejoining.
//
//   header  "RTSN" u32 version
//   record  f64 seconds since the client started, u8 kind, u32 length, bytes
//
// Kinds are MsgKind for frames, COORD_DIRECTORY for the world's directory
// (coord.h), or one of the events below with a short text payload.
#pragma once

#include "base.h"

enum { NETLOG_VERSION = 1 };

typedef enum : uint8_t {
	NETLOG_INFO = 200,          // "faction blue, latency 120 ms, jitter 30 ms"
	NETLOG_CONNECTED,
	NETLOG_DISCONNECTED,        // payload: the reason
	NETLOG_MAP,                 // went to the strategic map (COORD_DIRECTORY records follow)
} NetlogEvent;

static inline void netlog_begin(FILE *f)
{
	uint8_t head[8] = {'R', 'T', 'S', 'N', NETLOG_VERSION, 0, 0, 0};
	fwrite(head, 1, sizeof head, f);
}

static inline void netlog_record(FILE *f, double t, uint8_t kind, const void *data, ptrdiff_t len)
{
	if (!f) return;
	uint8_t head[13];
	uint64_t bits;
	memcpy(&bits, &t, sizeof bits);
	for (int i = 0; i < 8; i++) head[i] = (uint8_t)(bits >> (8 * i));
	head[8] = kind;
	for (int i = 0; i < 4; i++) head[9 + i] = (uint8_t)((uint32_t)len >> (8 * i));
	fwrite(head, 1, sizeof head, f);
	if (len) fwrite(data, 1, (size_t)len, f);
}

typedef struct {
	double t;
	uint8_t kind;
	const uint8_t *data;
	ptrdiff_t len;
} NetlogRecord;

typedef struct {
	const uint8_t *data;
	ptrdiff_t len, pos;
} NetlogReader;

static inline bool netlog_open(NetlogReader *r, Str file)
{
	*r = (NetlogReader){(const uint8_t *)file.ptr, file.len, 8};
	return file.len >= 8 && !memcmp(file.ptr, "RTSN", 4) && (uint8_t)file.ptr[4] == NETLOG_VERSION;
}

// False at the end, or at a truncated record.
static inline bool netlog_next(NetlogReader *r, NetlogRecord *out)
{
	if (r->len - r->pos < 13) return false;
	const uint8_t *p = r->data + r->pos;
	uint64_t bits = 0;
	for (int i = 0; i < 8; i++) bits |= (uint64_t)p[i] << (8 * i);
	uint32_t len = (uint32_t)p[9] | (uint32_t)p[10] << 8 | (uint32_t)p[11] << 16 | (uint32_t)p[12] << 24;
	if ((ptrdiff_t)len > r->len - r->pos - 13) return false;
	double t;
	memcpy(&t, &bits, sizeof t);
	*out = (NetlogRecord){t, p[8], p + 13, (ptrdiff_t)len};
	r->pos += 13 + (ptrdiff_t)len;
	return true;
}
