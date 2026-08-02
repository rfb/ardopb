#include "shell/host_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/host.h"
#include "shell/net.h"

/**
 * @file host_tcp.c
 * @brief The TCP command + data channel transport (see host_tcp.h).
 *
 * Talks to sockets, so it is exercised against a host, not the in-process suite
 * (which tests ardop_host_command and the framing helpers directly). Held to the
 * core -Werror bar. Every socket operation goes through [net.h](net.h), which is
 * why nothing here is conditional on the platform.
 *
 * ## Outbound queues
 *
 * Both channels are non-blocking, so `send` may take part of a record and no
 * more. Discarding that short count truncates a message mid-frame, and because
 * the data channel is length-prefixed, one truncation desynchronises it
 * permanently -- every subsequent frame is misparsed. So each direction has a
 * queue in front of the socket, drained from ::ardop_host_tcp_service.
 *
 * The overflow policy is the opposite of telemetry's, deliberately.
 * `telemetry_tcp.c` drops the oldest record because a dropped spectrum row
 * costs one frame of a display. Here, a dropped `RX_DATA` frame is lost user
 * payload and a dropped notification desynchronises the host's idea of the
 * session. Neither is survivable, so a host that will not drain its own channel
 * is disconnected and told why.
 */

#define LINE_MAX 3200
#define DATA_BUF_MAX 8192

/* Outbound capacity. The command channel carries short text; the data channel
 * carries up to DATA_BUF_MAX-sized frames, so it gets room for several. */
#define CMD_OUT_MAX 8192
#define DATA_OUT_MAX 65536

struct ardop_host_tcp {
	/* Command channel: CR/LF-delimited text on `port`. */
	ardop_socket cmd_listen_fd;
	ardop_socket cmd_fd;
	char line[LINE_MAX];
	size_t line_len;
	uint8_t cmd_out[CMD_OUT_MAX];
	size_t cmd_out_len;

	/* Data channel: length-prefixed binary on `port` + 1. */
	ardop_socket data_listen_fd;
	ardop_socket data_fd;
	uint8_t data_buf[DATA_BUF_MAX];
	size_t data_buf_len;
	uint8_t data_out[DATA_OUT_MAX];
	size_t data_out_len;

	/* Who is attached, for an interface that has to say which machine holds
	 * the transmitter. */
	char cmd_peer[ARDOP_NET_PEER_MAX];
	char data_peer[ARDOP_NET_PEER_MAX];

	ardop_host_observer_fn obs;
	void *obs_ctx;
	uint16_t port;
	bool announced;   /* the listening event, once an observer exists */
};

/*
 * Report, to whoever is watching.
 *
 * Also to stderr, because ardopb is a daemon with a terminal and its operator
 * reads it there. The observer is additive: it does not replace the log, it
 * gives a windowed application the same information.
 */
static void report(ardop_host_tcp *h, ardop_host_ev_kind kind,
		   const char *channel, const char *detail, const char *reply)
{
	if (h->obs)
		h->obs(h->obs_ctx, kind, channel, detail ? detail : "", reply);
}

/* --- outbound queues ------------------------------------------------------- */

/* Append to a channel's queue. False means it would not fit. */
static bool queue_out(uint8_t *buf, size_t cap, size_t *len, const void *src,
		      size_t n)
{
	if (n > cap - *len)
		return false;
	memcpy(buf + *len, src, n);
	*len += n;
	return true;
}

/* Push whatever the socket will take. False means the client is gone. */
static bool flush_out(ardop_socket s, uint8_t *buf, size_t *len)
{
	while (*len > 0) {
		size_t moved = 0;
		ardop_net_status st = ardop_net_send(s, buf, *len, &moved);
		if (st == ARDOP_NET_OK && moved > 0) {
			*len -= moved;
			memmove(buf, buf + moved, *len);
			continue;
		}
		if (st == ARDOP_NET_OK || st == ARDOP_NET_AGAIN)
			return true;   /* socket full; the rest waits. */
		return false;
	}
	return true;
}

static void drop(ardop_host_tcp *h, ardop_socket *client_fd, const char *what,
		 char *peer)
{
	if (ardop_net_valid(*client_fd)) {
		ardop_net_close(client_fd);
		fprintf(stderr, "host: %s client disconnected\n", what);
		report(h, ARDOP_HOST_EV_DISCONNECTED, what, peer, NULL);
		if (peer)
			peer[0] = '\0';
	}
}

/* Queue for the command channel, dropping the client if it has stalled. */
static void emit_cmd(ardop_host_tcp *h, const void *src, size_t n)
{
	if (!ardop_net_valid(h->cmd_fd))
		return;
	if (!queue_out(h->cmd_out, sizeof(h->cmd_out), &h->cmd_out_len, src, n)) {
		fprintf(stderr, "host: cmd client is not reading; disconnecting "
			"rather than losing notifications\n");
		h->cmd_out_len = 0;
		drop(h, &h->cmd_fd, "cmd", h->cmd_peer);
		return;
	}
	if (!flush_out(h->cmd_fd, h->cmd_out, &h->cmd_out_len)) {
		h->cmd_out_len = 0;
		drop(h, &h->cmd_fd, "cmd", h->cmd_peer);
	}
}

