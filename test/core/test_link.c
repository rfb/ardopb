#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/link.h"
#include "codec/frame.h"
#include "codec/rs.h"
#include "codec/stationid.h"
#include "codec/locator.h"
#include "link/quality.h"
#include "link/session.h"

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
static ardop_rs g_rs;

/*
 * The FSM is proven by scripted input -> action sequences, not by bit-equivalence
 * to ProcessRcvdARQFrame (which is inseparable from ~120 globals). This is the
 * proof method analysis/10 sec.9 step 3a defines for the link. Each test feeds a
 * hand-built input and asserts the emitted actions and resulting state.
 */

static ardop_link_input rx_frame(uint8_t frame_type)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = frame_type;
	return in;
}

/* Find the first action of a kind; returns NULL if absent. */
static const ardop_action *find_action(const ardop_action *acts, size_t n,
				       ardop_action_kind kind)
{
	for (size_t i = 0; i < n; i++)
		if (acts[i].kind == kind)
			return &acts[i];
	return NULL;
}

/*
 * DISC + a decoded DISCFRAME (a straggler from a previous session): answer END
 * carrying the *previous* session id, arm the closing-ID timer 3 s out, and
 * stay in DISC. Mirrors ProcessRcvdARQFrame's DISC/DISCFRAME arm (rule 1.5).
 */
static void test_disc_discframe_sends_end(void **state)
{
	(void)state;

	ardop_link l;
	ardop_link_init(&l);
	l.last_session_id = 0x5A;

	uint64_t now = 1000000;
	ardop_action acts[8];
	ardop_link_input in = rx_frame(ARDOP_FT_DISC);
	size_t n = ardop_link_step(&l, &in, now, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_END);
	assert_int_equal(send->data_len, 2);
	assert_int_equal(send->data[0], ARDOP_FT_END);
	assert_int_equal(send->data[1], ARDOP_FT_END ^ 0x5A);
	assert_int_equal(send->leader_ms, l.leader_ms);

	const ardop_action *timer = find_action(acts, n, ARDOP_ACT_SET_TIMER);
	assert_non_null(timer);
	/* 3000 ms at 12 kHz = 36000 samples. */
	assert_int_equal(timer->deadline, now + 36000);

	/* Stays disconnected. */
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* DISC + any other decoded frame (for now): no actions, still DISC. */
static void test_disc_ignores_other_frames(void **state)
{
	(void)state;

	ardop_link l;
	ardop_link_init(&l);

	uint64_t now = 500000;
	ardop_action acts[8];
	ardop_link_input in = rx_frame(ARDOP_FT_IDLE);
	size_t n = ardop_link_step(&l, &in, now, acts, 8);

	assert_int_equal(n, 0);
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* A FRAME_BAD event drives nothing in DISC. */
static void test_disc_ignores_bad_frame(void **state)
{
	(void)state;

	ardop_link l;
	ardop_link_init(&l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_BAD;
	in.as.rx.frame_type = ARDOP_FT_DISC;

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);
	assert_int_equal(n, 0);
}

/* Build a ConReq payload: caller and target station IDs, six wire bytes each. */
static void build_conreq(const char *caller, const char *target, uint8_t *out)
{
	ardop_stationid c, t;
	assert_int_equal(ardop_stationid_from_str(caller, &c),
			 ARDOP_STATIONID_OK);
	assert_int_equal(ardop_stationid_from_str(target, &t),
			 ARDOP_STATIONID_OK);
	assert_true(ardop_stationid_to_buffer(&c, out));
	assert_true(ardop_stationid_to_buffer(&t, out + ARDOP_PACKED6_SIZE));
}

/* A link listening as N0CALL, 2000 Hz max, ARQ. */
static void setup_listening(ardop_link *l)
{
	ardop_link_init(l);
	assert_int_equal(ardop_stationid_from_str("N0CALL", &l->mycall),
			 ARDOP_STATIONID_OK);
	l->bw_setting = ARDOP_ARQ_BW_2000_MAX;
	l->listening = true;
}

/*
 * DISC + a ConReq addressed to us with a compatible bandwidth: answer ConAck
 * (carrying the session id and the received-leader timing), tell the host the
 * oncoming TARGET, become IRS pending the first data, and arm the 10 s pending
 * timeout. Mirrors ProcessRcvdARQFrame's DISC/ConReq accept arm (rule 1.2).
 */
static void test_disc_conreq_accept(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "N0CALL", payload);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REQ_2000M;
	in.as.rx.data = payload;
	in.as.rx.data_len = sizeof(payload);
	in.as.rx.leader_ms = 240;

	uint64_t now = 1000000;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, now, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_CON_ACK_2000);
	/* ConAck with timing: [type][type^session][t][t][t], t = 240/10 = 24. */
	assert_int_equal(send->data_len, 5);
	uint8_t session = ardop_session_id("W1ABC", "N0CALL");
	assert_int_equal(send->data[0], ARDOP_FT_CON_ACK_2000);
	assert_int_equal(send->data[1], ARDOP_FT_CON_ACK_2000 ^ session);
	assert_int_equal(send->data[2], 24);
	assert_int_equal(send->leader_ms, l.reply_leader_ms);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "TARGET N0CALL");

	const ardop_action *timer = find_action(acts, n, ARDOP_ACT_SET_TIMER);
	assert_non_null(timer);
	assert_int_equal(timer->deadline, now + 10000 * 12);

	assert_int_equal(l.state, ARDOP_LINK_IRS_CON_ACK);
	assert_true(l.pending);
	assert_int_equal(l.session_id, session);
	assert_int_equal(l.session_bw, 2000);
}

