#define _DEFAULT_SOURCE /* localtime_r, for --log's timestamped filenames. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shell/audio_devices.h"
#include "shell/backend_ma.h"
#include "shell/backend_null.h"
#include "shell/host_tcp.h"
#include "shell/loop.h"
#include "shell/platform.h"
#include "shell/capture.h"
#include "shell/runtime.h"
#include "shell/wavwriter.h"
#include "shell/sys.h"
#include "shell/ptt.h"
#include "shell/build.h"
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
 *          [--null [SECONDS] | --audio [CAPTURE PLAYBACK]]
 *          [--audio-backend NAME] [--ptt SPEC] [--list-devices]
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
	ardop_capture *capture; /* NULL unless --capture is given. */
	ardop_wav_writer *rec;  /* NULL unless --record is given. */
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
	case ARDOP_OBS_LEADER:
		/* Not behind --trace: this is the one line that tells an
		 * operator tuning a station whether anything was heard at all,
		 * and which way the dial is wrong. */
		fprintf(stderr, "leader: %+.0f Hz, S/N %d dB\n",
			(double)o->offset_hz, o->sn);
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
		"       [--telemetry [PORT]] [--capture FILE] [--record FILE]\n"
		"       [--log DIR]\n"
		"       [--null [SECONDS] | --audio [CAPTURE PLAYBACK]]\n"
		"       [--audio-backend NAME] [--ptt SPEC] [--list-devices]\n"
		"\n"
		"  --audio          the sound card. With no arguments, the\n"
		"                   system defaults; a device may be given by\n"
		"                   id or by name (see --list-devices).\n"
		"  --audio-backend  force one audio API: alsa, pulseaudio,\n"
		"                   jack, wasapi, coreaudio ... Auto-selected\n"
		"                   otherwise; use alsa to bypass PulseAudio.\n"
		"  --ptt SPEC       none | rts:DEV | dtr:DEV | rigctld:HOST:PORT\n"
		"                   (a bare device path means rts:).\n"
		"  --capture FILE   write every frame, and the session's\n"
		"                   narrative around them, to a .pcap file for\n"
		"                   offline review (apps/ardop-pcap-dump).\n"
		"  --record FILE    write raw RX audio (12 kHz mono WAV, the\n"
		"                   modem's own rate) for replay through\n"
		"                   --decodewav-style tools while trying a\n"
		"                   demodulator change.\n"
		"  --log DIR        write both of the above at once, under one\n"
		"                   timestamp (DIR/session-<ts>.pcap and .wav),\n"
		"                   so they can never end up out of step with\n"
		"                   each other. DIR is created if missing.\n"
		"                   Mutually exclusive with --capture/--record.\n"
		"  --list-devices   print the audio devices and exit.\n"
		"  --version        print the build identifier and exit.\n",
		me);
}

