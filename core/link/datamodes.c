#include "link/datamodes.h"

/**
 * @file datamodes.c
 * @brief The gear-shift mode ladder, ported from GetDataModes /
 *        GetShiftUpThresholds (ARQ.c). The tables are transcribed verbatim.
 */

/* Ordered data frame types per bandwidth, most robust first. */
static const uint8_t modes_200[]        = {0x48, 0x42, 0x40, 0x44, 0x46};
static const uint8_t modes_200_fsk[]    = {0x48};
static const uint8_t modes_500[]        = {0x48, 0x42, 0x40, 0x50, 0x52, 0x54};
static const uint8_t modes_500_fsk[]    = {0x48};
static const uint8_t modes_1000[]       = {0x4C, 0x4A, 0x50, 0x60, 0x62, 0x64};
static const uint8_t modes_1000_fsk[]   = {0x4C, 0x4A};
static const uint8_t modes_2000[]       = {0x4C, 0x4A, 0x50, 0x60, 0x70, 0x72, 0x74};
static const uint8_t modes_2000_fsk[]   = {0x4C, 0x4A};
static const uint8_t modes_2000_fm[]    = {0x4C, 0x4A, 0x7C, 0x7A};
static const uint8_t modes_2000_fmfsk[] = {0x4C, 0x4A, 0x7C, 0x7A};

/* Shift-up quality thresholds, one per mode (top mode's entry unused). */
static const uint8_t thr_200[]      = {82, 84, 84, 85, 0};
static const uint8_t thr_500[]      = {80, 84, 84, 75, 79, 0};
static const uint8_t thr_1000[]     = {80, 80, 80, 80, 75, 0};
static const uint8_t thr_2000[]     = {80, 80, 80, 76, 85, 75, 0};
static const uint8_t thr_2000_fm[]  = {60, 85, 85, 0};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

const uint8_t *ardop_data_modes(int bw_hz, bool fsk_only, int tuning_range,
				bool use_600_modes, size_t *len)
{
	switch (bw_hz) {
	case 200:
		if (fsk_only) { *len = COUNT(modes_200_fsk); return modes_200_fsk; }
		*len = COUNT(modes_200); return modes_200;
	case 500:
		if (fsk_only) { *len = COUNT(modes_500_fsk); return modes_500_fsk; }
		*len = COUNT(modes_500); return modes_500;
	case 1000:
		if (fsk_only) { *len = COUNT(modes_1000_fsk); return modes_1000_fsk; }
		*len = COUNT(modes_1000); return modes_1000;
	case 2000:
		if (tuning_range > 0 && !use_600_modes) {
			if (fsk_only) {
				*len = COUNT(modes_2000_fsk);
				return modes_2000_fsk;
			}
			*len = COUNT(modes_2000);
			return modes_2000;
		}
		if (fsk_only) {
			*len = COUNT(modes_2000_fmfsk);
			return modes_2000_fmfsk;
		}
		*len = COUNT(modes_2000_fm);
		return modes_2000_fm;
	}

	*len = 0;
	return modes_200;   /* arbitrary non-NULL; len 0 means "no modes". */
}

const uint8_t *ardop_shift_up_thresholds(int bw_hz, int tuning_range,
					 bool use_600_modes)
{
	switch (bw_hz) {
	case 200:  return thr_200;
	case 500:  return thr_500;
	case 1000: return thr_1000;
	}
	/* Defaults to the 2000 table, per the original. */
	if (tuning_range > 0 && !use_600_modes)
		return thr_2000;
	return thr_2000_fm;
}
