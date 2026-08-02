#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/net.h"
#include "shell/ptt.h"
#include "shell/ptt_cat.h"
#include "shell/ptt_cm108.h"
#include "shell/sys.h"

/*
 * Keying.
 *
 * `shell/ptt.c` has claimed since it was written that "the rigctld path ... is
 * tested against a scripted fake server on localhost". It was not; there was no
 * PTT test of any kind. This is that file.
 *
 * Everything here runs with no radio and no interface, because everything that
 * *can* be decided without one is a pure function: the specification grammar,
 * the CAT frames and their replies, the CM108 report bytes, the chip table and
 * the auto-selection policy. What is left touching a device is a serial write
 * and a HID write, and those are the parts honestly marked untested.
 */

/* --- the specification grammar --------------------------------------------- */

static void test_parse_basic_methods(void **state)
{
	(void)state;
	ardop_ptt_config c;

	assert_true(ardop_ptt_parse(NULL, &c));
	assert_int_equal(c.method, ARDOP_PTT_NONE);
	assert_true(ardop_ptt_parse("", &c));
	assert_int_equal(c.method, ARDOP_PTT_NONE);
	assert_true(ardop_ptt_parse("none", &c));
	assert_int_equal(c.method, ARDOP_PTT_NONE);
	assert_true(ardop_ptt_parse("vox", &c));
	assert_int_equal(c.method, ARDOP_PTT_NONE);

	assert_true(ardop_ptt_parse("rts:/dev/ttyUSB0", &c));
	assert_int_equal(c.method, ARDOP_PTT_SERIAL_RTS);
	assert_string_equal(c.target, "/dev/ttyUSB0");

	assert_true(ardop_ptt_parse("dtr:/dev/ttyS1", &c));
	assert_int_equal(c.method, ARDOP_PTT_SERIAL_DTR);
	assert_string_equal(c.target, "/dev/ttyS1");

	/* A bare path is what --ptt used to take, and still works. */
	assert_true(ardop_ptt_parse("/dev/ttyUSB1", &c));
	assert_int_equal(c.method, ARDOP_PTT_SERIAL_RTS);
	assert_string_equal(c.target, "/dev/ttyUSB1");

	assert_false(ardop_ptt_parse("nonsense:thing", &c));
}

static void test_parse_rigctld_including_ipv6(void **state)
{
	(void)state;
	ardop_ptt_config c;

	assert_true(ardop_ptt_parse("rigctld:", &c));
	assert_int_equal(c.method, ARDOP_PTT_RIGCTLD);
	assert_string_equal(c.target, "127.0.0.1");
	assert_int_equal(c.port, 4532);

	assert_true(ardop_ptt_parse("rigctld:example.org", &c));
	assert_string_equal(c.target, "example.org");
	assert_int_equal(c.port, 4532);

	assert_true(ardop_ptt_parse("rigctld:1.2.3.4:4533", &c));
	assert_string_equal(c.target, "1.2.3.4");
	assert_int_equal(c.port, 4533);

	/*
	 * An IPv6 literal is full of colons, so splitting on the last one turned
	 * "rigctld:::1" into host ":" port 1 -- a connection to nowhere, reported
	 * as a keying failure. The bracketed form is the fix.
	 */
	assert_true(ardop_ptt_parse("rigctld:[::1]:4532", &c));
	assert_string_equal(c.target, "::1");
	assert_int_equal(c.port, 4532);

	assert_true(ardop_ptt_parse("rigctld:[::1]", &c));
	assert_string_equal(c.target, "::1");
	assert_int_equal(c.port, 4532);

	assert_true(ardop_ptt_parse("rigctld:[fe80::1%25eth0]:4599", &c));
	assert_string_equal(c.target, "fe80::1%25eth0");
	assert_int_equal(c.port, 4599);

	assert_false(ardop_ptt_parse("rigctld:[::1", &c));
}