/* ConReq to us but incompatible bandwidth: ConRejBW, REJECTEDBW, stay DISC. */
static void test_disc_conreq_reject_bw(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);
	l.bw_setting = ARDOP_ARQ_BW_200_FORCED;   /* forces 200 Hz */

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "N0CALL", payload);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REQ_2000F;  /* forced 2000 */
	in.as.rx.data = payload;
	in.as.rx.data_len = sizeof(payload);

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_CON_REJ_BW);
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "REJECTEDBW W1ABC");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* ConReq addressed to someone else: CANCELPENDING, no frame, stay DISC. */
static void test_disc_conreq_not_for_us(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "K9XYZ", payload);   /* not N0CALL */

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REQ_2000M;
	in.as.rx.data = payload;
	in.as.rx.data_len = sizeof(payload);

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);

	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "CANCELPENDING");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* A ConReq while not listening is ignored entirely. */
static void test_disc_conreq_not_listening(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);
	l.listening = false;

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "N0CALL", payload);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REQ_2000M;
	in.as.rx.data = payload;
	in.as.rx.data_len = sizeof(payload);

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);
	assert_int_equal(n, 0);
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

static ardop_link_input ping_input(const uint8_t *payload, int sn, int quality)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_PING;
	in.as.rx.data = payload;
	in.as.rx.data_len = 2 * ARDOP_PACKED6_SIZE;
	in.as.rx.sn = sn;
	in.as.rx.quality = quality;
	return in;
}

/*
 * DISC + a PING to us, PingAck enabled: reply PINGACK carrying the measured S/N
 * and quality, tell the host PINGREPLY, and arm a closing-ID timer under the
 * ping's target call. Mirrors ProcessPingFrame's ack arm.
 */
static void test_disc_ping_replies(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);
	l.enable_ping_ack = true;

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "N0CALL", payload);   /* caller>target, same layout */

	uint64_t now = 2000000;
	ardop_link_input in = ping_input(payload, 15, 80);
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, now, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_PING_ACK);
	assert_int_equal(send->data_len, 5);
	assert_int_equal(send->data[0], ARDOP_FT_PING_ACK);
	assert_int_equal(send->data[1], ARDOP_FT_PING_ACK ^ 0xFF);
	/* S/N 15 -> ((15+10)&0x1F)<<3 = 200; quality 80 -> (80-30)/10 = 5;
	 * info = 205 = 0xCD, repeated. */
	assert_int_equal(send->data[2], 0xCD);
	assert_int_equal(send->data[3], 0xCD);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "PINGREPLY");

	const ardop_action *timer = find_action(acts, n, ARDOP_ACT_SET_TIMER);
	assert_non_null(timer);
	assert_int_equal(timer->deadline, now + 3000 * 12);
	assert_int_equal(l.state, ARDOP_LINK_DISC);   /* a ping does not connect */
}

/* PING to us but PingAck disabled: no reply, just CANCELPENDING. */
static void test_disc_ping_ack_disabled(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);
	l.enable_ping_ack = false;

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "N0CALL", payload);

	ardop_link_input in = ping_input(payload, 10, 50);
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);

	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "CANCELPENDING");
}

/* PING to someone else: CANCELPENDING, no reply. */
static void test_disc_ping_not_for_us(void **state)
{
	(void)state;

	ardop_link l;
	setup_listening(&l);
	l.enable_ping_ack = true;

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "K9XYZ", payload);   /* not N0CALL */

	ardop_link_input in = ping_input(payload, 10, 50);
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);

	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "CANCELPENDING");
}

/*
 * Host CONNECT from DISC: build and send a ConReq encoding our bandwidth and the
 * callsign pair, become ISS awaiting ConAck, set the session id from the pair,
 * and arm the 2 s resend. Exercises the ARDOP_IN_HOST input path end-to-end.
 * Mirrors SendARQConnectRequest (rule 1.1).
 */
static void test_host_connect(void **state)
{
	(void)state;

	assert_true(ardop_rs_init(&g_rs, kRSLens,
				  (int)(sizeof kRSLens / sizeof kRSLens[0])));

	ardop_link l;
	ardop_link_init(&l);
	assert_int_equal(ardop_stationid_from_str("N0CALL", &l.mycall),
			 ARDOP_STATIONID_OK);
	l.bw_setting = ARDOP_ARQ_BW_2000_MAX;
	l.rs = &g_rs;

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host.kind = ARDOP_CMD_CONNECT;
	assert_int_equal(ardop_stationid_from_str("W1ABC", &in.as.host.target),
			 ARDOP_STATIONID_OK);
	in.as.host.bandwidth = ARDOP_ARQ_BW_UNDEFINED;  /* use station default */

	uint64_t now = 3000000;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, now, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_CON_REQ_2000M);
	assert_int_equal(send->data_len, 18);
	assert_int_equal(send->data[0], ARDOP_FT_CON_REQ_2000M);
	/* ConReq always goes out under session 0xFF, not the stored session. */
	assert_int_equal(send->data[1], ARDOP_FT_CON_REQ_2000M ^ 0xFF);

	/* The 12 data bytes decode back to our call then the target. */
	ardop_stationid a, b;
	assert_int_equal(ardop_stationid_from_bytes(send->data + 2, &a),
			 ARDOP_STATIONID_OK);
	assert_int_equal(ardop_stationid_from_bytes(
				 send->data + 2 + ARDOP_PACKED6_SIZE, &b),
			 ARDOP_STATIONID_OK);
	assert_string_equal(a.str, "N0CALL");
	assert_string_equal(b.str, "W1ABC");

	const ardop_action *timer = find_action(acts, n, ARDOP_ACT_SET_TIMER);
	assert_non_null(timer);
	assert_int_equal(timer->deadline, now + 2000 * 12);

	assert_int_equal(l.state, ARDOP_LINK_ISS_CON_REQ);
	assert_true(l.pending);
	assert_int_equal(l.session_id, ardop_session_id("N0CALL", "W1ABC"));
}

