#ifndef ARDOP_APP_ASP_H_
#define ARDOP_APP_ASP_H_

#include "app/asp_wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file asp.h
 * @brief An ASP/1 session: chat and file transfer over one ARQ connection.
 *
 * [analysis/17](../analysis/17-application-protocol.md). This is the stateful
 * half; `asp_wire.h` is the pure one.
 *
 * ## It owns no transport and no storage
 *
 * Bytes arrive by ::asp_recv and leave through ::asp_io::send. File content
 * arrives and leaves through the same table. That is not indirection for its own
 * sake -- it is what lets the whole protocol be driven by a test with two
 * sessions, a byte queue and a memory buffer, at whatever speed the test likes,
 * with no radio, no filesystem and no clock. §10 calls that out as the reason
 * this layer can be built in parallel with everything else.
 *
 * The application supplies a table that writes to the spine and to disk; the
 * loopback test supplies one that writes to arrays.
 *
 * ## Raw mode is decided once, on the first byte
 *
 * §2: a peer that does not open with a well-formed HELLO is not speaking ASP,
 * and the session degrades to unframed UTF-8 in both directions -- chat works,
 * files do not, and the interface says so. **The decision is never revisited
 * within a connection.** That matters because most stations on the air are
 * running plain ardopcf or a terminal, and rag-chewing correctly with them is a
 * feature rather than a fallback grudgingly tolerated.
 *
 * ## One transfer per direction
 *
 * §4: concurrency at 300 B/s makes both transfers slower and neither more likely
 * to finish. `DATA` therefore carries no id and no offset -- it belongs to the
 * one active transfer in that direction and continues where the last chunk
 * stopped. Additional offers queue above this layer.
 */

/** @brief How far a session has got with its peer. */
typedef enum {
	ASP_LINK_IDLE = 0,   /**< No connection. */
	ASP_LINK_HELLO_SENT, /**< Ours is out; the peer's first byte decides. */
	ASP_LINK_ASP,        /**< The peer speaks ASP. Everything is available. */
	ASP_LINK_RAW,        /**< The peer does not. Chat only, unframed. */
} asp_link_state;

/** @brief What an outbound or inbound transfer is doing. */
typedef enum {
	ASP_XFER_NONE = 0,
	ASP_XFER_OFFERED,    /**< OFFER sent, waiting for ACCEPT or REJECT. */
	ASP_XFER_RUNNING,    /**< DATA is flowing. */
	ASP_XFER_AWAIT_RESULT, /**< DONE sent, waiting for RESULT. */
	ASP_XFER_RECEIVING,  /**< An inbound transfer is arriving. */
} asp_xfer_state;

/**
 * @brief Everything the session needs from the world outside it.
 *
 * Every function may be called from ::asp_recv or ::asp_service. None may block.
 */
typedef struct {
	void *ctx;

	/**
	 * @brief Send bytes to the peer.
	 * @return Bytes accepted, which may be fewer than @p len or zero. The
	 *         session retries the remainder later rather than assuming
	 *         everything went, because the transmit queue is finite and
	 *         admission is the spine's to refuse.
	 */
	size_t (*send)(void *ctx, const void *data, size_t len);

	/** @brief Bytes ::send would accept right now. */
	size_t (*credit)(void *ctx);

	/**
	 * @brief Read from the file being offered.
	 * @return Bytes read, or 0 at the end or on error.
	 */
	size_t (*read_file)(void *ctx, uint32_t offset, void *out, size_t len);

	/**
	 * @brief Append to the file being received.
	 * @return false on any failure; the session answers RESULT_WRITE_FAILED.
	 */
	bool (*write_file)(void *ctx, const void *data, size_t len);

	/**
	 * @brief Decide what to do with an offer.
	 *
	 * §5: auto-accept is off by default, because a station that accepts
	 * arbitrary files from any caller will eventually receive something its
	 * operator did not want. An implementation that wants a prompt returns
	 * false here and calls ::asp_answer_offer when the operator has decided.
	 *
	 * @param[out] have        Bytes already held for this transfer, for resume.
	 * @param[out] prefix_crc  CRC-32 of exactly those bytes.
	 * @return true to accept now, false to defer to ::asp_answer_offer.
	 */
	bool (*offer_arrived)(void *ctx, const asp_offer *offer, uint32_t *have,
			      uint32_t *prefix_crc);

	/** @brief Truncate the partial file; the sender is restarting from zero. */
	void (*truncate_file)(void *ctx);

	/** @brief A chat line arrived. @p raw when the peer is not speaking ASP. */
	void (*text_arrived)(void *ctx, const char *text, size_t len, bool raw);

	/** @brief A transfer ended, one way or the other. */
	void (*transfer_done)(void *ctx, bool inbound, uint16_t id,
			      asp_result_code result);

	/** @brief Progress, for a UI. Called as bytes move. */
	void (*progress)(void *ctx, bool inbound, uint32_t done, uint32_t total);

	/** @brief Something went wrong at the protocol level; the link should end. */
	void (*protocol_error)(void *ctx, const char *why);
} asp_io;

