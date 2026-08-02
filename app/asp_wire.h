#ifndef ARDOP_APP_ASP_WIRE_H_
#define ARDOP_APP_ASP_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file asp_wire.h
 * @brief ASP/1 on the wire: framing, varints, and the payload of every message.
 *
 * [analysis/17](../analysis/17-application-protocol.md) is the specification;
 * this is the half of it that is pure. No sockets, no files, no state, no
 * allocation -- given the same bytes it always says the same thing, which is what
 * makes it testable without a radio and what lets the test harness carry its own
 * independent decoder to check this one against (§10, and the exit criterion that
 * two implementations of the framing agree).
 *
 * ## Every message is type, length, payload
 *
 * ```
 * +--------+------------------+--------------------+
 * | type   | length           | payload            |
 * | u8     | varint (1..4 B)  | `length` bytes     |
 * +--------+------------------+--------------------+
 * ```
 *
 * **Length precedes payload for every type, including the ones that have none.**
 * That is what makes the forward-compatibility rule possible: an unknown type is
 * skipped by its length and is never an error, exactly as `ardop_tlm_parse` skips
 * an unknown telemetry record. It is the single rule that lets a version 1 and a
 * version 2 station talk without a flag day.
 *
 * ## Bounds are static, and deliberately so
 *
 * The varint is capped at four bytes and the payload at 4096, so a receiver's
 * buffer is fixed-size and non-allocating like the rest of the tree. A fifth
 * continuation byte or a larger declared length is a protocol error, not a larger
 * buffer.
 *
 * ## The checksum lives here, not in core
 *
 * `core/codec/crc.c` holds the CRCs that are normative to ARDOP itself. CRC-32
 * here is a backstop against framing and resume errors -- the channel already has
 * per-frame CRC and Reed-Solomon underneath it -- and putting a non-protocol
 * checksum beside the protocol ones would blur exactly the line
 * [12](../analysis/12-normative-accidents.md) exists to keep sharp.
 */

/** @brief Largest payload any single message may carry. */
#define ASP_MAX_PAYLOAD 4096

/** @brief Largest encoded message: type + a 4-byte varint + the payload. */
#define ASP_MAX_MESSAGE (1 + 4 + ASP_MAX_PAYLOAD)

/** @brief Longest filename accepted on the wire, in bytes of UTF-8. */
#define ASP_MAX_NAME 255

/** @brief Longest callsign carried in HELLO or TEXT_B. */
#define ASP_MAX_CALL 32

/** @brief The HELLO magic. A peer that does not send this is not speaking ASP. */
#define ASP_MAGIC "ASP/1"

/** @brief Protocol version this implementation speaks. */
#define ASP_VERSION 1

/**
 * @brief Message types.
 *
 * `0x00` is reserved and never valid, so a run of zero bytes -- the commonest
 * thing a desynchronised or half-open stream produces -- can never be mistaken
 * for a message.
 */
typedef enum {
	ASP_MSG_HELLO = 0x01,  /**< Both profiles. Magic, version, callsign, flags. */
	ASP_MSG_BYE = 0x02,    /**< ARQ. Optional UTF-8 reason. */

	ASP_MSG_TEXT = 0x10,   /**< ARQ. UTF-8 message; sender is the peer. */
	ASP_MSG_TEXT_B = 0x11, /**< FEC. Callsign + message id + UTF-8. */

	ASP_MSG_OFFER = 0x20,  /**< ARQ. id, size, crc32, name, content type. */
	ASP_MSG_ACCEPT = 0x21, /**< ARQ. id, have, crc32 of the prefix held. */
	ASP_MSG_REJECT = 0x22, /**< ARQ. id, reason code. */
	ASP_MSG_START = 0x23,  /**< ARQ. id, offset the sender will begin at. */
	ASP_MSG_DATA = 0x24,   /**< ARQ. Raw bytes of the active transfer. */
	ASP_MSG_DONE = 0x25,   /**< ARQ. id. */
	ASP_MSG_RESULT = 0x26, /**< ARQ. id, status code. */
	ASP_MSG_CANCEL = 0x27, /**< ARQ. id, reason code. */
} asp_msg_type;

