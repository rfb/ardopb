#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/net.h"
#include "shell/sys.h"

#include "app/asp_wire.h"   /* asp_looks_like_hello, and nothing else */

#include "hostclient.h"

/**
 * @file ardop_cat.c
 * @brief ardop-cat -- a pipe over the radio, in whichever direction.
 *
 * One binary with the direction chosen by a flag, which is netcat's shape and
 * for netcat's reason: `nc host port` and `nc -l port` are one program because
 * they are one idea, and two binaries would be two places to fix anything.
 *
 *     ardop-cat --host 127.0.0.1:8600 N0BBB < send.bin     # dial and send
 *     ardop-cat --host 127.0.0.1:8700 --listen > got.bin   # answer and receive
 *
 * ## It is `cat`, and the name is the specification
 *
 * [analysis/17](../analysis/17-application-protocol.md) opens by describing the
 * tool this replaces: *"`ardop-tx` is `cat` over ARQ. No framing, no filename,
 * no length, no checksum, no resume, no completion signal."* Every one of those
 * is still true and none of them is a defect -- it is what a pipe is. The old
 * names implied this was *the* way to move data between stations, and since the
 * station application there is a real protocol for that (ASP), so the pipe
 * should be named as the pipe.
 *
 * What you get is netcat's guarantee, which is worth stating exactly: the bytes
 * that arrive are the bytes that were sent, in order, or the link dropped and
 * this exits non-zero. It sends nothing about the data, so the receiver learns
 * nothing about it -- not its name, not its length, not whether it is complete.
 * Check it yourself, the way you would with `nc`.
 *
 * ## Where it does not follow netcat
 *
 * **It is one direction per invocation.** `nc` is full duplex because TCP is;
 * this link is half duplex with an explicit turnover, and the completion rule
 * below -- everything drained and acked, then disconnect -- is one-directional
 * by nature. A bidirectional pipe is a feature, not a rename, and it is not here.
 *
 * **It refuses rather than staying silent about a mismatch.** `nc` pointed at an
 * HTTPS port prints binary garbage at you, and that is correct for `nc`: you are
 * on both ends, and the terminal is where it lands. Here the far end is a
 * stranger who may be running the station application, and what would land is a
 * file you go on to trust. So the one thing this checks is whether the peer is
 * speaking ASP, because writing that framing into a file produces a corrupt file
 * and an exit status of zero.
 */

/* Keep no more than this many bytes queued in the modem (its buffer is larger;
 * this leaves headroom and gives smooth flow control). */
#define TX_WINDOW 8192
#define CHUNK 2048

struct state { bool disconnected; };

static void on_line(void *ctx, const char *line)
{
	struct state *s = ctx;
	if (strcmp(line, "DISCONNECTED") == 0)
		s->disconnected = true;
	fprintf(stderr, "[modem] %s\n", line);
}

/* Parse "HOST:PORT" (or "PORT", or "HOST") with defaults. */
static void parse_host(const char *s, char *host, size_t hostcap, int *port)
{
	snprintf(host, hostcap, "127.0.0.1");
	*port = 8515;
	if (!s)
		return;
	const char *colon = strrchr(s, ':');
	if (colon) {
		size_t hl = (size_t)(colon - s);
		if (hl > 0 && hl < hostcap) {
			memcpy(host, s, hl);
			host[hl] = '\0';
		}
		*port = atoi(colon + 1);
	} else if (s[0] >= '0' && s[0] <= '9') {
		*port = atoi(s);
	} else {
		snprintf(host, hostcap, "%s", s);
	}
}

static void complain_about_asp(bool receiving)
{
	fprintf(stderr,
		"\n"
		"the other station is running the station application, which speaks a\n"
		"framed protocol (ASP) rather than a raw byte stream.\n"
		"\n");
	if (receiving)
		fprintf(stderr,
			"writing that to the output would interleave its framing with the file\n"
			"contents and produce a corrupt file with a successful exit status, so\n"
			"nothing has been written. run ardop-station to receive it, which will\n"
			"also check it against the sender's checksum.\n");
	else
		fprintf(stderr,
			"it will show this stream as chat text and will not save it as a file.\n"
			"run ardop-station at this end too, and offer the file from its Files\n"
			"screen.\n");
}

/* --- receiving -------------------------------------------------------------- */

