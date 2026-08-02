#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/telemetry.h"

/*
 * The telemetry wire format. The encoder runs in the modem and the decoder in a
 * separate process built by a different compiler, so the property that matters
 * is round-trip fidelity through the bytes -- not that two structs match.
 */

static void test_tlm_hello(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_TLM_HELLO_LEN];
	assert_int_equal(ardop_tlm_encode_hello(buf), ARDOP_TLM_HELLO_LEN);

	uint16_t bins = 0, first = 0;
	float hz = 0.0f;
	assert_true(ardop_tlm_parse_hello(buf, sizeof(buf), &bins, &first, &hz));
	assert_int_equal(bins, ARDOP_BUSY_MAG_BINS);
	assert_int_equal(first, ARDOP_BUSY_FIRST_BIN);
	assert_true(hz > 11.7f && hz < 11.8f);

	/* A short buffer is "not yet", not a misparse. */
	assert_false(ardop_tlm_parse_hello(buf, 4, &bins, &first, &hz));

	/* Wrong magic is rejected rather than half-read. */
	uint8_t bad[ARDOP_TLM_HELLO_LEN];
	memcpy(bad, buf, sizeof(bad));
	bad[0] ^= 0xFF;
	assert_false(ardop_tlm_parse_hello(bad, sizeof(bad), &bins, &first, &hz));
}

static void test_tlm_spectrum_roundtrip(void **state)
{
	(void)state;

	float mag[ARDOP_BUSY_MAG_BINS];
	for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++)
		mag[i] = (float)i * 1.5f;

	uint8_t buf[ARDOP_TLM_MAX_RECORD];
	ardop_telemetry in = {.kind = ARDOP_TLM_SPECTRUM, .mag = mag};
	size_t n = ardop_tlm_encode(&in, buf, sizeof(buf));
	assert_int_equal(n, ARDOP_TLM_HEADER_LEN + ARDOP_BUSY_MAG_BINS * 4);

	static ardop_tlm_decoded out;
	size_t consumed = 0;
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(consumed, n);
	assert_int_equal(out.rec.kind, ARDOP_TLM_SPECTRUM);
	for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++)
		assert_true(out.rec.mag[i] == mag[i]);

	/* One byte short of the whole record is incomplete, not corrupt. */
	assert_false(ardop_tlm_parse(buf, n - 1, &out, &consumed));
}

static void test_tlm_constellation_roundtrip(void **state)
{
	(void)state;

	enum { N = 300 };
	int16_t phase[N], pmag[N];
	for (int i = 0; i < N; i++) {
		phase[i] = (int16_t)(i * 20 - 3000);   /* spans negative. */
		pmag[i] = (int16_t)(i * 3);
	}

	uint8_t buf[ARDOP_TLM_MAX_RECORD];
	ardop_telemetry in = {.kind = ARDOP_TLM_CONSTELLATION,
			      .frame_type = 0x46,
			      .modulation = 3,       /* 16QAM. */
			      .n_points = N,
			      .mag_threshold = 4321, /* the adaptive ring. */
			      .phase_mrad = phase,
			      .point_mag = pmag};
	size_t n = ardop_tlm_encode(&in, buf, sizeof(buf));
	assert_int_equal(n, ARDOP_TLM_HEADER_LEN + 6 + N * 4);

	static ardop_tlm_decoded out;
	size_t consumed = 0;
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.kind, ARDOP_TLM_CONSTELLATION);
	assert_int_equal(out.rec.frame_type, 0x46);
	assert_int_equal(out.rec.modulation, 3);
	assert_int_equal(out.rec.n_points, N);
	assert_int_equal(out.rec.mag_threshold, 4321);
	for (int i = 0; i < N; i++) {
		assert_int_equal(out.rec.phase_mrad[i], phase[i]);
		assert_int_equal(out.rec.point_mag[i], pmag[i]);
	}

	/* A 4FSK snapshot reuses the arrays as (tone, margin) -- the tone index
	 * is small and the margin is per mille, both of which must survive as
	 * written rather than being interpreted as phase. */
	int16_t tone[4] = {0, 1, 2, 3};
	int16_t margin[4] = {1000, 640, 12, 0};
	ardop_telemetry fsk = {.kind = ARDOP_TLM_CONSTELLATION,
			       .frame_type = 0x48,
			       .modulation = 0,   /* 4FSK. */
			       .n_points = 4,
			       .phase_mrad = tone,
			       .point_mag = margin};
	n = ardop_tlm_encode(&fsk, buf, sizeof(buf));
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.modulation, 0);
	assert_int_equal(out.rec.mag_threshold, 0);
	for (int i = 0; i < 4; i++) {
		assert_int_equal(out.rec.phase_mrad[i], tone[i]);
		assert_int_equal(out.rec.point_mag[i], margin[i]);
	}
}

