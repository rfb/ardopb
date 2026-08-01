#include "shell/net.h"

#include <stdio.h>
#include <string.h>

/**
 * @file net.c
 * @brief The portable TCP surface (see net.h).
 *
 * The only file in the tree that includes a socket header. Everything above it
 * sees ::ardop_socket and ::ardop_net_status and nothing else.
 */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define NET_ERRNO         WSAGetLastError()
#  define NET_EWOULDBLOCK   WSAEWOULDBLOCK
#  define NET_EINTR         WSAEINTR
typedef int net_socklen;
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define NET_ERRNO         errno
#  define NET_EWOULDBLOCK   EWOULDBLOCK
#  define NET_EINTR         EINTR
typedef socklen_t net_socklen;
#endif

/* Winsock needs a per-process init. Refcounted here rather than done in main()
 * so that it cannot be forgotten by a new front end: the two calls that can
 * create a socket are the two that ensure it. */
#ifdef _WIN32
static int net_refs;

static bool net_startup(void)
{
	if (net_refs++ > 0)
		return true;
	WSADATA wsa;
	int e = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (e != 0) {
		net_refs = 0;
		fprintf(stderr, "net: WSAStartup failed (%d)\n", e);
		return false;
	}
	return true;
}

static void net_cleanup(void)
{
	if (net_refs > 0 && --net_refs == 0)
		WSACleanup();
}
#else
static bool net_startup(void) { return true; }
static void net_cleanup(void) { }
#endif

bool ardop_net_valid(ardop_socket s)
{
	return s != ARDOP_SOCKET_INVALID;
}

void ardop_net_close(ardop_socket *s)
{
	if (!s || !ardop_net_valid(*s))
		return;
#ifdef _WIN32
	closesocket(*s);
#else
	close(*s);
#endif
	*s = ARDOP_SOCKET_INVALID;
	net_cleanup();
}

bool ardop_net_set_nonblock(ardop_socket s)
{
#ifdef _WIN32
	u_long on = 1;
	return ioctlsocket(s, (long)FIONBIO, &on) == 0;
#else
	int fl = fcntl(s, F_GETFL, 0);
	if (fl < 0)
		return false;
	return fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

const char *ardop_net_last_error(void)
{
#ifdef _WIN32
	static char buf[256];
	DWORD e = (DWORD)WSAGetLastError();
	DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM
				 | FORMAT_MESSAGE_IGNORE_INSERTS,
				 NULL, e, 0, buf, sizeof(buf) - 1, NULL);
	if (n == 0) {
		snprintf(buf, sizeof(buf), "winsock error %lu", (unsigned long)e);
		return buf;
	}
	/* FormatMessage leaves a trailing CRLF and a period. */
	while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'
			 || buf[n - 1] == '.' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return buf;
#else
	return strerror(errno);
#endif
}

ardop_socket ardop_net_listen(uint16_t port)
{
	if (!net_startup())
		return ARDOP_SOCKET_INVALID;

	ardop_socket fd = (ardop_socket)socket(AF_INET, SOCK_STREAM, 0);
	if (!ardop_net_valid(fd)) {
		fprintf(stderr, "net: socket: %s\n", ardop_net_last_error());
		net_cleanup();
		return ARDOP_SOCKET_INVALID;
	}

#ifndef _WIN32
	/* POSIX only, and the asymmetry is not an oversight. On POSIX this lets
	 * a restarted daemon rebind a port still in TIME_WAIT. On Windows the
	 * same name permits a *different process* to bind a port already in
	 * use and steal connections from it -- the opposite of what is wanted
	 * here, where exactly one host may hold the channel. */
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&one,
			 (net_socklen)sizeof(one));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(fd, (const struct sockaddr *)&addr,
		 (net_socklen)sizeof(addr)) != 0
	    || listen(fd, 1) != 0) {
		fprintf(stderr, "net: bind/listen port %u: %s\n",
			(unsigned)port, ardop_net_last_error());
		ardop_net_close(&fd);
		return ARDOP_SOCKET_INVALID;
	}

	if (!ardop_net_set_nonblock(fd)) {
		fprintf(stderr, "net: set nonblocking: %s\n",
			ardop_net_last_error());
		ardop_net_close(&fd);
		return ARDOP_SOCKET_INVALID;
	}
	return fd;
}

ardop_socket ardop_net_accept(ardop_socket listener)
{
	ardop_socket fd = (ardop_socket)accept(listener, NULL, NULL);
	if (!ardop_net_valid(fd))
		return ARDOP_SOCKET_INVALID;
	(void)ardop_net_set_nonblock(fd);
#ifdef _WIN32
	/* Balance the refcount: this handle will be passed to
	 * ardop_net_close(), which decrements. */
	net_refs++;
#endif
	return fd;
}