/** @brief One session. Caller-owned; no allocation anywhere in this layer. */
typedef struct {
	const asp_io *io;
	asp_link_state state;

	char my_call[ASP_MAX_CALL];
	char peer_call[ASP_MAX_CALL];
	uint32_t peer_caps;

	/* Inbound framing. One message is at most ASP_MAX_MESSAGE, and a partial
	 * one waits here for the rest. */
	uint8_t in[ASP_MAX_MESSAGE];
	size_t in_len;

	/* Outbound, for what the transmit queue would not take yet. */
	uint8_t out[ASP_MAX_MESSAGE];
	size_t out_len;
	size_t out_sent;

	/* The one outbound transfer. */
	asp_xfer_state tx_state;
	uint16_t tx_id;
	uint32_t tx_size, tx_offset;
	uint32_t tx_crc;

	/* The one inbound transfer. */
	asp_xfer_state rx_state;
	uint16_t rx_id;
	uint32_t rx_size, rx_have;
	uint32_t rx_crc;          /**< Expected, from the offer. */
	uint32_t rx_running_crc;  /**< Computed as it arrives. */
	char rx_name[ASP_MAX_NAME + 1];

	uint16_t next_id;
} asp_session;

/**
 * @brief Start a session and send our HELLO.
 *
 * Call on every new ARQ connection. The peer's first byte decides whether this
 * becomes ::ASP_LINK_ASP or ::ASP_LINK_RAW.
 */
void asp_open(asp_session *s, const asp_io *io, const char *my_call);

/** @brief Abandon the session. Any transfer in progress is left resumable. */
void asp_close(asp_session *s);

/** @brief Feed bytes received from the peer. */
void asp_recv(asp_session *s, const void *data, size_t len);

/**
 * @brief Move whatever is waiting: queued output, then transfer data.
 *
 * Call whenever transmit credit may have changed -- §7: the app receives buffer
 * events, so this needs no polling loop of its own.
 */
void asp_service(asp_session *s);

/** @brief Send a chat line. Works in raw mode too. @return false if refused. */
bool asp_send_text(asp_session *s, const char *text, size_t len);

/**
 * @brief Offer a file.
 *
 * @param name  Sent as-is; the receiver sanitises it. Ours should already be a
 *              basename.
 * @param size  Total bytes, which ::asp_io::read_file must be able to supply.
 * @param crc32 CRC-32 of the whole file.
 * @return false if a transfer is already running or the peer cannot take files.
 */
bool asp_offer_file(asp_session *s, const char *name, const char *content_type,
		    uint32_t size, uint32_t crc32);

/** @brief Answer an offer that ::asp_io::offer_arrived deferred. */
bool asp_answer_offer(asp_session *s, bool accept, uint32_t have,
		      uint32_t prefix_crc, asp_reject_code reason);

/** @brief Cancel the active transfer in either direction. Resumable. */
bool asp_cancel(asp_session *s, bool inbound, asp_reject_code reason);

/** @brief Whether files are possible: the peer speaks ASP and accepts them. */
bool asp_can_send_files(const asp_session *s);

#endif /* ARDOP_APP_ASP_H_ */
