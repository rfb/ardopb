#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/backend_alsa.h"
#include "shell/backend_null.h"
#include "shell/loop.h"
#include "shell/platform.h"
#include "shell/runtime.h"

#include "codec/stationid.h"
#include "link/link.h"

/**
 * @file main.c
 * @brief The assembled program: the pure core, a platform backend, one loop.
 *
 * This is the whole thing tied together and made runnable -- the point of the
 * rebuild (analysis/13 W2). It is intentionally small: parse a little config,
 * build a backend, wire the runtime's callbacks to it, and run the driver loop.
 * Everything of consequence -- the modem, the protocol, the timing -- is in the
 * core and is covered by the golden corpus and the in-process tests; this file
 * only chooses a backend and turns the crank.
 *
 * Usage:
 *   ardop2 MYCALL [--listen] [--id]
 *          [--null [SECONDS] | --alsa CAPTURE PLAYBACK [--ptt SERIAL]]
 *
 * --id sends one ID frame at startup (a beacon), which with --null is a
 * hardware-free smoke test of the entire TX chain.
 */

/* The RS parity lengths every supported frame type uses. */
static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

/* App context: routes the runtime's callbacks to the chosen backend / stdout. */
struct app {
	const ardop_platform_ops *ops;
};

static void on_ptt(void *ctx, bool key)
{
	const struct app *a = ctx;
	if (a->ops->set_ptt)
		a->ops->set_ptt(a->ops->ctx, key);
}
static void on_host(void *ctx, const char *msg)
{
	(void)ctx;
	printf("[host] %s\n", msg);
	fflush(stdout);
}
static void on_data(void *ctx, const uint8_t *d, size_t n)
{
	(void)ctx;
	printf("[data] %zu bytes:", n);
	for (size_t i = 0; i < n && i < 32; i++)
		printf(" %02x", d[i]);
	printf(n > 32 ? " ...\n" : "\n");
	fflush(stdout);
}

static void usage(const char *me)
{
	fprintf(stderr,
		"usage: %s MYCALL [--listen] [--id]\n"
		"       [--null [SECONDS] | --alsa CAPTURE PLAYBACK [--ptt SERIAL]]\n",
		me);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	const char *mycall = argv[1];

	bool listen = false, send_id = false, use_null = true;
	uint64_t null_seconds = 5;
	const char *cap = NULL, *play = NULL, *ptt = NULL;

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--listen")) {
			listen = true;
		} else if (!strcmp(argv[i], "--id")) {
			send_id = true;
		} else if (!strcmp(argv[i], "--null")) {
			use_null = true;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				null_seconds = (uint64_t)strtoul(argv[++i],
								 NULL, 10);
		} else if (!strcmp(argv[i], "--alsa")) {
			if (i + 2 >= argc) {
				usage(argv[0]);
				return 2;
			}
			use_null = false;
			cap = argv[++i];
			play = argv[++i];
		} else if (!strcmp(argv[i], "--ptt")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			ptt = argv[++i];
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	static ardop_runtime rt;
	if (!ardop_runtime_init(&rt, kRSLens, NUM_RSLENS)) {
		fprintf(stderr, "runtime init failed\n");
		return 1;
	}
	if (ardop_stationid_from_str(mycall, &rt.link.mycall)
	    != ARDOP_STATIONID_OK) {
		fprintf(stderr, "invalid callsign: %s\n", mycall);
		return 1;
	}
	rt.link.bw_setting = ARDOP_ARQ_BW_2000_MAX;
	rt.link.listening = listen;

	/* Build the backend and its ops. */
	ardop_platform_ops ops;
	ardop_null_backend nb;
	ardop_alsa_backend *ab = NULL;
	if (use_null) {
		ardop_backend_null_init(&nb, &ops,
					null_seconds * ARDOP_MOD_SAMPLE_RATE);
		nb.verbose = true;
		fprintf(stderr, "backend: null (%llu s)\n",
			(unsigned long long)null_seconds);
	} else {
		ab = ardop_backend_alsa_open(cap, play, ptt, &ops);
		if (!ab) {
			fprintf(stderr, "alsa backend failed to open\n");
			return 1;
		}
		fprintf(stderr, "backend: alsa cap=%s play=%s ptt=%s\n",
			cap, play, ptt ? ptt : "(none)");
	}

	struct app app = {.ops = &ops};
	rt.ctx = &app;
	rt.on_ptt = on_ptt;
	rt.on_host = on_host;
	rt.on_data = on_data;

	if (send_id) {
		ardop_host_cmd id = {0};
		id.kind = ARDOP_CMD_SEND_ID;
		ardop_runtime_host(&rt, &id, 0);
	}

	ardop_loop lp;
	ardop_loop_init(&lp, &rt, &ops);
	ardop_loop_run(&lp);

	if (ab)
		ardop_backend_alsa_close(ab);
	fprintf(stderr, "stopped.\n");
	return 0;
}
