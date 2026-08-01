#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/resample.h"

/*
 * The properties that make integer decimation safe to put in front of the
 * demodulator. The interesting one is the anti-alias assertion: it is stated as
 * a number in dB rather than "the filter exists", because a filter that is
 * present but too shallow fails silently -- it shows up as a worse decode rate
 * on resampling platforms, never as a crash.
 */

#define RATE_12K 12000.0
#define AMP 12000.0

/* Fill `n` samples of a sine at `hz`, sampled at `fs`. */
static void tone(int16_t *buf, size_t n, double hz, double fs, double amp)
{
	for (size_t i = 0; i < n; i++) {
		double v = amp * sin(2.0 * M_PI * hz * (double)i / fs);
		buf[i] = (int16_t)lrint(v);
	}
}

/* RMS of a run, skipping `skip` leading samples of filter transient. */
static double rms(const int16_t *buf, size_t n, size_t skip)
{
	double acc = 0.0;
	size_t count = 0;
	for (size_t i = skip; i < n; i++) {
		double v = (double)buf[i];
		acc += v * v;
		count++;
	}
	return count ? sqrt(acc / (double)count) : 0.0;
}

/*
 * THE anti-alias test.
 *
 * 7 kHz sampled at 48 kHz, decimated to 12 kHz, folds to 12000 - 7000 = 5000 Hz
 * -- squarely inside the modem's passband, where it would land on the busy
 * detector's spectrum and the demodulator's S/N estimate as pure noise. Taking
 * every fourth sample without filtering first would pass it through at full
 * amplitude.
 */
static void test_decimate_rejects_aliasing_energy(void **state)
{
	(void)state;
	enum { N = 4096 };
	static int16_t in[N];
	static int16_t out[N / 4];

	tone(in, N, 7000.0, 48000.0, AMP);

	ardop_resampler rs;
	assert_true(ardop_resample_init(&rs, ARDOP_RESAMPLE_DECIMATE, 4));
	size_t n = ardop_resample(&rs, in, N, out);
	assert_int_equal(n, N / 4);

	double in_rms = rms(in, N, 0);
	double out_rms = rms(out, n, 64);   /* past the transient */
	double db = 20.0 * log10((out_rms + 1e-9) / in_rms);

	/* Better than 55 dB down. The design targets ~60; the margin is for
	 * float rounding, not for a filter that nearly works. */
	assert_true(db < -55.0);
}

/* The mirror: the passband itself must survive, or we have built a notch. */
static void test_decimate_preserves_passband(void **state)
{
	(void)state;
	enum { N = 4096 };
	static int16_t in[N];
	static int16_t out[N / 4];

	/* 2500 Hz -- the top of an ARDOP signal's occupied bandwidth. */
	tone(in, N, 2500.0, 48000.0, AMP);

	ardop_resampler rs;
	assert_true(ardop_resample_init(&rs, ARDOP_RESAMPLE_DECIMATE, 4));
	size_t n = ardop_resample(&rs, in, N, out);

	double in_rms = rms(in, N, 0);
	double out_rms = rms(out, n, 64);
	double db = 20.0 * log10(out_rms / in_rms);

	assert_true(db > -0.5 && db < 0.5);
}

/*
 * m == 1 is the identity, bit for bit. A native 12 kHz device -- every Linux
 * `plughw` and the whole golden corpus path -- must be provably untouched by
 * anything in resample.c.
 */
static void test_ratio_one_is_bit_identical(void **state)
{
	(void)state;
	enum { N = 1024 };
	static int16_t in[N], out[N];
	for (size_t i = 0; i < N; i++)
		in[i] = (int16_t)((i * 2654435761u) >> 17);

	for (int d = 0; d < 2; d++) {
		ardop_resampler rs;
		assert_true(ardop_resample_init(&rs,
			d ? ARDOP_RESAMPLE_INTERPOLATE : ARDOP_RESAMPLE_DECIMATE,
			1));
		memset(out, 0, sizeof(out));
		assert_int_equal(ardop_resample(&rs, in, N, out), N);
		assert_memory_equal(out, in, sizeof(in));
		assert_int_equal(ardop_resample_pending(&rs), 0);
		assert_int_equal(ardop_resample_flush(&rs, out, N), 0);
	}
}

/* Linear phase is the whole reason this is an FIR: a symmetric impulse response
 * has exactly constant group delay, so every carrier of a frame is delayed
 * identically and inter-carrier symbol timing cannot smear. */
static void test_filter_is_symmetric_for_every_ratio(void **state)
{
	(void)state;
	for (unsigned m = 2; m <= ARDOP_RESAMPLE_MAX_M; m++) {
		ardop_resampler rs;
		assert_true(ardop_resample_init(&rs, ARDOP_RESAMPLE_DECIMATE, m));
		assert_int_equal(rs.ntaps, ARDOP_RESAMPLE_TAPS(m));
		for (unsigned i = 0; i < rs.ntaps / 2; i++) {
			double a = rs.h[i], b = rs.h[rs.ntaps - 1u - i];
			assert_true(fabs(a - b) < 1e-7);
		}
	}
}

/* DC gain is unity decimating and m interpolating (the latter compensating the
 * zero-stuffing). A DC error is a level error on every single sample. */
