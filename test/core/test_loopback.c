#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/link.h"
#include "modem/modulate.h"
#include "modem/demodulate.h"
#include "codec/frame.h"
#include "codec/rs.h"
#include "codec/stationid.h"

/*
 * The prize: two ardop_link instances wired to each other through the real
 * modulator and demodulator. One station's SEND_FRAME action is modulated to
 * samples, pushed through the other station's demodulator, and the resulting
 * FRAME_DECODED events are stepped into the other station's link -- exactly the
 * signal chain a radio would carry, but in-process and deterministic. This is
 * analysis/10 sec.9 step 3b, and it exercises codec + modem + link composing
 * end to end in a way no single-layer test can.
 *
 * The demodulators run in receive-only (session-independent) mode, so a clean
 * two-station channel needs no session-key threading between link and demod.
 */

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

/* Candidate frame types for the receive-only frame-type decode: every byte the
 * core frame table names (the inherited bytValidFrameTypesALL set). */
static uint8_t g_valid_types[256];
static int g_valid_len;

static void build_valid_types(void)
{
	if (g_valid_len)
		return;
	for (int b = 0; b < 256; b++)
		if (ardop_frame_spec_for((uint8_t)b))
			g_valid_types[g_valid_len++] = (uint8_t)b;
}

struct station {
	ardop_link link;
	ardop_demod demod;
	ardop_rs rs;
	uint8_t tx[4096];
};

static void station_init(struct station *s, const char *call, bool listening)
{
	assert_true(ardop_rs_init(&s->rs, kRSLens, NUM_RSLENS));

	ardop_link_init(&s->link);
	assert_int_equal(ardop_stationid_from_str(call, &s->link.mycall),
			 ARDOP_STATIONID_OK);
	s->link.bw_setting = ARDOP_ARQ_BW_2000_MAX;
	s->link.rs = &s->rs;
	s->link.tx_data = s->tx;
	s->link.tx_cap = sizeof(s->tx);
	s->link.listening = listening;

	build_valid_types();
	ardop_demod_init(&s->demod, 100, 5);
	s->demod.rs = &s->rs;
	s->demod.ft_ctx.valid_types = g_valid_types;
	s->demod.ft_ctx.valid_len = g_valid_len;
	s->demod.ft_ctx.rxo = true;
}

/* A shared elapsed-sample clock for both stations' demods. */
static uint64_t g_now = 1000000;

/*
 * Modulate one SEND_FRAME action and push it through @p rx's demodulator,
 * stepping @p rx's link with each decoded frame. Returns @p rx's resulting
 * actions. This is the "over the air" hop for one frame.
 */
