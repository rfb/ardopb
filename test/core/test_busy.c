#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/busy.h"

/*
 * The inherited BusyDetect3 is the oracle. It reads its configuration and its
 * clock from globals, and keeps its state in globals; the port takes all three
 * explicitly. Declared here rather than via ARDOPC.h, which drags in the whole
 * protocol world.
 *
 * The clock: Now is getTicks(), which returns WavNow when DecodeWav[0][0] is
 * set. Setting that once makes the oracle read our WavNow as the millisecond
 * clock, so both sides can be driven off the same time.
 */
int BusyDetect3(float *dblMag, int intStart, int intStop);
void ClearBusy(void);
/* The inherited busy front end: the Blackman-Harris window and the FFT. */
void generateBH(void);
extern float bhWindow[];
void FourierTransform(int NumSamples, float *RealIn, float *RealOut,
		      float *ImagOut, int InverseTransform);

enum _ARQBandwidth {
	B200FORCED, B500FORCED, B1000FORCED, B2000FORCED,
	B200MAX, B500MAX, B1000MAX, B2000MAX, UNDEFINED
};
extern enum _ARQBandwidth ARQBandwidth;
extern int BusyDet;

extern int WavNow;
extern char DecodeWav[5][256];

/* Oracle state, for comparing the filter/hysteresis bookkeeping. */
extern int intBusyOnCnt;
extern int intBusyOffCnt;
extern int blnLastBusy;
extern unsigned int dttLastTrip;

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* A bandwidth to drive both sides identically. */
struct bw_pair {
	enum _ARQBandwidth legacy;
	ardop_bandwidth port;
};
static const struct bw_pair kBandwidths[] = {
	{B200MAX, ARDOP_BW_200},
	{B500FORCED, ARDOP_BW_500},
	{B1000MAX, ARDOP_BW_1000},
	{B2000FORCED, ARDOP_BW_2000},
};

/*
 * Drive a long randomised sequence through both implementations and require
 * the reported busy state -- and the hysteresis bookkeeping -- to agree at
 * every step. The spectra, bin range, bandwidth, sensitivity and clock all
 * vary, including occasional ClearBusy resets and near-threshold peaks so the
 * busy/idle transitions and the 5 s hold are actually exercised.
 */
static void test_busy_matches_legacy(void **state)
{
	(void)state;

	/* Route the oracle's getTicks() to WavNow. */
	DecodeWav[0][0] = 'x';

	ardop_busy_detector b = {0};
	/* Both sides start from a known reset at t = 1000 ms. */
	WavNow = 1000;
	ClearBusy();
	ardop_busy_clear(&b, 1000);

	uint32_t rng = 0xB0551E05u;
	uint32_t t = 1000;
	int start = 48, stop = 208;
	int bw_idx = 3, busy_det = 5;

	static float mag[512];

	for (int step = 0; step < 20000; step++) {
		uint32_t r = xorshift32(&rng);

		/* Advance the clock 20..270 ms. */
		t += 20 + (r % 250u);
		WavNow = (int)t;

		/* Occasionally change configuration or reset. */
		if ((r & 0x3Fu) == 0) {
			bw_idx = (int)(xorshift32(&rng) & 3u);
			ARQBandwidth = kBandwidths[bw_idx].legacy;
		}
		if ((r & 0xFFu) == 7) {
			busy_det = (int)(xorshift32(&rng) % 11u);  /* 0..10 */
		}
		if ((r & 0x1FFu) == 11) {
			/* A bandwidth (bin range) change: reinitialises averages. */
			start = 40 + (int)(xorshift32(&rng) % 20u);
			stop = start + 40 + (int)(xorshift32(&rng) % 150u);
		}
		if ((r & 0x3FFu) == 13) {
			ClearBusy();
			ardop_busy_clear(&b, t);
			continue;
		}

		ARQBandwidth = kBandwidths[bw_idx].legacy;
		BusyDet = busy_det;

		/* Build a spectrum: a noise floor with occasional peaks. */
		int noise = 20 + (int)(xorshift32(&rng) % 200u);
		for (int i = 0; i < 512; i++)
			mag[i] = (float)(xorshift32(&rng) % (uint32_t)noise);
		if (xorshift32(&rng) & 1u) {
			/* Add a signal peak inside the search range. */
			int peak = start + (int)(xorshift32(&rng)
						 % (uint32_t)(stop - start + 1));
			float amp = (float)(xorshift32(&rng) % 30000u);
			mag[peak] += amp;
			if (peak + 1 <= stop)
				mag[peak + 1] += amp * 0.5f;
		}

		int oracle = BusyDetect3(mag, start, stop);
		bool port = ardop_busy_detect(&b, mag, start, stop,
					      kBandwidths[bw_idx].port, busy_det,
					      t);

		if ((oracle != 0) != port)
			fail_msg("step %d: busy legacy=%d port=%d "
				 "(bw=%d det=%d start=%d stop=%d t=%u)",
				 step, oracle, port, bw_idx, busy_det, start,
				 stop, t);
		assert_int_equal(b.busy_on_count, intBusyOnCnt);
		assert_int_equal(b.busy_off_count, intBusyOffCnt);
		assert_int_equal(b.last_busy ? 1 : 0, blnLastBusy ? 1 : 0);
		assert_int_equal((int)b.last_trip, (int)dttLastTrip);
	}
}