ardop_socket ardop_net_connect(const char *host, uint16_t port)
{
	if (!net_startup())
		return ARDOP_SOCKET_INVALID;

	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	int e = getaddrinfo(host, portstr, &hints, &res);
	if (e != 0) {
		fprintf(stderr, "net: %s:%u: %s\n", host, (unsigned)port,
			gai_strerror(e));
		net_cleanup();
		return ARDOP_SOCKET_INVALID;
	}

	ardop_socket fd = ARDOP_SOCKET_INVALID;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = (ardop_socket)socket(ai->ai_family, ai->ai_socktype,
					  ai->ai_protocol);
		if (!ardop_net_valid(fd))
			continue;
		if (connect(fd, ai->ai_addr, (net_socklen)ai->ai_addrlen) == 0)
			break;
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
		fd = ARDOP_SOCKET_INVALID;
	}
	freeaddrinfo(res);

	if (!ardop_net_valid(fd)) {
		fprintf(stderr, "net: connect %s:%u: %s\n", host,
			(unsigned)port, ardop_net_last_error());
		net_cleanup();
	}
	return fd;
}

ardop_net_status ardop_net_recv(ardop_socket s, void *buf, size_t max,
				size_t *moved)
{
	*moved = 0;
	if (!ardop_net_valid(s))
		return ARDOP_NET_ERROR;

#ifdef _WIN32
	int n = recv(s, (char *)buf, (int)max, 0);
#else
	ssize_t n = recv(s, buf, max, 0);
#endif
	if (n > 0) {
		*moved = (size_t)n;
		return ARDOP_NET_OK;
	}
	if (n == 0)
		return ARDOP_NET_CLOSED;

	int e = NET_ERRNO;
	if (e == NET_EWOULDBLOCK || e == NET_EINTR)
		return ARDOP_NET_AGAIN;
#if !defined(_WIN32) && defined(EAGAIN)
	if (e == EAGAIN)
		return ARDOP_NET_AGAIN;
#endif
	return ARDOP_NET_ERROR;
}

ardop_net_status ardop_net_send(ardop_socket s, const void *buf, size_t len,
				size_t *moved)
{
	*moved = 0;
	if (!ardop_net_valid(s))
		return ARDOP_NET_ERROR;
	if (len == 0)
		return ARDOP_NET_OK;

	/* MSG_NOSIGNAL keeps a write to a departed peer from killing the
	 * process on Linux. Windows has no SIGPIPE and macOS uses the
	 * SO_NOSIGPIPE socket option instead, so the flag simply is not there. */
	int flags = 0;
#ifdef MSG_NOSIGNAL
	flags |= MSG_NOSIGNAL;
#endif

#ifdef _WIN32
	int n = send(s, (const char *)buf, (int)len, flags);
#else
	ssize_t n = send(s, buf, len, flags);
#endif
	if (n > 0) {
		*moved = (size_t)n;
		return ARDOP_NET_OK;
	}
	if (n == 0)
		return ARDOP_NET_OK;   /* nothing taken; not an error. */

	int e = NET_ERRNO;
	if (e == NET_EWOULDBLOCK || e == NET_EINTR)
		return ARDOP_NET_AGAIN;
#if !defined(_WIN32) && defined(EAGAIN)
	if (e == EAGAIN)
		return ARDOP_NET_AGAIN;
#endif
	return ARDOP_NET_ERROR;
}

int ardop_net_wait(const ardop_socket *set, size_t n, int timeout_ms,
		   bool *ready)
{
	fd_set r;
	FD_ZERO(&r);

	int maxfd = -1;
	int watched = 0;
	for (size_t i = 0; i < n; i++) {
		ready[i] = false;
		if (!ardop_net_valid(set[i]))
			continue;
		FD_SET(set[i], &r);
		watched++;
#ifndef _WIN32
		if (set[i] > maxfd)
			maxfd = set[i];
#endif
	}
	if (watched == 0)
		return 0;

	struct timeval tv;
	struct timeval *tvp = NULL;
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}

	/* Winsock ignores the first argument entirely; POSIX needs max fd + 1. */
	int s = select(maxfd + 1, &r, NULL, NULL, tvp);
	if (s < 0)
		return (NET_ERRNO == NET_EINTR) ? 0 : -1;

	for (size_t i = 0; i < n; i++)
		if (ardop_net_valid(set[i]) && FD_ISSET(set[i], &r))
			ready[i] = true;
	return s;
}
