#ifndef ARDOP_SHELL_TELEMETRY_TCP_H_
#define ARDOP_SHELL_TELEMETRY_TCP_H_

#include <stdint.h>

#include "shell/runtime.h"

/**
 * @file telemetry_tcp.h
 * @brief The telemetry transport: one outbound stream of ::ardop_telemetry.
 *
 * A display connects, receives the stream hello, and then reads records until
 * it disconnects. The channel is strictly one-way -- a display observes and
 * never commands, so nothing read from the socket can reach the modem, and
 * attaching one cannot change what goes on the air.
 *
 * @par Backpressure.
 * The sink is called from the audio path, so it must never block. Records are
 * buffered and written non-blocking; when a slow or stopped consumer fills the
 * buffer, the *oldest* whole records are dropped to make room. A missing
 * waterfall row is invisible; a stalled capture loses a frame. That trade is
 * the reason this exists as a separate transport rather than reusing the host
 * channel, whose delivery a mail client depends on.
 */

typedef struct ardop_telemetry_tcp ardop_telemetry_tcp;

/**
 * @brief Open the telemetry listener on @p port.
 *
 * @return The server, or NULL if the socket could not be opened.
 */
ardop_telemetry_tcp *ardop_telemetry_tcp_open(uint16_t port);

/**
 * @brief Register this server as @p rt's telemetry sink.
 *
 * Call once after opening. Records produced by @p rt are then queued for the
 * connected display, if any.
 */
void ardop_telemetry_tcp_attach(ardop_telemetry_tcp *t, ardop_runtime *rt);

/**
 * @brief Accept a pending connection and flush queued bytes. Never blocks.
 *
 * Call once per loop iteration, alongside ardop_host_tcp_service().
 *
 * @param t   Server, or NULL (a no-op, so the caller need not branch).
 * @param rt  Runtime, so a newly connected display can be sent a status record
 *            immediately rather than waiting for the next state change.
 */
void ardop_telemetry_tcp_service(ardop_telemetry_tcp *t, ardop_runtime *rt);

/** @brief Close the listener and any client, and free the server. */
void ardop_telemetry_tcp_close(ardop_telemetry_tcp *t);

#endif /* ARDOP_SHELL_TELEMETRY_TCP_H_ */
