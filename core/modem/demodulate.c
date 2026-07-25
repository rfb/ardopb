#include "modem/demodulate.h"

#include <math.h>
#include <string.h>

#include "codec/crc.h"
#include "codec/frame.h"
#include "codec/rs.h"
#include "modem/goertzel.h"
#include "modem/templates.h"

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

/* The inherited reduced-precision M_PI and the derived 2*pi. */
static const float ARDOP_PI = 3.1415926f;
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

/*
 * The 75 Hz envelope filter (3 resonators, bins 29..31), ported from Filter75Hz.
 * Its comb and resonator delay lines are local and reset each call, so the
 * output is a pure function of the input window -- with one inherited quirk that
 * must be preserved: `filter_out` is NOT reset per sample. It accumulates across
 * the whole window, and that running sum is on-air normative (it shaped the
 * decode the transmitter was tuned against). Reads filtered_mixed from
 * mfs_read_ptr; writes `count` samples to out.
 */
static void filter_75hz(const ardop_demod *d, int16_t *out, int count)
{
	const float r = 0.9995f;   /* stability factor, as in the 2 kHz filter */
	const int n = 240;         /* filter length 12000/50; delays 120 samples */
	float rn = powf(r, (float)n);
	float r2 = powf(r, 2);
	float coef[3];
	float zin_1 = 0, zin_2 = 0, zcomb;
	float zout_0[3] = {0}, zout_1[3] = {0}, zout_2[3] = {0};
	float filter_out = 0;  /* deliberately not reset per sample (see above) */
	int base = d->mfs_read_ptr;

	for (int i = 0; i < 3; i++)
		coef[i] = 2 * r * cosf(ARDOP_2PI * (float)(29 + i) / (float)n);

	for (int i = 0; i < count; i++) {
		float zin;

		if (i < n)
			zin = (float)d->filtered_mixed[base + i];
		else
			zin = (float)d->filtered_mixed[base + i]
			      - rn * (float)d->filtered_mixed[base + i - n];

		zcomb = zin - zin_2 * r2;
		zin_2 = zin_1;
		zin_1 = zin;

		for (int j = 0; j < 3; j++) {
			zout_0[j] = zcomb + coef[j] * zout_1[j] - r2 * zout_2[j];
			zout_2[j] = zout_1[j];
			zout_1[j] = zout_0[j];

			/* Bins 29 and 31 subtract (0.39811 transition), 30 adds. */
			if (j == 0 || j == 2)
				filter_out -= 0.39811f * zout_0[j];
			else
				filter_out += zout_0[j];
		}
		out[i] = (int16_t)(int)ceil(filter_out * 0.0041f);
	}
}

/*
 * Correlate 1.5 symbols of the 75 Hz-filtered baseband against the leader
 * template and return the sample offset of best correlation (the symbol
 * boundary), or -1 if the correlation is too weak. Ported from
 * EnvelopeCorrelator; the stats accumulation is dropped.
 */
static int envelope_correlator(const ardop_demod *d)
{
	float cor_max = -1000000.0f;
	float cor_sum, cor_product, cor_max_product = 0.0f;
	int j_at_max = 0;
	int16_t filt75[720];

	if (d->filtered_mixed_len < d->mfs_read_ptr + 720)
		return -1;

	filter_75hz(d, filt75, 720);

	for (int j = 0; j < 360; j++) {  /* over 1.5 symbols */
		cor_sum = 0;
		for (int i = 0; i < 240; i++) {  /* over one 50-baud symbol */
			/* 120 accommodates the 75 Hz filter's 120-sample delay. */
			cor_product = (float)(int50BaudTwoToneLeaderTemplate[i]
					      * filt75[120 + i + j]);
			cor_sum += cor_product;
			if (fabsf(cor_product) > cor_max_product)
				cor_max_product = fabsf(cor_product);
		}
		if (fabsf(cor_sum) > cor_max) {
			cor_max = fabsf(cor_sum);
			j_at_max = j;
		}
	}

	if (cor_max > 40 * cor_max_product)
		return j_at_max;
	return -1;
}

