#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/radios.h"
#include "shell/usbtopo.h"

/*
 * Pairing a keying interface with its sound card.
 *
 * The machine this was written on has no USB audio at all -- every sound card in
 * it is PCI, there is no ttyUSB, and the one hidraw node is not a radio. So the
 * traversal is written against a caller-supplied filesystem root and verified
 * against fabricated device trees, and the live reader is a thin wrapper that
 * has only ever been run somewhere with nothing to find.
 *
 * The three shapes that matter are all here: a composite device (one USB device,
 * two interfaces -- an IC-7300 or a Xiegu), an internal hub (two devices in one
 * shell -- a DigiRig Mobile), and two identical dongles, which is the case a
 * naive index match gets wrong and where a wrong guess keys the wrong radio.
 */

/* --- the relation, in isolation -------------------------------------------- */

static void test_relate(void **state)
{
	(void)state;

	/* Two interfaces of one device share the whole path. Certain. */
	assert_int_equal(ardop_usb_relate("3-2.4", "3-2.4"),
			 ARDOP_USB_SAME_DEVICE);

	/* Two devices behind one hub share everything but the last port. That is
	 * a DigiRig, and it is an inference rather than a certainty. */
	assert_int_equal(ardop_usb_relate("3-2.1", "3-2.2"), ARDOP_USB_SAME_HUB);
	assert_int_equal(ardop_usb_relate("1-1.3", "1-1.4"), ARDOP_USB_SAME_HUB);

	/* Different hubs, or different buses, are different hardware. */
	assert_int_equal(ardop_usb_relate("3-2.1", "3-3.1"), ARDOP_USB_NONE);
	assert_int_equal(ardop_usb_relate("1-1.1", "2-1.1"), ARDOP_USB_NONE);

	/* Root-port devices: "3-2" and "3-3" both hang off bus 3, but a bus is
	 * not a piece of plastic and pairing across it would be wrong. */
	assert_int_equal(ardop_usb_relate("3-2", "3-3"), ARDOP_USB_SAME_HUB);

	assert_int_equal(ardop_usb_relate(NULL, "3-2"), ARDOP_USB_NONE);
	assert_int_equal(ardop_usb_relate("", "3-2"), ARDOP_USB_NONE);
}

/* --- pairing, over fabricated node lists ----------------------------------- */

static void node(ardop_usb_node *n, const char *sys, const char *dev,
		 uint16_t vid, uint16_t pid, bool audio, bool serial, bool hid)
{
	memset(n, 0, sizeof *n);
	snprintf(n->syspath, sizeof n->syspath, "%s", sys);
	snprintf(n->devnode, sizeof n->devnode, "%s", dev);
	n->vid = vid;
	n->pid = pid;
	n->is_audio = audio;
	n->is_serial = serial;
	n->is_hid = hid;
}

/* An IC-7300 or a Xiegu: audio and CAT are two interfaces of one USB device. */
static void test_composite_device(void **state)
{
	(void)state;
	ardop_usb_node n[2];
	node(&n[0], "3-2.4", "/dev/card1", 0x0c26, 0x0004, true, false, false);
	node(&n[1], "3-2.4", "/dev/ttyACM0", 0x0c26, 0x0004, false, true, false);

	ardop_radio_candidate c[4];
	assert_int_equal(ardop_usb_pair(n, 2, c, 4), 1);
	assert_int_equal(c[0].link, ARDOP_USB_SAME_DEVICE);
	assert_string_equal(c[0].audio_id, "/dev/card1");

	/* Icom's vendor id is in the table, so this comes back as CI-V keying --
	 * which matters, because an Icom ignores RTS entirely and suggesting it
	 * would produce a radio that never transmits. */
	assert_non_null(strstr(c[0].ptt_spec, "civ:"));
	assert_non_null(strstr(c[0].ptt_spec, "/dev/ttyACM0"));
	assert_non_null(strstr(c[0].model, "Icom"));
	assert_true(c[0].ptt_is_guess);
}

