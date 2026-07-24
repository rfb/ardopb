#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/stationid.h"

/*
 * The original module is the oracle. Its API shares no names with the ardop_
 * port, so both headers coexist here; the port is proven to agree with it and
 * the check goes away when the old module does.
 */
#include "common/StationId.h"

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* Compare old vs new for one NUL-terminated input; fields only when accepted. */
static void expect_same_from_str(const char *s)
{
	StationId lo;
	station_id_err lr = stationid_from_str(s, &lo);

	ardop_stationid nw;
	ardop_stationid_err nr = ardop_stationid_from_str(s, &nw);

	if ((int)lr != (int)nr)
		fail_msg("from_str \"%s\": legacy err %d, port err %d",
			 s, (int)lr, (int)nr);

	if (lr == STATIONID_OK) {
		assert_string_equal(lo.call, nw.call);
		assert_string_equal(lo.ssid, nw.ssid);
		assert_string_equal(lo.str, nw.str);
		assert_memory_equal(lo.wire.b, nw.wire.b, PACKED6_SIZE);
	}
}

/* Curated inputs that reach each parse/validate/compress path. */
static void test_from_str_curated(void **state)
{
	(void)state;

	static const char *const inputs[] = {
		"", "A", "AB", "N0CALL", "n0call", "W1AW", "W1AW-0",
		"W1AW-15", "W1AW-A", "W1AW-z", "W1AW-99", "W1AW-100",
		"TOOLONGCALL", "N0CALL-", "-15", "N0 CALL", "@BC", "AB-",
		"AB-1", "K-1", "VE3ABC-9", "2E0ABC", "M0XYZ-Z", "n0call-a",
		"AB1C-16", "AB1C-41", "AB1C-42", "aaaaaaa-9",
	};

	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++)
		expect_same_from_str(inputs[i]);
}

