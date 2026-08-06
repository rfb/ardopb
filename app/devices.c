/* localtime_r for the capture filename's timestamp. */
#define _DEFAULT_SOURCE

#include "app/devices.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/ring.h"
#include "shell/backend_ma.h"
#include "shell/sys.h"

/**
 * @file devices.c
 * @brief The device manager (see devices.h).
 */

/* Room for a burst of clicks down a device list. A request carries a whole
 * selection -- two device ids and two names -- so this is sized from the record
 * rather than guessed; at 4 kB it held three, and the fourth click was refused. */
#define REQUEST_SLOTS 8

/* How long to sleep per service call when nothing is bound. With no platform
 * app_step returns instantly, so without this a caller's loop spins. */
#define IDLE_SLEEP_MS 20

typedef enum {
	REQ_OPEN = 1,
	REQ_CLOSE,
	REQ_PTT_TEST,
} req_kind;

typedef struct {
	uint8_t kind;
	unsigned ms;                 /* REQ_PTT_TEST */
	app_device_selection sel;    /* REQ_OPEN */
} request;

struct app_devices {
	app_ring reqq;
	uint8_t req_buf[REQUEST_SLOTS * (sizeof(request) + APP_RING_OVERHEAD)];

	/* Modem-thread state. */
	ardop_ma_backend *mb;
	ardop_ptt *ptt;
	/* Named pcap, not cap: open_devices() already has a local ardop_audio_device
	 * cap for the audio *capture* device, and this is the unrelated session log
	 * (shell/capture.h). NULL unless enabled. */
	ardop_capture *pcap;
	ardop_platform_ops ops;
	app_device_selection requested;
	app_device_selection in_use;
	ardop_audio_match cap_match, play_match;
	char detail[APP_TEXT_MAX];
	char ptt_describe[64];
	bool pcap_open_failed;   /* Reported as APP_DEV_EV_PCAP_FAILED, not
				  * SUBSTITUTED -- a different situation from a
				  * device swap, even though both share dv->detail
				  * as their text. */

	/* Published for any thread. */
	_Atomic int state;           /* app_dev_state */
	_Atomic unsigned generation; /* bumped on every applied change */
};

/* --- any thread ------------------------------------------------------------ */

app_devices *app_devices_new(void)
{
	app_devices *dv = calloc(1, sizeof *dv);
	if (!dv)
		return NULL;
	if (!app_ring_init(&dv->reqq, dv->req_buf, sizeof dv->req_buf)) {
		free(dv);
		return NULL;
	}
	atomic_store_explicit(&dv->state, (int)APP_DEV_CLOSED,
			      memory_order_relaxed);
	return dv;
}

void app_devices_free(app_devices *dv)
{
	free(dv);
}

static bool push(app_devices *dv, const request *r)
{
	return app_ring_write(&dv->reqq, r, sizeof *r, NULL, 0, 0);
}

bool app_devices_request(app_devices *dv, const app_device_selection *sel)
{
	request r;
	memset(&r, 0, sizeof r);
	r.kind = REQ_OPEN;
	r.sel = *sel;
	return push(dv, &r);
}

bool app_devices_request_close(app_devices *dv)
{
	request r;
	memset(&r, 0, sizeof r);
	r.kind = REQ_CLOSE;
	return push(dv, &r);
}

bool app_devices_request_ptt_test(app_devices *dv, unsigned ms)
{
	request r;
	memset(&r, 0, sizeof r);
	r.kind = REQ_PTT_TEST;
	r.ms = ms;
	return push(dv, &r);
}

app_dev_state app_devices_state(const app_devices *dv)
{
	app_devices *m = (app_devices *)(uintptr_t)dv;
	return (app_dev_state)atomic_load_explicit(&m->state,
						   memory_order_acquire);
}

void app_devices_status(app_devices *dv, app_device_status *out)
{
	memset(out, 0, sizeof *out);
	out->state = app_devices_state(dv);
	out->requested = dv->requested;
	out->in_use = dv->in_use;
	out->capture_match = dv->cap_match;
	out->playback_match = dv->play_match;
	out->ratio = ardop_backend_ma_ratio(dv->mb);
	out->block = dv->mb ? ardop_backend_ma_block(dv->mb) : 0;
	out->device_rate = dv->mb ? out->ratio * 12000u : 0;
	snprintf(out->ptt_describe, sizeof out->ptt_describe, "%s",
		 dv->ptt_describe);
	snprintf(out->detail, sizeof out->detail, "%s", dv->detail);
}

/* --- modem thread: mapping and teardown ------------------------------------ */

/* Translate a device fault into the spine's own vocabulary. The spine owns no
 * devices and does not include shell/fault.h; this is where the two meet. */
