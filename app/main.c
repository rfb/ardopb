#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/devices.h"
#include "app/loopback.h"
#include "app/script.h"
#include "app/spine.h"
#include "app/tnc_host_tcp.h"

#include "shell/audio_devices.h"
#include "shell/backend_ma.h"
#include "shell/backend_null.h"
#include "shell/fault.h"
#include "shell/host_tcp.h"
#include "shell/ptt.h"
#include "shell/settings.h"
#include "shell/sys.h"

/**
 * @file main.c
 * @brief ardop-spine: the station application's spine, with no user interface.
 *
 * [analysis/14](../analysis/14-station-application.md) workstream A. This is not
 * the application -- there is no window, no settings, no chat. It is the spine
 * the application will be built on, made runnable so that it can be exercised,
 * measured and sanitised before any of that exists.
 *
 * It is called `ardop-spine` and not `ardop-station` on purpose. The graphical
 * program will be a different binary linking the same objects, and taking the
 * name it will want would guarantee a confusing rename later. This one earns its
 * keep permanently anyway: it is how you drive a session with no display, on a
 * Pi or in continuous integration.
 *
 * Three ways to run it:
 *
 *   --loopback   two stations in memory, hearing each other. The only mode in
 *                which a whole connect/data/disconnect session can be shown
 *                without a radio, because the null backend returns silence and
 *                therefore has nobody to talk to.
 *   --null       one station, no device. For timers, host commands and the TNC
 *                interface.
 *   --audio      one station, a real sound card and real keying.
 */

static const char kUsage[] =
	"usage: ardop-spine --script FILE [options]\n"
	"\n"
	"  --script FILE        the command script; - reads standard input\n"
	"  --loopback           two in-memory stations that hear each other\n"
	"  --null [SECONDS]     no audio device (default 60 seconds)\n"
	"  --audio [CAP PLAY]   a real sound card, by device id or name\n"
	"  --audio-null-device  miniaudio's synthetic device, for tests\n"
	"  --audio-backend NAME alsa, pulseaudio, wasapi, coreaudio, jack\n"
	"  --ptt SPEC           none | rts:DEV | dtr:DEV | rigctld:HOST:PORT\n"
	"  --host PORT          host the TNC interface on PORT and PORT+1\n"
	"  --config PATH        load the saved device selection from PATH\n"
	"  --telemetry          compute spectrum and constellation telemetry\n"
	"  --list-devices       print the sound devices and exit\n";

enum backend_kind { BACKEND_LOOPBACK, BACKEND_NULL, BACKEND_MA };


