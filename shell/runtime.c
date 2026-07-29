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

/* Fan one observation out to every registered observer. */
static void emit(ardop_runtime *rt, const ardop_obs *o)
{
	for (int i = 0; i < rt->n_observers; i++)
		rt->observers[i].fn(rt->observers[i].ctx, o);
}

/* Emit the changes in the link's observable state since the last check. Called
 * after every step, so a state/mode/bandwidth/buffer change surfaces exactly
 * once regardless of which input caused it. */
static void emit_state_diffs(ardop_runtime *rt)
{
	const ardop_link *l = &rt->link;

	if (l->state != rt->last_state) {
		rt->last_state = l->state;
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_STATE, .state = l->state,
				      .remote = l->remote.str});
	}
	if (l->mode != rt->last_mode) {
		rt->last_mode = l->mode;
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_MODE, .mode = l->mode});
	}
	if (l->session_bw != rt->last_bw) {
		rt->last_bw = l->session_bw;
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_BANDWIDTH,
				      .bandwidth = l->session_bw});
	}
	if (l->tx_len != rt->last_buffer) {
		rt->last_buffer = l->tx_len;
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_BUFFER,
				      .buffer_len = l->tx_len});
	}
}

/* Begin modulating a frame to transmit, keying PTT. */
static void start_tx(ardop_runtime *rt, uint8_t frame_type,
		     const uint8_t *encoded, size_t len, uint16_t leader_ms)
{
	ardop_mod_init(&rt->mod, 30);
	if (!ardop_mod_begin(&rt->mod, frame_type, encoded, len, leader_ms,
			     rt->tx_samples, ARDOP_MOD_MAX_SAMPLES))
		return;   /* unsupported/oversized frame: drop it. */
	rt->tx_active = true;
	emit(rt, &(ardop_obs){.kind = ARDOP_OBS_TX_FRAME,
			      .frame_type = frame_type});
	if (!rt->ptt_keyed) {
		rt->ptt_keyed = true;
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_PTT, .key = true});
	}
}

/* Turn one link action into an effect. */
static void perform(ardop_runtime *rt, const ardop_action *a)
{
	switch (a->kind) {
	case ARDOP_ACT_SEND_FRAME:
		start_tx(rt, a->frame_type, a->data, a->data_len, a->leader_ms);
		break;
	case ARDOP_ACT_NOTIFY_HOST:
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_HOST_MSG,
				      .text = (const char *)a->data});
		break;
	case ARDOP_ACT_DELIVER_DATA:
		emit(rt, &(ardop_obs){
			.kind = ARDOP_OBS_RX_DATA,
			.tag = rt->link.mode == ARDOP_MODE_FEC ? "FEC" : "ARQ",
			.data = a->data, .data_len = a->data_len});
		break;
	case ARDOP_ACT_SET_TIMER:
		/* The link re-checks its own deadlines on every NONE step, so
		 * the shell need only keep calling ardop_runtime_timer; the
		 * deadline is a scheduling hint a real platform would use to
		 * pace its wakeups. Nothing to do here. */
		break;
	}
}

/* Step the link with one input, perform every resulting action, then surface
 * any state change. */
static void step_and_perform(ardop_runtime *rt, const ardop_link_input *in,
			     uint64_t now)
{
	ardop_action acts[MAX_ACTIONS];
	size_t na = ardop_link_step(&rt->link, in, now, acts, MAX_ACTIONS);
	for (size_t i = 0; i < na; i++)
		perform(rt, &acts[i]);
	emit_state_diffs(rt);
}

void ardop_runtime_observe(ardop_runtime *rt, ardop_observer_fn fn, void *ctx)
{
	if (rt->n_observers >= ARDOP_MAX_OBSERVERS)
		return;
	rt->observers[rt->n_observers].fn = fn;
	rt->observers[rt->n_observers].ctx = ctx;
	rt->n_observers++;
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

	ardop_busy_window(rt->busy_window);
	ardop_busy_clear(&rt->busy, 0);
	rt->busy_det = 5;   /* the inherited default sensitivity. */

	/* The state-diff baseline matches the freshly-initialised link, so the
	 * first real change is what surfaces (not the initial values). */
	rt->last_state = rt->link.state;
	rt->last_mode = rt->link.mode;
	rt->last_bw = rt->link.session_bw;
	rt->last_buffer = rt->link.tx_len;

	return true;
}