/** @brief Why a transfer was refused, in ::ASP_MSG_REJECT. */
typedef enum {
	ASP_REJECT_REFUSED = 0,   /**< The operator said no. */
	ASP_REJECT_NO_SPACE,      /**< Not enough room to receive it. */
	ASP_REJECT_TOO_LARGE,     /**< Beyond what this station accepts. */
	ASP_REJECT_UNSUPPORTED,   /**< Content type or feature not handled. */
	ASP_REJECT_BAD_NAME,      /**< The name could not be made safe. */
} asp_reject_code;

/** @brief How a transfer ended, in ::ASP_MSG_RESULT. */
typedef enum {
	ASP_RESULT_OK = 0,
	ASP_RESULT_CRC_MISMATCH,  /**< Whole-file CRC did not match the offer. */
	ASP_RESULT_SHORT,         /**< DONE arrived before the declared size. */
	ASP_RESULT_WRITE_FAILED,  /**< Local storage refused it. */
} asp_result_code;

/** @brief Capability bits in HELLO. None are required to interoperate. */
#define ASP_CAP_FILES  (1u << 0)  /**< Willing to receive OFFERs at all. */
#define ASP_CAP_RESUME (1u << 1)  /**< Keeps partials and will resume them. */
#define ASP_CAP_SHA256 (1u << 2)  /**< Reserved; §5 leaves the door open. */

/* --- varints ---------------------------------------------------------------- */

/**
 * @brief Encode @p v as unsigned LEB128.
 *
 * @return Bytes written (1..4), or 0 if @p cap is too small or @p v needs more
 *         than four bytes -- which no valid length does, since the payload is
 *         capped well below 2^28.
 */
size_t asp_varint_put(uint8_t *out, size_t cap, uint32_t v);

/**
 * @brief Decode an unsigned LEB128.
 *
 * @param[out] v     The value.
 * @param[out] used  Bytes consumed.
 * @return false if @p avail runs out mid-value, or if a fifth continuation byte
 *         appears. The caller cannot tell those apart from the return alone and
 *         does not need to: ::asp_frame_get does, and it is the only caller that
 *         has to distinguish "wait for more" from "this stream is broken".
 */
bool asp_varint_get(const uint8_t *buf, size_t avail, uint32_t *v, size_t *used);

/* --- CRC-32 ----------------------------------------------------------------- */

/**
 * @brief CRC-32 (IEEE 802.3, the zlib polynomial), streamable.
 *
 * Seed with ::ASP_CRC32_INIT for a fresh run and pass the previous return value
 * to continue -- a file is checksummed as it is written, not by being read back.
 */
uint32_t asp_crc32(uint32_t crc, const void *data, size_t len);

/** @brief The seed ::asp_crc32 starts from. */
#define ASP_CRC32_INIT 0u

/* --- framing ---------------------------------------------------------------- */

/**
 * @brief Write one complete message.
 *
 * @return Bytes written, or 0 if it does not fit or @p len exceeds
 *         ::ASP_MAX_PAYLOAD.
 */
size_t asp_frame_put(uint8_t *out, size_t cap, asp_msg_type type,
		     const void *payload, size_t len);

/** @brief What ::asp_frame_get concluded. */
typedef enum {
	ASP_FRAME_OK = 0,     /**< A complete message was read. */
	ASP_FRAME_SHORT,      /**< Not enough bytes yet; accumulate and retry. */
	ASP_FRAME_ERROR,      /**< Malformed. The stream is not resynchronisable. */
} asp_frame_status;

/**
 * @brief Read one message from the front of @p buf.
 *
 * @param[out] type      The type. May be one this build does not know.
 * @param[out] payload   Points into @p buf. Borrowed, valid until @p buf moves.
 * @param[out] len       Payload length.
 * @param[out] consumed  Total bytes this message occupied. Set on
 *                       ::ASP_FRAME_OK, so an unknown type can be skipped.
 *
 * ::ASP_FRAME_ERROR is terminal by design (§8): a reliable ordered stream that
 * has desynchronised means one of the two implementations has a bug, and hunting
 * for a resynchronisation point would turn a loud failure into a quiet one.
 */