/* Random callsign-shaped strings, to reach paths the curated list misses. */
static void test_from_str_random(void **state)
{
	(void)state;

	static const char charset[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-abcxyz @#";
	uint32_t rng = 0x51A71D00u;

	for (int iter = 0; iter < 40000; iter++) {
		char buf[16];
		int len = (int)(xorshift32(&rng) % 13u);  /* 0..12 */
		for (int i = 0; i < len; i++) {
			uint32_t k = xorshift32(&rng)
				     % (uint32_t)(sizeof(charset) - 1);
			buf[i] = charset[k];
		}
		buf[len] = '\0';
		expect_same_from_str(buf);
	}
}

/* The explicit-length entry point, including embedded junk after the slice. */
static void test_from_str_slice_matches_legacy(void **state)
{
	(void)state;

	static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-abc ";
	uint32_t rng = 0x00C0FFEEu;

	for (int iter = 0; iter < 40000; iter++) {
		char buf[24];
		int total = (int)(xorshift32(&rng) % 16u);
		for (int i = 0; i < total; i++)
			buf[i] = charset[xorshift32(&rng)
					 % (uint32_t)(sizeof(charset) - 1)];
		buf[total] = '\0';
		size_t len = (total == 0) ? 0
			   : (size_t)(xorshift32(&rng) % (uint32_t)total);

		StationId lo;
		station_id_err lr = stationid_from_str_slice(buf, len, &lo);
		ardop_stationid nw;
		ardop_stationid_err nr =
			ardop_stationid_from_str_slice(buf, len, &nw);

		if ((int)lr != (int)nr)
			fail_msg("from_str_slice \"%s\" len %zu: legacy %d, "
				 "port %d", buf, len, (int)lr, (int)nr);
		if (lr == STATIONID_OK)
			assert_memory_equal(lo.wire.b, nw.wire.b, PACKED6_SIZE);
	}
}

/* Decoding arbitrary wire bytes must agree on both the verdict and the fields. */
static void test_from_bytes_matches_legacy(void **state)
{
	(void)state;

	uint32_t rng = 0xB17E5000u;

	for (int iter = 0; iter < 40000; iter++) {
		uint8_t bytes[PACKED6_SIZE];
		for (int i = 0; i < PACKED6_SIZE; i++)
			bytes[i] = (uint8_t)xorshift32(&rng);

		StationId lo;
		station_id_err lr = stationid_from_bytes(bytes, &lo);
		ardop_stationid nw;
		ardop_stationid_err nr = ardop_stationid_from_bytes(bytes, &nw);

		if ((int)lr != (int)nr)
			fail_msg("from_bytes: legacy err %d, port err %d",
				 (int)lr, (int)nr);
		if (lr == STATIONID_OK) {
			assert_string_equal(lo.call, nw.call);
			assert_string_equal(lo.ssid, nw.ssid);
			assert_string_equal(lo.str, nw.str);
		}
	}
}

/* ok / eq / as_bytes / to_buffer must all match for valid and invalid IDs. */
static void test_accessors_match_legacy(void **state)
{
	(void)state;

	static const char *const inputs[] = {
		"N0CALL", "N0CALL-1", "W1AW", "", "junk!!!", "AB1C-9",
	};
	size_t n = sizeof(inputs) / sizeof(inputs[0]);

	for (size_t i = 0; i < n; i++) {
		StationId lo;
		ardop_stationid nw;
		(void)stationid_from_str(inputs[i], &lo);
		(void)ardop_stationid_from_str(inputs[i], &nw);

		assert_int_equal(stationid_ok(&lo), ardop_stationid_ok(&nw));

		uint8_t ld[PACKED6_SIZE], nd[PACKED6_SIZE];
		bool lb = stationid_to_buffer(&lo, ld);
		bool nb = ardop_stationid_to_buffer(&nw, nd);
		assert_int_equal(lb, nb);
		if (lb)
			assert_memory_equal(ld, nd, PACKED6_SIZE);

		const Packed6 *lp = stationid_as_bytes(&lo);
		const ardop_packed6 *np = ardop_stationid_as_bytes(&nw);
		assert_int_equal(lp == NULL, np == NULL);
		if (lp)
			assert_memory_equal(lp->b, np->b, PACKED6_SIZE);

		for (size_t j = 0; j < n; j++) {
			StationId lo2;
			ardop_stationid nw2;
			(void)stationid_from_str(inputs[j], &lo2);
			(void)ardop_stationid_from_str(inputs[j], &nw2);
			assert_int_equal(stationid_eq(&lo, &lo2),
					 ardop_stationid_eq(&nw, &nw2));
		}
	}
}

/* List parsing and joining must agree, across a range of output capacities. */
static void test_array_matches_legacy(void **state)
{
	(void)state;

	static const char *const lists[] = {
		"N0CALL",
		"N0CALL, W1AW-1 AB2C",
		"  K1ABC ,, W2XYZ-9 ",
		"bad!, N0CALL",
		"",
	};

	for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
		StationId lo[8];
		ardop_stationid nw[8];
		size_t ll = 999, nl = 999;

		station_id_err lr =
			stationid_from_str_to_array(lists[i], lo, 8, &ll);
		ardop_stationid_err nr =
			ardop_stationid_from_str_to_array(lists[i], nw, 8, &nl);

		assert_int_equal((int)lr, (int)nr);
		assert_int_equal(ll, nl);
		if (lr == STATIONID_OK)
			for (size_t k = 0; k < ll; k++)
				assert_string_equal(lo[k].str, nw[k].str);

		if (lr != STATIONID_OK)
			continue;

		for (size_t cap = 0; cap <= 40; cap++) {
			char lbuf[64], nbuf[64];
			memset(lbuf, '#', sizeof(lbuf));
			memset(nbuf, '#', sizeof(nbuf));
			bool lok = stationid_array_to_str(
				lo, ll, lbuf, cap, ", ", "MYAUX", " ");
			bool nok = ardop_stationid_array_to_str(
				nw, nl, nbuf, cap, ", ", "MYAUX", " ");
			assert_int_equal(lok, nok);
			assert_memory_equal(lbuf, nbuf, sizeof(lbuf));
		}
	}
}

/*
 * strerror agrees with the original, except for the one input whose message
 * the original dropped through an off-by-one -- the port returns the real
 * message there. That divergence is asserted outright so it stays intentional.
 */
static void test_strerror(void **state)
{
	(void)state;

	for (int e = 0; e <= ARDOP_STATIONID_ERR_MAX_ + 2; e++) {
		const char *nw =
			ardop_stationid_strerror((ardop_stationid_err)e);

		if (e == ARDOP_STATIONID_ERR_SSID_INVALID) {
			assert_string_equal(nw, "SSID unsupported or too long");
			assert_string_equal(
				stationid_strerror((station_id_err)e),
				"unknown error");
		} else {
			assert_string_equal(stationid_strerror((station_id_err)e),
					    nw);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_from_str_curated),
		cmocka_unit_test(test_from_str_random),
		cmocka_unit_test(test_from_str_slice_matches_legacy),
		cmocka_unit_test(test_from_bytes_matches_legacy),
		cmocka_unit_test(test_accessors_match_legacy),
		cmocka_unit_test(test_array_matches_legacy),
		cmocka_unit_test(test_strerror),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
