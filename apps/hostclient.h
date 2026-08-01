#ifndef ARDOP_APPS_HOSTCLIENT_H_
#define ARDOP_APPS_HOSTCLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file hostclient.h
 * @brief A small client for the ARDOP TCP host protocol.
 *
 * Connects to a running modem's host interface (ardopb --host P, or ardopcf):
 * a command channel on port P (CR-terminated text -- replies and asynchronous
 * notifications) and a data channel on port P+1 (length-prefixed binary). The
 * framing mirrors shell/host.c, which is the authority; it is reimplemented here
 * so the apps are plain socket clients with no dependency on the modem's C API.
 *
 * All calls are blocking with an explicit millisecond timeout, so a caller can
 * poll or drive its own event loop off ::hostclient::cmd_fd /
 * ::hostclient::data_fd -- pass those to ::ardop_net_wait rather than to
 * select() directly, so the apps stay portable.
 */

#include "shell/net.h"

/** Tag length on received data messages ("ARQ"/"FEC"/"ERR"/"IDF"). */
#define HC_TAG_LEN 3

typedef struct {
	ardop_socket cmd_fd;        /**< command channel socket. */
	ardop_socket data_fd;       /**< data channel socket (cmd port + 1). */
	char cmd_buf[4096];         /**< partial command-line input. */
	size_t cmd_len;
	uint8_t data_buf[16384];    /**< partial data-message input. */
	size_t data_len;
} hostclient;

/**
 * @brief Connect the command port @p port and the data port @p port + 1.
 * @return 0 on success, -1 on failure (a message is printed to stderr).
 */
int hc_open(hostclient *hc, const char *host, int port);

/** @brief Close both sockets. */
void hc_close(hostclient *hc);

/**
 * @brief Send a command line: writes "TEXT\r" to the command channel.
 * @return 0 on success, -1 on write error.
 */
int hc_cmd(hostclient *hc, const char *text);

/**
 * @brief Read one CR/LF-delimited line from the command channel.
 *
 * @param buf         Output, NUL-terminated (terminator stripped).
 * @param cap         Size of @p buf.
 * @param timeout_ms  Max wait; <0 blocks indefinitely.
 * @return 1 on a line, 0 on timeout, -1 on error or the peer closing.
 */
int hc_next_line(hostclient *hc, char *buf, size_t cap, int timeout_ms);

/**
 * @brief Send @p len payload bytes on the data channel (host -> modem framing).
 * @return 0 on success, -1 on error.
 */
int hc_send_data(hostclient *hc, const uint8_t *data, size_t len);

/**
 * @brief Receive one data message (modem -> host framing: len + 3-char tag).
 *
 * @param out         Output payload buffer.
 * @param cap         Size of @p out.
 * @param tag         Output, HC_TAG_LEN+1 bytes; NUL-terminated tag ("ARQ"...).
 * @param timeout_ms  Max wait; <0 blocks indefinitely.
 * @return payload length (>=0) on a message, -2 on timeout, -1 on error/close.
 */
int hc_recv_data(hostclient *hc, uint8_t *out, size_t cap, char *tag,
		 int timeout_ms);

/**
 * @brief Send a command, then read lines until one starts with @p prefix.
 *
 * Convenience for a query/reply on the command channel while asynchronous
 * notifications may interleave. Lines seen before the match are passed to
 * @p on_other (may be NULL) so the caller can react to CONNECTED/DISCONNECTED.
 *
 * @return 1 and copies the matching line into @p buf; 0 on timeout; -1 on error.
 */
int hc_query(hostclient *hc, const char *cmd, const char *prefix,
	     char *buf, size_t cap, int timeout_ms,
	     void (*on_other)(void *ctx, const char *line), void *ctx);

#endif /* ARDOP_APPS_HOSTCLIENT_H_ */