/* A CONNECT while already connected/connecting is ignored. */
static void test_host_connect_ignored_when_busy(void **state)
{
	(void)state;

	ardop_link l;
	ardop_link_init(&l);
	l.state = ARDOP_LINK_ISS;   /* already in a session */

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host.kind = ARDOP_CMD_CONNECT;
	assert_int_equal(ardop_stationid_from_str("W1ABC", &in.as.host.target),
			 ARDOP_STATIONID_OK);

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 1000, acts, 8);
	assert_int_equal(n, 0);
	assert_int_equal(l.state, ARDOP_LINK_ISS);
}

/* Drive a link from DISC through a CONNECT so it is ISS awaiting a ConAck. */
static void connect_as_iss(ardop_link *l)
{
	assert_true(ardop_rs_init(&g_rs, kRSLens,
				  (int)(sizeof kRSLens / sizeof kRSLens[0])));
	ardop_link_init(l);
	assert_int_equal(ardop_stationid_from_str("N0CALL", &l->mycall),
			 ARDOP_STATIONID_OK);
	l->bw_setting = ARDOP_ARQ_BW_2000_MAX;
	l->rs = &g_rs;

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host.kind = ARDOP_CMD_CONNECT;
	assert_int_equal(ardop_stationid_from_str("W1ABC", &in.as.host.target),
			 ARDOP_STATIONID_OK);
	in.as.host.bandwidth = ARDOP_ARQ_BW_UNDEFINED;

	ardop_action acts[8];
	(void)ardop_link_step(l, &in, 100, acts, 8);
	assert_int_equal(l->state, ARDOP_LINK_ISS_CON_REQ);
}

/*
 * ISS awaiting ConAck + the IRS's ConAck: adopt the negotiated width, reply with
 * our own ConAck carrying our received-leader timing (three-way handshake), and
 * move to ISS_CON_ACK connected. Mirrors the ISS/ISSConReq ConAck arm (rule 1.4).
 */
static void test_iss_conreq_gets_conack(void **state)
{
	(void)state;

	ardop_link l;
	connect_as_iss(&l);
	uint8_t session = l.session_id;

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_ACK_2000;
	in.as.rx.leader_ms = 250;

	uint64_t now = 200;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, now, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_CON_ACK_2000);
	assert_int_equal(send->data_len, 5);
	assert_int_equal(send->data[0], ARDOP_FT_CON_ACK_2000);
	assert_int_equal(send->data[1], ARDOP_FT_CON_ACK_2000 ^ session);
	assert_int_equal(send->data[2], 25);   /* 250 ms / 10 */

	assert_int_equal(l.state, ARDOP_LINK_ISS_CON_ACK);
	assert_int_equal(l.session_bw, 2000);
	assert_false(l.pending);
}

/* ISS awaiting ConAck + ConRejBusy: tell the host, abort to DISC. */
static void test_iss_conreq_rejected_busy(void **state)
{
	(void)state;

	ardop_link l;
	connect_as_iss(&l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REJ_BUSY;

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 200, acts, 8);

	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "REJECTEDBUSY W1ABC");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* ISS awaiting ConAck + ConRejBW: tell the host, abort to DISC. */
static void test_iss_conreq_rejected_bw(void **state)
{
	(void)state;

	ardop_link l;
	connect_as_iss(&l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REJ_BW;

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 200, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "REJECTEDBW W1ABC");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* Drive a listening link through a ConReq so it is IRS awaiting the ISS ConAck. */
static void accept_as_irs(ardop_link *l)
{
	setup_listening(l);

	uint8_t payload[2 * ARDOP_PACKED6_SIZE];
	build_conreq("W1ABC", "N0CALL", payload);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_REQ_2000M;
	in.as.rx.data = payload;
	in.as.rx.data_len = sizeof(payload);
	in.as.rx.leader_ms = 240;

	ardop_action acts[8];
	(void)ardop_link_step(l, &in, 1000, acts, 8);
	assert_int_equal(l->state, ARDOP_LINK_IRS_CON_ACK);
}

/*
 * IRS awaiting the ISS ConAck + that ConAck: complete the handshake -- adopt the
 * width, tell the host CONNECTED, answer with a DataACK carrying the decode
 * quality, become IRS_DATA. Mirrors the ISS-ConAck arm of the IRS/IRSConAck
 * state (rule 1.4).
 */
static void test_irs_conack_completes(void **state)
{
	(void)state;

	ardop_link l;
	accept_as_irs(&l);
	uint8_t session = l.session_id;

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_ACK_2000;
	in.as.rx.quality = 75;

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 2000, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "CONNECTED W1ABC 2000");

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	/* DataACK carrying quality 75. */
	uint8_t ack = ardop_quality_to_ack_type(75);
	assert_int_equal(send->frame_type, ack);
	assert_int_equal(send->data_len, 2);
	assert_int_equal(send->data[0], ack);
	assert_int_equal(send->data[1], ack ^ session);

	assert_int_equal(l.state, ARDOP_LINK_IRS_DATA);
	assert_false(l.pending);
	assert_int_equal(l.session_bw, 2000);
}