static void test_dc_gain(void **state)
{
	(void)state;
	enum { N = 2048 };
	static int16_t in[N], out[N * ARDOP_RESAMPLE_MAX_M];
	for (size_t i = 0; i < N; i++)
		in[i] = 1000;

	for (unsigned m = 2; m <= ARDOP_RESAMPLE_MAX_M; m++) {
		ardop_resampler dec, interp;
		assert_true(ardop_resample_init(&dec, ARDOP_RESAMPLE_DECIMATE, m));
		size_t n = ardop_resample(&dec, in, N, out);
		for (size_t i = n / 2; i < n; i++)
			assert_true(abs(out[i] - 1000) <= 2);

		assert_true(ardop_resample_init(&interp,
						ARDOP_RESAMPLE_INTERPOLATE, m));
		n = ardop_resample(&interp, in, N / m, out);
		assert_int_equal(n, (N / m) * m);
		for (size_t i = n / 2; i < n; i++)
			assert_true(abs(out[i] - 1000) <= 2);
	}
}

/*
 * Interpolate then decimate must return the signal, and the *reason* this test
 * exists is the flush: the interpolator holds (ntaps-1)/2 output samples of
 * group delay that have been accepted and not yet emitted. A backend that
 * unkeys PTT without draining that clips the tail of every transmission.
 */
static void test_round_trip_and_flush_tail(void **state)
{
	(void)state;
	enum { N = 1024, M = 4 };
	static int16_t src[N], up[N * M], down[N];

	tone(src, N, 1000.0, RATE_12K, AMP);

	ardop_resampler interp, dec;
	assert_true(ardop_resample_init(&interp, ARDOP_RESAMPLE_INTERPOLATE, M));
	assert_true(ardop_resample_init(&dec, ARDOP_RESAMPLE_DECIMATE, M));

	size_t nup = ardop_resample(&interp, src, N, up);
	assert_int_equal(nup, N * M);

	/* The pending tail is real and non-zero, and flush produces it. */
	size_t pending = ardop_resample_pending(&interp);
	assert_int_equal(pending, (ARDOP_RESAMPLE_TAPS(M) - 1) / 2);
	static int16_t tail[256];
	assert_int_equal(ardop_resample_flush(&interp, tail, sizeof(tail) / 2),
			 pending);

	size_t ndown = ardop_resample(&dec, up, nup, down);
	assert_int_equal(ndown, N);

	/* Group delay: (ntaps-1)/2 at 48 kHz through each stage, expressed at
	 * 12 kHz, so 12 samples each way. */
	const size_t delay = 2 * ((ARDOP_RESAMPLE_TAPS(M) - 1) / 2 / M);
	double err = 0.0, ref = 0.0;
	for (size_t i = 128; i + delay < N; i++) {
		double d = (double)down[i + delay] - (double)src[i];
		err += d * d;
		ref += (double)src[i] * (double)src[i];
	}
	double snr_db = 10.0 * log10(ref / (err + 1e-9));
	assert_true(snr_db > 40.0);
}

/* Interpolation overshoots source peaks; the conversion back to int16 must
 * clamp. A wrap would turn an overshoot into a full-scale discontinuity --
 * splatter, radiated, and invisible to -Wconversion. */
static void test_interpolate_saturates_instead_of_wrapping(void **state)
{
	(void)state;
	enum { N = 256, M = 4 };
	static int16_t in[N], out[N * M];

	/* A square wave at full scale: maximum Gibbs overshoot. */
	for (size_t i = 0; i < N; i++)
		in[i] = (i / 8) % 2 ? 32767 : -32768;

	ardop_resampler rs;
	assert_true(ardop_resample_init(&rs, ARDOP_RESAMPLE_INTERPOLATE, M));
	size_t n = ardop_resample(&rs, in, N, out);

	/* Nothing wrapped: a wrap turns a positive overshoot into a large
	 * negative sample, so adjacent extremes never straddle zero by more
	 * than the waveform itself does. */
	for (size_t i = 1; i < n; i++) {
		if (out[i - 1] > 30000)
			assert_true(out[i] > -30000);
		if (out[i - 1] < -30000)
			assert_true(out[i] < 30000);
	}
}

static void test_init_rejects_unsupported_ratios(void **state)
{
	(void)state;
	ardop_resampler rs;
	assert_false(ardop_resample_init(&rs, ARDOP_RESAMPLE_DECIMATE, 0));
	assert_false(ardop_resample_init(&rs, ARDOP_RESAMPLE_DECIMATE,
					 ARDOP_RESAMPLE_MAX_M + 1));
	/* 44100 Hz is why this refuses rather than approximates: 44100/12000 is
	 * 3.675, and a fractional resampler would decouple protocol time from
	 * device time (analysis/06 Rule 2). */
	assert_true(ardop_resample_init(&rs, ARDOP_RESAMPLE_DECIMATE, 4));
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_decimate_rejects_aliasing_energy),
		cmocka_unit_test(test_decimate_preserves_passband),
		cmocka_unit_test(test_ratio_one_is_bit_identical),
		cmocka_unit_test(test_filter_is_symmetric_for_every_ratio),
		cmocka_unit_test(test_dc_gain),
		cmocka_unit_test(test_round_trip_and_flush_tail),
		cmocka_unit_test(test_interpolate_saturates_instead_of_wrapping),
		cmocka_unit_test(test_init_rejects_unsupported_ratios),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