bool ardop_demod_symbol_framing(ardop_demod *d)
{
	float real, imag;
	float car_ph, abs_ph_err;
	float min_abs_ph_err = 5000;  /* initialised to an excessive value */
	int i_at_min_err = 0;
	int local_ptr;

	if ((d->filtered_mixed_len - d->mfs_read_ptr) < 860)
		return false;  /* not enough */

	/* Correlator positions the pointer at the symbol boundary. */
	local_ptr = d->mfs_read_ptr + envelope_correlator(d);

	/* Refine within +/-2 samples to the minimum 1500 Hz phase error (the
	 * leader's nominal phase is 0 or 180 degrees). */
	for (int i = -2; i <= 2; i++) {
		ardop_goertzel(d->filtered_mixed, local_ptr + i, 120, 30,
			       &real, &imag);
		car_ph = atan2f(imag, real);
		abs_ph_err = fabsf((float)((double)car_ph
			- ceil((double)(car_ph / ARDOP_PI)) * (double)ARDOP_PI));
		if (abs_ph_err < min_abs_ph_err) {
			min_abs_ph_err = abs_ph_err;
			i_at_min_err = i;
		}
	}

	d->mfs_read_ptr = local_ptr + i_at_min_err;
	d->state = ARDOP_RX_ACQUIRE_FRAME_SYNC;
	return true;
}

bool ardop_demod_frame_sync(ardop_demod *d)
{
	int local_ptr = d->mfs_read_ptr;
	int available_symbols = (d->filtered_mixed_len - d->mfs_read_ptr) / 240;
	float phase_sym1, phase_sym2, phase_sym3;
	float real, imag;
	float phase_diff12, phase_diff23;

	if (available_symbols < 3)
		return false;  /* need at least 3 symbols to compare */

	ardop_goertzel(d->filtered_mixed, local_ptr, 240, 30, &real, &imag);
	phase_sym1 = atan2f(imag, real);
	local_ptr += 240;
	ardop_goertzel(d->filtered_mixed, local_ptr, 240, 30, &real, &imag);
	phase_sym2 = atan2f(imag, real);
	local_ptr += 240;

	for (int i = 0; i <= available_symbols - 3; i++) {
		ardop_goertzel(d->filtered_mixed, local_ptr, 240, 30,
			       &real, &imag);
		phase_sym3 = atan2f(imag, real);

		phase_diff12 = phase_sym1 - phase_sym2;
		if (phase_diff12 > ARDOP_PI)  /* bound to +/- pi */
			phase_diff12 -= ARDOP_2PI;
		else if (phase_diff12 < -ARDOP_PI)
			phase_diff12 += ARDOP_2PI;

		phase_diff23 = phase_sym2 - phase_sym3;
		if (phase_diff23 > ARDOP_PI)
			phase_diff23 -= ARDOP_2PI;
		else if (phase_diff23 < -ARDOP_PI)
			phase_diff23 += ARDOP_2PI;

		/* Sync symbol: a large step into it, then no step out (< 60 deg). */
		if (fabsf(phase_diff12) > 0.6667f * ARDOP_PI
		    && fabsf(phase_diff23) < 0.3333f * ARDOP_PI) {
			/* 30 accommodates the initial filter-length pointer
			 * offset. The ceil is a no-op over integer division;
			 * kept to mirror the original exactly. */
			d->leader_rcvd_ms =
				(int)ceil((double)((local_ptr - 30) / 12));
			d->mfs_read_ptr = local_ptr + 240;
			return true;  /* pointer now at the first frame-type symbol */
		}

		phase_sym1 = phase_sym2;
		phase_sym2 = phase_sym3;
		local_ptr += 240;
	}

	/* Back up two symbols so the next call resumes cleanly. */
	d->mfs_read_ptr = local_ptr - 480;
	return false;
}

bool ardop_demod_frametype_tonemags(const ardop_demod *d, int ptr, int32_t *mags)
{
	float real, imag;

	if ((d->filtered_mixed_len - ptr) < 2400)
		return false;

	for (int i = 0; i < 10; i++) {
		/*
		 * The four 50-baud tones. Each magnitude is |Goertzel|^2, but
		 * with an inherited asymmetry preserved bit-for-bit: the real
		 * part's square is truncated to int *before* the imaginary
		 * part's square is added (the original casts only the first
		 * powf). It is RX-local -- see analysis/12.
		 */
		ardop_goertzel(d->filtered_mixed, ptr, 240, 1575 / 50.0f,
			       &real, &imag);
		mags[4 * i] = (int32_t)((float)(int)powf(real, 2)
					+ powf(imag, 2));
		ardop_goertzel(d->filtered_mixed, ptr, 240, 1525 / 50.0f,
			       &real, &imag);
		mags[1 + 4 * i] = (int32_t)((float)(int)powf(real, 2)
					    + powf(imag, 2));
		ardop_goertzel(d->filtered_mixed, ptr, 240, 1475 / 50.0f,
			       &real, &imag);
		mags[2 + 4 * i] = (int32_t)((float)(int)powf(real, 2)
					    + powf(imag, 2));
		ardop_goertzel(d->filtered_mixed, ptr, 240, 1425 / 50.0f,
			       &real, &imag);
		mags[3 + 4 * i] = (int32_t)((float)(int)powf(real, 2)
					    + powf(imag, 2));
		ptr += 240;
	}

	return true;
}