/* Drive a link all the way to connected IRS_DATA. */
static void connected_as_irs(ardop_link *l)
{
	accept_as_irs(l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_ACK_2000;
	in.as.rx.quality = 75;

	ardop_action acts[8];
	(void)ardop_link_step(l, &in, 2000, acts, 8);
	assert_int_equal(l->state, ARDOP_LINK_IRS_DATA);
}

static ardop_link_input data_frame(uint8_t ft, const uint8_t *payload,
				   int len, int quality)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ft;
	in.as.rx.data = payload;
	in.as.rx.data_len = len;
	in.as.rx.quality = quality;
	return in;
}

/*
 * Connected IRS + a good data frame: deliver the payload to the host and ACK
 * with the decode quality; a genuine retransmission (same frame type) is ACKed
 * again but not re-delivered; the next frame (alternated type) is delivered.
 * Mirrors the IRS/IRSData good-data arm and its intLastARQDataFrameToHost dedup.
 */
static void test_irs_data_deliver_and_dedup(void **state)
{
	(void)state;

	ardop_link l;
	connected_as_irs(&l);
	uint8_t session = l.session_id;

	const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34};

	/* First data frame (even, 0x42): delivered and ACKed. */
	ardop_link_input in = data_frame(0x42, payload, sizeof(payload), 80);
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 3000, acts, 8);

	const ardop_action *deliver = find_action(acts, n,
						  ARDOP_ACT_DELIVER_DATA);
	assert_non_null(deliver);
	assert_int_equal(deliver->data_len, sizeof(payload));
	assert_memory_equal(deliver->data, payload, sizeof(payload));

	const ardop_action *ack = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(ack);
	assert_int_equal(ack->frame_type, ardop_quality_to_ack_type(80));
	assert_int_equal(ack->data[1], ardop_quality_to_ack_type(80) ^ session);

	/* Same frame again (ISS missed our ACK): re-ACK, do NOT re-deliver. */
	in = data_frame(0x42, payload, sizeof(payload), 78);
	n = ardop_link_step(&l, &in, 4000, acts, 8);
	assert_null(find_action(acts, n, ARDOP_ACT_DELIVER_DATA));
	assert_non_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));

	/* The next frame (odd, 0x43): delivered again. */
	const uint8_t payload2[] = {0x55, 0x66};
	in = data_frame(0x43, payload2, sizeof(payload2), 82);
	n = ardop_link_step(&l, &in, 5000, acts, 8);
	deliver = find_action(acts, n, ARDOP_ACT_DELIVER_DATA);
	assert_non_null(deliver);
	assert_int_equal(deliver->data_len, sizeof(payload2));
	assert_memory_equal(deliver->data, payload2, sizeof(payload2));
}

/* Connected IRS + a failed data decode (new frame): NAK with quality. */
static void test_irs_data_nak(void **state)
{
	(void)state;

	ardop_link l;
	connected_as_irs(&l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_BAD;
	in.as.rx.frame_type = 0x42;
	in.as.rx.quality = 40;

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 3000, acts, 8);

	assert_null(find_action(acts, n, ARDOP_ACT_DELIVER_DATA));
	const ardop_action *nak = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(nak);
	assert_int_equal(nak->frame_type, ardop_quality_to_nak_type(40));
}

/* Connected IRS + DISC from the ISS: tell the host, answer END, return to DISC. */
static void test_irs_data_disc(void **state)
{
	(void)state;

	ardop_link l;
	connected_as_irs(&l);
	uint8_t session = l.session_id;

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_DISC;

	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 6000, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "DISCONNECTED");
	const ardop_action *end = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(end);
	assert_int_equal(end->frame_type, ARDOP_FT_END);
	assert_int_equal(end->data[1], ARDOP_FT_END ^ session);
	assert_int_equal(l.state, ARDOP_LINK_DISC);
	assert_int_equal(l.last_session_id, session);
}

static uint8_t g_tx[4096];

/* Drive a link to ISS_CON_ACK (connected, awaiting the IRS's DataACK), with a
 * TX queue attached. */
static void iss_to_con_ack(ardop_link *l)
{
	connect_as_iss(l);   /* -> ISS_CON_REQ, rs + mycall + 2000-max set */
	l->tx_data = g_tx;
	l->tx_cap = sizeof(g_tx);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_ACK_2000;
	in.as.rx.leader_ms = 240;
	ardop_action acts[8];
	(void)ardop_link_step(l, &in, 200, acts, 8);
	assert_int_equal(l->state, ARDOP_LINK_ISS_CON_ACK);
}

static ardop_link_input host_send(const uint8_t *data, size_t len)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host.kind = ARDOP_CMD_SEND_DATA;
	in.as.host.data = data;
	in.as.host.data_len = len;
	return in;
}

/* Host SEND_DATA appends to the queue and reports the new depth. */
static void test_host_send_data_queues(void **state)
{
	(void)state;

	ardop_link l;
	iss_to_con_ack(&l);

	const uint8_t data[] = {1, 2, 3, 4, 5};
	ardop_link_input in = host_send(data, sizeof(data));
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 300, acts, 8);

	assert_int_equal(l.tx_len, 5);
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "BUFFER 5");
}

/*
 * A field bug (found on the same captured on-air session as
 * test_break_disarms_stale_repeat, once that collision was fixed and a real
 * exchange got through): connected IRS holding queued host data, hearing the
 * ISS go idle, must BREAK to reclaim the link on its own -- AUTOBREAK
 * defaults TRUE in the reference (ARQ.c: `BOOL AutoBreak = TRUE`), and a
 * real client is entitled to rely on that documented default rather than
 * set it explicitly. ardop_link_init left auto_break at its zero-initialised
 * false, so queued data just sat there, ACKing the far end's IDLE forever.
 * No AUTOBREAK command appears anywhere in this test, deliberately: it
 * proves the default, not the host command (see test_host.c for that).
 */
