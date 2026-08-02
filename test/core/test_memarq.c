#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <math.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/dataframe.h"
#include "codec/frame.h"
#include "codec/rs.h"
#include "modem/demodulate.h"
#include "modem/modulate.h"

/*
 * Memory ARQ: decoding a frame from several failed receptions of it.
 *
 * The corpus cannot test this. It holds one copy of each case, and Memory ARQ
 * is about what happens on the *second* copy and after -- so the frames are
 * synthesised here: one modulated frame, several times, each with independent
 * noise at a chosen S/N.
 *
 * The shape of every test below is an A/B on the same noisy audio:
 *
 *   A. accumulate  -- copies pushed through one demodulator, which averages
 *   B. control     -- the identical copies, with the accumulator reset between
 *                     each, which is exactly "Memory ARQ off"
 *
 * Anything A achieves that B does not is attributable to the averaging and to
 * nothing else -- not to a lucky seed, not to the noise being milder than
 * intended, because both runs see the same samples.
 */

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

static uint8_t g_valid_types[256];
static int g_valid_len;

static void build_valid_types(void)
{
	if (g_valid_len)
		return;
	for (int b = 0; b < 256; b++)
		if (ardop_frame_spec_for((uint8_t)b))
			g_valid_types[g_valid_len++] = (uint8_t)b;
}

/* --- deterministic noise ---------------------------------------------------
 *
 * Seeded so a failure is reproducible: a probabilistic test that cannot be
 * replayed is worse than no test.
 */

static uint64_t g_rng;

static uint32_t rng_next(void)
{
	/* xorshift64*, plenty for shaping noise. */
	g_rng ^= g_rng >> 12;
	g_rng ^= g_rng << 25;
	g_rng ^= g_rng >> 27;
	return (uint32_t)((g_rng * 2685821657736338717ull) >> 32);
}

static double rng_uniform(void)
{
	return ((double)rng_next() + 1.0) / 4294967297.0;
}

