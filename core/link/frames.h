#ifndef ARDOP_LINK_FRAMES_H_
#define ARDOP_LINK_FRAMES_H_

#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file frames.h
 * @brief Builders for the small control frames the link sends.
 *
 * When the FSM decides to send a control frame it needs the encoded bytes to
 * hand to the modulator. These are the pure encoders for the fixed-shape
 * control frames -- the payload the FSM attaches to a "send frame" action. They
 * write into a caller-owned buffer and never allocate. The data-frame path
 * (payload + RS) is the codec layer's job; these cover the protocol's
 * handshake and signalling frames.
 *
 * Every control frame repeats its content for redundancy and carries the
 * session id in its second byte as `frame_type ^ session_id`, which is how a
 * receiver rejects a co-channel session's frames.
 */

/** Largest control frame these builders produce (ConACK/PingAck = 5 bytes). */
#define ARDOP_CONTROL_FRAME_MAX 5

/**
 * @brief Encode a 2-byte short control frame (BREAK, END, DISC, IDLE, ACK, …).
 *
 * Ported from `Encode4FSKControl`. Emits `[frame_type][frame_type ^ session_id]`.
 * This is also how a DataACK/DataNAK is framed once its quality-bearing type
 * byte is chosen (see quality.h).
 *
 * @param[in]  frame_type  The control frame type byte.
 * @param[in]  session_id  Session id XORed into the second byte.
 * @param[out] out         Caller buffer, at least 2 bytes.
 * @return The frame length, 2.
 */
ARDOP_MUSTUSE size_t ardop_encode_control(uint8_t frame_type, uint8_t session_id,
					  uint8_t *out);

/**
 * @brief Encode a Connect-ACK frame carrying leader-timing feedback.
 *
 * Ported from `EncodeConACKwTiming`. Emits `[type][type ^ session_id][t][t][t]`
 * where `t` is the received leader length in tens of milliseconds, capped at
 * 255 and forced to 0 if out of the 0..2550 ms range. The timing byte is
 * repeated three times for redundancy.
 *
 * @param[in]  frame_type      A ConAck frame type.
 * @param[in]  rcvd_leader_ms  Measured leader length, ms.
 * @param[in]  session_id      Session id XORed into the second byte.
 * @param[out] out             Caller buffer, at least 5 bytes.
 * @return The frame length, 5.
 */
ARDOP_MUSTUSE size_t ardop_encode_conack_timing(uint8_t frame_type,
						int rcvd_leader_ms,
						uint8_t session_id,
						uint8_t *out);

/**
 * @brief Encode a Ping-ACK frame carrying signal-to-noise and quality.
 *
 * Ported from `EncodePingAck`. Emits `[type][type ^ 0xFF][b][b][b]` where the
 * info byte `b` packs S/N in the upper five bits (−10..21 dB, saturating to a
 * max code at ≥21 dB) and quality in the lower three (30..100 in steps of 10).
 * A PingAck is not session-scoped, so its second byte uses 0xFF, not a session
 * id. The info byte is repeated three times for redundancy.
 *
 * @param[in]  frame_type  The PingAck frame type.
 * @param[in]  sn          Signal-to-noise ratio, dB.
 * @param[in]  quality     Decode quality, 30..100.
 * @param[out] out         Caller buffer, at least 5 bytes.
 * @return The frame length, 5.
 */
ARDOP_MUSTUSE size_t ardop_encode_pingack(uint8_t frame_type, int sn, int quality,
					  uint8_t *out);

#endif /* ARDOP_LINK_FRAMES_H_ */
