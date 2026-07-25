#include "modem/demodulate.h"

#include <math.h>
#include <string.h>

#include "modem/goertzel.h"

/*
 * Stage 1 of the receiver: the 50-baud two-tone leader search, ported from
 * SearchFor2ToneLeader3 in SoundInput.c. The DSP is unchanged and pinned to the
 * original by test/core/test_demodulate.c. What changed: the file-scope config,
 * state and outputs it read and wrote are now fields of ardop_demod; the clock
 * is elapsed samples, not wall-clock ms; and the diagnostics (logging, stats
 * accumulation, the capture string) are dropped -- none affect the result.
 *
 * The reduced-precision pi (3.1415926f) and the derived 2*pi match the
 * transmitter and the inherited receiver. See [[ardop-mpi-normative-accident]].
 */

/* The inherited dbl2Pi = 2 * M_PI with the reduced-precision M_PI. */
static const float ARDOP_2PI = 2 * 3.1415926f;

/* 20000 ms full-search gate, in samples at 12000 Hz. */
#define FULL_SEARCH_GATE_SAMPLES 240000u

/* The 2 kHz filter: xdblR (stability, must be < 1) and xintN (length = 12000
 * / 100 Hz) in the inherited FSMixFilter2000Hz. Kept as named constants. */
#define FILTER_R 0.9995f
#define FILTER_N 120

void ardop_demod_init(ardop_demod *d, int tuning_range, int squelch)
{
	*d = (ardop_demod){0};
	d->tuning_range = tuning_range;
	d->squelch = squelch;
	d->prior_fine_offset = 1000.0f;

	/*
	 * Resonator coefficients for the 2 kHz filter (bins 4..26). The
	 * inherited code computes these lazily on the first FSMixFilter2000Hz
	 * call; here they are a deterministic function of the constants, so we
	 * compute them once at init. filt_coef[0..3] stay zero, as they do in
	 * the original (its loop also starts at 4). ARDOP_2PI carries the
	 * reduced pi, matching the original's 2 * M_PI.
	 */
	for (int i = 4; i <= 26; i++)
		d->filt_coef[i] = 2 * FILTER_R
				  * cosf(ARDOP_2PI * (float)i / (float)FILTER_N);
}