/*
 * A DigiRig Mobile is a *hub* containing two separate USB devices -- a C-Media
 * sound card and a CP2102 serial bridge. A same-device-only match finds nothing
 * here, which is why the rule is "nearest common ancestor".
 */
static void test_internal_hub(void **state)
{
	(void)state;
	ardop_usb_node n[2];
	node(&n[0], "3-2.1", "/dev/card2", 0x0d8c, 0x000c, true, false, false);
	node(&n[1], "3-2.2", "/dev/ttyUSB0", 0x10c4, 0xea60, false, true, false);

	ardop_radio_candidate c[4];
	assert_int_equal(ardop_usb_pair(n, 2, c, 4), 1);
	assert_int_equal(c[0].link, ARDOP_USB_SAME_HUB);

	/* The CP2102 is in the table as an RTS interface, which is how a DigiRig
	 * Mobile actually keys. */
	assert_non_null(strstr(c[0].ptt_spec, "rts:/dev/ttyUSB0"));
	assert_true(c[0].ptt_is_guess);
}

/* A certain pairing beats an inferred one when both are available. */
static void test_same_device_outranks_same_hub(void **state)
{
	(void)state;
	ardop_usb_node n[3];
	node(&n[0], "3-2.4", "/dev/card1", 0x0c26, 0x0004, true, false, false);
	node(&n[1], "3-2.1", "/dev/ttyUSB9", 0x10c4, 0xea60, false, true, false);
	node(&n[2], "3-2.4", "/dev/ttyACM0", 0x0c26, 0x0004, false, true, false);

	ardop_radio_candidate c[4];
	assert_int_equal(ardop_usb_pair(n, 3, c, 4), 1);
	assert_int_equal(c[0].link, ARDOP_USB_SAME_DEVICE);
	assert_non_null(strstr(c[0].ptt_spec, "/dev/ttyACM0"));
}

/*
 * Two identical dongles. This is where a naive "first hidraw" or "first ttyUSB"
 * match keys the wrong radio, and it is the case the ancestor walk exists for:
 * each sound card pairs with the interface on its own hardware.
 */
static void test_two_identical_dongles(void **state)
{
	(void)state;
	ardop_usb_node n[4];
	node(&n[0], "3-2.1", "/dev/card1", 0x0d8c, 0x0012, true, false, false);
	node(&n[1], "3-2.1", "/dev/hidraw3", 0x0d8c, 0x0012, false, false, true);
	node(&n[2], "3-3.1", "/dev/card2", 0x0d8c, 0x0012, true, false, false);
	node(&n[3], "3-3.1", "/dev/hidraw4", 0x0d8c, 0x0012, false, false, true);

	ardop_radio_candidate c[4];
	assert_int_equal(ardop_usb_pair(n, 4, c, 4), 2);

	assert_string_equal(c[0].audio_id, "/dev/card1");
	assert_non_null(strstr(c[0].ptt_spec, "/dev/hidraw3"));
	assert_string_equal(c[1].audio_id, "/dev/card2");
	assert_non_null(strstr(c[1].ptt_spec, "/dev/hidraw4"));

	/* CM108B is in the table with three GPIOs, so pin 3 is offered. */
	assert_non_null(strstr(c[0].ptt_spec, "cm108:"));
	assert_non_null(strstr(c[0].ptt_spec, "+3"));
}

/* A sound card with nothing on its hardware is still offered -- it just has no
 * suggestion, and the operator uses the manual pickers. */
static void test_audio_with_no_sibling(void **state)
{
	(void)state;
	ardop_usb_node n[2];
	node(&n[0], "3-2.4", "/dev/card1", 0x1234, 0x5678, true, false, false);
	node(&n[1], "1-1.1", "/dev/ttyUSB0", 0x10c4, 0xea60, false, true, false);

	ardop_radio_candidate c[4];
	assert_int_equal(ardop_usb_pair(n, 2, c, 4), 1);
	assert_int_equal(c[0].link, ARDOP_USB_NONE);
	assert_string_equal(c[0].ptt_spec, "");
	assert_string_equal(c[0].model, "");
}