static void test_parse_cat(void **state)
{
	(void)state;
	ardop_ptt_config c;

	assert_true(ardop_ptt_parse("civ:/dev/ttyUSB0", &c));
	assert_int_equal(c.method, ARDOP_PTT_CAT);
	assert_int_equal(c.cat_family, ARDOP_CAT_CIV);
	assert_string_equal(c.target, "/dev/ttyUSB0");
	assert_int_equal(c.civ_addr, ARDOP_CIV_DEFAULT_ADDR);

	/* The CI-V address, which the radio table carries so an operator never
	 * has to learn what one is: a4 for the Xiegu X6100/X6200/G90. */
	assert_true(ardop_ptt_parse("civ:/dev/ttyUSB0@a4", &c));
	assert_int_equal(c.civ_addr, 0xa4);
	assert_string_equal(c.target, "/dev/ttyUSB0");

	assert_true(ardop_ptt_parse("civ:/dev/ttyUSB0@94", &c));
	assert_int_equal(c.civ_addr, 0x94);

	/* Icom and Xiegu are the same protocol, and both spellings work. */
	assert_true(ardop_ptt_parse("icom:/dev/ttyUSB0", &c));
	assert_int_equal(c.cat_family, ARDOP_CAT_CIV);
	assert_true(ardop_ptt_parse("xiegu:/dev/ttyUSB0@a4", &c));
	assert_int_equal(c.cat_family, ARDOP_CAT_CIV);
	assert_int_equal(c.civ_addr, 0xa4);

	assert_true(ardop_ptt_parse("kenwood:/dev/ttyUSB2", &c));
	assert_int_equal(c.cat_family, ARDOP_CAT_KENWOOD);
	assert_true(ardop_ptt_parse("yaesu:/dev/ttyUSB3", &c));
	assert_int_equal(c.cat_family, ARDOP_CAT_YAESU);

	/* A baud rate, because a radio at the wrong rate simply does not answer
	 * and that looks exactly like a dead cable. */
	assert_true(ardop_ptt_parse("civ:/dev/ttyUSB0:38400@a4", &c));
	assert_int_equal(c.baud, 38400);
	assert_int_equal(c.civ_addr, 0xa4);
	assert_string_equal(c.target, "/dev/ttyUSB0");

	assert_false(ardop_ptt_parse("civ:", &c));
	assert_false(ardop_ptt_parse("civ:/dev/ttyUSB0@zz", &c));
}

static void test_parse_cm108(void **state)
{
	(void)state;
	ardop_ptt_config c;

	assert_true(ardop_ptt_parse("cm108", &c));
	assert_int_equal(c.method, ARDOP_PTT_CM108);
	assert_int_equal(c.gpio, ARDOP_CM108_DEFAULT_GPIO);
	assert_string_equal(c.target, "");

	assert_true(ardop_ptt_parse("cm108:auto", &c));
	assert_string_equal(c.target, "");

	assert_true(ardop_ptt_parse("cm108:/dev/hidraw3", &c));
	assert_string_equal(c.target, "/dev/hidraw3");
	assert_int_equal(c.gpio, 3);

	assert_true(ardop_ptt_parse("cm108:/dev/hidraw3+4", &c));
	assert_string_equal(c.target, "/dev/hidraw3");
	assert_int_equal(c.gpio, 4);

	/* An SSS1623 has two GPIOs, so +2 is the sensible thing to write. */
	assert_true(ardop_ptt_parse("cm108:auto+2", &c));
	assert_int_equal(c.gpio, 2);

	assert_true(ardop_ptt_parse("cm108:0d8c:000c", &c));
	assert_int_equal(c.hid_vid, 0x0d8c);
	assert_int_equal(c.hid_pid, 0x000c);

	assert_false(ardop_ptt_parse("cm108:auto+0", &c));
	assert_false(ardop_ptt_parse("cm108:auto+9", &c));

	/* GPIO keying is still refused, and says what to use instead. */
	assert_false(ardop_ptt_parse("gpio:17", &c));
}

/* --- the CAT frames -------------------------------------------------------- */

/*
 * From Icom's own CI-V reference guide: command 1C sub-command 00, "Send/read
 * the transceiver's status", answered FB or FA.
 */