/* --- listeners ------------------------------------------------------------- */

ardop_host_tcp *ardop_host_tcp_open(uint16_t port)
{
	struct ardop_host_tcp *h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;
	h->cmd_fd = ARDOP_SOCKET_INVALID;
	h->data_fd = ARDOP_SOCKET_INVALID;

	h->cmd_listen_fd = ardop_net_listen(port);
	h->data_listen_fd = ardop_net_listen((uint16_t)(port + 1));
	if (!ardop_net_valid(h->cmd_listen_fd)
	    || !ardop_net_valid(h->data_listen_fd)) {
		ardop_host_tcp_close(h);
		return NULL;
	}
	h->port = port;
	fprintf(stderr, "host: listening on ports %u (cmd) and %u (data)\n",
		(unsigned)port, (unsigned)(port + 1));
	return h;
}

/*
 * Accept a client, or refuse one.
 *
 * A station has one host. Leaving a second connection in the backlog makes it
 * look accepted-but-silent to whoever dialled it, which is the worst of the
 * available behaviours; refusing it explicitly is the contract analysis/14
 * asks for.
 */
static void accept_into(ardop_host_tcp *h, ardop_socket listen_fd,
			ardop_socket *client_fd, const char *what,
			const char *refusal, char *peer)
{
	char who[ARDOP_NET_PEER_MAX];
	ardop_socket fd = ardop_net_accept(listen_fd, who, sizeof who);
	if (!ardop_net_valid(fd))
		return;   /* none pending. */

	if (ardop_net_valid(*client_fd)) {
		if (refusal) {
			size_t moved = 0;
			(void)ardop_net_send(fd, refusal, strlen(refusal),
					     &moved);
		}
		ardop_net_close(&fd);
		fprintf(stderr, "host: refused a second %s client\n", what);
		report(h, ARDOP_HOST_EV_REFUSED, what, who, NULL);
		return;
	}
	*client_fd = fd;
	if (peer)
		snprintf(peer, ARDOP_NET_PEER_MAX, "%s", who);
	fprintf(stderr, "host: %s client connected\n", what);
	report(h, ARDOP_HOST_EV_CONNECTED, what, who, NULL);
}

/* --- the command channel --------------------------------------------------- */

/* Run one complete command line: process and reply. */
static void process_line(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	char reply[1024];
	ardop_host_command(rt, h->line, now, reply, sizeof(reply));
	/*
	 * Reported whatever the reply, including none. A command that produced
	 * no answer is still a thing a guest did to this station, and Decision 4
	 * asks for visibility rather than protection.
	 */
	report(h, ARDOP_HOST_EV_COMMAND, "cmd", h->line, reply);
	if (reply[0] != '\0') {
		char out[1088];
		int m = snprintf(out, sizeof(out), "%s\r", reply);
		if (m > 0)
			emit_cmd(h, out, (size_t)m);
	}
}

/* Read and process the command channel. */
static void service_cmd(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	accept_into(h, h->cmd_listen_fd, &h->cmd_fd, "cmd",
		    "FAULT station already has a host\r", h->cmd_peer);
	if (!ardop_net_valid(h->cmd_fd))
		return;

	char buf[1024];
	size_t got = 0;
	ardop_net_status st = ardop_net_recv(h->cmd_fd, buf, sizeof(buf), &got);
	if (st == ARDOP_NET_CLOSED || st == ARDOP_NET_ERROR) {
		drop(h, &h->cmd_fd, "cmd", h->cmd_peer);
		h->line_len = 0;
		h->cmd_out_len = 0;
		return;
	}

	for (size_t i = 0; i < got; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n') {
			if (h->line_len == 0)
				continue;
			h->line[h->line_len] = '\0';
			process_line(h, rt, now);
			h->line_len = 0;
		} else if (h->line_len < sizeof(h->line) - 1) {
			h->line[h->line_len++] = c;
		}
	}

	/* Drain anything the earlier replies could not push. */
	if (ardop_net_valid(h->cmd_fd)
	    && !flush_out(h->cmd_fd, h->cmd_out, &h->cmd_out_len)) {
		h->cmd_out_len = 0;
		drop(h, &h->cmd_fd, "cmd", h->cmd_peer);
	}
}

/* --- the data channel ------------------------------------------------------ */

