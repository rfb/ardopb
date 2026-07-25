#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/crc.h"
#include "codec/frame.h"
#include "codec/rs.h"
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

/*
 * Oracle for stage 3: the symbol-framing and frame-sync searches. Both read the
 * baseband in intFilteredMixedSamples from intMFSReadPtr and advance it; the
 * frame-sync records the leader length in intLeaderRcvdMs. AccumulateStats is
 * held at 0 so they don't touch the stats globals.
 */
int Acquire2ToneLeaderSymbolFraming(void);
int AcquireFrameSyncRSB(void);
extern int intMFSReadPtr;
extern int intLeaderRcvdMs;

/*
 * Oracle for stage 4: the frame-type tone demod and the per-candidate decode
 * distance. UseSDFT is forced off so DemodFrameType4FSK takes the Goertzel path
 * the port mirrors.
 */
int DemodFrameType4FSK(int intPtr, short *intSamples, int *intToneMags);
float ComputeDecodeDistance(int intTonePtr, int *intToneMags,
			    unsigned char bytFrameType, unsigned char bytID);
extern int UseSDFT;

/*
 * Oracle for stage 5 (4FSK): Demod1Car4FSKChar demodulates four 4FSK symbols
 * into Decoded[charIndex] and writes 16 tone mags at intToneMags[intToneMagsIndex].
 * The carrier params come from the globals below.
 */
void Demod1Car4FSKChar(int Start, unsigned char *Decoded);
extern int intCenterFreq;
extern int intBaud;
extern int intSampPerSym;
extern int intToneMagsIndex;
extern int charIndex;
extern int intToneMags[];

/*
 * Oracle for stage 5 decode: CorrectRawDataWithRS RS-corrects a carrier block
 * and CRC-checks it, recording success in CarrierOk[Carrier]. init_rs sets up
 * the rockliff tables it uses, mirroring ardop_rs_init for the port.
 */
int CorrectRawDataWithRS(unsigned char *raw, unsigned char *corrected,
			 int data_len, int rs_len, int frame_type, int carrier);
int init_rs(int *lengths, int count);
extern char CarrierOk[8];

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))
static ardop_rs g_rs;

/*
 * Oracle for stage 5 (PSK): InitDemodPSK sets up per-carrier frequency bins and
 * reference phases from the training symbol; Demod1CarPSKChar demodulates
 * psk_mode symbols per carrier into intPhases/intMags. The carrier count comes
 * from intNumCar and the PSK order from strMod[0].
 */
void InitDemodPSK(void);
int Demod1CarPSKChar(int Start, int Carrier);
void Decode1CarPSK(unsigned char *Decoded, int Carrier);
extern char strMod[16];
extern int intNumCar;
extern int intPSKMode;
extern int intPhasesLen;
extern short intPhases[8][520];
extern short intMags[8][520];
extern short intPSKPhase_1[8];
extern short intNforGoertzel[8];
extern short intCP[8];
extern short intCarMagThreshold[8];
extern float dblFreqBin[8];

/*
 * Oracle for stage 4b: the minimal-distance acceptance decision. It reads the
 * connection state from globals (ProtocolState selects the valid-types list;
 * blnPending/blnARQConnected/bytLastARQSessionID pick the branch) and, on a
 * confident decode, sets dttLastGoodFrameTypeDecode = Now. The test drives all
 * of those and detects the timestamp write to compare against set_last_good.
 */
int MinimalDistanceFrameType(int *intToneMags, unsigned char bytSessionID);
extern int ProtocolState;   /* enum _ARDOPState; ISS == 2 */
extern int blnPending;
extern int blnARQConnected;
extern unsigned char bytLastARQSessionID;
extern float dblOffsetHz;
extern const unsigned char bytValidFrameTypesALL[];
extern const unsigned char bytValidFrameTypesISS[];
extern int bytValidFrameTypesLengthALL;
extern int bytValidFrameTypesLengthISS;

