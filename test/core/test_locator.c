#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/locator.h"

/* The original module is the oracle; names do not collide with the port. */
#include "common/Locator.h"

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/*
 * Compare old vs new for one input. Both zero the locator on error, so the grid
 * string and wire bytes can be compared unconditionally.
 */
static void expect_same_locator(const char *s)
{
	/*
	 * Zero both first: a realistic caller passes a locator to be filled,
	 * and the original leaves it untouched on empty input (the port zeros
	 * it instead). With both pre-zeroed the two agree for every input.
	 */
	Locator lo;
	memset(&lo, 0, sizeof(lo));
	locator_err lr = locator_from_str(s, &lo);

	ardop_locator nw;
	memset(&nw, 0, sizeof(nw));
	ardop_locator_err nr = ardop_locator_from_str(s, &nw);

	if ((int)lr != (int)nr)
		fail_msg("from_str \"%s\": legacy err %d, port err %d",
			 s, (int)lr, (int)nr);
	assert_string_equal(lo.grid, nw.grid);
	assert_memory_equal(lo.wire.b, nw.wire.b, PACKED6_SIZE);

	/* Accessors must agree too. */
	assert_int_equal(locator_is_populated(&lo),
			 ardop_locator_is_populated(&nw));
	assert_memory_equal(locator_as_bytes(&lo)->b,
			    ardop_locator_as_bytes(&nw)->b, PACKED6_SIZE);
}

/* Curated grids reaching each field/square/subsquare/extsquare check. */
static void test_from_str_curated(void **state)
{
	(void)state;

	static const char *const inputs[] = {
		"", "AB", "BL", "BL11", "BL11bh", "BL11bh16", "bl11BH16",
		"BL11BH16", "R", "BL1", "ZZ11", "BL99", "BLxx", "BL11YY16",
		"BL11bh99", "TOOLONGGRID", "AA00aa00", "SS11", "AR9X",
		"bl11bh16", "Bl11Bh16", "12", "A1",
	};

	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++)
		expect_same_locator(inputs[i]);
}

/* Random grid-shaped strings to reach paths the curated list misses. */
static void test_from_str_random(void **state)
{
	(void)state;

	static const char charset[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ";
	uint32_t rng = 0x6DA71000u;

	for (int iter = 0; iter < 40000; iter++) {
		char buf[16];
		int len = (int)(xorshift32(&rng) % 11u);  /* 0..10 */
		for (int i = 0; i < len; i++)
			buf[i] = charset[xorshift32(&rng)
					 % (uint32_t)(sizeof(charset) - 1)];
		buf[len] = '\0';
		expect_same_locator(buf);
	}
}

/* The explicit-length entry point. */
static void test_from_str_slice_matches_legacy(void **state)
{
	(void)state;

	static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcx0123456789 ";
	uint32_t rng = 0x10CA7012u;

	for (int iter = 0; iter < 40000; iter++) {
		char buf[24];
		int total = (int)(xorshift32(&rng) % 14u);
		for (int i = 0; i < total; i++)
			buf[i] = charset[xorshift32(&rng)
					 % (uint32_t)(sizeof(charset) - 1)];
		buf[total] = '\0';
		size_t len = (total == 0) ? 0
			   : (size_t)(xorshift32(&rng) % (uint32_t)total);

		Locator lo;
		memset(&lo, 0, sizeof(lo));
		locator_err lr = locator_from_str_slice(buf, len, &lo);
		ardop_locator nw;
		memset(&nw, 0, sizeof(nw));
		ardop_locator_err nr =
			ardop_locator_from_str_slice(buf, len, &nw);

		if ((int)lr != (int)nr)
			fail_msg("from_str_slice \"%s\" len %zu: legacy %d, "
				 "port %d", buf, len, (int)lr, (int)nr);
		assert_string_equal(lo.grid, nw.grid);
		assert_memory_equal(lo.wire.b, nw.wire.b, PACKED6_SIZE);
	}
}

/* Decoding arbitrary wire bytes must agree, including the legacy "no GS" ones. */
static void test_from_bytes_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x9E0C0DE0u;

	for (int iter = 0; iter < 40000; iter++) {
		uint8_t bytes[PACKED6_SIZE];
		for (int i = 0; i < PACKED6_SIZE; i++)
			bytes[i] = (uint8_t)xorshift32(&rng);

		Locator lo;
		locator_err lr = locator_from_bytes(bytes, &lo);
		ardop_locator nw;
		ardop_locator_err nr = ardop_locator_from_bytes(bytes, &nw);

		if ((int)lr != (int)nr)
			fail_msg("from_bytes: legacy err %d, port err %d",
				 (int)lr, (int)nr);
		assert_string_equal(lo.grid, nw.grid);
		assert_memory_equal(lo.wire.b, nw.wire.b, PACKED6_SIZE);
	}

	/* The two legacy "unset grid square" sequences decode to empty, OK. */
	const uint8_t nogs[2][PACKED6_SIZE] = {
		{0xbc, 0xf0, 0x27, 0xcc, 0x00, 0x00},
		{0xba, 0xf0, 0x27, 0xcc, 0x00, 0x00},
	};
	for (int i = 0; i < 2; i++) {
		ardop_locator nw;
		assert_int_equal(ardop_locator_from_bytes(nogs[i], &nw),
				 ARDOP_LOCATOR_OK);
		assert_false(ardop_locator_is_populated(&nw));
	}
}

/*
 * The port's one intentional improvement: empty input fully defines the
 * locator (the original left it untouched). Proven by handing it a non-empty
 * buffer and checking it comes back empty.
 */
static void test_empty_input_zeroes_output(void **state)
{
	(void)state;

	ardop_locator loc;
	memset(&loc, 'Z', sizeof(loc));
	assert_int_equal(ardop_locator_from_str("", &loc), ARDOP_LOCATOR_OK);
	assert_false(ardop_locator_is_populated(&loc));
	assert_string_equal(loc.grid, "");
}

/* strerror must match the original across all codes (no divergence here). */
static void test_strerror(void **state)
{
	(void)state;

	for (int e = 0; e <= ARDOP_LOCATOR_ERR_MAX_ + 2; e++)
		assert_string_equal(locator_strerror((locator_err)e),
				    ardop_locator_strerror((ardop_locator_err)e));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_from_str_curated),
		cmocka_unit_test(test_from_str_random),
		cmocka_unit_test(test_from_str_slice_matches_legacy),
		cmocka_unit_test(test_from_bytes_matches_legacy),
		cmocka_unit_test(test_empty_input_zeroes_output),
		cmocka_unit_test(test_strerror),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
