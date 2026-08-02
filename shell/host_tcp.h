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
/** @brief What a guest did. Rides ::ardop_host_observer_fn. */
typedef enum {
	ARDOP_HOST_EV_LISTENING = 0, /**< The server came up. @c detail: ports. */
	ARDOP_HOST_EV_CONNECTED,     /**< A client attached. @c detail: peer. */
	ARDOP_HOST_EV_DISCONNECTED,  /**< It left, or was dropped. */
	ARDOP_HOST_EV_REFUSED,       /**< A second client was turned away. */
	ARDOP_HOST_EV_COMMAND,       /**< @c detail: the line. @c reply: the answer. */
} ardop_host_ev_kind;

/**
 * @brief Told about everything a guest does.
 *
 * These used to be six `fprintf(stderr)` calls, which is a reasonable thing for
 * a daemon with a terminal and useless inside a windowed application: the events
 * an operator most needs to see -- somebody attached, somebody changed your
 * callsign -- went to a stream nobody was reading.
 *
 * [analysis/14](../analysis/14-station-application.md) Decision 4 requires it:
 * a guest's configuration commands are *applied and surfaced*, and the interface
 * shows the value that changed and which client changed it. That is only
 * possible if the transport reports them.
 *
 * Called from ::ardop_host_tcp_service, on the modem thread. **Must not block.**
 *
 * @param channel "cmd" or "data".
 * @param detail  Kind-dependent; never NULL, possibly empty.
 * @param reply   The answer, for ::ARDOP_HOST_EV_COMMAND. NULL otherwise.
 */
typedef void (*ardop_host_observer_fn)(void *ctx, ardop_host_ev_kind kind,
				       const char *channel, const char *detail,
				       const char *reply);

/**
 * @brief Watch guest activity. Pass NULL to stop.
 *
 * Set before ::ardop_host_tcp_service is first called, or the first connection
 * is missed. ::ardop_host_tcp_open reports its own listening event through the
 * observer only if one is installed by ::ardop_host_tcp_observe afterwards, so
 * that event is re-emitted on the first service call.
 */
void ardop_host_tcp_observe(ardop_host_tcp *h, ardop_host_observer_fn fn,
			    void *ctx);

/** @brief The attached command client's address, or "" if none. */
const char *ardop_host_tcp_peer(const ardop_host_tcp *h);

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