static void test_cat_frames(void **state)
{
	(void)state;
	uint8_t f[ARDOP_CAT_FRAME_MAX];

	const uint8_t key_a4[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x01, 0xFD};
	const uint8_t unkey_a4[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x00, 0xFD};

	assert_int_equal(ardop_cat_frame(ARDOP_CAT_CIV, 0xA4, true, f, sizeof f),
			 sizeof key_a4);
	assert_memory_equal(f, key_a4, sizeof key_a4);

	assert_int_equal(ardop_cat_frame(ARDOP_CAT_CIV, 0xA4, false, f, sizeof f),
			 sizeof unkey_a4);
	assert_memory_equal(f, unkey_a4, sizeof unkey_a4);

	/* The address is the only thing that varies between rigs: 94 is an
	 * IC-7300, 70 an X5105. */
	assert_int_equal(ardop_cat_frame(ARDOP_CAT_CIV, 0x94, true, f, sizeof f), 8);
	assert_int_equal(f[2], 0x94);
	assert_int_equal(ardop_cat_frame(ARDOP_CAT_CIV, 0x70, true, f, sizeof f), 8);
	assert_int_equal(f[2], 0x70);

	assert_int_equal(ardop_cat_frame(ARDOP_CAT_KENWOOD, 0, true, f, sizeof f), 3);
	assert_memory_equal(f, "TX;", 3);
	assert_int_equal(ardop_cat_frame(ARDOP_CAT_KENWOOD, 0, false, f, sizeof f), 3);
	assert_memory_equal(f, "RX;", 3);

	assert_int_equal(ardop_cat_frame(ARDOP_CAT_YAESU, 0, true, f, sizeof f), 4);
	assert_memory_equal(f, "TX1;", 4);
	assert_int_equal(ardop_cat_frame(ARDOP_CAT_YAESU, 0, false, f, sizeof f), 4);
	assert_memory_equal(f, "TX0;", 4);

	/* A buffer that cannot hold the frame produces nothing, rather than a
	 * truncated command the radio would misread. */
	assert_int_equal(ardop_cat_frame(ARDOP_CAT_CIV, 0xA4, true, f, 4), 0);
}

static void test_cat_replies(void **state)
{
	(void)state;
	size_t used = 0;

	const uint8_t ok[] = {0xFE, 0xFE, 0xE0, 0xA4, 0xFB, 0xFD};
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV, 0xA4, ok,
					       sizeof ok, &used),
			 ARDOP_CAT_ACK);
	assert_int_equal(used, sizeof ok);

	const uint8_t ng[] = {0xFE, 0xFE, 0xE0, 0xA4, 0xFA, 0xFD};
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV, 0xA4, ng,
					       sizeof ng, &used),
			 ARDOP_CAT_NAK);

	/* Short of the terminator: read more rather than guessing. */
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV, 0xA4, ok, 4, &used),
			 ARDOP_CAT_INCOMPLETE);
	assert_int_equal(used, 0);

	/*
	 * The one that matters most in practice: a rig with CI-V transceive
	 * enabled echoes our own command back before answering it. Taking the
	 * echo for the reply would report that the radio had accepted a frame it
	 * has not yet parsed.
	 */
	const uint8_t echo_then_ok[] = {
		0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x01, 0xFD,   /* our command */
		0xFE, 0xFE, 0xE0, 0xA4, 0xFB, 0xFD,               /* the answer */
	};
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV, 0xA4,
					       echo_then_ok,
					       sizeof echo_then_ok, &used),
			 ARDOP_CAT_IGNORE);
	assert_int_equal(used, 8);
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV, 0xA4,
					       echo_then_ok + used,
					       sizeof echo_then_ok - used, &used),
			 ARDOP_CAT_ACK);

	/* A frame addressed to a different controller is not ours. */
	const uint8_t other[] = {0xFE, 0xFE, 0xEE, 0xA4, 0xFB, 0xFD};
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV, 0xA4, other,
					       sizeof other, &used),
			 ARDOP_CAT_IGNORE);

	/* Broadcast accepts an answer from any rig, which is what makes the
	 * default address usable when the real one is unknown. */
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_CIV,
					       ARDOP_CIV_DEFAULT_ADDR, ok,
					       sizeof ok, &used),
			 ARDOP_CAT_ACK);

	/* Kenwood says nothing at all, and that is documented rather than
	 * silently treated as confirmation. */
	assert_false(ardop_cat_is_acknowledged(ARDOP_CAT_KENWOOD));
	assert_true(ardop_cat_is_acknowledged(ARDOP_CAT_CIV));
	assert_true(ardop_cat_is_acknowledged(ARDOP_CAT_YAESU));

	/* Yaesu complains only on failure. */
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_YAESU, 0,
					       (const uint8_t *)"?;", 2, &used),
			 ARDOP_CAT_NAK);
	assert_int_equal(ardop_cat_parse_reply(ARDOP_CAT_YAESU, 0, NULL, 0,
					       &used),
			 ARDOP_CAT_ACK);
}

/* --- CM108 ----------------------------------------------------------------- */

/*
 * Five bytes, not four. direwolf's cm108.c writes the fifth with the comment
 * "Writing 5 bytes works. I have no idea why. From the CMedia datasheet it looks
 * like we need 4." Every interface in the field has been tested against that,
 * and with no hardware here, preferring the datasheet would be a guess.
 */
