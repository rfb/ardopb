#include "app/tnc_host_tcp.h"

#include <stdio.h>

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

/*
 * Translate one transport event into one spine event.
 *
 * The mapping is the whole of this function, deliberately: deciding *what an
 * operator should be told* is policy, and policy belongs above the seam. All
 * that happens here is that a host_tcp vocabulary becomes a spine vocabulary.
 */
static void watch_observer(void *ctx, ardop_host_ev_kind kind,
			   const char *channel, const char *detail,
			   const char *reply)
{
	app_tnc_watch *w = ctx;
	char text[APP_TEXT_MAX];

	switch (kind) {
	case ARDOP_HOST_EV_LISTENING:
		snprintf(text, sizeof text, "TNC interface listening on %s",
			 detail);
		app_report_guest(w->spine, APP_GUEST_LISTENING, text);
		return;
	case ARDOP_HOST_EV_CONNECTED:
		snprintf(text, sizeof text, "%s client connected from %s",
			 channel, detail[0] ? detail : "an unknown address");
		app_report_guest(w->spine, APP_GUEST_CONNECTED, text);
		return;
	case ARDOP_HOST_EV_DISCONNECTED:
		snprintf(text, sizeof text, "%s client disconnected%s%s",
			 channel, detail[0] ? " -- " : "", detail);
		app_report_guest(w->spine, APP_GUEST_DISCONNECTED, text);
		return;
	case ARDOP_HOST_EV_REFUSED:
		snprintf(text, sizeof text,
			 "refused a second %s client from %s -- one host at a "
			 "time", channel,
			 detail[0] ? detail : "an unknown address");
		app_report_guest(w->spine, APP_GUEST_REFUSED, text);
		return;
	case ARDOP_HOST_EV_COMMAND:
		/* The command and its answer in one line, because they are read
		 * together and a screen that separated them would need the
		 * operator to pair them up again. */
		if (reply && reply[0])
			snprintf(text, sizeof text, "%s -> %s", detail, reply);
		else
			snprintf(text, sizeof text, "%s", detail);
		app_report_guest(w->spine, APP_GUEST_COMMAND, text);
		return;
	}
}

void app_tnc_host_tcp_watch(app_tnc_watch *w, ardop_host_tcp *h, app_spine *sp)
{
	w->spine = sp;
	w->host = h;
	ardop_host_tcp_observe(h, watch_observer, w);
}

void app_tnc_host_tcp_bind(app_tnc_ops *ops, ardop_host_tcp *h)
{
	ops->ctx = h;
	ops->service = tnc_service;
	ops->attached = tnc_attached;
	ops->notify = tnc_notify;
	ops->send_data = tnc_send_data;
}
