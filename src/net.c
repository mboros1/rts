#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static bool set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	return true;
}

int net_listen(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 16) < 0 || !set_nonblocking(fd)) {
		close(fd);
		return -1;
	}
	return fd;
}

int net_accept(int listen_fd)
{
	int fd = accept(listen_fd, nullptr, nullptr);
	if (fd < 0) return -1;
	if (!set_nonblocking(fd)) {
		close(fd);
		return -1;
	}
	return fd;
}

int net_connect(const char *host, int port)
{
	struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
	char service[16];
	snprintf(service, sizeof service, "%d", port);
	if (getaddrinfo(host, service, &hints, &res) != 0) return -1;
	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	bool ok = fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0 && set_nonblocking(fd);
	freeaddrinfo(res);
	if (!ok) {
		if (fd >= 0) close(fd);
		return -1;
	}
	return fd;
}

void net_close_fd(int fd)
{
	if (fd >= 0) close(fd);
}

void net_sleep(double seconds)
{
	struct timespec ts = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
	nanosleep(&ts, nullptr);
}

void conn_init(Conn *c, Arena *a, int fd, ptrdiff_t out_cap, ptrdiff_t in_cap)
{
	ASSERT(is_pow2(out_cap));
	*c = (Conn){
		.fd = fd,
		.open = fd >= 0,
		.out = PUSH(a, uint8_t, out_cap),
		.out_cap = out_cap,
		.pending_cap = 1 << 14,
		.in = PUSH(a, uint8_t, in_cap),
		.in_cap = in_cap,
	};
	c->pending = PUSH(a, PendingFrame, c->pending_cap);
}

void conn_reopen(Conn *c, int fd)
{
	c->fd = fd;
	c->open = fd >= 0;
	c->why_closed = nullptr;
	c->out_written = c->out_queued = c->out_released = 0;
	c->pending_head = c->pending_tail = 0;
	c->in_len = c->in_consumed = 0;
	c->last_release = 0.0;
}

void conn_set_delay(Conn *c, double latency, double jitter, uint64_t seed)
{
	c->latency = latency;
	c->jitter = jitter;
	c->rng = rng_seed(seed);
}

void conn_close(Conn *c, const char *why)
{
	if (!c->open) return;
	net_close_fd(c->fd);
	c->open = false;
	c->fd = -1;
	c->why_closed = why;
}

static void out_write(Conn *c, const void *p, ptrdiff_t n)
{
	const uint8_t *b = p;
	for (ptrdiff_t i = 0; i < n; i++) c->out[(c->out_queued + i) & (c->out_cap - 1)] = b[i];
	c->out_queued += n;
}

bool conn_send(Conn *c, uint8_t kind, const void *payload, ptrdiff_t len, double now)
{
	if (!c->open) return false;
	ptrdiff_t frame = 5 + len;
	if (len > FRAME_MAX || c->out_queued + frame - c->out_written > c->out_cap
	    || c->pending_tail - c->pending_head == c->pending_cap) {
		conn_close(c, "peer too slow: send queue full");
		return false;
	}
	uint32_t n = (uint32_t)(1 + len);
	uint8_t head[5] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24), kind};
	out_write(c, head, 5);
	out_write(c, payload, len);
	double delay = c->latency;
	if (c->jitter > 0.0) delay += rng_range(&c->rng, (float)-c->jitter, (float)c->jitter);
	double release = now + (delay > 0.0 ? delay : 0.0);
	if (release < c->last_release) release = c->last_release; // never reorder
	c->last_release = release;
	c->pending[c->pending_tail++ & (c->pending_cap - 1)] = (PendingFrame){release, c->out_queued};
	return true;
}

void conn_pump(Conn *c, double now)
{
	if (!c->open) return;
	// Frames whose delay has passed become writable.
	while (c->pending_head < c->pending_tail) {
		PendingFrame *p = &c->pending[c->pending_head & (c->pending_cap - 1)];
		if (p->release > now) break;
		c->out_released = p->end;
		c->pending_head++;
	}
	while (c->out_written < c->out_released) {
		ptrdiff_t at = (ptrdiff_t)(c->out_written & (c->out_cap - 1));
		ptrdiff_t n = (ptrdiff_t)(c->out_released - c->out_written);
		if (n > c->out_cap - at) n = c->out_cap - at;   // up to the ring's end
		ssize_t w = send(c->fd, c->out + at, (size_t)n, 0);
		if (w < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			conn_close(c, "send failed");
			return;
		}
		c->out_written += w;
		c->bytes_sent += w;
	}
	// Drop consumed input, then read.
	if (c->in_consumed > 0) {
		memmove(c->in, c->in + c->in_consumed, (size_t)(c->in_len - c->in_consumed));
		c->in_len -= c->in_consumed;
		c->in_consumed = 0;
	}
	while (c->in_len < c->in_cap) {
		ssize_t r = recv(c->fd, c->in + c->in_len, (size_t)(c->in_cap - c->in_len), 0);
		if (r > 0) {
			c->in_len += r;
			c->bytes_received += r;
			continue;
		}
		if (r == 0) conn_close(c, "peer closed the connection");
		else if (errno != EAGAIN && errno != EWOULDBLOCK) conn_close(c, "receive failed");
		break;
	}
}

bool conn_next(Conn *c, Frame *f)
{
	ptrdiff_t avail = c->in_len - c->in_consumed;
	if (avail < 5) return false;
	const uint8_t *p = c->in + c->in_consumed;
	uint32_t n = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	if (n == 0 || n > FRAME_MAX + 1) {
		conn_close(c, "malformed frame length");
		return false;
	}
	if ((ptrdiff_t)n + 4 > c->in_cap) {
		conn_close(c, "frame larger than the receive buffer");
		return false;
	}
	if ((ptrdiff_t)n + 4 > avail) return false;
	*f = (Frame){p[4], p + 5, (ptrdiff_t)n - 1};
	c->in_consumed += (ptrdiff_t)n + 4;
	return true;
}