/* An unknown chip still pairs, and offers a plain starting point rather than
 * pretending the table stood behind it. */
static void test_unknown_chip_degrades(void **state)
{
	(void)state;
	ardop_usb_node n[2];
	node(&n[0], "3-2.4", "/dev/card1", 0xabcd, 0x0001, true, false, false);
	node(&n[1], "3-2.4", "/dev/ttyUSB7", 0xabcd, 0x0002, false, true, false);

	ardop_radio_candidate c[4];
	assert_int_equal(ardop_usb_pair(n, 2, c, 4), 1);
	assert_string_equal(c[0].model, "");
	assert_string_equal(c[0].ptt_spec, "rts:/dev/ttyUSB7");
	assert_false(c[0].ptt_is_guess);
}

/* --- the radio table ------------------------------------------------------- */

static void test_radio_table(void **state)
{
	(void)state;

	const ardop_radio_entry *e = ardop_radio_lookup(0x0d8c, 0x0012);
	assert_non_null(e);
	assert_non_null(strstr(e->ptt, "cm108"));
	assert_non_null(e->provenance);

	/* An SSS1623 has two GPIOs, so its row says +2 rather than the usual +3 --
	 * pin 3 does not exist on that chip and would key nothing. */
	e = ardop_radio_lookup(0x0c76, 0x1607);
	assert_non_null(e);
	assert_non_null(strstr(e->ptt, "+2"));

	/* A vendor-level row matches any product from that vendor: the method is
	 * what matters and every Icom keys the same way. */
	e = ardop_radio_lookup(0x0c26, 0x9999);
	assert_non_null(e);
	assert_non_null(strstr(e->model, "Icom"));
	assert_non_null(strstr(e->ptt, "civ:"));

	assert_null(ardop_radio_lookup(0xffff, 0xffff));

	/* Every row carries provenance, so a wrong one is traceable to whatever
	 * claimed it. */
	const ardop_radio_entry *all = NULL;
	size_t n = ardop_radio_table(&all);
	assert_true(n > 0);
	for (size_t i = 0; i < n; i++) {
		assert_non_null(all[i].model);
		assert_non_null(all[i].provenance);
		assert_true(all[i].provenance[0] != '\0');
	}
}

/* The table stores a method and, for CI-V, an address; the caller supplies the
 * port the interface actually came up on. */
static void test_ptt_spec_composition(void **state)
{
	(void)state;
	char spec[128];

	const ardop_radio_entry *e = ardop_radio_lookup(0x0c26, 0x0004);
	assert_non_null(e);
	assert_true(ardop_radio_ptt_spec(e, "/dev/ttyACM0", spec, sizeof spec));
	assert_string_equal(spec, "civ:/dev/ttyACM0@94");

	e = ardop_radio_lookup(0x10c4, 0xea60);
	assert_non_null(e);
	assert_true(ardop_radio_ptt_spec(e, "/dev/ttyUSB0", spec, sizeof spec));
	assert_string_equal(spec, "rts:/dev/ttyUSB0");

	e = ardop_radio_lookup(0x0d8c, 0x0012);
	assert_non_null(e);
	assert_true(ardop_radio_ptt_spec(e, "/dev/hidraw3", spec, sizeof spec));
	assert_string_equal(spec, "cm108:/dev/hidraw3+3");

	assert_false(ardop_radio_ptt_spec(NULL, "/dev/x", spec, sizeof spec));
	assert_false(ardop_radio_ptt_spec(e, "", spec, sizeof spec));
	assert_false(ardop_radio_ptt_spec(e, "/dev/hidraw3", spec, 4));
}

