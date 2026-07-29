#include "shell/host_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "shell/host.h"

/**
 * @file host_tcp.c
 * @brief The TCP command-channel transport (see host_tcp.h).
 *
 * Talks to sockets, so it is exercised against a host, not the in-process suite
 * (which tests ardop_host_command directly). Held to the core -Werror bar.
 */

#define LINE_MAX 3200

struct ardop_host_tcp {
	int listen_fd;
	int client_fd;
	char line[LINE_MAX];
	size_t line_len;
};

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

ardop_host_tcp *ardop_host_tcp_open(uint16_t port)
{
	struct ardop_host_tcp *h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;
	h->client_fd = -1;

	h->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (h->listen_fd < 0) {
		perror("host: socket");
		free(h);
		return NULL;
	}
	int one = 1;
	(void)setsockopt(h->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
			 sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(h->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
	    || listen(h->listen_fd, 1) < 0) {
		perror("host: bind/listen");
		close(h->listen_fd);
		free(h);
		return NULL;
	}
	set_nonblock(h->listen_fd);
	fprintf(stderr, "host: listening on port %u\n", (unsigned)port);
	return h;
}

/* Accept a client if none is connected and one is waiting. */
static void accept_client(ardop_host_tcp *h)
{
	if (h->client_fd >= 0)
		return;
	int fd = accept(h->listen_fd, NULL, NULL);
	if (fd < 0)
		return;   /* EWOULDBLOCK / none pending. */
	set_nonblock(fd);
	h->client_fd = fd;
	h->line_len = 0;
	fprintf(stderr, "host: client connected\n");
}

static void drop_client(ardop_host_tcp *h)
{
	if (h->client_fd >= 0) {
		close(h->client_fd);
		h->client_fd = -1;
		fprintf(stderr, "host: client disconnected\n");
	}
}

/* Run one complete command line: strip terminator, process, reply. */
static void process_line(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	char reply[1024];
	ardop_host_command(rt, h->line, now, reply, sizeof(reply));
	if (reply[0] != '\0') {
		char out[1088];
		int m = snprintf(out, sizeof(out), "%s\r", reply);
		if (m > 0)
			(void)!write(h->client_fd, out, (size_t)m);
	}
}

void ardop_host_tcp_service(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	accept_client(h);
	if (h->client_fd < 0)
		return;

	char buf[1024];
	ssize_t got = read(h->client_fd, buf, sizeof(buf));
	if (got == 0) {
		drop_client(h);
		return;
	}
	if (got < 0)
		return;   /* EWOULDBLOCK: nothing to read now. */

	for (ssize_t i = 0; i < got; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n') {
			if (h->line_len == 0)
				continue;   /* skip empty lines / CRLF pair. */
			h->line[h->line_len] = '\0';
			process_line(h, rt, now);
			h->line_len = 0;
		} else if (h->line_len < sizeof(h->line) - 1) {
			h->line[h->line_len++] = c;
		}   /* overlong line: drop excess until a terminator. */
	}
}

void ardop_host_tcp_notify(ardop_host_tcp *h, const char *msg)
{
	if (!h || h->client_fd < 0)
		return;
	char out[1088];
	int m = snprintf(out, sizeof(out), "%s\r", msg);
	if (m > 0)
		(void)!write(h->client_fd, out, (size_t)m);
}

void ardop_host_tcp_close(ardop_host_tcp *h)
{
	if (!h)
		return;
	drop_client(h);
	if (h->listen_fd >= 0)
		close(h->listen_fd);
	free(h);
}
