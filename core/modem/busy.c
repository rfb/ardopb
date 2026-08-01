#include "modem/busy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "modem/fft.h"

/**
 * @file busy.c
 * @brief Channel-busy detector, ported from BusyDetect.c (BusyDetect3).
 *
 * The arithmetic is transcribed to stay bit-identical to the original: the
 * signal/baseline ratios are computed in float exactly as `SortSignals2` does,
 * and the thresholds keep the original's mixed float/double promotion (the
 * `0.008 * powf(...)` terms are double, so the comparison happens in double).
 */

/** Busy must persist this many consecutive calls (~250 ms) to be reported. */
#define BUSY_CONFIRM_COUNT 3
/** Busy is held this long past the last trip before it may clear. */
#define BUSY_HOLD_MS 5000u
/** Clear-delay offset applied by ardop_busy_clear (matches ClearBusy). */
#define BUSY_CLEAR_DELAY_MS 610u

/* Ascending float comparator for qsort, matching BusyDetect.c's `compare`. */
static int compare_float(const void *p1, const void *p2)
{
	float x = *(const float *)p1;
	float y = *(const float *)p2;

	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/*
 * Separate the strongest intNumBins bins from the rest of [start, stop] and
 * report the average magnitude of each group. Ported from SortSignals2 (the
 * variant BusyDetect3 actually calls; the older SortSignals is dead code with a
 * different baseline divisor). The summation order is preserved so the float
 * results are bit-identical.
 */
static void sort_signals(const float *mag, int start, int stop, int num_bins,
			 float *avg_signal_per_bin, float *avg_baseline_per_bin)
{
	float sorted[200];
	float sum1 = 0, sum2 = 0;
	int num_to_sort = (stop - start) + 1;
	int i;

	memcpy(sorted, &mag[start], (size_t)num_to_sort * sizeof(float));
	qsort(sorted, (size_t)num_to_sort, sizeof(float), compare_float);

	for (i = num_to_sort - 1; i >= 0; i--) {
		if (i >= (num_to_sort - num_bins))
			sum1 += sorted[i];
		else
			sum2 += sorted[i];
	}

	*avg_signal_per_bin = sum1 / (float)num_bins;
	*avg_baseline_per_bin = sum2 / (float)(stop - start - num_bins - 1);
}

void ardop_busy_clear(ardop_busy_detector *b, uint32_t now_ms)
{
	b->last_busy_trip = now_ms;
	b->prior_last_busy_trip = b->last_busy_trip;
	/* +610 ms so the downstream busy-blocking test still works. */
	b->last_busy_clear = b->last_busy_trip + BUSY_CLEAR_DELAY_MS;
	/* Clears busy immediately (required for scanning when re-enabled). */
	b->last_trip = b->last_busy_trip - BUSY_HOLD_MS;
	b->last_busy = false;
	b->busy_on_count = 0;
	b->busy_off_count = 0;
	/* Force the rolling averages to reinitialise on the next call. */
	b->last_start = 0;
	b->last_stop = 0;
}

bool ardop_busy_detect(ardop_busy_detector *b, const float *mag, int start,
		       int stop, ardop_bandwidth bw, int busy_det,
		       uint32_t now_ms)
{
	const float slow_alpha = 0.2f;
	float avg_signal_narrow, avg_signal_wide;
	float avg_baseline_narrow, avg_baseline_wide;
	float ston_narrow, ston_wide;
	int narrow = 8;                        /* ~94 Hz (8 x 11.72 Hz). */
	int wide = ((stop - start) * 2) / 3;   /* ~2/3 of the bandwidth. */
	bool busy = false;

	/*
	 * Narrow band. Note ston_narrow is a fresh local each call: the
	 * original reads a function-local here too (not the persistent
	 * dblAvgStoNSlow* globals), so the "(1 - alpha) * ston" term is always
	 * (1 - alpha) * 0. Preserved deliberately -- see the header note.
	 */
	ston_narrow = 0;
	sort_signals(mag, start, stop, narrow, &avg_signal_narrow,
		     &avg_baseline_narrow);
	if (b->last_start == start && b->last_stop == stop) {
		ston_narrow = (1 - slow_alpha) * ston_narrow
			      + slow_alpha * avg_signal_narrow / avg_baseline_narrow;
	} else {
		ston_narrow = avg_signal_narrow / avg_baseline_narrow;
		b->last_start = start;
		b->last_stop = stop;
	}

	/* Wide band (66% of the current bandwidth). */
	ston_wide = 0;
	sort_signals(mag, start, stop, wide, &avg_signal_wide,
		     &avg_baseline_wide);
	if (b->last_start == start && b->last_stop == stop) {
		ston_wide = (1 - slow_alpha) * ston_wide
			    + slow_alpha * avg_signal_wide / avg_baseline_wide;
	} else {
		ston_wide = avg_signal_wide / avg_baseline_wide;
		b->last_start = start;
		b->last_stop = stop;
	}

	/*
	 * Thresholds. The original keeps these in double (the 0.008/0.02/0.016
	 * literals promote powf's float result), so the comparison runs in
	 * double; replicated exactly. B*MAX and B*FORCED behave identically, so
	 * they collapse to one case per bandwidth here.
	 */
	double det4 = 0.008 * (double)powf((float)busy_det, 4.0f);
	switch (bw) {
	case ARDOP_BW_200:
	case ARDOP_BW_500:
		busy = (double)ston_narrow > (3 + det4)
		       || (double)ston_wide > (5 + 0.02 * (double)powf((float)busy_det, 4.0f));
		break;
	case ARDOP_BW_1000:
	case ARDOP_BW_2000:
		busy = (double)ston_narrow > (3 + det4)
		       || (double)ston_wide > (5 + 0.016 * (double)powf((float)busy_det, 4.0f));
		break;
	}

	if (busy_det == 0)
		busy = false;  /* 0 disables the check. */

	if (busy) {
		/* Require several adjacent busy conditions to skip nuisance
		 * trips: busy must be present >3 times (~250 ms). */
		b->busy_on_count += 1;
		b->busy_off_count = 0;
		if (b->busy_on_count > BUSY_CONFIRM_COUNT)
			b->last_trip = now_ms;
	} else {
		b->busy_off_count += 1;
		b->busy_on_count = 0;
	}

	if (!b->last_busy && b->busy_on_count >= BUSY_CONFIRM_COUNT) {
		b->prior_last_busy_trip = b->last_busy_trip;
		b->last_busy_trip = now_ms;
		b->last_busy = true;
	} else if (b->last_busy && (now_ms - b->last_trip) > BUSY_HOLD_MS
		   && b->busy_off_count >= BUSY_CONFIRM_COUNT) {
		b->last_busy_clear = now_ms;
		b->last_busy = false;
	}

	return b->last_busy;
}

/* The reduced pi, as elsewhere; see [[ardop-mpi-normative-accident]]. */
#define ARDOP_BUSY_PI 3.1415926f

/* Centre bin of the tuning-line range (bin 128 = 1500 Hz, offset by 25). */
#define BUSY_CENTRE_BIN 103

void ardop_busy_window(float *w)
{
	/* Ported from generateBH. The window is symmetric; store the first half.
	 * The reduced M_PI is preserved so the coefficients match bit-for-bit. */
	float pi = ARDOP_BUSY_PI;
	for (int i = 0; i < 513; i++) {
		float fi = (float)i;
		w[i] = (float)(0.35875 - 0.48829 * cos(2.0f * pi * fi / 1024.0f)
			       + 0.14128 * cos(4.0f * pi * fi / 1024.0f)
			       - 0.01168 * cos(6.0f * pi * fi / 1024.0f));
	}
}

void ardop_busy_spectrum(const float *window, const int16_t *samples,
			 float *mag_out)
{
	float windowed[ARDOP_BUSY_WINDOW];
	float re[ARDOP_BUSY_WINDOW];
	float im[ARDOP_BUSY_WINDOW];

	/* Apply the symmetric Blackman-Harris window (as UpdateBusyDetector). */
	windowed[0] = (float)samples[0] * window[0];
	windowed[512] = (float)samples[512] * window[512];
	for (int i = 1; i < 512; i++) {
		windowed[i] = (float)samples[i] * window[i];
		windowed[1024 - i] = (float)samples[1024 - i] * window[i];
	}

	ardop_fft(ARDOP_BUSY_WINDOW, windowed, re, im, false);

	for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++)
		mag_out[i] = powf(re[i + ARDOP_BUSY_FIRST_BIN], 2)
			     + powf(im[i + ARDOP_BUSY_FIRST_BIN], 2);
}

bool ardop_busy_detect_spectrum(ardop_busy_detector *b, const float *mag,
				int bw_hz, ardop_bandwidth bw,
				int tuning_range, int busy_det, uint32_t now_ms)
{
	/* The tuning-line search range, widened by bandwidth and tuning range.
	 * 11.719 Hz is one FFT bin (12000/1024). */
	int delta = (int)((float)(bw_hz / 2 + tuning_range) / 11.719f);
	int low = BUSY_CENTRE_BIN - delta;
	int high = BUSY_CENTRE_BIN + delta;
	if (low < 3)
		low = 3;
	if (high > 203)
		high = 203;

	return ardop_busy_detect(b, mag, low, high, bw, busy_det, now_ms);
}

bool ardop_busy_analyze(ardop_busy_detector *b, const float *window,
			const int16_t *samples, int bw_hz, ardop_bandwidth bw,
			int tuning_range, int busy_det, uint32_t now_ms)
{
	float mag[ARDOP_BUSY_MAG_BINS];

	ardop_busy_spectrum(window, samples, mag);
	return ardop_busy_detect_spectrum(b, mag, bw_hz, bw, tuning_range,
					  busy_det, now_ms);
}
