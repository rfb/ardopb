#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "codec/frame.h"

/*
 * The old implementation, used here as an oracle.
 *
 * Declared locally rather than by including src/common/ARDOPC.h, because that
 * header drags in ~600 lines of unrelated externs and would make this test
 * depend on the very thing it exists to replace. When the old function is
 * finally deleted, this declaration and the equivalence test go with it, and
 * the rest of the file keeps working.
 */
int FrameInfo(unsigned char frame_type, int *odd, int *num_car, char *mod,
	      int *baud, int *data_len, int *rs_len, unsigned char *qual_thresh,
	      char *type_name);

/*
 * Every frame type byte must be classified the same way by the new table and
 * the old function.
 *
 * This is the test that makes replacing a 340-line switch statement with a
 * data table a safe change rather than a hopeful one. It is exhaustive: all
 * 256 possible bytes, valid and invalid, every field.
 */
static void test_matches_legacy_for_all_256_types(void **state)
{
	(void)state;

	int valid_count = 0;

	for (int i = 0; i < 256; i++) {
		uint8_t frame_type = (uint8_t)i;

		int odd = -1;
		int num_car = -1;
		int baud = -1;
		int data_len = -1;
		int rs_len = -1;
		unsigned char qual_thresh = 0;
		/*
		 * The old interface writes into caller buffers with strcpy and
		 * no length, so these are generously sized. Removing that
		 * hazard is one of the reasons for the replacement.
		 */
		char mod[64] = {0};
		char type_name[64] = {0};

		int legacy_ok = FrameInfo(frame_type, &odd, &num_car, mod, &baud,
					  &data_len, &rs_len, &qual_thresh,
					  type_name);

		const ardop_frame_spec *spec = ardop_frame_spec_for(frame_type);

		if (!legacy_ok) {
			/* Both must agree the byte is not a frame type. */
			if (spec != NULL) {
				fail_msg("0x%02X: new table accepts a type the "
					 "old function rejects", frame_type);
			}
			continue;
		}

		if (spec == NULL) {
			fail_msg("0x%02X: new table rejects a type the old "
				 "function accepts (%s)", frame_type, type_name);
		}
		valid_count++;

		/*
		 * Compare every field. fail_msg rather than assert_int_equal so
		 * a failure names the offending frame type -- with 256
		 * iterations, "expected 64, got 32" alone is not actionable.
		 */
		if (strcmp(spec->name, type_name) != 0) {
			fail_msg("0x%02X: name %s, old function says %s",
				 frame_type, spec->name, type_name);
		}
		if (strcmp(ardop_modulation_name(spec->modulation), mod) != 0) {
			fail_msg("0x%02X (%s): modulation %s, old function "
				 "says %s", frame_type, spec->name,
				 ardop_modulation_name(spec->modulation), mod);
		}
		if (spec->baud != baud) {
			fail_msg("0x%02X (%s): baud %u, old function says %d",
				 frame_type, spec->name, spec->baud, baud);
		}
		if (spec->carriers != num_car) {
			fail_msg("0x%02X (%s): carriers %u, old function says "
				 "%d", frame_type, spec->name, spec->carriers,
				 num_car);
		}
		if (spec->data_bytes_per_carrier != data_len) {
			fail_msg("0x%02X (%s): data bytes %u, old function says "
				 "%d", frame_type, spec->name,
				 spec->data_bytes_per_carrier, data_len);
		}
		if (spec->rs_bytes_per_carrier != rs_len) {
			fail_msg("0x%02X (%s): RS bytes %u, old function says "
				 "%d", frame_type, spec->name,
				 spec->rs_bytes_per_carrier, rs_len);
		}
		if (spec->quality_threshold != qual_thresh) {
			fail_msg("0x%02X (%s): quality threshold %u, old "
				 "function says %u", frame_type, spec->name,
				 spec->quality_threshold, qual_thresh);
		}

		/*
		 * The dropped field. The new struct does not store `odd`
		 * because it is derivable; this proves the derivation is
		 * correct for every valid type rather than assuming it.
		 */
		if (ardop_frame_is_odd(frame_type) != (odd != 0)) {
			fail_msg("0x%02X (%s): odd flag disagrees with the old "
				 "function", frame_type, spec->name);
		}
	}

	/*
	 * Guards against the whole loop silently passing because the oracle
	 * rejected everything -- for example if a linkage change made
	 * FrameInfo always fail.
	 */
	assert_int_equal(valid_count, 121);
}

/* Invalid type bytes must be rejected, not returned as a zeroed spec. */
static void test_rejects_invalid_types(void **state)
{
	(void)state;

	/* Sampled from each gap in the valid ranges. */
	const uint8_t invalid[] = {
		0x20, 0x22, 0x25, 0x28, 0x2A, 0x2B, 0x2F, 0x3F,
		0x4E, 0x4F, 0x56, 0x5F, 0x66, 0x6F, 0x76, 0x79,
		0x7E, 0x80, 0xC0, 0xDF,
	};

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert_null(ardop_frame_spec_for(invalid[i]));
	}
}