int main(int argc, char **argv)
{
	const char *script_path = NULL;
	const char *config_path = NULL;
	const char *ptt_spec = NULL;
	const char *audio_backend = NULL;
	const char *cap = NULL, *play = NULL;
	enum backend_kind kind = BACKEND_LOOPBACK;
	unsigned null_seconds = 60;
	int host_port = 0;
	bool ma_null_device = false;
	bool telemetry = false;

	/* --list-devices before anything else, so it works with no script and no
	 * device open -- and honours --audio-backend wherever it appears. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--audio-backend") == 0 && i + 1 < argc)
			audio_backend = argv[i + 1];
		if (strcmp(argv[i], "--list-devices") == 0)
			return ardop_audio_print_devices(audio_backend) ? 0 : 1;
	}

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--script") == 0 && i + 1 < argc) {
			script_path = argv[++i];
		} else if (strcmp(a, "--loopback") == 0) {
			kind = BACKEND_LOOPBACK;
		} else if (strcmp(a, "--null") == 0) {
			kind = BACKEND_NULL;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				null_seconds = (unsigned)strtoul(argv[++i],
								 NULL, 10);
		} else if (strcmp(a, "--audio") == 0) {
			kind = BACKEND_MA;
			/* Both device selectors or neither: a capture device
			 * with a defaulted playback device is almost always a
			 * typo rather than an intention. */
			if (i + 2 < argc && argv[i + 1][0] != '-' &&
			    argv[i + 2][0] != '-') {
				cap = argv[++i];
				play = argv[++i];
			}
		} else if (strcmp(a, "--audio-null-device") == 0) {
			kind = BACKEND_MA;
			ma_null_device = true;
		} else if (strcmp(a, "--audio-backend") == 0 && i + 1 < argc) {
			audio_backend = argv[++i];
		} else if (strcmp(a, "--ptt") == 0 && i + 1 < argc) {
			ptt_spec = argv[++i];
		} else if (strcmp(a, "--host") == 0 && i + 1 < argc) {
			host_port = atoi(argv[++i]);
		} else if (strcmp(a, "--config") == 0 && i + 1 < argc) {
			config_path = argv[++i];
		} else if (strcmp(a, "--telemetry") == 0) {
			telemetry = true;
		} else {
			fputs(kUsage, stderr);
			return 2;
		}
	}

	if (!script_path) {
		fputs(kUsage, stderr);
		return 2;
	}

	/* Before any device is opened, so Ctrl-C can always unkey. */
	ardop_install_signal_handlers();

	app_config cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.telemetry = telemetry;

	app_loopback *lb = NULL;
	app_spine *sp = NULL, *peer = NULL;
	app_devices *dv = NULL;
	ardop_null_backend nb;
	ardop_platform_ops ops;
	int rc = 0;

	memset(&nb, 0, sizeof nb);
	memset(&ops, 0, sizeof ops);

	if (kind == BACKEND_LOOPBACK) {
		lb = app_loopback_open(&cfg);
		if (!lb) {
			fputs("ardop-spine: cannot build the loopback\n", stderr);
			return 1;
		}
		sp = app_loopback_side(lb, 0);
		peer = app_loopback_side(lb, 1);
	} else {
		sp = app_open(&cfg);
		if (!sp) {
			fputs("ardop-spine: cannot open the spine\n", stderr);
			return 1;
		}

		if (kind == BACKEND_NULL) {
			/* The device-free backend is bound directly: it has no
			 * device to select, nothing to enumerate and nothing to
			 * fault, so routing it through the manager would be
			 * ceremony. */
			ardop_backend_null_init(&nb, &ops,
						(uint64_t)null_seconds * 12000u);
			nb.realtime = host_port != 0;
			app_set_platform(sp, &ops, 0);
		} else {
			/* Everything with a real device goes through the manager,
			 * which owns the sound card and the keying line together
			 * and can rebuild both without restarting the program. */
			dv = app_devices_new();
			if (!dv) {
				fputs("ardop-spine: out of memory\n", stderr);
				app_close(sp);
				return 1;
			}

			app_device_selection sel;
			memset(&sel, 0, sizeof sel);

			/* A saved selection is the starting point; anything named
			 * on the command line overrides it, so --audio still works
			 * exactly as before and a configured station needs no
			 * arguments at all. */
			if (config_path && strcmp(config_path, "-") != 0) {
				ardop_settings st;
				if (ardop_settings_load(&st, config_path)) {
					app_devices_selection_load(&sel, &st);
					if (st.skipped)
						fprintf(stderr,
							"config: skipped %zu "
							"unreadable line(s)\n",
							st.skipped);
				}
			}

			if (cap)
				snprintf(sel.capture_id, sizeof sel.capture_id,
					 "%s", cap);
			if (play)
				snprintf(sel.playback_id, sizeof sel.playback_id,
					 "%s", play);
			if (audio_backend)
				snprintf(sel.backend, sizeof sel.backend, "%s",
					 audio_backend);
			if (ptt_spec)
				snprintf(sel.ptt_spec, sizeof sel.ptt_spec, "%s",
					 ptt_spec);
			if (ma_null_device)
				sel.null_device = true;

			if (!app_devices_request(dv, &sel)) {
				fputs("ardop-spine: cannot queue the device "
				      "request\n", stderr);
				app_devices_free(dv);
				app_close(sp);
				return 1;
			}
		}
	}

	/* The TNC interface. Bound after the spine exists and before the script
	 * runs, so a client that connects immediately owns the link from the
	 * first step. */
	ardop_host_tcp *host = NULL;
	app_tnc_ops tnc;
	if (host_port > 0) {
		host = ardop_host_tcp_open((uint16_t)host_port);
		if (!host) {
			fprintf(stderr, "ardop-spine: cannot listen on %d\n",
				host_port);
			rc = 1;
			goto out;
		}
		app_tnc_host_tcp_bind(&tnc, host);
		app_set_tnc(sp, &tnc);
		/* ardop_host_tcp_open already reported the ports. */
	}

	app_script *sc = app_script_open(script_path, sp, peer);
	if (!sc) {
		rc = 1;
		goto out;
	}

	while (!ardop_stop_requested()) {
		if (!app_script_pump(sc))
			break;

		if (lb) {
			app_loopback_step(lb);
		} else {
			if (dv)
				app_devices_service(dv, sp);
			app_step(sp);
		}

		/* Drain both queues on both sides every step. The event queue is
		 * lossless and nothing else empties it; the display queue is
		 * lossy, and @stress deliberately stops draining it. */
		app_event ev;
		while (app_events_pop(sp, &ev))
			app_script_event(sc, 0, &ev);
		if (peer)
			while (app_events_pop(peer, &ev))
				app_script_event(sc, 1, &ev);

		if (app_script_draining_display(sc)) {
			static ardop_tlm_decoded rec;   /* ~10 kB; not a frame */
			while (app_display_pop(sp, &rec))
				;
			if (peer)
				while (app_display_pop(peer, &rec))
					;
		}

		app_script_tick(sc, app_elapsed(sp));

		/* Device faults are detected here, where the backend's type is
		 * known, and reacted to in the spine, where a user interface can
		 * see the reaction. */
		/* Device faults are detected and reported by the manager, which
		 * owns both the backend and the keying line -- and polls both,
		 * with a fallthrough. A ternary here was a real defect: PTT is
		 * only ever opened alongside a real audio backend, so
		 * `mb ? ma_fault : ptt_fault` left the PTT latch unreachable in
		 * the one configuration where a PTT exists. */
		if (app_run_state_of(sp) == APP_RUN_FAULTED && rc == 0)
			rc = 1;

		if (!lb && ops.should_stop && ops.should_stop(ops.ctx))
			break;
	}

	if (app_script_failed(sc))
		rc = 1;
	app_script_close(sc);

out:
	/* Teardown order matters and is the same as ardopb's: the host server,
	 * then the spine (which unkeys), then the audio device, then PTT last --
	 * unkeying must outlive the device that keyed. */
	ardop_host_tcp_close(host);
	if (lb) {
		app_loopback_close(lb);
	} else {
		/* The manager closes in the one order that cannot leave a
		 * transmitter keyed: detach the spine, then the backend, then
		 * PTT. app_close unkeys through the bound platform, so it runs
		 * while the backend is still alive. */
		app_close(sp);
		if (dv) {
			app_devices_close(dv, NULL);
			app_devices_free(dv);
		}
	}

	fprintf(stderr, "ardop-spine: %s\n", rc ? "failed" : "done");
	return rc;
}
