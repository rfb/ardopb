#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "hostclient.h"

/**
 * @file ardop_chat.c
 * @brief ardop-chat -- a very basic line-oriented two-way chat.
 *
 * Each line you type is sent; each received line is printed as "peer> ...". Two
 * transports, chosen at launch:
 *   - ARQ (default): a reliable connection to one station. One side --call TARGET,
 *     the other --listen. Half-duplex: typing turns the link over (AUTOBREAK).
 *   - FEC (--fec): connectionless broadcast; anyone in FEC receive mode hears it.
 *     --fecmode picks the frame type to broadcast with.
 *
 *     ardop-chat --host 127.0.0.1:8515 --call N0CALL
 *     ardop-chat --host 127.0.0.1:8515 --listen
 *     ardop-chat --host 127.0.0.1:8515 --fec [--fecmode 4FSK.200.50S]
 */

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

/* Drain and print every complete data message currently buffered. */
static int print_incoming(hostclient *hc)
{
	for (;;) {
		uint8_t payload[4096];
		char tag[HC_TAG_LEN + 1];
		int n = hc_recv_data(hc, payload, sizeof(payload), tag, 0);
		if (n == -2)
			return 0;
		if (n < 0)
			return -1;
		printf("peer> %.*s", n, payload);
		if (n == 0 || payload[n - 1] != '\n')
			printf("\n");
		fflush(stdout);
	}
}

/* Wait for queued lines to go out before quitting: in ARQ before DISCONNECT
 * (which does not flush the queue), in FEC before dropping the socket. Settle
 * first so the just-sent bytes are registered (avoiding a BUFFER==0 read before
 * the modem has picked them up), then wait for the queue to empty and the link
 * to come to rest -- IDLE in ARQ (last frame acked), DISC in FEC (broadcast
 * complete). */
static void drain_buffer(hostclient *hc, const char *rest_state)
{
	char buf[128];
	usleep(500000);
	for (int i = 0; i < 300; i++) {
		if (hc_query(hc, "BUFFER", "BUFFER ", buf, sizeof(buf), 3000,
			     NULL, NULL) != 1)
			return;
		if (atoi(buf + strlen("BUFFER ")) == 0)
			break;
		usleep(200000);
	}
	for (int i = 0; i < 300; i++) {
		if (hc_query(hc, "STATE", "STATE ", buf, sizeof(buf), 3000,
			     NULL, NULL) != 1)
			return;
		if (strcmp(buf, rest_state) == 0)
			return;
		usleep(200000);
	}
}

int main(int argc, char **argv)
{
	const char *hostarg = NULL, *target = NULL;
	const char *fecmode = "4PSK.200.100";
	bool listen = false, fec = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc)
			hostarg = argv[++i];
		else if (!strcmp(argv[i], "--call") && i + 1 < argc)
			target = argv[++i];
		else if (!strcmp(argv[i], "--fecmode") && i + 1 < argc)
			fecmode = argv[++i];
		else if (!strcmp(argv[i], "--listen"))
			listen = true;
		else if (!strcmp(argv[i], "--fec"))
			fec = true;
		else {
			fprintf(stderr, "usage: %s [--host HOST:PORT] "
				"[--call TARGET | --listen | --fec [--fecmode MODE]]\n",
				argv[0]);
			return 2;
		}
	}

	char host[256];
	int port;
	parse_host(hostarg, host, sizeof(host), &port);

	hostclient hc;
	if (hc_open(&hc, host, port) != 0)
		return 1;

	bool connected = fec;   /* FEC needs no connection. */
	if (fec) {
		char cmd[128];
		hc_cmd(&hc, "PROTOCOLMODE FEC");
		snprintf(cmd, sizeof(cmd), "FECMODE %s", fecmode);
		hc_cmd(&hc, cmd);
		fprintf(stderr, "FEC broadcast chat (%s). "
			"Type lines; Ctrl-D to quit.\n", fecmode);
	} else if (target) {
		hc_cmd(&hc, "AUTOBREAK TRUE");
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "ARQCALL %s 5", target);
		hc_cmd(&hc, cmd);
		fprintf(stderr, "dialing %s ...\n", target);
	} else if (listen) {
		hc_cmd(&hc, "AUTOBREAK TRUE");
		hc_cmd(&hc, "LISTEN TRUE");
		fprintf(stderr, "waiting for a connection ...\n");
	} else {
		fprintf(stderr, "specify one of --call TARGET, --listen, or --fec\n");
		hc_close(&hc);
		return 2;
	}

	int maxfd = hc.cmd_fd;
	if (hc.data_fd > maxfd)
		maxfd = hc.data_fd;
	maxfd += 1;

	bool done = false;
	while (!done) {
		fd_set r;
		FD_ZERO(&r);
		/* Only read the keyboard once we can actually send -- otherwise a
		 * line typed (or piped) before the link is up would be dropped. */
		if (connected)
			FD_SET(STDIN_FILENO, &r);
		FD_SET(hc.cmd_fd, &r);
		FD_SET(hc.data_fd, &r);
		if (select(maxfd, &r, NULL, NULL, NULL) < 0)
			break;

		if (FD_ISSET(hc.data_fd, &r)) {
			if (print_incoming(&hc) < 0)
				break;
		}
		if (FD_ISSET(hc.cmd_fd, &r)) {
			char line[512];
			int lr = hc_next_line(&hc, line, sizeof(line), 0);
			if (lr < 0)
				break;
			if (lr == 1) {
				fprintf(stderr, "[modem] %s\n", line);
				if (strncmp(line, "CONNECTED", 9) == 0)
					connected = true;
				if (strcmp(line, "DISCONNECTED") == 0)
					break;
			}
		}
		if (connected && FD_ISSET(STDIN_FILENO, &r)) {
			char line[512];
			if (!fgets(line, sizeof(line), stdin)) {
				/* stdin EOF: hang up, letting queued lines drain
				 * first -- in ARQ because DISCONNECT does not
				 * flush, in FEC because closing the socket
				 * while a broadcast is still queued drops it. */
				if (fec) {
					drain_buffer(&hc, "STATE DISC");
				} else {
					drain_buffer(&hc, "STATE IDLE");
					hc_cmd(&hc, "DISCONNECT");
				}
				done = true;
				continue;
			}
			hc_send_data(&hc, (const uint8_t *)line, strlen(line));
			if (fec)
				hc_cmd(&hc, "FECSEND TRUE");
		}
	}

	hc_close(&hc);
	return 0;
}