static void test_tlm_audio_and_status_roundtrip(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_TLM_MAX_RECORD];
	static ardop_tlm_decoded out;
	size_t consumed = 0;

	ardop_telemetry audio = {.kind = ARDOP_TLM_AUDIO,
				 .rms = 0.125f, .peak = 0.5f};
	size_t n = ardop_tlm_encode(&audio, buf, sizeof(buf));
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.kind, ARDOP_TLM_AUDIO);
	assert_true(out.rec.rms == 0.125f);
	assert_true(out.rec.peak == 0.5f);

	/* Negative S/N is the interesting case: it must survive as a signed
	 * value, not wrap to a large positive one. */
	ardop_telemetry st = {.kind = ARDOP_TLM_STATUS,
			      .state = 3, .mode = 1, .busy = true, .ptt = false,
			      .sn = -12, .quality = 87, .bandwidth = 500,
			      .buffer_len = 70000};
	n = ardop_tlm_encode(&st, buf, sizeof(buf));
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.kind, ARDOP_TLM_STATUS);
	assert_int_equal(out.rec.state, 3);
	assert_int_equal(out.rec.mode, 1);
	assert_true(out.rec.busy);
	assert_false(out.rec.ptt);
	assert_int_equal(out.rec.sn, -12);
	assert_int_equal(out.rec.quality, 87);
	assert_int_equal(out.rec.bandwidth, 500);
	assert_int_equal(out.rec.buffer_len, 70000);
}

/*
 * The frame record, whose interesting field is the timestamp.
 *
 * `frame_at` is elapsed samples in a uint64, and it is encoded as two 32-bit
 * halves -- so the case worth asserting is a value that does not fit in one.
 * At 12 kHz, 2^32 samples is around four days: reachable by a station left
 * running, and a history whose ordering silently inverted after four days would
 * be a memorable bug.
 */
static void test_tlm_frame_roundtrip(void **state)
{
	(void)state;
	uint8_t buf[ARDOP_TLM_MAX_RECORD];
	ardop_tlm_decoded out;
	size_t consumed;

	const uint64_t deep = ((uint64_t)5 << 32) | 0x1234abcdu;
	ardop_telemetry fr = {.kind = ARDOP_TLM_FRAME,
			      .frame_at = deep,
			      .frame_type = 0xa1,
			      .frame_dir = ARDOP_TLM_DIR_RX,
			      .quality = 92,
			      .sn = -7};
	size_t n = ardop_tlm_encode(&fr, buf, sizeof(buf));
	assert_true(n > 0);
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.kind, ARDOP_TLM_FRAME);
	assert_true(out.rec.frame_at == deep);
	assert_int_equal(out.rec.frame_type, 0xa1);
	assert_int_equal(out.rec.frame_dir, ARDOP_TLM_DIR_RX);
	assert_int_equal(out.rec.quality, 92);
	assert_int_equal(out.rec.sn, -7);

	/* A transmitted frame has no receive figures, and -1 must survive as
	 * -1 rather than wrapping. */
	ardop_telemetry tx = {.kind = ARDOP_TLM_FRAME,
			      .frame_at = 0,
			      .frame_type = 0x31,
			      .frame_dir = ARDOP_TLM_DIR_TX,
			      .quality = -1,
			      .sn = -1};
	n = ardop_tlm_encode(&tx, buf, sizeof(buf));
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.frame_dir, ARDOP_TLM_DIR_TX);
	assert_int_equal(out.rec.quality, -1);
	assert_int_equal(out.rec.sn, -1);
	assert_true(out.rec.frame_at == 0);
}

