#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <math.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/goertzel.h"

/*
 * The inherited demodulator's Goertzel and peak-locator functions are the
 * oracle. Declared locally rather than via SoundInput.c's headers, which drag
 * in the whole receive-state world. They take `short *`, which is layout- and
 * call-compatible with the int16_t the port uses.
 */
void GoertzelRealImag(short *in, int ptr, int n, float m, float *re, float *im);
void GoertzelRealImagHanning(short *in, int ptr, int n, float m,
			     float *re, float *im);
void GoertzelRealImagHamming(short *in, int ptr, int n, float m,
			     float *re, float *im);
float SpectralPeakLocator(float km1re, float km1im, float kre, float kim,
			  float kp1re, float kp1im, float *centmag);

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* Bit-identical floats, not just close: a DSP port that drifts by an LSB
 * desynchronises from the transmitter, so equality is the bar. */
static void assert_same(const char *what, int n, float m, float lre, float lim,
			float re, float im)
{
	if (memcmp(&lre, &re, sizeof(float)) != 0
	    || memcmp(&lim, &im, sizeof(float)) != 0)
		fail_msg("%s n=%d m=%.4f: legacy (%.9g, %.9g), port (%.9g, %.9g)",
			 what, n, (double)m, (double)lre, (double)lim,
			 (double)re, (double)im);
}

/* A window of random samples spanning the full int16 range. */
static void fill_random(short *buf, int count, uint32_t *rng)
{
	for (int i = 0; i < count; i++)
		buf[i] = (short)xorshift32(rng);
}

/*
 * The plain Goertzel must match the original for every combination of length,
 * bin (including non-integer bins) and offset into the buffer.
 */
static void test_goertzel_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x9E3779B9u;
	static short buf[2048];
	fill_random(buf, 2048, &rng);

	const int lens[] = { 8, 15, 32, 64, 120, 240, 480, 1024 };
	int cases = 0;

	for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		int n = lens[li];
		for (int t = 0; t < 40; t++) {
			int ptr = (int)(xorshift32(&rng) % (uint32_t)(2048 - n));
			/* Bins from 0 up, including fractional ones. */
			float m = (float)(xorshift32(&rng) % (uint32_t)(n * 4))
				  / 4.0f;

			float lre, lim, re, im;
			GoertzelRealImag(buf, ptr, n, m, &lre, &lim);
			ardop_goertzel(buf, ptr, n, m, &re, &im);
			assert_same("goertzel", n, m, lre, lim, re, im);
			cases++;
		}
	}
	assert_int_equal(cases, 320);
}

static void test_hanning_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x1234ABCDu;
	static short buf[512];
	fill_random(buf, 512, &rng);

	/* Hanning window buffer in the original is 120 long. */
	const int lens[] = { 16, 32, 60, 100, 120 };

	for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		int n = lens[li];
		for (int t = 0; t < 40; t++) {
			int ptr = (int)(xorshift32(&rng) % (uint32_t)(512 - n));
			float m = (float)(xorshift32(&rng) % (uint32_t)(n * 4))
				  / 4.0f;

			float lre, lim, re, im;
			GoertzelRealImagHanning(buf, ptr, n, m, &lre, &lim);
			ardop_goertzel_hanning(buf, ptr, n, m, &re, &im);
			assert_same("hanning", n, m, lre, lim, re, im);
		}
	}
}

static void test_hamming_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x55AA1234u;
	static short buf[2048];
	fill_random(buf, 2048, &rng);

	const int lens[] = { 16, 64, 240, 600, 1024, 1200 };

	for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		int n = lens[li];
		for (int t = 0; t < 30; t++) {
			int ptr = (int)(xorshift32(&rng) % (uint32_t)(2048 - n));
			float m = (float)(xorshift32(&rng) % (uint32_t)(n * 4))
				  / 4.0f;

			float lre, lim, re, im;
			GoertzelRealImagHamming(buf, ptr, n, m, &lre, &lim);
			ardop_goertzel_hamming(buf, ptr, n, m, &re, &im);
			assert_same("hamming", n, m, lre, lim, re, im);
		}
	}
}

/*
 * The original caches each window keyed only on N, and recomputes when N
 * changes. Calling with alternating N proves the port -- which recomputes every
 * call -- agrees with the original's cache-refresh path as well as its hit path.
 */
static void test_windows_agree_when_n_alternates(void **state)
{
	(void)state;

	uint32_t rng = 0xFEEDFACEu;
	static short buf[512];
	fill_random(buf, 512, &rng);

	const int seq[] = { 32, 100, 32, 60, 100, 60 };
	for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		int n = seq[i];
		float lre, lim, re, im;
		GoertzelRealImagHanning(buf, 0, n, 7.5f, &lre, &lim);
		ardop_goertzel_hanning(buf, 0, n, 7.5f, &re, &im);
		assert_same("hanning-alt", n, 7.5f, lre, lim, re, im);
	}
}

static void test_peak_locator_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xC0DE1234u;
	for (int t = 0; t < 5000; t++) {
		float v[6];
		for (int i = 0; i < 6; i++)
			v[i] = (float)((int)(xorshift32(&rng) % 20000u) - 10000)
			       / 100.0f;

		float lmag, mag;
		float lo = SpectralPeakLocator(v[0], v[1], v[2], v[3], v[4],
					       v[5], &lmag);
		float mo = ardop_spectral_peak_locator(v[0], v[1], v[2], v[3],
						       v[4], v[5], &mag);

		if (memcmp(&lo, &mo, sizeof(float)) != 0
		    || memcmp(&lmag, &mag, sizeof(float)) != 0)
			fail_msg("peak %d: legacy (%.9g,%.9g) port (%.9g,%.9g)",
				 t, (double)lo, (double)lmag, (double)mo,
				 (double)mag);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_goertzel_matches_legacy),
		cmocka_unit_test(test_hanning_matches_legacy),
		cmocka_unit_test(test_hamming_matches_legacy),
		cmocka_unit_test(test_windows_agree_when_n_alternates),
		cmocka_unit_test(test_peak_locator_matches_legacy),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
