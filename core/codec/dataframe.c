#include "codec/dataframe.h"

#include <string.h>

#include "codec/crc.h"
#include "codec/frame.h"

/**
 * @file dataframe.c
 * @brief Data-frame byte encoder, ported from EncodePSKData / EncodeFSKData
 *        (ARDOPC.c).
 */

int ardop_encode_data_frame(const ardop_rs *rs, uint8_t frame_type,
			    uint8_t session_id, const uint8_t *payload,
			    int length, uint8_t *out)
{
	const ardop_frame_spec *spec = ardop_frame_spec_for(frame_type);

	if (spec == NULL || spec->data_bytes_per_carrier == 0 || length <= 0)
		return 0;

	/* Blocks and their per-block data/RS sizes. The 4FSK.2000.600 frame is a
	 * single carrier whose 600 data bytes are sent as three sequential
	 * [len][200][CRC][50] sub-blocks; every other frame is one block per
	 * carrier. */
	int n_blocks = spec->carriers;
	int block_data = spec->data_bytes_per_carrier;
	int block_rs = spec->rs_bytes_per_carrier;
	if (spec->modulation == ARDOP_MOD_4FSK
	    && spec->data_bytes_per_carrier == 600) {
		n_blocks = 3;
		block_data = 200;
		block_rs = 50;
	}

	out[0] = frame_type;
	out[1] = (uint8_t)(frame_type ^ session_id);

	int sent = 0;
	int ptr = 2;
	for (int b = 0; b < n_blocks; b++) {
		uint8_t *block = &out[ptr];
		int block_len = block_data + 3 + block_rs;
		int remaining = length - sent;
		int take = remaining >= block_data ? block_data : remaining;
		if (take < 0)
			take = 0;

		memset(block, 0, (size_t)block_len);   /* zero-pad the tail. */
		block[0] = (uint8_t)take;
		if (take > 0) {
			memcpy(&block[1], &payload[sent], (size_t)take);
			sent += take;
		}

		/* CRC-16 over the length byte + data, frame type mixed in. */
		uint8_t trailer[2];
		ardop_crc16_trailer(block, (size_t)(block_data + 1), frame_type,
				    trailer);
		block[block_data + 1] = trailer[0];
		block[block_data + 2] = trailer[1];

		if (ardop_rs_append(rs, block, block_data + 3, block_rs) != 0)
			return -1;

		ptr += block_len;
	}

	return ptr;
}