/* The rebuilt window must match generateBH's bhWindow bit-for-bit. */
static void test_busy_window_matches_legacy(void **state)
{
	(void)state;
	generateBH();
	float w[513];
	ardop_busy_window(w);
	for (int i = 0; i < 513; i++)
		assert_memory_equal(&w[i], &bhWindow[i], sizeof(float));
}

/*
 * The whole busy front end -- window, FFT, magnitude spectrum, tuning-line
 * range -- must agree with the inherited UpdateBusyDetector path. The reference
 * builds the spectrum with generateBH's bhWindow and the original
 * FourierTransform, applies the same bin range, and calls the oracle
 * BusyDetect3; ardop_busy_analyze does all of that internally. Both are driven
 * off the same clock and their busy decision must match at every 1024-sample
 * window.
 */
static void test_busy_analyze_matches_legacy(void **state)
{
	(void)state;

	DecodeWav[0][0] = 'x';   /* route getTicks() to WavNow. */
	generateBH();
	float w[513];
	ardop_busy_window(w);

	ardop_busy_detector b = {0};
	WavNow = 1000;
	ClearBusy();
	ardop_busy_clear(&b, 1000);

	const int bw_hz = 2000, tuning_range = 100, busy_det = 5;
	ARQBandwidth = B2000MAX;
	BusyDet = busy_det;

	/* The same tuning-line range the port computes, for the reference. */
	int delta = (int)((float)(bw_hz / 2 + tuning_range) / 11.719f);
	int low = 103 - delta, high = 103 + delta;
	if (low < 3) low = 3;
	if (high > 203) high = 203;

	uint32_t rng = 0x1234ABCDu;
	uint32_t t = 1000;

	static int16_t samples[1024];
	static float windowed[1024], re[1024], im[1024], mag[206];

	for (int step = 0; step < 400; step++) {
		t += 100;
		WavNow = (int)t;

		/* A noisy window, sometimes carrying an in-band tone so busy
		 * actually trips and clears over the run. */
		int noise = 200 + (int)(xorshift32(&rng) % 400u);
		for (int i = 0; i < 1024; i++)
			samples[i] = (int16_t)((int)(xorshift32(&rng)
						      % (uint32_t)noise) - noise / 2);
		if (xorshift32(&rng) & 1u) {
			double freq = 800.0 + (xorshift32(&rng) % 1200u);
			double amp = 3000.0 + (xorshift32(&rng) % 8000u);
			for (int i = 0; i < 1024; i++)
				samples[i] = (int16_t)(samples[i]
					+ (int)(amp * sin(2.0 * 3.14159265
							  * freq * i / 12000.0)));
		}

		/* Reference: the inherited front end + oracle detector. */
		windowed[0] = (float)samples[0] * bhWindow[0];
		windowed[512] = (float)samples[512] * bhWindow[512];
		for (int i = 1; i < 512; i++) {
			windowed[i] = (float)samples[i] * bhWindow[i];
			windowed[1024 - i] = (float)samples[1024 - i] * bhWindow[i];
		}
		FourierTransform(1024, windowed, re, im, 0);
		for (int i = 0; i < 206; i++)
			mag[i] = powf(re[i + 25], 2) + powf(im[i + 25], 2);
		int oracle = BusyDetect3(mag, low, high);

		bool port = ardop_busy_analyze(&b, w, samples, bw_hz,
					       ARDOP_BW_2000, tuning_range,
					       busy_det, t);

		if ((oracle != 0) != port)
			fail_msg("step %d: analyze legacy=%d port=%d t=%u",
				 step, oracle, port, t);
		assert_int_equal(b.busy_on_count, intBusyOnCnt);
		assert_int_equal(b.last_busy ? 1 : 0, blnLastBusy ? 1 : 0);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_busy_matches_legacy),
		cmocka_unit_test(test_busy_window_matches_legacy),
		cmocka_unit_test(test_busy_analyze_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
