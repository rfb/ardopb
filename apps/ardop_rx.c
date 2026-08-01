#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell/net.h"
#include "shell/sys.h"

#include "hostclient.h"

/**
 * @file ardop_rx.c
 * @brief ardop-rx -- receive an ARQ data stream and write it to stdout.
 *
 * The receiving end of the pipe: connects to a listening modem, waits for an
 * incoming connection, and streams every decoded data payload to stdout until
 * the peer disconnects (which is end-of-stream).
 *
 *     ardop-rx --host 127.0.0.1:8515 > file
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

int main(int argc, char **argv)
{
	/* Binary stdio before anything else: payload is written to stdout, and Windows
	 * text mode would rewrite every 0x0A and stop at 0x1A. */
	ardop_stdio_binary();

	const char *hostarg = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc)
			hostarg = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--host HOST:PORT]\n", argv[0]);
			return 2;
		}
	}

	char host[256];
	int port;
	parse_host(hostarg, host, sizeof(host), &port);

	hostclient hc;
	if (hc_open(&hc, host, port) != 0)
		return 1;
	hc_cmd(&hc, "LISTEN TRUE");   /* ensure the modem answers. */

	fprintf(stderr, "waiting for a connection ...\n");
	bool connected = false;
	while (!connected) {
		char line[512];
		int r = hc_next_line(&hc, line, sizeof(line), -1);
		if (r <= 0) {
			fprintf(stderr, "lost the modem\n");
			hc_close(&hc);
			return 1;
		}
		fprintf(stderr, "[modem] %s\n", line);
		if (strncmp(line, "CONNECTED", 9) == 0)
			connected = true;
	}

	/* Stream data until the peer disconnects. */
	int rc = 0;
	for (;;) {
		const ardop_socket socks[2] = { hc.cmd_fd, hc.data_fd };
		bool ready[2] = { false, false };
		if (ardop_net_wait(socks, 2, -1, ready) < 0)
			break;

		if (ready[1]) {
			for (;;) {
				uint8_t payload[4096];
				char tag[HC_TAG_LEN + 1];
				int n = hc_recv_data(&hc, payload,
						     sizeof(payload), tag, 0);
				if (n == -2)
					break;            /* no full message. */
				if (n < 0) {
					rc = 1;
					goto done;
				}
				if (n > 0)
					fwrite(payload, 1, (size_t)n, stdout);
				fflush(stdout);
			}
		}
		if (ready[0]) {
			char line[512];
			int lr = hc_next_line(&hc, line, sizeof(line), 0);
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
	hc_close(&hc);
	return rc;
}
