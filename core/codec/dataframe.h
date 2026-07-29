#ifndef ARDOP_CODEC_DATAFRAME_H_
#define ARDOP_CODEC_DATAFRAME_H_

#include <stddef.h>
#include <stdint.h>

#include "codec/rs.h"
#include "common/mustuse.h"

/**
 * @file dataframe.h
 * @brief Encode a payload into the wire bytes of a data frame.
 *
 * The TX counterpart to the demodulator's per-carrier decode: it lays a byte
 * stream out into the frame structure the modulator sends and the receiver
 * pulls apart -- a two-byte type header, then one block per carrier, each a
 * length byte, the carrier's data, a frame-type-mixed CRC-16, and Reed-Solomon
 * parity. Pure byte construction over the codec primitives (frame table, CRC,
 * RS); no DSP, no state of its own.
 */

/** Largest data frame these encoders produce: 8 x 195 (16QAM.2000) + header. */
#define ARDOP_DATAFRAME_MAX 1562

/**
 * @brief Encode @p payload into a data frame of type @p frame_type.
 *
 * Ported from `EncodePSKData` / `EncodeFSKData` (which are byte-identical for a
 * given payload; the sole difference, a `>` vs `>=` fill test, produces the same
 * bytes). Writes the two-byte header `[frame_type][frame_type ^ session_id]`
 * then, for each carrier, `[len][data(data_len)][CRC-16][RS(rs_len)]`, filling
 * carriers in order and zero-padding the last. The 4FSK.2000.600 frame is the
 * special case its original also carves out: one carrier of 759 bytes laid out
 * as three `[len][200][CRC][50]` sub-blocks.
 *
 * @param[in]  rs         RS context whose length set includes the frame's rs_len
 *                        (or rs_len/3 for the 600 frame).
 * @param[in]  frame_type A data frame type.
 * @param[in]  session_id Session id XORed into the header's second byte.
 * @param[in]  payload    Bytes to send.
 * @param[in]  length     Number of payload bytes (must be > 0).
 * @param[out] out        Caller buffer, at least ARDOP_DATAFRAME_MAX bytes.
 * @return The encoded length in bytes, 0 if @p frame_type is not a data frame or
 *         @p length is 0, or -1 if RS encoding fails.
 */
ARDOP_MUSTUSE int ardop_encode_data_frame(const ardop_rs *rs,
					  uint8_t frame_type, uint8_t session_id,
					  const uint8_t *payload, int length,
					  uint8_t *out);

#endif /* ARDOP_CODEC_DATAFRAME_H_ */