static void test_irs_data_auto_break_reclaims_link(void **state)
{
	(void)state;

	ardop_link l;
	connected_as_irs(&l);
	l.tx_data = g_tx;
	l.tx_cap = sizeof(g_tx);

	const uint8_t data[] = {1, 2, 3, 4};
	ardop_link_input q = host_send(data, sizeof(data));
	ardop_action acts[8];
	(void)ardop_link_step(&l, &q, 3000, acts, 8);
	assert_int_equal(l.tx_len, sizeof(data));

	ardop_link_input in = rx_frame(ARDOP_FT_IDLE);
	size_t n = ardop_link_step(&l, &in, 4000, acts, 8);

	const ardop_action *brk = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(brk);
	assert_int_equal(brk->frame_type, ARDOP_FT_BREAK);
	assert_int_equal(l.state, ARDOP_LINK_IRS_TO_ISS);
}

/*
 * ISS awaiting the IRS DataACK, with data queued: on the DataACK, tell the host
 * CONNECTED and send the first data frame at the most-robust mode for the
 * bandwidth, even parity (last acked seeded odd). Mirrors the ISS ConAck+DataACK
 * arm feeding the first GetNextFrameData send.
 */
static void test_iss_sends_first_data(void **state)
{
	(void)state;

	ardop_link l;
	iss_to_con_ack(&l);
	uint8_t session = l.session_id;

	/* Queue some payload. */
	const uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
	ardop_link_input q = host_send(payload, sizeof(payload));
	ardop_action acts[8];
	(void)ardop_link_step(&l, &q, 300, acts, 8);

	/* The IRS's DataACK completes the handshake. */
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ardop_quality_to_ack_type(90);   /* a DataACK */
	size_t n = ardop_link_step(&l, &in, 400, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "CONNECTED W1ABC 2000");

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	/* 2000-Hz ladder, most robust first = 0x4C; even (base is even, seed odd). */
	assert_int_equal(send->frame_type, 0x4C);
	assert_int_equal(send->data[0], 0x4C);
	assert_int_equal(send->data[1], 0x4C ^ session);
	assert_true(send->data_len > 2);
	assert_int_equal(l.state, ARDOP_LINK_ISS);
	assert_int_equal(l.outstanding_len, sizeof(payload));
}

/* ISS DataACK with an empty queue: send IDLE, go IDLE. */
static void test_iss_idle_when_empty(void **state)
{
	(void)state;

	ardop_link l;
	iss_to_con_ack(&l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ardop_quality_to_ack_type(90);
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 400, acts, 8);

	const ardop_action *send = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_IDLE);
	assert_int_equal(l.state, ARDOP_LINK_IDLE);
}

static ardop_link_input dataack(int quality)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ardop_quality_to_ack_type(quality);
	return in;
}

static ardop_link_input datanak(int quality)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ardop_quality_to_nak_type(quality);
	return in;
}

/*
 * A field bug (found via a captured on-air session, VA7DEP retrying a data
 * frame that kept failing): idling with nothing to send arms a repeat timer
 * for the IDLE keep-alive (arm_repeat, per test_iss_idle_when_empty above).
 * If the IRS then BREAKs to take the link, that timer must not survive the
 * handover -- the IRS role only ever sends response frames, which never arm
 * one (arm_repeat's own doc comment) -- or step_timer keeps firing on the
 * old deadline and resends the stale IDLE, transmitting on top of whatever
 * the new ISS is sending. Confirmed by the same symptom recurring on
 * successive real ARQ retries, all failing, until the on-air session was
 * killed by hand.
 */
static void test_break_disarms_stale_repeat(void **state)
{
	(void)state;

	ardop_link l;
	iss_to_con_ack(&l);

	/* Empties out to IDLE, arming a 2000 ms repeat for the IDLE it sends. */
	ardop_action acts[8];
	ardop_link_input in = dataack(90);
	size_t n = ardop_link_step(&l, &in, 400, acts, 8);
	assert_int_equal(l.state, ARDOP_LINK_IDLE);
	assert_non_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	assert_true(l.repeat_deadline > 0);

	/* The IRS BREAKs: we yield the link. */
	in = rx_frame(ARDOP_FT_BREAK);
	n = ardop_link_step(&l, &in, 500, acts, 8);
	assert_int_equal(l.state, ARDOP_LINK_IRS_FROM_ISS);
	assert_int_equal(l.repeat_deadline, 0);

	/* Well past the deadline the stale timer would have fired on: nothing
	 * is sent, because there is no repeat armed any more. */
	ardop_link_input none = {0};
	none.kind = ARDOP_IN_NONE;
	n = ardop_link_step(&l, &none, 30000, acts, 8);
	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
}

/* Reach connected ISS with `len` payload queued and the first frame sent. */
static void iss_sending(ardop_link *l, size_t len)
{
	iss_to_con_ack(l);
	static uint8_t big[2048];
	for (size_t i = 0; i < len; i++)
		big[i] = (uint8_t)i;
	ardop_link_input q = host_send(big, len);
	ardop_action acts[8];
	(void)ardop_link_step(l, &q, 300, acts, 8);
	/* First DataACK completes the handshake and sends the first frame. */
	ardop_link_input ack = dataack(90);
	(void)ardop_link_step(l, &ack, 400, acts, 8);
	assert_int_equal(l->state, ARDOP_LINK_ISS);
}