/* Map the link's ARQ bandwidth setting to the busy detector's Hz + enum. The
 * low two bits of the setting select the width (200/500/1000/2000), FORCED and
 * MAX sharing a width. */
static void busy_bandwidth(const ardop_link *l, int *hz, ardop_bandwidth *bw)
{
	static const int kHz[4] = {200, 500, 1000, 2000};
	static const ardop_bandwidth kBw[4] = {ARDOP_BW_200, ARDOP_BW_500,
					       ARDOP_BW_1000, ARDOP_BW_2000};
	int idx = (int)l->bw_setting & 3;
	*hz = kHz[idx];
	*bw = kBw[idx];
}

/* Accumulate captured samples into 1024-sample windows and, while idle, run the
 * busy detector on each; emit ARDOP_OBS_BUSY when the state changes. Busy is a
 * DISC-state signal (the original only checks it while looking for a leader). */
static void feed_busy(ardop_runtime *rt, const int16_t *samples, size_t n,
		      uint64_t now_samples)
{
	if (rt->link.state != ARDOP_LINK_DISC)
		return;

	size_t off = 0;
	while (off < n) {
		size_t room = ARDOP_BUSY_WINDOW - rt->busy_accum_len;
		size_t take = (n - off < room) ? (n - off) : room;
		memcpy(rt->busy_accum + rt->busy_accum_len, samples + off,
		       take * sizeof(int16_t));
		rt->busy_accum_len += take;
		off += take;
		if (rt->busy_accum_len < ARDOP_BUSY_WINDOW)
			break;

		rt->busy_accum_len = 0;
		int hz;
		ardop_bandwidth bw;
		busy_bandwidth(&rt->link, &hz, &bw);
		bool busy = ardop_busy_analyze(&rt->busy, rt->busy_window,
					       rt->busy_accum, hz, bw,
					       rt->demod.tuning_range,
					       rt->busy_det,
					       (uint32_t)(now_samples / 12));
		if (busy != rt->busy_state) {
			rt->busy_state = busy;
			emit(rt, &(ardop_obs){.kind = ARDOP_OBS_BUSY,
					      .busy = busy});
		}
	}
}

void ardop_runtime_rx(ardop_runtime *rt, const int16_t *samples, size_t n,
		      uint64_t now_samples)
{
	rt->now = now_samples;
	feed_busy(rt, samples, n, now_samples);
	ardop_event events[MAX_EVENTS];
	size_t ne = ardop_demod_push(&rt->demod, samples, n, now_samples,
				     events, MAX_EVENTS);
	for (size_t e = 0; e < ne; e++) {
		const ardop_event *ev = &events[e];
		if (ev->kind == ARDOP_EV_FRAME_DECODED)
			emit(rt, &(ardop_obs){.kind = ARDOP_OBS_RX_FRAME,
					      .frame_type = ev->frame_type,
					      .quality = ev->quality,
					      .sn = ev->sn});
		else if (ev->kind == ARDOP_EV_FRAME_BAD)
			emit(rt, &(ardop_obs){.kind = ARDOP_OBS_RX_FRAME_BAD,
					      .frame_type = ev->frame_type});
		else
			continue;   /* LEADER_DETECTED: not a link input. */

		ardop_link_input in = {0};
		in.kind = ARDOP_IN_RX;
		in.as.rx = *ev;
		step_and_perform(rt, &in, now_samples);
	}
}

void ardop_runtime_timer(ardop_runtime *rt, uint64_t now_samples)
{
	rt->now = now_samples;
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_NONE;
	step_and_perform(rt, &in, now_samples);
}

void ardop_runtime_host(ardop_runtime *rt, const ardop_host_cmd *cmd,
			uint64_t now_samples)
{
	rt->now = now_samples;
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
		/* The frame's samples are exhausted. Tell the link the
		 * transmission is done: in a FEC broadcast this triggers the
		 * next frame (there is no ACK to drive it). If that starts a new
		 * transmission, keep PTT keyed and drain it; otherwise unkey. */
		rt->tx_active = false;
		ardop_link_input in = {0};
		in.kind = ARDOP_IN_TX_DONE;
		step_and_perform(rt, &in, rt->now);
		if (rt->tx_active)
			return ardop_runtime_pull_tx(rt, out, max);
		rt->ptt_keyed = false;
		emit(rt, &(ardop_obs){.kind = ARDOP_OBS_PTT, .key = false});
	}
	return n;
}

bool ardop_runtime_tx_active(const ardop_runtime *rt)
{
	return rt->tx_active;
}
