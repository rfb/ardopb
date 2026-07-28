#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/frames.h"

/* Inherited control-frame encoders, the oracles. Declared here rather than
 * via ARDOPC.h. */
int Encode4FSKControl(unsigned char bytFrameType, unsigned char bytSessionID,
		      unsigned char *bytreturn);
int EncodeConACKwTiming(unsigned char bytFrameType, int intRcvdLeaderLenMs,
			unsigned char bytSessionID, unsigned char *bytreturn);
int EncodePingAck(int bytFrameType, int intSN, int intQuality,
		  unsigned char *bytreturn);

static void assert_same(const char *what, const uint8_t *port, int port_len,
			const unsigned char *legacy, int legacy_len)
{
	if (port_len != legacy_len)
		fail_msg("%s: length port %d, legacy %d", what, port_len,
			 legacy_len);
	for (int i = 0; i < port_len; i++)
		if (port[i] != legacy[i])
			fail_msg("%s: byte %d port %02x, legacy %02x", what, i,
				 port[i], legacy[i]);
}

/* The 2-byte control frame, across every type and session-id byte. */
static void test_control_matches_legacy(void **state)
{
	(void)state;

	for (int t = 0; t < 256; t++) {
		for (int s = 0; s < 256; s++) {
			uint8_t port[ARDOP_CONTROL_FRAME_MAX];
			unsigned char legacy[8];
			size_t pn = ardop_encode_control((uint8_t)t, (uint8_t)s,
							 port);
			int ln = Encode4FSKControl((unsigned char)t,
						   (unsigned char)s, legacy);
			assert_same("control", port, (int)pn, legacy, ln);
		}
	}
}

/* ConACK with timing: sweep the leader length through the cap and the
 * out-of-range clamp, over a few types and session ids. */
static void test_conack_timing_matches_legacy(void **state)
{
	(void)state;

	const uint8_t types[] = {0x39, 0x3A, 0x3B, 0x3C};
	const uint8_t sids[] = {0x00, 0x5A, 0xC3, 0xFF};

	for (size_t ti = 0; ti < sizeof types; ti++) {
		for (size_t si = 0; si < sizeof sids; si++) {
			for (int ms = -50; ms <= 2650; ms++) {
				uint8_t port[ARDOP_CONTROL_FRAME_MAX];
				unsigned char legacy[8];
				size_t pn = ardop_encode_conack_timing(
					types[ti], ms, sids[si], port);
				int ln = EncodeConACKwTiming(types[ti], ms,
							     sids[si], legacy);
				assert_same("conack", port, (int)pn, legacy, ln);
			}
		}
	}
}

/* PingAck: sweep S/N through the >=21 dB saturation and quality through the
 * 30..100 mapping, including out-of-range values. */
static void test_pingack_matches_legacy(void **state)
{
	(void)state;

	for (int sn = -20; sn <= 30; sn++) {
		for (int q = 0; q <= 110; q++) {
			uint8_t port[ARDOP_CONTROL_FRAME_MAX];
			unsigned char legacy[8];
			size_t pn = ardop_encode_pingack(0x3D, sn, q, port);
			int ln = EncodePingAck(0x3D, sn, q, legacy);
			assert_same("pingack", port, (int)pn, legacy, ln);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_control_matches_legacy),
		cmocka_unit_test(test_conack_timing_matches_legacy),
		cmocka_unit_test(test_pingack_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
