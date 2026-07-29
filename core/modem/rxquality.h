#ifndef ARDOP_MODEM_RXQUALITY_H_
#define ARDOP_MODEM_RXQUALITY_H_

#include <stdbool.h>
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

/**
 * @brief PSK/QAM decode quality from per-symbol differential phases and mags.
 *
 * Ported from `UpdatePhaseConstellation` (its quality path). For each symbol
 * after the reference it measures the phase error from the nearest constellation
 * point; quality falls as `200 * average_phase_error / phase_step` grows. For
 * 16QAM it additionally splits the symbols into an inner and outer amplitude
 * ring and scales the score down by the average radius error of each. The
 * reference symbol (index 0) is excluded. Uses the reduced-precision pi the rest
 * of the modem uses.
 *
 * @param phases     Differential phases, milliradians, one per symbol.
 * @param mags       Symbol magnitudes, one per symbol.
 * @param phases_len Number of symbols including the reference (> 1).
 * @param psk_order  4 or 8 (the PSK order; ignored and treated as 8 for QAM).
 * @param is_qam     True for 16QAM (adds the amplitude-ring term).
 * @return The decode quality, >= 0 (not clamped above; ~100 for a clean frame).
 */
ARDOP_MUSTUSE int ardop_quality_psk(const short *phases, const short *mags,
				    int phases_len, int psk_order, bool is_qam);

#endif /* ARDOP_MODEM_RXQUALITY_H_ */