float ardop_frametype_decode_distance(const int32_t *mags, int tone_ptr,
				      uint8_t frame_type, uint8_t id)
{
	float distance = 0;
	uint8_t mask = 0xC0;

	for (int j = 0; j <= 4; j++) {  /* over 5 symbols */
		int tone_sum = 0;
		int tone_index;

		for (int k = 0; k <= 3; k++)
			tone_sum += mags[tone_ptr + (4 * j) + k];
		if (tone_sum == 0)
			tone_sum = 1;  /* guard against divide-by-zero */

		/* The first four symbols carry the type's dibits (XORed with
		 * the decode key); the fifth is the parity symbol. */
		if (j < 4)
			tone_index = ((frame_type ^ id) & mask) >> (6 - 2 * j);
		else
			tone_index = ardop_frame_type_parity(frame_type);

		distance += 1.0f - (1.0f * (float)mags[tone_ptr + (4 * j)
						       + tone_index])
				   / (1.0f * (float)tone_sum);
		mask = (uint8_t)(mask >> 2);
	}

	return distance / 5;  /* normalise back to 0..1 */
}

/* Frame-type classification values (ARDOPC.h), for the acceptance rules. */
#define FT_DATANAK_MAX	0x1F
#define FT_BREAK	0x23
#define FT_IDLE		0x24
#define FT_DISC		0x29
#define FT_END		0x2C
#define FT_CONREJBUSY	0x2D
#define FT_CONREJBW	0x2E
#define FT_CONREQ_MIN	0x31
#define FT_CONREQ_MAX	0x38
#define FT_DATAACK_MIN	0xE0
#define FT_DATAACK_MAX	0xFF

int ardop_frametype_minimal_distance(const int32_t *mags,
				     const ardop_frametype_decode_ctx *ctx,
				     bool *set_last_good)
{
	float min1 = 5, min2 = 5, min3 = 5;  /* large initial distances */
	int iat1 = 0, iat2 = 0, iat3 = 0;

	*set_last_good = false;

	/* Minimal distance over the valid types, for each of the three keys. */
	for (int i = 0; i < ctx->valid_len; i++) {
		uint8_t type = ctx->valid_types[i];
		float d1 = ardop_frametype_decode_distance(mags, 0, type, 0);
		float d2 = ardop_frametype_decode_distance(mags, 20, type,
							   ctx->session_id);
		float d3;

		if (ctx->pending)
			d3 = ardop_frametype_decode_distance(mags, 20, type,
							     0xFF);
		else
			d3 = ardop_frametype_decode_distance(
				mags, 20, type, ctx->last_arq_session_id);

		if (d1 < min1) { min1 = d1; iat1 = type; }
		if (d2 < min2) { min2 = d2; iat2 = type; }
		if (d3 < min3) { min3 = d3; iat3 = type; }
	}

	/*
	 * Acceptance rules by connection state. The distance thresholds keep the
	 * inherited literal types exactly (0.3 double vs 0.3f float vs 0.4), as
	 * they can differ at the boundary. Only some accepting branches refresh
	 * the tuning timestamp; that branch-specificity is reported via
	 * set_last_good.
	 */
	if (ctx->session_id == 0xFF) {
		/* FEC / monitoring / not yet pending or connected. */
		if (iat1 == FT_DISC && iat3 == FT_DISC
		    && (min1 < 0.3 || min3 < 0.3))
			return iat1;  /* stale DISC from a prior session */
		if (iat1 == iat2 && (min1 < 0.3 || min2 < 0.3)) {
			*set_last_good = true;
			return iat1;
		}
		if (min1 < 0.3 && min1 < min2
		    && ardop_frame_is_data((uint8_t)iat1))
			return iat1;
		if (min2 < 0.3 && min2 < min1
		    && ardop_frame_is_data((uint8_t)iat2))
			return iat2;
		return -1;
	} else if (ctx->pending) {
		/* Expecting a ConAck from the ISS. */
		if (iat1 == iat2) {
			if (min1 < 0.3 || min2 < 0.3) {
				*set_last_good = true;
				return iat1;
			}
			return -1;
		} else if (iat1 == iat3) {
			/* ISS missed our ConAck and repeated the ConReq. */
			if (iat1 >= FT_CONREQ_MIN && iat1 <= FT_CONREQ_MAX
			    && (min1 < 0.3 || min3 < 0.3)) {
				*set_last_good = true;
				return iat1;
			}
			return -1;
		}
		/* no match: fall through to the poor-decode return */
	} else if (ctx->arq_connected) {
		if (iat1 == iat2) {
			bool critical = (iat1 >= FT_DATAACK_MIN
					 && iat1 <= FT_DATAACK_MAX)
					|| iat1 == FT_BREAK || iat1 == FT_END
					|| iat1 == FT_DISC;
			if (critical) {
				if (min1 < 0.3f || min2 < 0.3f) {
					*set_last_good = true;
					return iat1;
				}
				return -1;
			}
			/* non-critical frames: no protocol risk, looser bound */
			if (min1 < 0.4 || min2 < 0.4) {
				*set_last_good = true;
				return iat1;
			}
			return -1;
		}
		return -1;
	}

	return -1;  /* poor-quality decode; don't use */
}

