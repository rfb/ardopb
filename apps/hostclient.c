#include "hostclient.h"

#include <stdio.h>
#include <string.h>

#include "shell/net.h"

/**
 * @file hostclient.c
 * @brief The ARDOP host-protocol client (see hostclient.h).
 */

int hc_open(hostclient *hc, const char *host, int port)
{
	memset(hc, 0, sizeof(*hc));
	hc->cmd_fd = ardop_net_connect(host, (uint16_t)port);
	if (!ardop_net_valid(hc->cmd_fd))
		return -1;
	hc->data_fd = ardop_net_connect(host, (uint16_t)(port + 1));
	if (!ardop_net_valid(hc->data_fd)) {
		ardop_net_close(&hc->cmd_fd);
		return -1;
	}
	return 0;
}

void hc_close(hostclient *hc)
{
	ardop_net_close(&hc->cmd_fd);
	ardop_net_close(&hc->data_fd);
}

/* Write all @p len bytes. Returns 0 or -1.
 *
 * The loop is the point: these sockets are blocking, but send() may still take
 * less than offered, and a partial write on the length-prefixed data channel
 * desynchronises it for good. */
static int write_all(ardop_socket s, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len > 0) {
		size_t moved = 0;
		ardop_net_status st = ardop_net_send(s, p, len, &moved);
		if (st == ARDOP_NET_AGAIN)
			continue;
		if (st != ARDOP_NET_OK)
			return -1;
		p += moved;
		len -= moved;
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

/* Wait up to timeout_ms for @p s to be readable. 1 ready, 0 timeout, -1 err. */
static int wait_readable(ardop_socket s, int timeout_ms)
{
	bool ready = false;
	return ardop_net_wait(&s, 1, timeout_ms, &ready);
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
		size_t got = 0;
		if (ardop_net_recv(hc->cmd_fd, hc->cmd_buf + hc->cmd_len,
				   sizeof(hc->cmd_buf) - hc->cmd_len, &got)
		    != ARDOP_NET_OK)
			return -1;
		hc->cmd_len += got;
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
		size_t got = 0;
		if (ardop_net_recv(hc->data_fd, hc->data_buf + hc->data_len,
				   sizeof(hc->data_buf) - hc->data_len, &got)
		    != ARDOP_NET_OK)
			return -1;
		hc->data_len += got;
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
