#include "link/link.h"

#include <stdio.h>
#include <string.h>

#include "codec/crc.h"
#include "codec/dataframe.h"
#include "codec/frame.h"
#include "codec/locator.h"
#include "codec/packed6.h"
#include "codec/stationid.h"
#include "link/bandwidth.h"
#include "link/datamodes.h"
#include "link/frames.h"
#include "link/quality.h"
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

/** Resend a ConReq this often while awaiting a ConAck (ARQ.c 2000 ms). */
#define CONREQ_REPEAT_MS 2000

/** A ConReq frame: 2 header + mycall(6) + target(6) + RS(4). */
#define CONREQ_LEN 18
#define CONREQ_RS_LEN 4

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

/* Build a 2-byte control frame into out_frame and emit it as a SEND_FRAME. */
static void send_control(ardop_link *l, ardop_action *actions, size_t *n,
			 size_t max, uint8_t frame_type)
{
	size_t len = ardop_encode_control(frame_type, l->session_id,
					  l->out_frame);
	send_frame(l, actions, n, max, frame_type, len);
}

/* Build a station ID frame into out_frame: [ID][ID^0xFF][call(6)][grid(6)][RS4].
 * Returns the length (18) or 0 on failure. Ported from EncodeARQIDFrame. */
static size_t build_id_frame(ardop_link *l, const ardop_stationid *call)
{
	l->out_frame[0] = ARDOP_FT_ID;
	l->out_frame[1] = (uint8_t)(ARDOP_FT_ID ^ 0xFF);   /* ID: session 0xFF. */
	if (!ardop_stationid_to_buffer(call, &l->out_frame[2]))
		return 0;
	const ardop_packed6 *grid = ardop_locator_as_bytes(&l->grid);
	memcpy(&l->out_frame[2 + ARDOP_PACKED6_SIZE], grid->b,
	       ARDOP_PACKED6_SIZE);
	if (!l->rs || ardop_rs_append(l->rs, &l->out_frame[2], 12, 4) != 0)
		return 0;
	return 18;
}

/*
 * Arm the repeat timer to resend the frame now in out_frame if no response
 * arrives within the repeat interval. Called by the sender after a frame that
 * awaits a reply (ConReq, ConAck, data, IDLE); response-only frames (an IRS
 * ACK/NAK) do not arm it. The bytes stay in out_frame until the next send.
 */
static void arm_repeat(ardop_link *l, uint8_t frame_type, size_t len,
		       uint64_t now)
{
	if (l->repeat_interval_ms == 0)
		l->repeat_interval_ms = 2000;   /* intFrameRepeatInterval default. */
	l->repeat_frame_type = frame_type;
	l->repeat_frame_len = len;
	l->repeat_deadline = now + ARDOP_MS_TO_SAMPLES(l->repeat_interval_ms);
}

/* Tear the machine back down to a clean disconnected state, keeping the session
 * id available (a late DISC can still be answered). */
static void reset_to_disc(ardop_link *l)
{
	l->last_session_id = l->session_id;
	l->state = ARDOP_LINK_DISC;
	l->pending = false;
	l->repeat_interval_ms = 0;
	l->repeat_deadline = 0;
	l->pending_deadline = 0;
}

/* The session width a ConAck frame type establishes. */
static int conack_bandwidth(uint8_t frame_type)
{
	switch (frame_type) {
	case ARDOP_FT_CON_ACK_200:  return 200;
	case ARDOP_FT_CON_ACK_500:  return 500;
	case ARDOP_FT_CON_ACK_1000: return 1000;
	case ARDOP_FT_CON_ACK_2000: return 2000;
	}
	return 0;
}

/* A DISC was received while connected: tell the host, answer END, hold the
 * closing ID, and return to DISC keeping the session id for a late DISC
 * (rule 1.5). Shared by the ISS, IRS and IDLE states. */
static void send_end_and_disconnect(ardop_link *l, uint64_t now,
				    ardop_action *actions, size_t *n, size_t max)
{
	notify_host(l, actions, n, max, "DISCONNECTED");
	send_control(l, actions, n, max, ARDOP_FT_END);
	l->final_id_deadline = now + ARDOP_MS_TO_SAMPLES(FINAL_ID_HOLD_MS);
	set_timer(l, actions, n, max, l->final_id_deadline);
	reset_to_disc(l);
}

/*
 * Teardown frames common to every connected state: a DISC from the peer is
 * answered with END and returns us to DISC; an END (the peer's answer to our own
 * DISC, or their disconnect) drops us straight to DISC. Returns true, with
 * actions filled, if @p ev was a teardown frame.
 */
