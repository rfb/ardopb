#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/audio_devices.h"

/*
 * The device resolution rule (analysis/15 §4): exact id, then exact name, then
 * the system default.
 *
 * It is worth testing precisely because the failure it prevents is silent. An
 * ALSA card index renumbers when USB devices come up in a different order, and
 * the naive answer -- fall back to the default -- transmits through whatever is
 * now first in the list, which on a laptop is the built-in speakers. Into a
 * radio. With no message.
 *
 * The rule is a pure function over a list, which is what lets all of this run on
 * a machine with no sound card at all.
 */

/* Build a list without repeating the boilerplate at every call site. */
static void dev(ardop_audio_device *d, const char *id, const char *name,
		bool is_default)
{
	memset(d, 0, sizeof(*d));
	snprintf(d->id, sizeof(d->id), "%s", id);
	snprintf(d->name, sizeof(d->name), "%s", name);
	d->is_default = is_default;
}

/* The happy case: the device the operator chose is still exactly where it was. */
static void test_match_by_id(void **state)
{
	(void)state;
	ardop_audio_device devs[3];
	dev(&devs[0], "hw:CARD=PCH,DEV=0", "Built-in Audio", true);
	dev(&devs[1], "hw:CARD=CODEC,DEV=0", "USB Audio CODEC", false);
	dev(&devs[2], "hw:CARD=Webcam,DEV=0", "Webcam", false);

	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 3, "hw:CARD=CODEC,DEV=0",
						  "USB Audio CODEC", &idx),
			 ARDOP_AUDIO_MATCH_ID);
	assert_int_equal(idx, 1);
	assert_false(ardop_audio_match_is_substitution(ARDOP_AUDIO_MATCH_ID));
}

/*
 * The case the two fields exist for: the interface was replugged and its id
 * moved, but it is still present under the same name. That is the operator's
 * device and using it is correct -- so it is not a substitution, and the caller
 * writes the new id back.
 */
static void test_match_by_name_after_renumber(void **state)
{
	(void)state;
	ardop_audio_device devs[2];
	dev(&devs[0], "hw:CARD=PCH,DEV=0", "Built-in Audio", true);
	dev(&devs[1], "hw:CARD=CODEC_1,DEV=0", "USB Audio CODEC", false);

	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 2, "hw:CARD=CODEC,DEV=0",
						  "USB Audio CODEC", &idx),
			 ARDOP_AUDIO_MATCH_NAME);
	assert_int_equal(idx, 1);
	assert_false(ardop_audio_match_is_substitution(ARDOP_AUDIO_MATCH_NAME));
	assert_string_equal(devs[idx].id, "hw:CARD=CODEC_1,DEV=0");
}

/*
 * The interface is gone entirely. Falling back to the default is the documented
 * behaviour, but it IS a substitution and the operator has to be told before
 * they key a transmitter through their laptop speakers.
 */
static void test_fallback_to_default_is_a_substitution(void **state)
{
	(void)state;
	ardop_audio_device devs[2];
	dev(&devs[0], "hw:CARD=PCH,DEV=0", "Built-in Audio", true);
	dev(&devs[1], "hw:CARD=Webcam,DEV=0", "Webcam", false);

	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 2, "hw:CARD=CODEC,DEV=0",
						  "USB Audio CODEC", &idx),
			 ARDOP_AUDIO_MATCH_DEFAULT);
	assert_int_equal(idx, 0);
	assert_true(ardop_audio_match_is_substitution(ARDOP_AUDIO_MATCH_DEFAULT));
}

/*
 * A selection that is gone with no default advertised must fail rather than
 * pick the first device. "Something is missing, choose again" is a dialogue;
 * guessing which piece of hardware to transmit through is not.
 */
static void test_no_match_and_no_default(void **state)
{
	(void)state;
	ardop_audio_device devs[2];
	dev(&devs[0], "hw:CARD=A,DEV=0", "Card A", false);
	dev(&devs[1], "hw:CARD=B,DEV=0", "Card B", false);

	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 2, "hw:CARD=CODEC,DEV=0",
						  "USB Audio CODEC", &idx),
			 ARDOP_AUDIO_MATCH_NONE);
	assert_int_equal(idx, 99);   /* untouched */

	assert_int_equal(ardop_audio_match_device(NULL, 0, "x", "y", &idx),
			 ARDOP_AUDIO_MATCH_NONE);
}

