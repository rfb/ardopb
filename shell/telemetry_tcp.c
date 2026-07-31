#include "shell/telemetry_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "shell/telemetry.h"

/**
 * @file telemetry_tcp.c
 * @brief The telemetry transport (see telemetry_tcp.h).
 *
 * Structured like host_tcp.c -- non-blocking listener, one client, accept and
 * drop in the service call -- but write-only, and with a drop-oldest queue in
 * front of the socket so a stalled display cannot stall the modem.
 */

/*
 * Queue depth. A spectrum row is ~830 bytes at ~11.7/s and a constellation
 * snapshot up to ~8 kB per frame, so 256 kB is a few seconds of slack: enough
 * to ride out a display repainting or a scheduler hiccup, small enough that a
 * consumer which has genuinely stopped is noticed rather than buffered forever.
 */
#define TLM_QUEUE_BYTES (256 * 1024)

struct ardop_telemetry_tcp {
	int listen_fd;
	int fd;

	/* Ring of whole records awaiting the socket. head..tail, wrapping. */
	uint8_t buf[TLM_QUEUE_BYTES];
	size_t head;      /* next byte to write to the socket. */
	size_t len;       /* bytes queued. */

	unsigned long dropped;   /* records discarded for backpressure. */
	bool warned;             /* so the drop warning is printed once. */
};

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int open_listener(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("telemetry: socket");
		return -1;
	}
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
	    || listen(fd, 1) < 0) {
		perror("telemetry: bind/listen");
		close(fd);
		return -1;
	}
	set_nonblock(fd);
	return fd;
}

static void drop_client(ardop_telemetry_tcp *t)
{
	if (t->fd >= 0) {
		close(t->fd);
		t->fd = -1;
		fprintf(stderr, "telemetry: client disconnected\n");
	}
	t->head = 0;
	t->len = 0;
}

/* --- the drop-oldest queue ------------------------------------------------ */

/* Discard the record at the head. Records are self-delimiting (kind + u16
 * length), so the oldest can be found and removed without a side index. */
static bool drop_oldest(ardop_telemetry_tcp *t)
{
	if (t->len < ARDOP_TLM_HEADER_LEN)
		return false;

	/* The length field may straddle the wrap, so read it bytewise. */
	size_t lo = (t->head + 1) % TLM_QUEUE_BYTES;
	size_t hi = (t->head + 2) % TLM_QUEUE_BYTES;
	size_t payload = (size_t)t->buf[lo] | ((size_t)t->buf[hi] << 8);
	size_t total = ARDOP_TLM_HEADER_LEN + payload;

	if (total > t->len)
		return false;   /* corrupt: caller resets. */

	t->head = (t->head + total) % TLM_QUEUE_BYTES;
	t->len -= total;
	t->dropped++;
	return true;
}

static void queue_bytes(ardop_telemetry_tcp *t, const uint8_t *b, size_t n)
{
	if (n > TLM_QUEUE_BYTES)
		return;

	while (TLM_QUEUE_BYTES - t->len < n) {
		if (!drop_oldest(t)) {
			/* Should be unreachable; resync rather than spin. */
			t->head = 0;
			t->len = 0;
			break;
		}
	}

	size_t tail = (t->head + t->len) % TLM_QUEUE_BYTES;
	size_t first = TLM_QUEUE_BYTES - tail;
	if (first > n)
		first = n;
	memcpy(t->buf + tail, b, first);
	if (n > first)
		memcpy(t->buf, b + first, n - first);
	t->len += n;
}

/* Push whatever the socket will take right now. Never blocks. */
static void flush(ardop_telemetry_tcp *t)
{
	while (t->fd >= 0 && t->len > 0) {
		size_t run = TLM_QUEUE_BYTES - t->head;
		if (run > t->len)
			run = t->len;

		ssize_t w = send(t->fd, t->buf + t->head, run, MSG_NOSIGNAL);
		if (w > 0) {
			t->head = (t->head + (size_t)w) % TLM_QUEUE_BYTES;
			t->len -= (size_t)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;   /* socket full: the rest waits. */
		drop_client(t);
		return;
	}
}

/* --- the sink ------------------------------------------------------------- */

static void on_telemetry(void *ctx, const ardop_telemetry *rec)
{
	ardop_telemetry_tcp *t = ctx;
	uint8_t enc[ARDOP_TLM_MAX_RECORD];

	if (t->fd < 0)
		return;   /* nobody watching: encode nothing. */

	size_t n = ardop_tlm_encode(rec, enc, sizeof(enc));
	if (n == 0)
		return;

	size_t before = t->dropped;
	queue_bytes(t, enc, n);
	if (t->dropped != before && !t->warned) {
		t->warned = true;
		fprintf(stderr,
			"telemetry: consumer too slow, dropping records\n");
	}
	flush(t);
}

/* --- lifecycle ------------------------------------------------------------ */

ardop_telemetry_tcp *ardop_telemetry_tcp_open(uint16_t port)
{
	ardop_telemetry_tcp *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->fd = -1;

	t->listen_fd = open_listener(port);
	if (t->listen_fd < 0) {
		free(t);
		return NULL;
	}
	fprintf(stderr, "telemetry: listening on port %u\n", (unsigned)port);
	return t;
}

void ardop_telemetry_tcp_attach(ardop_telemetry_tcp *t, ardop_runtime *rt)
{
	if (t)
		ardop_runtime_set_telemetry(rt, on_telemetry, t);
}

void ardop_telemetry_tcp_service(ardop_telemetry_tcp *t, ardop_runtime *rt)
{
	if (!t)
		return;

	if (t->fd < 0) {
		int fd = accept(t->listen_fd, NULL, NULL);
		if (fd >= 0) {
			set_nonblock(fd);
			t->fd = fd;
			t->head = 0;
			t->len = 0;
			t->warned = false;
			fprintf(stderr, "telemetry: client connected\n");

			uint8_t hello[ARDOP_TLM_HELLO_LEN];
			size_t n = ardop_tlm_encode_hello(hello);
			queue_bytes(t, hello, n);
			/* Paint the panel now rather than at the next state
			 * change, which on a quiet channel could be minutes. */
			ardop_runtime_telemetry_status(rt);
		}
	}

	/* A one-way stream still has to notice the peer leaving: a read of 0
	 * is the only in-band signal until a write happens to fail. */
	if (t->fd >= 0) {
		uint8_t discard[64];
		ssize_t r = recv(t->fd, discard, sizeof(discard), MSG_DONTWAIT);
		if (r == 0) {
			drop_client(t);
			return;
		}
	}

	flush(t);
}

void ardop_telemetry_tcp_close(ardop_telemetry_tcp *t)
{
	if (!t)
		return;
	drop_client(t);
	if (t->listen_fd >= 0)
		close(t->listen_fd);
	if (t->dropped)
		fprintf(stderr, "telemetry: %lu record(s) dropped\n",
			t->dropped);
	free(t);
}