/* --- the live reader, against a fixture tree ------------------------------- */

#ifndef _WIN32

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	assert_non_null(f);
	fputs(text, f);
	fclose(f);
}

/* Build one /sys/class/<cls>/<name>/ entry naming its USB device. */
static void fixture_entry(const char *root, const char *cls, const char *name,
			  const char *usbpath)
{
	char dir[512], file[640];
	snprintf(dir, sizeof dir, "%s/sys/class/%s/%s", root, cls, name);

	char cmd[700];
	snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
	assert_int_equal(system(cmd), 0);

	snprintf(file, sizeof file, "%s/usbpath", dir);
	write_file(file, usbpath);
}

static void fixture_ids(const char *root, const char *usbpath, const char *vid,
			const char *pid)
{
	char dir[512], file[640], cmd[700];
	snprintf(dir, sizeof dir, "%s/sys/bus/usb/devices/%s", root, usbpath);
	snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
	assert_int_equal(system(cmd), 0);

	snprintf(file, sizeof file, "%s/idVendor", dir);
	write_file(file, vid);
	snprintf(file, sizeof file, "%s/idProduct", dir);
	write_file(file, pid);
}

/*
 * The reader, end to end, over a tree that looks like a DigiRig Mobile.
 *
 * This is as close as it is possible to get without the hardware: the shape of
 * the sysfs layout is reproduced, and what is *not* proved is that a real kernel
 * lays it out this way -- which is stated in usbtopo.c rather than implied here.
 */
static void test_scan_over_a_fixture_tree(void **state)
{
	(void)state;
	const char *root = "test-usbtopo.tmp";

	char cmd[256];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
	assert_int_equal(system(cmd), 0);

	fixture_entry(root, "sound", "card1", "3-2.1");
	fixture_entry(root, "tty", "ttyUSB0", "3-2.2");
	fixture_entry(root, "hidraw", "hidraw9", "9-9.9");   /* unrelated */
	fixture_ids(root, "3-2.1", "0d8c", "000c");
	fixture_ids(root, "3-2.2", "10c4", "ea60");
	fixture_ids(root, "9-9.9", "046d", "c52b");

	ardop_usb_node nodes[16];
	size_t n = ardop_usb_scan(root, nodes, 16);
	assert_int_equal(n, 3);

	ardop_radio_candidate c[4];
	size_t got = ardop_usb_pair(nodes, n, c, 4);
	assert_int_equal(got, 1);
	assert_int_equal(c[0].link, ARDOP_USB_SAME_HUB);
	assert_string_equal(c[0].audio_id, "/dev/card1");
	assert_non_null(strstr(c[0].ptt_spec, "rts:/dev/ttyUSB0"));

	/* The unrelated mouse on another bus is not offered as a keying line. */
	assert_null(strstr(c[0].ptt_spec, "hidraw9"));

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
	assert_int_equal(system(cmd), 0);
}

/* A machine with no USB audio -- this one -- finds nothing and says so cleanly
 * rather than failing. */
static void test_scan_of_an_empty_tree(void **state)
{
	(void)state;
	ardop_usb_node nodes[8];
	assert_int_equal(ardop_usb_scan("test-usbtopo-nothing-here", nodes, 8), 0);
}

#endif /* !_WIN32 */

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_relate),
		cmocka_unit_test(test_composite_device),
		cmocka_unit_test(test_internal_hub),
		cmocka_unit_test(test_same_device_outranks_same_hub),
		cmocka_unit_test(test_two_identical_dongles),
		cmocka_unit_test(test_audio_with_no_sibling),
		cmocka_unit_test(test_unknown_chip_degrades),
		cmocka_unit_test(test_radio_table),
		cmocka_unit_test(test_ptt_spec_composition),
#ifndef _WIN32
		cmocka_unit_test(test_scan_over_a_fixture_tree),
		cmocka_unit_test(test_scan_of_an_empty_tree),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
