#include "codec/packed6.h"

#include <string.h>

/*
 * The alphabet is ASCII 32 ('space') through 95 ('_'), stored as the low six
 * bits of (character - 32). Lowercase folds to uppercase first. Anything else
 * is not representable: it becomes a space and the caller is told packing
 * failed.
 */
enum {
	ALPHABET_LOW = ' ',   /* 32 */
	ALPHABET_HIGH = '_',  /* 95 */
	SIX_BITS = 0x3F,
};

/*
 * Pack four alphabet characters into three bytes: four six-bit codes, most
 * significant first, fill a 24-bit word that is emitted big-endian. Returns
 * false if any character was outside the alphabet (that position is packed as a
 * space so the output is still well-formed).
 */
static bool compress_four_to_three(const char in[4], uint8_t out[3])
{
	bool ok = true;
	uint32_t pack = 0;

	for (size_t i = 0; i < 4; i++) {
		uint8_t c = (uint8_t)in[i];

		if (c >= 'a' && c <= 'z')
			c = (uint8_t)(c - ('a' - 'A'));  /* fold to uppercase */

		uint8_t code;
		if (c >= ALPHABET_LOW && c <= ALPHABET_HIGH) {
			code = (uint8_t)(c - ALPHABET_LOW);
		} else {
			code = 0;  /* space */
			ok = false;
		}

		pack = (pack << 6) | (code & SIX_BITS);
	}

	out[0] = (uint8_t)((pack >> 16) & 0xFF);
	out[1] = (uint8_t)((pack >> 8) & 0xFF);
	out[2] = (uint8_t)(pack & 0xFF);

	return ok;
}

/* Undo compress_four_to_three(); always succeeds. Output is not terminated. */
static void decompress_three_to_four(const uint8_t in[3], char out[4])
{
	uint32_t unpack = ((uint32_t)in[0] << 16)
			| ((uint32_t)in[1] << 8)
			| (uint32_t)in[2];

	for (size_t i = 0; i < 4; i++) {
		uint8_t code = (uint8_t)((unpack >> (6 * (3 - i))) & SIX_BITS);
		out[i] = (char)(code + ALPHABET_LOW);
	}
}

bool ardop_packed6_from_str(const char *str, ardop_packed6 *packed)
{
	const char *s = str ? str : "";

	/* Bounded length: never scan past what could possibly fit, +1 so an
	 * over-long string is detected as such by from_str_slice. */
	size_t len = 0;
	while (len <= ARDOP_PACKED6_MAX && s[len] != '\0')
		len++;

	return ardop_packed6_from_str_slice(s, len, packed);
}

bool ardop_packed6_from_str_slice(const char *str, size_t len,
				  ardop_packed6 *packed)
{
	const char *s = str ? str : "";
	bool ok = true;

	if (len > ARDOP_PACKED6_MAX) {
		len = ARDOP_PACKED6_MAX;  /* truncate, and report it */
		ok = false;
	}

	/* Left-justify into an 8-char field, padding short input with spaces.
	 * Copying stops at a NUL inside the slice, matching the precision of the
	 * old "%-8.*s" formatting, so a slice length past the string's end is
	 * padded rather than reading beyond it. */
	char work[ARDOP_PACKED6_MAX];
	size_t i = 0;
	for (; i < len && i < ARDOP_PACKED6_MAX && s[i] != '\0'; i++)
		work[i] = s[i];
	for (; i < ARDOP_PACKED6_MAX; i++)
		work[i] = ' ';

	ok &= compress_four_to_three(&work[0], &packed->b[0]);
	ok &= compress_four_to_three(&work[4], &packed->b[3]);

	return ok;
}

void ardop_packed6_from_bytes(const uint8_t bytes[ARDOP_PACKED6_SIZE],
			      ardop_packed6 *packed)
{
	memcpy(packed->b, bytes, sizeof(packed->b));
}

bool ardop_packed6_to_str(const ardop_packed6 *packed, char *out, size_t outsize)
{
	if (outsize < ARDOP_PACKED6_MAX + 1) {
		if (outsize >= 1)
			out[0] = '\0';
		return false;
	}

	ardop_packed6_to_fixed_str(packed, out);
	return true;
}

void ardop_packed6_to_fixed_str(const ardop_packed6 *packed,
				char out[ARDOP_PACKED6_MAX + 1])
{
	decompress_three_to_four(&packed->b[0], &out[0]);
	decompress_three_to_four(&packed->b[3], &out[4]);
	out[ARDOP_PACKED6_MAX] = '\0';
}
