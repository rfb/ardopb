#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/demodulate.h"
#include "modem/modulate.h"

/*
 * Oracle: the inherited SearchFor2ToneLeader3 and the globals it reads/writes.
 * getTicks() returns WavNow whenever DecodeWav[0][0] is set, so setting those
 * two makes the original's clock deterministic in a unit test.
 */
int SearchFor2ToneLeader3(short *samples, int length, float *offset, int *sn);
extern char DecodeWav[5][256];
extern int WavNow;
extern int TuningRange;
extern int Squelch;
extern int AccumulateStats;
extern float dblPriorFineOffset;
extern int dttLastGoodFrameTypeDecode;

/*
 * Oracle for stage 2: MixNCOFilter downmixes and filters, appending to
 * intFilteredMixedSamples. It reads/writes the NCO phase, the resonator delay
 * lines, the coefficients and the prior-sample history -- all file-scope in the
 * original -- so a clean run means zeroing them first (and clearing coef[26] to
 * force the lazy recompute).
 */
void MixNCOFilter(short *samples, int length, float offset);
extern float dblNCOPhase;
extern float xdblZin_1, xdblZin_2, xdblZComb;
extern float xdblZout_0[27], xdblZout_1[27], xdblZout_2[27], xdblCoef[27];
extern short intPriorMixedSamples[120];
extern short intFilteredMixedSamples[5000];
extern int intFilteredMixedSamplesLength;

/* Full-search branch: Now - lastGoodDecode must exceed 20000 ms. */
#define NOW_MS 1000000
#define NOW_SAMPLES ((uint64_t)NOW_MS * 12)

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static bool feq(float a, float b)
{
	return memcmp(&a, &b, sizeof(float)) == 0;
}

/*
 * Run the original and the port on the same window from the same state and
 * require identical results: the return, the persistent prior-offset state
 * (which drives the two-probe detection), and the tuning offset and S/N when a
 * leader is found.
 */
static void expect_same(short *samp, int length, float prior_offset)
{
	DecodeWav[0][0] = 'x';
	WavNow = NOW_MS;
	TuningRange = 100;
	Squelch = 5;
	AccumulateStats = 0;
	dttLastGoodFrameTypeDecode = 0;
	dblPriorFineOffset = 1000.0f;

	float loff = prior_offset;
	int lsn = -99999;
	int lret = SearchFor2ToneLeader3(samp, length, &loff, &lsn);

	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	d.offset_hz = prior_offset;
	int msn = -99999;
	bool mret = ardop_demod_leader_search(&d, samp, length, NOW_SAMPLES, &msn);

	if ((lret != 0) != mret)
		fail_msg("return: legacy %d, port %d", lret, mret);
	if (!feq(dblPriorFineOffset, d.prior_fine_offset))
		fail_msg("prior_fine_offset: legacy %.9g, port %.9g",
			 (double)dblPriorFineOffset, (double)d.prior_fine_offset);
	if (lret) {
		if (!feq(loff, d.offset_hz))
			fail_msg("offset: legacy %.9g, port %.9g",
				 (double)loff, (double)d.offset_hz);
		if (lsn != msn)
			fail_msg("S/N: legacy %d, port %d", lsn, msn);
	}
}

/*
 * On random noise the search should agree at every step -- it exercises the
 * whole DSP path (Goertzels, peak interpolation, thresholds) even though it
 * settles on "no leader".
 */
static void test_matches_legacy_on_noise(void **state)
{
	(void)state;

	uint32_t rng = 0xA1B2C3D4u;
	static short buf[4096];
	for (int i = 0; i < 4096; i++)
		buf[i] = (short)xorshift32(&rng);

	for (int t = 0; t < 200; t++) {
		int ptr = (int)(xorshift32(&rng) % (4096 - 1200));
		/* Prior offsets across the tuning range, including 1000 (none). */
		float prior = (float)((int)(xorshift32(&rng) % 200u) - 100);
		expect_same(&buf[ptr], 1200, prior);
	}
}

/* Below 1200 samples the search must decline, like the original. */
static void test_short_window_declines(void **state)
{
	(void)state;

	short buf[1200] = {0};
	expect_same(buf, 1199, 0.0f);
	expect_same(buf, 800, 25.0f);
}

