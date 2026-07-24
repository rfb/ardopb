#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/modulate.h"

/*
 * Golden modulator vectors. Each is one frame's encoded bytes and the expected
 * shape of the audio the inherited modulator produces for it: the sample count,
 * and an FNV-1a-64 hash of the little-endian int16 samples.
 *
 * These are NOT hashes of this module's own output. They were captured from the
 * reference ardopcf binary (via the test/golden harness, DRIVELEVEL 30, default
 * 240 ms leader) and this module was proven to reproduce every sample exactly
 * before they were frozen here. So a mismatch means the port has drifted from
 * the on-air waveform -- the same guarantee the WAV golden vectors give, in a
 * form a C unit test can check without audio tooling.
 *
 * Encoded bytes were produced by the inherited encoders for these host frames:
 *   BREAK/IDLE/DISC/END  Encode4FSKControl(<type>, session 0)
 *   DataNAK              EncodeDATANAK(quality 50, session 0xFF)   -> "06 f9"
 *   IDFrame              Encode4FSKIDFrame("n0call", "dm")
 * All are 4FSK at 50 baud, the modulation implemented so far.
 */
static const struct mod_vector {
	const char *name;
	uint8_t frame_type;
	uint8_t encoded[24];
	size_t encoded_len;
	size_t expected_samples;
	uint64_t expected_fnv;
} MOD_VECTORS[] = {
	{ "BREAK", 0x23, { 0x23, 0x23 }, 2, 5580, 0xdf2728f66f1ff01dULL },
	{ "IDLE", 0x24, { 0x24, 0x24 }, 2, 5580, 0x63d1a6725a1a401aULL },
	{ "DISC", 0x29, { 0x29, 0x29 }, 2, 5580, 0xa977c0439deea78fULL },
	{ "END", 0x2c, { 0x2c, 0x2c }, 2, 5580, 0x584e9caf98eda9d9ULL },
	{ "DataNAK", 0x06, { 0x06, 0xf9 }, 2, 5580, 0x8957ffc0220a7995ULL },
	{ "IDFrame", 0x30, { 0x30, 0xcf, 0xb9, 0x08, 0xe1, 0xb2, 0xc0, 0x10, 0x92,
			     0xd0, 0x00, 0x00, 0x00, 0x00, 0x90, 0xac, 0x36,
			     0x32 }, 18, 20940, 0x425ac8d080eabb73ULL },
};
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

static int16_t g_buf[ARDOP_MOD_MAX_SAMPLES];

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

		size_t n = ardop_mod_pull(&m, g_buf, ARDOP_MOD_MAX_SAMPLES);
		if (n != mv->expected_samples)
			fail_msg("%s: %zu samples, expected %zu", mv->name, n,
				 mv->expected_samples);

		uint64_t h = fnv1a_samples(g_buf, n);
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

	const struct mod_vector *mv = &MOD_VECTORS[NUM_VECTORS - 1];  /* IDFrame */

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
	size_t n = ardop_mod_pull(&m, g_buf, ARDOP_MOD_MAX_SAMPLES);
	assert_int_equal(n, mv->expected_samples);  /* count is drive-independent */
	assert_int_not_equal(fnv1a_samples(g_buf, n), mv->expected_fnv);

	/* Fully drained: further pulls yield nothing. */
	assert_int_equal(ardop_mod_pull(&m, g_buf, 100), 0);
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