/*
 * Connected ISS draining the queue: each DataACK confirms the outstanding frame
 * (dropping its bytes), alternates the even/odd frame type, and sends the next;
 * when the queue empties the ISS falls to IDLE. Mode 0 for 2000 Hz is 0x4C, a
 * 32-byte frame, so 80 bytes take three frames (the last is short).
 */
static void test_iss_data_drain(void **state)
{
	(void)state;

	ardop_link l;
	iss_sending(&l, 80);   /* first frame 0x4C (32 B) sent; tx_len 80. */
	assert_int_equal(l.last_data_sent, 0x4C);
	assert_int_equal(l.outstanding_len, 32);

	ardop_action acts[8];

	/* ACK #1: drop 32 -> 48 left, send 0x4D (toggled). The queue depth is
	 * also pushed to the host here -- ARDOPC.c's RemoveDataFromQueue does
	 * this on every drain ("called when ACK received, or on FEC send"),
	 * not just on enqueue; without it a client's send-progress display
	 * only ever sees the size once and looks stalled through the rest of
	 * the send (a field symptom: Pat's progress sitting at 0%). */
	ardop_link_input in = dataack(90);
	size_t n = ardop_link_step(&l, &in, 500, acts, 8);
	assert_int_equal(l.tx_len, 48);
	const ardop_action *s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);
	assert_int_equal(s->frame_type, 0x4D);
	const ardop_action *buf = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(buf);
	assert_string_equal((const char *)buf->data, "BUFFER 48");

	/* ACK #2: drop 32 -> 16 left, send 0x4C, carrying the last 16 bytes. */
	in = dataack(90);
	n = ardop_link_step(&l, &in, 600, acts, 8);
	assert_int_equal(l.tx_len, 16);
	s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_int_equal(s->frame_type, 0x4C);
	assert_int_equal(l.outstanding_len, 16);

	/* ACK #3: drop 16 -> empty, IDLE. */
	in = dataack(90);
	n = ardop_link_step(&l, &in, 700, acts, 8);
	assert_int_equal(l.tx_len, 0);
	s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_int_equal(s->frame_type, ARDOP_FT_IDLE);
	assert_int_equal(l.state, ARDOP_LINK_IDLE);
}

/*
 * Gear-shift up then down. With plenty of data queued and high-quality ACKs the
 * ISS shifts up from mode 0 (0x4C) to mode 1 (0x4A) once it has >= 2 ACKs; a
 * subsequent NAK on the freshly-entered mode (which has never worked, so one NAK
 * suffices) shifts back down to mode 0.
 */
static void test_iss_gearshift(void **state)
{
	(void)state;

	ardop_link l;
	iss_sending(&l, 500);   /* first frame at mode 0. */
	assert_int_equal(l.mode_ptr, 0);

	ardop_action acts[8];

	/* ACK #1 (ack_ctr becomes 1): not enough to shift up yet. */
	ardop_link_input in = dataack(90);
	(void)ardop_link_step(&l, &in, 500, acts, 8);
	assert_int_equal(l.mode_ptr, 0);

	/* ACK #2 (ack_ctr 2, quality above threshold, data remains): shift up. */
	in = dataack(90);
	size_t n = ardop_link_step(&l, &in, 600, acts, 8);
	assert_int_equal(l.mode_ptr, 1);
	const ardop_action *s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);
	assert_int_equal(s->frame_type, 0x4A);   /* mode 1. */

	/* NAK on mode 1 (never worked -> DownNAKS = 1): shift back to mode 0. */
	in = datanak(40);
	n = ardop_link_step(&l, &in, 700, acts, 8);
	assert_int_equal(l.mode_ptr, 0);
	s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);   /* a shift resends immediately. */
	assert_int_equal(s->frame_type, 0x4C);   /* back at mode 0. */
}

/*
 * The repeat timer: while awaiting a response, a NONE step before the deadline
 * does nothing; once the deadline passes it resends the outstanding frame (here
 * the ConReq) and re-arms. Mirrors the CheckTimers repeat path.
 */
static void test_timer_repeat_resends(void **state)
{
	(void)state;

	ardop_link l;
	connect_as_iss(&l);   /* ISS_CON_REQ; ConReq (0x34) armed at now 100. */

	ardop_link_input none = {0};
	none.kind = ARDOP_IN_NONE;
	ardop_action acts[8];

	/* Well before the 2 s repeat deadline: nothing. */
	size_t n = ardop_link_step(&l, &none, 1000, acts, 8);
	assert_int_equal(n, 0);

	/* Past the deadline (2000 ms = 24000 samples after now 100): resend. */
	n = ardop_link_step(&l, &none, 100 + 24000, acts, 8);
	const ardop_action *s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);
	assert_int_equal(s->frame_type, ARDOP_FT_CON_REQ_2000M);
}

/* A response stops the resend re-arming on the old frame: after the ConReq is
 * answered the repeat tracks the new (ConAck) frame, not the ConReq. */
static void test_timer_repeat_follows_latest(void **state)
{
	(void)state;

	ardop_link l;
	connect_as_iss(&l);

	/* The IRS answers: ISS replies its own ConAck and re-arms on that. */
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_CON_ACK_2000;
	in.as.rx.leader_ms = 240;
	ardop_action acts[8];
	(void)ardop_link_step(&l, &in, 500, acts, 8);
	assert_int_equal(l.repeat_frame_type, ARDOP_FT_CON_ACK_2000);
}

/*
 * The pending timeout: an IRS that answered a ConReq but never saw the ISS
 * complete the handshake gives up after 10 s -- DISCONNECTED, back to DISC.
 * Mirrors tmrIRSPendingTimeout.
 */