#define ISS_STATE 2   /* enum _ARDOPState: OFFLINE, DISC, ISS, ... */
#define NON_ISS_STATE 1

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

/*
 * Downmix a sample stream to baseband with the proven stage-2 filter, mirror it
 * into the oracle's buffer, then run symbol framing and frame sync in both and
 * require identical results: the returns, the advancing read pointer, and (on a
 * sync) the recorded leader length. Same baseband into both, so this isolates
 * the stage-3 logic.
 */
static void run_sync_compare(const int16_t *samples, int total, float offset)
{
	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	for (int off = 0; off < total; off += 1200) {
		int len = total - off < 1200 ? total - off : 1200;
		ardop_demod_mix_filter(&d, &samples[off], len, offset);
	}
	assert_true(d.filtered_mixed_len <= 5000);

	AccumulateStats = 0;
	intFilteredMixedSamplesLength = d.filtered_mixed_len;
	for (int i = 0; i < d.filtered_mixed_len; i++)
		intFilteredMixedSamples[i] = d.filtered_mixed[i];
	intMFSReadPtr = 30;
	intLeaderRcvdMs = 0;
	d.mfs_read_ptr = 30;

	int oframe = Acquire2ToneLeaderSymbolFraming();
	bool mframe = ardop_demod_symbol_framing(&d);
	if ((oframe != 0) != mframe)
		fail_msg("framing return: legacy %d, port %d", oframe, mframe);
	if (intMFSReadPtr != d.mfs_read_ptr)
		fail_msg("framing ptr: legacy %d, port %d", intMFSReadPtr,
			 d.mfs_read_ptr);

	if (oframe) {
		int osync = AcquireFrameSyncRSB();
		int optr = intMFSReadPtr;
		int oms = intLeaderRcvdMs;
		bool msync = ardop_demod_frame_sync(&d);
		if ((osync != 0) != msync)
			fail_msg("sync return: legacy %d, port %d", osync, msync);
		if (optr != d.mfs_read_ptr)
			fail_msg("sync ptr: legacy %d, port %d", optr,
				 d.mfs_read_ptr);
		if (osync && oms != d.leader_rcvd_ms)
			fail_msg("leader ms: legacy %d, port %d", oms,
				 d.leader_rcvd_ms);
	}
}

