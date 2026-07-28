#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include "setup.h"

#include "link/bandwidth.h"

/*
 * The inherited IRSNegotiateBW is the oracle. It reads the station's setting
 * from the global ARQBandwidth, returns the ConAck/ConRejBW frame type, and
 * writes the agreed width to the global intSessionBW on acceptance. Declared
 * here rather than via ARQ.c's headers.
 */
int IRSNegotiateBW(int intConReqFrameType);

enum _ARQBandwidth {
	B200FORCED, B500FORCED, B1000FORCED, B2000FORCED,
	B200MAX, B500MAX, B1000MAX, B2000MAX, UNDEFINED
};
extern enum _ARQBandwidth ARQBandwidth;
extern int intSessionBW;

/*
 * For every bandwidth setting and every possible frame-type byte, the port must
 * return the same ConAck/ConRejBW type as IRSNegotiateBW, and agree on the
 * session width whenever the connection is accepted. Sweeping all 256 type
 * bytes covers the ConReq range and confirms everything else is rejected.
 */
static void test_negotiate_matches_legacy(void **state)
{
	(void)state;

	for (int s = 0; s <= (int)ARDOP_ARQ_BW_UNDEFINED; s++) {
		ARQBandwidth = (enum _ARQBandwidth)s;

		for (int t = 0; t < 256; t++) {
			intSessionBW = -1;
			int legacy = IRSNegotiateBW(t);
			int legacy_bw = intSessionBW;

			int port_bw = -1;
			uint8_t port = ardop_negotiate_bandwidth(
				(ardop_arq_bandwidth)s, (uint8_t)t, &port_bw);

			if (port != legacy)
				fail_msg("setting %d type %02x: port %02x,"
					 " legacy %02x", s, t, port, legacy);
			/* Width is written only on acceptance; on rejection both
			 * leave their sentinel untouched. */
			assert_int_equal(port_bw, legacy_bw);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_negotiate_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