static void test_timer_pending_timeout(void **state)
{
	(void)state;

	ardop_link l;
	accept_as_irs(&l);   /* IRS_CON_ACK; pending deadline set at now 1000. */

	ardop_link_input none = {0};
	none.kind = ARDOP_IN_NONE;
	ardop_action acts[8];

	/* 10 s = 120000 samples after now 1000. */
	size_t n = ardop_link_step(&l, &none, 1000 + 120000, acts, 8);
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "DISCONNECTED");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* Drive a connected ISS to IDLE (queue empty). */
static void iss_idle(ardop_link *l)
{
	iss_to_con_ack(l);
	ardop_link_input ack = dataack(90);
	ardop_action acts[8];
	(void)ardop_link_step(l, &ack, 400, acts, 8);
	assert_int_equal(l->state, ARDOP_LINK_IDLE);
}

/*
 * Idling: a DataACK to an IDLE with nothing queued keeps idling (no data frame);
 * once the host queues data, the next DataACK resumes sending. Mirrors the IDLE
 * arm's "ACK with data -> ISS Data" path.
 */
static void test_idle_resumes_on_data(void **state)
{
	(void)state;

	ardop_link l;
	iss_idle(&l);

	ardop_action acts[8];

	/* ACK while still empty: keep idling, send nothing new. */
	ardop_link_input ack = dataack(90);
	size_t n = ardop_link_step(&l, &ack, 500, acts, 8);
	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	assert_int_equal(l.state, ARDOP_LINK_IDLE);

	/* Host queues data. */
	const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
	ardop_link_input q = host_send(data, sizeof(data));
	(void)ardop_link_step(&l, &q, 600, acts, 8);

	/* Next ACK resumes sending a data frame. */
	ack = dataack(90);
	n = ardop_link_step(&l, &ack, 700, acts, 8);
	const ardop_action *s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);
	assert_int_equal(s->frame_type, 0x4C);   /* mode 0 data frame. */
	assert_int_equal(l.state, ARDOP_LINK_ISS);
}

/* Idling + DISC: disconnect (DISCONNECTED + END, back to DISC). */
static void test_idle_disc(void **state)
{
	(void)state;

	ardop_link l;
	iss_idle(&l);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_DISC;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 800, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "DISCONNECTED");
	const ardop_action *end = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(end);
	assert_int_equal(end->frame_type, ARDOP_FT_END);
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/*
 * Host DISCONNECT while connected: announce it and send DISC; when the peer
 * answers END, drop to DISC and tell the host DISCONNECTED. Mirrors
 * CheckForDisconnect + the END teardown.
 */
static void test_host_disconnect(void **state)
{
	(void)state;

	ardop_link l;
	iss_sending(&l, 40);   /* connected ISS. */

	ardop_link_input dc = {0};
	dc.kind = ARDOP_IN_HOST;
	dc.as.host.kind = ARDOP_CMD_DISCONNECT;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &dc, 1000, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data,
			    "STATUS INITIATING ARQ DISCONNECT");
	const ardop_action *s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);
	assert_int_equal(s->frame_type, ARDOP_FT_DISC);

	/* Peer answers END. */
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_RX;
	in.as.rx.kind = ARDOP_EV_FRAME_DECODED;
	in.as.rx.frame_type = ARDOP_FT_END;
	n = ardop_link_step(&l, &in, 2000, acts, 8);
	notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "DISCONNECTED");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* DISCONNECT while already disconnected is ignored. */
static void test_host_disconnect_ignored(void **state)
{
	(void)state;

	ardop_link l;
	ardop_link_init(&l);

	ardop_link_input dc = {0};
	dc.kind = ARDOP_IN_HOST;
	dc.as.host.kind = ARDOP_CMD_DISCONNECT;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &dc, 1000, acts, 8);

	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "DISCONNECT IGNORED");
}

/* A repeated DISC gives up (forces the disconnect) after five tries. */
static void test_host_disconnect_gives_up(void **state)
{
	(void)state;

	ardop_link l;
	iss_sending(&l, 40);

	ardop_link_input dc = {0};
	dc.kind = ARDOP_IN_HOST;
	dc.as.host.kind = ARDOP_CMD_DISCONNECT;
	ardop_action acts[8];
	(void)ardop_link_step(&l, &dc, 1000, acts, 8);

	/* Fire the repeat timer repeatedly; the fifth firing forces disconnect. */
	ardop_link_input none = {0};
	none.kind = ARDOP_IN_NONE;
	uint64_t t = 1000;
	int forced = 0;
	for (int i = 0; i < 6; i++) {
		t += 24000 + 1;   /* past each 2 s repeat interval. */
		size_t n = ardop_link_step(&l, &none, t, acts, 8);
		const ardop_action *notify =
			find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
		if (notify && strcmp((const char *)notify->data,
				     "DISCONNECTED") == 0) {
			forced = 1;
			break;
		}
	}
	assert_true(forced);
	assert_int_equal(l.state, ARDOP_LINK_DISC);
}

/* Host ABORT drops the link immediately and clears the queue. */
static void test_host_abort(void **state)
{
	(void)state;

	ardop_link l;
	iss_sending(&l, 100);
	assert_true(l.tx_len > 0);

	ardop_link_input ab = {0};
	ab.kind = ARDOP_IN_HOST;
	ab.as.host.kind = ARDOP_CMD_ABORT;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &ab, 1000, acts, 8);

	const ardop_action *notify = find_action(acts, n, ARDOP_ACT_NOTIFY_HOST);
	assert_non_null(notify);
	assert_string_equal((const char *)notify->data, "ABORT");
	assert_int_equal(l.state, ARDOP_LINK_DISC);
	assert_int_equal(l.tx_len, 0);
}

