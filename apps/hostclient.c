#define _DEFAULT_SOURCE
#include "hostclient.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @file hostclient.c
 * @brief The ARDOP host-protocol client (see hostclient.h).
 */

/* Connect a TCP socket to host:port. Returns fd or -1. */
static int dial(const char *host, int port)
{
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	int e = getaddrinfo(host, portstr, &hints, &res);
	if (e != 0) {
		fprintf(stderr, "host %s:%d: %s\n", host, port, gai_strerror(e));
		return -1;
	}

	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		fprintf(stderr, "connect %s:%d: %s\n", host, port,
			strerror(errno));
	return fd;
}

int hc_open(hostclient *hc, const char *host, int port)
{
	memset(hc, 0, sizeof(*hc));
	hc->cmd_fd = dial(host, port);
	if (hc->cmd_fd < 0)
		return -1;
	hc->data_fd = dial(host, port + 1);
	if (hc->data_fd < 0) {
		close(hc->cmd_fd);
		hc->cmd_fd = -1;
		return -1;
	}
	return 0;
}

void hc_close(hostclient *hc)
{
	if (hc->cmd_fd >= 0)
		close(hc->cmd_fd);
	if (hc->data_fd >= 0)
		close(hc->data_fd);
	hc->cmd_fd = hc->data_fd = -1;
}

/* Write all @p len bytes to @p fd. Returns 0 or -1. */
static int write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len > 0) {
		ssize_t w = write(fd, p, len);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

int hc_cmd(hostclient *hc, const char *text)
{
	char line[3200];
	int n = snprintf(line, sizeof(line), "%s\r", text);
	if (n < 0 || (size_t)n >= sizeof(line))
		return -1;
	return write_all(hc->cmd_fd, line, (size_t)n);
}

/* Wait up to timeout_ms for @p fd to be readable. 1 ready, 0 timeout, -1 err. */
static int wait_readable(int fd, int timeout_ms)
{
	fd_set r;
	FD_ZERO(&r);
	FD_SET(fd, &r);
	struct timeval tv;
	struct timeval *tvp = NULL;
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}
	int s = select(fd + 1, &r, NULL, NULL, tvp);
	if (s < 0 && errno == EINTR)
		return 0;
	return s;
}

int hc_next_line(hostclient *hc, char *buf, size_t cap, int timeout_ms)
{
	for (;;) {
		/* A complete line already buffered? */
		for (size_t i = 0; i < hc->cmd_len; i++) {
			if (hc->cmd_buf[i] == '\r' || hc->cmd_buf[i] == '\n') {
				size_t take = i < cap - 1 ? i : cap - 1;
				memcpy(buf, hc->cmd_buf, take);
				buf[take] = '\0';
				/* Drop the line and its terminator. */
				size_t rest = hc->cmd_len - (i + 1);
				memmove(hc->cmd_buf, hc->cmd_buf + i + 1, rest);
				hc->cmd_len = rest;
				if (take == 0)
					continue;   /* skip empty lines. */
				return 1;
			}
		}
		int rr = wait_readable(hc->cmd_fd, timeout_ms);
		if (rr == 0)
			return 0;
		if (rr < 0)
			return -1;
		if (hc->cmd_len >= sizeof(hc->cmd_buf))
			hc->cmd_len = 0;   /* overlong line: resync. */
		ssize_t got = read(hc->cmd_fd, hc->cmd_buf + hc->cmd_len,
				   sizeof(hc->cmd_buf) - hc->cmd_len);
		if (got <= 0)
			return -1;
		hc->cmd_len += (size_t)got;
	}
}

int hc_send_data(hostclient *hc, const uint8_t *data, size_t len)
{
	uint8_t hdr[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
	if (write_all(hc->data_fd, hdr, 2) != 0)
		return -1;
	return write_all(hc->data_fd, data, len);
}

int hc_recv_data(hostclient *hc, uint8_t *out, size_t cap, char *tag,
		 int timeout_ms)
{
	for (;;) {
		/* A complete message buffered? <2 len><3 tag><payload>. */
		if (hc->data_len >= 2) {
			size_t mlen = ((size_t)hc->data_buf[0] << 8)
				      | hc->data_buf[1];
			if (hc->data_len >= 2 + mlen) {
				size_t plen = mlen >= HC_TAG_LEN
					      ? mlen - HC_TAG_LEN : 0;
				memcpy(tag, hc->data_buf + 2, HC_TAG_LEN);
				tag[HC_TAG_LEN] = '\0';
				size_t take = plen < cap ? plen : cap;
				memcpy(out, hc->data_buf + 2 + HC_TAG_LEN, take);
				size_t consumed = 2 + mlen;
				size_t rest = hc->data_len - consumed;
				memmove(hc->data_buf, hc->data_buf + consumed,
					rest);
				hc->data_len = rest;
				return (int)take;
			}
		}
		int rr = wait_readable(hc->data_fd, timeout_ms);
		if (rr == 0)
			return -2;
		if (rr < 0)
			return -1;
		if (hc->data_len >= sizeof(hc->data_buf))
			hc->data_len = 0;   /* oversized: resync. */
		ssize_t got = read(hc->data_fd, hc->data_buf + hc->data_len,
				   sizeof(hc->data_buf) - hc->data_len);
		if (got <= 0)
			return -1;
		hc->data_len += (size_t)got;
	}
}

int hc_query(hostclient *hc, const char *cmd, const char *prefix,
	     char *buf, size_t cap, int timeout_ms,
	     void (*on_other)(void *ctx, const char *line), void *ctx)
{
	if (hc_cmd(hc, cmd) != 0)
		return -1;
	size_t plen = strlen(prefix);
	for (;;) {
		int r = hc_next_line(hc, buf, cap, timeout_ms);
		if (r <= 0)
			return r;
		if (strncmp(buf, prefix, plen) == 0)
			return 1;
		if (on_other)
			on_other(ctx, buf);
	}
}