bool ardop_demod_leader_search(ardop_demod *d, const int16_t *samples,
			       int length, uint64_t now_samples, int *sn_out)
{
	float gr[56], gi[56], mag[56];
	float power, left_mag, right_mag;
	float max_peak = 0.0f, max_peak_sn = 0.0f, bin_adj;
	int interp_cnt = 0;
	int i_at_max = 0;
	float interp_threshold = 1.0f;
	int start_bin, stop_bin;
	float left_car, right_car, bin_interp_left, bin_interp_right;
	float ctr_r, ctr_i, left_p, right_p;
	float left_r[3], left_i[3], right_r[3], right_i[3];
	int ptr = 0;
	float avg_noise, coarse_offset = 1000.0f;
	float bin_adj_1475, bin_adj_1525;
	float trial_offset, power_early, sn_db_early;
	float sn_db;

	if (length < 1200)
		return false;

	if ((now_samples - d->last_good_frametype_decode > FULL_SEARCH_GATE_SAMPLES)
	    && d->tuning_range > 0) {
		/* Full search over the whole tuning range. */
		start_bin = (200 - d->tuning_range) / 10;
		stop_bin = 55 - start_bin;
		max_peak = 0;

		for (int i = start_bin; i <= stop_bin; i++) {
			ardop_goertzel_hamming(samples, ptr, 1200,
					       (float)i + 122.5f, &gr[i], &gi[i]);
			mag[i] = powf(gr[i], 2) + powf(gi[i], 2);
		}

		for (int i = start_bin + 5; i <= stop_bin - 10; i++) {
			power = sqrtf(mag[i] * mag[i + 5]);
			avg_noise = (mag[i - 5] + mag[i - 3] + mag[i + 8]
				     + mag[i + 10]) / 4;
			max_peak = power / avg_noise;
			if (max_peak > max_peak_sn) {
				max_peak_sn = max_peak;
				i_at_max = i + 122;
			}
		}

		if (((i_at_max - 123) >= start_bin)
		    && ((i_at_max - 118) <= stop_bin)) {
			bin_adj_1475 = ardop_spectral_peak_locator(
				gr[i_at_max - 123], gi[i_at_max - 123],
				gr[i_at_max - 122], gi[i_at_max - 122],
				gr[i_at_max - 121], gi[i_at_max - 121], &left_mag);
			bin_adj = 0.0f;
			if (bin_adj_1475 < interp_threshold
			    && bin_adj_1475 > -interp_threshold) {
				bin_adj = bin_adj_1475;
				interp_cnt += 1;
			}

			bin_adj_1525 = ardop_spectral_peak_locator(
				gr[i_at_max - 118], gi[i_at_max - 118],
				gr[i_at_max - 117], gi[i_at_max - 117],
				gr[i_at_max - 116], gi[i_at_max - 116], &right_mag);
			if (bin_adj_1525 < interp_threshold
			    && bin_adj_1525 > -interp_threshold) {
				bin_adj += bin_adj_1525;
				interp_cnt += 1;
			}
			if (interp_cnt == 0) {
				d->prior_fine_offset = 1000.0f;
				return false;
			}
			bin_adj = bin_adj / (float)interp_cnt;
			coarse_offset = 10.0f * ((float)i_at_max + bin_adj - 147);
		} else {
			d->prior_fine_offset = 1000.0f;
			return false;
		}
	}

	/* Narrow search. */
	if (coarse_offset < 999)
		trial_offset = coarse_offset;
	else
		trial_offset = d->offset_hz;

	if (fabsf(trial_offset) > (float)d->tuning_range && d->tuning_range > 0) {
		d->prior_fine_offset = 1000.0f;
		return false;
	}

	left_car = 147.5f + trial_offset / 10.0f;
	right_car = 152.5f + trial_offset / 10.0f;

	/* Average four noise bins around the pair. */
	ardop_goertzel_hamming(samples, ptr, 1200, 142.5f + trial_offset / 10.0f,
			       &ctr_r, &ctr_i);
	avg_noise = powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel_hamming(samples, ptr, 1200, 145.0f + trial_offset / 10.0f,
			       &ctr_r, &ctr_i);
	avg_noise += powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel_hamming(samples, ptr, 1200, 155.0f + trial_offset / 10.0f,
			       &ctr_r, &ctr_i);
	avg_noise += powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel_hamming(samples, ptr, 1200, 157.5f + trial_offset / 10.0f,
			       &ctr_r, &ctr_i);
	avg_noise += powf(ctr_r, 2) + powf(ctr_i, 2);
	avg_noise = avg_noise * 0.25f;

	/* One bin either side of each tone, for the peak locator. */
	ardop_goertzel_hamming(samples, ptr, 1200, left_car - 1,
			       &left_r[0], &left_i[0]);
	ardop_goertzel_hamming(samples, ptr, 1200, left_car,
			       &left_r[1], &left_i[1]);
	left_p = powf(left_r[1], 2) + powf(left_i[1], 2);
	ardop_goertzel_hamming(samples, ptr, 1200, left_car + 1,
			       &left_r[2], &left_i[2]);
	ardop_goertzel_hamming(samples, ptr, 1200, right_car - 1,
			       &right_r[0], &right_i[0]);
	ardop_goertzel_hamming(samples, ptr, 1200, right_car,
			       &right_r[1], &right_i[1]);
	right_p = powf(right_r[1], 2) + powf(right_i[1], 2);
	ardop_goertzel(samples, ptr, 1200, right_car + 1,
		       &right_r[2], &right_i[2]);

	/* Reject a single tone; otherwise average the two. */
	if (left_p > 4 * right_p)
		power = right_p;
	else if (right_p > 4 * left_p)
		power = left_p;
	else
		power = sqrtf(left_p * right_p);

	sn_db = 10 * log10f(power / avg_noise);

	/* Early detect on the first two symbols (480 samples). */
	ardop_goertzel(samples, ptr, 480, 57.0f + trial_offset / 25.0f,
		       &ctr_r, &ctr_i);
	avg_noise = powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel(samples, ptr, 480, 58.0f + trial_offset / 25.0f,
		       &ctr_r, &ctr_i);
	avg_noise += powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel(samples, ptr, 480, 62.0f + trial_offset / 25.0f,
		       &ctr_r, &ctr_i);
	avg_noise += powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel(samples, ptr, 480, 63.0f + trial_offset / 25.0f,
		       &ctr_r, &ctr_i);
	{
		/* max(1000.0f, 0.25 * (...)) -- the 0.25 is a double literal in
		 * the original, so this average is formed in double. */
		double d_noise = 0.25 * (double)(avg_noise + powf(ctr_r, 2)
						 + powf(ctr_i, 2));
		avg_noise = (float)(1000.0 > d_noise ? 1000.0 : d_noise);
	}
	left_car = 59 + trial_offset / 25;
	right_car = 61 + trial_offset / 25;

	ardop_goertzel(samples, ptr, 480, left_car, &ctr_r, &ctr_i);
	left_p = powf(ctr_r, 2) + powf(ctr_i, 2);
	ardop_goertzel(samples, ptr, 480, right_car, &ctr_r, &ctr_i);
	right_p = powf(ctr_r, 2) + powf(ctr_i, 2);

	if (left_p > 4 * right_p)
		power_early = right_p;
	else if (right_p > 4 * left_p)
		power_early = left_p;
	else
		power_early = sqrtf(left_p * right_p);

	sn_db_early = 10 * log10f(power_early / avg_noise);

	if (sn_db > (4 + (float)d->squelch) && sn_db_early > (float)d->squelch
	    && (avg_noise > 100.0f || d->prior_fine_offset != 1000.0f)) {
		bin_interp_left = ardop_spectral_peak_locator(
			left_r[0], left_i[0], left_r[1], left_i[1],
			left_r[2], left_i[2], &left_mag);
		bin_interp_right = ardop_spectral_peak_locator(
			right_r[0], right_i[0], right_r[1], right_i[1],
			right_r[2], right_i[2], &right_mag);

		bin_interp_left = bin_interp_left * left_mag
				  / (left_mag + right_mag);
		bin_interp_right = bin_interp_right * right_mag
				   / (left_mag + right_mag);

		if (fabsf(bin_interp_left + bin_interp_right) < 1.0f) {
			float adj = (bin_interp_left + bin_interp_right) * 10.0f;
			if (bin_interp_left + bin_interp_right > 0)
				d->offset_hz = trial_offset
					       + (adj < 3 ? adj : 3.0f);
			else
				d->offset_hz = trial_offset
					       + (adj > -3 ? adj : -3.0f);

			if (fabsf(d->prior_fine_offset - d->offset_hz) < 2.9f) {
				d->nco_freq = 3000 + d->offset_hz;
				d->nco_phase_inc = ARDOP_2PI * d->nco_freq / 12000;
				d->last_leader_detect = now_samples;
				d->start_rmt_leader_measure = now_samples;
				d->state = ARDOP_RX_ACQUIRE_SYMBOL_SYNC;
				d->sn_db = sn_db;
				*sn_out = (int)(sn_db - 24.77);
				d->prior_fine_offset = 1000.0f;
				return true;
			}
			d->prior_fine_offset = d->offset_hz;
		}
	}
	return false;
}

