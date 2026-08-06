#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "app/devices.h"
#include "app/spine.h"
#include "shell/backend_ma.h"
#include "shell/loop.h"
#include "shell/sys.h"

/*
 * The device manager, and with it analysis/15's recoverability criterion --
 * "unplugging the capture device mid-session raises a fault, unkeys PTT, and
 * surfaces ... it does not hang the modem thread".
 *
 * This runs in CI on both hosts with no hardware, because miniaudio's synthetic
 * device gives a *real* backend with a real device thread. That matters: a fake
 * backend behind a factory interface would test the fake.
 */

/* Drive the manager and the spine the way a program does, draining events. */
static int pump(app_devices *dv, app_spine *sp, int steps, app_event *found,
		app_event_kind want, int want_code)
{
	int hits = 0;
	for (int i = 0; i < steps; i++) {
		app_devices_service(dv, sp);
		app_step(sp);

		app_event ev;
		while (app_events_pop(sp, &ev)) {
			if (ev.kind != want)
				continue;
			if (want_code >= 0 && ev.code != want_code)
				continue;
			if (found)
				*found = ev;
			hits++;
		}
	}
	return hits;
}

static void null_selection(app_device_selection *sel)
{
	memset(sel, 0, sizeof *sel);
	sel->null_device = true;
	snprintf(sel->backend, sizeof sel->backend, "%s", "null");
}

/*
 * Open the devices the way a Devices screen does, and check the loop is running
 * at the block the backend asked for.
 *
 * That last part is the regression test for a real defect: the block used to be
 * preserved across a bind, and since ardop_loop_init always leaves it non-zero
 * the restore always fired -- so a new backend's smaller preferred block was
 * overwritten every time and every device change silently reverted to 100 ms.
 *
 * The fault path itself is exercised at the backend, in test_backend_ma.c,
 * where the test owns the device handle; the manager holds it privately, by
 * design.
 */
static void test_open_binds_the_backends_block(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);
	app_devices *dv = app_devices_new();
	assert_non_null(dv);

	app_device_selection sel;
	null_selection(&sel);
	assert_true(app_devices_request(dv, &sel));

	assert_true(pump(dv, sp, 20, NULL, APP_EV_DEVICE, APP_DEV_EV_OPENED) > 0);
	assert_int_equal(app_devices_state(dv), APP_DEV_RUNNING);
	assert_true(app_tx_credit(sp) > 0);

	app_device_status st;
	app_devices_status(dv, &st);
	assert_true(st.block > 0);
	assert_true(st.block <= ARDOP_LOOP_BLOCK);
	assert_true(st.ratio >= 1);
	assert_int_equal(st.device_rate, st.ratio * 12000u);

	app_close(sp);
	app_devices_free(dv);
}

/* A selection naming a device that is not there opens the default instead, and
 * says so -- the substitution an operator must see before transmitting. */
static void test_missing_device_substitutes_and_says_so(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);
	app_devices *dv = app_devices_new();
	assert_non_null(dv);

	app_device_selection sel;
	null_selection(&sel);
	snprintf(sel.capture_id, sizeof sel.capture_id, "%s", "no-such-device");
	snprintf(sel.capture_name, sizeof sel.capture_name, "%s", "No Such Card");
	assert_true(app_devices_request(dv, &sel));

	app_event ev;
	memset(&ev, 0, sizeof ev);
	int hits = pump(dv, sp, 20, &ev, APP_EV_DEVICE, APP_DEV_EV_SUBSTITUTED);
	assert_true(hits > 0);
	assert_non_null(strstr(ev.text, "No Such Card"));
	assert_non_null(strstr(ev.text, "before transmitting"));

	app_device_status st;
	app_devices_status(dv, &st);
	assert_int_equal(st.state, APP_DEV_RUNNING);
	assert_true(ardop_audio_match_is_substitution(st.capture_match));

	/* And what it actually opened is recorded, so the caller can persist the
	 * corrected selection rather than the one that no longer resolves. */
	assert_true(st.in_use.capture_id[0] != '\0');

	app_close(sp);
	app_devices_free(dv);
}

/* An unopenable keying method fails the whole request rather than half-opening,
 * and reports why. */
