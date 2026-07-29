#ifndef ARDOP_MODEM_RXQUALITY_H_
#define ARDOP_MODEM_RXQUALITY_H_

#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file rxquality.h
 * @brief Decode-quality metrics from the received constellation.
 *
 * After a frame is demodulated the receiver scores how clean the constellation
 * looked, 0..100. That score is reported back to the sender -- it rides in the
 * ACK/NAK frame type and drives the sender's gear-shifting -- so it is a
 * protocol input, not just a display value. The inherited code computes it as a
 * side effect of drawing the constellation plot; these are the pure computations
 * with the plotting stripped out. See [analysis/02](../../analysis/02-protocol-fsm.md).
 */

/**
 * @brief 4FSK decode quality from per-symbol tone magnitudes.
 *
 * Ported from `Update4FSKConstellation` (its quality path; the plotting is
 * dropped). For each 4-tone symbol it finds the dominant tone and measures how
 * far the other three fall below it, accumulating a distance that a clean symbol
 * makes small; quality is `100 - 2.7 * average_distance`, clamped to 0..100.
 *
 * A faithfully-preserved quirk: the running `rad` value is rescaled by 42/50
 * after each symbol (a leftover of the plot geometry) and carries into the next
 * symbol, so a symbol with zero total energy reuses the previous symbol's scaled
 * radius. This only bites on a dead symbol and is kept for bit-exactness.
 *
 * @param tone_mags     Tone magnitudes, four per symbol (symbol-major).
 * @param tone_mags_len Total entries (4 * number of symbols); must be > 0.
 * @return The decode quality, 0..100.
 */
ARDOP_MUSTUSE int ardop_quality_4fsk(const int32_t *tone_mags,
				     int tone_mags_len);

#endif /* ARDOP_MODEM_RXQUALITY_H_ */
