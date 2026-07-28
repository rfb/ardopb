#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/session.h"

/*
 * The inherited GenerateSessionID is the oracle. Declared here rather than via
 * ARQ.c's headers, which drag in the whole protocol world. It takes the two
 * callsign strings, exactly as the port does.
 */
unsigned char GenerateSessionID(const char *strCallingCallSign,
				const char *strTargetCallsign);

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* A random callsign-shaped string: 3..8 chars of [A-Z0-9], optional -SSID. */
static void random_callsign(uint32_t *rng, char *out)
{
	static const char alnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	int len = 3 + (int)(xorshift32(rng) % 6u);
	int i;

	for (i = 0; i < len; i++)
		out[i] = alnum[xorshift32(rng) % 36u];
	if (xorshift32(rng) & 1u) {
		out[i++] = '-';
		out[i++] = (char)('0' + (int)(xorshift32(rng) % 10u));
		if (xorshift32(rng) & 1u)
			out[i++] = (char)('0' + (int)(xorshift32(rng) % 10u));
	}
	out[i] = '\0';
}

/*
 * The session ID must match GenerateSessionID for every pair of callsigns,
 * including the 0xFF -> 0 remap (which the random corpus reaches through the
 * CRC), across a broad sweep of callsign shapes.
 */
static void test_session_id_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0x5E5510A1u;

	/* A couple of fixed pairs first, including a self-addressed one. */
	assert_int_equal(ardop_session_id("N0CALL", "N0CALL-1"),
			 GenerateSessionID("N0CALL", "N0CALL-1"));
	assert_int_equal(ardop_session_id("", ""),
			 GenerateSessionID("", ""));

	int saw_zero = 0;
	for (int t = 0; t < 200000; t++) {
		char a[16], b[16];
		random_callsign(&rng, a);
		random_callsign(&rng, b);

		uint8_t port = ardop_session_id(a, b);
		uint8_t legacy = GenerateSessionID(a, b);
		if (port != legacy)
			fail_msg("session id '%s'+'%s': port %02x, legacy %02x",
				 a, b, port, legacy);
		assert_int_not_equal(port, 0xFF);   /* never returns the FEC marker */
		if (port == 0)
			saw_zero = 1;
	}
	/* The remap path (and ordinary zeros) should be exercised. */
	assert_true(saw_zero);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_session_id_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