/* Read the data channel and queue each complete message as SEND_DATA. */
static void service_data(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	/* No refusal string on this channel, unlike the command one, and that is
	 * deliberate: it carries length-prefixed binary, so an ASCII line would
	 * be read as a frame header by the very client it was meant to inform.
	 * Connect-then-EOF is the only signal that cannot be misparsed. */
	accept_into(h, h->data_listen_fd, &h->data_fd, "data", NULL,
		    h->data_peer);
	if (!ardop_net_valid(h->data_fd))
		return;

	if (h->data_buf_len < sizeof(h->data_buf)) {
		size_t got = 0;
		ardop_net_status st = ardop_net_recv(
			h->data_fd, h->data_buf + h->data_buf_len,
			sizeof(h->data_buf) - h->data_buf_len, &got);
		if (st == ARDOP_NET_CLOSED || st == ARDOP_NET_ERROR) {
			drop(h, &h->data_fd, "data", h->data_peer);
			h->data_buf_len = 0;
			h->data_out_len = 0;
			return;
		}
		h->data_buf_len += got;
	}

	const uint8_t *payload;
	size_t plen, consumed;
	while (ardop_host_data_parse(h->data_buf, h->data_buf_len, &payload,
				     &plen, &consumed)) {
		ardop_host_cmd sd = {.kind = ARDOP_CMD_SEND_DATA,
				     .data = payload, .data_len = plen};
		ardop_runtime_host(rt, &sd, now);
		memmove(h->data_buf, h->data_buf + consumed,
			h->data_buf_len - consumed);
		h->data_buf_len -= consumed;
	}

	/* A message larger than the buffer can never complete; resync rather
	 * than wedge. (Real ARDOP frames are well under DATA_BUF_MAX.) */
	if (h->data_buf_len == sizeof(h->data_buf))
		h->data_buf_len = 0;

	if (!flush_out(h->data_fd, h->data_out, &h->data_out_len)) {
		h->data_out_len = 0;
		drop(h, &h->data_fd, "data", h->data_peer);
	}
}

void ardop_host_tcp_service(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	if (h && h->obs && !h->announced) {
		char detail[64];
		snprintf(detail, sizeof detail, "%u (cmd), %u (data)",
			 (unsigned)h->port, (unsigned)(h->port + 1));
		report(h, ARDOP_HOST_EV_LISTENING, "cmd", detail, NULL);
		h->announced = true;
	}

	/* Data before commands. A host loads the buffer on the data channel and
	 * then acts on it from the command channel (write data, then FECSEND);
	 * servicing commands first would run the command against the buffer as
	 * it stood before this tick's data arrived. */
	service_data(h, rt, now);
	service_cmd(h, rt, now);
}

void ardop_host_tcp_observe(ardop_host_tcp *h, ardop_host_observer_fn fn,
			    void *ctx)
{
	if (!h)
		return;
	h->obs = fn;
	h->obs_ctx = ctx;
	h->announced = false;   /* re-announce, so a late observer still hears it */
}

const char *ardop_host_tcp_peer(const ardop_host_tcp *h)
{
	return h ? h->cmd_peer : "";
}

void ardop_host_tcp_notify(ardop_host_tcp *h, const char *msg)
{
	if (!h || !ardop_net_valid(h->cmd_fd))
		return;
	char out[1088];
	int m = snprintf(out, sizeof(out), "%s\r", msg);
	if (m > 0)
		emit_cmd(h, out, (size_t)m);
}

void ardop_host_tcp_send_data(ardop_host_tcp *h, const char *tag,
			      const uint8_t *data, size_t len)
{
	if (!h || !ardop_net_valid(h->data_fd))
		return;
	uint8_t out[2 + ARDOP_HOST_TAG_LEN + 2048];
	size_t n = ardop_host_data_frame(out, sizeof(out), tag, data, len);
	if (n == 0)
		return;

	if (!queue_out(h->data_out, sizeof(h->data_out), &h->data_out_len, out,
		       n)) {
		fprintf(stderr, "host: data client is not reading; "
			"disconnecting rather than truncating a frame\n");
		h->data_out_len = 0;
		drop(h, &h->data_fd, "data", h->data_peer);
		return;
	}
	if (!flush_out(h->data_fd, h->data_out, &h->data_out_len)) {
		h->data_out_len = 0;
		drop(h, &h->data_fd, "data", h->data_peer);
	}
}

bool ardop_host_tcp_client_connected(const ardop_host_tcp *h)
{
	/* Either channel, not just the command one. A client that opened only the
	 * data port can still queue SEND_DATA through service_data(), which is
	 * precisely the interleaving this signal exists to prevent. */
	return h && (ardop_net_valid(h->cmd_fd) || ardop_net_valid(h->data_fd));
}

void ardop_host_tcp_close(ardop_host_tcp *h)
{
	if (!h)
		return;
	drop(h, &h->cmd_fd, "cmd", h->cmd_peer);
	drop(h, &h->data_fd, "data", h->data_peer);
	ardop_net_close(&h->cmd_listen_fd);
	ardop_net_close(&h->data_listen_fd);
	free(h);
}
