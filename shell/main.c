#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/backend_alsa.h"
#include "shell/backend_null.h"
#include "shell/host_tcp.h"
#include "shell/loop.h"
#include "shell/platform.h"
#include "shell/runtime.h"
#include "shell/telemetry_tcp.h"

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
 *   ardopb MYCALL [--listen] [--id] [--trace] [--host PORT]
 *          [--null [SECONDS] | --alsa CAPTURE PLAYBACK [--ptt SERIAL]]
 *
 * --id sends one ID frame at startup (a beacon), which with --null is a
 * hardware-free smoke test of the entire TX chain. --host opens the TCP command
 * channel (host.h) so a host app can drive the station. --telemetry opens the
 * one-way DSP observation stream (telemetry.h) a display attaches to; it is off
 * unless asked for, so the headless default computes nothing extra.
 */

/* The RS parity lengths every supported frame type uses. */
static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

/* App context: routes the one observation stream to the backend / host / stdout. */
struct app {
	const ardop_platform_ops *ops;
	ardop_host_tcp *host;   /* NULL unless --host is given. */
	ardop_telemetry_tcp *tlm; /* NULL unless --telemetry is given. */
	bool trace;             /* --trace: print every observation. */
};

/* The single observer: performs the side-effects (PTT to the backend, host
 * messages and data to the TCP client) and, under --trace, logs everything. */
static void app_observe(void *ctx, const ardop_obs *o)
{
	struct app *a = ctx;

	switch (o->kind) {
	case ARDOP_OBS_PTT:
		if (a->ops->set_ptt)
			a->ops->set_ptt(a->ops->ctx, o->key);
		if (a->trace)
			fprintf(stderr, "[obs] ptt %s\n", o->key ? "key" : "unkey");
		break;
	case ARDOP_OBS_HOST_MSG:
		printf("[host] %s\n", o->text);
		fflush(stdout);
		ardop_host_tcp_notify(a->host, o->text);   /* no-op if unconnected. */
		break;
	case ARDOP_OBS_RX_DATA:
		printf("[data] %s %zu bytes\n", o->tag, o->data_len);
		fflush(stdout);
		ardop_host_tcp_send_data(a->host, o->tag, o->data, o->data_len);
		break;
	case ARDOP_OBS_STATE:
		if (a->trace)
			fprintf(stderr, "[obs] state=%d remote=%s\n", o->state,
				o->remote);
		break;
	case ARDOP_OBS_MODE:
		if (a->trace)
			fprintf(stderr, "[obs] mode=%d\n", o->mode);
		break;
	case ARDOP_OBS_BANDWIDTH:
		if (a->trace)
			fprintf(stderr, "[obs] bandwidth=%d\n", o->bandwidth);
		break;
	case ARDOP_OBS_RX_FRAME:
		if (a->trace)
			fprintf(stderr, "[obs] rx frame 0x%02x q=%d sn=%d\n",
				o->frame_type, o->quality, o->sn);
		break;
	case ARDOP_OBS_RX_FRAME_BAD:
		if (a->trace)
			fprintf(stderr, "[obs] rx frame 0x%02x FAILED\n",
				o->frame_type);
		break;
	case ARDOP_OBS_TX_FRAME:
		if (a->trace)
			fprintf(stderr, "[obs] tx frame 0x%02x\n", o->frame_type);
		break;
	case ARDOP_OBS_BUSY:
		if (a->trace)
			fprintf(stderr, "[obs] busy=%s\n", o->busy ? "yes" : "no");
		break;
	case ARDOP_OBS_BUFFER:
		if (a->trace)
			fprintf(stderr, "[obs] buffer=%zu\n", o->buffer_len);
		break;
	}
}

static void usage(const char *me)
{
	fprintf(stderr,
		"usage: %s MYCALL [--listen] [--id] [--trace] [--host PORT]\n"
		"       [--telemetry [PORT]]\n"
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

	bool listen = false, send_id = false, use_null = true, trace = false;
	uint64_t null_seconds = 5;
	const char *cap = NULL, *play = NULL, *ptt = NULL;
	uint16_t host_port = 0;
	uint16_t tlm_port = 0;
	bool want_tlm = false;

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--listen")) {
			listen = true;
		} else if (!strcmp(argv[i], "--id")) {
			send_id = true;
		} else if (!strcmp(argv[i], "--trace")) {
			trace = true;
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
		} else if (!strcmp(argv[i], "--host")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			host_port = (uint16_t)strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--telemetry")) {
			want_tlm = true;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				tlm_port = (uint16_t)strtoul(argv[++i], NULL,
							     10);
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
		nb.realtime = (host_port != 0);   /* live pacing for host use. */
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

	static struct app app;
	app.ops = &ops;
	app.trace = trace;
	if (host_port != 0) {
		app.host = ardop_host_tcp_open(host_port);
		if (!app.host) {
			fprintf(stderr, "host: failed to open port %u\n",
				(unsigned)host_port);
			return 1;
		}
	}
	/* Default the telemetry port to host_port + 2, clear of the command and
	 * data ports (PORT and PORT + 1). */
	if (want_tlm) {
		if (tlm_port == 0) {
			if (host_port == 0) {
				fprintf(stderr, "--telemetry needs a port when "
					"--host is not given\n");
				return 2;
			}
			tlm_port = (uint16_t)(host_port + 2);
		}
		app.tlm = ardop_telemetry_tcp_open(tlm_port);
		if (!app.tlm) {
			fprintf(stderr, "telemetry: failed to open port %u\n",
				(unsigned)tlm_port);
			return 1;
		}
		ardop_telemetry_tcp_attach(app.tlm, &rt);
	}
	ardop_runtime_observe(&rt, app_observe, &app);

	if (send_id) {
		ardop_host_cmd id = {0};
		id.kind = ARDOP_CMD_SEND_ID;
		ardop_runtime_host(&rt, &id, 0);
	}

	/* The driver loop, interleaved with servicing the host command channel
	 * (which runs ardop_host_command against the runtime between audio
	 * blocks). Equivalent to ardop_loop_run when there is no host server. */
	ardop_loop lp;
	ardop_loop_init(&lp, &rt, &ops);
	while (!(ops.should_stop && ops.should_stop(ops.ctx))) {
		if (app.host)
			ardop_host_tcp_service(app.host, &rt, lp.t);
		ardop_telemetry_tcp_service(app.tlm, &rt);
		ardop_loop_step(&lp);
	}

	if (app.host)
		ardop_host_tcp_close(app.host);
	ardop_telemetry_tcp_close(app.tlm);
	if (ab)
		ardop_backend_alsa_close(ab);
	fprintf(stderr, "stopped.\n");
	return 0;
}
