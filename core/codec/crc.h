#ifndef ARDOP_CODEC_CRC_H_
#define ARDOP_CODEC_CRC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file crc.h
 * @brief The two cyclic redundancy checks ARDOP puts on the air.
 *
 * These are normative. The polynomials, the initial register values and the
 * bit order are what let a receiver agree with a transmitter about whether a
 * frame arrived intact, so they may not be changed without breaking interop.
 * They are transcribed from `GenCRC16()`/`GenCRC8()` in the implementation this
 * project forked from, and `test/core/test_crc.c` re-checks both against those
 * functions over a large random corpus so the transcription cannot drift.
 *
 * Two incidental hazards from the old interfaces are removed here:
 *   - the length is a `size_t` byte count, not the old `unsigned short`, and
 *   - the CRC-8 takes an explicit length instead of calling `strlen` on its
 *     input, so it can checksum data that contains a zero byte.
 */

/**
 * @brief CRC-16 of a byte range: the frame-integrity check.
 *
 * CRC-16-CCITT, x^16 + x^12 + x^5 + 1, computed MS-bit first with the register
 * seeded to 0xFFFF. This is the value carried in a data frame's trailer; see
 * ardop_crc16_trailer() for how the trailer is formed.
 *
 * @param data Bytes to checksum. May be NULL only if @p len is 0.
 * @param len  Number of bytes.
 * @return The 16-bit CRC.
 */
ARDOP_MUSTUSE uint16_t ardop_crc16(const uint8_t *data, size_t len);

/**
 * @brief CRC-8 of a byte range: used only to derive the ARQ session ID.
 *
 * CRC-8-CCITT, x^8 + x^7 + x^3 + x^2 + 1, computed MS-bit first with the
 * register seeded to 0xFF.
 *
 * @param data Bytes to checksum. May be NULL only if @p len is 0.
 * @param len  Number of bytes.
 * @return The 8-bit CRC.
 */
ARDOP_MUSTUSE uint8_t ardop_crc8(const uint8_t *data, size_t len);

/**
 * @brief The 2-byte trailer that ends a data frame.
 *
 * The trailer is the CRC-16 of @p data, big-endian, with the frame type mixed
 * into the low byte:
 *
 *   trailer[0] = crc >> 8
 *   trailer[1] = (crc & 0xFF) ^ frame_type
 *
 * XORing the frame type in means a frame whose type byte was misread fails the
 * CRC even if its payload is intact, so the type is protected without spending
 * extra bytes on it.
 *
 * @param data       Frame payload to checksum.
 * @param len        Length of @p data in bytes.
 * @param frame_type The frame type byte this trailer belongs to.
 * @param trailer    Two bytes of caller storage, written on return.
 */
void ardop_crc16_trailer(const uint8_t *data, size_t len, uint8_t frame_type,
			 uint8_t trailer[2]);

/**
 * @brief Whether a received trailer matches the payload it follows.
 *
 * The inverse of ardop_crc16_trailer(): recomputes the CRC of @p data and
 * compares it, frame type and all, against the two trailer bytes as received.
 *
 * @param data       Frame payload as received.
 * @param len        Length of @p data in bytes.
 * @param frame_type The frame type byte the receiver believes this is.
 * @param trailer    The two trailer bytes as received.
 * @return true if the trailer is consistent with the payload and frame type.
 */
ARDOP_MUSTUSE bool ardop_crc16_trailer_ok(const uint8_t *data, size_t len,
					  uint8_t frame_type,
					  const uint8_t trailer[2]);

#endif /* ARDOP_CODEC_CRC_H_ */
