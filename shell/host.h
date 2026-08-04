#ifndef ARDOP_SHELL_HOST_H_
#define ARDOP_SHELL_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/runtime.h"

/**
 * @file host.h
 * @brief The TCP host command protocol, as a step over the runtime.
 *
 * This is [analysis/13](../analysis/13-completing-the-rebuild.md) W2.3: the
 * ~1300-line strcmp ladder in `HostInterface.c` re-expressed as "parse a host
 * command -> apply it to the runtime (a config change, or an ardop_link_input) ->
 * format the reply." The wire format is frozen -- Pat and WoAD depend on it -- so
 * the reply strings here match the inherited TNC byte-for-byte; only the plumbing
 * (globals -> the caller-owned runtime) changes.
 *
 * It is pure with respect to I/O: it reads and mutates the runtime's
 * configuration and drives ardop_runtime_host(), but opens no socket and does no
 * framing. The transport (the two TCP ports, the CR terminator, the data
 * channel's length prefix) is the caller's job; ::ardop_host_command takes one
 * already-deframed command line and hands back one reply line to send. That
 * split is what makes the whole command surface testable without a socket.
 */

/**
 * Reported product name in the `VERSION` reply.
 *
 * The reply is `VERSION <product>_<build>`, for example
 * `VERSION ardopb_0f35a4d`.
 *
 * ## Why this is no longer `ardopcf_1.0.4.1.3-b`
 *
 * The inherited value reported another program's name and another program's
 * release number. [analysis/07](../analysis/07-migration-path.md) kept it
 * deliberately, to protect host programs that might match on the name.
 *
 * That protection was never tested and is not needed. Host programs already
 * meet several ARDOP implementations -- `ARDOP_Win`, `ardopc`, `ardopcf` and
 * this one -- and those report different names and different numbers. A client
 * that could only accept one string would already fail against most of the
 * ecosystem.
 *
 * What the old value cost was real: every fault report, every log line and every
 * host program's records named a program that did not produce them.
 *
 * ## The version is the build
 *
 * There is no second number to maintain and no number to forget to increase.
 * ::ardop_build_id supplies it, so the string a host program records is the
 * exact string that finds the source. See `build.h`.
 */
#define ARDOP_HOST_PRODUCT "ardopb"

/**
 * @brief Process one host command line, applying it to @p rt.
 *
 * @param rt     The runtime to configure / drive. Config commands mutate its
 *               link fields; action commands are fed through
 *               ardop_runtime_host().
 * @param line   One command line, CR/LF already stripped, NUL-terminated.
 * @param now    Current time (elapsed samples) for any link input generated.
 * @param reply  Buffer for the reply line to send back (no terminator added).
 *               Set to "" when the command warrants no reply.
 * @param cap    Size of @p reply.
 */
void ardop_host_command(ardop_runtime *rt, const char *line, uint64_t now,
			char *reply, size_t cap);

/* --- data channel framing (the second, binary TCP port) ------------------ */

/** Length of the type tag prepended to received data ("ARQ"/"FEC"/...). */
#define ARDOP_HOST_TAG_LEN 3

/**
 * @brief Frame received data for the host: `<2-byte BE (3+len)><tag><data>`.
 *
 * The TNC->host direction of the data channel, matching
 * TCPAddTagToDataAndSendToHost. The 2-byte big-endian length counts the 3 tag
 * bytes plus the payload (not the length bytes themselves).
 *
 * @param out   Output buffer.
 * @param cap   Capacity of @p out.
 * @param tag   Exactly ::ARDOP_HOST_TAG_LEN characters ("ARQ", "FEC", "ERR", ...).
 * @param data  Payload bytes.
 * @param len   Payload length.
 * @return Total bytes written (2 + 3 + @p len), or 0 if it would not fit.
 */
size_t ardop_host_data_frame(uint8_t *out, size_t cap, const char *tag,
			     const uint8_t *data, size_t len);

/**
 * @brief Parse one host->TNC data message from the front of a byte stream.
 *
 * The host->TNC direction: `<2-byte BE len><len payload bytes>`. On success the
 * payload is located within @p buf (no copy) and @p consumed gives the whole
 * message size so the caller can advance past it.
 *
 * @param buf          Received bytes.
 * @param avail        Number valid in @p buf.
 * @param[out] payload      Set to the payload start within @p buf.
 * @param[out] payload_len  Set to the payload length.
 * @param[out] consumed     Set to bytes to drop from the front (2 + payload_len).
 * @return true if a complete message is present; false if more bytes are needed.
 */
bool ardop_host_data_parse(const uint8_t *buf, size_t avail,
			   const uint8_t **payload, size_t *payload_len,
			   size_t *consumed);

/**
 * @brief The wire name for an ARQ bandwidth setting, or NULL.
 *
 * Exposed so the interface can offer the choices without writing them down a
 * second time. [analysis/16](../analysis/16-user-interface.md)'s exit criterion
 * is that no protocol name appears in the UI; the table these come from is the
 * one ported verbatim from ARQ.c's ARQBandwidths, and it stays the only copy.
 */
const char *ardop_host_bandwidth_name(ardop_arq_bandwidth bw);

/** @brief The inverse. @return false if @p name is not a bandwidth. */
ARDOP_MUSTUSE bool ardop_host_bandwidth_for_name(const char *name,
						 ardop_arq_bandwidth *out);

/**
 * @brief The host-facing name for a link state ("DISC", "ISS", "IRS", ...).
 *
 * The collapsed set the protocol reports, not the full internal enum: several
 * machine states answer to one host-facing name, which is what a host client and
 * an operator both want to see.
 */
const char *ardop_host_state_name(ardop_link_state state);

/** @brief The wire name for a protocol mode ("ARQ", "FEC", "RXO"), or NULL. */
const char *ardop_host_mode_name(ardop_link_mode mode);

/** @brief The inverse. @return false if @p name is not a mode. */
ARDOP_MUSTUSE bool ardop_host_mode_for_name(const char *name,
					    ardop_link_mode *out);

#endif /* ARDOP_SHELL_HOST_H_ */
