#include "modem/rxquality.h"

/**
 * @file rxquality.c
 * @brief Constellation decode-quality metrics, ported from the quality paths of
 *        Update4FSKConstellation (SoundInput.c).
 */

/* The plot radius the original rescales by; load-bearing, see the header. */
#define PLOT_RADIUS 42

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