/* The modulator's own frame, downmixed and run through both sync searches. */
static void test_sync_matches_legacy_on_frame(void **state)
{
	(void)state;

	static int16_t frame[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod m;
	ardop_mod_init(&m, 30);
	const uint8_t enc[2] = { 0x23, 0x23 };
	assert_true(ardop_mod_begin(&m, 0x23, enc, sizeof(enc), 240, frame,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t n = ardop_mod_pull(&m, frame, ARDOP_MOD_MAX_SAMPLES);
	int total = (int)n < 4800 ? (int)n : 4800;  /* keep baseband < 5000 */
	assert_true(total >= 3600);

	run_sync_compare(frame, total, 0.0f);

	/*
	 * And the point: on the real frame this is a detection, not vacuous
	 * agreement. Framing must succeed and advance to frame sync, and the
	 * sync search must lock onto the sync symbol just past the leader.
	 */
	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	for (int off = 0; off < total; off += 1200) {
		int len = total - off < 1200 ? total - off : 1200;
		ardop_demod_mix_filter(&d, &frame[off], len, 0.0f);
	}
	d.mfs_read_ptr = 30;
	assert_true(ardop_demod_symbol_framing(&d));
	assert_int_equal(d.state, ARDOP_RX_ACQUIRE_FRAME_SYNC);
	assert_true(ardop_demod_frame_sync(&d));
	assert_true(d.leader_rcvd_ms > 150 && d.leader_rcvd_ms < 260);
}

/*
 * Noise baseband across the tuning range: symbol framing always returns (it
 * only declines on too-few samples) and both must agree on the pointer even
 * when the correlation is junk.
 */
static void test_sync_matches_legacy_on_noise(void **state)
{
	(void)state;

	uint32_t rng = 0x9E3779B9u;
	static int16_t buf[4800];
	for (int i = 0; i < 4800; i++)
		buf[i] = (int16_t)xorshift32(&rng);

	for (int t = 0; t < 20; t++) {
		int len = 1200 + (int)(xorshift32(&rng) % (4800 - 1200 + 1));
		float offset = (float)((int)(xorshift32(&rng) % 200u) - 100);
		run_sync_compare(buf, len, offset);
	}
}

/*
 * Frame-type tone magnitudes on the modulator's own frame: advance through
 * stages 1-3 to the frame-type field, then require the port's 40 magnitudes to
 * match DemodFrameType4FSK bit-for-bit -- and, as a real detection check, that
 * the frame's own type (BREAK, 0x23) scores a small decode distance.
 */
static void test_frametype_matches_legacy_on_frame(void **state)
{
	(void)state;

	/*
	 * A short (120 ms) leader keeps the whole frame-type field inside the
	 * 5000-sample baseband buffer. The live pipeline instead compacts the
	 * buffer as the read pointer advances; that is stage 6's job, so here we
	 * just keep the leader short enough not to need it.
	 */
	static int16_t frame[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod m;
	ardop_mod_init(&m, 30);
	const uint8_t enc[2] = { 0x23, 0x23 };
	assert_true(ardop_mod_begin(&m, 0x23, enc, sizeof(enc), 120, frame,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t nn = ardop_mod_pull(&m, frame, ARDOP_MOD_MAX_SAMPLES);
	int total = (int)nn < 4900 ? (int)nn : 4900;

	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	for (int off = 0; off < total; off += 1200) {
		int len = total - off < 1200 ? total - off : 1200;
		ardop_demod_mix_filter(&d, &frame[off], len, 0.0f);
	}
	d.mfs_read_ptr = 30;
	assert_true(ardop_demod_symbol_framing(&d));
	assert_true(ardop_demod_frame_sync(&d));  /* now at frame-type field */

	int ptr = d.mfs_read_ptr;
	assert_true(d.filtered_mixed_len - ptr >= 2400);

	UseSDFT = 0;
	intFilteredMixedSamplesLength = d.filtered_mixed_len;
	for (int i = 0; i < d.filtered_mixed_len; i++)
		intFilteredMixedSamples[i] = d.filtered_mixed[i];

	int omags[ARDOP_FRAMETYPE_TONE_MAGS];
	assert_true(DemodFrameType4FSK(ptr, intFilteredMixedSamples, omags));

	int32_t pmags[ARDOP_FRAMETYPE_TONE_MAGS];
	assert_true(ardop_demod_frametype_tonemags(&d, ptr, pmags));

	for (int i = 0; i < ARDOP_FRAMETYPE_TONE_MAGS; i++)
		if (pmags[i] != omags[i])
			fail_msg("mag %d: legacy %d, port %d", i, omags[i],
				 pmags[i]);

	/* The real frame's own type scores low, and both agree exactly. */
	float od = ComputeDecodeDistance(0, omags, 0x23, 0);
	float pd = ardop_frametype_decode_distance(pmags, 0, 0x23, 0);
	assert_true(feq(od, pd));
	assert_true(pd < 0.3f);
}

/* The decode distance must match ComputeDecodeDistance for arbitrary tones. */
static void test_decode_distance_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xC0FFEE11u;
	for (int t = 0; t < 2000; t++) {
		int omags[ARDOP_FRAMETYPE_TONE_MAGS];
		int32_t pmags[ARDOP_FRAMETYPE_TONE_MAGS];
		for (int i = 0; i < ARDOP_FRAMETYPE_TONE_MAGS; i++) {
			int v = (int)(xorshift32(&rng) % 100000u);
			omags[i] = v;
			pmags[i] = v;
		}
		uint8_t type = (uint8_t)xorshift32(&rng);
		uint8_t id = (uint8_t)xorshift32(&rng);
		int tone_ptr = (xorshift32(&rng) & 1u) ? 20 : 0;

		float od = ComputeDecodeDistance(tone_ptr, omags, type, id);
		float pd = ardop_frametype_decode_distance(pmags, tone_ptr,
							   type, id);
		if (!feq(od, pd))
			fail_msg("distance t=%d type=%x id=%x ptr=%d: "
				 "legacy %.9g, port %.9g", t, type, id,
				 tone_ptr, (double)od, (double)pd);
	}
}

/*
 * Run the minimal-distance decision in both the original and the port from the
 * same tone magnitudes and the same connection state, and require the same
 * frame type and the same "refresh the tuning timestamp" decision. The oracle
 * reports the timestamp write by whether it overwrote our sentinel with Now.
 */
static void expect_mindist_same(const int32_t *mags, bool is_iss,
				uint8_t session_id, bool pending,
				bool arq_connected, uint8_t last_arq_sid, int tag)
{
	int omags[ARDOP_FRAMETYPE_TONE_MAGS];
	for (int i = 0; i < ARDOP_FRAMETYPE_TONE_MAGS; i++)
		omags[i] = mags[i];

	ProtocolState = is_iss ? ISS_STATE : NON_ISS_STATE;
	blnPending = pending;
	blnARQConnected = arq_connected;
	bytLastARQSessionID = last_arq_sid;
	dblOffsetHz = 12.0f;
	AccumulateStats = 0;
	DecodeWav[0][0] = 'x';
	WavNow = 555555;
	dttLastGoodFrameTypeDecode = -999999;

	int oret = MinimalDistanceFrameType(omags, session_id);
	bool oset = (dttLastGoodFrameTypeDecode == 555555);

	ardop_frametype_decode_ctx ctx = {
		.valid_types = is_iss ? bytValidFrameTypesISS
				      : bytValidFrameTypesALL,
		.valid_len = is_iss ? bytValidFrameTypesLengthISS
				    : bytValidFrameTypesLengthALL,
		.session_id = session_id,
		.pending = pending,
		.arq_connected = arq_connected,
		.last_arq_session_id = last_arq_sid,
	};
	bool mset = false;
	int mret = ardop_frametype_minimal_distance(mags, &ctx, &mset);

	if (oret != mret)
		fail_msg("[%d] type: legacy %d, port %d "
			 "(iss=%d sid=%02x pend=%d conn=%d last=%02x)", tag,
			 oret, mret, is_iss, session_id, pending, arq_connected,
			 last_arq_sid);
	if (oset != mset)
		fail_msg("[%d] set_last_good: legacy %d, port %d", tag, oset,
			 mset);
}

/* Force one frame-type byte's tones to cleanly encode value b at tone_ptr. */
static void set_byte_tones(int32_t *mags, int tone_ptr, uint8_t b)
{
	uint8_t mask = 0xC0;
	for (int j = 0; j <= 4; j++) {
		int idx;
		if (j < 4)
			idx = (b & mask) >> (6 - 2 * j);
		else
			idx = ardop_frame_type_parity(b);
		for (int k = 0; k < 4; k++)
			mags[tone_ptr + 4 * j + k] = (k == idx) ? 10000 : 1;
		mask = (uint8_t)(mask >> 2);
	}
}

/*
 * Synthesised clean decodes across random types and connection states. Setting
 * byte 1's tones to T1 and byte 2's to T2^session makes the two bytes decode
 * to T1 and T2, so iat1==iat2 (and the accept branches) fire whenever T1==T2 --
 * exercising acceptance, not just rejection, deterministically. Both
 * implementations must agree on every combination.
 */
static void test_mindist_matches_legacy_synth(void **state)
{
	(void)state;

	uint32_t rng = 0x1234ABCDu;
	int lenALL = bytValidFrameTypesLengthALL;

	for (int t = 0; t < 5000; t++) {
		uint8_t t1 = bytValidFrameTypesALL[xorshift32(&rng)
						   % (uint32_t)lenALL];
		/* Half the time force T2==T1 so iat1==iat2 can accept. */
		uint8_t t2 = (xorshift32(&rng) & 1u)
			     ? t1
			     : bytValidFrameTypesALL[xorshift32(&rng)
						     % (uint32_t)lenALL];
		uint8_t session = (uint8_t)xorshift32(&rng);
		if (xorshift32(&rng) % 3u == 0)
			session = 0xFF;
		uint8_t last = (uint8_t)xorshift32(&rng);
		bool pending = xorshift32(&rng) & 1u;
		bool conn = xorshift32(&rng) & 1u;
		bool is_iss = xorshift32(&rng) & 1u;

		int32_t mags[ARDOP_FRAMETYPE_TONE_MAGS];
		set_byte_tones(mags, 0, t1);
		set_byte_tones(mags, 20, (uint8_t)(t2 ^ session));

		expect_mindist_same(mags, is_iss, session, pending, conn, last,
				     t);
	}
}

/* Noisy tone sets: mostly rejects, exercising the argmin and the reject paths. */
static void test_mindist_matches_legacy_noise(void **state)
{
	(void)state;

	uint32_t rng = 0x77AA33FFu;
	for (int t = 0; t < 3000; t++) {
		int32_t mags[ARDOP_FRAMETYPE_TONE_MAGS];
		for (int i = 0; i < ARDOP_FRAMETYPE_TONE_MAGS; i++)
			mags[i] = (int32_t)(xorshift32(&rng) % 100000u);

		uint8_t session = (uint8_t)xorshift32(&rng);
		if (xorshift32(&rng) % 3u == 0)
			session = 0xFF;
		uint8_t last = (uint8_t)xorshift32(&rng);
		bool pending = xorshift32(&rng) & 1u;
		bool conn = xorshift32(&rng) & 1u;
		bool is_iss = xorshift32(&rng) & 1u;

		expect_mindist_same(mags, is_iss, session, pending, conn, last,
				     t);
	}
}

/*
 * 4FSK character demod on real baseband: the frame-type field is itself 4FSK
 * 50-baud, so demodulating it with the char demodulator must match
 * Demod1Car4FSKChar bit-for-bit -- the byte and all 16 tone mags -- at every
 * offset. And the first character over the field is the frame's own first
 * frame-type byte (BREAK, 0x23), a real join into the symbol demodulator.
 */
static void test_4fsk_char_matches_legacy(void **state)
{
	(void)state;

	static int16_t frame[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod m;
	ardop_mod_init(&m, 30);
	const uint8_t enc[2] = { 0x23, 0x23 };
	assert_true(ardop_mod_begin(&m, 0x23, enc, sizeof(enc), 120, frame,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t nn = ardop_mod_pull(&m, frame, ARDOP_MOD_MAX_SAMPLES);
	int total = (int)nn < 4900 ? (int)nn : 4900;

	ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	for (int off = 0; off < total; off += 1200) {
		int len = total - off < 1200 ? total - off : 1200;
		ardop_demod_mix_filter(&d, &frame[off], len, 0.0f);
	}
	d.mfs_read_ptr = 30;
	assert_true(ardop_demod_symbol_framing(&d));
	assert_true(ardop_demod_frame_sync(&d));  /* at the frame-type field */

	intFilteredMixedSamplesLength = d.filtered_mixed_len;
	for (int i = 0; i < d.filtered_mixed_len; i++)
		intFilteredMixedSamples[i] = d.filtered_mixed[i];

	int checked = 0;
	for (int c = 0; c < 4; c++) {
		int start = d.mfs_read_ptr + c * 4 * 240;
		if (d.filtered_mixed_len - start < 4 * 240)
			break;

		intCenterFreq = 1500;
		intBaud = 50;
		intSampPerSym = 240;
		intToneMagsIndex = 0;
		charIndex = 0;
		unsigned char odec[8] = {0};
		Demod1Car4FSKChar(start, odec);
		uint8_t obyte = odec[0];

		int32_t pmags[ARDOP_4FSK_CHAR_TONE_MAGS];
		uint8_t pbyte = ardop_demod_4fsk_char(&d, start, 1500, 50, 240,
						      pmags);

		if (pbyte != obyte)
			fail_msg("char %d byte: legacy %02x, port %02x", c,
				 obyte, pbyte);
		for (int i = 0; i < ARDOP_4FSK_CHAR_TONE_MAGS; i++)
			if (pmags[i] != intToneMags[i])
				fail_msg("char %d mag %d: legacy %d, port %d", c,
					 i, intToneMags[i], pmags[i]);

		if (c == 0)
			assert_int_equal(pbyte, 0x23);  /* BREAK's first byte */
		checked++;
	}
	assert_true(checked >= 1);
}

/* Build a valid carrier block: [len][data+pad][CRC][RS parity]. */
static void build_rs_block(uint8_t *raw, int data_len, int rs_len,
			   uint8_t frame_type, uint8_t net_len, uint32_t *rng)
{
	raw[0] = net_len;
	for (int i = 0; i < net_len; i++)
		raw[1 + i] = (uint8_t)xorshift32(rng);
	for (int i = net_len; i < data_len; i++)
		raw[1 + i] = 0;  /* padding */

	uint8_t trailer[2];
	ardop_crc16_trailer(raw, (size_t)(data_len + 1), frame_type, trailer);
	raw[data_len + 1] = trailer[0];
	raw[data_len + 2] = trailer[1];

	assert_int_equal(ardop_rs_append(&g_rs, raw, data_len + 3, rs_len), 0);
}

/*
 * Run one carrier block through CorrectRawDataWithRS and the port from an
 * identical copy, and require the same return, the same corrected bytes, and
 * (for a fresh decode) the same success verdict.
 */
static void expect_rs_same(const uint8_t *block, int data_len, int rs_len,
			   uint8_t frame_type, bool already_ok, int tag)
{
	int combined = data_len + rs_len + 3;
	uint8_t oraw[256], praw[256], ocorr[256], pcorr[256];
	memcpy(oraw, block, (size_t)combined);
	memcpy(praw, block, (size_t)combined);

	CarrierOk[3] = already_ok ? 1 : 0;
	int oret = CorrectRawDataWithRS(oraw, ocorr, data_len, rs_len,
					frame_type, 3);
	bool ook = CarrierOk[3];

	bool pok = false;
	int pret = ardop_decode_carrier_rs(&g_rs, praw, pcorr, data_len, rs_len,
					   frame_type, already_ok, &pok);

	if (oret != pret)
		fail_msg("[%d] return: legacy %d, port %d", tag, oret, pret);
	for (int i = 0; i < pret; i++)
		if (pcorr[i] != ocorr[i])
			fail_msg("[%d] corrected byte %d: legacy %d, port %d",
				 tag, i, ocorr[i], pcorr[i]);
	if (!already_ok && ook != pok)
		fail_msg("[%d] decoded_ok: legacy %d, port %d", tag, ook, pok);
}

/*
 * RS correct + CRC check across error counts: none, within the RS budget
 * (rs_len/2), and beyond it. The port composes the already-proven core rs and
 * crc; this pins the composition to CorrectRawDataWithRS.
 */
static void test_carrier_rs_matches_legacy(void **state)
{
	(void)state;

	assert_true(ardop_rs_init(&g_rs, kRSLens, NUM_RSLENS));
	assert_int_equal(init_rs((int *)kRSLens, NUM_RSLENS), 0);

	uint32_t rng = 0xDECAF001u;
	int rslens[] = {4, 8, 16, 32};
	int datalens[] = {16, 32, 64, 100};

	for (int ri = 0; ri < 4; ri++) {
		for (int di = 0; di < 4; di++) {
			int rs_len = rslens[ri];
			int data_len = datalens[di];
			uint8_t ft = (uint8_t)(0x48 + (ri & 1));
			int combined = data_len + rs_len + 3;

			for (int trial = 0; trial < 40; trial++) {
				uint8_t net = (uint8_t)(xorshift32(&rng)
							% (uint32_t)(data_len + 1));
				uint8_t block[256];
				build_rs_block(block, data_len, rs_len, ft, net,
					       &rng);

				/* Corrupt a chosen number of distinct bytes. */
				int nerr = (int)(xorshift32(&rng)
						 % (uint32_t)(rs_len));
				for (int e = 0; e < nerr; e++) {
					int pos = (int)(xorshift32(&rng)
							% (uint32_t)combined);
					block[pos] = (uint8_t)(block[pos]
							       ^ (xorshift32(&rng)
								  | 1u));
				}

				expect_rs_same(block, data_len, rs_len, ft,
					       false, ri * 100 + di * 10 + trial);
			}

			/* The already-decoded short-circuit, on a clean block. */
			uint8_t clean[256];
			build_rs_block(clean, data_len, rs_len, ft, 5, &rng);
			expect_rs_same(clean, data_len, rs_len, ft, true, 999);
		}
	}
}

/*
 * PSK init + per-carrier demod on real baseband. For each carrier count and PSK
 * order, InitDemodPSK and the port must set up identical carrier bins and
 * reference phases, and Demod1CarPSKChar and the port must produce identical
 * differential phases and magnitudes (all integer milliradians -- the
 * byte-exactness-sensitive part) at every carrier and offset.
 */
static void expect_psk_same(ardop_demod *d, int num_car, int psk_mode,
			    int start)
{
	strMod[0] = (psk_mode == 8) ? '8' : '4';
	strMod[1] = 0;
	intNumCar = num_car;
	InitDemodPSK();
	for (int c = 0; c < 8; c++)
		CarrierOk[c] = 0;
	for (int c = 0; c < num_car; c++)
		Demod1CarPSKChar(start, c);

	ardop_demod_psk_init(d, num_car, psk_mode);
	for (int c = 0; c < num_car; c++)
		ardop_demod_psk_char(d, start, c, false);

	if (d->phases_len != intPhasesLen)
		fail_msg("psk %dc/%d phases_len: legacy %d, port %d", num_car,
			 psk_mode, intPhasesLen, d->phases_len);

	for (int c = 0; c < num_car; c++) {
		/* Init state (unchanged by the demod). */
		if (d->freq_bin[c] != dblFreqBin[c])
			fail_msg("psk %dc/%d car %d freq_bin: legacy %g, port %g",
				 num_car, psk_mode, c, (double)dblFreqBin[c],
				 (double)d->freq_bin[c]);
		if (d->n_for_goertzel[c] != intNforGoertzel[c]
		    || d->cp[c] != intCP[c]
		    || d->car_mag_threshold[c] != intCarMagThreshold[c])
			fail_msg("psk %dc/%d car %d init state mismatch",
				 num_car, psk_mode, c);
		if (d->psk_phase_1[c] != intPSKPhase_1[c])
			fail_msg("psk %dc/%d car %d final phase: legacy %d, port %d",
				 num_car, psk_mode, c, intPSKPhase_1[c],
				 d->psk_phase_1[c]);

		/* Demod output: carrier c wrote at indices [c*psk_mode, ...). */
		for (int j = c * psk_mode; j < (c + 1) * psk_mode; j++) {
			if (d->phases[c][j] != intPhases[c][j])
				fail_msg("psk %dc/%d car %d phase[%d]: "
					 "legacy %d, port %d", num_car, psk_mode,
					 c, j, intPhases[c][j], d->phases[c][j]);
			if (d->mags[c][j] != intMags[c][j])
				fail_msg("psk %dc/%d car %d mag[%d]: "
					 "legacy %d, port %d", num_car, psk_mode,
					 c, j, intMags[c][j], d->mags[c][j]);
		}
	}
}

static void test_psk_matches_legacy(void **state)
{
	(void)state;

	/* A real frame's baseband gives the carriers something to lock onto. */
	static int16_t frame[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod m;
	ardop_mod_init(&m, 30);
	const uint8_t enc[2] = { 0x23, 0x23 };
	assert_true(ardop_mod_begin(&m, 0x23, enc, sizeof(enc), 240, frame,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t nn = ardop_mod_pull(&m, frame, ARDOP_MOD_MAX_SAMPLES);
	int total = (int)nn < 4900 ? (int)nn : 4900;

	static ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	for (int off = 0; off < total; off += 1200) {
		int len = total - off < 1200 ? total - off : 1200;
		ardop_demod_mix_filter(&d, &frame[off], len, 0.0f);
	}
	intFilteredMixedSamplesLength = d.filtered_mixed_len;
	for (int i = 0; i < d.filtered_mixed_len; i++)
		intFilteredMixedSamples[i] = d.filtered_mixed[i];

	int cars[] = {1, 2, 4, 8};
	int modes[] = {4, 8};
	for (int ci = 0; ci < 4; ci++)
		for (int mi = 0; mi < 2; mi++)
			for (int start = 200; start + 8 * 120 <= total;
			     start += 700)
				expect_psk_same(&d, cars[ci], modes[mi], start);
}

/*
 * PSK phase->bits decode over random phases (all decision regions), for 4PSK
 * and 8PSK: the port must slice phases into bytes exactly like Decode1CarPSK.
 */
static void test_psk_decode_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x5A5AF00Du;
	static ardop_demod d;
	ardop_demod_init(&d, 100, 5);

	for (int trial = 0; trial < 500; trial++) {
		int psk_mode = (xorshift32(&rng) & 1u) ? 8 : 4;
		int units = 1 + (int)(xorshift32(&rng) % 8u);
		int len = units * psk_mode;
		int carrier = (int)(xorshift32(&rng) % 8u);

		for (int j = 0; j < len; j++) {
			short p = (short)((int)(xorshift32(&rng) % 6601u)
					  - 3300);
			d.phases[carrier][j] = p;
			intPhases[carrier][j] = p;
		}
		d.psk_mode = psk_mode;
		d.phases_len = len;
		intPSKMode = psk_mode;
		intPhasesLen = len;
		CarrierOk[carrier] = 0;

		unsigned char odec[256] = {0};
		uint8_t pdec[256] = {0};
		Decode1CarPSK(odec, carrier);
		int pn = ardop_decode_psk_char(&d, carrier, pdec, false);

		int expect = (psk_mode == 4) ? len / 4 : (len / 8) * 3;
		if (pn != expect)
			fail_msg("psk%d decode count: got %d, expected %d",
				 psk_mode, pn, expect);
		for (int i = 0; i < pn; i++)
			if (pdec[i] != odec[i])
				fail_msg("psk%d byte %d: legacy %02x, port %02x",
					 psk_mode, i, odec[i], pdec[i]);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_matches_legacy_on_noise),
		cmocka_unit_test(test_short_window_declines),
		cmocka_unit_test(test_detects_modulator_leader),
		cmocka_unit_test(test_mix_matches_legacy_on_noise),
		cmocka_unit_test(test_mix_matches_legacy_on_leader),
		cmocka_unit_test(test_sync_matches_legacy_on_frame),
		cmocka_unit_test(test_sync_matches_legacy_on_noise),
		cmocka_unit_test(test_frametype_matches_legacy_on_frame),
		cmocka_unit_test(test_decode_distance_matches_legacy),
		cmocka_unit_test(test_mindist_matches_legacy_synth),
		cmocka_unit_test(test_mindist_matches_legacy_noise),
		cmocka_unit_test(test_4fsk_char_matches_legacy),
		cmocka_unit_test(test_carrier_rs_matches_legacy),
		cmocka_unit_test(test_psk_matches_legacy),
		cmocka_unit_test(test_psk_decode_matches_legacy),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
