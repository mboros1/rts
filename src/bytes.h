// Little-endian byte encoding into fixed buffers, and bounds-checked decoding.
//
// A ByteBuf never grows: writing past its capacity sets `overflow` and drops
// the bytes, and the caller decides what that means. A ByteReader never reads
// past its end: a short read sets `bad` and returns zero, so a decoder can
// read a whole record and check once.
#pragma once

#include "arena.h"
#include "base.h"

typedef struct {
	uint8_t *data;
	ptrdiff_t cap;
	ptrdiff_t len;
	bool overflow;
} ByteBuf;

static inline ByteBuf bytebuf_make(Arena *a, ptrdiff_t cap)
{
	return (ByteBuf){PUSH(a, uint8_t, cap), cap, 0, false};
}

static inline void put_bytes(ByteBuf *b, const void *p, ptrdiff_t n)
{
	if (b->overflow || n > b->cap - b->len) {
		b->overflow = true;
		return;
	}
	if (n) memcpy(b->data + b->len, p, (size_t)n);
	b->len += n;
}

static inline void put8(ByteBuf *b, uint8_t x) { put_bytes(b, &x, 1); }
static inline void put16(ByteBuf *b, uint16_t x)
{
	uint8_t v[2] = {(uint8_t)x, (uint8_t)(x >> 8)};
	put_bytes(b, v, 2);
}
static inline void put32(ByteBuf *b, uint32_t x)
{
	uint8_t v[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)};
	put_bytes(b, v, 4);
}
static inline void put64(ByteBuf *b, uint64_t x)
{
	put32(b, (uint32_t)x);
	put32(b, (uint32_t)(x >> 32));
}
static inline void putf(ByteBuf *b, float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof bits);
	put32(b, bits);
}

typedef struct {
	const uint8_t *data;
	ptrdiff_t len;
	ptrdiff_t pos;
	bool bad;
} ByteReader;

static inline ByteReader byte_reader(const void *data, ptrdiff_t len)
{
	return (ByteReader){data, len, 0, false};
}

static inline const uint8_t *take(ByteReader *r, ptrdiff_t n)
{
	if (r->bad || n > r->len - r->pos) {
		r->bad = true;
		return nullptr;
	}
	const uint8_t *p = r->data + r->pos;
	r->pos += n;
	return p;
}

static inline uint8_t get8(ByteReader *r)
{
	const uint8_t *p = take(r, 1);
	return p ? p[0] : 0;
}
static inline uint16_t get16(ByteReader *r)
{
	const uint8_t *p = take(r, 2);
	return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
}
static inline uint32_t get32(ByteReader *r)
{
	const uint8_t *p = take(r, 4);
	return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}
static inline uint64_t get64(ByteReader *r)
{
	uint64_t lo = get32(r);
	return lo | (uint64_t)get32(r) << 32;
}
static inline float getf(ByteReader *r)
{
	uint32_t bits = get32(r);
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}
