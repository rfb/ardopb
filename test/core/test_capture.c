#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/capture.h"

/*
 * The capture wire format. Written once and read back potentially much later
 * on a different machine, so round-trip fidelity through the bytes -- not
 * struct equality -- is what these tests check, the same discipline as
 * test_telemetry.c.
 */

static void test_capture_frame_roundtrip(void **state)
{
	(void)state;

	uint8_t payload[5] = {1, 2, 3, 4, 5};
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_frame(buf, sizeof(buf),
					      ARDOP_CAPTURE_FRAME_RX, 0x50,
					      93, 17, 500, payload,
					      sizeof(payload));
	assert_true(n > 0);

	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_FRAME_RX);
	assert_int_equal(out.frame_type, 0x50);
	assert_int_equal(out.quality, 93);
	assert_int_equal(out.sn, 17);
	assert_int_equal(out.bandwidth_hz, 500);
	assert_int_equal(out.payload_len, sizeof(payload));
	assert_memory_equal(out.payload, payload, sizeof(payload));

	/* A short buffer is "not yet", not a misparse. */
	assert_false(ardop_capture_parse_record(buf, n - 1, &out));
}

/* A failed decode carries no payload, per capture.h -- the demodulator's own
 * payload buffer is not trustworthy once CRC/RS has failed. */
static void test_capture_frame_rx_failed_has_no_payload(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_frame(buf, sizeof(buf),
					      ARDOP_CAPTURE_FRAME_RX_FAILED,
					      0x50, -1, 14, 500, NULL, 0);
	assert_true(n > 0);

	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_FRAME_RX_FAILED);
	assert_int_equal(out.quality, -1);
	assert_int_equal(out.sn, 14);
	assert_int_equal(out.payload_len, 0);
	assert_null(out.payload);
}

/* The kind this format exists for: a leader that never resolves to a frame at
 * all -- previously invisible, per analysis/16 sec 12/13. */
static void test_capture_leader_roundtrip(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_leader(buf, sizeof(buf), 12.5f, 18);
	assert_true(n > 0);

	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_LEADER);
	assert_true(out.offset_hz > 12.4f && out.offset_hz < 12.6f);
	assert_int_equal(out.sn, 18);
}

static void test_capture_state_roundtrip(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_state(buf, sizeof(buf), 3, "VA7DEP");
	assert_true(n > 0);

	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_STATE);
	assert_int_equal(out.link_state, 3);
	assert_int_equal(out.remote_len, 6);
	assert_memory_equal(out.remote, "VA7DEP", 6);

	/* Empty remote (disconnected) round-trips to a zero-length, non-NULL
	 * string, not a NULL that a printf %s would crash on. */
	n = ardop_capture_encode_state(buf, sizeof(buf), 0, "");
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.remote_len, 0);
	assert_string_equal(out.remote, "");
}

static void test_capture_ptt_and_busy_roundtrip(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];

	size_t n = ardop_capture_encode_ptt(buf, sizeof(buf), true);
	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_PTT);
	assert_true(out.flag);

	n = ardop_capture_encode_ptt(buf, sizeof(buf), false);
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_false(out.flag);

	n = ardop_capture_encode_busy(buf, sizeof(buf), true);
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_BUSY);
	assert_true(out.flag);
}

static void test_capture_bandwidth_roundtrip(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_bandwidth(buf, sizeof(buf), 2000);
	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_BANDWIDTH);
	assert_int_equal(out.bandwidth_hz, 2000);
}

static void test_capture_rx_data_roundtrip(void **state)
{
	(void)state;

	uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_rx_data(buf, sizeof(buf), "ARQ", data,
						sizeof(data));
	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_RX_DATA);
	assert_int_equal(out.tag_len, 3);
	assert_memory_equal(out.tag, "ARQ", 3);
	assert_int_equal(out.payload_len, sizeof(data));
	assert_memory_equal(out.payload, data, sizeof(data));
}

static void test_capture_host_msg_roundtrip(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_host_msg(buf, sizeof(buf),
						 "CONNECTED VA7DEP 500");
	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_int_equal(out.kind, ARDOP_CAPTURE_HOST_MSG);
	assert_int_equal(out.text_len, strlen("CONNECTED VA7DEP 500"));
	assert_memory_equal(out.text, "CONNECTED VA7DEP 500", out.text_len);
}

/* Wrong magic or wrong version is rejected outright, never half-read --
 * mirrors test_telemetry.c's test_tlm_hello. */
static void test_capture_rejects_bad_magic_or_version(void **state)
{
	(void)state;

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_ptt(buf, sizeof(buf), true);

	uint8_t bad_magic[ARDOP_CAPTURE_MAX_RECORD];
	memcpy(bad_magic, buf, n);
	bad_magic[0] ^= 0xFF;
	ardop_capture_record out;
	assert_false(ardop_capture_parse_record(bad_magic, n, &out));

	uint8_t bad_version[ARDOP_CAPTURE_MAX_RECORD];
	memcpy(bad_version, buf, n);
	bad_version[1] = 99;
	assert_false(ardop_capture_parse_record(bad_version, n, &out));
}

/* An over-long string or payload is clamped to fit the caller's buffer, not
 * refused -- capture.h's ARDOP_CAPTURE_MAX_PAYLOAD contract. */
static void test_capture_encode_clamps_to_buffer(void **state)
{
	(void)state;

	static uint8_t big[ARDOP_CAPTURE_MAX_PAYLOAD + 1000];
	memset(big, 0xAB, sizeof(big));

	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_frame(buf, sizeof(buf),
					      ARDOP_CAPTURE_FRAME_TX, 0x70, -1,
					      -1, 2000, big, sizeof(big));
	assert_true(n > 0);
	assert_true(n <= sizeof(buf));

	ardop_capture_record out;
	assert_true(ardop_capture_parse_record(buf, n, &out));
	assert_true(out.payload_len < sizeof(big));   /* truncated, not refused */

	/* A buffer too small even for the fixed part is refused (0), not
	 * partially written. */
	uint8_t tiny[2];
	assert_int_equal(ardop_capture_encode_ptt(tiny, sizeof(tiny), true), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_capture_frame_roundtrip),
		cmocka_unit_test(test_capture_frame_rx_failed_has_no_payload),
		cmocka_unit_test(test_capture_leader_roundtrip),
		cmocka_unit_test(test_capture_state_roundtrip),
		cmocka_unit_test(test_capture_ptt_and_busy_roundtrip),
		cmocka_unit_test(test_capture_bandwidth_roundtrip),
		cmocka_unit_test(test_capture_rx_data_roundtrip),
		cmocka_unit_test(test_capture_host_msg_roundtrip),
		cmocka_unit_test(test_capture_rejects_bad_magic_or_version),
		cmocka_unit_test(test_capture_encode_clamps_to_buffer),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
