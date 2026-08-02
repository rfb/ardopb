#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/settings.h"

/*
 * The persistent key/value store.
 *
 * Hermetic: every path is built under the test's own working directory and
 * removed afterwards, and nothing here calls ardop_settings_path(), so no case
 * depends on HOME, XDG_CONFIG_HOME or APPDATA. That separation is why the path
 * lookup lives in one function and the parser in another.
 */

#define TMP "test-settings.tmp"

static void unlink_tmp(void)
{
	remove(TMP);
	remove(TMP ".new");
}

static void write_file(const char *text)
{
	FILE *f = fopen(TMP, "w");
	assert_non_null(f);
	fputs(text, f);
	fclose(f);
}

static void read_file(char *out, size_t cap)
{
	FILE *f = fopen(TMP, "r");
	assert_non_null(f);
	size_t n = fread(out, 1, cap - 1, f);
	out[n] = '\0';
	fclose(f);
}

/* --- the basics ------------------------------------------------------------ */

static void test_round_trip(void **state)
{
	(void)state;
	unlink_tmp();

	ardop_settings s;
	memset(&s, 0, sizeof s);
	assert_true(ardop_settings_set(&s, "station.mycall", "N0CALL-4"));
	assert_true(ardop_settings_set_num(&s, "ptt.gpio", 3));
	assert_true(ardop_settings_set_bool(&s, "link.listen", true));
	assert_true(ardop_settings_save(&s, TMP));

	ardop_settings back;
	assert_true(ardop_settings_load(&back, TMP));
	assert_int_equal(back.n, 3);
	assert_int_equal(back.skipped, 0);
	assert_false(back.truncated);
	assert_string_equal(ardop_settings_get(&back, "station.mycall", ""),
			    "N0CALL-4");
	assert_int_equal(ardop_settings_get_num(&back, "ptt.gpio", 0), 3);
	assert_true(ardop_settings_get_bool(&back, "link.listen", false));

	/* Absent keys take the fallback rather than inventing a value. */
	assert_string_equal(ardop_settings_get(&back, "nope", "dflt"), "dflt");
	assert_int_equal(ardop_settings_get_num(&back, "nope", 42), 42);
	assert_true(ardop_settings_get_bool(&back, "nope", true));

	unlink_tmp();
}

/* First run is not an error. An application that refuses to start because it has
 * never been configured is one nobody gets past. */
static void test_missing_file_is_success(void **state)
{
	(void)state;
	unlink_tmp();

	ardop_settings s;
	assert_true(ardop_settings_load(&s, TMP));
	assert_int_equal(s.n, 0);
	assert_int_equal(s.skipped, 0);
	assert_false(s.truncated);
}

/*
 * The rule that makes the format extensible: a key this build has never heard of
 * survives a load, a modification and a save. Without it, workstream C adding a
 * key would silently delete the device manager's, or the other way round.
 */
static void test_unknown_keys_are_preserved(void **state)
{
	(void)state;
	unlink_tmp();
	write_file("ui.theme=dark\n"
		   "audio.capture.id=hw:CARD=CODEC,DEV=0\n"
		   "something.from.the.future=42\n");

	ardop_settings s;
	assert_true(ardop_settings_load(&s, TMP));
	assert_int_equal(s.n, 3);

	assert_true(ardop_settings_set(&s, "audio.capture.id", "hw:CARD=X,DEV=0"));
	assert_true(ardop_settings_save(&s, TMP));

	ardop_settings back;
	assert_true(ardop_settings_load(&back, TMP));
	assert_int_equal(back.n, 3);
	assert_string_equal(ardop_settings_get(&back, "ui.theme", ""), "dark");
	assert_string_equal(ardop_settings_get(&back, "something.from.the.future",
					       ""), "42");
	assert_string_equal(ardop_settings_get(&back, "audio.capture.id", ""),
			    "hw:CARD=X,DEV=0");

	unlink_tmp();
}

/*
 * One bad hand edit must not cost an operator their callsign. The malformed
 * line is skipped and counted; everything around it still loads.
 */
static void test_bad_lines_are_skipped_not_fatal(void **state)
{
	(void)state;
	unlink_tmp();
	write_file("# a comment\n"
		   "\n"
		   "station.mycall=N0CALL\n"
		   "this line has no equals sign\n"
		   "   link.listen  =  true   # trailing comment\n"
		   "=novalue\n"
		   "link.arqbw=500MAX\n");

	ardop_settings s;
	assert_true(ardop_settings_load(&s, TMP));

	assert_int_equal(s.n, 3);
	assert_int_equal(s.skipped, 2);
	assert_string_equal(ardop_settings_get(&s, "station.mycall", ""), "N0CALL");
	assert_string_equal(ardop_settings_get(&s, "link.arqbw", ""), "500MAX");

	/* Whitespace around the key and the value is stripped, and a comment
	 * after a value does not become part of it. */
	assert_true(ardop_settings_get_bool(&s, "link.listen", false));

	unlink_tmp();
}

/* An over-long key or value is refused rather than silently shortened: a
 * truncated device id can never match again. */
