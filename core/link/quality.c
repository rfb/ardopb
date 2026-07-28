#include "link/quality.h"

/**
 * @file quality.c
 * @brief ACK/NAK decode-quality codec, ported from EncodeDATAACK/EncodeDATANAK
 *        (ARDOPC.c) and the `38 + 2 * (type & 0x1F)` decode (SoundInput.c).
 */

/* Scale a 0..100 quality into the 5-bit field, as both encoders do. */
static uint8_t scale_quality(int quality)
{
	int scaled = (quality / 2) - 19;
	if (scaled < 0)
		scaled = 0;
	return (uint8_t)scaled;
}

uint8_t ardop_quality_to_ack_type(int quality)
{
	/* The ACK encoder clamps quality to 100; the NAK one does not. */
	if (quality > 100)
		quality = 100;
	return (uint8_t)(0xE0u + scale_quality(quality));
}

uint8_t ardop_quality_to_nak_type(int quality)
{
	/* No clamp here, matching EncodeDATANAK (preserved; see the header). */
	return scale_quality(quality);
}

int ardop_quality_from_type(uint8_t frame_type)
{
	return 38 + 2 * (frame_type & 0x1F);
}
