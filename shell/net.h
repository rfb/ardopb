#ifndef ARDOP_SHELL_NET_H_
#define ARDOP_SHELL_NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file net.h
 * @brief The portable TCP surface the host and telemetry transports sit on.
 *
 * **This header deliberately includes no operating-system header.** The handle
 * is typed from `<stdint.h>` alone, so `host_tcp.c` and `telemetry_tcp.c` never
 * transitively pull in `<windows.h>` and its macro storm. Everything
 * platform-specific lives in `net.c`.
 *
 * ## What this exists to prevent
 *
 * The port is not "`int` becomes `SOCKET`". Three things are silently wrong on
 * Windows and none of them fail to compile:
 *
 *  - `SOCKET` is *unsigned*, so `fd >= 0` is always true and `fd < 0` never is.
 *    Rather than fix each site, the type is opaque and comparison is replaced
 *    by ::ardop_net_valid, so the wrong idiom cannot be written again.
 *  - MinGW maps `read`/`write`/`close` onto C-runtime file descriptors, not
 *    Winsock handles. They compile, link, and fail at runtime with `EBADF`.
 *    ::ardop_net_recv and ::ardop_net_send are the only way to move bytes.
 *  - A partial `send` on a non-blocking socket is normal and was being
 *    discarded, which truncates a record mid-frame. The @p moved out-parameter
 *    makes the short count impossible to ignore without naming a variable to
 *    ignore it with.
 *
 * `errno` versus `WSAGetLastError()` never reaches a caller: ::ardop_net_status
 * carries the only distinctions the callers actually branch on.
 *
 * Winsock initialisation is refcounted inside `net.c` and performed by
 * ::ardop_net_listen and ::ardop_net_connect. It is deliberately *not* a
 * `main()` responsibility: an init that must happen before first use, on one
 * platform only, is a latent ordering bug, and it would otherwise have to be
 * repeated in three `apps/` mains plus every future front end.
 */

#ifdef _WIN32
/** @brief A socket handle. Windows' SOCKET is UINT_PTR, hence uintptr_t. */
typedef uintptr_t ardop_socket;
/** @brief The "no socket" value. Equals Winsock's INVALID_SOCKET. */
#define ARDOP_SOCKET_INVALID ((ardop_socket) ~(uintptr_t)0)
#else
typedef int ardop_socket;
#define ARDOP_SOCKET_INVALID ((ardop_socket)-1)
#endif

/** @brief The outcome of a transfer. */
typedef enum {
	ARDOP_NET_OK = 0,   /**< Moved `*moved` bytes; may be fewer than asked. */
	ARDOP_NET_AGAIN,    /**< Nothing available now (EAGAIN/WSAEWOULDBLOCK). */
	ARDOP_NET_CLOSED,   /**< The peer closed in an orderly way. */
	ARDOP_NET_ERROR     /**< Broken; the caller should drop the client. */
} ardop_net_status;

/** @brief True if @p s refers to an open socket. */
bool ardop_net_valid(ardop_socket s);

/**
 * @brief Close @p *s if open and set it to ::ARDOP_SOCKET_INVALID.
 *
 * Takes a pointer because every call site already had the "close it and mark it
 * gone" shape, and because leaving a stale handle behind is the bug this
 * prevents. Safe on NULL and on an already-invalid handle.
 */
void ardop_net_close(ardop_socket *s);

/**
 * @brief Bind a non-blocking listener to @p port on all interfaces.
 *
 * Sets SO_REUSEADDR on POSIX only. On Windows that option means something
 * genuinely different -- it lets a *second process* bind a port already in use
 * and race for connections -- which would defeat the "one host at a time"
 * guarantee rather than support it.
 *
 * @return The listener, or ::ARDOP_SOCKET_INVALID (diagnostic already printed).
 */
ardop_socket ardop_net_listen(uint16_t port);

/** @brief Room for "255.255.255.255:65535" and for an IPv6 literal. */
#define ARDOP_NET_PEER_MAX 64

/**
 * @brief Accept a pending connection, non-blocking.
 *
 * @param listener The listening socket.
 * @param peer     If non-NULL, receives "address:port" for the client, or the
 *                 empty string if it could not be determined. An operator
 *                 looking at a list of attached clients needs to know *which*
 *                 machine is holding their transmitter, and "a client" does not
 *                 answer that.
 * @param peer_cap Bytes available at @p peer; ::ARDOP_NET_PEER_MAX is enough.
 * @return The client socket, or ::ARDOP_SOCKET_INVALID if none was waiting.
 */
ardop_socket ardop_net_accept(ardop_socket listener, char *peer,
			      size_t peer_cap);

/**
 * @brief Connect to @p host : @p port, resolving through getaddrinfo.
 * @return A blocking connected socket, or ::ARDOP_SOCKET_INVALID.
 */
ardop_socket ardop_net_connect(const char *host, uint16_t port);

/** @brief Put @p s into non-blocking mode. @return false on failure. */
bool ardop_net_set_nonblock(ardop_socket s);

/**
 * @brief Read up to @p max bytes.
 * @param moved Bytes read; set to 0 unless the result is ::ARDOP_NET_OK.
 */
ardop_net_status ardop_net_recv(ardop_socket s, void *buf, size_t max,
				size_t *moved);

/**
 * @brief Write up to @p len bytes without blocking or raising SIGPIPE.
 * @param moved Bytes written -- **check it**, a short write is normal.
 */
ardop_net_status ardop_net_send(ardop_socket s, const void *buf, size_t len,
				size_t *moved);

/**
 * @brief Wait until one of @p n sockets is readable.
 *
 * @param ready  Caller array of @p n flags, filled in on return.
 * @param timeout_ms Negative to wait indefinitely.
 * @return Number of ready sockets, 0 on timeout, -1 on error.
 */
int ardop_net_wait(const ardop_socket *set, size_t n, int timeout_ms,
		   bool *ready);

/** @brief Human-readable text for the last socket error on this thread. */
const char *ardop_net_last_error(void);

#endif /* ARDOP_SHELL_NET_H_ */
