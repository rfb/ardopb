#include "link/link.h"

#include "codec/frame.h"
#include "link/frames.h"

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

/* --- DISC: not connected ------------------------------------------------- */

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
		ardop_action t = {0};
		t.kind = ARDOP_ACT_SET_TIMER;
		t.deadline = l->final_id_deadline;
		emit(actions, &n, max, &t);
		return n;
	}

	return n;
}

/* --- dispatch ------------------------------------------------------------ */

void ardop_link_init(ardop_link *l)
{
	*l = (ardop_link){0};
	l->mode = ARDOP_MODE_ARQ;
	l->state = ARDOP_LINK_DISC;
	l->leader_ms = 240;   /* inherited LeaderLength default. */
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
		case ARDOP_LINK_IRS:
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
