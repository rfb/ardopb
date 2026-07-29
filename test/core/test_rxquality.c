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
int UpdatePhaseConstellation(short *intPhases, short *intMag, char *strMod,
			     int blnQAM);
extern int intToneMagsLength;
extern int intPhasesLen;
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

/*
 * The PSK/QAM quality must match UpdatePhaseConstellation across 4PSK, 8PSK and
 * 16QAM over random differential phases and magnitudes. QAM magnitudes are drawn
 * bimodally so both the inner and outer amplitude rings are populated (the ring
 * averages would divide by zero otherwise, in the port and the original alike).
 */
static void test_quality_psk_matches_legacy(void **state)
{
	(void)state;

	AccumulateStats = 0;
	uint32_t rng = 0x9B5CAFE3u;

	const struct { char mod[8]; int order; int qam; } modes[] = {
		{"4PSK", 4, 0}, {"8PSK", 8, 0}, {"16QAM", 8, 1},
	};

	for (int trial = 0; trial < 20000; trial++) {
		int m = (int)(xorshift32(&rng) % 3u);
		int len = 2 + (int)(xorshift32(&rng) % 40u);
		short phases[64], mags[64];

		for (int i = 0; i < len; i++) {
			/* Milliradians in (-pi, pi]. */
			phases[i] = (short)((int)(xorshift32(&rng) % 6284u)
					    - 3142);
			if (modes[m].qam) {
				/* Bimodal: inner (~4-8k) or outer (~14-20k). */
				if (xorshift32(&rng) & 1u)
					mags[i] = (short)(4000 + (int)(
						xorshift32(&rng) % 4000u));
				else
					mags[i] = (short)(14000 + (int)(
						xorshift32(&rng) % 6000u));
			} else {
				mags[i] = (short)(xorshift32(&rng) % 30000u);
			}
		}

		short ophases[64], omags[64];
		char mod[8];
		for (int i = 0; i < len; i++) {
			ophases[i] = phases[i];
			omags[i] = mags[i];
		}
		for (int i = 0; i < 8; i++)
			mod[i] = modes[m].mod[i];

		intPhasesLen = len;
		int oq = UpdatePhaseConstellation(ophases, omags, mod,
						  modes[m].qam);
		int pq = ardop_quality_psk(phases, mags, len, modes[m].order,
					   modes[m].qam != 0);

		if (oq != pq)
			fail_msg("trial %d (%s len %d): legacy %d, port %d",
				 trial, modes[m].mod, len, oq, pq);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_quality_4fsk_matches_legacy),
		cmocka_unit_test(test_quality_psk_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