/* No saved selection is not a failure -- it is first run. */
static void test_empty_selection_takes_the_default(void **state)
{
	(void)state;
	ardop_audio_device devs[2];
	dev(&devs[0], "hw:CARD=A,DEV=0", "Card A", false);
	dev(&devs[1], "hw:CARD=B,DEV=0", "Card B", true);

	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 2, "", "", &idx),
			 ARDOP_AUDIO_MATCH_DEFAULT);
	assert_int_equal(idx, 1);

	assert_int_equal(ardop_audio_match_device(devs, 2, NULL, NULL, &idx),
			 ARDOP_AUDIO_MATCH_DEFAULT);
	assert_int_equal(idx, 1);

	/* With nothing marked default, the first is used -- but only because the
	 * operator expressed no preference to violate. */
	dev(&devs[1], "hw:CARD=B,DEV=0", "Card B", false);
	assert_int_equal(ardop_audio_match_device(devs, 2, "", "", &idx),
			 ARDOP_AUDIO_MATCH_DEFAULT);
	assert_int_equal(idx, 0);
}

/* Two devices of the same model. First wins: arbitrary, and documented. */
static void test_duplicate_names_take_the_first(void **state)
{
	(void)state;
	ardop_audio_device devs[3];
	dev(&devs[0], "hw:CARD=PCH,DEV=0", "Built-in Audio", true);
	dev(&devs[1], "hw:CARD=CODEC,DEV=0", "USB Audio CODEC", false);
	dev(&devs[2], "hw:CARD=CODEC_1,DEV=0", "USB Audio CODEC", false);

	/* By id, the second of the pair is still reachable exactly -- which is the
	 * whole reason the id is tried first. */
	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 3, "hw:CARD=CODEC_1,DEV=0",
						  "USB Audio CODEC", &idx),
			 ARDOP_AUDIO_MATCH_ID);
	assert_int_equal(idx, 2);

	/* By name alone they are indistinguishable. */
	assert_int_equal(ardop_audio_match_device(devs, 3, "gone",
						  "USB Audio CODEC", &idx),
			 ARDOP_AUDIO_MATCH_NAME);
	assert_int_equal(idx, 1);
}

/* An id at the buffer's limit must still round-trip; truncation here would mean
 * a saved selection that can never match again. */
static void test_maximum_length_id(void **state)
{
	(void)state;
	char big[ARDOP_DEV_ID_MAX];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';

	ardop_audio_device devs[1];
	dev(&devs[0], big, "Long", false);
	assert_int_equal(strlen(devs[0].id), ARDOP_DEV_ID_MAX - 1);

	size_t idx = 99;
	assert_int_equal(ardop_audio_match_device(devs, 1, big, "Long", &idx),
			 ARDOP_AUDIO_MATCH_ID);
	assert_int_equal(idx, 0);
}

static void test_match_str_is_never_null(void **state)
{
	(void)state;
	for (int i = ARDOP_AUDIO_MATCH_ID; i <= ARDOP_AUDIO_MATCH_NONE; i++)
		assert_non_null(ardop_audio_match_str((ardop_audio_match)i));
}

/*
 * Enumeration itself, against miniaudio's synthetic backend -- the same trick
 * test_backend_ma uses, so it runs identically on Linux and Windows CI with no
 * sound card present.
 */
static void test_enumerate_null_backend(void **state)
{
	(void)state;
	static ardop_audio_device devs[16];

	size_t n = ardop_audio_enumerate(ARDOP_AUDIO_CAPTURE, devs,
					 sizeof(devs) / sizeof(devs[0]), "null");
	assert_true(n > 0);
	for (size_t i = 0; i < n; i++) {
		assert_true(devs[i].id[0] != '\0');
		assert_true(devs[i].name[0] != '\0');
	}

	/* And resolving against it end to end. */
	ardop_audio_device got;
	ardop_audio_match m = ardop_audio_resolve(ARDOP_AUDIO_CAPTURE, devs[0].id,
						  devs[0].name, "null", &got);
	assert_int_equal(m, ARDOP_AUDIO_MATCH_ID);
	assert_string_equal(got.id, devs[0].id);

	assert_int_equal(ardop_audio_enumerate(ARDOP_AUDIO_CAPTURE, devs, 0,
					       "null"), 0);
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_match_by_id),
		cmocka_unit_test(test_match_by_name_after_renumber),
		cmocka_unit_test(test_fallback_to_default_is_a_substitution),
		cmocka_unit_test(test_no_match_and_no_default),
		cmocka_unit_test(test_empty_selection_takes_the_default),
		cmocka_unit_test(test_duplicate_names_take_the_first),
		cmocka_unit_test(test_maximum_length_id),
		cmocka_unit_test(test_match_str_is_never_null),
		cmocka_unit_test(test_enumerate_null_backend),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
