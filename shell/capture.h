#ifndef ARDOP_SHELL_CAPTURE_H_
#define ARDOP_SHELL_CAPTURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file capture.h
 * @brief A session, written to a classic `.pcap` file for offline review.
 *
 * Every frame the modem sends or receives (decoded or not), and the session's
 * narrative around them -- leader acquisitions, link-state transitions, PTT
 * timing, negotiated bandwidth, busy-detector state and delivered application
 * data -- as one time-ordered stream, so a session can be examined afterward
 * instead of relayed live. `apps/ardop-pcap-dump` reads it back as text;
 * `wireshark -r FILE` also opens it, as raw `LINKTYPE_USER0` frames (147, one
 * of tcpdump/Wireshark's private-use range -- nothing here is a real protocol
 * a dissector already knows).
 *
 * @par Wire format.
 * A standard 24-byte pcap global header, then one pcap packet per event: pcap's
 * own 16-byte record header (wall-clock timestamp), wrapping *our* record --
 * a 3-byte prefix (magic, version, ::ardop_capture_kind) followed by a
 * kind-specific body. Little-endian throughout, written byte-at-a-time rather
 * than by struct copy, for the same reason as telemetry.h: this file may be
 * read back on a different machine than the one that wrote it.
 *
 * @par Frames vs. everything else.
 * ::ARDOP_CAPTURE_FRAME_RX / _TX / _RX_FAILED carry a frame's raw bytes, which
 * exist at exactly three places in `shell/runtime.c` -- the caller passes them
 * explicitly via ::ardop_capture_write_frame. Every other kind is already
 * carried, scalar-complete, by the observation stream (`ardop_obs` in
 * runtime.h), so it is written from one place: the `emit()` function every
 * observation already passes through.
 */

typedef struct ardop_capture ardop_capture;

/** @brief What a record carries. See capture.h's file comment for the split. */
typedef enum {
	ARDOP_CAPTURE_FRAME_RX = 0,      /**< Decoded (frame_type, quality, sn, payload). */
	ARDOP_CAPTURE_FRAME_TX = 1,      /**< Handed to the modulator. quality/sn = -1. */
	ARDOP_CAPTURE_FRAME_RX_FAILED = 2, /**< Acquired, not decoded. No payload. */
	ARDOP_CAPTURE_LEADER = 3,        /**< Acquisition, whether or not framing followed. */
	ARDOP_CAPTURE_STATE = 4,         /**< Link state changed. */
	ARDOP_CAPTURE_PTT = 5,           /**< Transmitter keyed/unkeyed. */
	ARDOP_CAPTURE_BANDWIDTH = 6,     /**< Negotiated session width changed. */
	ARDOP_CAPTURE_BUSY = 7,          /**< Channel-busy state changed. */
	ARDOP_CAPTURE_RX_DATA = 8,       /**< Delivered application payload (ARQ/FEC). */
	ARDOP_CAPTURE_HOST_MSG = 9,      /**< Host-protocol status text. */
} ardop_capture_kind;

/** @brief Record magic, one byte: 'A'. */
#define ARDOP_CAPTURE_MAGIC 0x41u

/** @brief Wire format version. Bumped on any incompatible layout change. */
#define ARDOP_CAPTURE_VERSION 1u

/** @brief Bytes in a record's shared prefix (magic, version, kind). */
#define ARDOP_CAPTURE_PREFIX_LEN 3

/**
 * @brief Largest variable-length part (a frame payload, RX_DATA, or host
 * message text) a record carries whole.
 *
 * Real ARDOP frames and status lines are a small fraction of this; it exists
 * so a caller can size one fixed buffer rather than compute a bound per kind.
 * Anything longer is truncated to fit -- a capture losing the tail of one
 * oversized record is preferable to losing the record, or the process.
 */
#define ARDOP_CAPTURE_MAX_PAYLOAD 4096u

/** @brief Largest whole record this format ever writes, prefix included. */
#define ARDOP_CAPTURE_MAX_RECORD (300u + ARDOP_CAPTURE_MAX_PAYLOAD)

/**
 * @brief One decoded record. Flat like ::ardop_obs: read only the fields the
 * @p kind documents. Pointers are borrowed, valid only for the call, and are
 * *not* NUL-terminated -- each has its own explicit length.
 */
