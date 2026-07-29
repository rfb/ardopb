#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/rxquality.h"

/*
 * The inherited Update4FSKConstellation is the oracle. It reads the symbol count
 * from the global intToneMagsLength and returns the quality through its second
 * argument (the constellation plotting is a no-op without a display). Declared
 * here rather than via SoundInput.c's headers.
 */
void Update4FSKConstellation(int *intToneMags, int *intQuality);
extern int intToneMagsLength;
extern int AccumulateStats;

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/*
 * The 4FSK quality must match Update4FSKConstellation across a range of symbol
 * counts and tone-magnitude patterns: clean (one tone dominant), noisy (tones
 * near-equal), and mixed, plus the dead-symbol (all-zero) case the rescale quirk
 * touches.
 */
static void test_quality_4fsk_matches_legacy(void **state)
{
	(void)state;

	AccumulateStats = 0;
	uint32_t rng = 0x4F5C0DE1u;

	for (int trial = 0; trial < 20000; trial++) {
		int nsym = 1 + (int)(xorshift32(&rng) % 40u);
		int len = nsym * 4;
		int mags[200];

		int shape = (int)(xorshift32(&rng) % 4u);
		for (int i = 0; i < len; i += 4) {
			if (shape == 0) {
				/* Clean: one dominant tone. */
				for (int t = 0; t < 4; t++)
					mags[i + t] = (int)(xorshift32(&rng)
							    % 500u);
				mags[i + (int)(xorshift32(&rng) % 4u)] +=
					20000 + (int)(xorshift32(&rng) % 40000u);
			} else if (shape == 1) {
				/* Noisy: all tones similar. */
				int base = 1000 + (int)(xorshift32(&rng)
							% 3000u);
				for (int t = 0; t < 4; t++)
					mags[i + t] = base
						+ (int)(xorshift32(&rng) % 500u);
			} else if (shape == 2) {
				/* A dead (all-zero) symbol among others. */
				for (int t = 0; t < 4; t++)
					mags[i + t] = 0;
			} else {
				for (int t = 0; t < 4; t++)
					mags[i + t] = (int)(xorshift32(&rng)
							    % 60000u);
			}
		}

		intToneMagsLength = len;
		int oq = 0;
		Update4FSKConstellation(mags, &oq);
		int pq = ardop_quality_4fsk((const int32_t *)mags, len);

		if (oq != pq)
			fail_msg("trial %d (nsym %d shape %d): legacy %d, port %d",
				 trial, nsym, shape, oq, pq);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_quality_4fsk_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
