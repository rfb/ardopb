#ifndef ARDOP_SHELL_HOST_TCP_H_
#define ARDOP_SHELL_HOST_TCP_H_

#include <stdbool.h>
#include <stdint.h>

#include "shell/runtime.h"

/**
 * @file host_tcp.h
 * @brief A minimal TCP transport for the host command channel (host.h).
 *
 * The impure half of W2.3: it accepts one host connection, reads CR/LF-delimited
 * command lines, runs each through ::ardop_host_command against the runtime, and
 * writes the CR-terminated reply back. Asynchronous notifications from the core
 * (CONNECTED, DISCONNECTED, STATUS, ...) are pushed to the same socket with
 * ::ardop_host_tcp_notify, which the caller wires to the runtime's host callback.
 *
 * This is the command channel only. ARDOP's separate binary *data* channel
 * (a second port, 2-byte length-prefixed frames both ways) is deliberately not
 * here yet; received payload is surfaced via the runtime's data callback and
 * left for that follow-up. Non-blocking and single-client -- enough to drive and
 * observe the assembled program, not the production multiplexer.
 */
typedef struct ardop_host_tcp ardop_host_tcp;

/** @brief Open a listening socket on @p port. NULL on failure (logged). */
ardop_host_tcp *ardop_host_tcp_open(uint16_t port);

/**
 * @brief Accept a pending client and process any complete command lines.
 *
 * Non-blocking: returns promptly whether or not anything was ready. Call it once
 * per loop iteration with the current time.
 */
void ardop_host_tcp_service(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now);

/** @brief Push one asynchronous notification line to the client (adds CR). */
void ardop_host_tcp_notify(ardop_host_tcp *h, const char *msg);

/** @brief Close the client and listening sockets and free the server. */
void ardop_host_tcp_close(ardop_host_tcp *h);

#endif /* ARDOP_SHELL_HOST_TCP_H_ */