uint8_t ardop_demod_4fsk_char(const ardop_demod *d, int start, int center_freq,
			      int baud, int samp_per_sym, int32_t *tone_mags)
{
	float real, imag, mag[4];
	/* Highest frequency first: the sideband is reversed, so this is the
	 * lowest tone as sent. Bins are in units of the baud rate. */
	float search_freq = (float)center_freq + 1.5f * (float)baud;
	uint8_t byte = 0;

	for (int j = 0; j < 4; j++) {
		ardop_goertzel(d->filtered_mixed, start, samp_per_sym,
			       search_freq / (float)baud, &real, &imag);
		mag[0] = powf(real, 2) + powf(imag, 2);
		ardop_goertzel(d->filtered_mixed, start, samp_per_sym,
			       (search_freq - (float)baud) / (float)baud,
			       &real, &imag);
		mag[1] = powf(real, 2) + powf(imag, 2);
		ardop_goertzel(d->filtered_mixed, start, samp_per_sym,
			       (search_freq - (float)(2 * baud)) / (float)baud,
			       &real, &imag);
		mag[2] = powf(real, 2) + powf(imag, 2);
		ardop_goertzel(d->filtered_mixed, start, samp_per_sym,
			       (search_freq - (float)(3 * baud)) / (float)baud,
			       &real, &imag);
		mag[3] = powf(real, 2) + powf(imag, 2);

		uint8_t sym;
		if (mag[0] > mag[1] && mag[0] > mag[2] && mag[0] > mag[3])
			sym = 0;
		else if (mag[1] > mag[0] && mag[1] > mag[2] && mag[1] > mag[3])
			sym = 1;
		else if (mag[2] > mag[0] && mag[2] > mag[1] && mag[2] > mag[3])
			sym = 2;
		else
			sym = 3;

		byte = (uint8_t)((byte << 2) + sym);
		tone_mags[4 * j + 0] = (int32_t)mag[0];
		tone_mags[4 * j + 1] = (int32_t)mag[1];
		tone_mags[4 * j + 2] = (int32_t)mag[2];
		tone_mags[4 * j + 3] = (int32_t)mag[3];
		start += samp_per_sym;
	}

	return byte;
}

int ardop_decode_carrier_rs(const ardop_rs *rs, uint8_t *raw,
			    uint8_t *corrected, int data_len, int rs_len,
			    uint8_t frame_type, bool carrier_already_ok,
			    bool *decoded_ok)
{
	int combined = data_len + rs_len + 3;
	int nerrors;

	*decoded_ok = false;

	if (carrier_already_ok) {
		/* Already good; it may just be misplaced after another carrier
		 * decoded wrong, so re-emit the net bytes without re-decoding. */
		memcpy(corrected, &raw[1], raw[0]);
		return raw[0];
	}

	/* Always RS-correct before the CRC check: RS can repair a block that
	 * would otherwise pass a lucky CRC, and correcting first lowers the
	 * chance of an undetected error. */
	nerrors = ardop_rs_correct(rs, raw, combined, rs_len, false);
	if (nerrors < 0)
		goto bad;  /* RS could not correct */

	/*
	 * RS occasionally reports success on a block it did not truly fix, so
	 * the CRC (with the frame type mixed in) is the real arbiter. A sane
	 * reported length guards the memcpy.
	 */
	if (nerrors >= 0 && raw[0] <= data_len
	    && ardop_crc16_trailer_ok(raw, (size_t)(data_len + 1), frame_type,
				      &raw[data_len + 1])) {
		memcpy(corrected, &raw[1], raw[0]);
		*decoded_ok = true;
		return raw[0];
	}

bad:
	/* Hand back the uncorrected data bytes (no length byte, no parity). */
	memcpy(corrected, &raw[1], (size_t)data_len);
	return data_len;
}

/*
 * Angle subtraction in milliradians, wrapped to +/- pi (+/- 3142). Ported from
 * ComputeAng1_Ang2.
 */