static app_fault fault_of(ardop_fault f)
{
	switch (f) {
	case ARDOP_FAULT_CAPTURE_LOST:      return APP_FAULT_CAPTURE;
	case ARDOP_FAULT_PLAYBACK_LOST:
	case ARDOP_FAULT_PLAYBACK_UNDERRUN: return APP_FAULT_PLAYBACK;
	case ARDOP_FAULT_PTT_LOST:          return APP_FAULT_PTT;
	case ARDOP_FAULT_NONE:              break;
	}
	return APP_FAULT_OTHER;
}

static void set_state(app_devices *dv, app_dev_state st)
{
	atomic_store_explicit(&dv->state, (int)st, memory_order_release);
}

/*
 * Tear down, in the one order that cannot leave a transmitter keyed.
 *
 * The spine is detached first: unkeying goes out through the bound platform, so
 * a backend closed while still bound would be keyed into after it was freed.
 * PTT goes last, because unkeying has to outlive the device that keyed.
 */
static void teardown(app_devices *dv, app_spine *sp)
{
	if (sp) {
		(void)app_set_platform(sp, NULL, 0);
		app_set_capture(sp, NULL);
	}
	ardop_capture_close(dv->pcap);
	dv->pcap = NULL;

	ardop_backend_ma_close(dv->mb);
	dv->mb = NULL;

	ardop_ptt_close(dv->ptt);
	dv->ptt = NULL;

	memset(&dv->ops, 0, sizeof dv->ops);
	dv->ptt_describe[0] = '\0';
	dv->cap_match = ARDOP_AUDIO_MATCH_NONE;
	dv->play_match = ARDOP_AUDIO_MATCH_NONE;
	memset(&dv->in_use, 0, sizeof dv->in_use);
}

/* --- modem thread: opening -------------------------------------------------- */

/* Compose the sentence an operator reads when they did not get the device they
 * asked for. Named, rather than inline, because the wording is the feature. */
static void note_substitution(app_devices *dv, const char *dir,
			      const char *wanted, ardop_audio_match how,
			      const ardop_audio_device *got)
{
	if (!ardop_audio_match_is_substitution(how))
		return;

	/* A substitution needs something to substitute *for*. With no saved
	 * selection the operator asked for the system default and got it, which
	 * is first run, not a surprise -- warning about it would put a banner in
	 * front of every new user. */
	if (!wanted || !*wanted)
		return;

	size_t used = strlen(dv->detail);
	snprintf(dv->detail + used, sizeof dv->detail - used,
		 "%s%s: '%s' is not present; opened %s '%s' -- check this before "
		 "transmitting.",
		 used ? " " : "", dir, wanted, ardop_audio_match_str(how),
		 got->name);
}

