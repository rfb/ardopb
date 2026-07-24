#include "codec/crc.h"

/*
 * Both CRCs are the same shift-and-conditionally-XOR loop, differing only in
 * width, polynomial and seed. The old code wrote the two branches of the
 * conditional XOR out in full (`0xFFFF & (1 + (reg << 1))` etc.); the shift is
 * identical on both sides, so it is hoisted out here and only the XOR is
 * conditional. The result is bit-for-bit the same -- test/core/test_crc.c
 * proves it against the originals.
 *
 * The register is held in the exact-width unsigned type, so the truncation the
 * old code spelled as `& 0xFFFF` / `& 0xFF` happens on assignment instead.
 */

uint16_t ardop_crc16(const uint8_t *data, size_t len)
{
	/* CRC-16-CCITT: x^16 + x^12 + x^5 + 1, MS-bit first, seed 0xFFFF. */
	const uint16_t poly = 0x8810u;
	uint16_t reg = 0xFFFFu;

	for (size_t j = 0; j < len; j++) {
		for (int bit = 7; bit >= 0; bit--) {
			bool in_bit = (data[j] >> bit) & 1u;
			bool msb_was_set = (reg & 0x8000u) != 0u;

			reg = (uint16_t)((reg << 1) | (in_bit ? 1u : 0u));
			if (msb_was_set)
				reg ^= poly;
		}
	}

	return reg;
}

uint8_t ardop_crc8(const uint8_t *data, size_t len)
{
	/* CRC-8-CCITT: x^8 + x^7 + x^3 + x^2 + 1, MS-bit first, seed 0xFF. */
	const uint8_t poly = 0xC6u;
	uint8_t reg = 0xFFu;

	for (size_t j = 0; j < len; j++) {
		for (int bit = 7; bit >= 0; bit--) {
			bool in_bit = (data[j] >> bit) & 1u;
			bool msb_was_set = (reg & 0x80u) != 0u;

			reg = (uint8_t)((reg << 1) | (in_bit ? 1u : 0u));
			if (msb_was_set)
				reg ^= poly;
		}
	}

	return reg;
}

void ardop_crc16_trailer(const uint8_t *data, size_t len, uint8_t frame_type,
			 uint8_t trailer[2])
{
	uint16_t crc = ardop_crc16(data, len);

	trailer[0] = (uint8_t)(crc >> 8);
	trailer[1] = (uint8_t)((crc & 0xFFu) ^ frame_type);
}

bool ardop_crc16_trailer_ok(const uint8_t *data, size_t len, uint8_t frame_type,
			    const uint8_t trailer[2])
{
	uint8_t expected[2];

	ardop_crc16_trailer(data, len, frame_type, expected);

	return trailer[0] == expected[0] && trailer[1] == expected[1];
}