static int compute_ang1_ang2(int ang1, int ang2)
{
	int diff = ang1 - ang2;

	if (diff < -3142)
		diff += 6284;
	else if (diff > 3142)
		diff -= 6284;
	return diff;
}

void ardop_demod_psk_init(ardop_demod *d, int num_car, int psk_mode)
{
	/* Highest carrier first (reversed sideband): 1500 for a single carrier,
	 * otherwise stepping down by 200 Hz from the top of the group. */
	float car_freq = (num_car == 1) ? 1500.0f
					: (float)(1400 + (num_car / 2) * 200);

	d->psk_mode = psk_mode;
	d->psk_samp_per_sym = 120;
	d->phases_len = 0;

	for (int i = 0; i < num_car; i++) {
		float real, imag;

		d->n_for_goertzel[i] = 120;
		d->freq_bin[i] = car_freq / 100;
		d->cp[i] = 0;

		/* Reference phase from the training symbol at the buffer start. */
		ardop_goertzel_hanning(d->filtered_mixed, 0, d->n_for_goertzel[i],
				       d->freq_bin[i], &real, &imag);
		d->psk_phase_1[i] = (short)(1000 * atan2f(imag, real));

		/* Reference magnitude (for QAM); the truncation to short before
		 * the 0.75 scaling is the inherited two-step and is preserved. */
		d->car_mag_threshold[i] =
			(short)sqrtf(powf(real, 2) + powf(imag, 2));
		d->car_mag_threshold[i] =
			(short)(d->car_mag_threshold[i] * 0.75);

		car_freq -= 200;
	}
}

int ardop_demod_psk_char(ardop_demod *d, int start, int carrier,
			 bool carrier_already_ok)
{
	int num_symbols = d->psk_mode;
	int orig_start = start;

	if (carrier_already_ok) {
		/* Skip the DSP but still advance, as the original does. */
		d->phases_len += num_symbols;
		return d->psk_samp_per_sym * num_symbols;
	}

	for (int i = 0; i < num_symbols; i++) {
		float real, imag;
		short phase_0;

		if (d->cp[carrier] == 0)
			ardop_goertzel_hanning(d->filtered_mixed, start,
					       d->n_for_goertzel[carrier],
					       d->freq_bin[carrier],
					       &real, &imag);
		else
			ardop_goertzel(d->filtered_mixed,
				       start + d->cp[carrier],
				       d->n_for_goertzel[carrier],
				       d->freq_bin[carrier], &real, &imag);

		d->mags[carrier][d->phases_len] =
			(short)sqrtf(powf(real, 2) + powf(imag, 2));
		phase_0 = (short)(1000 * atan2f(imag, real));
		d->phases[carrier][d->phases_len] =
			(short)(-compute_ang1_ang2(phase_0,
						   d->psk_phase_1[carrier]));
		d->psk_phase_1[carrier] = phase_0;
		d->phases_len++;
		start += d->psk_samp_per_sym;
	}

	return start - orig_start;
}

int ardop_decode_psk_char(const ardop_demod *d, int carrier, uint8_t *decoded,
			  bool carrier_already_ok)
{
	int len = d->phases_len;
	int psk_start = 0;
	int char_index = 0;

	if (carrier_already_ok)
		return 0;  /* already decoded; don't do it again */

	while (len > 0) {
		if (d->psk_mode == 4) {
			/* Four phases -> one byte, 2 bits each. */
			uint8_t raw = 0;

			for (int k = 0; k < 4; k++) {
				short p = d->phases[carrier][psk_start];

				if (k != 0)
					raw = (uint8_t)(raw << 2);
				if (p < 786 && p > -786)
					;  /* 0 */
				else if (p >= 786 && p < 2356)
					raw = (uint8_t)(raw + 1);
				else if (p >= 2356 || p <= -2356)
					raw = (uint8_t)(raw + 2);
				else
					raw = (uint8_t)(raw + 3);
				psk_start++;
			}
			decoded[char_index++] = raw;
			len -= 4;
		} else if (d->psk_mode == 8) {
			/* Eight phases -> 24 bits -> three bytes, 3 bits each. */
			unsigned int bits24 = 0;

			for (int k = 0; k < 8; k++) {
				short p = d->phases[carrier][psk_start];

				bits24 <<= 3;
				if (p < 393 && p > -393)
					;  /* 0 */
				else if (p >= 393 && p < 1179)
					bits24 += 1;
				else if (p >= 1179 && p < 1965)
					bits24 += 2;
				else if (p >= 1965 && p < 2751)
					bits24 += 3;
				else if (p >= 2751 || p < -2751)
					bits24 += 4;
				else if (p >= -2751 && p < -1965)
					bits24 += 5;
				else if (p >= -1965 && p <= -1179)
					bits24 += 6;
				else
					bits24 += 7;
				psk_start++;
			}
			decoded[char_index++] = (uint8_t)(bits24 >> 16);
			decoded[char_index++] = (uint8_t)(bits24 >> 8);
			decoded[char_index++] = (uint8_t)bits24;
			len -= 8;
		} else {
			return char_index;  /* unsupported PSK mode */
		}
	}

	return char_index;
}

