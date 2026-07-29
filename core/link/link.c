#include "link/link.h"

#include <stdio.h>
#include <string.h>

#include "codec/frame.h"
#include "codec/stationid.h"
#include "link/bandwidth.h"
#include "link/frames.h"
#include "link/session.h"

/**
 * @file link.c
 * @brief The link state machine, ported from ProcessRcvdARQFrame (ARQ.c) and
 *        the host command handlers, recast as a pure step function.
 *
 * Built one state at a time. Each transition composes the already-proven link
 * leaves (session, quality, bandwidth, frames) and returns actions rather than
 * transmitting, so the whole machine is testable with scripted inputs.
 */

/** The closing ID hold after sending END: 3000 ms (ARQ.c tmrFinalID). */
#define FINAL_ID_HOLD_MS 3000

/** Auto-abort a pending IRS connection after 10 s (ARQ.c tmrIRSPendingTimeout). */
#define IRS_PENDING_TIMEOUT_MS 10000

/* Append an action if there is room; silently drop past capacity (the caller
 * sizes the array to the proven per-step maximum, so this cannot happen in
 * practice -- it just keeps the port memory-safe). */
static void emit(ardop_action *actions, size_t *n, size_t max,
		 const ardop_action *a)
{
	if (*n < max)
		actions[(*n)++] = *a;
}

/* Build an encoded frame into the link's scratch and emit a SEND_FRAME action
 * pointing at it. The bytes stay valid until the next ardop_link_step(). */
static void send_frame(ardop_link *l, ardop_action *actions, size_t *n,
		       size_t max, uint8_t frame_type, size_t frame_len)
{
	ardop_action a = {0};
	a.kind = ARDOP_ACT_SEND_FRAME;
	a.frame_type = frame_type;
	a.data = l->out_frame;
	a.data_len = frame_len;
	a.leader_ms = l->leader_ms;
	emit(actions, n, max, &a);
}

/* Emit a SET_TIMER action for an absolute deadline. */
static void set_timer(ardop_link *l, ardop_action *actions, size_t *n,
		      size_t max, uint64_t deadline)
{
	(void)l;
	ardop_action t = {0};
	t.kind = ARDOP_ACT_SET_TIMER;
	t.deadline = deadline;
	emit(actions, n, max, &t);
}

/* Build a NOTIFY_HOST text action into the link's host scratch. */
static void notify_host(ardop_link *l, ardop_action *actions, size_t *n,
			size_t max, const char *text)
{
	ardop_action a = {0};
	snprintf(l->out_host, sizeof(l->out_host), "%s", text);
	a.kind = ARDOP_ACT_NOTIFY_HOST;
	a.data = (const uint8_t *)l->out_host;
	a.data_len = strlen(l->out_host);
	emit(actions, n, max, &a);
}

/* --- DISC: not connected ------------------------------------------------- */

/*
 * A received ConReq. If it is for us and the bandwidth is compatible, answer
 * ConAck with timing and become IRS (pending first data); if incompatible,
 * answer ConRejBW and stay DISC; if not for us, tell the host to cancel any
 * pending. Ported from ProcessRcvdARQFrame's DISC/ConReq arm (rules 1.2, 1.3).
 *
 * The BusyBlock guard (ConRejBusy when the channel was busy just before the
 * leader) is not yet wired: it needs the busy detector's timestamps threaded
 * into the link, which lands when BUSY_CHANGED events are consumed.
 */