static void test_cm108_report_bytes(void **state)
{
	(void)state;
	uint8_t r[ARDOP_CM108_REPORT_LEN];

	assert_true(ardop_cm108_report(3, true, r));
	assert_int_equal(r[0], 0x00);
	assert_int_equal(r[1], 0x00);
	assert_int_equal(r[2], 0x04);   /* GPIO3 asserted */
	assert_int_equal(r[3], 0x04);   /* GPIO3 is an output */
	assert_int_equal(r[4], 0x00);

	assert_true(ardop_cm108_report(3, false, r));
	assert_int_equal(r[2], 0x00);   /* deasserted */
	assert_int_equal(r[3], 0x04);   /* still an output */

	/* Every pin, both states. */
	for (unsigned g = 1; g <= 8; g++) {
		uint8_t want = (uint8_t)(1u << (g - 1));
		assert_true(ardop_cm108_report(g, true, r));
		assert_int_equal(r[2], want);
		assert_int_equal(r[3], want);
		assert_true(ardop_cm108_report(g, false, r));
		assert_int_equal(r[2], 0x00);
		assert_int_equal(r[3], want);
	}

	assert_false(ardop_cm108_report(0, true, r));
	assert_false(ardop_cm108_report(9, true, r));
}

/*
 * The chip table earns its place through the GPIO counts, not the names: an
 * SSS1623 has two pins, so the usual default of 3 would set a bit the chip does
 * not have and key nothing at all.
 */
static void test_cm108_chip_table(void **state)
{
	(void)state;

	assert_int_equal(ardop_cm108_gpio_count(0x0d8c, 0x000c), 8);
	assert_int_equal(ardop_cm108_gpio_count(0x0d8c, 0x0012), 3);
	assert_int_equal(ardop_cm108_gpio_count(0x0d8c, 0x0139), 3);
	assert_int_equal(ardop_cm108_gpio_count(0x0d8c, 0x013a), 8);

	/* The second vendor, which a C-Media-only match would have missed. */
	assert_int_equal(ardop_cm108_gpio_count(0x0c76, 0x1607), 2);
	assert_int_equal(ardop_cm108_gpio_count(0x0c76, 0x1605), 2);

	assert_true(ardop_cm108_is_known(0x0d8c, 0x0008));
	assert_true(ardop_cm108_is_known(0x0c76, 0x160b));
	assert_false(ardop_cm108_is_known(0x1234, 0x5678));
	assert_int_equal(ardop_cm108_gpio_count(0x1234, 0x5678), 0);

	assert_non_null(ardop_cm108_chip_name(0x0d8c, 0x0012));
	assert_null(ardop_cm108_chip_name(0x1234, 0x5678));
}

static void test_cm108_auto_policy(void **state)
{
	(void)state;
	ardop_cm108_candidate c[3];
	size_t idx = 99;
	char why[512];

	memset(c, 0, sizeof c);
	snprintf(c[0].path, sizeof c[0].path, "%s", "/dev/hidraw0");
	c[0].vid = 0x046d; c[0].pid = 0xc52b;          /* a mouse */
	snprintf(c[1].path, sizeof c[1].path, "%s", "/dev/hidraw3");
	c[1].vid = 0x0d8c; c[1].pid = 0x000c;          /* the interface */

	assert_int_equal(ardop_cm108_choose(c, 2, 0, 0, &idx, why, sizeof why), 1);
	assert_int_equal(idx, 1);

	/* Nothing plugged in: say what to do, not just that it failed. */
	assert_int_equal(ardop_cm108_choose(c, 1, 0, 0, &idx, why, sizeof why), 0);
	assert_non_null(strstr(why, "cm108:/dev/hidrawN"));

	/*
	 * Two identical dongles. This is exactly the case where a guess keys the
	 * wrong radio, so it refuses -- and names both, with their ids, so the
	 * operator can paste one back.
	 */
	snprintf(c[2].path, sizeof c[2].path, "%s", "/dev/hidraw4");
	c[2].vid = 0x0d8c; c[2].pid = 0x000c;
	assert_int_equal(ardop_cm108_choose(c, 3, 0, 0, &idx, why, sizeof why), -1);
	assert_non_null(strstr(why, "/dev/hidraw3"));
	assert_non_null(strstr(why, "/dev/hidraw4"));

	/* And an explicit VID:PID narrows two to one when they differ. */
	c[2].pid = 0x0012;
	assert_int_equal(ardop_cm108_choose(c, 3, 0x0d8c, 0x0012, &idx, why,
					    sizeof why), 1);
	assert_int_equal(idx, 2);
}