static void test_over_long_entries_are_refused(void **state)
{
	(void)state;
	ardop_settings s;
	memset(&s, 0, sizeof s);

	char key[ARDOP_SETTING_KEY_MAX + 8];
	memset(key, 'k', sizeof key - 1);
	key[sizeof key - 1] = '\0';
	assert_false(ardop_settings_set(&s, key, "x"));

	char value[ARDOP_SETTING_VALUE_MAX + 8];
	memset(value, 'v', sizeof value - 1);
	value[sizeof value - 1] = '\0';
	assert_false(ardop_settings_set(&s, "ok", value));

	assert_int_equal(s.n, 0);
}

/*
 * A file with more keys than the store can hold must not be written back --
 * doing so would delete whatever did not fit, silently, and probably somebody
 * else's settings.
 */
static void test_truncated_store_refuses_to_save(void **state)
{
	(void)state;
	unlink_tmp();

	FILE *f = fopen(TMP, "w");
	assert_non_null(f);
	for (int i = 0; i < ARDOP_SETTINGS_MAX + 10; i++)
		fprintf(f, "key.%d=%d\n", i, i);
	fclose(f);

	ardop_settings s;
	assert_true(ardop_settings_load(&s, TMP));
	assert_true(s.truncated);
	assert_int_equal(s.n, ARDOP_SETTINGS_MAX);

	assert_false(ardop_settings_save(&s, TMP));

	/* And the file it refused to overwrite is still whole. */
	ardop_settings back;
	assert_true(ardop_settings_load(&back, TMP));
	assert_true(back.truncated);

	unlink_tmp();
}

/* Replacing a key updates in place rather than appending a second copy. */
static void test_set_replaces_and_unset_removes(void **state)
{
	(void)state;
	ardop_settings s;
	memset(&s, 0, sizeof s);

	assert_true(ardop_settings_set(&s, "a", "1"));
	assert_true(ardop_settings_set(&s, "b", "2"));
	assert_true(ardop_settings_set(&s, "a", "3"));
	assert_int_equal(s.n, 2);
	assert_string_equal(ardop_settings_get(&s, "a", ""), "3");

	assert_true(ardop_settings_unset(&s, "a"));
	assert_int_equal(s.n, 1);
	assert_false(ardop_settings_unset(&s, "a"));
	assert_string_equal(ardop_settings_get(&s, "b", ""), "2");
}

/* A value that is not a number, or not a boolean, falls back rather than
 * becoming 0 or false by accident. */
static void test_typed_reads_reject_nonsense(void **state)
{
	(void)state;
	ardop_settings s;
	memset(&s, 0, sizeof s);
	assert_true(ardop_settings_set(&s, "n", "12x"));
	assert_true(ardop_settings_set(&s, "b", "maybe"));
	assert_true(ardop_settings_set(&s, "empty", ""));

	assert_int_equal(ardop_settings_get_num(&s, "n", -1), -1);
	assert_true(ardop_settings_get_bool(&s, "b", true));
	assert_false(ardop_settings_get_bool(&s, "b", false));
	assert_int_equal(ardop_settings_get_num(&s, "empty", 7), 7);

	/* The spellings that are accepted, case-insensitively. */
	const char *yes[] = {"true", "TRUE", "Yes", "on", "1"};
	const char *no[] = {"false", "FALSE", "No", "off", "0"};
	for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) {
		assert_true(ardop_settings_set(&s, "v", yes[i]));
		assert_true(ardop_settings_get_bool(&s, "v", false));
		assert_true(ardop_settings_set(&s, "v", no[i]));
		assert_false(ardop_settings_get_bool(&s, "v", true));
	}
}

/*
 * The save is atomic: it writes a sibling and replaces. An interrupted save
 * therefore leaves either the old file or the new one, never an empty one --
 * which matters because the thing most likely to be in there is the callsign
 * the station identifies with.
 */
static void test_save_is_atomic(void **state)
{
	(void)state;
	unlink_tmp();

	ardop_settings s;
	memset(&s, 0, sizeof s);
	assert_true(ardop_settings_set(&s, "station.mycall", "N0CALL"));
	assert_true(ardop_settings_save(&s, TMP));

	/* No staging file is left behind. */
	FILE *leftover = fopen(TMP ".new", "r");
	assert_null(leftover);

	char text[512];
	read_file(text, sizeof text);
	assert_non_null(strstr(text, "station.mycall=N0CALL"));
	assert_non_null(strstr(text, "#"));   /* the explanatory header */

	/* Saving over an existing file replaces it -- Win32 rename() will not,
	 * which is why ardop_replace_file exists. */
	assert_true(ardop_settings_set(&s, "station.mycall", "N0AAA"));
	assert_true(ardop_settings_save(&s, TMP));
	read_file(text, sizeof text);
	assert_non_null(strstr(text, "N0AAA"));
	assert_null(strstr(text, "N0CALL-"));

	unlink_tmp();
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_round_trip),
		cmocka_unit_test(test_missing_file_is_success),
		cmocka_unit_test(test_unknown_keys_are_preserved),
		cmocka_unit_test(test_bad_lines_are_skipped_not_fatal),
		cmocka_unit_test(test_over_long_entries_are_refused),
		cmocka_unit_test(test_truncated_store_refuses_to_save),
		cmocka_unit_test(test_set_replaces_and_unset_removes),
		cmocka_unit_test(test_typed_reads_reject_nonsense),
		cmocka_unit_test(test_save_is_atomic),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