static size_t hop(struct station *rx, const ardop_action *send,
		  ardop_action *out, size_t max_out)
{
	static int16_t buf[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod mod;

	ardop_mod_init(&mod, 30);
	assert_true(ardop_mod_begin(&mod, send->frame_type, send->data,
				    send->data_len, send->leader_ms, buf,
				    ARDOP_MOD_MAX_SAMPLES));
	size_t nsamp = ardop_mod_pull(&mod, buf, ARDOP_MOD_MAX_SAMPLES);

	/* Trailing silence flushes the streaming demod's look-ahead. */
	size_t pad = nsamp + 4800;
	assert_true(pad <= ARDOP_MOD_MAX_SAMPLES);
	for (size_t i = nsamp; i < pad; i++)
		buf[i] = 0;

	size_t n_out = 0;
	for (size_t off = 0; off < pad; off += 1200) {
		size_t chunk = pad - off < 1200 ? pad - off : 1200;
		ardop_event evs[8];
		size_t ne = ardop_demod_push(&rx->demod, &buf[off], chunk,
					     g_now, evs, 8);
		g_now += chunk;
		for (size_t e = 0; e < ne; e++) {
			if (evs[e].kind == ARDOP_EV_LEADER_DETECTED)
				continue;
			ardop_link_input in = {0};
			in.kind = ARDOP_IN_RX;
			in.as.rx = evs[e];
			n_out += ardop_link_step(&rx->link, &in, g_now,
						 &out[n_out], max_out - n_out);
		}
	}
	return n_out;
}

/* Find the (single) SEND_FRAME in an action list, or NULL. */
static const ardop_action *find_send(const ardop_action *acts, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (acts[i].kind == ARDOP_ACT_SEND_FRAME)
			return &acts[i];
	return NULL;
}

/*
 * Run the four-message handshake so @p a is the connected ISS (with an empty
 * queue, so it has fallen to IDLE) and @p b is the connected IRS. Returns @p a's
 * last SEND_FRAME (the IDLE keep-alive).
 */
static const ardop_action *establish(struct station *a, struct station *b,
				     ardop_action *acts, size_t max)
{
	ardop_link_input cmd = {0};
	cmd.kind = ARDOP_IN_HOST;
	cmd.as.host.kind = ARDOP_CMD_CONNECT;
	assert_int_equal(ardop_stationid_from_str(b->link.mycall.str,
						  &cmd.as.host.target),
			 ARDOP_STATIONID_OK);
	cmd.as.host.bandwidth = ARDOP_ARQ_BW_UNDEFINED;
	size_t n = ardop_link_step(&a->link, &cmd, g_now, acts, max);
	const ardop_action *send = find_send(acts, n);

	n = hop(b, send, acts, max);          /* ConReq -> B: ConAck */
	send = find_send(acts, n);
	n = hop(a, send, acts, max);          /* ConAck -> A: ConAck */
	send = find_send(acts, n);
	n = hop(b, send, acts, max);          /* ConAck -> B: DataACK */
	send = find_send(acts, n);
	n = hop(a, send, acts, max);          /* DataACK -> A: IDLE (no data) */
	send = find_send(acts, n);

	assert_int_equal(a->link.state, ARDOP_LINK_IDLE);
	assert_int_equal(b->link.state, ARDOP_LINK_IRS_DATA);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_IDLE);
	return send;
}

static ardop_link_input host_send_data(const uint8_t *data, size_t len)
{
	ardop_link_input in = {0};
	in.kind = ARDOP_IN_HOST;
	in.as.host.kind = ARDOP_CMD_SEND_DATA;
	in.as.host.data = data;
	in.as.host.data_len = len;
	return in;
}

/*
 * Link turnover over the loopback. A is the connected ISS (idle); B, the IRS,
 * has data and AutoBreak. When A's IDLE reaches B, B BREAKs to take the link; A
 * yields; B becomes ISS and sends its data, which A -- now the IRS -- receives.
 * Exercises the IRStoISS/IRSfromISS handover analysis/02 flagged, end to end.
 */
static void test_loopback_turnover(void **state)
{
	(void)state;

	static struct station a, b;
	station_init(&a, "N0AAA", false);
	station_init(&b, "N0BBB", true);

	ardop_action acts[16];
	const ardop_action *send = establish(&a, &b, acts, 16);

	/* B queues data and will break automatically. */
	b.link.auto_break = true;
	const uint8_t reply[] = "Now I have something to say.";
	int rlen = (int)sizeof(reply) - 1;
	ardop_link_input q = host_send_data(reply, (size_t)rlen);
	(void)ardop_link_step(&b.link, &q, g_now, acts, 16);

	/* A's IDLE -> B: B breaks and enters IRS_TO_ISS. */
	size_t n = hop(&b, send, acts, 16);
	assert_int_equal(b.link.state, ARDOP_LINK_IRS_TO_ISS);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_BREAK);

	/* B's BREAK -> A: A yields the link (ACK) and settles into IRS_FROM_ISS. */
	n = hop(&a, send, acts, 16);
	assert_int_equal(a.link.state, ARDOP_LINK_IRS_FROM_ISS);
	send = find_send(acts, n);
	assert_non_null(send);   /* the DataACK. */

	/* A's ACK -> B: turnover complete, B is ISS and sends its data. */
	n = hop(&b, send, acts, 16);
	assert_int_equal(b.link.state, ARDOP_LINK_ISS);
	send = find_send(acts, n);
	assert_non_null(send);

	/* B's data -> A: A (now IRS) delivers it and ACKs; the handover settled. */
	n = hop(&a, send, acts, 16);
	const ardop_action *deliver = NULL;
	for (size_t i = 0; i < n; i++)
		if (acts[i].kind == ARDOP_ACT_DELIVER_DATA)
			deliver = &acts[i];
	assert_non_null(deliver);
	assert_int_equal(deliver->data_len, rlen);
	assert_memory_equal(deliver->data, reply, (size_t)rlen);
	assert_int_equal(a.link.state, ARDOP_LINK_IRS_DATA);
}

