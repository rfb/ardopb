#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/crc.h"

/*
 * The old implementations, used here as oracles. Declared locally rather than
 * via src/common/ARDOPC.h for the same reason as in test_frame.c: so the test
 * depends on the functions it is replacing and nothing else, and so both go
 * away together when the old code does.
 *
 * GenCRC16 and the frame-trailer pair live in ARDOPC.c; GenCRC8 lives in ARQ.c.
 */
unsigned int GenCRC16(unsigned char *Data, unsigned short length);
unsigned char GenCRC8(char *Data);
void GenCRC16FrameType(char *Data, int Length, unsigned char bytFrameType);
int CheckCRC16FrameType(unsigned char *Data, int Length,
			unsigned char bytFrameType);

/* A small reproducible PRNG so the corpus is identical on every run. */
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
 * CRC-16 over thousands of random buffers of every length from 0 to 512 must
 * match the old function exactly. This is the whole point of the module: the
 * value that goes on the air is unchanged.
 */
static void test_crc16_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xC0FFEEu;
	int cases = 0;

	for (int len = 0; len <= 512; len++) {
		uint8_t buf[512];
		for (int i = 0; i < len; i++)
			buf[i] = (uint8_t)xorshift32(&rng);

		uint16_t got = ardop_crc16(buf, (size_t)len);
		unsigned int want = GenCRC16(buf, (unsigned short)len);

		if (got != want) {
			fail_msg("CRC16 len %d: got 0x%04X, legacy 0x%04X",
				 len, got, want);
		}
		cases++;
	}

	/* Guard against the loop silently not running. */
	assert_int_equal(cases, 513);
}

/*
 * CRC-8 over random NUL-free strings must match GenCRC8. The old function reads
 * its length with strlen, so the oracle is fed a terminated string while the
 * new function is told the length explicitly; both must agree.
 */
static void test_crc8_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x1234567u;
	int cases = 0;

	for (int len = 0; len <= 64; len++) {
		char str[65];
		for (int i = 0; i < len; i++) {
			/* Any byte except NUL, so strlen sees the full length. */
			uint8_t b = (uint8_t)(xorshift32(&rng) % 255u) + 1u;
			str[i] = (char)b;
		}
		str[len] = '\0';

		uint8_t got = ardop_crc8((const uint8_t *)str, strlen(str));
		unsigned char want = GenCRC8(str);

		if (got != want) {
			fail_msg("CRC8 len %d: got 0x%02X, legacy 0x%02X",
				 len, got, want);
		}
		cases++;
	}

	assert_int_equal(cases, 65);
}

/*
 * The frame trailer the new module writes must be byte-identical to the two
 * bytes the old GenCRC16FrameType appends, for every frame type.
 */
static void test_crc16_trailer_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xABCDEFu;

	for (int type = 0; type < 256; type++) {
		int len = 1 + (int)(xorshift32(&rng) % 200u);
		uint8_t payload[256];
		for (int i = 0; i < len; i++)
			payload[i] = (uint8_t)xorshift32(&rng);

		/* Old function appends in place, so give it room for 2 bytes. */
		uint8_t legacy[256 + 2];
		memcpy(legacy, payload, (size_t)len);
		GenCRC16FrameType((char *)legacy, len, (unsigned char)type);

		uint8_t trailer[2];
		ardop_crc16_trailer(payload, (size_t)len, (uint8_t)type, trailer);

		if (trailer[0] != legacy[len] || trailer[1] != legacy[len + 1]) {
			fail_msg("trailer type 0x%02X len %d: got %02X %02X, "
				 "legacy %02X %02X", type, len, trailer[0],
				 trailer[1], legacy[len], legacy[len + 1]);
		}
	}
}

/*
 * ardop_crc16_trailer_ok must accept exactly what CheckCRC16FrameType accepts.
 * Checked both ways: a trailer the old code produced must verify, and a
 * corrupted trailer must be rejected by both.
 */
static void test_crc16_trailer_ok_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x55AA55u;

	for (int iter = 0; iter < 2000; iter++) {
		int len = 1 + (int)(xorshift32(&rng) % 200u);
		uint8_t type = (uint8_t)xorshift32(&rng);

		uint8_t buf[256 + 2];
		for (int i = 0; i < len; i++)
			buf[i] = (uint8_t)xorshift32(&rng);

		/* Lay down a genuine trailer, then maybe corrupt something. */
		GenCRC16FrameType((char *)buf, len, type);

		/* Half the time, flip a random bit somewhere in payload+trailer. */
		bool corrupt = (xorshift32(&rng) & 1u) != 0u;
		if (corrupt) {
			int pos = (int)(xorshift32(&rng) % (uint32_t)(len + 2));
			buf[pos] ^= (uint8_t)(1u << (xorshift32(&rng) & 7u));
		}

		bool got = ardop_crc16_trailer_ok(buf, (size_t)len, type,
						  &buf[len]);
		bool want = CheckCRC16FrameType(buf, len, type) != 0;

		if (got != want) {
			fail_msg("trailer_ok iter %d (len %d, type 0x%02X, "
				 "corrupt %d): got %d, legacy %d", iter, len,
				 type, corrupt, got, want);
		}
	}
}

/*
 * Properties that hold regardless of the oracle, so they would catch a table
 * transcribed from a wrong reference: the seed values are observable on empty
 * input, and a freshly made trailer round-trips while any single flipped bit
 * breaks it.
 */
static void test_crc_properties(void **state)
{
	(void)state;

	/* Empty input returns the seed for each width. */
	assert_int_equal(ardop_crc16(NULL, 0), 0xFFFFu);
	assert_int_equal(ardop_crc8(NULL, 0), 0xFFu);

	const uint8_t payload[] = {'A', 'R', 'D', 'O', 'P', 0x00, 0xFF, 0x42};
	const uint8_t type = 0x3C;

	uint8_t trailer[2];
	ardop_crc16_trailer(payload, sizeof(payload), type, trailer);
	assert_true(ardop_crc16_trailer_ok(payload, sizeof(payload), type,
					   trailer));

	/* Wrong frame type must fail even with the right payload CRC. */
	assert_false(ardop_crc16_trailer_ok(payload, sizeof(payload),
					    (uint8_t)(type ^ 1u), trailer));

	/* Any single-bit flip in the payload must break the check. */
	for (size_t i = 0; i < sizeof(payload); i++) {
		for (int bit = 0; bit < 8; bit++) {
			uint8_t corrupted[sizeof(payload)];
			memcpy(corrupted, payload, sizeof(payload));
			corrupted[i] ^= (uint8_t)(1u << bit);
			assert_false(ardop_crc16_trailer_ok(
				corrupted, sizeof(payload), type, trailer));
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_crc16_matches_legacy),
		cmocka_unit_test(test_crc8_matches_legacy),
		cmocka_unit_test(test_crc16_trailer_matches_legacy),
		cmocka_unit_test(test_crc16_trailer_ok_matches_legacy),
		cmocka_unit_test(test_crc_properties),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