int ardop_demod_qam_char(ardop_demod *d, int start, int carrier,
			 bool carrier_already_ok)
{
	int num_symbols = 2;
	int orig_start = start;

	if (carrier_already_ok) {
		d->phases_len += num_symbols;
		return d->psk_samp_per_sym * num_symbols;
	}

	for (int i = 0; i < num_symbols; i++) {
		float real, imag;
		short phase_0;

		ardop_goertzel_hanning(d->filtered_mixed,
				       start + d->cp[carrier],
				       d->n_for_goertzel[carrier],
				       d->freq_bin[carrier], &real, &imag);
		d->mags[carrier][d->phases_len] =
			(short)sqrtf(powf(real, 2) + powf(imag, 2));
		phase_0 = (short)(1000 * atan2f(imag, real));
		d->phases[carrier][d->phases_len] =
			(short)(-compute_ang1_ang2(phase_0,
						   d->psk_phase_1[carrier]));
		d->psk_phase_1[carrier] = phase_0;
		d->phases_len++;
		start += d->psk_samp_per_sym;
	}

	return start - orig_start;
}

int ardop_decode_qam_char(ardop_demod *d, int carrier, uint8_t *decoded,
			  bool carrier_already_ok)
{
	int threshold = d->car_mag_threshold[carrier];
	int len = d->phases_len;
	int psk_start = 0;
	int char_index = 0;

	if (carrier_already_ok)
		return 0;

	while (len > 0) {
		unsigned int data = 0;  /* two nibbles -> one byte */

		for (int k = 0; k < 2; k++) {
			short p = d->phases[carrier][psk_start];
			short mag = d->mags[carrier][psk_start];

			data <<= 4;
			/* Three bits from the phase sector (as 8PSK). */
			if (p < 393 && p > -393)
				;  /* 0 */
			else if (p >= 393 && p < 1179)
				data += 1;
			else if (p >= 1179 && p < 1965)
				data += 2;
			else if (p >= 1965 && p < 2751)
				data += 3;
			else if (p >= 2751 || p < -2751)
				data += 4;
			else if (p >= -2751 && p < -1965)
				data += 5;
			else if (p >= -1965 && p <= -1179)
				data += 6;
			else
				data += 7;

			/* Fourth bit: inner ring when below the rolling
			 * threshold, which tracks toward the seen magnitude
			 * (weighted more strongly for inner symbols). */
			if (mag < threshold) {
				data += 8;
				threshold = (threshold * 900 + mag * 150) / 1000;
			} else {
				threshold = (threshold * 900 + mag * 75) / 1000;
			}
			d->car_mag_threshold[carrier] = (short)threshold;
			psk_start++;
		}
		decoded[char_index++] = (uint8_t)data;
		len -= 2;
	}

	return char_index;
}

/* ------------------------------------------------------------------------- *
 * Stage 6: the streaming push FSM (ported from ProcessNewSamples).
 * ------------------------------------------------------------------------- */

/* Short control frames carry no data: the frame type is the whole message. */
static bool is_short_control_frame(uint8_t t)
{
	if (t <= FT_DATANAK_MAX)
		return true;
	if (t == FT_BREAK || t == FT_IDLE || t == FT_DISC || t == FT_END
	    || t == FT_CONREJBUSY || t == FT_CONREJBW)
		return true;
	if (t >= FT_DATAACK_MIN)
		return true;
	return false;
}

/* Reset the baseband buffer between frames (ClearAllMixedSamples). */
static void clear_mixed(ardop_demod *d)
{
	d->filtered_mixed_len = 0;
	d->mfs_read_ptr = 0;
}

/* Slide filtered_mixed down so mfs_read_ptr becomes 0, dropping used samples. */
static void compact_mixed(ardop_demod *d)
{
	d->filtered_mixed_len -= d->mfs_read_ptr;
	if (d->filtered_mixed_len < 0)
		d->filtered_mixed_len = 0;
	memmove(d->filtered_mixed, &d->filtered_mixed[d->mfs_read_ptr],
		(size_t)d->filtered_mixed_len * sizeof(int16_t));
	d->mfs_read_ptr = 0;
}

