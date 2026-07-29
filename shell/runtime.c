#include "shell/runtime.h"

#include <string.h>

#include "codec/frame.h"

/**
 * @file runtime.c
 * @brief The main-loop body: analysis/10 §7 without device I/O.
 */

/** Actions one link step can emit (one frame + a handful of host/timer/ptt). */
#define MAX_ACTIONS 8
/** Events one demod push can emit. */
#define MAX_EVENTS 8

/* Begin modulating a frame to transmit, keying PTT. */
static void start_tx(ardop_runtime *rt, uint8_t frame_type,
		     const uint8_t *encoded, size_t len, uint16_t leader_ms)
{
	ardop_mod_init(&rt->mod, 30);
	if (!ardop_mod_begin(&rt->mod, frame_type, encoded, len, leader_ms,
			     rt->tx_samples, ARDOP_MOD_MAX_SAMPLES))
		return;   /* unsupported/oversized frame: drop it. */
	rt->tx_active = true;
	if (rt->on_ptt)
		rt->on_ptt(rt->ctx, true);
}

/* Turn one link action into an effect. */
static void perform(ardop_runtime *rt, const ardop_action *a)
{
	switch (a->kind) {
	case ARDOP_ACT_SEND_FRAME:
		start_tx(rt, a->frame_type, a->data, a->data_len, a->leader_ms);
		break;
	case ARDOP_ACT_NOTIFY_HOST:
		if (rt->on_host)
			rt->on_host(rt->ctx, (const char *)a->data);
		break;
	case ARDOP_ACT_DELIVER_DATA:
		if (rt->on_data)
			rt->on_data(rt->ctx, a->data, a->data_len);
		break;
	case ARDOP_ACT_SET_TIMER:
		/* The link re-checks its own deadlines on every NONE step, so
		 * the shell need only keep calling ardop_runtime_timer; the
		 * deadline is a scheduling hint a real platform would use to
		 * pace its wakeups. Nothing to do here. */
		break;
	}
}

/* Step the link with one input and perform every resulting action. */
static void step_and_perform(ardop_runtime *rt, const ardop_link_input *in,
			     uint64_t now)
{
	ardop_action acts[MAX_ACTIONS];
	size_t na = ardop_link_step(&rt->link, in, now, acts, MAX_ACTIONS);
	for (size_t i = 0; i < na; i++)
		perform(rt, &acts[i]);
}

bool ardop_runtime_init(ardop_runtime *rt, const int *rslens, int n_rs)
{
	memset(rt, 0, sizeof(*rt));

	if (!ardop_rs_init(&rt->rs, rslens, n_rs))
		return false;

	ardop_link_init(&rt->link);
	rt->link.rs = &rt->rs;
	rt->link.tx_data = rt->tx_queue;
	rt->link.tx_cap = sizeof(rt->tx_queue);

	ardop_demod_init(&rt->demod, 100, 5);
	rt->demod.rs = &rt->rs;
	for (int b = 0; b < 256; b++)
		if (ardop_frame_spec_for((uint8_t)b))
			rt->valid_types[rt->valid_len++] = (uint8_t)b;
	rt->demod.ft_ctx.valid_types = rt->valid_types;
	rt->demod.ft_ctx.valid_len = rt->valid_len;
	rt->demod.ft_ctx.rxo = true;   /* session-independent frame-type decode. */

	return true;
}

void ardop_runtime_rx(ardop_runtime *rt, const int16_t *samples, size_t n,
		      uint64_t now_samples)
{
	ardop_event events[MAX_EVENTS];
	size_t ne = ardop_demod_push(&rt->demod, samples, n, now_samples,
				     events, MAX_EVENTS);
	for (size_t e = 0; e < ne; e++) {
		if (events[e].kind == ARDOP_EV_LEADER_DETECTED)
			continue;
		ardop_link_input in = {0};
		in.kind = ARDOP_IN_RX;
		in.as.rx = events[e];
		step_and_perform(rt, &in, now_samples);
	}
}

void ardop_runtime_timer(ardop_runtime *rt, uint64_t now_samples)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_NONE;
	step_and_perform(rt, &in, now_samples);
}

void ardop_runtime_host(ardop_runtime *rt, const ardop_host_cmd *cmd,
			uint64_t now_samples)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host = *cmd;
	step_and_perform(rt, &in, now_samples);
}

size_t ardop_runtime_pull_tx(ardop_runtime *rt, int16_t *out, size_t max)
{
	if (!rt->tx_active)
		return 0;

	size_t n = ardop_mod_pull(&rt->mod, out, max);
	if (n == 0) {
		rt->tx_active = false;
		if (rt->on_ptt)
			rt->on_ptt(rt->ctx, false);
	}
	return n;
}

bool ardop_runtime_tx_active(const ardop_runtime *rt)
{
	return rt->tx_active;
}
