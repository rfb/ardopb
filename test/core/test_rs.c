#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/rs.h"

/*
 * The original global-state implementation is the oracle. Its three functions
 * come from its header; its Galois-field and generator tables are file-scope
 * globals we extern here to compare directly -- the strongest possible check
 * that the field construction was ported faithfully. New names (ardop_rs_*,
 * ardop_rs) do not collide with the old ones, so both coexist in this test.
 */
#include "rockliff/rrs.h"

extern int alpha_to[ARDOP_RS_SYMBOLS + 1];
extern int index_of[ARDOP_RS_SYMBOLS + 1];
extern int gg[ARDOP_RS_MAX_COUNT][ARDOP_RS_MAX_RSLEN + 1];
extern int rslen_set[ARDOP_RS_MAX_COUNT];
extern int rslen_count;

/* The parity lengths ardopcf actually uses (ALSASound.c / Waveout.c). */
static int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof(kRSLens) / sizeof(kRSLens[0])))

/* A single context, initialised once for the whole suite. */
static ardop_rs g_rs;

static int group_setup(void **state)
{
	(void)state;
	/* Both implementations get the identical length set. */
	assert_true(ardop_rs_init(&g_rs, kRSLens, NUM_RSLENS));
	assert_int_equal(init_rs(kRSLens, NUM_RSLENS), 0);
	return 0;
}

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
 * The generated tables must be byte-identical to the original's globals. If the
 * field or the generator polynomials were built even slightly differently,
 * every encode and decode below would be suspect; checking the tables outright
 * localises such a fault to its source.
 */
static void test_tables_match_legacy(void **state)
{
	(void)state;

	assert_int_equal(g_rs.rslen_count, rslen_count);
	assert_memory_equal(g_rs.alpha_to, alpha_to, sizeof(alpha_to));
	assert_memory_equal(g_rs.index_of, index_of, sizeof(index_of));
	assert_memory_equal(g_rs.rslen_set, rslen_set, sizeof(rslen_set));
	assert_memory_equal(g_rs.gg, gg, sizeof(gg));
}

/*
 * Appending parity to random messages of every length, at every parity length,
 * must produce the identical buffer and return value as the original.
 */
static void test_append_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xA5A5F00Du;

	for (int iter = 0; iter < 4000; iter++) {
		int rslen = kRSLens[xorshift32(&rng) % (uint32_t)NUM_RSLENS];
		int maxdata = ARDOP_RS_SYMBOLS - rslen;
		int datalen = 1 + (int)(xorshift32(&rng) % (uint32_t)maxdata);

		uint8_t legacy[ARDOP_RS_SYMBOLS];
		uint8_t mine[ARDOP_RS_SYMBOLS];
		for (int i = 0; i < datalen; i++) {
			uint8_t b = (uint8_t)xorshift32(&rng);
			legacy[i] = b;
			mine[i] = b;
		}

		int lret = rs_append(legacy, datalen, rslen);
		int mret = ardop_rs_append(&g_rs, mine, datalen, rslen);

		if (lret != mret
		    || memcmp(legacy, mine, (size_t)(datalen + rslen))) {
			fail_msg("append datalen %d rslen %d: legacy ret %d, "
				 "port ret %d, bytes differ", datalen, rslen,
				 lret, mret);
		}
	}
}

/*
 * The decoder is where the algorithm's complexity lives. For every parity
 * length, corrupt a freshly encoded block with error counts spanning all of the
 * decoder's paths -- none, within the correction budget, and beyond it -- and
 * require the port to make the identical decision and produce the identical
 * bytes as the original, in both correcting and test-only modes.
 */