/*
 * Acquire4FSKFrameType wrapper: measure the frame-type tones, choose the type
 * by minimal distance (using the caller-set decode keys), advance past the
 * 10-symbol field, and refresh the tuning timestamp on a confident decode.
 * Returns the type, -1 for a poor decode, or -2 for insufficient samples.
 */
static int acquire_frame_type(ardop_demod *d, uint64_t now)
{
	int32_t mags[ARDOP_FRAMETYPE_TONE_MAGS];
	bool set_last_good;
	int type;

	if ((d->filtered_mixed_len - d->mfs_read_ptr) < 240 * 10.5)
		return -2;  /* wait for more samples */
	if (!ardop_demod_frametype_tonemags(d, d->mfs_read_ptr, mags))
		return -1;

	type = ardop_frametype_minimal_distance(mags, &d->ft_ctx, &set_last_good);
	if (set_last_good)
		d->last_good_frametype_decode = now;

	d->mfs_read_ptr += 240 * 10;  /* advance to the first data symbol */
	return type;
}

/* Emit an event if there is room. */
static void emit(ardop_event *events, size_t *nev, size_t max_events,
		 const ardop_event *ev)
{
	if (*nev < max_events)
		events[(*nev)++] = *ev;
}

/*
 * A whole frame's carriers are demodulated; RS-decode each and emit the result.
 * For now the single-carrier path (4FSK/PSK/QAM 1 carrier) is wired; wider
 * frames extend the carrier loop.
 */
static void deliver_frame(ardop_demod *d, ardop_event *events, size_t *nev,
			  size_t max_events)
{
	uint8_t *raw = d->frame_data[0];
	bool ok = false;
	int net = ardop_decode_carrier_rs(d->rs, raw, d->payload,
					  d->frame_data_len, d->frame_rs_len,
					  d->frame_type, false, &ok);
	ardop_event ev = {0};

	ev.frame_type = d->frame_type;
	if (ok) {
		ev.kind = ARDOP_EV_FRAME_DECODED;
		ev.data = d->payload;
		ev.data_len = net;
	} else {
		ev.kind = ARDOP_EV_FRAME_BAD;
	}
	emit(events, nev, max_events, &ev);
}

