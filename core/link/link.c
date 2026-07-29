#include "link/link.h"

#include <stdio.h>
#include <string.h>

#include "codec/frame.h"
#include "codec/stationid.h"
#include "link/bandwidth.h"
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
		l->repeat_deadline = now + ARDOP_MS_TO_SAMPLES(CONREQ_REPEAT_MS);

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

/* Build a 2-byte control frame into out_frame and emit it as a SEND_FRAME. */
static void send_control(ardop_link *l, ardop_action *actions, size_t *n,
			 size_t max, uint8_t frame_type)
{
	size_t len = ardop_encode_control(frame_type, l->session_id,
					  l->out_frame);
	send_frame(l, actions, n, max, frame_type, len);
}

/*
 * Connected and receiving. A good data frame is delivered to the host unless it
 * repeats the last one delivered (the even/odd type alternation makes a genuine
 * retransmission share its type), and is always ACKed with the decode quality --
 * the ISS may have missed the previous ACK. A failed decode is re-ACKed if it
 * matches the last frame already delivered, else NAKed. A DISC ends the session.
 * Ported from ProcessRcvdARQFrame's IRS/IRSData arm. The BREAK turnover and the
 * IRSfromISS substate are not yet ported (they need the host BREAK command).
 */
static size_t step_irs_data_rx(ardop_link *l, const ardop_event *ev,
			       uint64_t now, ardop_action *actions, size_t max)
{
	size_t n = 0;

	/* DISC from the ISS: tell the host, answer END, hold the closing ID,
	 * and return to DISC keeping the session id for a late DISC (rule 1.5). */
	if (ev->kind == ARDOP_EV_FRAME_DECODED
	    && ev->frame_type == ARDOP_FT_DISC) {
		notify_host(l, actions, &n, max, "DISCONNECTED");
		send_control(l, actions, &n, max, ARDOP_FT_END);
		l->final_id_deadline = now + ARDOP_MS_TO_SAMPLES(FINAL_ID_HOLD_MS);
		set_timer(l, actions, &n, max, l->final_id_deadline);
		reset_to_disc(l);
		return n;
	}

	/* A good data frame: deliver if new, always ACK. */
	if (ev->kind == ARDOP_EV_FRAME_DECODED
	    && ardop_frame_is_data(ev->frame_type)) {
		if ((int)ev->frame_type != l->last_data_to_host) {
			deliver_data(l, actions, &n, max, ev->data,
				     ev->data_len);
			l->last_data_to_host = ev->frame_type;
		}
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
		send_control(l, actions, &n, max,
			     ardop_quality_to_nak_type(ev->quality));
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
	l->repeat_deadline = now + ARDOP_MS_TO_SAMPLES(CONREQ_REPEAT_MS);

	ardop_action a = {0};
	a.kind = ARDOP_ACT_SEND_FRAME;
	a.frame_type = type;
	a.data = l->out_frame;
	a.data_len = CONREQ_LEN;
	a.leader_ms = l->leader_ms;
	emit(actions, &n, max, &a);

	set_timer(l, actions, &n, max, l->repeat_deadline);
	return n;
}

static size_t step_host(ardop_link *l, const ardop_host_cmd *cmd,
			uint64_t now, ardop_action *actions, size_t max)
{
	switch (cmd->kind) {
	case ARDOP_CMD_CONNECT:
		return step_host_connect(l, cmd, now, actions, max);
	case ARDOP_CMD_DISCONNECT:
	case ARDOP_CMD_ABORT:
	case ARDOP_CMD_SEND_DATA:
	case ARDOP_CMD_SET_MODE:
	case ARDOP_CMD_FEC_SEND:
	case ARDOP_CMD_SEND_ID:
		/* Ported as these commands land. */
		return 0;
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
		case ARDOP_LINK_ISS:
		case ARDOP_LINK_IDLE:
		case ARDOP_LINK_IRS_TO_ISS:
			/* Ported as these states land. */
			return 0;
		}
		return 0;

	case ARDOP_IN_HOST:
		return step_host(l, &in->as.host, now_samples, actions,
				 max_actions);

	case ARDOP_IN_NONE:
		/* Timer service ported as timers land. */
		return 0;
	}
	return 0;
}
