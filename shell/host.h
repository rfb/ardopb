#ifndef ARDOP_SHELL_HOST_H_
#define ARDOP_SHELL_HOST_H_

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

/** Reported product name in the VERSION reply. Kept as the interop identity the
 *  ecosystem recognises, independent of this build's own name. */
#define ARDOP_HOST_PRODUCT "ardopcf"

/** Reported version in the VERSION reply. The trailing marker flags the rebuilt
 *  core so an operator can tell the two apart without changing the product name
 *  a host app matches on. */
#define ARDOP_HOST_VERSION "1.0.4.1.3-b"

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

#endif /* ARDOP_SHELL_HOST_H_ */
