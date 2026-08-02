#ifndef ARDOP_APP_ASP_APP_H_
#define ARDOP_APP_ASP_APP_H_

#include "app/asp.h"
#include "app/spine.h"

#include <stdio.h>

/**
 * @file asp_app.h
 * @brief ASP bound to a real spine and real files.
 *
 * `asp.h` deliberately owns no transport and no storage, which is what lets the
 * protocol be proved against arrays with no radio. This is the other half: the
 * ::asp_io that reads and writes actual files and puts actual bytes into the
 * modem's transmit queue.
 *
 * It is a separate object for the reason `app/devices.c` is: it is the only file
 * that names both the protocol and the spine, so neither has to know about the
 * other. `app/asp.c` still compiles with no spine in sight, and `app/spine.c`
 * has never heard of a file transfer.
 *
 * ## Resume lives here, not in the protocol
 *
 * [analysis/17](../analysis/17-application-protocol.md) §5: resume identity
 * across connections is `(peer callsign, name, size, crc32)` -- the transfer id
 * is meaningless once the link drops. The session cannot implement that, because
 * it does not outlive a connection. So the partial file does: a receive keeps
 * `<name>.part` in the receive directory, and an offer that matches an existing
 * partial is answered with what is held and the CRC of exactly those bytes.
 *
 * The session then decides whether to trust it, which is the whole point of the
 * prefix CRC -- N bytes on disk is not evidence they are *this* file's first N.
 *
 * ## Auto-accept is off
 *
 * §5 again: a station that automatically accepts arbitrary files from any caller
 * is a station that will eventually receive something its operator did not want.
 * The default is to ask, via ::asp_app_hooks::offer_arrived, and the answer comes
 * back through ::asp_app_answer.
 */

/** @brief What the application above wants to be told. All optional. */
typedef struct {
	void *ctx;

	/** @brief A chat line. @p raw when the peer is not speaking ASP. */
	void (*text_arrived)(void *ctx, const char *text, size_t len, bool raw);

	/**
	 * @brief An offer needs an answer.
	 *
	 * @p resumable is how many bytes are already held, so a prompt can say
	 * "resume from 40%" rather than only "accept". Call ::asp_app_answer.
	 */
	void (*offer_arrived)(void *ctx, const asp_offer *offer,
			      const char *safe_name, uint32_t resumable);

	/** @brief Bytes moved, for a progress bar. */
	void (*progress)(void *ctx, bool inbound, uint32_t done,
			 uint32_t total);

	/** @brief A transfer ended. @p path is the finished file, or NULL. */
	void (*transfer_done)(void *ctx, bool inbound, asp_result_code result,
			      const char *path);

	/** @brief The link state changed: ASP, raw, or gone. */
	void (*link_changed)(void *ctx, asp_link_state state,
			     const char *peer_call);

	/** @brief Something worth putting in a log. */
	void (*note)(void *ctx, const char *text);
} asp_app_hooks;

/** @brief One connection's worth of ASP over a spine. Caller-owned. */
typedef struct {
	app_spine *spine;
	asp_session session;
	asp_io io;
	asp_app_hooks hooks;

	char recv_dir[512];
	char my_call[ASP_MAX_CALL];

	/* The file being sent. Held open for the transfer's duration so a
	 * resume check does not have to reopen and reseek. */
	FILE *send_fp;
	char send_name[ASP_MAX_NAME + 1];

	/* The partial being received, and where it will land when complete. */
	FILE *recv_fp;
	char part_path[600];
	char final_path[600];

	asp_link_state last_state;
	bool auto_accept;   /**< Off by default; see the header. */
} asp_app;

/**
 * @brief Start a session on a fresh ARQ connection.
 *
 * @param recv_dir Where received files land. Created if missing. Every received
 *                 file goes here and nowhere else -- §5 requires a single fixed
 *                 directory chosen by the operator, never a path derived from
 *                 the peer.
 * @return false if @p recv_dir could not be made usable.
 */
bool asp_app_open(asp_app *a, app_spine *spine, const char *my_call,
		  const char *recv_dir, const asp_app_hooks *hooks);

/** @brief End the session. A partial receive is kept, and is resumable. */
void asp_app_close(asp_app *a);

/**
 * @brief Feed payload that arrived from the peer.
 *
 * Only `ARQ`-tagged data. §1: `ERR` and `IDF` tagged payload is never parsed as
 * protocol, because an error marker or a station ID in the middle of a file is
 * exactly the bug `apps/ardop_rx.c` had before `ardop-cat` replaced it.
 */
void asp_app_rx(asp_app *a, const char *tag, const void *data, size_t len);

/** @brief Move whatever is waiting. Call when transmit credit may have changed. */
void asp_app_service(asp_app *a);

/** @brief Send a chat line. Works in raw mode. */
bool asp_app_send_text(asp_app *a, const char *text);

/**
 * @brief Offer a local file.
 *
 * The whole file is read once to compute its CRC before the offer goes out,
 * which is the only way to fill in `OFFER`. On a slow disk that is a pause; on
 * an HF link it is nothing beside what follows.
 *
 * @return false if it cannot be read, or a transfer is already running.
 */
bool asp_app_send_file(asp_app *a, const char *path);

/**
 * @brief The largest file that can be offered.
 *
 * Two independent ceilings meet here and this is the lower of them. `OFFER`
 * carries the size as a `u32`, and ::asp_io::read_file seeks with `fseek`, whose
 * offset is a `long` -- 32 bits on Windows. So 2 GB - 1 is what both can express,
 * and a file above it is refused rather than silently wrapped: `size += (uint32_t)n`
 * over a 5 GB file produces a plausible small number, an offer nobody can satisfy,
 * and a CRC failure hours later.
 *
 * Not a real constraint on a mode that moves a few hundred bytes a second: this
 * ceiling is over two months of continuous transmission.
 */
#define ASP_APP_MAX_FILE 0x7fffffffu

/** @brief Answer a deferred offer. */
bool asp_app_answer(asp_app *a, bool accept);

/** @brief Cancel the transfer in one direction. Resumable. */
bool asp_app_cancel(asp_app *a, bool inbound);

#endif /* ARDOP_APP_ASP_APP_H_ */