static bool connected_teardown(ardop_link *l, const ardop_event *ev,
			       uint64_t now, ardop_action *actions, size_t *n,
			       size_t max)
{
	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return false;

	if (ev->frame_type == ARDOP_FT_DISC) {
		send_end_and_disconnect(l, now, actions, n, max);
		return true;
	}
	if (ev->frame_type == ARDOP_FT_END) {
		notify_host(l, actions, n, max, "DISCONNECTED");
		reset_to_disc(l);
		return true;
	}
	return false;
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
	l->final_id_call = target;   /* ID under the call this session answered on. */
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

/*
 * A received PING. If it is for us and we answer pings, reply PINGACK carrying
 * the measured S/N and quality, tell the host, and arm a short closing-ID timer
 * (with the ping's target as the ID call) unless one is already pending.
 * Otherwise tell the host to cancel any pending. Ported from ProcessPingFrame's
 * DISC arm.
 */
static size_t step_disc_ping(ardop_link *l, const ardop_event *ev,
			     uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;
	ardop_stationid caller, target;

	if (ev->data_len < 2 * (int)ARDOP_PACKED6_SIZE)
		return 0;
	if (ardop_stationid_from_bytes(ev->data, &caller) != ARDOP_STATIONID_OK)
		return 0;
	if (ardop_stationid_from_bytes(ev->data + ARDOP_PACKED6_SIZE, &target)
	    != ARDOP_STATIONID_OK)
		return 0;

	if (l->listening && l->enable_ping_ack
	    && ardop_ping_to_me(&caller, &target, &l->mycall, l->auxcalls,
				l->n_aux)) {
		size_t len = ardop_encode_pingack(ARDOP_FT_PING_ACK, ev->sn,
						  ev->quality, l->out_frame);
		send_frame(l, actions, &n, max, ARDOP_FT_PING_ACK, len);
		notify_host(l, actions, &n, max, "PINGREPLY");

		/* Send an ID after the PingAck, under the ping's target call --
		 * but only if a closing ID is not already scheduled. */
		if (l->final_id_deadline == 0) {
			l->final_id_call = target;
			l->final_id_deadline =
				now + ARDOP_MS_TO_SAMPLES(FINAL_ID_HOLD_MS);
			set_timer(l, actions, &n, max, l->final_id_deadline);
		}
		return n;
	}

	notify_host(l, actions, &n, max, "CANCELPENDING");
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

	/* A ping. */
	if (ev->frame_type == ARDOP_FT_PING)
		return step_disc_ping(l, ev, now, actions, max);

	return n;
}

/* --- ISS_CON_REQ: sent a ConReq, awaiting the ConAck --------------------- */

/*
 * Awaiting the IRS's ConAck. The session id is already correct (set when the
 * ConReq went out), so a decode of a ConAck under it confirms our session --
 * record the negotiated width, reply with our own ConAck carrying our received
 * leader (the three-way handshake), and move to ISS. A ConRejBusy/ConRejBW
 * aborts back to DISC. Ported from ProcessRcvdARQFrame's ISS/ISSConReq arm
 * (rules 1.3, 1.4).
 */
static size_t step_iss_conreq_rx(ardop_link *l, const ardop_event *ev,
				 uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	if (ev->frame_type >= ARDOP_FT_CON_ACK_MIN
	    && ev->frame_type <= ARDOP_FT_CON_ACK_MAX) {
		l->session_bw = conack_bandwidth(ev->frame_type);
		l->pending = false;
		l->state = ARDOP_LINK_ISS_CON_ACK;
		l->repeat_interval_ms = CONREQ_REPEAT_MS;

		size_t len = ardop_encode_conack_timing(ev->frame_type,
							ev->leader_ms,
							l->session_id,
							l->out_frame);
		ardop_action a = {0};
		a.kind = ARDOP_ACT_SEND_FRAME;
		a.frame_type = ev->frame_type;
		a.data = l->out_frame;
		a.data_len = len;
		a.leader_ms = l->leader_ms;
		emit(actions, &n, max, &a);
		arm_repeat(l, ev->frame_type, len, now);
		set_timer(l, actions, &n, max, l->repeat_deadline);
		return n;
	}

	if (ev->frame_type == ARDOP_FT_CON_REJ_BUSY
	    || ev->frame_type == ARDOP_FT_CON_REJ_BW) {
		char msg[64];
		const char *why = ev->frame_type == ARDOP_FT_CON_REJ_BUSY
					  ? "REJECTEDBUSY" : "REJECTEDBW";
		snprintf(msg, sizeof(msg), "%s %s", why, l->remote.str);
		notify_host(l, actions, &n, max, msg);
		reset_to_disc(l);
		return n;
	}

	return 0;   /* any other frame is ignored while awaiting the ConAck. */
}

/* --- ISS send ------------------------------------------------------------ */

/* Whole-frame data capacity of a frame type (FrameSize[]). */
static int frame_capacity(uint8_t frame_type)
{
	const ardop_frame_spec *spec = ardop_frame_spec_for(frame_type);
	if (spec == NULL)
		return 0;
	return spec->carriers * spec->data_bytes_per_carrier;
}

/* Drop the leading @p count bytes from the outbound queue (RemoveDataFromQueue). */
static void tx_dequeue(ardop_link *l, int count)
{
	if (count <= 0)
		return;
	if (count > (int)l->tx_len)
		count = (int)l->tx_len;
	memmove(l->tx_data, l->tx_data + count, l->tx_len - (size_t)count);
	l->tx_len -= (size_t)count;
}

/*
 * Gear-shift: choose whether the next frame moves to a more robust or faster
 * mode. Ported from Gearshift_9. Shifts down after DownNAKS consecutive NAKs
 * (2 normally, 1 if the current mode has never worked); shifts up when the
 * average quality clears the mode's threshold with >= 2 ACKs, unless the
 * remaining data already fits the current mode or the next mode was tried and
 * immediately failed (retried only after 5 ACKs). Sets shift_up_dn; the send
 * applies it. Constants are normative-ish (link convergence) -- preserved.
 */
static void gearshift(ardop_link *l)
{
	int down_naks = 2;
	int bytes_remaining = (int)l->tx_len;

	if (l->mode_has_worked[l->mode_ptr] == 0)
		down_naks = 1;   /* revert immediately from a mode that never worked. */

	if (l->ack_ctr)
		l->mode_has_worked[l->mode_ptr]++;
	else if (l->nak_ctr)
		l->mode_naks[l->mode_ptr]++;

	if (l->mode_ptr > 0 && l->nak_ctr >= down_naks) {
		l->shift_up_dn = -1;
		l->avg_quality = 0;
		l->nak_ctr = 0;
		l->ack_ctr = 0;
		return;
	}

	if (l->avg_quality > l->thresholds[l->mode_ptr]
	    && l->mode_ptr < (int)l->modes_len - 1 && l->ack_ctr >= 2) {
		/* Don't shift up if the remaining data fits the current mode. */
		if (bytes_remaining <= frame_capacity(l->modes[l->mode_ptr])) {
			l->shift_up_dn = 0;
			return;
		}
		/* Don't retry a mode that was tried and immediately failed until
		 * 5 successive ACKs. */
		if (l->mode_has_been_tried[l->mode_ptr + 1]
		    && l->mode_has_worked[l->mode_ptr + 1] == 0
		    && l->ack_ctr < 5) {
			l->shift_up_dn = 0;
			return;
		}
		l->shift_up_dn = 1;
		l->mode_has_been_tried[l->mode_ptr + 1] = 1;
		l->avg_quality = 0;
		l->nak_ctr = 0;
		l->ack_ctr = 0;
	}
}

/* Apply a pending gear shift to the mode pointer (GetNextFrameData's shift arm). */
static void apply_shift(ardop_link *l)
{
	if (l->shift_up_dn < 0) {
		if (l->mode_ptr > 0) {
			int p = l->mode_ptr + l->shift_up_dn;
			l->mode_ptr = p < 0 ? 0 : p;
		}
		l->shift_up_dn = 0;
	} else if (l->shift_up_dn > 0) {
		if (l->mode_ptr < (int)l->modes_len) {
			int p = l->mode_ptr + l->shift_up_dn;
			l->mode_ptr = p > (int)l->modes_len ? (int)l->modes_len
							    : p;
		}
		l->shift_up_dn = 0;
	}
}

/*
 * Build and send the next outbound frame: a data frame at the current gear-shift
 * mode, or an IDLE keep-alive if the queue is empty. Any pending shift is applied
 * first. The data frame type toggles its even/odd bit against the last one
 * acknowledged so a retransmission is distinguishable from the next frame
 * (GetNextFrameData). It carries the queue's leading bytes up to the frame
 * capacity; those bytes stay queued until acked.
 */
static void iss_send_next(ardop_link *l, uint64_t now, ardop_action *actions,
			  size_t *n, size_t max)
{
	apply_shift(l);

	if (l->tx_len == 0) {
		send_control(l, actions, n, max, ARDOP_FT_IDLE);
		l->state = ARDOP_LINK_IDLE;
		arm_repeat(l, ARDOP_FT_IDLE, 2, now);
		return;
	}

	uint8_t base = l->modes[l->mode_ptr];
	uint8_t ft = ((base & 1u) == (l->last_data_acked & 1u))
			     ? (uint8_t)(base ^ 1u) : base;
	l->last_data_sent = ft;

	const ardop_frame_spec *spec = ardop_frame_spec_for(ft);
	int cap = spec->carriers * spec->data_bytes_per_carrier;
	int take = (int)l->tx_len < cap ? (int)l->tx_len : cap;

	int len = ardop_encode_data_frame(l->rs, ft, l->session_id, l->tx_data,
					  take, l->out_frame);
	if (len <= 0)
		return;
	l->outstanding_len = take;
	l->state = ARDOP_LINK_ISS;

	ardop_action a = {0};
	a.kind = ARDOP_ACT_SEND_FRAME;
	a.frame_type = ft;
	a.data = l->out_frame;
	a.data_len = (size_t)len;
	a.leader_ms = l->leader_ms;
	emit(actions, n, max, &a);
	arm_repeat(l, ft, (size_t)len, now);
}

/* --- timer service (ARDOP_IN_NONE) --------------------------------------- */

/*
 * Service the deadlines: give up on a pending connection that never completed,
 * and resend the outstanding frame if no response arrived within the repeat
 * interval. Ported from the CheckTimers repeat and tmrIRSPendingTimeout paths.
 * The overall send-timeout give-up, the DISC retry count and the final-ID send
 * are deferred (the latter needs the ID-frame builder).
 */
static size_t step_timer(ardop_link *l, uint64_t now, ardop_action *actions,
			 size_t max)
{
	size_t n = 0;

	if (l->pending_deadline && now >= l->pending_deadline) {
		l->pending_deadline = 0;
		notify_host(l, actions, &n, max, "DISCONNECTED");
		reset_to_disc(l);
		return n;
	}

	/* The closing ID after END: once the hold expires, send our ID frame.
	 * The original also waits for the busy detector to clear; that gate lands
	 * with the BUSY_CHANGED events. */
	if (l->final_id_deadline && now >= l->final_id_deadline) {
		l->final_id_deadline = 0;
		/* Use the session's local call if we captured one, else ours. */
		const ardop_stationid *call = l->final_id_call.str[0]
					      ? &l->final_id_call : &l->mycall;
		size_t len = build_id_frame(l, call);
		if (len > 0)
			send_frame(l, actions, &n, max, ARDOP_FT_ID, len);
		return n;
	}

	if (l->repeat_deadline && now >= l->repeat_deadline) {
		/* A DISC we are sending to disconnect gets five tries, then we
		 * force the disconnect regardless (GetNextARQFrame's DISC path). */
		if (l->repeat_frame_type == ARDOP_FT_DISC
		    && l->disc_repeat_count > 0) {
			l->disc_repeat_count++;
			if (l->disc_repeat_count > 5) {
				l->disc_repeat_count = 0;
				notify_host(l, actions, &n, max, "DISCONNECTED");
				reset_to_disc(l);
				return n;
			}
		}
		ardop_action a = {0};
		a.kind = ARDOP_ACT_SEND_FRAME;
		a.frame_type = l->repeat_frame_type;
		a.data = l->out_frame;
		a.data_len = l->repeat_frame_len;
		a.leader_ms = l->leader_ms;
		emit(actions, &n, max, &a);
		l->repeat_deadline =
			now + ARDOP_MS_TO_SAMPLES(l->repeat_interval_ms);
	}

	return n;
}

/* --- ISS_CON_ACK: sent our ConAck, awaiting the IRS's DataACK ------------- */

/*
 * The final handshake message from the IRS's side: its DataACK confirms our
 * ConAck was received, so the connection is established. Tell the host CONNECTED,
 * initialise the gear-shift ladder for the session bandwidth, and send the first
 * frame (data or IDLE). Ported from the ISS ConAck+DataACK arm of
 * ProcessRcvdARQFrame.
 */
/* Initialise the gear-shift ladder and counters to begin sending as the ISS.
 * Shared by the connect-time start and the BREAK-driven turnover. */
static void iss_begin_sending(ardop_link *l)
{
	l->last_data_acked = 1;   /* odd -> first data frame is even. */
	l->avg_quality = 0;
	l->modes = ardop_data_modes(l->session_bw, l->fsk_only,
				    l->tuning_range, l->use_600_modes,
				    &l->modes_len);
	l->thresholds = ardop_shift_up_thresholds(l->session_bw,
						  l->tuning_range,
						  l->use_600_modes);
	l->mode_ptr = 0;
	l->ack_ctr = 0;
	l->nak_ctr = 0;
	l->shift_up_dn = 0;
	memset(l->mode_has_worked, 0, sizeof(l->mode_has_worked));
	memset(l->mode_has_been_tried, 0, sizeof(l->mode_has_been_tried));
	memset(l->mode_naks, 0, sizeof(l->mode_naks));
}

static size_t step_iss_conack_rx(ardop_link *l, const ardop_event *ev,
				 uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	if (ev->frame_type >= ARDOP_FRAME_DATA_ACK_MIN) {
		l->pending = false;
		iss_begin_sending(l);

		char msg[64];
		snprintf(msg, sizeof(msg), "CONNECTED %s %d", l->remote.str,
			 l->session_bw);
		notify_host(l, actions, &n, max, msg);

		iss_send_next(l, now, actions, &n, max);
		return n;
	}

	return 0;
}

/* --- ISS: connected, sending data ---------------------------------------- */

/*
 * The IRS sent BREAK: ACK it and yield the link, becoming IRS (settling out of
 * IRS_FROM_ISS on the first frame received). Applies whether we were actively
 * sending (ISS) or idling (IDLE).
 *
 * **The queue is kept.** It used to be discarded here, with a note that
 * ardopcf's SaveQueueOnBreak -- the option that let the application restore the
 * data -- had been dropped in the port. Dropping the option is fine; dropping
 * the data is not, and it contradicted this machine's own rule 3.4 twelve
 * hundred lines further down, where the IRS deliberately sends its BREAK on a
 * *still-unacked* frame "so the ISS keeps that frame for after the turnover".
 * The ISS cannot keep a frame it has just thrown away.
 *
 * Nothing here changes what goes on the air. These are bytes the host gave us
 * that have not been transmitted, or -- for the outstanding frame -- were
 * transmitted and deliberately not acknowledged, precisely so that we would send
 * them again. Discarding them lost host data silently: no NAK, no fault, no
 * counter, and an application that had been told the bytes were accepted.
 *
 * @p outstanding_len is still cleared, because nothing is in flight any more.
 * The bytes it counted stay at the head of the queue and go out again once we
 * are the ISS.
 */
static void iss_yield_on_break(ardop_link *l, ardop_action *actions, size_t *n,
			       size_t max)
{
	l->outstanding_len = 0;
	l->last_data_to_host = -1;
	l->state = ARDOP_LINK_IRS_FROM_ISS;
	send_control(l, actions, n, max, ardop_quality_to_ack_type(100));
}

/*
 * Connected and sending. On a DataACK the outstanding frame's bytes are
 * confirmed: drop them from the queue, fold the reported quality into the
 * average, gear-shift, and send the next frame (data or IDLE). On a DataNAK,
 * gear-shift and resend only if it produced a shift (otherwise the repeat timer
 * resends the current frame unchanged). A DISC ends the session. Ported from
 * ProcessRcvdARQFrame's ISS/ISSData arm.
 */
static size_t step_iss_data_rx(ardop_link *l, const ardop_event *ev,
			       uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	if (connected_teardown(l, ev, now, actions, &n, max))
		return n;

	/* BREAK from the IRS: yield the link. */
	if (ev->frame_type == ARDOP_FT_BREAK) {
		iss_yield_on_break(l, actions, &n, max);
		return n;
	}

	/* DataACK: confirm and advance. */
	if (ev->frame_type >= ARDOP_FRAME_DATA_ACK_MIN) {
		l->ack_ctr++;
		l->last_data_acked = l->last_data_sent;
		tx_dequeue(l, l->outstanding_len);
		l->outstanding_len = 0;
		l->avg_quality = ardop_quality_avg(
			l->avg_quality, ardop_quality_from_type(ev->frame_type));
		gearshift(l);
		l->nak_ctr = 0;
		iss_send_next(l, now, actions, &n, max);
		return n;
	}

	/* DataNAK: gear-shift; resend now only if the mode changed. */
	if (ev->frame_type <= ARDOP_FRAME_DATA_NAK_MAX) {
		l->nak_ctr++;
		l->avg_quality = ardop_quality_avg(
			l->avg_quality, ardop_quality_from_type(ev->frame_type));
		gearshift(l);
		if (l->shift_up_dn != 0) {
			l->nak_ctr = 0;
			iss_send_next(l, now, actions, &n, max);
		}
		l->ack_ctr = 0;
		return n;
	}

	return 0;
}

/* --- IDLE: connected ISS with nothing to send ---------------------------- */

/*
 * Idling: the ISS has drained its queue and is sending IDLE keep-alives. A
 * DataACK to an IDLE resumes sending if the host has since queued data (else it
 * keeps idling, the repeat timer resending IDLE); a DISC ends the session.
 * Ported from ProcessRcvdARQFrame's IDLE arm. The 9-minute ID (needs the
 * ID-frame builder) and the BREAK turnover (A5) are deferred.
 */
static size_t step_idle_rx(ardop_link *l, const ardop_event *ev, uint64_t now,
			   ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	if (connected_teardown(l, ev, now, actions, &n, max))
		return n;

	/* BREAK from the IRS while we idle: yield the link. */
	if (ev->frame_type == ARDOP_FT_BREAK) {
		iss_yield_on_break(l, actions, &n, max);
		return n;
	}

	if (ev->frame_type >= ARDOP_FRAME_DATA_ACK_MIN) {
		if (l->tx_len > 0)
			iss_send_next(l, now, actions, &n, max);
		return n;
	}

	return 0;
}

/* --- IRS_CON_ACK: sent our ConAck, awaiting the ISS's ConAck ------------- */

/*
 * The IRS has answered a ConReq with a ConAck and is awaiting the ISS's ConAck,
 * which completes the four-message handshake. On it: commit the session, adopt
 * the width, tell the host CONNECTED, answer with a DataACK carrying the decode
 * quality, and become IRS_DATA ready to receive. Ported from the ISS-ConAck arm
 * of ProcessRcvdARQFrame's IRS/IRSConAck state (rule 1.4).
 *
 * A repeated ConReq here (the ISS missed our ConAck) should re-send the ConAck;
 * that robustness path is not yet ported -- noted for when the handshake is
 * exercised over a lossy loopback.
 */
static size_t step_irs_conack_rx(ardop_link *l, const ardop_event *ev,
				 uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	if (ev->frame_type >= ARDOP_FT_CON_ACK_MIN
	    && ev->frame_type <= ARDOP_FT_CON_ACK_MAX) {
		l->session_bw = conack_bandwidth(ev->frame_type);
		l->pending = false;
		l->pending_deadline = 0;
		l->avg_quality = 0;
		l->state = ARDOP_LINK_IRS_DATA;

		char msg[64];
		snprintf(msg, sizeof(msg), "CONNECTED %s %d", l->remote.str,
			 l->session_bw);
		notify_host(l, actions, &n, max, msg);

		uint8_t ack = ardop_quality_to_ack_type(ev->quality);
		size_t len = ardop_encode_control(ack, l->session_id,
						  l->out_frame);
		ardop_action a = {0};
		a.kind = ARDOP_ACT_SEND_FRAME;
		a.frame_type = ack;
		a.data = l->out_frame;
		a.data_len = len;
		a.leader_ms = l->leader_ms;
		emit(actions, &n, max, &a);
		(void)now;
		return n;
	}

	return 0;
}

/* --- IRS_DATA: connected, receiving data --------------------------------- */

/* Emit a DELIVER_DATA action carrying a copy of the payload. */
static void deliver_data(ardop_link *l, ardop_action *actions, size_t *n,
			 size_t max, const uint8_t *data, int len)
{
	ardop_action a = {0};
	if (len > (int)sizeof(l->out_data))
		len = (int)sizeof(l->out_data);
	memcpy(l->out_data, data, (size_t)len);
	a.kind = ARDOP_ACT_DELIVER_DATA;
	a.data = l->out_data;
	a.data_len = (size_t)len;
	emit(actions, n, max, &a);
}

/* Request the link: send BREAK and enter IRS_TO_ISS, repeating BREAK until the
 * ISS acknowledges. The original randomises the repeat interval to dodge
 * collisions (ComputeInterFrameInterval(1000 + rand()%2000)); the pure core uses
 * a fixed interval instead -- determinism over collision-jitter. (Rule 3.4.) */
static void send_break(ardop_link *l, uint64_t now, ardop_action *actions,
		       size_t *n, size_t max)
{
	send_control(l, actions, n, max, ARDOP_FT_BREAK);
	l->state = ARDOP_LINK_IRS_TO_ISS;
	l->break_pending = false;
	l->repeat_interval_ms = 2000;
	arm_repeat(l, ARDOP_FT_BREAK, 2, now);
}

/*
 * Connected and receiving. A good data frame is delivered to the host unless it
 * repeats the last one delivered (the even/odd type alternation makes a genuine
 * retransmission share its type), and is always ACKed with the decode quality --
 * the ISS may have missed the previous ACK. A failed decode is re-ACKed if it
 * matches the last frame already delivered, else NAKed. A DISC ends the session.
 *
 * If we hold data and a break was requested (host BREAK) or is automatic, we
 * BREAK to take the link -- on a new data frame (rule 3.4) or when the ISS goes
 * IDLE. This handler also serves the IRS_FROM_ISS settling state, which cannot
 * break until it has received a frame (rule 3.5). Ported from the IRS/IRSData +
 * IRSfromISS arm of ProcessRcvdARQFrame.
 */
static size_t step_irs_data_rx(ardop_link *l, const ardop_event *ev,
			       uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;
	bool can_break = (l->state == ARDOP_LINK_IRS_DATA);

	if (connected_teardown(l, ev, now, actions, &n, max))
		return n;

	/* The ISS is idle: if we have data queued and want the link, BREAK;
	 * otherwise keep the link alive with an ACK. */
	if (ev->kind == ARDOP_EV_FRAME_DECODED
	    && ev->frame_type == ARDOP_FT_IDLE) {
		if (can_break && l->tx_len > 0
		    && (l->break_pending || l->auto_break)) {
			send_break(l, now, actions, &n, max);
			return n;
		}
		send_control(l, actions, &n, max,
			     ardop_quality_to_ack_type(ev->quality));
		return n;
	}

	/* A good data frame. */
	if (ev->kind == ARDOP_EV_FRAME_DECODED
	    && ardop_frame_is_data(ev->frame_type)) {
		/* Requested a break: send it on a new (still-unacked) frame,
		 * before ACKing, so the ISS keeps that frame for after the
		 * turnover (rule 3.4). */
		if (can_break && l->break_pending
		    && (int)ev->frame_type != l->last_acked_type) {
			send_break(l, now, actions, &n, max);
			return n;
		}
		if ((int)ev->frame_type != l->last_data_to_host) {
			deliver_data(l, actions, &n, max, ev->data,
				     ev->data_len);
			l->last_data_to_host = ev->frame_type;
		}
		/* First frame after a handover settles us into IRS (rule 3.5). */
		if (l->state == ARDOP_LINK_IRS_FROM_ISS)
			l->state = ARDOP_LINK_IRS_DATA;
		send_control(l, actions, &n, max,
			     ardop_quality_to_ack_type(ev->quality));
		l->last_acked_type = ev->frame_type;
		return n;
	}

	/* A failed decode already ACKed once: re-ACK, data is already delivered. */
	if (ev->kind == ARDOP_EV_FRAME_BAD
	    && (int)ev->frame_type == l->last_acked_type) {
		send_control(l, actions, &n, max,
			     ardop_quality_to_ack_type(ev->quality));
		return n;
	}

	/* A failed decode of a new data frame: NAK with quality. */
	if (ev->kind == ARDOP_EV_FRAME_BAD
	    && ardop_frame_is_data(ev->frame_type)) {
		if (l->state == ARDOP_LINK_IRS_FROM_ISS)
			l->state = ARDOP_LINK_IRS_DATA;
		send_control(l, actions, &n, max,
			     ardop_quality_to_nak_type(ev->quality));
		return n;
	}

	return 0;
}

/* --- IRS_TO_ISS: sent BREAK, awaiting the ACK to become ISS --------------- */

/*
 * We requested the link with BREAK and are repeating it. When the ISS ACKs, the
 * turnover completes: stop the BREAKs, become the ISS, and start sending our
 * queued data. Ported from the IRStoISS+ACK transition that the inherited tree
 * wrongly performs inside ProcessNewSamples (SoundInput.c:1070) -- here it is a
 * proper FSM transition.
 */
static size_t step_irs_to_iss_rx(ardop_link *l, const ardop_event *ev,
				 uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (connected_teardown(l, ev, now, actions, &n, max))
		return n;

	if (ev->kind == ARDOP_EV_FRAME_DECODED
	    && ev->frame_type >= ARDOP_FRAME_DATA_ACK_MIN) {
		iss_begin_sending(l);
		l->last_data_to_host = -1;   /* re-arm dedup for the new IRS. */
		iss_send_next(l, now, actions, &n, max);
		return n;
	}

	return 0;
}

/* --- host commands ------------------------------------------------------- */

/*
 * Host CONNECT (ARQCALL): initiate an ARQ session. Build a ConReq encoding our
 * bandwidth setting and our + the target callsign, become ISS awaiting the
 * ConAck, set the session id from the callsign pair, and repeat the ConReq
 * every 2 s until answered. Ported from SendARQConnectRequest (rule 1.1).
 *
 * Only initiates from DISC; a CONNECT in any other state is ignored (the shell
 * disconnects first). The ConReq always goes out under session id 0xFF.
 */
static size_t step_host_connect(ardop_link *l, const ardop_host_cmd *cmd,
				uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (l->state != ARDOP_LINK_DISC)
		return 0;

	/* A per-call bandwidth overrides the station default when given. */
	ardop_arq_bandwidth bw = cmd->bandwidth != ARDOP_ARQ_BW_UNDEFINED
					 ? cmd->bandwidth
					 : l->bw_setting;
	uint8_t type = ardop_bandwidth_conreq_type(bw);
	if (type == 0)
		return 0;

	l->out_frame[0] = type;
	l->out_frame[1] = (uint8_t)(type ^ 0xFF);   /* ConReq: session 0xFF. */
	if (!ardop_stationid_to_buffer(&l->mycall, &l->out_frame[2]))
		return 0;
	if (!ardop_stationid_to_buffer(&cmd->target,
				       &l->out_frame[2 + ARDOP_PACKED6_SIZE]))
		return 0;
	if (!l->rs || ardop_rs_append(l->rs, &l->out_frame[2], 12,
				      CONREQ_RS_LEN) != 0)
		return 0;

	l->remote = cmd->target;
	l->local = l->mycall;
	l->final_id_call = l->mycall;
	l->session_id = ardop_session_id(l->mycall.str, cmd->target.str);
	l->pending = true;
	l->state = ARDOP_LINK_ISS_CON_REQ;
	l->repeat_interval_ms = CONREQ_REPEAT_MS;

	ardop_action a = {0};
	a.kind = ARDOP_ACT_SEND_FRAME;
	a.frame_type = type;
	a.data = l->out_frame;
	a.data_len = CONREQ_LEN;
	a.leader_ms = l->leader_ms;
	emit(actions, &n, max, &a);

	arm_repeat(l, type, CONREQ_LEN, now);
	set_timer(l, actions, &n, max, l->repeat_deadline);
	return n;
}

/*
 * Host SEND_DATA: append payload to the outbound queue (AddDataToDataToSend) and
 * report the new queue depth. The ISS drains it as transmit opportunities arise;
 * this only enqueues. Bytes past the queue capacity are dropped.
 */
static size_t step_host_send_data(ardop_link *l, const ardop_host_cmd *cmd,
				  ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (l->tx_data && cmd->data && l->tx_len < l->tx_cap) {
		size_t room = l->tx_cap - l->tx_len;
		size_t take = cmd->data_len < room ? cmd->data_len : room;
		memcpy(l->tx_data + l->tx_len, cmd->data, take);
		l->tx_len += take;
	}

	char msg[32];
	snprintf(msg, sizeof(msg), "BUFFER %zu", l->tx_len);
	notify_host(l, actions, &n, max, msg);
	return n;
}

/*
 * Host DISCONNECT: from a connected state, begin a graceful disconnect -- send
 * DISC and repeat it (up to five tries in the timer service) until the peer
 * answers END. From DISC there is nothing to disconnect. Ported from
 * CheckForDisconnect.
 */
static size_t step_host_disconnect(ardop_link *l, uint64_t now,
				   ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (l->state == ARDOP_LINK_DISC) {
		notify_host(l, actions, &n, max, "DISCONNECT IGNORED");
		return n;
	}

	notify_host(l, actions, &n, max, "STATUS INITIATING ARQ DISCONNECT");
	send_control(l, actions, &n, max, ARDOP_FT_DISC);
	l->disc_repeat_count = 1;
	l->repeat_interval_ms = 2000;
	arm_repeat(l, ARDOP_FT_DISC, 2, now);
	return n;
}

/*
 * Host ABORT (dirty disconnect): drop the link immediately, discard queued data,
 * and tell the host. Ported from Abort()/GetNextARQFrame's blnAbort path.
 */
static size_t step_host_abort(ardop_link *l, ardop_action *actions, size_t max)
{
	size_t n = 0;

	l->tx_len = 0;
	l->disc_repeat_count = 0;
	reset_to_disc(l);
	notify_host(l, actions, &n, max, "ABORT");
	return n;
}

/*
 * Host BREAK: request the IRS->ISS turnover. Only meaningful while receiving
 * (IRS); it arms a pending break that is acted on when the next data frame or an
 * IDLE arrives. Ported from Break().
 */
static size_t step_host_break(ardop_link *l)
{
	if (l->state == ARDOP_LINK_IRS_DATA)
		l->break_pending = true;
	return 0;
}

static void fec_send_next(ardop_link *l, ardop_action *actions, size_t *n,
			  size_t max);

/* Host FECSEND: start (or add to) a FEC broadcast of the queued data. The frame
 * type is cmd->arg when given, else the standing fec_frame_type. Ported from
 * StartFEC. Only starts while idle (DISC); while already sending, data is simply
 * queued and drains behind the current broadcast. */
static size_t step_host_fec_send(ardop_link *l, const ardop_host_cmd *cmd,
				 ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (cmd->arg)
		l->fec_frame_type = (uint8_t)cmd->arg;

	l->mode = ARDOP_MODE_FEC;
	if (l->state == ARDOP_LINK_FEC_SEND)
		return 0;   /* already broadcasting; new data was queued. */
	if (l->state != ARDOP_LINK_DISC)
		return 0;

	l->session_id = 0;
	l->state = ARDOP_LINK_FEC_SEND;
	fec_send_next(l, actions, &n, max);
	return n;
}

/* Host SEND_ID: transmit a station ID frame (only while disconnected, so it does
 * not collide with an ARQ exchange). Ported from SendID. */
static size_t step_host_id(ardop_link *l, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (l->state != ARDOP_LINK_DISC)
		return 0;

	size_t len = build_id_frame(l, &l->mycall);
	if (len > 0)
		send_frame(l, actions, &n, max, ARDOP_FT_ID, len);
	return n;
}

static size_t step_host(ardop_link *l, const ardop_host_cmd *cmd,
			uint64_t now, ardop_action *actions, size_t max)
{
	switch (cmd->kind) {
	case ARDOP_CMD_CONNECT:
		return step_host_connect(l, cmd, now, actions, max);
	case ARDOP_CMD_SEND_DATA:
		return step_host_send_data(l, cmd, actions, max);
	case ARDOP_CMD_DISCONNECT:
		return step_host_disconnect(l, now, actions, max);
	case ARDOP_CMD_ABORT:
		return step_host_abort(l, actions, max);
	case ARDOP_CMD_BREAK:
		return step_host_break(l);
	case ARDOP_CMD_SET_MODE:
		/* Switch protocol mode only while disconnected. arg is the
		 * ardop_link_mode (ARQ/FEC/RXO). */
		if (l->state == ARDOP_LINK_DISC && cmd->arg >= ARDOP_MODE_ARQ
		    && cmd->arg <= ARDOP_MODE_RXO)
			l->mode = (ardop_link_mode)cmd->arg;
		return 0;
	case ARDOP_CMD_SEND_ID:
		return step_host_id(l, actions, max);
	case ARDOP_CMD_FEC_SEND:
		return step_host_fec_send(l, cmd, actions, max);
	}
	return 0;
}

/* --- dispatch ------------------------------------------------------------ */

void ardop_link_init(ardop_link *l)
{
	*l = (ardop_link){0};
	l->mode = ARDOP_MODE_ARQ;
	l->state = ARDOP_LINK_DISC;
	l->leader_ms = 240;        /* inherited LeaderLength default. */
	l->reply_leader_ms = 240;  /* inherited intARQDefaultDlyMs default. */
	l->last_data_to_host = -1;
	l->last_acked_type = -1;
	l->fec_frame_type = 0x40;  /* 4PSK.200.100.E, the FECMODE default. */
	l->tuning_range = 100;   /* inherited default; selects the 2000 modes. */
}

/* --- FEC: broadcast, no ACKs --------------------------------------------- */

/* Encode and send the next FEC data frame from the queue's front, removing its
 * bytes immediately (there are no ACKs to confirm them). When the queue is
 * empty the broadcast is done: tell the host and return to DISC (still in FEC
 * mode). Ported from GetNextFECFrame. */
static void fec_send_next(ardop_link *l, ardop_action *actions, size_t *n,
			  size_t max)
{
	if (l->tx_len == 0) {
		notify_host(l, actions, n, max, "STATUS FEC send complete");
		reset_to_disc(l);
		return;
	}

	/* A frame type that carries no payload cannot broadcast anything, and
	 * silently returning to DISC would leave the host watching a buffer
	 * that never drains. Say so. */
	int cap = frame_capacity(l->fec_frame_type);
	if (cap <= 0) {
		notify_host(l, actions, n, max,
			    "STATUS FEC send failed: invalid FECMODE");
		reset_to_disc(l);
		return;
	}
	int take = (int)l->tx_len < cap ? (int)l->tx_len : cap;
	int len = ardop_encode_data_frame(l->rs, l->fec_frame_type, 0,
					  l->tx_data, take, l->out_frame);
	if (len <= 0) {
		notify_host(l, actions, n, max,
			    "STATUS FEC send failed: encode error");
		reset_to_disc(l);
		return;
	}
	tx_dequeue(l, take);          /* no ACKs in FEC: consume now. */
	l->fec_reps_sent = 0;
	l->repeat_frame_type = l->fec_frame_type;
	l->repeat_frame_len = (size_t)len;

	ardop_action a = {0};
	a.kind = ARDOP_ACT_SEND_FRAME;
	a.frame_type = l->fec_frame_type;
	a.data = l->out_frame;
	a.data_len = (size_t)len;
	a.leader_ms = l->leader_ms;
	emit(actions, n, max, &a);
}

/* A FEC transmission just finished: repeat the frame if repeats remain, else
 * advance to the next frame (or finish). Driven by the TX-done input. */
static size_t step_fec_tx_done(ardop_link *l, ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (l->fec_reps_sent < l->fec_repeats) {
		l->fec_reps_sent++;
		ardop_action a = {0};
		a.kind = ARDOP_ACT_SEND_FRAME;
		a.frame_type = l->repeat_frame_type;
		a.data = l->out_frame;
		a.data_len = l->repeat_frame_len;
		a.leader_ms = l->leader_ms;
		emit(actions, &n, max, &a);
		return n;
	}

	fec_send_next(l, actions, &n, max);
	return n;
}

/*
 * FEC receive: deliver every good data frame to the host, skipping a frame whose
 * type and content match the last delivered (a repeat). Unlike ARQ there is no
 * parity alternation, so identical consecutive payloads are indistinguishable
 * from repeats and are dropped -- a known FEC limitation the original shares.
 * Ported from ProcessRcvdFECDataFrame's dedup.
 */
static size_t step_fec_rx(ardop_link *l, const ardop_event *ev,
			  ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED
	    || !ardop_frame_is_data(ev->frame_type) || ev->data_len <= 0)
		return 0;

	uint16_t crc = ardop_crc16(ev->data, (size_t)ev->data_len);
	if (l->fec_have_last && (int)ev->frame_type == l->last_data_to_host
	    && crc == l->last_fec_crc)
		return 0;   /* a repeat: already delivered. */

	deliver_data(l, actions, &n, max, ev->data, ev->data_len);
	l->last_data_to_host = ev->frame_type;
	l->last_fec_crc = crc;
	l->fec_have_last = true;
	return n;
}

