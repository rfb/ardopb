#include "app/tnc_host_tcp.h"

/**
 * @file tnc_host_tcp.c
 * @brief ardop_host_tcp behind an app_tnc_ops (see tnc_host_tcp.h).
 *
 * Four forwarding functions and nothing else. If this file ever grows policy,
 * the policy belongs in the spine, where a user interface can see it.
 */

static void tnc_service(void *ctx, ardop_runtime *rt, uint64_t now)
{
	ardop_host_tcp_service(ctx, rt, now);
}

static bool tnc_attached(void *ctx)
{
	return ardop_host_tcp_client_connected(ctx);
}

static void tnc_notify(void *ctx, const char *msg)
{
	ardop_host_tcp_notify(ctx, msg);
}

static void tnc_send_data(void *ctx, const char *tag, const uint8_t *data,
			  size_t len)
{
	ardop_host_tcp_send_data(ctx, tag, data, len);
}

void app_tnc_host_tcp_bind(app_tnc_ops *ops, ardop_host_tcp *h)
{
	ops->ctx = h;
	ops->service = tnc_service;
	ops->attached = tnc_attached;
	ops->notify = tnc_notify;
	ops->send_data = tnc_send_data;
}
