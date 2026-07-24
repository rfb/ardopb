#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/modulate.h"

/*
 * Golden modulator vectors: each frame's encoded bytes plus the expected shape
 * of the audio the inherited modulator produces -- the sample count and an
 * FNV-1a-64 of the little-endian int16 samples.
 *
 * These are NOT hashes of this module's own output. They were captured from the
 * reference ardopcf binary (via the test/golden harness, DRIVELEVEL 30, 240 ms
 * leader), and this module was proven to reproduce every sample of the entire
 * golden corpus -- all 22 control frames and all 108 data cases -- before the
 * representative subset here was frozen. A mismatch means the port has drifted
 * from the on-air waveform, the same guarantee the WAV golden vectors give, in a
 * form a C unit test can check without audio tooling.
 *
 * The set spans every modulation the port implements: 4FSK 50 baud (control
 * frames), 4FSK 600 baud, and 4PSK/8PSK/16QAM at 1, 2 and 4 carriers. 4PSK at
 * two carriers is included specifically because it is the case that exposed the
 * float-vs-double scaling bug during bring-up.
 */
#include "modulate_vectors.h"

#define NUM_VECTORS ((int)(sizeof(MOD_VECTORS) / sizeof(MOD_VECTORS[0])))

/* FNV-1a over the little-endian byte image of the samples (matches the vectors,
 * which were hashed the same way, independent of host endianness). */
static uint64_t fnv1a_samples(const int16_t *s, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < n; i++) {
		uint8_t lo = (uint8_t)(s[i] & 0xff);
		uint8_t hi = (uint8_t)((uint16_t)s[i] >> 8);
		h = (h ^ lo) * 0x100000001b3ULL;
		h = (h ^ hi) * 0x100000001b3ULL;
	}
	return h;
}

static int16_t g_buf[ARDOP_MOD_MAX_SAMPLES];  /* frame storage for begin() */
static int16_t g_out[ARDOP_MOD_MAX_SAMPLES];  /* pull() destination */

/*
 * The core guarantee: for every vector, the modulator emits exactly the samples
 * ardopcf would, drained in one pull.
 */
static void test_vectors_are_byte_exact(void **state)
{
	(void)state;

	for (int v = 0; v < NUM_VECTORS; v++) {
		const struct mod_vector *mv = &MOD_VECTORS[v];
		ardop_mod m;
		ardop_mod_init(&m, 30);

		assert_true(ardop_mod_begin(&m, mv->frame_type, mv->encoded,
					    mv->encoded_len, 240, g_buf,
					    ARDOP_MOD_MAX_SAMPLES));

		size_t n = ardop_mod_pull(&m, g_out, ARDOP_MOD_MAX_SAMPLES);
		if (n != mv->expected_samples)
			fail_msg("%s: %zu samples, expected %zu", mv->name, n,
				 mv->expected_samples);

		uint64_t h = fnv1a_samples(g_out, n);
		if (h != mv->expected_fnv)
			fail_msg("%s: sample hash 0x%016llx, expected 0x%016llx"
				 " -- modulator output no longer matches ardopcf",
				 mv->name, (unsigned long long)h,
				 (unsigned long long)mv->expected_fnv);

		assert_false(ardop_mod_busy(&m));
	}
}

/*
 * Pulling in small chunks must yield exactly the same stream as one big pull --
 * the drain boundary is not allowed to affect the samples.
 */
static void test_chunked_pull_matches(void **state)
{
	(void)state;

	const struct mod_vector *mv = &MOD_VECTORS[NUM_VECTORS - 1];  /* 600 baud */

	ardop_mod m;
	ardop_mod_init(&m, 30);
	assert_true(ardop_mod_begin(&m, mv->frame_type, mv->encoded,
				    mv->encoded_len, 240, g_buf,
				    ARDOP_MOD_MAX_SAMPLES));

	/* Drain 137 samples at a time into a fresh buffer. */
	static int16_t chunked[ARDOP_MOD_MAX_SAMPLES];
	size_t total = 0, n;
	while ((n = ardop_mod_pull(&m, &chunked[total], 137)) > 0)
		total += n;

	assert_int_equal(total, mv->expected_samples);
	assert_int_equal(fnv1a_samples(chunked, total), mv->expected_fnv);
}

/* Drive level scales the output, so a different level must change the hash (and
 * a zero-length pull past the end returns nothing). */
static void test_drive_level_changes_output(void **state)
{
	(void)state;

	const struct mod_vector *mv = &MOD_VECTORS[0];  /* BREAK */

	ardop_mod m;
	ardop_mod_init(&m, 60);  /* not 30 */
	assert_true(ardop_mod_begin(&m, mv->frame_type, mv->encoded,
				    mv->encoded_len, 240, g_buf,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t n = ardop_mod_pull(&m, g_out, ARDOP_MOD_MAX_SAMPLES);
	assert_int_equal(n, mv->expected_samples);  /* count is drive-independent */
	assert_int_not_equal(fnv1a_samples(g_out, n), mv->expected_fnv);

	/* Fully drained: further pulls yield nothing. */
	assert_int_equal(ardop_mod_pull(&m, g_out, 100), 0);
}

/* begin() must reject what it cannot faithfully produce. */
static void test_begin_rejects(void **state)
{
	(void)state;

	uint8_t enc[24] = { 0 };
	ardop_mod m;
	ardop_mod_init(&m, 30);

	/* An invalid frame type (a gap in the table). */
	enc[0] = 0x20;
	assert_false(ardop_mod_begin(&m, 0x20, enc, 2, 240, g_buf,
				     ARDOP_MOD_MAX_SAMPLES));

	/* A buffer too small for the frame. */
	enc[0] = 0x23;
	enc[1] = 0x23;
	static int16_t tiny[16];
	assert_false(ardop_mod_begin(&m, 0x23, enc, 2, 240, tiny,
				     sizeof(tiny) / sizeof(tiny[0])));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_vectors_are_byte_exact),
		cmocka_unit_test(test_chunked_pull_matches),
		cmocka_unit_test(test_drive_level_changes_output),
		cmocka_unit_test(test_begin_rejects),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