/*
 * Nothing is lost across a turnover, and nothing arrives twice.
 *
 * This is the property analysis/17 amendment 7 found broken by driving the
 * application protocol over the real link. It is asserted here instead, at the
 * link, with no protocol on top: A sends a block big enough to need many data
 * frames, B has a reply queued so AutoBreak turns the link over in the middle,
 * and afterwards the concatenation of everything B delivered to its host must
 * equal exactly what A queued.
 *
 * The failure mode it exists to catch is silent in every other test: the IRS
 * de-duplicates on the parity of the data frame type, and if the two ends'
 * toggles fall out of step it drops a genuinely new frame with no NAK, no fault
 * and no counter, while the sender sees it ACKed.
 */
static void test_loopback_turnover_loses_nothing(void **state)
{
	(void)state;

	static struct station a, b;
	station_init(&a, "N0AAA", false);
	station_init(&b, "N0BBB", true);

	ardop_action acts[16];
	const ardop_action *send = establish(&a, &b, acts, 16);

	a.link.auto_break = true;
	b.link.auto_break = true;

	/* Big enough to need many frames as the gear-shift walks up the data
	 * rates, and patterned so a mis-ordered or repeated frame is visible
	 * rather than merely a length mismatch. Sized to the station's queue:
	 * the link truncates a longer one on enqueue, which would make this a
	 * test of the harness. */
	static uint8_t payload[sizeof a.tx];
	for (size_t i = 0; i < sizeof payload; i++)
		payload[i] = (uint8_t)(i * 31u + (i >> 8) * 7u);

	ardop_link_input q = host_send_data(payload, sizeof payload);
	(void)ardop_link_step(&a.link, &q, g_now, acts, 16);

	/* B has something to say, which is what makes AutoBreak fire -- and it
	 * keeps having something to say, so the link turns over repeatedly
	 * *during* A's transfer. One turnover is the easy case; the application
	 * protocol produces several, because the receiver acknowledges at its
	 * own level while the sender is still sending. */
	static const uint8_t reply[] = "roger, sending now";
	q = host_send_data(reply, sizeof reply - 1);
	(void)ardop_link_step(&b.link, &q, g_now, acts, 16);
	int replies_left = 4;

	/* Follow the single frame in flight, whichever way it is going, and
	 * collect everything either station hands to its host. */
	static uint8_t got_at_b[sizeof payload * 2];
	size_t got_len = 0;
	struct station *rx = &b;

	for (int hops = 0; hops < 2000 && send; hops++) {
		size_t n = hop(rx, send, acts, 16);

		for (size_t i = 0; i < n; i++) {
			if (acts[i].kind != ARDOP_ACT_DELIVER_DATA)
				continue;
			if (rx != &b)
				continue;   /* B's reply to A; not under test */
			assert_true(got_len + acts[i].data_len <=
				    sizeof got_at_b);
			memcpy(got_at_b + got_len, acts[i].data,
			       acts[i].data_len);
			got_len += acts[i].data_len;
		}

		send = find_send(acts, n);
		rx = (rx == &a) ? &b : &a;

		/* Keep B wanting the link until we have had several turnovers. */
		if (b.link.tx_len == 0 && replies_left > 0 &&
		    got_len > 0 && got_len < sizeof payload) {
			replies_left--;
			ardop_link_input again =
				host_send_data(reply, sizeof reply - 1);
			(void)ardop_link_step(&b.link, &again, g_now, acts, 16);
		}

		if (a.link.tx_len == 0 && b.link.tx_len == 0 &&
		    got_len >= sizeof payload)
			break;
	}

	assert_int_equal(got_len, sizeof payload);
	assert_memory_equal(got_at_b, payload, sizeof payload);
}