static bool open_devices(app_devices *dv, app_spine *sp,
			 const app_device_selection *sel)
{
	dv->detail[0] = '\0';
	dv->pcap_open_failed = false;

	/* PTT first and independently of audio: the keying method and the sound
	 * card are unrelated in reality, and an operator can pair any interface
	 * with any keying line. */
	if (sel->ptt_spec[0]) {
		ardop_ptt_config pc;
		if (!ardop_ptt_parse(sel->ptt_spec, &pc)) {
			snprintf(dv->detail, sizeof dv->detail,
				 "PTT: '%s' is not a keying method I understand.",
				 sel->ptt_spec);
			return false;
		}
		dv->ptt = ardop_ptt_open(&pc);
		if (!dv->ptt) {
			snprintf(dv->detail, sizeof dv->detail,
				 "PTT: cannot open '%s'.", sel->ptt_spec);
			return false;
		}
		snprintf(dv->ptt_describe, sizeof dv->ptt_describe, "%s",
			 ardop_ptt_describe(dv->ptt));
	}

	ardop_ma_config mc;
	memset(&mc, 0, sizeof mc);
	mc.capture_id = sel->capture_id[0] ? sel->capture_id : NULL;
	mc.playback_id = sel->playback_id[0] ? sel->playback_id : NULL;
	mc.ptt = dv->ptt;
	mc.use_null_device = sel->null_device;
	mc.backend_name = sel->backend[0] ? sel->backend : NULL;

	dv->mb = ardop_backend_ma_open(&mc, &dv->ops);
	if (!dv->mb) {
		snprintf(dv->detail, sizeof dv->detail,
			 "Audio: cannot open the device. See the console for why.");
		return false;
	}

	/* The block belongs to the backend, so it arrives with the backend --
	 * see app_set_platform. */
	if (!app_set_platform(sp, &dv->ops, ardop_backend_ma_block(dv->mb))) {
		snprintf(dv->detail, sizeof dv->detail,
			 "Audio: cannot bind while transmitting.");
		return false;
	}

	/* The session log, if asked for. A failure here must not fail device
	 * open -- a debugging aid can never be the reason a station cannot get
	 * on the air -- so it is reported (APP_DEV_EV_PCAP_FAILED, in
	 * apply_open) rather than returned. */
	if (sel->pcap_enabled) {
		if (!ardop_mkdir_p(sel->pcap_dir)) {
			/* Precision-capped, not a bare %s: pcap_dir is longer
			 * than dv->detail can ever guarantee holding whole, and
			 * an explicit bound lets the compiler prove that rather
			 * than merely trusting snprintf's own truncation. */
			snprintf(dv->detail, sizeof dv->detail,
				 "session log: cannot create %.200s",
				 sel->pcap_dir);
			dv->pcap_open_failed = true;
		} else {
			time_t t = (time_t)(ardop_wall_ms() / 1000u);
			struct tm tmv;
			localtime_r(&t, &tmv);
			char stamp[32];
			strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);

			char path[600];
			snprintf(path, sizeof path, "%s/session-%s.pcap",
				 sel->pcap_dir, stamp);
			dv->pcap = ardop_capture_open(path);
			if (!dv->pcap) {
				snprintf(dv->detail, sizeof dv->detail,
					 "session log: cannot open %.200s",
					 path);
				dv->pcap_open_failed = true;
			} else {
				app_set_capture(sp, dv->pcap);
			}
		}
	}

	dv->cap_match = ardop_backend_ma_capture_match(dv->mb);
	dv->play_match = ardop_backend_ma_playback_match(dv->mb);

	/* Record what was *actually* opened. When a selection resolved by name
	 * because the id moved, this carries the new id, so the caller can
	 * persist it and the next start matches exactly. */
	ardop_audio_device cap, play;
	ardop_backend_ma_capture_device(dv->mb, &cap);
	ardop_backend_ma_playback_device(dv->mb, &play);

	dv->in_use = *sel;
	snprintf(dv->in_use.capture_id, sizeof dv->in_use.capture_id, "%s",
		 cap.id);
	snprintf(dv->in_use.capture_name, sizeof dv->in_use.capture_name, "%s",
		 cap.name);
	snprintf(dv->in_use.playback_id, sizeof dv->in_use.playback_id, "%s",
		 play.id);
	snprintf(dv->in_use.playback_name, sizeof dv->in_use.playback_name, "%s",
		 play.name);

	note_substitution(dv, "capture", sel->capture_name, dv->cap_match, &cap);
	note_substitution(dv, "playback", sel->playback_name, dv->play_match,
			  &play);
	return true;
}

static void apply_open(app_devices *dv, app_spine *sp,
		       const app_device_selection *sel)
{
	teardown(dv, sp);
	dv->requested = *sel;

	if (!open_devices(dv, sp, sel)) {
		teardown(dv, sp);
		set_state(dv, APP_DEV_FAILED);
		app_report_device(sp, APP_DEV_EV_OPEN_FAILED, dv->detail);
		return;
	}

	bool recovered = app_clear_fault(sp) == APP_RUN_FAULTED;
	set_state(dv, APP_DEV_RUNNING);
	atomic_fetch_add_explicit(&dv->generation, 1, memory_order_relaxed);

	if (dv->pcap_open_failed)
		app_report_device(sp, APP_DEV_EV_PCAP_FAILED, dv->detail);
	else if (dv->detail[0])
		app_report_device(sp, APP_DEV_EV_SUBSTITUTED, dv->detail);
	else if (recovered)
		app_report_device(sp, APP_DEV_EV_RECOVERED,
				  "devices reopened; transmit is enabled again");
	else
		app_report_device(sp, APP_DEV_EV_OPENED, "devices opened");
}

/* --- modem thread: the service --------------------------------------------- */

