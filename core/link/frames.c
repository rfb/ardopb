#include "link/frames.h"

/**
 * @file frames.c
 * @brief Control-frame builders, ported from Encode4FSKControl /
 *        EncodeConACKwTiming / EncodePingAck (ARDOPC.c).
 */

size_t ardop_encode_control(uint8_t frame_type, uint8_t session_id, uint8_t *out)
{
	out[0] = frame_type;
	out[1] = (uint8_t)(frame_type ^ session_id);
	return 2;
}

size_t ardop_encode_conack_timing(uint8_t frame_type, int rcvd_leader_ms,
				  uint8_t session_id, uint8_t *out)
{
	/* Received leader in tens of ms, capped at 255; out-of-range forces 0. */
	int timing = rcvd_leader_ms / 10;
	if (timing > 255)
		timing = 255;
	if (rcvd_leader_ms > 2550 || rcvd_leader_ms < 0)
		timing = 0;

	out[0] = frame_type;
	out[1] = (uint8_t)(frame_type ^ session_id);
	out[2] = (uint8_t)timing;
	out[3] = (uint8_t)timing;
	out[4] = (uint8_t)timing;
	return 5;
}

size_t ardop_encode_pingack(uint8_t frame_type, int sn, int quality, uint8_t *out)
{
	int info;

	/* Upper 5 bits: S/N -10..21 dB mapped to 0..31, saturating at >= 21 dB. */
	if (sn >= 21)
		info = 0xF8;
	else
		info = ((sn + 10) & 0x1F) << 3;

	/* Lower 3 bits: quality 30..100 mapped to 0..7. */
	int q = (quality - 30) / 10;
	if (q < 0)
		q = 0;
	info += q & 7;

	out[0] = frame_type;
	out[1] = (uint8_t)(frame_type ^ 0xFF);
	out[2] = (uint8_t)info;
	out[3] = (uint8_t)info;
	out[4] = (uint8_t)info;
	return 5;
}