/* Which backend --null / --audio selected. */
enum backend_kind { BACKEND_NULL, BACKEND_MA };

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version")) {
			char line[160];
			printf("%s\n",
			       ardop_build_line("ardopb", line, sizeof line));
			return 0;
		}
		if (!strcmp(argv[i], "--list-devices")) {
			const char *be = NULL;
			for (int j = 1; j < argc - 1; j++)
				if (!strcmp(argv[j], "--audio-backend"))
					be = argv[j + 1];
			return ardop_audio_print_devices(be) ? 0 : 1;
		}
	}

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	const char *mycall = argv[1];

	bool listen = false, send_id = false, trace = false;
	enum backend_kind kind = BACKEND_NULL;
	uint64_t null_seconds = 5;
	const char *cap = NULL, *play = NULL, *ptt_spec = NULL;
	const char *capture_path = NULL;
	const char *record_path = NULL;
	const char *log_dir = NULL;
	uint16_t host_port = 0;
	uint16_t tlm_port = 0;
	bool want_tlm = false;
	bool ma_null_device = false;
	const char *audio_backend = NULL;

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--listen")) {
			listen = true;
		} else if (!strcmp(argv[i], "--id")) {
			send_id = true;
		} else if (!strcmp(argv[i], "--trace")) {
			trace = true;
		} else if (!strcmp(argv[i], "--null")) {
			kind = BACKEND_NULL;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				null_seconds = (uint64_t)strtoul(argv[++i],
								 NULL, 10);
		} else if (!strcmp(argv[i], "--audio")) {
			kind = BACKEND_MA;
			/* Both device selectors or neither. */
			if (i + 2 < argc && argv[i + 1][0] != '-'
			    && argv[i + 2][0] != '-') {
				cap = argv[++i];
				play = argv[++i];
			}
		} else if (!strcmp(argv[i], "--audio-null-device")) {
			/* Test hook: miniaudio's synthetic device, so the whole
			 * ring / resampler / drain path runs in CI. */
			kind = BACKEND_MA;
			ma_null_device = true;
		} else if (!strcmp(argv[i], "--audio-backend")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			audio_backend = argv[++i];
		} else if (!strcmp(argv[i], "--ptt")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			ptt_spec = argv[++i];
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
		} else if (!strcmp(argv[i], "--capture")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			capture_path = argv[++i];
		} else if (!strcmp(argv[i], "--record")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			record_path = argv[++i];
		} else if (!strcmp(argv[i], "--log")) {
			if (i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}
			log_dir = argv[++i];
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	/* --log DIR is sugar for --capture/--record sharing one timestamp, so
	 * the two files can never drift apart the way a hand-picked pair of
	 * names can (a session recorded under one callsign's filename that
	 * turns out, on inspection, to hold a different one). Built here, once
	 * argv is fully parsed, so the ordinary --capture/--record paths below
	 * need no special-casing. */
	char log_capture_path[600], log_record_path[600];
	if (log_dir) {
		if (capture_path || record_path) {
			fprintf(stderr,
				"--log cannot be combined with --capture or "
				"--record\n");
			return 2;
		}
		if (!ardop_mkdir_p(log_dir)) {
			fprintf(stderr, "--log: cannot create %s\n", log_dir);
			return 1;
		}
		time_t t = (time_t)(ardop_wall_ms() / 1000u);
		struct tm tmv;
		localtime_r(&t, &tmv);
		char stamp[32];
		strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);

		snprintf(log_capture_path, sizeof log_capture_path,
			 "%s/session-%s.pcap", log_dir, stamp);
		snprintf(log_record_path, sizeof log_record_path,
			 "%s/session-%s.wav", log_dir, stamp);
		capture_path = log_capture_path;
		record_path = log_record_path;
	}

	/* Before any device is opened: an interrupt must be able to unkey. PTT
	 * over a CAT link does not fall when the process dies -- the rig never
	 * learns the controller is gone -- so leaving on Ctrl-C would leave a
	 * transmitter keyed. */
	ardop_install_signal_handlers();

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

	/* PTT is built first and independently: the keying method and the audio
	 * device are unrelated in reality, so an operator can pair any
	 * interface with any radio. */
	ardop_ptt *ptt = NULL;
	if (kind != BACKEND_NULL) {
		ardop_ptt_config pc;
		if (!ardop_ptt_parse(ptt_spec, &pc))
			return 2;
		ptt = ardop_ptt_open(&pc);
		if (!ptt) {
			fprintf(stderr, "ptt: failed to open\n");
			return 1;
		}
	}

	/* Build the backend and its ops. */
	ardop_platform_ops ops;
	ardop_null_backend nb;
	ardop_ma_backend *mb = NULL;
	switch (kind) {
	case BACKEND_NULL:
		ardop_backend_null_init(&nb, &ops,
					null_seconds * ARDOP_MOD_SAMPLE_RATE);
		nb.verbose = true;
		nb.realtime = (host_port != 0);   /* live pacing for host use. */
		fprintf(stderr, "backend: null (%llu s)\n",
			(unsigned long long)null_seconds);
		break;

	case BACKEND_MA: {
		ardop_ma_config mc = {
			.capture_id = cap,
			.playback_id = play,
			.ptt = ptt,
			.use_null_device = ma_null_device,
			.backend_name = audio_backend,
		};
		mb = ardop_backend_ma_open(&mc, &ops);
		if (!mb) {
			ardop_ptt_close(ptt);
			return 1;
		}
		break;
	}
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
	if (capture_path) {
		app.capture = ardop_capture_open(capture_path);
		if (!app.capture) {
			fprintf(stderr, "capture: cannot open %s\n",
				capture_path);
			return 1;
		}
		ardop_runtime_set_capture(&rt, app.capture);
	}
	if (record_path) {
		app.rec = ardop_wav_writer_open(record_path);
		if (!app.rec) {
			fprintf(stderr, "record: cannot open %s\n", record_path);
			return 1;
		}
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
	lp.rec = app.rec;
	if (mb)
		lp.block = ardop_backend_ma_block(mb);

	while (!ardop_stop_requested()
	       && !(ops.should_stop && ops.should_stop(ops.ctx))) {
		if (app.host)
			ardop_host_tcp_service(app.host, &rt, lp.t);
		ardop_telemetry_tcp_service(app.tlm, &rt);
		ardop_loop_step(&lp);

		/*
		 * Device faults are polled here rather than signalled through
		 * ardop_platform_ops, because the reaction is policy and
		 * loop.c must hold none. A modem that believes it is
		 * transmitting into a device that has gone away must not keep
		 * feeding the link.
		 */
		ardop_fault f = mb ? ardop_backend_ma_fault(mb)
				   : ARDOP_FAULT_NONE;
		if (f == ARDOP_FAULT_NONE)
			f = ardop_ptt_fault(ptt);
		if (f != ARDOP_FAULT_NONE) {
			fprintf(stderr, "\nfault: %s -- aborting the session "
				"and unkeying\n", ardop_fault_str(f));
			ardop_host_cmd disc = {.kind = ARDOP_CMD_DISCONNECT};
			ardop_runtime_host(&rt, &disc, lp.t);
			if (ops.set_ptt)
				ops.set_ptt(ops.ctx, false);
			ardop_host_tcp_notify(app.host, "FAULT device lost");
			break;
		}
	}

	if (app.host)
		ardop_host_tcp_close(app.host);
	ardop_telemetry_tcp_close(app.tlm);
	ardop_capture_close(app.capture);
	ardop_wav_writer_close(app.rec);
	if (mb)
		ardop_backend_ma_close(mb);
	/* After the backends: unkeying must outlive the device that keyed. */
	ardop_ptt_close(ptt);
	fprintf(stderr, "stopped.\n");
	return 0;
}
