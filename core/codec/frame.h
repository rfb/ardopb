#ifndef ARDOP_CODEC_FRAME_H_
#define ARDOP_CODEC_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file frame.h
 * @brief The ARDOP frame type table.
 *
 * Every frame ARDOP transmits begins with a frame type byte, and that byte
 * determines the entire shape of the frame: how it is modulated, how many
 * carriers it occupies, how many payload and Reed-Solomon bytes each carrier
 * holds, and how good a decode has to be before it is accepted.
 *
 * This is normative. The values here are what makes a transmission
 * interoperable with other ARDOP implementations, and they may not be changed
 * without changing what goes on the air. They are transcribed from
 * `FrameInfo()` in the implementation this project forked from, and
 * `test/core/test_frame.c` re-checks all 256 type bytes against that function
 * so the transcription cannot silently drift.
 */

/** @brief Lowest frame type byte carrying a DataNAK. */
#define ARDOP_FRAME_DATA_NAK_MIN 0x00u
/** @brief Highest frame type byte carrying a DataNAK. */
#define ARDOP_FRAME_DATA_NAK_MAX 0x1Fu
/** @brief Lowest frame type byte carrying a DataACK. */
#define ARDOP_FRAME_DATA_ACK_MIN 0xE0u
/** @brief Highest frame type byte carrying a DataACK. */
#define ARDOP_FRAME_DATA_ACK_MAX 0xFFu

/**
 * @brief Modulation scheme used to carry a frame.
 *
 * An enum rather than the string the old interface copied into a caller's
 * buffer: comparing a scheme is now a machine word rather than `strcmp`, a
 * misspelling is a compile error, and the unbounded `strcpy` into a caller
 * buffer is gone.
 */
typedef enum {
	ARDOP_MOD_4FSK = 0,  /**< 4-level frequency shift keying. */
	ARDOP_MOD_4PSK,      /**< 4-phase shift keying. */
	ARDOP_MOD_8PSK,      /**< 8-phase shift keying. */
	ARDOP_MOD_16QAM,     /**< 16-point quadrature amplitude modulation. */
} ardop_modulation;

/**
 * @brief Everything the rest of the system needs to know about a frame type.
 *
 * Note what is *not* here: whether the frame is odd. That is exactly
 * `frame_type & 1` for all 121 valid types, so storing it would create a
 * second source of truth that could disagree with the first. Use
 * ardop_frame_is_odd() instead.
 */
typedef struct {
	/**
	 * @brief Human-readable name, e.g. "4PSK.200.100.E" or "IDFrame".
	 *
	 * Static storage; never NULL for a spec returned by
	 * ardop_frame_spec_for(). Owned by this module, not the caller.
	 */
	const char *name;

	/** @brief How the frame is modulated. */
	ardop_modulation modulation;

	/** @brief Symbol rate in baud. One of 50, 100 or 600. */
	uint16_t baud;

	/** @brief Number of simultaneous carriers. One of 1, 2, 4 or 8. */
	uint8_t carriers;

	/**
	 * @brief Payload bytes carried by *each* carrier.
	 *
	 * Named per-carrier because the old field was called `intDataLen` and
	 * meant per-carrier, which reads as a whole-frame length and is an easy
	 * factor-of-eight mistake. Use ardop_frame_payload_bytes() for the
	 * whole-frame figure.
	 */
	uint16_t data_bytes_per_carrier;

	/**
	 * @brief Reed-Solomon parity bytes carried by each carrier.
	 *
	 * At most half of these can be spent correcting errors, so the
	 * correction budget per carrier is `rs_bytes_per_carrier / 2`.
	 */
	uint16_t rs_bytes_per_carrier;

	/**
	 * @brief Minimum decode quality, 0-100, before the frame is accepted.
	 *
	 * Lower for the fast data modes, which are expected to run closer to
	 * the noise floor, and higher for the short control frames, where a
	 * false positive is more expensive than a retry.
	 */
	uint8_t quality_threshold;
} ardop_frame_spec;

/**
 * @brief Look up the specification for a frame type byte.
 *
 * @param frame_type The frame type byte, as received or about to be sent.
 * @return A pointer to a static, immutable spec, or NULL if the byte is not a
 *         valid ARDOP frame type. 121 of the 256 possible bytes are valid.
 *
 * The returned pointer is valid for the lifetime of the program and must not
 * be freed.
 */
ARDOP_MUSTUSE const ardop_frame_spec *ardop_frame_spec_for(uint8_t frame_type);

/**
 * @brief Whether a frame type is the odd member of its even/odd pair.
 *
 * ARDOP alternates between the even and odd form of a data frame type so that
 * a receiver can tell a retransmission from the next frame. The distinction is
 * carried in the low bit of the frame type for every valid type.
 */
static inline bool ardop_frame_is_odd(uint8_t frame_type)
{
	return (frame_type & 1u) != 0u;
}

/**
 * @brief Total payload bytes carried by a whole frame, across all carriers.
 */
static inline uint16_t ardop_frame_payload_bytes(const ardop_frame_spec *spec)
{
	return (uint16_t)(spec->carriers * spec->data_bytes_per_carrier);
}

/**
 * @brief Total Reed-Solomon parity bytes in a whole frame, across all carriers.
 */
static inline uint16_t ardop_frame_rs_bytes(const ardop_frame_spec *spec)
{
	return (uint16_t)(spec->carriers * spec->rs_bytes_per_carrier);
}

/**
 * @brief Name of a modulation scheme, e.g. "16QAM".
 *
 * @return A static string, or "?" if the value is not a valid enumerator.
 */
const char *ardop_modulation_name(ardop_modulation modulation);

#endif /* ARDOP_CODEC_FRAME_H_ */
