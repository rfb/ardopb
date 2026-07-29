#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/dataframe.h"
#include "codec/frame.h"
#include "codec/rs.h"

/*
 * The inherited encoders are the oracle: EncodeFSKData for 4FSK (it carries the
 * 600-baud 3-part branch), EncodePSKData for PSK/QAM. They read the session id
 * from the global bytSessionID and use the global RS tables (init_rs). Declared
 * here rather than via ARDOPC.h.
 */
int EncodeFSKData(unsigned char bytFrameType, unsigned char *bytDataToSend,
		  int Length, unsigned char *bytEncodedBytes);
int EncodePSKData(unsigned char bytFrameType, unsigned char *bytDataToSend,
		  int Length, unsigned char *bytEncodedBytes);
extern unsigned char bytSessionID;
int init_rs(int *lengths, int count);

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))
static ardop_rs g_rs;

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static void check_one(uint8_t ft, uint8_t session, int length, uint32_t *rng)
{
	uint8_t payload[1024];
	for (int i = 0; i < length; i++)
		payload[i] = (uint8_t)xorshift32(rng);

	const ardop_frame_spec *spec = ardop_frame_spec_for(ft);
	assert_non_null(spec);

	bytSessionID = session;
	unsigned char oracle[ARDOP_DATAFRAME_MAX];
	int olen;
	if (spec->modulation == ARDOP_MOD_4FSK)
		olen = EncodeFSKData(ft, payload, length, oracle);
	else
		olen = EncodePSKData(ft, payload, length, oracle);

	uint8_t port[ARDOP_DATAFRAME_MAX];
	int plen = ardop_encode_data_frame(&g_rs, ft, session, payload, length,
					   port);

	if (plen != olen)
		fail_msg("ft %02x len %d: length port %d, oracle %d", ft,
			 length, plen, olen);
	for (int i = 0; i < plen; i++)
		if (port[i] != oracle[i])
			fail_msg("ft %02x len %d: byte %d port %02x, oracle %02x",
				 ft, length, i, port[i], oracle[i]);
}

/*
 * A representative data frame from each modulation and carrier count, encoded at
 * full, partial and minimal fill (the partial and minimal cases exercise the
 * length byte and the zero-padding of the trailing carrier), must be byte-exact
 * to EncodeFSKData / EncodePSKData -- including the 4FSK.2000.600 3-part frame.
 */
static void test_encode_matches_legacy(void **state)
{
	(void)state;

	assert_true(ardop_rs_init(&g_rs, kRSLens, NUM_RSLENS));
	assert_int_equal(init_rs((int *)kRSLens, NUM_RSLENS), 0);

	/* frame type, whole-frame capacity (carriers * data_bytes_per_carrier). */
	const struct { uint8_t ft; } cases[] = {
		{0x48}, {0x42}, {0x44}, {0x46},   /* 1 car: 4FSK/4PSK/8PSK/16QAM */
		{0x50}, {0x60}, {0x70},           /* 4PSK 2/4/8 car               */
		{0x74},                           /* 16QAM 8 car                  */
		{0x7A},                           /* 4FSK.2000.600 (3-part)       */
	};

	uint32_t rng = 0xDA7AF00Du;
	for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		uint8_t ft = cases[c].ft;
		const ardop_frame_spec *spec = ardop_frame_spec_for(ft);
		int cap = spec->carriers * spec->data_bytes_per_carrier;

		for (uint8_t sid = 0; sid < 3; sid++) {
			uint8_t session = (uint8_t)(sid * 0x5A);
			check_one(ft, session, cap, &rng);        /* full   */
			check_one(ft, session, cap / 2 + 1, &rng);/* partial */
			check_one(ft, session, 1, &rng);          /* minimal */
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_encode_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
