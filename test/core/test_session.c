#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/session.h"
#include "codec/stationid.h"
#include "common/StationId.h"   /* inherited StationId, for the IsCallToMe oracle */

/*
 * The inherited GenerateSessionID is the oracle. Declared here rather than via
 * ARQ.c's headers, which drag in the whole protocol world. It takes the two
 * callsign strings, exactly as the port does.
 */
unsigned char GenerateSessionID(const char *strCallingCallSign,
				const char *strTargetCallsign);

/*
 * For the callsign match, the oracle is IsCallToMe, which reads the local
 * callsign set from globals and uses the inherited StationId type. We populate
 * those globals and build matching core ardop_stationid values from the same
 * strings.
 */
int IsCallToMe(const StationId *caller, const StationId *target,
	       unsigned char *bytReplySessionID);
extern StationId Callsign;
extern StationId AuxCalls[];
extern size_t AuxCallsLength;

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

/*
 * The "is this frame for me?" match must agree with IsCallToMe: same yes/no,
 * and the same reply session ID when it is for us -- including the aux-call
 * case, where the ID is derived from the matched aux callsign, not the primary.
 */
static void test_call_to_me_matches_legacy(void **state)
{
	(void)state;

	const char *mine = "N0CALL";
	const char *aux[] = {"W1ABC-5", "K2XYZ"};
	const size_t n_aux = 2;

	/* Inherited globals for the oracle. */
	assert_int_equal(stationid_from_str(mine, &Callsign), STATIONID_OK);
	assert_int_equal(stationid_from_str(aux[0], &AuxCalls[0]),
			 STATIONID_OK);
	assert_int_equal(stationid_from_str(aux[1], &AuxCalls[1]),
			 STATIONID_OK);
	AuxCallsLength = n_aux;

	/* Core equivalents. */
	ardop_stationid c_mycall, c_aux[2];
	assert_int_equal(ardop_stationid_from_str(mine, &c_mycall),
			 ARDOP_STATIONID_OK);
	assert_int_equal(ardop_stationid_from_str(aux[0], &c_aux[0]),
			 ARDOP_STATIONID_OK);
	assert_int_equal(ardop_stationid_from_str(aux[1], &c_aux[1]),
			 ARDOP_STATIONID_OK);

	/* Targets: the three that match, plus valid callsigns that don't
	 * (including near-misses that differ only by SSID). */
	const char *targets[] = {
		"N0CALL", "W1ABC-5", "K2XYZ",     /* match: primary + both aux */
		"N0CALL-1", "W1ABC", "K2XYZ-9",   /* SSID near-misses          */
		"N0DX", "VK3ABC", "G0ABC-12",     /* unrelated                 */
	};
	const char *callers[] = {"M0ABC", "JA1XYZ-3", "N0CALL"};

	for (size_t ci = 0; ci < sizeof callers / sizeof callers[0]; ci++) {
		for (size_t ti = 0; ti < sizeof targets / sizeof targets[0];
		     ti++) {
			StationId s_caller, s_target;
			ardop_stationid c_caller, c_target;
			assert_int_equal(stationid_from_str(callers[ci],
							    &s_caller),
					 STATIONID_OK);
			assert_int_equal(stationid_from_str(targets[ti],
							    &s_target),
					 STATIONID_OK);
			assert_int_equal(ardop_stationid_from_str(callers[ci],
								  &c_caller),
					 ARDOP_STATIONID_OK);
			assert_int_equal(ardop_stationid_from_str(targets[ti],
								  &c_target),
					 ARDOP_STATIONID_OK);

			unsigned char sid_o = 0;
			int o = IsCallToMe(&s_caller, &s_target, &sid_o) ? 1 : 0;
			uint8_t sid_p = 0;
			int p = ardop_call_to_me(&c_caller, &c_target, &c_mycall,
						 c_aux, n_aux, &sid_p) ? 1 : 0;

			if (o != p)
				fail_msg("'%s'->'%s': legacy %d, port %d",
					 callers[ci], targets[ti], o, p);
			if (o)
				assert_int_equal(sid_p, sid_o);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_session_id_matches_legacy),
		cmocka_unit_test(test_call_to_me_matches_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