/*
 * Receive-only mode: SET_MODE switches it on while disconnected; then every
 * decoded frame's content is delivered to the host with a status line, and the
 * link never transmits (no ACK, no anything). Mirrors ProcessRXOFrame.
 */
static void test_rxo_mode(void **state)
{
	(void)state;

	ardop_link l;
	ardop_link_init(&l);

	ardop_link_input sm = {0};
	sm.kind = ARDOP_IN_HOST;
	sm.as.host.kind = ARDOP_CMD_SET_MODE;
	sm.as.host.arg = ARDOP_MODE_RXO;
	ardop_action acts[8];
	(void)ardop_link_step(&l, &sm, 0, acts, 8);
	assert_int_equal(l.mode, ARDOP_MODE_RXO);

	/* A data frame is delivered to the host, and nothing is transmitted. */
	const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
	ardop_link_input in = data_frame(0x42, payload, sizeof(payload), 80);
	size_t n = ardop_link_step(&l, &in, 100, acts, 8);

	const ardop_action *d = find_action(acts, n, ARDOP_ACT_DELIVER_DATA);
	assert_non_null(d);
	assert_int_equal(d->data_len, sizeof(payload));
	assert_memory_equal(d->data, payload, sizeof(payload));
	assert_null(find_action(acts, n, ARDOP_ACT_SEND_FRAME));
	assert_int_equal(l.state, ARDOP_LINK_DISC);   /* never leaves DISC. */
}

/*
 * Host SEND_ID transmits a station ID frame carrying our callsign and grid,
 * RS-protected; and the closing-ID timer fires an ID (under the session's call)
 * after a disconnect's hold expires. Mirrors EncodeARQIDFrame/SendID + tmrFinalID.
 */
static void test_send_id(void **state)
{
	(void)state;

	assert_true(ardop_rs_init(&g_rs, kRSLens,
				  (int)(sizeof kRSLens / sizeof kRSLens[0])));
	ardop_link l;
	ardop_link_init(&l);
	l.rs = &g_rs;
	assert_int_equal(ardop_stationid_from_str("N0CALL", &l.mycall),
			 ARDOP_STATIONID_OK);
	assert_int_equal(ardop_locator_from_str("EM79vr", &l.grid),
			 0);

	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host.kind = ARDOP_CMD_SEND_ID;
	ardop_action acts[8];
	size_t n = ardop_link_step(&l, &in, 0, acts, 8);

	const ardop_action *s = find_action(acts, n, ARDOP_ACT_SEND_FRAME);
	assert_non_null(s);
	assert_int_equal(s->frame_type, ARDOP_FT_ID);
	assert_int_equal(s->data_len, 18);
	assert_int_equal(s->data[0], ARDOP_FT_ID);
	assert_int_equal(s->data[1], ARDOP_FT_ID ^ 0xFF);

	/* The 12 data bytes are our callsign then our grid. */
	ardop_stationid call;
	ardop_locator grid;
	assert_int_equal(ardop_stationid_from_bytes(s->data + 2, &call),
			 ARDOP_STATIONID_OK);
	assert_string_equal(call.str, "N0CALL");
	assert_int_equal(ardop_locator_from_bytes(s->data + 2 + ARDOP_PACKED6_SIZE,
						  &grid), 0);
	assert_string_equal(grid.grid, "EM79vr");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_disc_discframe_sends_end),
		cmocka_unit_test(test_disc_ignores_other_frames),
		cmocka_unit_test(test_disc_ignores_bad_frame),
		cmocka_unit_test(test_disc_conreq_accept),
		cmocka_unit_test(test_disc_conreq_reject_bw),
		cmocka_unit_test(test_disc_conreq_not_for_us),
		cmocka_unit_test(test_disc_conreq_not_listening),
		cmocka_unit_test(test_disc_ping_replies),
		cmocka_unit_test(test_disc_ping_ack_disabled),
		cmocka_unit_test(test_disc_ping_not_for_us),
		cmocka_unit_test(test_host_connect),
		cmocka_unit_test(test_host_connect_ignored_when_busy),
		cmocka_unit_test(test_iss_conreq_gets_conack),
		cmocka_unit_test(test_iss_conreq_rejected_busy),
		cmocka_unit_test(test_iss_conreq_rejected_bw),
		cmocka_unit_test(test_irs_conack_completes),
		cmocka_unit_test(test_irs_data_deliver_and_dedup),
		cmocka_unit_test(test_irs_data_nak),
		cmocka_unit_test(test_irs_data_disc),
		cmocka_unit_test(test_host_send_data_queues),
		cmocka_unit_test(test_irs_data_auto_break_reclaims_link),
		cmocka_unit_test(test_iss_sends_first_data),
		cmocka_unit_test(test_iss_idle_when_empty),
		cmocka_unit_test(test_break_disarms_stale_repeat),
		cmocka_unit_test(test_iss_data_drain),
		cmocka_unit_test(test_iss_gearshift),
		cmocka_unit_test(test_timer_repeat_resends),
		cmocka_unit_test(test_timer_repeat_follows_latest),
		cmocka_unit_test(test_timer_pending_timeout),
		cmocka_unit_test(test_idle_resumes_on_data),
		cmocka_unit_test(test_idle_disc),
		cmocka_unit_test(test_host_disconnect),
		cmocka_unit_test(test_host_disconnect_ignored),
		cmocka_unit_test(test_host_disconnect_gives_up),
		cmocka_unit_test(test_host_abort),
		cmocka_unit_test(test_rxo_mode),
		cmocka_unit_test(test_send_id),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
