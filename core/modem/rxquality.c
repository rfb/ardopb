#include "modem/rxquality.h"

#include <math.h>

/**
 * @file rxquality.c
 * @brief Constellation decode-quality metrics, ported from the quality paths of
 *        Update4FSKConstellation / UpdatePhaseConstellation (SoundInput.c).
 */

/* The plot radius the original rescales by; load-bearing, see the header. */
#define PLOT_RADIUS 42

/* The reduced-precision two-pi the whole modem uses (M_PI redefined to a float
 * literal on the air-side path); dbPhaseStep is derived from it. */
#define ARDOP_2PI (2 * 3.1415926f)

/* One symbol's contribution: `others` is the sum of the three non-dominant
 * tones. The 5-floor is applied only when there is energy, matching the
 * original's `max(5, ...)` sitting inside its `sum > 0` guard; on a dead symbol
 * `rad` keeps the previous symbol's rescaled value. */
static void accumulate(int32_t others, int32_t sum, int *rad,
		       float *distance_sum)
{
	if (sum > 0) {
		*rad = 42 - (int)(80 * others / sum);
		if (*rad < 5)
			*rad = 5;
	}
	*distance_sum += (float)(42 - *rad);
	*rad = (*rad * PLOT_RADIUS) / 50;
}

int ardop_quality_4fsk(const int32_t *tone_mags, int tone_mags_len)
{
	int rad = 0;
	float distance_sum = 0;

	for (int i = 0; i < tone_mags_len; i += 4) {
		int32_t t0 = tone_mags[i];
		int32_t t1 = tone_mags[i + 1];
		int32_t t2 = tone_mags[i + 2];
		int32_t t3 = tone_mags[i + 3];
		int32_t sum = t0 + t1 + t2 + t3;

		/* Find the dominant tone; the comparisons and the sum > 0 guard on
		 * the final branch match the original exactly. */
		if (t0 > t1 && t0 > t2 && t0 > t3)
			accumulate(t1 + t2 + t3, sum, &rad, &distance_sum);
		else if (t1 > t0 && t1 > t2 && t1 > t3)
			accumulate(t0 + t2 + t3, sum, &rad, &distance_sum);
		else if (t2 > t0 && t2 > t1 && t2 > t3)
			accumulate(t1 + t0 + t3, sum, &rad, &distance_sum);
		else if (sum > 0)
			accumulate(t1 + t2 + t0, sum, &rad, &distance_sum);
	}

	int quality = (int)(100 - (2.7f * (distance_sum
					   / (float)(tone_mags_len / 4))));
	if (quality < 0)
		quality = 0;
	else if (quality > 100)
		quality = 100;
	return quality;
}

int ardop_quality_psk(const short *phases, const short *mags, int phases_len,
		      int psk_order, bool is_qam)
{
	float phase_step;
	float phase_error_sum = 0;
	float avg_rad_inner = 0, avg_rad_outer = 0;
	float rad_error_inner = 0, rad_error_outer = 0;
	int rad_inner = 0, rad_outer = 0;
	float mag_max = 0;

	if (is_qam) {
		psk_order = 8;
		phase_step = ARDOP_2PI / (float)psk_order;
		/* Auto-scale off the peak, then split into an inner/outer ring. */
		for (int j = 1; j < phases_len; j++)
			mag_max = mag_max > mags[j] ? mag_max : (float)mags[j];
		for (int k = 1; k < phases_len; k++) {
			if (mags[k] < 0.75f * mag_max) {
				avg_rad_inner += (float)mags[k];
				rad_inner++;
			} else {
				avg_rad_outer += (float)mags[k];
				rad_outer++;
			}
		}
		avg_rad_inner = avg_rad_inner / (float)rad_inner;
		avg_rad_outer = avg_rad_outer / (float)rad_outer;
	} else {
		phase_step = ARDOP_2PI / (float)psk_order;
		for (int j = 1; j < phases_len; j++)
			mag_max = mag_max > mags[j] ? mag_max : (float)mags[j];
	}

	for (int i = 1; i < phases_len; i++) {
		/* Nearest constellation point (0.001f converts milliradians). */
		int p = (int)round((0.001f * (float)phases[i]) / phase_step);

		if ((float)mags[i] > (avg_rad_inner + avg_rad_outer) / 2)
			rad_error_outer += fabsf(avg_rad_outer - (float)mags[i]);
		else
			rad_error_inner += fabsf(avg_rad_inner - (float)mags[i]);

		/* The phase error uses double 0.001 (not 0.001f) while p*step is
		 * computed in float then promoted, matching the original's mixed
		 * literals, before narrowing in fabsf. */
		float phase_error = fabsf((float)((0.001 * phases[i])
						  - (float)p * phase_step));
		phase_error_sum += phase_error;
	}

	float phase_term = 100.0f
			   - 200.0f * (phase_error_sum / (float)phases_len)
				     / phase_step;
	float q;
	if (is_qam) {
		q = (1.0f
		     - (rad_error_inner / ((float)rad_inner * avg_rad_inner)
			+ rad_error_outer / ((float)rad_outer * avg_rad_outer)))
		    * phase_term;
	} else {
		q = phase_term;
	}

	return (int)(q < 0 ? 0 : q);
}
