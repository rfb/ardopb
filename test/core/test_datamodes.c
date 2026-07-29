#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/datamodes.h"

/*
 * The inherited GetDataModes / GetShiftUpThresholds are the oracles. They read
 * FSKOnly, TuningRange and Use600Modes from globals and, for GetDataModes, set
 * the mode count in bytFrameTypesForBWLength. Declared here rather than via
 * ARQ.c's headers.
 */
unsigned char *GetDataModes(int intBW);
unsigned char *GetShiftUpThresholds(int intBW);
extern int FSKOnly;
extern int TuningRange;
extern int Use600Modes;
extern int bytFrameTypesForBWLength;

/*
 * The mode ladder and its shift-up thresholds must match the originals for every
 * bandwidth across the option combinations that select between the tables
 * (FSK-only, tuning range, 600-baud modes) -- including the 2000 Hz split and an
 * unknown bandwidth (empty list).
 */
static void test_data_modes_match_legacy(void **state)
{
	(void)state;

	const int bws[] = {200, 500, 1000, 2000, 1234};

	for (size_t bi = 0; bi < sizeof bws / sizeof bws[0]; bi++) {
		int bw = bws[bi];
		for (int fsk = 0; fsk <= 1; fsk++) {
			for (int tr = 0; tr <= 1; tr++) {
				for (int u6 = 0; u6 <= 1; u6++) {
					FSKOnly = fsk;
					TuningRange = tr ? 100 : 0;
					Use600Modes = u6;

					const unsigned char *oracle =
						GetDataModes(bw);
					int olen = bytFrameTypesForBWLength;

					size_t plen = 999;
					const uint8_t *port = ardop_data_modes(
						bw, fsk, TuningRange, u6, &plen);

					if ((int)plen != olen)
						fail_msg("modes bw=%d fsk=%d tr=%d"
							 " u6=%d: len port %zu,"
							 " legacy %d", bw, fsk,
							 tr, u6, plen, olen);
					if (plen > 0)
						assert_memory_equal(port, oracle,
								    plen);

					/* Thresholds: compare over the mode
					 * count. */
					const unsigned char *othr =
						GetShiftUpThresholds(bw);
					const uint8_t *pthr =
						ardop_shift_up_thresholds(
							bw, TuningRange, u6);
					if (plen > 0)
						assert_memory_equal(pthr, othr,
								    plen);
				}
			}
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_data_modes_match_legacy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