static size_t step_disc_conreq(ardop_link *l, const ardop_event *ev,
			       uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;
	ardop_stationid caller, target;
	uint8_t reply_session = 0;

	if (!l->listening)
		return 0;   /* ignore connect requests unless listening. */

	/* The ConReq payload is the caller and target station IDs, six wire
	 * bytes each. A malformed payload is dropped. */
	if (ev->data_len < 2 * (int)ARDOP_PACKED6_SIZE)
		return 0;
	if (ardop_stationid_from_bytes(ev->data, &caller) != ARDOP_STATIONID_OK)
		return 0;
	if (ardop_stationid_from_bytes(ev->data + ARDOP_PACKED6_SIZE, &target)
	    != ARDOP_STATIONID_OK)
		return 0;

	if (!ardop_call_to_me(&caller, &target, &l->mycall, l->auxcalls,
			      l->n_aux, &reply_session)) {
		notify_host(l, actions, &n, max, "CANCELPENDING");
		return n;
	}

	uint8_t reply = ardop_negotiate_bandwidth(l->bw_setting, ev->frame_type,
						  &l->session_bw);

	if (reply == ARDOP_FT_CON_REJ_BW) {
		/* Incompatible bandwidth: reject, stay disconnected. (Rule 1.3.) */
		char msg[64];
		snprintf(msg, sizeof(msg), "REJECTEDBW %s", caller.str);
		notify_host(l, actions, &n, max, msg);

		size_t len = ardop_encode_control(ARDOP_FT_CON_REJ_BW,
						  reply_session, l->out_frame);
		send_frame(l, actions, &n, max, ARDOP_FT_CON_REJ_BW, len);
		return n;
	}

	/* Accept: become IRS, pending the first data frame. (Rule 1.2.) */
	char msg[64];
	snprintf(msg, sizeof(msg), "TARGET %s", target.str);
	notify_host(l, actions, &n, max, msg);

	l->session_id = reply_session;
	l->pending = true;
	l->remote = caller;
	l->local = target;
	l->avg_quality = 0;
	l->state = ARDOP_LINK_IRS_CON_ACK;
	l->pending_deadline = now + ARDOP_MS_TO_SAMPLES(IRS_PENDING_TIMEOUT_MS);

	size_t len = ardop_encode_conack_timing(reply, ev->leader_ms,
						reply_session, l->out_frame);
	ardop_action a = {0};
	a.kind = ARDOP_ACT_SEND_FRAME;
	a.frame_type = reply;
	a.data = l->out_frame;
	a.data_len = len;
	a.leader_ms = l->reply_leader_ms;   /* quick turnaround leader. */
	emit(actions, &n, max, &a);

	set_timer(l, actions, &n, max, l->pending_deadline);
	return n;
}

static size_t step_disc_rx(ardop_link *l, const ardop_event *ev,
			   uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	/* A DISC from a previous connection: the other station missed our END.
	 * Answer END with the *previous* session id and hold the closing ID
	 * until its ID has had time to play. Stay in DISC. (Protocol rule 1.5.) */
	if (ev->frame_type == ARDOP_FT_DISC) {
		size_t len = ardop_encode_control(ARDOP_FT_END,
						  l->last_session_id,
						  l->out_frame);
		send_frame(l, actions, &n, max, ARDOP_FT_END, len);

		l->final_id_deadline = now + ARDOP_MS_TO_SAMPLES(FINAL_ID_HOLD_MS);
		set_timer(l, actions, &n, max, l->final_id_deadline);
		return n;
	}

	/* A connect request. */
	if (ev->frame_type >= ARDOP_FT_CON_REQ_MIN
	    && ev->frame_type <= ARDOP_FT_CON_REQ_MAX)
		return step_disc_conreq(l, ev, now, actions, max);

	return n;
}

/* --- dispatch ------------------------------------------------------------ */

void ardop_link_init(ardop_link *l)
{
	*l = (ardop_link){0};
	l->mode = ARDOP_MODE_ARQ;
	l->state = ARDOP_LINK_DISC;
	l->leader_ms = 240;        /* inherited LeaderLength default. */
	l->reply_leader_ms = 240;  /* inherited intARQDefaultDlyMs default. */
}

size_t ardop_link_step(ardop_link *l, const ardop_link_input *in,
		       uint64_t now_samples, ardop_action *actions,
		       size_t max_actions)
{
	switch (in->kind) {
	case ARDOP_IN_RX:
		switch (l->state) {
		case ARDOP_LINK_DISC:
			return step_disc_rx(l, &in->as.rx, now_samples,
					     actions, max_actions);
		case ARDOP_LINK_ISS:
		case ARDOP_LINK_IRS_CON_ACK:
		case ARDOP_LINK_IRS_DATA:
		case ARDOP_LINK_IDLE:
		case ARDOP_LINK_IRS_TO_ISS:
			/* Ported as these states land. */
			return 0;
		}
		return 0;

	case ARDOP_IN_HOST:
		/* Ported as host commands land. */
		return 0;

	case ARDOP_IN_NONE:
		/* Timer service ported as timers land. */
		return 0;
	}
	return 0;
}