size_t ardop_demod_push(ardop_demod *d, const int16_t *samples, size_t n,
			uint64_t now_samples, ardop_event *events,
			size_t max_events)
{
	size_t nev = 0;
	const int16_t *smp;
	int nsmp;

	/* Append to anything held back from last time. */
	if (d->raw_len) {
		memcpy(&d->raw[d->raw_len], samples, n * sizeof(int16_t));
		d->raw_len += (int)n;
		nsmp = d->raw_len;
		smp = d->raw;
	} else {
		nsmp = (int)n;
		smp = samples;
	}
	d->raw_len = 0;

	if (nsmp < 1024) {
		/* Not enough to work on yet; hold it. */
		memmove(d->raw, smp, (size_t)nsmp * sizeof(int16_t));
		d->raw_len = nsmp;
		return nev;
	}

	/* --- Searching for leader (on raw, unmixed samples) --- */
	if (d->state == ARDOP_RX_SEARCHING_FOR_LEADER) {
		while (d->state == ARDOP_RX_SEARCHING_FOR_LEADER && nsmp >= 1200) {
			int sn = 0;
			bool found = ardop_demod_leader_search(d, smp, nsmp,
							       now_samples, &sn);
			if (found) {
				ardop_event ev = {0};

				d->last_leader_detect = now_samples;
				nsmp -= 480;
				smp += 480;
				/* InitializeMixedSamples: filter delay offset. */
				d->filtered_mixed_len = 0;
				d->mfs_read_ptr = 30;
				d->state = ARDOP_RX_ACQUIRE_SYMBOL_SYNC;

				ev.kind = ARDOP_EV_LEADER_DETECTED;
				ev.offset_hz = d->offset_hz;
				ev.sn = sn;
				emit(events, &nev, max_events, &ev);
			} else {
				nsmp -= 240;  /* SlowCPU's 480 hop is dropped */
				smp += 240;
			}
		}
		if (d->state == ARDOP_RX_SEARCHING_FOR_LEADER) {
			memmove(d->raw, smp, (size_t)nsmp * sizeof(int16_t));
			d->raw_len = nsmp;
			return nev;
		}
	}

	/* Mix and filter all remaining samples to baseband. */
	ardop_demod_mix_filter(d, smp, nsmp, d->offset_hz);
	nsmp = 0;

	/* --- Acquire symbol sync --- */
	if (d->state == ARDOP_RX_ACQUIRE_SYMBOL_SYNC) {
		if ((d->filtered_mixed_len - d->mfs_read_ptr) > 860) {
			if (!ardop_demod_symbol_framing(d)) {
				d->state = ARDOP_RX_SEARCHING_FOR_LEADER;
				clear_mixed(d);
				return nev;
			}
			/* symbol_framing advanced to ACQUIRE_FRAME_SYNC. */
		}
	}

	/* --- Acquire frame sync --- */
	if (d->state == ARDOP_RX_ACQUIRE_FRAME_SYNC) {
		if (ardop_demod_frame_sync(d))
			d->state = ARDOP_RX_ACQUIRE_FRAME_TYPE;
		compact_mixed(d);
		/* 1000 ms (12000 samples) without frame sync: give up. */
		if ((now_samples - d->last_leader_detect) > 12000) {
			d->state = ARDOP_RX_SEARCHING_FOR_LEADER;
			clear_mixed(d);
		}
	}

	/* --- Acquire frame type --- */
	if (d->state == ARDOP_RX_ACQUIRE_FRAME_TYPE) {
		int ft = acquire_frame_type(d, now_samples);
		const ardop_frame_spec *spec;

		if (ft == -2)
			return nev;  /* insufficient samples */
		if (ft == -1) {
			d->state = ARDOP_RX_SEARCHING_FOR_LEADER;
			clear_mixed(d);
			return nev;
		}

		compact_mixed(d);
		spec = ardop_frame_spec_for((uint8_t)ft);
		if (spec == NULL) {
			d->state = ARDOP_RX_SEARCHING_FOR_LEADER;
			clear_mixed(d);
			return nev;
		}

		d->frame_type = (uint8_t)ft;

		if (is_short_control_frame(d->frame_type)) {
			/* No data follows; the frame is complete. */
			ardop_event ev = {0};

			d->state = ARDOP_RX_SEARCHING_FOR_LEADER;
			clear_mixed(d);
			ev.kind = ARDOP_EV_FRAME_DECODED;
			ev.frame_type = d->frame_type;
			emit(events, &nev, max_events, &ev);
			return nev;
		}

		/* Data-bearing frame: set up the streaming demod. */
		d->frame_num_car = spec->carriers;
		d->frame_baud = spec->baud;
		d->frame_data_len = spec->data_bytes_per_carrier;
		d->frame_rs_len = spec->rs_bytes_per_carrier;
		d->frame_mod = spec->modulation;
		d->frame_samp_per_sym = 12000 / spec->baud;

		/* Bytes to demodulate: data frames add a length byte + 2 CRC. */
		if (ardop_frame_is_data(d->frame_type))
			d->symbols_left = d->frame_data_len + d->frame_rs_len + 3;
		else
			d->symbols_left = d->frame_data_len + d->frame_rs_len;
		d->char_index = 0;
		d->phases_len = 0;

		if (d->frame_mod == ARDOP_MOD_4PSK)
			ardop_demod_psk_init(d, d->frame_num_car, 4);
		else if (d->frame_mod == ARDOP_MOD_8PSK)
			ardop_demod_psk_init(d, d->frame_num_car, 8);
		else if (d->frame_mod == ARDOP_MOD_16QAM)
			ardop_demod_psk_init(d, d->frame_num_car, 8);

		d->state = ARDOP_RX_ACQUIRE_FRAME;
	}

	/* --- Acquire frame: stream the data (single-carrier 4FSK) --- */
	if (d->state == ARDOP_RX_ACQUIRE_FRAME
	    && d->frame_mod == ARDOP_MOD_4FSK && d->frame_num_car == 1) {
		int start = 0;

		while (d->state == ARDOP_RX_ACQUIRE_FRAME) {
			int32_t tm[ARDOP_4FSK_CHAR_TONE_MAGS];
			int used = d->frame_samp_per_sym * 4;

			if (d->filtered_mixed_len < d->frame_samp_per_sym * 4.5) {
				/* Not enough for another byte; keep the tail. */
				if (d->filtered_mixed_len > 0)
					memmove(d->filtered_mixed,
						&d->filtered_mixed[start],
						(size_t)d->filtered_mixed_len
						* sizeof(int16_t));
				return nev;
			}

			d->frame_data[0][d->char_index] = ardop_demod_4fsk_char(
				d, start, 1500, d->frame_baud,
				d->frame_samp_per_sym, tm);
			d->char_index++;
			d->symbols_left--;
			start += used;
			d->filtered_mixed_len -= used;

			if (d->symbols_left == 0) {
				d->state = ARDOP_RX_SEARCHING_FOR_LEADER;
				clear_mixed(d);
				deliver_frame(d, events, &nev, max_events);
			}
		}
	}

	return nev;
}
