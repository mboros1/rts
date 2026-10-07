// TCP connections carrying framed messages, with emulated network delay.
//
// Frames are u32 length (of what follows), u8 kind, payload. Sockets are
// non-blocking: conn_pump moves bytes both ways and never waits.
//
// Delay emulation sits on the sending side, in the real code path: each
// frame is held until latency +- jitter has passed. Release times never go
// backwards, so frames keep their order, as TCP would.
//
// Buffers are fixed and sized up front. A connection whose peer falls so far
// behind that its queue fills is closed, never grown.
#pragma once

#include "arena.h"
#include "base.h"
#include "rng.h"

typedef struct {
	double release;         // when the frame may go on the wire
	int64_t end;            // byte offset in the out stream where it ends
} PendingFrame;

typedef struct {
	int fd;
	bool open;
	const char *why_closed;

	// Outgoing: a byte stream with monotonically increasing offsets.
	uint8_t *out;
	ptrdiff_t out_cap;
	int64_t out_written;    // bytes handed to the socket
	int64_t out_queued;     // bytes accepted from conn_send
	int64_t out_released;   // bytes whose frames' delay has passed
	PendingFrame *pending;
	ptrdiff_t pending_cap;
	int64_t pending_head, pending_tail;

	// Incoming: bytes read and not yet consumed.
	uint8_t *in;
	ptrdiff_t in_cap;
	ptrdiff_t in_len;
	ptrdiff_t in_consumed;

	double latency, jitter; // seconds
	double last_release;
	Rng rng;

	int64_t bytes_sent, bytes_received;
} Conn;

typedef struct {
	uint8_t kind;
	const uint8_t *data;
	ptrdiff_t len;
} Frame;

// Largest frame either side accepts: a snapshot of a full battle fits.
static constexpr ptrdiff_t FRAME_MAX = (ptrdiff_t)8 << 20;

int net_listen(int port);                       // -1 on failure
int net_accept(int listen_fd);                  // -1 if nobody is waiting
int net_connect(const char *host, int port);    // -1 on failure; blocking
void net_close_fd(int fd);
void net_sleep(double seconds);

void conn_init(Conn *c, Arena *a, int fd, ptrdiff_t out_cap, ptrdiff_t in_cap);
void conn_set_delay(Conn *c, double latency, double jitter, uint64_t seed);

// Reuse a connection's buffers for a new socket (a reconnect).
void conn_reopen(Conn *c, int fd);

// Queue a frame. False (and the connection closed) if the queue is full.
bool conn_send(Conn *c, uint8_t kind, const void *payload, ptrdiff_t len, double now);

// Write released bytes and read what's available. Closes on error or EOF.
void conn_pump(Conn *c, double now);

// The next complete frame, valid until the next conn_pump. False if none.
bool conn_next(Conn *c, Frame *f);

void conn_close(Conn *c, const char *why);
