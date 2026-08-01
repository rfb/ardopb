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
 * It serves both ARDOP host ports: the command channel on @p port and the
 * binary data channel on @p port + 1 (2-byte length-prefixed frames both ways,
 * per host.h). Non-blocking and single-client per port -- enough to drive and
 * observe the assembled program, not the production multiplexer.
 */
typedef struct ardop_host_tcp ardop_host_tcp;

/**
 * @brief Open the command listener on @p port and the data listener on @p port+1.
 * @return The server, or NULL on failure (logged).
 */
ardop_host_tcp *ardop_host_tcp_open(uint16_t port);

/**
 * @brief Accept pending clients and process any complete command lines and data
 *        messages (data messages queue into the runtime as SEND_DATA).
 *
 * Non-blocking: returns promptly whether or not anything was ready. Call it once
 * per loop iteration with the current time.
 */
void ardop_host_tcp_service(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now);

/** @brief Push one asynchronous notification line to the command client (adds CR). */
void ardop_host_tcp_notify(ardop_host_tcp *h, const char *msg);

/**
 * @brief Frame received payload and send it on the data channel.
 * @param tag One of "ARQ"/"FEC"/"ERR"/"IDF" (3 chars).
 */
void ardop_host_tcp_send_data(ardop_host_tcp *h, const char *tag,
			      const uint8_t *data, size_t len);

/**
 * @brief Whether either channel currently has a client.
 *
 * The station's ownership signal. There is one link, one session and one
 * transmit queue, so a program that both embeds the modem and hosts this
 * interface has to decide which of the two may drive a session -- and a
 * connected client is the one that wins (analysis/14 Decision 4). The embedding
 * application disables its own transmission for as long as this is true.
 */
bool ardop_host_tcp_client_connected(const ardop_host_tcp *h);

/** @brief Close all clients and listening sockets and free the server. */
void ardop_host_tcp_close(ardop_host_tcp *h);

#endif /* ARDOP_SHELL_HOST_TCP_H_ */
