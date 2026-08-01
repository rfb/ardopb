#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/ring.h"

/*
 * The SPSC ring's single-threaded contract: what it accepts, what it refuses,
 * and that a wrap does not reorder or lose samples.
 *
 * The *concurrent* property -- that the release/acquire pairing actually makes
 * the samples visible before the count advertising them -- is not provable from
 * one thread and is not attempted here. `make test-ring-tsan` builds a
 * two-thread stress under ThreadSanitizer for that, on Linux, because MinGW has
 * no TSan and the file is portable enough that proving it once is enough.
 */

#define CAP 16

static void test_ring_empty_and_full(void **state)
{
	(void)state;
	int16_t store[CAP];
	ardop_ring rg;

	assert_true(ardop_ring_init(&rg, store, CAP));
	assert_int_equal(ardop_ring_avail(&rg), 0);
	assert_int_equal(ardop_ring_space(&rg), CAP);

	int16_t out[CAP];
	assert_int_equal(ardop_ring_read(&rg, out, CAP), 0);

	/* Every slot is usable -- the monotonic counters mean "full" and
	 * "empty" are distinguishable without sacrificing one. */
	int16_t in[CAP];
	for (int i = 0; i < CAP; i++)
		in[i] = (int16_t)(i + 1);
	assert_int_equal(ardop_ring_write(&rg, in, CAP), CAP);
	assert_int_equal(ardop_ring_avail(&rg), CAP);
	assert_int_equal(ardop_ring_space(&rg), 0);

	/* A write against a full ring is a short write, not an overwrite. */
	int16_t extra = 99;
	assert_int_equal(ardop_ring_write(&rg, &extra, 1), 0);

	assert_int_equal(ardop_ring_read(&rg, out, CAP), CAP);
	assert_memory_equal(out, in, sizeof(in));
	assert_int_equal(ardop_ring_avail(&rg), 0);
}

static void test_ring_partial_write_reports_short(void **state)
{
	(void)state;
	int16_t store[CAP];
	ardop_ring rg;
	assert_true(ardop_ring_init(&rg, store, CAP));

	int16_t in[CAP * 2];
	for (int i = 0; i < CAP * 2; i++)
		in[i] = (int16_t)i;

	/* Offering more than fits writes what fits and says so. The audio
	 * callback relies on this: it must never block and must never scribble
	 * past what the consumer has released. */
	assert_int_equal(ardop_ring_write(&rg, in, CAP * 2), CAP);

	int16_t out[CAP];
	assert_int_equal(ardop_ring_read(&rg, out, CAP * 2), CAP);
	assert_memory_equal(out, in, sizeof(out));
}

static void test_ring_wraps_without_losing_order(void **state)
{
	(void)state;
	int16_t store[CAP];
	ardop_ring rg;
	assert_true(ardop_ring_init(&rg, store, CAP));

	/* Drive many times the capacity through in ragged chunks so reads and
	 * writes land at every offset, including split runs across the seam. */
	int16_t next_w = 0, next_r = 0;
	for (int round = 0; round < 500; round++) {
		size_t chunk = (size_t)(round % 7) + 1;

		int16_t in[8];
		for (size_t i = 0; i < chunk; i++)
			in[i] = next_w++;
		size_t wrote = ardop_ring_write(&rg, in, chunk);
		next_w = (int16_t)(next_w - (int16_t)(chunk - wrote));

		int16_t out[8];
		size_t got = ardop_ring_read(&rg, out, (size_t)(round % 5) + 1);
		for (size_t i = 0; i < got; i++)
			assert_int_equal(out[i], next_r++);
	}
}

static void test_ring_reset_discards(void **state)
{
	(void)state;
	int16_t store[CAP];
	ardop_ring rg;
	assert_true(ardop_ring_init(&rg, store, CAP));

	int16_t in[CAP];
	memset(in, 0x5A, sizeof(in));
	assert_int_equal(ardop_ring_write(&rg, in, CAP), CAP);

	/* This is what runs at a PTT edge: the station's own transmission, or
	 * stale playback, must not survive into the next key-up. */
	ardop_ring_reset(&rg);
	assert_int_equal(ardop_ring_avail(&rg), 0);
	assert_int_equal(ardop_ring_space(&rg), CAP);

	int16_t out[CAP];
	assert_int_equal(ardop_ring_read(&rg, out, CAP), 0);
}

static void test_ring_init_rejects_nonsense(void **state)
{
	(void)state;
	int16_t store[CAP];
	ardop_ring rg;
	assert_false(ardop_ring_init(&rg, NULL, CAP));
	assert_false(ardop_ring_init(&rg, store, 0));
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ring_empty_and_full),
		cmocka_unit_test(test_ring_partial_write_reports_short),
		cmocka_unit_test(test_ring_wraps_without_losing_order),
		cmocka_unit_test(test_ring_reset_discards),
		cmocka_unit_test(test_ring_init_rejects_nonsense),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
