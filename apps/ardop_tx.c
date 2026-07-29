#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hostclient.h"

/**
 * @file ardop_tx.c
 * @brief ardop-tx -- send stdin to a target over an ARQ connection.
 *
 * A "pipe over the radio": connects to a running modem's host interface, dials
 * TARGET, streams stdin as reliable ARQ data (with flow control so the modem's
 * finite send buffer never overflows), waits for it all to drain and be acked,
 * then disconnects. The peer runs ardop-rx.
 *
 *     cat file | ardop-tx --host 127.0.0.1:8515 N0CALL
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

int main(int argc, char **argv)
{
	const char *hostarg = NULL, *target = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc)
			hostarg = argv[++i];
		else if (argv[i][0] != '-' && !target)
			target = argv[i];
		else {
			fprintf(stderr, "usage: %s [--host HOST:PORT] TARGET\n",
				argv[0]);
			return 2;
		}
	}
	if (!target) {
		fprintf(stderr, "usage: %s [--host HOST:PORT] TARGET\n", argv[0]);
		return 2;
	}

	char host[256];
	int port;
	parse_host(hostarg, host, sizeof(host), &port);

	hostclient hc;
	if (hc_open(&hc, host, port) != 0)
		return 1;
	struct state st = {0};

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "ARQCALL %s 5", target);
	fprintf(stderr, "dialing %s ...\n", target);
	hc_cmd(&hc, cmd);

	/* Wait for the connection to come up (or fail). */
	bool connected = false;
	for (;;) {
		char line[512];
		int r = hc_next_line(&hc, line, sizeof(line), 60000);
		if (r <= 0) {
			fprintf(stderr, "no response from modem\n");
			hc_close(&hc);
			return 1;
		}
		if (strncmp(line, "CONNECTED", 9) == 0) {
			fprintf(stderr, "[modem] %s\n", line);
			connected = true;
			break;
		}
		if (strcmp(line, "DISCONNECTED") == 0) {
			fprintf(stderr, "connect failed (no answer)\n");
			hc_close(&hc);
			return 1;
		}
		fprintf(stderr, "[modem] %s\n", line);
	}
	(void)connected;

	/* Stream stdin with flow control. */
	uint8_t chunk[CHUNK];
	bool eof = false;
	while (!eof && !st.disconnected) {
		int queued = query_buffer(&hc, &st);
		if (queued < 0)
			break;
		if (queued >= TX_WINDOW) {
			usleep(100000);
			continue;
		}
		size_t want = TX_WINDOW - (size_t)queued;
		if (want > sizeof(chunk))
			want = sizeof(chunk);
		ssize_t got = read(STDIN_FILENO, chunk, want);
		if (got < 0) {
			fprintf(stderr, "stdin read error\n");
			break;
		}
		if (got == 0) {
			eof = true;
			break;
		}
		if (hc_send_data(&hc, chunk, (size_t)got) != 0) {
			fprintf(stderr, "send error\n");
			break;
		}
	}

	int rc = 1;
	if (eof && !st.disconnected) {
		/* Wait for the queue to drain and the last frame to be acked. */
		int queued;
		while ((queued = query_buffer(&hc, &st)) > 0)
			usleep(150000);
		while (queued == 0 && !st.disconnected) {
			char sbuf[128];
			int r = hc_query(&hc, "STATE", "STATE ", sbuf,
					 sizeof(sbuf), 5000, on_line, &st);
			if (r <= 0)
				break;
			if (strcmp(sbuf, "STATE IDLE") == 0)
				break;
			usleep(200000);
		}
		fprintf(stderr, "sent; disconnecting ...\n");
		hc_cmd(&hc, "DISCONNECT");
		for (;;) {
			char line[512];
			int r = hc_next_line(&hc, line, sizeof(line), 30000);
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

	hc_close(&hc);
	return rc;
}