asp_frame_status asp_frame_get(const uint8_t *buf, size_t avail,
			       asp_msg_type *type, const uint8_t **payload,
			       size_t *len, size_t *consumed);

/* --- payloads --------------------------------------------------------------- */

/** @brief ::ASP_MSG_HELLO. */
typedef struct {
	uint8_t version;
	uint32_t caps;
	char call[ASP_MAX_CALL];
} asp_hello;

/** @brief ::ASP_MSG_OFFER. */
typedef struct {
	uint16_t id;
	uint32_t size;
	uint32_t crc32;
	char name[ASP_MAX_NAME + 1];
	char content_type[64];
} asp_offer;

/** @brief ::ASP_MSG_ACCEPT. */
typedef struct {
	uint16_t id;
	uint32_t have;         /**< Bytes already held. */
	uint32_t prefix_crc;   /**< CRC-32 of exactly those bytes. */
} asp_accept;

/** @brief ::ASP_MSG_TEXT_B, the FEC broadcast form. */
typedef struct {
	char call[ASP_MAX_CALL];
	uint16_t msg_id;
	char text[ASP_MAX_PAYLOAD];
	size_t text_len;
} asp_text_b;

size_t asp_hello_put(uint8_t *out, size_t cap, const asp_hello *h);
bool asp_hello_get(const uint8_t *p, size_t len, asp_hello *h);

size_t asp_offer_put(uint8_t *out, size_t cap, const asp_offer *o);
bool asp_offer_get(const uint8_t *p, size_t len, asp_offer *o);

size_t asp_accept_put(uint8_t *out, size_t cap, const asp_accept *a);
bool asp_accept_get(const uint8_t *p, size_t len, asp_accept *a);

/** @brief REJECT, RESULT and CANCEL share a shape: id then a one-byte code. */
size_t asp_id_code_put(uint8_t *out, size_t cap, uint16_t id, uint8_t code);
bool asp_id_code_get(const uint8_t *p, size_t len, uint16_t *id, uint8_t *code);

/** @brief START (id, offset) and DONE (id, no offset -- pass NULL). */
size_t asp_start_put(uint8_t *out, size_t cap, uint16_t id, uint32_t from);
bool asp_start_get(const uint8_t *p, size_t len, uint16_t *id, uint32_t *from);

size_t asp_id_put(uint8_t *out, size_t cap, uint16_t id);
bool asp_id_get(const uint8_t *p, size_t len, uint16_t *id);

size_t asp_text_b_put(uint8_t *out, size_t cap, const asp_text_b *t);
bool asp_text_b_get(const uint8_t *p, size_t len, asp_text_b *t);

/* --- filenames -------------------------------------------------------------- */

/**
 * @brief Reduce a peer-supplied name to something safe to create.
 *
 * §5: *"accept a filename from a stranger over the radio and write it to disk"
 * is exactly the shape of bug that is embarrassing to have.* So this is
 * specified rather than left to implementations, and it is a **pure function**
 * so it can be tested against the hostile cases with no filesystem involved.
 *
 * Applied, in order: every directory component is stripped (`/`, `\`, and
 * anything before the last of either, plus a drive letter); control characters
 * and the characters Windows forbids become `_`; leading dots and spaces and
 * trailing dots and spaces are removed; a Windows reserved device name gains a
 * `_` suffix; and the result is truncated to fit.
 *
 * @param[out] out  Receives the safe name, NUL-terminated.
 * @return false if nothing usable survived -- an empty name, or one made
 *         entirely of separators. The caller rejects the transfer; it does not
 *         invent a name, because a file an operator did not expect under a name
 *         nobody chose is worse than a refusal.
 */
bool asp_safe_name(const char *name, char *out, size_t cap);

#endif /* ARDOP_APP_ASP_WIRE_H_ */