void app_devices_service(app_devices *dv, app_spine *sp)
{
	/* Drain the whole ring and keep the last of each kind. An operator
	 * clicking down a device list must not open five sound cards in a row. */
	request last_open, r;
	bool want_open = false, want_close = false;
	unsigned ptt_test_ms = 0;
	bool want_ptt_test = false;

	memset(&last_open, 0, sizeof last_open);
	while (app_ring_read(&dv->reqq, &r, sizeof r) == sizeof r) {
		switch ((req_kind)r.kind) {
		case REQ_OPEN:
			last_open = r;
			want_open = true;
			want_close = false;
			break;
		case REQ_CLOSE:
			want_close = true;
			want_open = false;
			break;
		case REQ_PTT_TEST:
			want_ptt_test = true;
			ptt_test_ms = r.ms;
			break;
		}
	}

	if (want_open || want_close) {
		/* app_set_platform would refuse mid-transmission anyway, and it
		 * is right to: a swap while keyed leaves the radio transmitting
		 * with nothing able to unkey it. Hold the request rather than
		 * dropping it -- a transmission ends in seconds. */
		if (ardop_runtime_tx_active(app_runtime(sp))) {
			request again;
			memset(&again, 0, sizeof again);
			if (want_open)
				again = last_open;
			else
				again.kind = REQ_CLOSE;
			(void)push(dv, &again);
		} else if (want_close) {
			teardown(dv, sp);
			set_state(dv, APP_DEV_CLOSED);
			app_report_device(sp, APP_DEV_EV_CLOSED, "devices closed");
		} else {
			apply_open(dv, sp, &last_open.sel);
		}
	}

	if (want_ptt_test) {
		if (app_devices_state(dv) != APP_DEV_RUNNING) {
			app_report_device(sp, APP_DEV_EV_PTT_TEST,
					  "PTT test: no device is open");
		} else if (app_runtime(sp)->link.state != ARDOP_LINK_DISC) {
			/* Keying under a live session would transmit noise over
			 * somebody else's frame. */
			app_report_device(sp, APP_DEV_EV_PTT_TEST,
					  "PTT test: refused while connected");
		} else {
			if (dv->ops.set_ptt)
				dv->ops.set_ptt(dv->ops.ctx, true);
			ardop_sleep_ms(ptt_test_ms ? ptt_test_ms : 1000);
			if (dv->ops.set_ptt)
				dv->ops.set_ptt(dv->ops.ctx, false);

			ardop_fault pf = ardop_ptt_fault(dv->ptt);
			app_report_device(sp, APP_DEV_EV_PTT_TEST,
					  pf == ARDOP_FAULT_NONE
						  ? "PTT test: keyed and unkeyed. "
						    "Did the radio transmit?"
						  : "PTT test: keying reported a "
						    "fault");
		}
	}

	if (app_devices_state(dv) == APP_DEV_RUNNING) {
		/* Both latches, with a fallthrough rather than a ternary. The
		 * manager owns the PTT whether or not an audio backend exists,
		 * so there is no configuration in which one of them is
		 * unreachable -- which a ternary here previously made true. */
		ardop_fault f = ardop_backend_ma_fault(dv->mb);
		if (f == ARDOP_FAULT_NONE)
			f = ardop_ptt_fault(dv->ptt);
		if (f != ARDOP_FAULT_NONE) {
			set_state(dv, APP_DEV_FAULTED);
			snprintf(dv->detail, sizeof dv->detail, "%s",
				 ardop_fault_str(f));
			app_report_fault(sp, fault_of(f), ardop_fault_str(f));
		}
	}

	if (app_devices_state(dv) != APP_DEV_RUNNING)
		ardop_sleep_ms(IDLE_SLEEP_MS);
}

void app_devices_close(app_devices *dv, app_spine *sp)
{
	teardown(dv, sp);
	set_state(dv, APP_DEV_CLOSED);
}

/* --- persistence ----------------------------------------------------------- */

void app_devices_selection_load(app_device_selection *sel,
				const ardop_settings *s)
{
	memset(sel, 0, sizeof *sel);
	snprintf(sel->capture_id, sizeof sel->capture_id, "%s",
		 ardop_settings_get(s, "audio.capture.id", ""));
	snprintf(sel->capture_name, sizeof sel->capture_name, "%s",
		 ardop_settings_get(s, "audio.capture.name", ""));
	snprintf(sel->playback_id, sizeof sel->playback_id, "%s",
		 ardop_settings_get(s, "audio.playback.id", ""));
	snprintf(sel->playback_name, sizeof sel->playback_name, "%s",
		 ardop_settings_get(s, "audio.playback.name", ""));
	snprintf(sel->backend, sizeof sel->backend, "%s",
		 ardop_settings_get(s, "audio.backend", ""));
	snprintf(sel->ptt_spec, sizeof sel->ptt_spec, "%s",
		 ardop_settings_get(s, "ptt.spec", ""));
	sel->null_device = ardop_settings_get_bool(s, "audio.null_device", false);
	sel->pcap_enabled = ardop_settings_get_bool(s, "pcap.enabled", false);
	snprintf(sel->pcap_dir, sizeof sel->pcap_dir, "%s",
		 ardop_settings_get(s, "pcap.dir", ""));
}

bool app_devices_selection_store(const app_device_selection *sel,
				 ardop_settings *s)
{
	return ardop_settings_set(s, "audio.capture.id", sel->capture_id) &&
	       ardop_settings_set(s, "audio.capture.name", sel->capture_name) &&
	       ardop_settings_set(s, "audio.playback.id", sel->playback_id) &&
	       ardop_settings_set(s, "audio.playback.name", sel->playback_name) &&
	       ardop_settings_set(s, "audio.backend", sel->backend) &&
	       ardop_settings_set(s, "ptt.spec", sel->ptt_spec) &&
	       ardop_settings_set_bool(s, "audio.null_device", sel->null_device) &&
	       ardop_settings_set_bool(s, "pcap.enabled", sel->pcap_enabled) &&
	       ardop_settings_set(s, "pcap.dir", sel->pcap_dir);
}