static void test_cm108_hid_id_parser(void **state)
{
	(void)state;
	uint16_t vid = 0, pid = 0;

	assert_true(ardop_cm108_parse_hid_id("HID_ID=0003:00000D8C:0000000C\n",
					     &vid, &pid));
	assert_int_equal(vid, 0x0d8c);
	assert_int_equal(pid, 0x000c);

	assert_true(ardop_cm108_parse_hid_id("HID_ID=0003:00000C76:00001607",
					     &vid, &pid));
	assert_int_equal(vid, 0x0c76);
	assert_int_equal(pid, 0x1607);

	/* Bus 0005 is Bluetooth, which has no GPIO to key with. */
	assert_false(ardop_cm108_parse_hid_id("HID_ID=0005:00000D8C:0000000C",
					      &vid, &pid));

	assert_false(ardop_cm108_parse_hid_id("DEVNAME=hidraw0", &vid, &pid));
	assert_false(ardop_cm108_parse_hid_id("HID_ID=garbage", &vid, &pid));
	assert_false(ardop_cm108_parse_hid_id(NULL, &vid, &pid));
}

/* --- rigctld --------------------------------------------------------------- */

/*
 * What is NOT tested here, stated plainly.
 *
 * The rigctld byte exchange -- that we send exactly "T 1\n" and "T 0\n", that
 * "RPRT 0" is accepted and "RPRT -1" latches a fault, and that close() unkeys --
 * needs a server answering while ardop_ptt_open() blocks waiting for it, and
 * therefore needs a second thread. This suite runs on both hosts inside
 * test-core, which has no threading; the tree's only threaded tests are
 * Linux-only and live behind their own targets.
 *
 * shell/ptt.c's file comment used to claim this exchange "is tested against a
 * scripted fake server on localhost". It was not, and the comment has been
 * corrected rather than left to be believed. The frames themselves are pinned
 * where they can be -- ardop_cat_frame above is exhaustive -- and what remains
 * unverified is one snprintf and a socket write.
 */

/*
 * A rigctld that is not there must fail the *open*, not the first transmission.
 * Discovering a dead CAT link three minutes into a session, as a keying failure,
 * is the behaviour this replaces.
 */
static void test_rigctld_absent_fails_open(void **state)
{
	(void)state;
	ardop_ptt_config c;
	/* A port nothing is listening on. */
	assert_true(ardop_ptt_parse("rigctld:127.0.0.1:14599", &c));
	assert_null(ardop_ptt_open(&c));
}

/* --- the always-available method ------------------------------------------- */

static void test_none_is_always_openable(void **state)
{
	(void)state;
	ardop_ptt_config c;
	assert_true(ardop_ptt_parse("none", &c));

	ardop_ptt *p = ardop_ptt_open(&c);
	assert_non_null(p);
	assert_false(ardop_ptt_keyed(p));

	ardop_ptt_set(p, true);
	assert_true(ardop_ptt_keyed(p));
	ardop_ptt_set(p, false);
	assert_false(ardop_ptt_keyed(p));

	assert_int_equal(ardop_ptt_fault(p), ARDOP_FAULT_NONE);
	assert_non_null(strstr(ardop_ptt_describe(p), "none"));
	ardop_ptt_close(p);

	assert_string_equal(ardop_ptt_describe(NULL), "none");
	assert_int_equal(ardop_ptt_fault(NULL), ARDOP_FAULT_NONE);
	ardop_ptt_close(NULL);
}

static void test_opening_a_missing_serial_port_fails(void **state)
{
	(void)state;
	ardop_ptt_config c;
	assert_true(ardop_ptt_parse("rts:/dev/there-is-no-such-port", &c));
	assert_null(ardop_ptt_open(&c));
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_parse_basic_methods),
		cmocka_unit_test(test_parse_rigctld_including_ipv6),
		cmocka_unit_test(test_parse_cat),
		cmocka_unit_test(test_parse_cm108),
		cmocka_unit_test(test_cat_frames),
		cmocka_unit_test(test_cat_replies),
		cmocka_unit_test(test_cm108_report_bytes),
		cmocka_unit_test(test_cm108_chip_table),
		cmocka_unit_test(test_cm108_auto_policy),
		cmocka_unit_test(test_cm108_hid_id_parser),
		cmocka_unit_test(test_rigctld_absent_fails_open),
		cmocka_unit_test(test_none_is_always_openable),
		cmocka_unit_test(test_opening_a_missing_serial_port_fails),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
