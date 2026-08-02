#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keyboard poll interval while waiting on the sockets. */
#define CHAT_POLL_MS 50

#include "shell/net.h"
#include "shell/sys.h"

#include "core/codec/frame.h"

#include "fecchat.h"
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
 *
 * ## The two transports are framed differently, and that is the specification
 *
 * ARQ chat sends bare lines. There is a session, so the peer is already known
 * and a callsign on every message would restate it; and unframed text is what
 * interoperates with a plain terminal, with `ardopcf`, and with the station
 * application, which degrades to raw mode for exactly this case
 * ([analysis/17](../analysis/17-application-protocol.md) §2).
 *
 * FEC chat cannot do that. There is no session and no peer -- §1: *"every
 * message must be self-contained and idempotent"* -- so §6's `TEXT_B` carries
 * the sender's callsign and a message id, and `apps/fecchat.c` is that profile.
 * Without it a broadcast net is a column of lines with no idea who said them.
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

/* Print one line, adding the newline the sender may not have sent. */
static void say(const char *who, const char *text, size_t len)
{
	printf("%s> %.*s", who, (int)len, text);
	if (len == 0 || text[len - 1] != '\n')
		printf("\n");
	fflush(stdout);
}

/*
 * Drain and print every complete data message currently buffered.
 *
 * The tag is read rather than ignored. §1: only the payload of the profile in
 * use is protocol, and an ERR marker or a station identification parsed as a
 * message is the defect ardop_rx.c had.
 */