/*
 * The two 32-byte ranges each describe one frame shape, with the low five bits
 * carrying a quality value. Every byte in the range must resolve, and resolve
 * identically.
 */
static void test_nak_and_ack_ranges_are_uniform(void **state)
{
	(void)state;

	const ardop_frame_spec *first_nak =
		ardop_frame_spec_for(ARDOP_FRAME_DATA_NAK_MIN);
	assert_non_null(first_nak);
	assert_string_equal(first_nak->name, "DataNAK");

	for (unsigned t = ARDOP_FRAME_DATA_NAK_MIN;
	     t <= ARDOP_FRAME_DATA_NAK_MAX; t++) {
		assert_ptr_equal(ardop_frame_spec_for((uint8_t)t), first_nak);
	}

	const ardop_frame_spec *first_ack =
		ardop_frame_spec_for(ARDOP_FRAME_DATA_ACK_MIN);
	assert_non_null(first_ack);
	assert_string_equal(first_ack->name, "DataACK");

	for (unsigned t = ARDOP_FRAME_DATA_ACK_MIN;
	     t <= ARDOP_FRAME_DATA_ACK_MAX; t++) {
		assert_ptr_equal(ardop_frame_spec_for((uint8_t)t), first_ack);
	}
}

/*
 * Invariants that hold across the whole table. These are not transcribed from
 * the old function -- they are properties the protocol requires, so they would
 * catch a table that was faithfully transcribed from a *wrong* oracle.
 */
static void test_table_invariants(void **state)
{
	(void)state;

	for (int i = 0; i < 256; i++) {
		const ardop_frame_spec *spec = ardop_frame_spec_for((uint8_t)i);
		if (spec == NULL) {
			continue;
		}

		assert_non_null(spec->name);
		assert_true(spec->name[0] != '\0');

		/* ARDOP defines exactly three symbol rates. */
		assert_true(spec->baud == 50 || spec->baud == 100
			    || spec->baud == 600);

		/* Carriers are powers of two, one per 250 Hz of bandwidth. */
		assert_true(spec->carriers == 1 || spec->carriers == 2
			    || spec->carriers == 4 || spec->carriers == 8);

		/* A quality threshold is a percentage. */
		assert_true(spec->quality_threshold <= 100);

		/*
		 * Reed-Solomon corrects up to half its parity bytes, so an odd
		 * count would waste one. A frame with no payload carries no
		 * parity.
		 */
		assert_int_equal(spec->rs_bytes_per_carrier % 2, 0);
		if (spec->data_bytes_per_carrier == 0) {
			assert_int_equal(spec->rs_bytes_per_carrier, 0);
		}

		/* The derived helpers must not overflow their return type. */
		assert_true(ardop_frame_payload_bytes(spec) <= 1024);
		assert_true(ardop_frame_rs_bytes(spec) <= 512);
	}
}

/* Data frame types come in even/odd pairs that are identical but for the bit. */
static void test_even_odd_pairs_match(void **state)
{
	(void)state;

	for (unsigned t = 0x40; t <= 0x7D; t += 2) {
		const ardop_frame_spec *even = ardop_frame_spec_for((uint8_t)t);
		const ardop_frame_spec *odd = ardop_frame_spec_for((uint8_t)(t + 1));

		if (even == NULL || odd == NULL) {
			/* Both halves of a pair must be valid, or neither. */
			assert_ptr_equal(even, odd);
			continue;
		}

		assert_int_equal(even->modulation, odd->modulation);
		assert_int_equal(even->baud, odd->baud);
		assert_int_equal(even->carriers, odd->carriers);
		assert_int_equal(even->data_bytes_per_carrier,
				 odd->data_bytes_per_carrier);
		assert_int_equal(even->rs_bytes_per_carrier,
				 odd->rs_bytes_per_carrier);
		assert_int_equal(even->quality_threshold, odd->quality_threshold);

		assert_false(ardop_frame_is_odd((uint8_t)t));
		assert_true(ardop_frame_is_odd((uint8_t)(t + 1)));
	}
}

static void test_modulation_names(void **state)
{
	(void)state;

	assert_string_equal(ardop_modulation_name(ARDOP_MOD_4FSK), "4FSK");
	assert_string_equal(ardop_modulation_name(ARDOP_MOD_4PSK), "4PSK");
	assert_string_equal(ardop_modulation_name(ARDOP_MOD_8PSK), "8PSK");
	assert_string_equal(ardop_modulation_name(ARDOP_MOD_16QAM), "16QAM");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_matches_legacy_for_all_256_types),
		cmocka_unit_test(test_rejects_invalid_types),
		cmocka_unit_test(test_nak_and_ack_ranges_are_uniform),
		cmocka_unit_test(test_table_invariants),
		cmocka_unit_test(test_even_odd_pairs_match),
		cmocka_unit_test(test_modulation_names),
	};

	ardop_test_setup();
	return cmocka_run_group_tests(tests, NULL, NULL);
}