/*
 * A full ARQ session over the loopback: A connects to B, B answers, they
 * complete the four-message handshake, A sends a block of data that B delivers
 * to its host, and A disconnects. Every frame really goes through modulate +
 * demodulate.
 */
static void test_loopback_connect_data_disconnect(void **state)
{
	(void)state;

	static struct station a, b;   /* static: ~200 KB of demod/link state. */
	station_init(&a, "N0AAA", false);
	station_init(&b, "N0BBB", true);

	ardop_action acts[16];
	const ardop_action *send;

	/* A: host CONNECT to B -> ConReq. */
	ardop_link_input cmd = {0};
	cmd.kind = ARDOP_IN_HOST;
	cmd.as.host.kind = ARDOP_CMD_CONNECT;
	assert_int_equal(ardop_stationid_from_str("N0BBB", &cmd.as.host.target),
			 ARDOP_STATIONID_OK);
	cmd.as.host.bandwidth = ARDOP_ARQ_BW_UNDEFINED;
	size_t n = ardop_link_step(&a.link, &cmd, g_now, acts, 16);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(a.link.state, ARDOP_LINK_ISS_CON_REQ);

	/* ConReq A->B: B answers ConAck, becomes IRS pending. */
	n = hop(&b, send, acts, 16);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(b.link.state, ARDOP_LINK_IRS_CON_ACK);

	/* ConAck B->A: A adopts the width, replies ConAck. */
	n = hop(&a, send, acts, 16);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(a.link.state, ARDOP_LINK_ISS_CON_ACK);
	assert_int_equal(a.link.session_bw, 2000);

	/* ConAck A->B: B completes, sends DataACK, is connected. */
	n = hop(&b, send, acts, 16);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(b.link.state, ARDOP_LINK_IRS_DATA);

	/* Queue data on A before it sees the DataACK. */
	const uint8_t payload[] = "Hello over the air, N0BBB!";
	int plen = (int)sizeof(payload) - 1;   /* drop the NUL. */
	ardop_link_input data = {0};
	data.kind = ARDOP_IN_HOST;
	data.as.host.kind = ARDOP_CMD_SEND_DATA;
	data.as.host.data = payload;
	data.as.host.data_len = (size_t)plen;
	(void)ardop_link_step(&a.link, &data, g_now, acts, 16);

	/* DataACK B->A: A is connected and sends the first data frame. */
	n = hop(&a, send, acts, 16);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(a.link.state, ARDOP_LINK_ISS);

	/* Data A->B: B delivers it to its host and ACKs. */
	n = hop(&b, send, acts, 16);
	const ardop_action *deliver = NULL;
	for (size_t i = 0; i < n; i++)
		if (acts[i].kind == ARDOP_ACT_DELIVER_DATA)
			deliver = &acts[i];
	assert_non_null(deliver);
	assert_int_equal(deliver->data_len, plen);
	assert_memory_equal(deliver->data, payload, (size_t)plen);
	send = find_send(acts, n);   /* B's DataACK. */
	assert_non_null(send);

	/* DataACK B->A: queue drained, A falls to IDLE. */
	n = hop(&a, send, acts, 16);
	assert_int_equal(a.link.state, ARDOP_LINK_IDLE);
	assert_int_equal(a.link.tx_len, 0);

	/* A: host DISCONNECT -> DISC; B answers END; A ends. */
	ardop_link_input dc = {0};
	dc.kind = ARDOP_IN_HOST;
	dc.as.host.kind = ARDOP_CMD_DISCONNECT;
	n = ardop_link_step(&a.link, &dc, g_now, acts, 16);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_DISC);

	/* DISC A->B: B disconnects and sends END. */
	n = hop(&b, send, acts, 16);
	assert_int_equal(b.link.state, ARDOP_LINK_DISC);
	send = find_send(acts, n);
	assert_non_null(send);
	assert_int_equal(send->frame_type, ARDOP_FT_END);

	/* END B->A: A disconnects. */
	(void)hop(&a, send, acts, 16);
	assert_int_equal(a.link.state, ARDOP_LINK_DISC);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loopback_connect_data_disconnect),
		cmocka_unit_test(test_loopback_turnover),
		cmocka_unit_test(test_loopback_turnover_loses_nothing),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
