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
 * @brief The TCP command + data channel transport (see host_tcp.h).
 *
 * Talks to sockets, so it is exercised against a host, not the in-process suite
 * (which tests ardop_host_command and the framing helpers directly). Held to the
 * core -Werror bar.
 */

#define LINE_MAX 3200
#define DATA_BUF_MAX 8192

struct ardop_host_tcp {
	/* Command channel: CR/LF-delimited text on `port`. */
	int cmd_listen_fd;
	int cmd_fd;
	char line[LINE_MAX];
	size_t line_len;

	/* Data channel: length-prefixed binary on `port` + 1. */
	int data_listen_fd;
	int data_fd;
	uint8_t data_buf[DATA_BUF_MAX];
	size_t data_buf_len;
};

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Open a non-blocking listening socket on port, or -1. */
static int open_listener(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("host: socket");
		return -1;
	}
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
	    || listen(fd, 1) < 0) {
		perror("host: bind/listen");
		close(fd);
		return -1;
	}
	set_nonblock(fd);
	return fd;
}

ardop_host_tcp *ardop_host_tcp_open(uint16_t port)
{
	struct ardop_host_tcp *h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;
	h->cmd_fd = -1;
	h->data_fd = -1;

	h->cmd_listen_fd = open_listener(port);
	h->data_listen_fd = open_listener((uint16_t)(port + 1));
	if (h->cmd_listen_fd < 0 || h->data_listen_fd < 0) {
		ardop_host_tcp_close(h);
		return NULL;
	}
	fprintf(stderr, "host: listening on ports %u (cmd) and %u (data)\n",
		(unsigned)port, (unsigned)(port + 1));
	return h;
}

/* Accept a client on a listener if the slot is free. */
static void accept_into(int listen_fd, int *client_fd, const char *what)
{
	if (*client_fd >= 0)
		return;
	int fd = accept(listen_fd, NULL, NULL);
	if (fd < 0)
		return;   /* none pending. */
	set_nonblock(fd);
	*client_fd = fd;
	fprintf(stderr, "host: %s client connected\n", what);
}

static void drop(int *client_fd, const char *what)
{
	if (*client_fd >= 0) {
		close(*client_fd);
		*client_fd = -1;
		fprintf(stderr, "host: %s client disconnected\n", what);
	}
}

/* Run one complete command line: process and reply. */
static void process_line(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	char reply[1024];
	ardop_host_command(rt, h->line, now, reply, sizeof(reply));
	if (reply[0] != '\0') {
		char out[1088];
		int m = snprintf(out, sizeof(out), "%s\r", reply);
		if (m > 0)
			(void)!write(h->cmd_fd, out, (size_t)m);
	}
}

/* Read and process the command channel. */
static void service_cmd(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	accept_into(h->cmd_listen_fd, &h->cmd_fd, "cmd");
	if (h->cmd_fd < 0)
		return;

	char buf[1024];
	ssize_t got = read(h->cmd_fd, buf, sizeof(buf));
	if (got == 0) {
		drop(&h->cmd_fd, "cmd");
		h->line_len = 0;
		return;
	}
	if (got < 0)
		return;

	for (ssize_t i = 0; i < got; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n') {
			if (h->line_len == 0)
				continue;
			h->line[h->line_len] = '\0';
			process_line(h, rt, now);
			h->line_len = 0;
		} else if (h->line_len < sizeof(h->line) - 1) {
			h->line[h->line_len++] = c;
		}
	}
}

/* Read the data channel and queue each complete message as SEND_DATA. */
static void service_data(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	accept_into(h->data_listen_fd, &h->data_fd, "data");
	if (h->data_fd < 0)
		return;

	if (h->data_buf_len < sizeof(h->data_buf)) {
		ssize_t got = read(h->data_fd,
				   h->data_buf + h->data_buf_len,
				   sizeof(h->data_buf) - h->data_buf_len);
		if (got == 0) {
			drop(&h->data_fd, "data");
			h->data_buf_len = 0;
			return;
		}
		if (got < 0)
			return;
		h->data_buf_len += (size_t)got;
	}

	const uint8_t *payload;
	size_t plen, consumed;
	while (ardop_host_data_parse(h->data_buf, h->data_buf_len, &payload,
				     &plen, &consumed)) {
		ardop_host_cmd sd = {.kind = ARDOP_CMD_SEND_DATA,
				     .data = payload, .data_len = plen};
		ardop_runtime_host(rt, &sd, now);
		memmove(h->data_buf, h->data_buf + consumed,
			h->data_buf_len - consumed);
		h->data_buf_len -= consumed;
	}

	/* A message larger than the buffer can never complete; resync rather
	 * than wedge. (Real ARDOP frames are well under DATA_BUF_MAX.) */
	if (h->data_buf_len == sizeof(h->data_buf))
		h->data_buf_len = 0;
}

void ardop_host_tcp_service(ardop_host_tcp *h, ardop_runtime *rt, uint64_t now)
{
	service_cmd(h, rt, now);
	service_data(h, rt, now);
}

void ardop_host_tcp_notify(ardop_host_tcp *h, const char *msg)
{
	if (!h || h->cmd_fd < 0)
		return;
	char out[1088];
	int m = snprintf(out, sizeof(out), "%s\r", msg);
	if (m > 0)
		(void)!write(h->cmd_fd, out, (size_t)m);
}

void ardop_host_tcp_send_data(ardop_host_tcp *h, const char *tag,
			      const uint8_t *data, size_t len)
{
	if (!h || h->data_fd < 0)
		return;
	uint8_t out[2 + ARDOP_HOST_TAG_LEN + 2048];
	size_t n = ardop_host_data_frame(out, sizeof(out), tag, data, len);
	if (n > 0)
		(void)!write(h->data_fd, out, n);
}

void ardop_host_tcp_close(ardop_host_tcp *h)
{
	if (!h)
		return;
	drop(&h->cmd_fd, "cmd");
	drop(&h->data_fd, "data");
	if (h->cmd_listen_fd >= 0)
		close(h->cmd_listen_fd);
	if (h->data_listen_fd >= 0)
		close(h->data_listen_fd);
	free(h);
}