/* Build the leader the modulator actually transmits for a BREAK frame. */
static int make_leader(short *out, int cap)
{
	static int16_t frame[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod m;
	ardop_mod_init(&m, 30);
	const uint8_t enc[2] = { 0x23, 0x23 };
	assert_true(ardop_mod_begin(&m, 0x23, enc, sizeof(enc), 240, frame,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t n = ardop_mod_pull(&m, frame, ARDOP_MOD_MAX_SAMPLES);
	int copy = (int)n < cap ? (int)n : cap;
	for (int i = 0; i < copy; i++)
		out[i] = (short)frame[i];
	return copy;
}

/*
 * The real join: feed the modulator's own leader into the receiver. On windows
 * of that leader the port and the original must still agree exactly, and -- the
 * point -- the two-probe sequence must actually detect it, near zero offset.
 */
static void test_detects_modulator_leader(void **state)
{
	(void)state;

	static short leader[4096];
	int n = make_leader(leader, 4096);
	assert_true(n >= 2400);

	/* Equivalence across several windows of the genuine leader. */
	for (int off = 300; off + 1200 <= 2400; off += 300)
		expect_same(&leader[off], 1200, 0.0f);

	/*
	 * Detection needs two consistent probes: the first records the fine
	 * offset, the second confirms it. Drive the port through that sequence
	 * on a steady leader window and require a detection near 0 Hz.
	 */
	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	int sn = 0;
	bool first = ardop_demod_leader_search(&d, &leader[600], 1200,
					       NOW_SAMPLES, &sn);
	bool second = ardop_demod_leader_search(&d, &leader[600], 1200,
						NOW_SAMPLES, &sn);
	assert_false(first);   /* first probe only records the offset */
	assert_true(second);   /* second confirms and detects */
	assert_int_equal(d.state, ARDOP_RX_ACQUIRE_SYMBOL_SYNC);
	assert_true(d.offset_hz > -5.0f && d.offset_hz < 5.0f);
}

/*
 * Feed one block through the original MixNCOFilter and the port from the same
 * zeroed state and require the appended baseband to be bit-identical, sample
 * count and all. This exercises the whole stage-2 path: the NCO downmix, the
 * comb, and the 23 resonators.
 */
static void expect_mix_same(const int16_t *in, int length, float offset)
{
	dblNCOPhase = 0;
	xdblZin_1 = xdblZin_2 = xdblZComb = 0;
	memset(xdblZout_0, 0, sizeof xdblZout_0);
	memset(xdblZout_1, 0, sizeof xdblZout_1);
	memset(xdblZout_2, 0, sizeof xdblZout_2);
	memset(xdblCoef, 0, sizeof xdblCoef);
	memset(intPriorMixedSamples, 0, sizeof intPriorMixedSamples);
	intFilteredMixedSamplesLength = 0;

	static short in_copy[2400];
	assert_true(length <= 2400);
	for (int i = 0; i < length; i++)
		in_copy[i] = in[i];
	MixNCOFilter(in_copy, length, offset);

	int olen = intFilteredMixedSamplesLength;
	static short oracle[5000];
	for (int i = 0; i < olen; i++)
		oracle[i] = intFilteredMixedSamples[i];

	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	ardop_demod_mix_filter(&d, in, length, offset);

	if (d.filtered_mixed_len != olen)
		fail_msg("length: legacy %d, port %d", olen,
			 d.filtered_mixed_len);
	for (int i = 0; i < olen; i++)
		if (d.filtered_mixed[i] != oracle[i])
			fail_msg("sample %d: legacy %d, port %d", i,
				 oracle[i], d.filtered_mixed[i]);
}

/*
 * Random noise across a range of tuning offsets: the NCO frequency and the
 * whole filter must track the original exactly regardless of offset.
 */
static void test_mix_matches_legacy_on_noise(void **state)
{
	(void)state;

	uint32_t rng = 0x51EED123u;
	static short buf[2400];
	for (int i = 0; i < 2400; i++)
		buf[i] = (short)xorshift32(&rng);

	for (int t = 0; t < 100; t++) {
		int length = 120 + (int)(xorshift32(&rng) % (2400 - 120 + 1));
		float offset = (float)((int)(xorshift32(&rng) % 200u) - 100);
		expect_mix_same(buf, length, offset);
	}
}

/*
 * The join again, one layer deeper: the modulator's own leader, downmixed and
 * filtered by both, sample-for-sample. Multiple contiguous blocks also confirm
 * the NCO phase and filter delay lines carry across calls -- feed the same span
 * as one block and as two, and the concatenated output must match.
 */
static void test_mix_matches_legacy_on_leader(void **state)
{
	(void)state;

	static short leader[4096];
	int n = make_leader(leader, 4096);
	assert_true(n >= 2400);

	expect_mix_same(leader, 1200, 0.0f);
	expect_mix_same(leader, 2400, 12.0f);
	expect_mix_same(&leader[240], 600, -37.5f);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_matches_legacy_on_noise),
		cmocka_unit_test(test_short_window_declines),
		cmocka_unit_test(test_detects_modulator_leader),
		cmocka_unit_test(test_mix_matches_legacy_on_noise),
		cmocka_unit_test(test_mix_matches_legacy_on_leader),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
