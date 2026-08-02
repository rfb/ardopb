#include "shell/telemetry_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/net.h"
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
	ardop_socket listen_fd;
	ardop_socket fd;

	/* Ring of whole records awaiting the socket. head..tail, wrapping. */
	uint8_t buf[TLM_QUEUE_BYTES];
	size_t head;      /* next byte to write to the socket. */
	size_t len;       /* bytes queued. */

	unsigned long dropped;   /* records discarded for backpressure. */
	bool warned;             /* so the drop warning is printed once. */
};

static void drop_client(ardop_telemetry_tcp *t)
{
	if (ardop_net_valid(t->fd)) {
		ardop_net_close(&t->fd);
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
	while (ardop_net_valid(t->fd) && t->len > 0) {
		size_t run = TLM_QUEUE_BYTES - t->head;
		if (run > t->len)
			run = t->len;

		size_t moved = 0;
		ardop_net_status st = ardop_net_send(t->fd, t->buf + t->head,
						     run, &moved);
		if (st == ARDOP_NET_OK && moved > 0) {
			t->head = (t->head + moved) % TLM_QUEUE_BYTES;
			t->len -= moved;
			continue;
		}
		if (st == ARDOP_NET_OK || st == ARDOP_NET_AGAIN)
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

	if (!ardop_net_valid(t->fd))
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
	t->fd = ARDOP_SOCKET_INVALID;

	t->listen_fd = ardop_net_listen(port);
	if (!ardop_net_valid(t->listen_fd)) {
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

	if (!ardop_net_valid(t->fd)) {
		ardop_socket fd = ardop_net_accept(t->listen_fd, NULL, 0);
		if (ardop_net_valid(fd)) {
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
	if (ardop_net_valid(t->fd)) {
		uint8_t discard[64];
		size_t got = 0;
		/* The socket is already non-blocking, so no per-call flag is
		 * needed -- MSG_DONTWAIT does not exist on Winsock anyway. */
		if (ardop_net_recv(t->fd, discard, sizeof(discard), &got)
		    == ARDOP_NET_CLOSED) {
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
	ardop_net_close(&t->listen_fd);
	if (t->dropped)
		fprintf(stderr, "telemetry: %lu record(s) dropped\n",
			t->dropped);
	free(t);
}