typedef struct {
	uint8_t version;
	ardop_capture_kind kind;

	uint8_t frame_type;             /**< FRAME_*. */
	int16_t quality;                /**< FRAME_*: -1 if none (TX, RX_FAILED). */
	int16_t sn;                     /**< FRAME_*, LEADER: -1 if none. */
	uint16_t bandwidth_hz;          /**< FRAME_*, BANDWIDTH. */
	const uint8_t *payload;         /**< FRAME_*, or RX_DATA's data. */
	uint16_t payload_len;

	float offset_hz;                /**< LEADER. */

	uint8_t link_state;             /**< STATE: an ::ardop_link_state value. */
	const char *remote;             /**< STATE: the other station, or "". */
	uint8_t remote_len;

	bool flag;                      /**< PTT (key), BUSY. */

	const char *tag;                /**< RX_DATA: "ARQ" or "FEC". */
	uint8_t tag_len;

	const char *text;               /**< HOST_MSG. */
	uint16_t text_len;
} ardop_capture_record;

/* --- pure: encode into a caller buffer, decode from one ------------------- */

/**
 * @brief Encode one frame record (RX, TX, or RX_FAILED).
 * @return Bytes written, or 0 if @p cap is too small for even the fixed part.
 *         @p payload_len is truncated to fit @p cap, not refused.
 */
size_t ardop_capture_encode_frame(uint8_t *out, size_t cap,
				  ardop_capture_kind kind, uint8_t frame_type,
				  int16_t quality, int16_t sn,
				  uint16_t bandwidth_hz, const uint8_t *payload,
				  uint16_t payload_len);

size_t ardop_capture_encode_leader(uint8_t *out, size_t cap, float offset_hz,
				   int16_t sn);
size_t ardop_capture_encode_state(uint8_t *out, size_t cap, uint8_t link_state,
				  const char *remote);
size_t ardop_capture_encode_ptt(uint8_t *out, size_t cap, bool key);
size_t ardop_capture_encode_bandwidth(uint8_t *out, size_t cap,
				      uint16_t bandwidth_hz);
size_t ardop_capture_encode_busy(uint8_t *out, size_t cap, bool busy);
size_t ardop_capture_encode_rx_data(uint8_t *out, size_t cap, const char *tag,
				    const uint8_t *data, uint16_t data_len);
size_t ardop_capture_encode_host_msg(uint8_t *out, size_t cap,
				     const char *text);

/**
 * @brief Decode one record.
 * @return false if @p avail is short, or magic/version don't match -- never a
 *         half-read.
 */
bool ardop_capture_parse_record(const uint8_t *buf, size_t avail,
				ardop_capture_record *out);

/* --- impure: a file on disk ------------------------------------------------ */

/**
 * @brief Open @p path and write the pcap global header (`LINKTYPE_USER0`).
 * @return NULL on failure; a diagnostic is printed to stderr.
 */
ardop_capture *ardop_capture_open(const char *path);

/**
 * @brief Write one frame record, wrapped in a pcap packet stamped with the
 * current wall-clock time.
 *
 * NULL @p cap is a silent no-op, so every call site can be unconditional. A
 * write failure (a full disk) is absorbed the same way: a debugging aid must
 * never be the reason a session goes wrong.
 */
void ardop_capture_write_frame(ardop_capture *cap, ardop_capture_kind kind,
			       uint8_t frame_type, int16_t quality, int16_t sn,
			       uint16_t bandwidth_hz, const uint8_t *payload,
			       uint16_t payload_len);

void ardop_capture_write_leader(ardop_capture *cap, float offset_hz,
				int16_t sn);
void ardop_capture_write_state(ardop_capture *cap, uint8_t link_state,
			       const char *remote);
void ardop_capture_write_ptt(ardop_capture *cap, bool key);
void ardop_capture_write_bandwidth(ardop_capture *cap, uint16_t bandwidth_hz);
void ardop_capture_write_busy(ardop_capture *cap, bool busy);
void ardop_capture_write_rx_data(ardop_capture *cap, const char *tag,
				 const uint8_t *data, size_t data_len);
void ardop_capture_write_host_msg(ardop_capture *cap, const char *text);

/** @brief Close and free. NULL is fine. */
void ardop_capture_close(ardop_capture *cap);

#endif /* ARDOP_SHELL_CAPTURE_H_ */