/* A stream of back-to-back records parses one at a time, and an unknown kind
 * reports its length so a reader can skip it -- the property that lets a newer
 * daemon add records without breaking an older display. */
static void test_tlm_stream_and_forward_compat(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_TLM_MAX_RECORD * 3];
	size_t off = 0;

	ardop_telemetry audio = {.kind = ARDOP_TLM_AUDIO, .rms = 0.25f,
				 .peak = 0.75f};
	off += ardop_tlm_encode(&audio, buf + off, sizeof(buf) - off);

	/* An unknown record: kind 99, 5 bytes of payload. */
	size_t unknown_at = off;
	buf[off++] = 99;
	buf[off++] = 5;
	buf[off++] = 0;
	for (int i = 0; i < 5; i++)
		buf[off++] = (uint8_t)i;

	ardop_telemetry st = {.kind = ARDOP_TLM_STATUS, .sn = -3};
	off += ardop_tlm_encode(&st, buf + off, sizeof(buf) - off);

	static ardop_tlm_decoded out;
	size_t pos = 0, consumed = 0;

	assert_true(ardop_tlm_parse(buf + pos, off - pos, &out, &consumed));
	assert_int_equal(out.rec.kind, ARDOP_TLM_AUDIO);
	pos += consumed;
	assert_int_equal(pos, unknown_at);

	/* Unknown: false, but consumed is set so the reader advances. */
	assert_false(ardop_tlm_parse(buf + pos, off - pos, &out, &consumed));
	assert_int_equal(consumed, 8);
	pos += consumed;

	assert_true(ardop_tlm_parse(buf + pos, off - pos, &out, &consumed));
	assert_int_equal(out.rec.kind, ARDOP_TLM_STATUS);
	assert_int_equal(out.rec.sn, -3);
	pos += consumed;
	assert_int_equal(pos, off);
}

/* Encoding into a buffer that cannot hold the record refuses rather than
 * overruns -- the encoder writes into a fixed socket buffer. */
static void test_tlm_encode_refuses_short_buffer(void **state)
{
	(void)state;

	float mag[ARDOP_BUSY_MAG_BINS] = {0};
	uint8_t small[16];
	ardop_telemetry in = {.kind = ARDOP_TLM_SPECTRUM, .mag = mag};
	assert_int_equal(ardop_tlm_encode(&in, small, sizeof(small)), 0);

	/* An over-long constellation is clamped to the cap, not truncated
	 * into a length that disagrees with its payload. */
	static int16_t big_phase[ARDOP_TLM_MAX_POINTS + 100];
	static int16_t big_mag[ARDOP_TLM_MAX_POINTS + 100];
	uint8_t buf[ARDOP_TLM_MAX_RECORD];
	ardop_telemetry con = {.kind = ARDOP_TLM_CONSTELLATION,
			       .n_points = ARDOP_TLM_MAX_POINTS + 100,
			       .phase_mrad = big_phase,
			       .point_mag = big_mag};
	size_t n = ardop_tlm_encode(&con, buf, sizeof(buf));
	assert_int_equal(n, ARDOP_TLM_HEADER_LEN + 6 + ARDOP_TLM_MAX_POINTS * 4);

	static ardop_tlm_decoded out;
	size_t consumed = 0;
	assert_true(ardop_tlm_parse(buf, n, &out, &consumed));
	assert_int_equal(out.rec.n_points, ARDOP_TLM_MAX_POINTS);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_tlm_hello),
		cmocka_unit_test(test_tlm_spectrum_roundtrip),
		cmocka_unit_test(test_tlm_constellation_roundtrip),
		cmocka_unit_test(test_tlm_audio_and_status_roundtrip),
		cmocka_unit_test(test_tlm_frame_roundtrip),
		cmocka_unit_test(test_tlm_stream_and_forward_compat),
		cmocka_unit_test(test_tlm_encode_refuses_short_buffer),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