/*
 * The 23-section frequency-selective filter, ported from FSMixFilter2000Hz. A
 * comb feeds 23 resonators (bins 4..26); their scaled sum, rescaled for the
 * filter gain, is the baseband output. dblRn/dblR2 and the coefficients are
 * deterministic functions of the constants; the comb and resonator delay lines
 * (filt_z*) carry across calls, as does the 120-sample prior_mixed history.
 */
static void fs_mix_filter_2000hz(ardop_demod *d, const int16_t *mixed,
				 int length)
{
	float rn = powf(FILTER_R, FILTER_N);
	float r2 = powf(FILTER_R, 2);

	for (int i = 0; i < length; i++) {
		float filtered = 0;
		float zin;

		if (i < FILTER_N)
			zin = (float)mixed[i] - rn * (float)d->prior_mixed[i];
		else
			zin = (float)mixed[i] - rn * (float)mixed[i - FILTER_N];

		/* Comb. */
		d->filt_zcomb = zin - d->filt_zin_2 * r2;
		d->filt_zin_2 = d->filt_zin_1;
		d->filt_zin_1 = zin;

		/* Resonators: bins 4 and 26 scaled by 0.389, the rest summed
		 * with alternating sign (even +, odd -). */
		for (int j = 4; j <= 26; j++) {
			d->filt_zout_0[j] = d->filt_zcomb
					    + d->filt_coef[j] * d->filt_zout_1[j]
					    - r2 * d->filt_zout_2[j];
			d->filt_zout_2[j] = d->filt_zout_1[j];
			d->filt_zout_1[j] = d->filt_zout_0[j];

			if (j == 4 || j == 26)
				filtered += 0.389f * d->filt_zout_0[j];
			else if ((j & 1) == 0)
				filtered += d->filt_zout_0[j];
			else
				filtered -= d->filt_zout_0[j];
		}

		filtered = filtered * 0.00833333333f;
		d->filtered_mixed[d->filtered_mixed_len++] = (int16_t)filtered;
	}

	/* Keep the last FILTER_N mixed samples for the next call's history. */
	memmove(d->prior_mixed, &mixed[length - FILTER_N],
		FILTER_N * sizeof(int16_t));
}

void ardop_demod_mix_filter(ardop_demod *d, const int16_t *samples, int length,
			    float offset_hz)
{
	int16_t mixed[2400];

	if (length == 0)
		return;

	/* Nominal NCO is 3000 Hz: downmixing (NCO - Fnew) lands the signal at
	 * a 1500 Hz centre and inverts the sideband. */
	d->nco_freq = 3000 + offset_hz;
	d->nco_phase_inc = d->nco_freq * ARDOP_2PI / 12000;

	for (int i = 0; i < length; i++) {
		mixed[i] = (int16_t)(int)ceilf((float)samples[i]
					       * cosf(d->nco_phase));
		d->nco_phase += d->nco_phase_inc;
		if (d->nco_phase > ARDOP_2PI)
			d->nco_phase -= ARDOP_2PI;
	}

	fs_mix_filter_2000hz(d, mixed, length);
}