static int do_listen(hostclient *hc)
{
	hc_cmd(hc, "LISTEN TRUE");   /* ensure the modem answers. */

	fprintf(stderr, "waiting for a connection ...\n");
	bool connected = false;
	while (!connected) {
		char line[512];
		int r = hc_next_line(hc, line, sizeof(line), -1);
		if (r <= 0) {
			fprintf(stderr, "lost the modem\n");
			return 1;
		}
		fprintf(stderr, "[modem] %s\n", line);
		if (strncmp(line, "CONNECTED", 9) == 0)
			connected = true;
	}

	int rc = 0;
	bool first = true;
	char skipped[HC_TAG_LEN + 1] = {0};

	for (;;) {
		const ardop_socket socks[2] = { hc->cmd_fd, hc->data_fd };
		bool ready[2] = { false, false };
		if (ardop_net_wait(socks, 2, -1, ready) < 0)
			break;

		if (ready[1]) {
			for (;;) {
				uint8_t payload[4096];
				char tag[HC_TAG_LEN + 1];
				int n = hc_recv_data(hc, payload,
						     sizeof(payload), tag, 0);
				if (n == -2)
					break;            /* no full message. */
				if (n < 0) {
					rc = 1;
					goto done;
				}
				if (n == 0)
					continue;

				/*
				 * The tag is read rather than ignored.
				 *
				 * analysis/17 opens by naming this: the tool
				 * this replaces received the tag into a buffer
				 * and never looked at it, so an error marker or
				 * a station identification landed in the middle
				 * of the file. Only ARQ payload is the stream.
				 */
				if (strcmp(tag, "ARQ") != 0) {
					if (strcmp(skipped, tag) != 0) {
						snprintf(skipped, sizeof skipped,
							 "%s", tag);
						fprintf(stderr,
							"[skipped %s-tagged payload: "
							"not part of the stream]\n",
							tag);
					}
					continue;
				}

				if (first) {
					first = false;
					if (asp_looks_like_hello(payload, (size_t)n)) {
						complain_about_asp(true);
						rc = 2;
						goto done;
					}
				}

				fwrite(payload, 1, (size_t)n, stdout);
				fflush(stdout);
			}
		}
		if (ready[0]) {
			char line[512];
			int lr = hc_next_line(hc, line, sizeof(line), 0);
			if (lr < 0) {
				rc = 1;
				break;
			}
			if (lr == 1) {
				fprintf(stderr, "[modem] %s\n", line);
				if (strcmp(line, "DISCONNECTED") == 0)
					break;   /* end of stream. */
			}
		}
	}
done:
	fflush(stdout);
	return rc;
}

/* --- sending ---------------------------------------------------------------- */

/* Query the modem's send-buffer length; -1 on error/disconnect. */
static int query_buffer(hostclient *hc, struct state *st)
{
	char buf[128];
	int r = hc_query(hc, "BUFFER", "BUFFER ", buf, sizeof(buf), 5000,
			 on_line, st);
	if (r <= 0 || st->disconnected)
		return -1;
	return atoi(buf + strlen("BUFFER "));
}

/*
 * Anything the peer says while we are sending.
 *
 * A pipe has nothing to do with inbound data and the tool this replaces never
 * read it -- but it is the one place the mismatch is detectable from this end: a
 * station application answers a connection with its HELLO before anything else.
 * Reported once, and not fatal, because the operator may know exactly what they
 * are doing and the bytes are still going out.
 */
static void peek_at_peer(hostclient *hc, bool *warned)
{
	uint8_t payload[4096];
	char tag[HC_TAG_LEN + 1];
	int n = hc_recv_data(hc, payload, sizeof(payload), tag, 0);
	if (n <= 0 || *warned)
		return;
	if (strcmp(tag, "ARQ") == 0 && asp_looks_like_hello(payload, (size_t)n)) {
		*warned = true;
		complain_about_asp(false);
	}
}

