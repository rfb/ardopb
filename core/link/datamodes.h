#ifndef ARDOP_LINK_DATAMODES_H_
#define ARDOP_LINK_DATAMODES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file datamodes.h
 * @brief The gear-shift data-mode ladder.
 *
 * For each session bandwidth the sender has an ordered list of data frame types,
 * most robust first, that gear-shifting walks to trade throughput against
 * robustness. Which list applies depends on the width and two station options
 * (FSK-only, and whether the 600-baud FM modes are in play). These are the pure
 * lookups over those fixed tables; the stateful gear-shift decision that indexes
 * them is ported separately. See [analysis/02](../../analysis/02-protocol-fsm.md).
 */

/**
 * @brief The ordered data frame types to use at a session bandwidth.
 *
 * Ported from `GetDataModes`. Returns the mode list, most robust first, for
 * @p bw_hz (200/500/1000/2000). At 2000 Hz the choice also depends on whether a
 * tuning range is set and the 600-baud modes are disabled. An unknown bandwidth
 * yields an empty list.
 *
 * @param[in]  bw_hz         Session width, Hz.
 * @param[in]  fsk_only      Restrict to 4FSK modes (host FSKONLY).
 * @param[in]  tuning_range  Nonzero if a tuning range is configured.
 * @param[in]  use_600_modes Whether the 600-baud FM modes are in use.
 * @param[out] len           Number of modes in the returned list.
 * @return A pointer to a static, immutable mode list (never NULL; @p *len may
 *         be 0 for an unknown bandwidth).
 */
ARDOP_MUSTUSE const uint8_t *ardop_data_modes(int bw_hz, bool fsk_only,
					      int tuning_range,
					      bool use_600_modes, size_t *len);

/**
 * @brief The shift-up quality thresholds for a session bandwidth.
 *
 * Ported from `GetShiftUpThresholds`. Returns one threshold per mode (in the
 * same order as ardop_data_modes()): the average quality above which
 * gear-shifting may move up from that mode. The top mode's entry is unused (it
 * never shifts up). Indexed with the length from ardop_data_modes().
 *
 * @param bw_hz         Session width, Hz (defaults to the 2000 table otherwise).
 * @param tuning_range  Nonzero if a tuning range is configured (affects 2000).
 * @param use_600_modes Whether the 600-baud FM modes are in use (affects 2000).
 * @return A pointer to a static, immutable threshold list.
 */
ARDOP_MUSTUSE const uint8_t *ardop_shift_up_thresholds(int bw_hz,
						       int tuning_range,
						       bool use_600_modes);

#endif /* ARDOP_LINK_DATAMODES_H_ */