static void test_correct_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x1BADB002u;

	for (int iter = 0; iter < 8000; iter++) {
		int rslen = kRSLens[xorshift32(&rng) % (uint32_t)NUM_RSLENS];
		int maxdata = ARDOP_RS_SYMBOLS - rslen;
		int datalen = 1 + (int)(xorshift32(&rng) % (uint32_t)maxdata);
		int combinedlen = datalen + rslen;

		/* Build a valid codeword with the (already-proven) encoder. */
		uint8_t block[ARDOP_RS_SYMBOLS];
		for (int i = 0; i < datalen; i++)
			block[i] = (uint8_t)xorshift32(&rng);
		assert_int_equal(ardop_rs_append(&g_rs, block, datalen, rslen), 0);

		/*
		 * Introduce 0..rslen errors: straddles the rslen/2 correction
		 * limit so the corrected, uncorrectable and false-correction
		 * paths all run.
		 */
		int nerr = (int)(xorshift32(&rng) % (uint32_t)(rslen + 1));
		uint8_t corrupt[ARDOP_RS_SYMBOLS];
		memcpy(corrupt, block, (size_t)combinedlen);
		for (int e = 0; e < nerr; e++) {
			int pos = (int)(xorshift32(&rng) % (uint32_t)combinedlen);
			uint8_t flip = (uint8_t)(1u + xorshift32(&rng) % 255u);
			corrupt[pos] = (uint8_t)(corrupt[pos] ^ flip);
		}

		/* Correcting mode. */
		uint8_t legacy[ARDOP_RS_SYMBOLS];
		uint8_t mine[ARDOP_RS_SYMBOLS];
		memcpy(legacy, corrupt, (size_t)combinedlen);
		memcpy(mine, corrupt, (size_t)combinedlen);

		int lret = rs_correct(legacy, combinedlen, rslen, true, false);
		int mret = ardop_rs_correct(&g_rs, mine, combinedlen, rslen,
					    false);

		if (lret != mret
		    || memcmp(legacy, mine, (size_t)combinedlen)) {
			fail_msg("correct datalen %d rslen %d nerr %d: legacy "
				 "ret %d, port ret %d, bytes differ", datalen,
				 rslen, nerr, lret, mret);
		}

		/* Test-only mode: same decision, buffer left untouched. */
		uint8_t lt[ARDOP_RS_SYMBOLS];
		uint8_t mt[ARDOP_RS_SYMBOLS];
		memcpy(lt, corrupt, (size_t)combinedlen);
		memcpy(mt, corrupt, (size_t)combinedlen);

		int ltret = rs_correct(lt, combinedlen, rslen, true, true);
		int mtret = ardop_rs_correct(&g_rs, mt, combinedlen, rslen, true);

		if (ltret != mtret
		    || memcmp(mt, corrupt, (size_t)combinedlen)) {
			fail_msg("correct test_only datalen %d rslen %d nerr "
				 "%d: legacy ret %d, port ret %d", datalen,
				 rslen, nerr, ltret, mtret);
		}
	}
}

/*
 * Oracle-independent guarantee: any error pattern within the correction budget
 * (rslen/2 bytes) must be repaired back to the exact original message. This
 * would catch a fault the port shares with the original.
 */
static void test_within_budget_always_corrects(void **state)
{
	(void)state;

	uint32_t rng = 0xC0DEC0DEu;

	for (int iter = 0; iter < 4000; iter++) {
		int rslen = kRSLens[xorshift32(&rng) % (uint32_t)NUM_RSLENS];
		int maxdata = ARDOP_RS_SYMBOLS - rslen;
		int datalen = 1 + (int)(xorshift32(&rng) % (uint32_t)maxdata);
		int combinedlen = datalen + rslen;

		uint8_t original[ARDOP_RS_SYMBOLS];
		for (int i = 0; i < datalen; i++)
			original[i] = (uint8_t)xorshift32(&rng);

		uint8_t block[ARDOP_RS_SYMBOLS];
		memcpy(block, original, (size_t)datalen);
		assert_int_equal(ardop_rs_append(&g_rs, block, datalen, rslen), 0);

		/* At most rslen/2 errors, at distinct positions. */
		int budget = rslen / 2;
		int nerr = (int)(xorshift32(&rng) % (uint32_t)(budget + 1));
		uint8_t used[ARDOP_RS_SYMBOLS] = {0};
		for (int e = 0; e < nerr; e++) {
			int pos;
			do {
				pos = (int)(xorshift32(&rng)
					    % (uint32_t)combinedlen);
			} while (used[pos]);
			used[pos] = 1;
			uint8_t flip = (uint8_t)(1u + xorshift32(&rng) % 255u);
			block[pos] = (uint8_t)(block[pos] ^ flip);
		}

		int ret = ardop_rs_correct(&g_rs, block, combinedlen, rslen,
					   false);
		assert_true(ret >= 0);  /* never reports uncorrectable */
		assert_memory_equal(block, original, (size_t)datalen);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_tables_match_legacy),
		cmocka_unit_test(test_append_matches_legacy),
		cmocka_unit_test(test_correct_matches_legacy),
		cmocka_unit_test(test_within_budget_always_corrects),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, group_setup, NULL);
}