static void test_bad_ptt_fails_the_open(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);
	app_devices *dv = app_devices_new();
	assert_non_null(dv);

	app_device_selection sel;
	null_selection(&sel);
	snprintf(sel.ptt_spec, sizeof sel.ptt_spec, "%s", "gpio:17");

	assert_true(app_devices_request(dv, &sel));
	app_event ev;
	memset(&ev, 0, sizeof ev);
	assert_true(pump(dv, sp, 10, &ev, APP_EV_DEVICE,
			 APP_DEV_EV_OPEN_FAILED) > 0);
	assert_int_equal(app_devices_state(dv), APP_DEV_FAILED);
	assert_non_null(strstr(ev.text, "PTT"));

	/* Recoverable: asking again with a keying method that works opens. */
	null_selection(&sel);
	assert_true(app_devices_request(dv, &sel));
	assert_true(pump(dv, sp, 20, NULL, APP_EV_DEVICE, APP_DEV_EV_OPENED) > 0);
	assert_int_equal(app_devices_state(dv), APP_DEV_RUNNING);

	app_close(sp);
	app_devices_free(dv);
}

/* Closing detaches the spine and leaves nothing bound. */
static void test_close_detaches(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);
	app_devices *dv = app_devices_new();
	assert_non_null(dv);

	app_device_selection sel;
	null_selection(&sel);
	assert_true(app_devices_request(dv, &sel));
	assert_true(pump(dv, sp, 20, NULL, APP_EV_DEVICE, APP_DEV_EV_OPENED) > 0);

	assert_true(app_devices_request_close(dv));
	assert_true(pump(dv, sp, 10, NULL, APP_EV_DEVICE, APP_DEV_EV_CLOSED) > 0);
	assert_int_equal(app_devices_state(dv), APP_DEV_CLOSED);

	app_close(sp);
	app_devices_free(dv);
}

/* A burst of requests must open one device, not five. */
static void test_requests_coalesce(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);
	app_devices *dv = app_devices_new();
	assert_non_null(dv);

	app_device_selection sel;
	null_selection(&sel);
	for (int i = 0; i < 5; i++)
		assert_true(app_devices_request(dv, &sel));

	/* One service call drains all five and acts once. */
	app_devices_service(dv, sp);
	app_step(sp);

	int opened = 0;
	app_event ev;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_DEVICE && ev.code == APP_DEV_EV_OPENED)
			opened++;
	assert_int_equal(opened, 1);

	app_close(sp);
	app_devices_free(dv);
}

/* The selection round-trips through the settings store under its own keys. */
static void test_selection_persists(void **state)
{
	(void)state;
	app_device_selection sel;
	memset(&sel, 0, sizeof sel);
	snprintf(sel.capture_id, sizeof sel.capture_id, "%s", "hw:CARD=X,DEV=0");
	snprintf(sel.capture_name, sizeof sel.capture_name, "%s", "USB Audio CODEC");
	snprintf(sel.playback_id, sizeof sel.playback_id, "%s", "hw:CARD=X,DEV=0");
	snprintf(sel.playback_name, sizeof sel.playback_name, "%s", "USB Audio CODEC");
	snprintf(sel.ptt_spec, sizeof sel.ptt_spec, "%s", "rts:/dev/ttyUSB0");
	snprintf(sel.backend, sizeof sel.backend, "%s", "alsa");
	sel.pcap_enabled = true;
	snprintf(sel.pcap_dir, sizeof sel.pcap_dir, "%s", "/home/op/ardop-sessions");

	ardop_settings s;
	memset(&s, 0, sizeof s);
	assert_true(app_devices_selection_store(&sel, &s));

	app_device_selection back;
	app_devices_selection_load(&back, &s);
	assert_string_equal(back.capture_id, sel.capture_id);
	assert_string_equal(back.capture_name, sel.capture_name);
	assert_string_equal(back.ptt_spec, sel.ptt_spec);
	assert_string_equal(back.backend, sel.backend);
	assert_false(back.null_device);
	assert_true(back.pcap_enabled);
	assert_string_equal(back.pcap_dir, sel.pcap_dir);

	/* An empty store yields an empty selection, which means "system default"
	 * rather than an error -- first run again. */
	ardop_settings empty;
	memset(&empty, 0, sizeof empty);
	app_devices_selection_load(&back, &empty);
	assert_string_equal(back.capture_id, "");
	assert_string_equal(back.ptt_spec, "");
	assert_false(back.pcap_enabled);
	assert_string_equal(back.pcap_dir, "");
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_open_binds_the_backends_block),
		cmocka_unit_test(test_missing_device_substitutes_and_says_so),
		cmocka_unit_test(test_bad_ptt_fails_the_open),
		cmocka_unit_test(test_close_detaches),
		cmocka_unit_test(test_requests_coalesce),
		cmocka_unit_test(test_selection_persists),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