static int do_send(hostclient *hc, const char *target)
{
	struct state st = {0};
	bool warned = false;

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "ARQCALL %s 5", target);
	fprintf(stderr, "dialing %s ...\n", target);
	hc_cmd(hc, cmd);

	/* Wait for the connection to come up (or fail). */
	for (;;) {
		char line[512];
		int r = hc_next_line(hc, line, sizeof(line), 60000);
		if (r <= 0) {
			fprintf(stderr, "no response from modem\n");
			return 1;
		}
		if (strncmp(line, "CONNECTED", 9) == 0) {
			fprintf(stderr, "[modem] %s\n", line);
			break;
		}
		if (strcmp(line, "DISCONNECTED") == 0) {
			fprintf(stderr, "connect failed (no answer)\n");
			return 1;
		}
		fprintf(stderr, "[modem] %s\n", line);
	}

	/* Stream stdin with flow control. */
	uint8_t chunk[CHUNK];
	bool eof = false;
	while (!eof && !st.disconnected) {
		peek_at_peer(hc, &warned);

		int queued = query_buffer(hc, &st);
		if (queued < 0)
			break;
		if (queued >= TX_WINDOW) {
			ardop_sleep_ms(100);
			continue;
		}
		size_t want = TX_WINDOW - (size_t)queued;
		if (want > sizeof(chunk))
			want = sizeof(chunk);
		size_t got = fread(chunk, 1, want, stdin);
		if (got == 0) {
			if (ferror(stdin)) {
				fprintf(stderr, "stdin read error\n");
				break;
			}
			eof = true;
			break;
		}
		if (hc_send_data(hc, chunk, got) != 0) {
			fprintf(stderr, "send error\n");
			break;
		}
	}

	int rc = 1;
	if (eof && !st.disconnected) {
		/* Wait for the queue to drain and the last frame to be acked. */
		int queued;
		while ((queued = query_buffer(hc, &st)) > 0)
			ardop_sleep_ms(150);
		while (queued == 0 && !st.disconnected) {
			char sbuf[128];
			int r = hc_query(hc, "STATE", "STATE ", sbuf,
					 sizeof(sbuf), 5000, on_line, &st);
			if (r <= 0)
				break;
			if (strcmp(sbuf, "STATE IDLE") == 0)
				break;
			ardop_sleep_ms(200);
		}
		fprintf(stderr, "sent; disconnecting ...\n");
		hc_cmd(hc, "DISCONNECT");
		for (;;) {
			char line[512];
			int r = hc_next_line(hc, line, sizeof(line), 30000);
			if (r <= 0)
				break;
			fprintf(stderr, "[modem] %s\n", line);
			if (strcmp(line, "DISCONNECTED") == 0) {
				rc = 0;
				break;
			}
		}
	} else if (st.disconnected) {
		fprintf(stderr, "link dropped during transfer\n");
	}
	return rc;
}

static void usage(const char *me)
{
	fprintf(stderr,
		"usage: %s [--host HOST:PORT] TARGET   send stdin to TARGET\n"
		"       %s [--host HOST:PORT] --listen  receive to stdout\n"
		"\n"
		"a raw byte pipe over an ARQ link: no filename, no length, no\n"
		"checksum. for a checked transfer with a name on it, use\n"
		"ardop-station, which speaks the ASP protocol at both ends.\n",
		me, me);
}

int main(int argc, char **argv)
{
	/* Binary stdio before anything else: payload crosses stdin and stdout, and
	 * Windows text mode would rewrite every 0x0A and stop at 0x1A. */
	ardop_stdio_binary();

	const char *hostarg = NULL, *target = NULL;
	bool listen = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc)
			hostarg = argv[++i];
		else if (!strcmp(argv[i], "--listen") || !strcmp(argv[i], "-l"))
			listen = true;
		else if (argv[i][0] != '-' && !target)
			target = argv[i];
		else {
			usage(argv[0]);
			return 2;
		}
	}

	/* One direction per invocation, said plainly rather than by preferring
	 * one silently: a tool that quietly ignored the callsign and listened
	 * would be the worst of both. */
	if (listen == (target != NULL)) {
		fprintf(stderr, listen ? "give a TARGET or --listen, not both\n"
				       : "give a TARGET to dial, or --listen\n");
		usage(argv[0]);
		return 2;
	}

	char host[256];
	int port;
	parse_host(hostarg, host, sizeof(host), &port);

	hostclient hc;
	if (hc_open(&hc, host, port) != 0)
		return 1;

	const int rc = listen ? do_listen(&hc) : do_send(&hc, target);
	hc_close(&hc);
	return rc;
}
