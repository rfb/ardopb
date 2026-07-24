#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/packed6.h"

/*
 * The original module is the oracle. Its public API shares no names with the
 * ardop_ port -- different type (Packed6 vs ardop_packed6), functions and
 * macros, different include guard -- so both headers coexist here, and the test
 * goes away with the old module.
 */
#include "common/Packed6.h"

/* A small reproducible PRNG so the corpus is identical on every run. */
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
 * Packing a NUL-terminated string must produce the same six bytes and the same
 * success flag as the original, for a wide corpus that exercises padding,
 * lowercase folding, out-of-alphabet characters and over-length truncation.
 */
static void test_from_str_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xBEEF01u;
	int cases = 0;

	for (int iter = 0; iter < 20000; iter++) {
		/* 0..10 chars so we straddle the 8-char limit. */
		int len = (int)(xorshift32(&rng) % 11u);
		char str[16];
		for (int i = 0; i < len; i++) {
			/*
			 * Bias toward the alphabet but include bytes outside it
			 * (and lowercase) so the failure and folding paths run.
			 * Never NUL, so the string length is what we intend.
			 */
			uint8_t b = (uint8_t)(0x20u + xorshift32(&rng) % 0x60u);
			str[i] = (char)b;
		}
		str[len] = '\0';

		Packed6 legacy;
		bool lok = packed6_from_str(str, &legacy);

		ardop_packed6 mine;
		bool mok = ardop_packed6_from_str(str, &mine);

		if (lok != mok || memcmp(legacy.b, mine.b, ARDOP_PACKED6_SIZE)) {
			fail_msg("from_str \"%s\": legacy ok=%d bytes %02X.., "
				 "port ok=%d bytes %02X..", str, lok,
				 legacy.b[0], mok, mine.b[0]);
		}
		cases++;
	}

	assert_int_equal(cases, 20000);
}

/*
 * The explicit-length entry point, including lengths past the 8-char limit,
 * must also agree byte-for-byte and flag-for-flag.
 */
static void test_from_str_slice_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x5EED22u;

	for (int iter = 0; iter < 20000; iter++) {
		int len = (int)(xorshift32(&rng) % 12u);  /* 0..11 */
		char str[16];
		for (int i = 0; i < len; i++) {
			uint8_t b = (uint8_t)(0x20u + xorshift32(&rng) % 0x60u);
			str[i] = (char)b;
		}
		str[len] = '\0';

		Packed6 legacy;
		bool lok = packed6_from_str_slice(str, (size_t)len, &legacy);

		ardop_packed6 mine;
		bool mok = ardop_packed6_from_str_slice(str, (size_t)len, &mine);

		if (lok != mok || memcmp(legacy.b, mine.b, ARDOP_PACKED6_SIZE)) {
			fail_msg("from_str_slice len %d: legacy ok=%d, port "
				 "ok=%d, bytes differ=%d", len, lok, mok,
				 memcmp(legacy.b, mine.b, ARDOP_PACKED6_SIZE));
		}
	}
}

/*
 * Unpacking any six bytes must yield the same eight characters as the original,
 * and the sized variant must make the same accept/reject decision on buffer
 * capacity.
 */
static void test_unpack_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x0DDBA11u;

	for (int iter = 0; iter < 20000; iter++) {
		uint8_t bytes[ARDOP_PACKED6_SIZE];
		for (int i = 0; i < ARDOP_PACKED6_SIZE; i++)
			bytes[i] = (uint8_t)xorshift32(&rng);

		Packed6 legacy;
		packed6_from_bytes(bytes, &legacy);
		char lout[ARDOP_PACKED6_MAX + 1];
		packed6_to_fixed_str(&legacy, lout);

		ardop_packed6 mine;
		ardop_packed6_from_bytes(bytes, &mine);
		char mout[ARDOP_PACKED6_MAX + 1];
		ardop_packed6_to_fixed_str(&mine, mout);

		if (strcmp(lout, mout) != 0)
			fail_msg("to_fixed_str: legacy \"%s\", port \"%s\"",
				 lout, mout);

		/* The sized variant across too-small, exact and roomy buffers. */
		for (size_t cap = 0; cap <= ARDOP_PACKED6_MAX + 2; cap++) {
			char lbuf[ARDOP_PACKED6_MAX + 4];
			char mbuf[ARDOP_PACKED6_MAX + 4];
			memset(lbuf, '#', sizeof(lbuf));
			memset(mbuf, '#', sizeof(mbuf));

			bool lr = packed6_to_str(&legacy, lbuf, cap);
			bool mr = ardop_packed6_to_str(&mine, mbuf, cap);

			if (lr != mr || memcmp(lbuf, mbuf, sizeof(lbuf)))
				fail_msg("to_str cap %zu: legacy ok=%d, port "
					 "ok=%d", cap, lr, mr);
		}
	}
}

/*
 * Oracle-independent properties, so a fault shared with the original would
 * still be caught: a value packed from an already-normalised string unpacks
 * back to that string, and lowercase folds to uppercase.
 */
static void test_roundtrip_and_folding(void **state)
{
	(void)state;

	/* Already uppercase, exactly 8 chars: pack/unpack is the identity. */
	const char *normalised = "N0CALL-7";
	ardop_packed6 p;
	assert_true(ardop_packed6_from_str(normalised, &p));

	char out[ARDOP_PACKED6_MAX + 1];
	ardop_packed6_to_fixed_str(&p, out);
	assert_string_equal(out, normalised);

	/* Short input is space-padded to 8 on the way back out. */
	assert_true(ardop_packed6_from_str("W1AW", &p));
	ardop_packed6_to_fixed_str(&p, out);
	assert_string_equal(out, "W1AW    ");

	/* Lowercase folds to uppercase. */
	ardop_packed6 lower, upper;
	assert_true(ardop_packed6_from_str("w1aw", &lower));
	assert_true(ardop_packed6_from_str("W1AW", &upper));
	assert_memory_equal(lower.b, upper.b, ARDOP_PACKED6_SIZE);

	/* A character outside the alphabet fails but still produces output. */
	ardop_packed6 bad;
	assert_false(ardop_packed6_from_str("W1AW\x7F", &bad));

	/* Over-length input is rejected. */
	assert_false(ardop_packed6_from_str("TOOLONG12", &p));

	/* NULL is treated as empty and succeeds. */
	assert_true(ardop_packed6_from_str(NULL, &p));
	ardop_packed6_to_fixed_str(&p, out);
	assert_string_equal(out, "        ");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_from_str_matches_legacy),
		cmocka_unit_test(test_from_str_slice_matches_legacy),
		cmocka_unit_test(test_unpack_matches_legacy),
		cmocka_unit_test(test_roundtrip_and_folding),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