static int print_incoming(hostclient *hc, fecchat *f, bool fec)
{
	for (;;) {
		uint8_t payload[4096];
		char tag[HC_TAG_LEN + 1];
		int n = hc_recv_data(hc, payload, sizeof(payload), tag, 0);
		if (n == -2)
			return 0;
		if (n < 0)
			return -1;
		if (n == 0)
			continue;

		const char *want = fec ? "FEC" : "ARQ";
		if (strcmp(tag, want) != 0) {
			fprintf(stderr, "[%s] %.*s\n", tag, n, payload);
			continue;
		}

		if (!fec) {
			say("peer", (const char *)payload, (size_t)n);
			continue;
		}

		char call[ASP_MAX_CALL];
		uint16_t id;
		const char *text;
		size_t text_len;
		if (fecchat_decode(payload, (size_t)n, call, sizeof call, &id,
				   &text, &text_len) != FECCHAT_MESSAGE) {
			/* §6: shown, not discarded. A station broadcasting bare
			 * lines is the commonest thing on the channel. */
			say("?", text, text_len);
			continue;
		}
		if (!fecchat_is_new(f, call, id, ardop_mono_ms()))
			continue;   /* a FECREPEATS copy; already shown. */
		say(call, text, text_len);
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
	ardop_sleep_ms(500);
	for (int i = 0; i < 300; i++) {
		if (hc_query(hc, "BUFFER", "BUFFER ", buf, sizeof(buf), 3000,
			     NULL, NULL) != 1)
			return;
		if (atoi(buf + strlen("BUFFER ")) == 0)
			break;
		ardop_sleep_ms(200);
	}
	for (int i = 0; i < 300; i++) {
		if (hc_query(hc, "STATE", "STATE ", buf, sizeof(buf), 3000,
			     NULL, NULL) != 1)
			return;
		if (strcmp(buf, rest_state) == 0)
			return;
		ardop_sleep_ms(200);
	}
}

/* The frame type a FECMODE name refers to, or 0xff. The names are core's, so
 * this asks core rather than keeping a table that could disagree with it. */
static uint8_t frame_type_for(const char *name)
{
	for (int t = 0; t < 256; t++) {
		char n[32];
		if (!ardop_data_frame_name((uint8_t)t, n, sizeof n))
			continue;
		if (strcmp(n, name) == 0)
			return (uint8_t)t;
	}
	return 0xff;
}

/*
 * Our callsign, from the modem rather than from a new flag.
 *
 * The station is already configured with one and a second place to say it is a
 * second place for it to be wrong. TEXT_B carries it, so a broadcast cannot go
 * out without it -- which is also what §9 wants of anything transmitting.
 */
static bool my_callsign(hostclient *hc, char *out, size_t cap)
{
	char buf[128];
	if (hc_query(hc, "MYCALL", "MYCALL ", buf, sizeof buf, 5000, NULL,
		     NULL) != 1)
		return false;
	const char *call = buf + strlen("MYCALL ");
	while (*call == ' ')
		call++;
	if (*call == '\0')
		return false;
	snprintf(out, cap, "%s", call);
	return true;
}

/*
 * Send one typed line as one or more complete TEXT_B messages.
 *
 * Split rather than truncated, and split into *whole* messages rather than
 * fragments: §1 requires every FEC message to be self-contained, so a sentence
 * too long for one frame becomes two messages with their own ids, each of which
 * stands alone if the other is lost. Broken at a space where there is one, so
 * the split lands between words rather than inside one.
 */
static int send_fec_line(hostclient *hc, fecchat *f, const char *call,
			 const char *line, size_t len, size_t capacity)
{
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		len--;
	if (len == 0 || capacity == 0)
		return 0;

	size_t sent = 0;
	while (sent < len) {
		size_t take = len - sent;
		if (take > capacity) {
			take = capacity;
			size_t brk = take;
			while (brk > 0 && line[sent + brk] != ' ')
				brk--;
			if (brk > capacity / 4)   /* not a pathological split */
				take = brk;
		}

		uint8_t msg[ASP_MAX_MESSAGE];
		const size_t n = fecchat_encode(f, call, line + sent, take, msg,
						sizeof msg);
		if (n == 0)
			return -1;
		if (hc_send_data(hc, msg, n) != 0)
			return -1;
		sent += take;
		while (sent < len && line[sent] == ' ')
			sent++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	fecchat fecstate;
	fecchat_init(&fecstate);
	char mycall[ASP_MAX_CALL] = {0};
	size_t fec_capacity = 0;

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

		if (!my_callsign(&hc, mycall, sizeof mycall)) {
			fprintf(stderr,
				"this station has no callsign set, and a broadcast "
				"carries one.\nset it on the modem (MYCALL) and "
				"try again.\n");
			hc_close(&hc);
			return 2;
		}

		const uint8_t type = frame_type_for(fecmode);
		if (type != 0xff)
			fec_capacity = fecchat_text_capacity(type, mycall);
		if (fec_capacity == 0) {
			fprintf(stderr,
				"%s cannot carry a message from %s: the frame "
				"holds fewer bytes than the callsign and message "
				"id need.\ntry a wider or faster --fecmode.\n",
				fecmode, mycall);
			hc_close(&hc);
			return 2;
		}

		/*
		 * The capacity is printed because it is small and surprising:
		 * the most robust mode carries sixteen bytes a frame, of which
		 * §6's header wants ten. An operator told "six characters"
		 * chooses a different mode; one who is not told finds out by
		 * having a sentence split ten ways.
		 */
		fprintf(stderr,
			"FEC broadcast chat as %s (%s, %zu characters per "
			"message; longer lines are split).\n"
			"Type lines; Ctrl-D to quit.\n",
			mycall, fecmode, fec_capacity);
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

	/*
	 * The sockets and the keyboard are waited on separately rather than in
	 * one select(). Winsock's select() takes sockets only -- a console
	 * handle, a pipe and a file are all invalid there -- so the portable
	 * shape is a short socket wait plus a non-blocking stdin poll. The cost
	 * is up to CHAT_POLL_MS of typing latency on a link whose round trip is
	 * measured in seconds.
	 */
	bool done = false;
	while (!done) {
		const ardop_socket socks[2] = { hc.cmd_fd, hc.data_fd };
		bool ready[2] = { false, false };
		if (ardop_net_wait(socks, 2, CHAT_POLL_MS, ready) < 0)
			break;

		if (ready[1]) {
			if (print_incoming(&hc, &fecstate, fec) < 0)
				break;
		}
		if (ready[0]) {
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
		/* Only read the keyboard once we can actually send -- otherwise
		 * a line typed (or piped) before the link is up would be
		 * dropped. */
		if (connected && ardop_stdin_ready(0)) {
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
			if (fec) {
				if (send_fec_line(&hc, &fecstate, mycall, line,
						  strlen(line),
						  fec_capacity) == 0)
					hc_cmd(&hc, "FECSEND TRUE");
				else
					fprintf(stderr,
						"[that line could not be sent]\n");
			} else {
				hc_send_data(&hc, (const uint8_t *)line,
					     strlen(line));
			}
		}
	}

	hc_close(&hc);
	return 0;
}
