#ifndef ARDOP_CODEC_PACKED6_H_
#define ARDOP_CODEC_PACKED6_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file packed6.h
 * @brief Six-bit packing of callsigns and grid squares.
 *
 * ARDOP identifies stations by callsign and grid square, and it carries them on
 * the air in a compressed form: eight characters from a 64-symbol alphabet fit
 * in six bytes, six bits each. This is normative -- the alphabet, the character
 * folding and the bit order are what let a receiver recover the same string a
 * transmitter packed -- so it may not be changed without breaking interop.
 *
 * The alphabet is ASCII space (32) through underscore (95): digits, uppercase
 * letters and a handful of punctuation. Lowercase letters are folded to
 * uppercase; anything else outside the range is replaced with a space and makes
 * packing report failure.
 *
 * This is a port of `Packed6` from the implementation this project forked from,
 * which was already well factored; the change here is the `ardop_` naming and
 * the stricter build. `test/core/test_packed6.c` checks it against the original
 * so the port is proven faithful rather than assumed so.
 */

/** @brief Number of packed bytes. */
#define ARDOP_PACKED6_SIZE 6

/** @brief Maximum number of characters a packed value represents. */
#define ARDOP_PACKED6_MAX 8

/**
 * @brief Eight alphabet characters compressed into six bytes.
 *
 * A struct rather than a bare array so it is a distinct type the compiler can
 * check, and so it can be returned and assigned by value. The single member is
 * layout-compatible with `uint8_t[ARDOP_PACKED6_SIZE]`.
 */
typedef struct {
	uint8_t b[ARDOP_PACKED6_SIZE];
} ardop_packed6;

/**
 * @brief Pack a NUL-terminated string.
 *
 * Packs up to ARDOP_PACKED6_MAX characters; a shorter string is padded with
 * spaces, a longer one is truncated (and packing reports failure). Lowercase is
 * folded to uppercase; a character outside the alphabet becomes a space and
 * makes the result false.
 *
 * @param[in]  str    NUL-terminated input. NULL is treated as empty.
 * @param[out] packed The packed representation, always written.
 * @return true if every input character was representable and the string fit.
 */
ARDOP_MUSTUSE bool ardop_packed6_from_str(const char *str,
					  ardop_packed6 *packed);

/**
 * @brief Pack the first @p len characters of a string.
 *
 * As ardop_packed6_from_str(), but the length is given explicitly, so the input
 * need not be NUL-terminated and may contain any bytes within @p len.
 *
 * @param[in]  str    Input characters. NULL is treated as empty.
 * @param[in]  len    Number of characters to read from @p str.
 * @param[out] packed The packed representation, always written.
 * @return true if every character was representable and @p len fit in
 *         ARDOP_PACKED6_MAX.
 */
ARDOP_MUSTUSE bool ardop_packed6_from_str_slice(const char *str, size_t len,
						ardop_packed6 *packed);

/**
 * @brief Wrap six already-packed bytes.
 *
 * @param[in]  bytes  Exactly ARDOP_PACKED6_SIZE packed bytes.
 * @param[out] packed The wrapped value.
 */
void ardop_packed6_from_bytes(const uint8_t bytes[ARDOP_PACKED6_SIZE],
			      ardop_packed6 *packed);

/**
 * @brief Unpack into a caller-sized string buffer.
 *
 * @param[in]  packed  The packed value.
 * @param[out] out     Destination buffer; always NUL-terminated when
 *                     @p outsize > 0.
 * @param[in]  outsize Capacity of @p out in bytes.
 * @return true if the full string was written, false if @p out was too small
 *         (needs at least ARDOP_PACKED6_MAX + 1 bytes).
 */
ARDOP_MUSTUSE bool ardop_packed6_to_str(const ardop_packed6 *packed, char *out,
					size_t outsize);

/**
 * @brief Unpack into a fixed buffer that is always large enough.
 *
 * The compile-time-sized counterpart to ardop_packed6_to_str(); it cannot fail.
 *
 * @param[in]  packed The packed value.
 * @param[out] out    Exactly ARDOP_PACKED6_MAX + 1 bytes, NUL-terminated on
 *                    return.
 */
void ardop_packed6_to_fixed_str(const ardop_packed6 *packed,
				char out[ARDOP_PACKED6_MAX + 1]);

#endif /* ARDOP_CODEC_PACKED6_H_ */