/* --- RXO: receive-only monitor mode -------------------------------------- */

/*
 * Receive-only: hand every decoded frame's content to the host and never
 * transmit. The ARQ/FEC state machine is bypassed entirely. Ported from the
 * essence of ProcessRXOFrame (the per-frame logging/status is dropped; the data
 * and a status line reach the host). This is the mode `--decodewav` runs in.
 */
static size_t step_rxo_rx(ardop_link *l, const ardop_event *ev,
			  ardop_action *actions, size_t max)
{
	size_t n = 0;

	if (ev->kind != ARDOP_EV_FRAME_DECODED)
		return 0;

	if (ev->data_len > 0)
		deliver_data(l, actions, &n, max, ev->data, ev->data_len);

	const ardop_frame_spec *spec = ardop_frame_spec_for(ev->frame_type);
	char msg[64];
	snprintf(msg, sizeof(msg), "STATUS [RXO] %s received",
		 spec ? spec->name : "?");
	notify_host(l, actions, &n, max, msg);
	return n;
}

size_t ardop_link_step(ardop_link *l, const ardop_link_input *in,
		       uint64_t now_samples, ardop_action *actions,
		       size_t max_actions)
{
	switch (in->kind) {
	case ARDOP_IN_RX:
		/* Receive-only mode bypasses the ARQ/FEC machine. */
		if (l->mode == ARDOP_MODE_RXO)
			return step_rxo_rx(l, &in->as.rx, actions,
					   max_actions);
		/* FEC receive (a station in FEC mode that is not itself
		 * broadcasting) delivers every good data frame. */
		if (l->mode == ARDOP_MODE_FEC
		    && l->state != ARDOP_LINK_FEC_SEND)
			return step_fec_rx(l, &in->as.rx, actions,
					   max_actions);
		switch (l->state) {
		case ARDOP_LINK_DISC:
			return step_disc_rx(l, &in->as.rx, now_samples,
					     actions, max_actions);
		case ARDOP_LINK_ISS_CON_REQ:
			return step_iss_conreq_rx(l, &in->as.rx, now_samples,
						  actions, max_actions);
		case ARDOP_LINK_IRS_CON_ACK:
			return step_irs_conack_rx(l, &in->as.rx, now_samples,
						  actions, max_actions);
		case ARDOP_LINK_IRS_DATA:
			return step_irs_data_rx(l, &in->as.rx, now_samples,
						actions, max_actions);
		case ARDOP_LINK_ISS_CON_ACK:
			return step_iss_conack_rx(l, &in->as.rx, now_samples,
						  actions, max_actions);
		case ARDOP_LINK_ISS:
			return step_iss_data_rx(l, &in->as.rx, now_samples,
						actions, max_actions);
		case ARDOP_LINK_IDLE:
			return step_idle_rx(l, &in->as.rx, now_samples,
					    actions, max_actions);
		case ARDOP_LINK_IRS_TO_ISS:
			return step_irs_to_iss_rx(l, &in->as.rx, now_samples,
						  actions, max_actions);
		case ARDOP_LINK_IRS_FROM_ISS:
			/* Behaves like IRS receiving; settles to IRS_DATA on the
			 * first frame. It cannot break until then (rule 3.5). */
			return step_irs_data_rx(l, &in->as.rx, now_samples,
						actions, max_actions);
		case ARDOP_LINK_FEC_SEND:
			/* Broadcasting: not listening. */
			return 0;
		}
		return 0;

	case ARDOP_IN_HOST:
		return step_host(l, &in->as.host, now_samples, actions,
				 max_actions);

	case ARDOP_IN_NONE:
		return step_timer(l, now_samples, actions, max_actions);

	case ARDOP_IN_TX_DONE:
		/* Drives the back-to-back FEC broadcast. */
		if (l->state == ARDOP_LINK_FEC_SEND)
			return step_fec_tx_done(l, actions, max_actions);
		/* ARQ: the reply/repeat window is time spent *waiting* for the
		 * other station, so it starts when our transmission actually
		 * finishes -- not when the frame was queued, which would count
		 * the frame's own airtime against the timeout. The original got
		 * this for free by blocking through the send (SoundFlush); with a
		 * non-blocking modulator we re-arm on the observed TX completion.
		 * Ported from the dttNextPlay/tmrSendTimeout arming after send. */
		if (l->repeat_deadline) {
			size_t n = 0;
			l->repeat_deadline = now_samples
				+ ARDOP_MS_TO_SAMPLES(l->repeat_interval_ms);
			set_timer(l, actions, &n, max_actions,
				  l->repeat_deadline);
			return n;
		}
		return 0;
	}
	return 0;
}
