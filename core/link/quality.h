#ifndef ARDOP_LINK_QUALITY_H_
#define ARDOP_LINK_QUALITY_H_

#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file quality.h
 * @brief The ACK/NAK decode-quality codec.
 *
 * An ARQ receiver reports how well it decoded a data frame, and that report
 * rides in the frame type itself: a DataACK is one of 0xE0..0xFF and a DataNAK
 * one of 0x00..0x1F, with the low five bits carrying a scaled quality. The
 * sender reads it to drive gear-shifting. These are the pure conversions
 * between a 0..100 quality and those type bytes; the stateful gear-shift logic
 * that consumes them is ported separately.
 * See [analysis/02](../../analysis/02-protocol-fsm.md).
 */

/**
 * @brief The DataACK frame type carrying a given decode quality.
 *
 * Ported from `EncodeDATAACK`: quality is clamped to 100, scaled to the 5-bit
 * field as `max(0, quality/2 - 19)`, and offset into the ACK range. So quality
 * <= 38 encodes as 0xE0 and 100 as 0xFF, in steps of 2.
 *
 * @param quality Decode quality, 0..100 (values above 100 are clamped).
 * @return The DataACK frame type byte, 0xE0..0xFF.
 */
ARDOP_MUSTUSE uint8_t ardop_quality_to_ack_type(int quality);

/**
 * @brief The DataNAK frame type carrying a given decode quality.
 *
 * Ported from `EncodeDATANAK`. Identical scaling to the ACK, but note the
 * original does **not** clamp quality to 100 here (only the ACK path does), so a
 * quality above 100 scales past the 5-bit field and out of the NAK range. In
 * practice quality is always <= 100; the missing clamp is preserved so the byte
 * stays bit-identical for any input.
 *
 * @param quality Decode quality, normally 0..100.
 * @return The DataNAK frame type byte (0x00..0x1F for quality <= 100).
 */
ARDOP_MUSTUSE uint8_t ardop_quality_to_nak_type(int quality);

/**
 * @brief Recover the decode quality from a DataACK or DataNAK frame type.
 *
 * Ported from the `q = 38 + 2 * (frameType & 0x1F)` decode used throughout the
 * receiver. The caller is responsible for only passing an ACK/NAK type; the
 * arithmetic uses just the low five bits, so the representable range is 38..100
 * in steps of 2 (a report of 38 means "38 or worse").
 *
 * @param frame_type A DataACK (0xE0..0xFF) or DataNAK (0x00..0x1F) byte.
 * @return The decode quality, 38..100.
 */
ARDOP_MUSTUSE int ardop_quality_from_type(uint8_t frame_type);

#endif /* ARDOP_LINK_QUALITY_H_ */