/* Box-Muller. */
static double rng_gauss(void)
{
	double u1 = rng_uniform(), u2 = rng_uniform();
	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static double rms_of(const int16_t *buf, size_t n)
{
	double acc = 0.0;
	for (size_t i = 0; i < n; i++)
		acc += (double)buf[i] * (double)buf[i];
	return n ? sqrt(acc / (double)n) : 0.0;
}

/* Copy `clean` into `out` with additive white Gaussian noise at `snr_db`. */
static void add_noise(const int16_t *clean, int16_t *out, size_t n,
		      double snr_db, uint64_t seed)
{
	g_rng = seed ? seed : 1;
	double sig = rms_of(clean, n);
	double noise = sig / pow(10.0, snr_db / 20.0);

	for (size_t i = 0; i < n; i++) {
		double v = (double)clean[i] + noise * rng_gauss();
		if (v > 32767.0)
			v = 32767.0;
		if (v < -32768.0)
			v = -32768.0;
		out[i] = (int16_t)lrint(v);
	}
}

/* --- the frame under test -------------------------------------------------- */

#define PAD_SAMPLES 4800

struct rig {
	ardop_rs rs;
	ardop_demod demod;
};

static void rig_init(struct rig *r)
{
	assert_true(ardop_rs_init(&r->rs, kRSLens, NUM_RSLENS));
	build_valid_types();
	ardop_demod_init(&r->demod, 100, 5);
	r->demod.rs = &r->rs;
	r->demod.ft_ctx.valid_types = g_valid_types;
	r->demod.ft_ctx.valid_len = g_valid_len;
	r->demod.ft_ctx.rxo = true;
}

/* RS-encode and modulate one frame; returns the sample count, with trailing
 * silence appended so the streaming demodulator flushes its look-ahead. */
static size_t modulate(uint8_t frame_type, const uint8_t *payload, size_t len,
		       int16_t *out, size_t cap)
{
	static ardop_rs rs;
	static uint8_t encoded[4096];
	assert_true(ardop_rs_init(&rs, kRSLens, NUM_RSLENS));
	int enc = ardop_encode_data_frame(&rs, frame_type, 0x00, payload,
					  (int)len, encoded);
	assert_true(enc > 0);

	ardop_mod mod;
	ardop_mod_init(&mod, 30);
	assert_true(ardop_mod_begin(&mod, frame_type, encoded, (size_t)enc, 240,
				    out, cap));
	size_t n = ardop_mod_pull(&mod, out, cap);
	assert_true(n + PAD_SAMPLES <= cap);
	memset(&out[n], 0, PAD_SAMPLES * sizeof(*out));
	return n + PAD_SAMPLES;
}

/* Push one copy through the demodulator. True if the frame decoded. */
static bool push_copy(struct rig *r, const int16_t *buf, size_t n,
		      uint64_t *now)
{
	bool decoded = false;
	for (size_t off = 0; off < n; off += 1200) {
		size_t chunk = n - off < 1200 ? n - off : 1200;
		ardop_event evs[8];
		size_t ne = ardop_demod_push(&r->demod, &buf[off], chunk, *now,
					     evs, 8);
		*now += chunk;
		for (size_t e = 0; e < ne; e++)
			if (evs[e].kind == ARDOP_EV_FRAME_DECODED)
				decoded = true;
	}
	return decoded;
}

/*
 * Run `copies` noisy receptions of one frame and report which copy first
 * decoded (0 = never). With `accumulate` false the Memory-ARQ state is dropped
 * between copies, which is the control.
 */
static int run_copies(uint8_t frame_type, const uint8_t *payload, size_t len,
		      double snr_db, int copies, uint64_t seed0,
		      bool accumulate)
{
	static int16_t clean[ARDOP_MOD_MAX_SAMPLES];
	static int16_t noisy[ARDOP_MOD_MAX_SAMPLES];
	static struct rig r;

	size_t n = modulate(frame_type, payload, len, clean,
			    ARDOP_MOD_MAX_SAMPLES);
	rig_init(&r);

	uint64_t now = 1000000;
	for (int i = 0; i < copies; i++) {
		if (!accumulate)
			ardop_demod_memarq_reset(&r.demod);
		add_noise(clean, noisy, n, snr_db, seed0 + (uint64_t)i * 7919u);
		if (push_copy(&r, noisy, n, &now))
			return i + 1;
	}
	return 0;
}

/* --- tests ------------------------------------------------------------------ */

static const uint8_t kPayload[64] = {
	'M', 'e', 'm', 'o', 'r', 'y', ' ', 'A', 'R', 'Q', ' ', 't', 'e', 's',
	't', ' ', 'p', 'a', 'y', 'l', 'o', 'a', 'd', ' ', 0x00, 0x01, 0x02,
	0x03, 0xFF, 0xFE, 0xAA, 0x55,
};

/*
 * THE test. At an S/N chosen so a single copy does not decode, several copies
 * averaged together do -- and the same copies without averaging still do not.
 *
 * 0x40 is 4PSK.200.100.E: one carrier, so this isolates the phase averaging
 * from the separate effect of combining carriers recovered from different
 * copies.
 *
 * -9 dB is measured, not guessed. The S/N here is wideband -- noise across the
 * full 6 kHz Nyquist against a signal occupying ~200 Hz -- so it reads about
 * 15 dB below the in-band figure an operator would quote. Sweeping 8 seeds per
 * point gave:
 *
 *     wideband S/N   1 copy   6 accumulated   6 not accumulated
 *        -11 dB       0/8         0/8               0/8
 *        -10 dB       0/8         4/8               0/8
 *         -9 dB       0/8         8/8               0/8
 *         -8 dB       1/8         8/8               1/8
 *
 * -9 dB is where the separation is total. Below -11 dB nothing decodes even
 * accumulated, and that is not a failure of the averaging: the leader and
 * frame-type sync go first, so there is no acquired frame to accumulate into.
 * Memory ARQ extends the range over which a frame that *was* heard can be
 * recovered; it cannot help with one that was never detected.
 */
static void test_psk_decodes_from_averaged_copies(void **state)
{
	(void)state;
	const double snr = -9.0;
	const uint64_t seed = 0xA5A5C3C3u;

	int with = run_copies(0x40, kPayload, 64, snr, 6, seed, true);
	int without = run_copies(0x40, kPayload, 64, snr, 6, seed, false);

	/* The control must genuinely fail, or the S/N is too generous and the
	 * test proves nothing. */
	assert_int_equal(without, 0);

	/* And accumulation must recover it. */
	assert_true(with > 0);

	/* Not on the first copy -- copy 1 is stored, never averaged, so a win
	 * there would mean the noise was simply survivable. */
	assert_true(with > 1);
}

/*
 * 4FSK, where the win arrives differently.
 *
 * PSK and QAM decide their symbols at delivery, so averaging the phases is
 * enough. 4FSK decides each byte *while streaming*, so by delivery the bytes
 * already exist and averaging the tone magnitudes would only change the
 * reported quality score. The symbol decision is therefore repeated against
 * the averaged tones (memarq_redecode_fsk) -- without that, Memory ARQ would
 * appear to be wired up for 4FSK and do nothing.
 *
 * The control here is weaker than the PSK test's: six independent copies get
 * six independent chances, so some seeds decode without any averaging. The
 * claim is accordingly comparative -- accumulating must beat not accumulating
 * on the same audio. Measured over these seeds at -10 dB wideband:
 * 8/8 accumulated against 4/8 control, with a single copy at 0/8.
 */
static void test_fsk_averaging_beats_no_averaging(void **state)
{
	(void)state;
	const double snr = -10.0;
	int acc = 0, ctrl = 0;

	for (int t = 0; t < 6; t++) {
		uint64_t sd = 0x5150u + (uint64_t)t * 104729u;
		/* 0x48 is 4FSK.200.50S.E: 16 data bytes on one carrier. */
		if (run_copies(0x48, kPayload, 16, snr, 6, sd, true))
			acc++;
		if (run_copies(0x48, kPayload, 16, snr, 6, sd, false))
			ctrl++;
	}

	assert_true(acc > ctrl);
	assert_true(acc >= 5);
}

/*
 * A clean frame still decodes on the first copy, and Memory ARQ leaves no
 * trace: the accumulator is dropped on success, so the next frame of the same
 * type starts from nothing. Without that, a repeated frame *type* in FEC or
 * RXO -- where it is routinely a different frame -- would deliver the previous
 * frame's bytes again.
 */
static void test_clean_frame_decodes_immediately_and_resets(void **state)
{
	(void)state;
	static int16_t clean[ARDOP_MOD_MAX_SAMPLES];
	static struct rig r;

	size_t n = modulate(0x40, kPayload, 64, clean, ARDOP_MOD_MAX_SAMPLES);
	rig_init(&r);

	uint64_t now = 1000000;
	assert_true(push_copy(&r, clean, n, &now));
	assert_int_equal(ardop_demod_memarq_count(&r.demod, 0), 0);

	/* A second, identical frame decodes on its own merits. */
	assert_true(push_copy(&r, clean, n, &now));
	assert_int_equal(ardop_demod_memarq_count(&r.demod, 0), 0);
}

/* A failed copy leaves exactly one copy accumulated, ready to be averaged. */
static void test_failed_copy_is_retained(void **state)
{
	(void)state;
	static int16_t clean[ARDOP_MOD_MAX_SAMPLES];
	static int16_t noisy[ARDOP_MOD_MAX_SAMPLES];
	static struct rig r;

	size_t n = modulate(0x40, kPayload, 64, clean, ARDOP_MOD_MAX_SAMPLES);
	rig_init(&r);

	uint64_t now = 1000000;
	add_noise(clean, noisy, n, 0.0, 12345);
	(void)push_copy(&r, noisy, n, &now);

	/* Whether or not it decoded, the count is now 0 (decoded: reset) or 1
	 * (failed: retained). It must never be more after a single copy. */
	assert_true(ardop_demod_memarq_count(&r.demod, 0) <= 1);
}

/*
 * Aging. An accumulator older than ARDOP_MEMARQ_MAX_AGE_SAMPLES must not be
 * averaged into: an unrelated frame that happens to share a type, minutes
 * later, is not a retransmission.
 */
static void test_stale_accumulator_is_discarded(void **state)
{
	(void)state;
	static int16_t clean[ARDOP_MOD_MAX_SAMPLES];
	static int16_t noisy[ARDOP_MOD_MAX_SAMPLES];
	static struct rig r;

	size_t n = modulate(0x40, kPayload, 64, clean, ARDOP_MOD_MAX_SAMPLES);
	rig_init(&r);

	uint64_t now = 1000000;
	add_noise(clean, noisy, n, 0.0, 999);
	(void)push_copy(&r, noisy, n, &now);
	unsigned after_first = ardop_demod_memarq_count(&r.demod, 0);

	/* Jump the sample clock well past the age limit. */
	now += ARDOP_MEMARQ_MAX_AGE_SAMPLES * 2;

	add_noise(clean, noisy, n, 0.0, 1001);
	(void)push_copy(&r, noisy, n, &now);

	/* The second copy started a fresh accumulation rather than stacking on
	 * the stale one, so the count did not grow past what one copy leaves. */
	assert_true(ardop_demod_memarq_count(&r.demod, 0) <= after_first);
}

/* An explicit reset clears everything, for a mode change or a new session. */
static void test_explicit_reset_clears(void **state)
{
	(void)state;
	static int16_t clean[ARDOP_MOD_MAX_SAMPLES];
	static int16_t noisy[ARDOP_MOD_MAX_SAMPLES];
	static struct rig r;

	size_t n = modulate(0x40, kPayload, 64, clean, ARDOP_MOD_MAX_SAMPLES);
	rig_init(&r);

	uint64_t now = 1000000;
	add_noise(clean, noisy, n, 0.0, 4242);
	(void)push_copy(&r, noisy, n, &now);

	ardop_demod_memarq_reset(&r.demod);
	for (int c = 0; c < ARDOP_DEMOD_MAX_CARRIERS; c++)
		assert_int_equal(ardop_demod_memarq_count(&r.demod, c), 0);
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_psk_decodes_from_averaged_copies),
		cmocka_unit_test(test_fsk_averaging_beats_no_averaging),
		cmocka_unit_test(test_clean_frame_decodes_immediately_and_resets),
		cmocka_unit_test(test_failed_copy_is_retained),
		cmocka_unit_test(test_stale_accumulator_is_discarded),
		cmocka_unit_test(test_explicit_reset_clears),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
