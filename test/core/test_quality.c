#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include "setup.h"

#include "link/quality.h"

/*
 * The inherited encoders are the oracle. They write a 2-byte control frame
 * [type, type ^ sessionID]; only byte 0 carries the quality, which is what the
 * port produces. Declared here rather than via ARDOPC.h.
 */
int EncodeDATAACK(int intQuality, unsigned char bytSessionID,
		  unsigned char *bytreturn);
int EncodeDATANAK(int intQuality, unsigned char bytSessionID,
		  unsigned char *bytreturn);

/* ComputeQualityAvg mutates the global intAvgQuality; the port is pure. */
void ComputeQualityAvg(int intReportedQuality);
extern int intAvgQuality;

/*
 * The ACK and NAK type bytes must match EncodeDATAACK / EncodeDATANAK for every
 * quality, including out-of-range values: the ACK path clamps to 100, the NAK
 * path deliberately does not, and both edges are checked here.
 */
static void test_quality_encode_matches_legacy(void **state)
{
	(void)state;

	for (int q = -20; q <= 130; q++) {
		unsigned char ack[2], nak[2];
		EncodeDATAACK(q, 0x00, ack);
		EncodeDATANAK(q, 0x00, nak);

		assert_int_equal(ardop_quality_to_ack_type(q), ack[0]);
		assert_int_equal(ardop_quality_to_nak_type(q), nak[0]);
	}
}

/*
 * The decode is the fixed q = 38 + 2 * (type & 0x1F) formula the receiver uses
 * in several places; check it across all 256 type bytes, and that it round-trips
 * with the encoders over the representable 38..100 range.
 */
static void test_quality_decode_and_roundtrip(void **state)
{
	(void)state;

	for (int t = 0; t < 256; t++)
		assert_int_equal(ardop_quality_from_type((uint8_t)t),
				 38 + 2 * (t & 0x1F));

	/* Representable qualities (even, 38..100) survive encode then decode,
	 * through both the ACK and NAK type ranges. */
	for (int q = 38; q <= 100; q += 2) {
		assert_int_equal(ardop_quality_from_type(
					 ardop_quality_to_ack_type(q)), q);
		assert_int_equal(ardop_quality_from_type(
					 ardop_quality_to_nak_type(q)), q);
	}
	/* Sub-38 qualities clamp to 38 (the "38 or worse" floor). */
	assert_int_equal(ardop_quality_from_type(ardop_quality_to_ack_type(0)),
			 38);
	assert_int_equal(ardop_quality_from_type(ardop_quality_to_ack_type(37)),
			 38);
}

/*
 * The running quality average must match ComputeQualityAvg for every
 * (stored average, reported quality) pair over the plausible range -- including
 * the unseeded (0) case that takes the report verbatim and the +0.5 rounding.
 */
static void test_quality_avg_matches_legacy(void **state)
{
	(void)state;

	for (int avg = 0; avg <= 120; avg++) {
		for (int reported = 0; reported <= 120; reported++) {
			intAvgQuality = avg;
			ComputeQualityAvg(reported);
			if (ardop_quality_avg(avg, reported) != intAvgQuality)
				fail_msg("avg %d + %d: port %d, legacy %d",
					 avg, reported,
					 ardop_quality_avg(avg, reported),
					 intAvgQuality);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_quality_encode_matches_legacy),
		cmocka_unit_test(test_quality_decode_and_roundtrip),
		cmocka_unit_test(test_quality_avg_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
