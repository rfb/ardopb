#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "app/loopback.h"
#include "app/ring.h"
#include "app/spine.h"

/*
 * The embedding spine (analysis/14 workstream A), tested with no sockets and no
 * threads. The one property this file cannot show is the thread-safety of the
 * rings -- that needs a second thread and a sanitiser, and lives in
 * test/core/stress_spine.c behind `make test-app-tsan`.
 *
 * Everything else is here: the record ring's framing and refusal rules, the
 * transmit credit's arithmetic, the TNC takeover rule against a fake transport,
 * the truncation assertion, the deep-copying of borrowed observation pointers,
 * and a whole ARQ session over the in-memory loopback.
 */

/* Stepping budget for the loopback. One step is ARDOP_LOOP_BLOCK samples, so
 * 1200 at 12 kHz -- 100 ms of simulated air. 4000 of them is over six minutes,
 * far past any protocol timeout the session under test can hit. */
#define MAX_STEPS 4000

/* --- the record ring ------------------------------------------------------ */

static void test_ring_framing(void **state)
{
	(void)state;
	uint8_t storage[256];
	app_ring rg;
	assert_true(app_ring_init(&rg, storage, sizeof storage));

	assert_int_equal(app_ring_used(&rg), 0);
	assert_int_equal(app_ring_peek(&rg), 0);

	uint8_t out[64];
	assert_int_equal(app_ring_read(&rg, out, sizeof out), 0);

	/* A record written in two parts comes back as one. */
	assert_true(app_ring_write(&rg, "abc", 3, "de", 2, 0));
	assert_int_equal(app_ring_peek(&rg), 5);
	assert_int_equal(app_ring_used(&rg), 5 + APP_RING_OVERHEAD);
	assert_int_equal(app_ring_read(&rg, out, sizeof out), 5);
	assert_memory_equal(out, "abcde", 5);
	assert_int_equal(app_ring_used(&rg), 0);

	/* Order is preserved across many records. */
	for (int i = 0; i < 5; i++) {
		uint8_t b = (uint8_t)i;
		assert_true(app_ring_write(&rg, &b, 1, NULL, 0, 0));
	}
	for (int i = 0; i < 5; i++) {
		assert_int_equal(app_ring_read(&rg, out, sizeof out), 1);
		assert_int_equal(out[0], i);
	}
	assert_false(app_ring_peek(&rg) != 0);
}

/*
 * Every record, including its length prefix, must survive straddling the end of
 * the buffer. Writing and reading one at a time walks the write offset all the
 * way round several times, so each of the four header bytes lands in the last
 * position at some point.
 */
static void test_ring_wrap(void **state)
{
	(void)state;
	uint8_t storage[64];
	app_ring rg;
	assert_true(app_ring_init(&rg, storage, sizeof storage));

	uint8_t body[13];
	uint8_t out[sizeof body];
	for (int round = 0; round < 200; round++) {
		memset(body, (uint8_t)round, sizeof body);
		assert_true(app_ring_write(&rg, body, sizeof body, NULL, 0, 0));
		assert_int_equal(app_ring_peek(&rg), sizeof body);
		assert_int_equal(app_ring_read(&rg, out, sizeof out),
				 sizeof body);
		assert_memory_equal(out, body, sizeof body);
	}
}

/*
 * A refused write must disturb nothing. This is what lets a lossy producer treat
 * refusal as "drop this record" rather than "the queue is now in an unknown
 * state".
 */
static void test_ring_refusal_is_atomic(void **state)
{
	(void)state;
	uint8_t storage[64];
	app_ring rg;
	assert_true(app_ring_init(&rg, storage, sizeof storage));

	/* Fill it: five 8-byte records cost 60 bytes of a 64-byte ring. */
	uint8_t body[8];
	memset(body, 0xAA, sizeof body);
	int wrote = 0;
	while (app_ring_write(&rg, body, sizeof body, NULL, 0, 0))
		wrote++;
	assert_int_equal(wrote, 5);

	size_t used = app_ring_used(&rg);
	assert_false(app_ring_write(&rg, body, sizeof body, NULL, 0, 0));
	assert_int_equal(app_ring_used(&rg), used);

	/* And everything already queued is intact. */
	uint8_t out[sizeof body];
	for (int i = 0; i < wrote; i++) {
		assert_int_equal(app_ring_read(&rg, out, sizeof out),
				 sizeof body);
		assert_memory_equal(out, body, sizeof body);
	}
}

/* The reserve: ordinary writes stop short, the privileged one still fits. */
static void test_ring_reserve(void **state)
{
	(void)state;
	uint8_t storage[64];
	app_ring rg;
	assert_true(app_ring_init(&rg, storage, sizeof storage));

	uint8_t body[8];
	memset(body, 0x5A, sizeof body);
	while (app_ring_write(&rg, body, sizeof body, NULL, 0, 24))
		;

	/* Refused with a reserve, accepted without one -- which is exactly how
	 * the event queue reports its own overflow. */
	assert_false(app_ring_write(&rg, body, sizeof body, NULL, 0, 24));
	assert_true(app_ring_write(&rg, body, sizeof body, NULL, 0, 0));
}

/* An undeliverable record is dropped, not left to wedge the queue behind it. */
static void test_ring_oversize_is_discarded(void **state)
{
	(void)state;
	uint8_t storage[128];
	app_ring rg;
	assert_true(app_ring_init(&rg, storage, sizeof storage));

	uint8_t big[40];
	memset(big, 0x11, sizeof big);
	assert_true(app_ring_write(&rg, big, sizeof big, NULL, 0, 0));
	assert_true(app_ring_write(&rg, "tail", 4, NULL, 0, 0));

	uint8_t small[8];
	assert_int_equal(app_ring_read(&rg, small, sizeof small), 0);

	/* The record behind it is still reachable. */
	uint8_t out[8];
	assert_int_equal(app_ring_read(&rg, out, sizeof out), 4);
	assert_memory_equal(out, "tail", 4);
}

/* --- the transmit credit --------------------------------------------------- */

/* Drain every event, returning how many there were. */
static int drain(app_spine *sp)
{
	app_event ev;
	int n = 0;
	while (app_events_pop(sp, &ev))
		n++;
	return n;
}

static void test_credit_arithmetic(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);

	/* Credit is true from the moment the spine exists, not from the first
	 * step: the link's queue really is empty and really can take bytes, and
	 * they wait in the command ring until the modem thread runs. Reporting
	 * "queue full" about an empty queue would be a lie a settings screen
	 * would show for as long as the audio device took to open. */
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);

	/* One call is capped at a chunk, however much is offered. */
	uint8_t payload[APP_TX_CHUNK * 2];
	memset(payload, 0x42, sizeof payload);

	app_tx_status why = APP_TX_OK;
	size_t took = app_tx_submit(sp, payload, sizeof payload, &why);
	assert_int_equal(took, APP_TX_CHUNK);

	/* The cap is per call, not per step: the link's queue is sixteen kilobytes
	 * and a submitter may fill it without waiting. */
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);

	/* Keep offering, without stepping, until it is all spoken for. This is
	 * the in-flight accounting: the credit falls as bytes are *offered*, not
	 * as they are applied, so the link's capacity cannot be promised twice. */
	size_t total = took;
	while (app_tx_credit(sp) > 0)
		total += app_tx_submit(sp, payload, APP_TX_CHUNK, &why);
	assert_int_equal(total, app_runtime(sp)->link.tx_cap);

	/* Not one byte has reached the link yet, and yet the credit is correctly
	 * zero -- which is the whole point of accounting for offers. */
	assert_int_equal(app_runtime(sp)->link.tx_len, 0);
	assert_int_equal(app_tx_submit(sp, payload, 16, &why), 0);
	assert_int_equal(why, APP_TX_FULL);

	/* Applying them moves exactly that many bytes into the link. */
	app_step(sp);
	assert_int_equal(app_runtime(sp)->link.tx_len,
			 app_runtime(sp)->link.tx_cap);
	assert_int_equal(app_tx_credit(sp), 0);

	app_status st;
	app_snapshot(sp, &st);
	assert_int_equal(st.tx_credit, 0);
	assert_int_equal(st.tx_reason, APP_TX_FULL);
	assert_int_equal(st.event_lost, 0);

	/* Purging the link's queue restores it, which is the drain path a real
	 * session takes one frame at a time. */
	assert_true(app_submit_line(sp, "PURGEBUFFER"));
	app_step(sp);
	assert_int_equal(app_runtime(sp)->link.tx_len, 0);
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);

	(void)drain(sp);
	app_close(sp);
}

/*
 * The link truncates a SEND_DATA past its capacity silently and reports nothing
 * (core/link/link.c:1023-1039). The credit makes that unreachable, so this
 * forces it -- by filling the link's queue behind the credit's back, between the
 * submission and its application -- and asserts the spine notices.
 */
static void test_truncation_is_reported(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);

	app_step(sp);
	uint8_t payload[APP_TX_CHUNK];
	memset(payload, 0x7E, sizeof payload);
	assert_int_equal(app_tx_submit(sp, payload, sizeof payload, NULL),
			 APP_TX_CHUNK);

	/* The submission is in the ring but not yet applied. Fill the link. */
	ardop_runtime *rt = app_runtime(sp);
	rt->link.tx_len = rt->link.tx_cap;

	app_step(sp);

	app_event ev;
	bool saw = false;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_FAULT &&
		    strstr(ev.text, "transmit queue took"))
			saw = true;
	assert_true(saw);

	app_close(sp);
}

/*
 * A device fault must be survivable.
 *
 * `shell/fault.h` names the intended reaction as "abort the ARQ session, unkey,
 * tell the operator, offer reselection" -- and reselection needs the reverse
 * edge. This used to be a one-way flag, which meant a spine that had seen one
 * fault could never transmit again and a device-selection screen was
 * structurally impossible: unplugging a USB interface meant restarting the
 * program.
 */
static void test_fault_is_recoverable(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);

	app_step(sp);
	assert_int_equal(app_run_state_of(sp), APP_RUN_OK);
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);
	(void)drain(sp);

	app_report_fault(sp, APP_FAULT_CAPTURE, "capture device lost");

	/* Typed, so an interface can offer "choose another capture device"
	 * without matching on a sentence. */
	app_event ev;
	bool saw = false;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_FAULT && ev.code == APP_FAULT_CAPTURE)
			saw = true;
	assert_true(saw);

	assert_int_equal(app_run_state_of(sp), APP_RUN_FAULTED);
	assert_int_equal(app_fault_state(sp), APP_FAULT_CAPTURE);

	uint8_t byte = 0;
	app_tx_status why = APP_TX_OK;
	assert_int_equal(app_tx_submit(sp, &byte, 1, &why), 0);
	assert_int_equal(why, APP_TX_FAULTED);

	/* First fault wins: a capture loss that also drops the keying line reads
	 * as one failure, not two. */
	app_report_fault(sp, APP_FAULT_PTT, "PTT control lost");
	assert_int_equal(app_fault_state(sp), APP_FAULT_CAPTURE);

	/* The caller repairs the device -- here, nothing to repair -- and says so. */
	assert_int_equal(app_clear_fault(sp), APP_RUN_FAULTED);
	assert_int_equal(app_run_state_of(sp), APP_RUN_OK);
	assert_int_equal(app_fault_state(sp), APP_FAULT_NONE);
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);

	/* And transmission genuinely works again, not just the credit. */
	uint8_t payload[64];
	memset(payload, 0x5A, sizeof payload);
	assert_int_equal(app_tx_submit(sp, payload, sizeof payload, &why),
			 sizeof payload);
	app_step(sp);
	assert_int_equal(app_runtime(sp)->link.tx_len, sizeof payload);

	app_close(sp);
}

/* Closing is terminal; a fault is not. Nothing may resurrect a spine that is
 * being torn down. */
static void test_closing_is_not_clearable(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);
	app_step(sp);

	app_report_fault(sp, APP_FAULT_PLAYBACK, "playback device lost");
	assert_int_equal(app_run_state_of(sp), APP_RUN_FAULTED);
	assert_int_equal(app_clear_fault(sp), APP_RUN_FAULTED);
	assert_int_equal(app_run_state_of(sp), APP_RUN_OK);

	app_close(sp);
}

/* --- the TNC takeover rule ------------------------------------------------- */

struct fake_tnc {
	bool attached;
	int services;
	char last_notify[128];
	size_t data_seen;
};

static void fake_service(void *ctx, ardop_runtime *rt, uint64_t now)
{
	(void)rt;
	(void)now;
	((struct fake_tnc *)ctx)->services++;
}
static bool fake_attached(void *ctx)
{
	return ((struct fake_tnc *)ctx)->attached;
}
static void fake_notify(void *ctx, const char *msg)
{
	struct fake_tnc *f = ctx;
	snprintf(f->last_notify, sizeof f->last_notify, "%s", msg);
}
static void fake_send_data(void *ctx, const char *tag, const uint8_t *d,
			   size_t n)
{
	(void)tag;
	(void)d;
	((struct fake_tnc *)ctx)->data_seen += n;
}

static void test_tnc_takeover(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);

	struct fake_tnc fake;
	memset(&fake, 0, sizeof fake);
	app_tnc_ops ops = {
		.ctx = &fake,
		.service = fake_service,
		.attached = fake_attached,
		.notify = fake_notify,
		.send_data = fake_send_data,
	};
	app_set_tnc(sp, &ops);

	app_step(sp);
	assert_int_equal(fake.services, 1);
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);
	(void)drain(sp);

	/* A client connects. */
	fake.attached = true;
	app_step(sp);

	app_event ev;
	bool owner = false;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_OWNER && ev.flag)
			owner = true;
	assert_true(owner);

	app_status st;
	app_snapshot(sp, &st);
	assert_true(st.tnc_attached);

	/* Transmission is disabled at the admission point, with a reason. */
	app_tx_status why = APP_TX_OK;
	uint8_t byte = 0;
	assert_int_equal(app_tx_submit(sp, &byte, 1, &why), 0);
	assert_int_equal(why, APP_TX_TNC_OWNS);

	/* So are session commands, and they say so rather than failing silently. */
	assert_true(app_submit_line(sp, "ARQCALL N0BBB 2000"));
	app_step(sp);
	bool refused = false;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_REPLY &&
		    strstr(ev.text, "TNC client owns the link"))
			refused = true;
	assert_true(refused);
	assert_int_equal(app_runtime(sp)->link.state, ARDOP_LINK_DISC);

	/* Configuration is not disabled: refusing a guest's MYCALL would break
	 * Pat's startup handshake, and refusing the operator's is the same
	 * mistake pointed the other way. */
	assert_true(app_submit_config(sp, APP_CFG_MYCALL,
				      (app_cfg_value){.str = "N0AAA"}));
	app_step(sp);
	assert_string_equal(app_runtime(sp)->link.mycall.str, "N0AAA");
	(void)drain(sp);

	/* The client leaves; the release is the disconnect itself. */
	fake.attached = false;
	app_step(sp);
	owner = false;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_OWNER && !ev.flag)
			owner = true;
	assert_true(owner);
	assert_int_equal(app_tx_credit(sp), APP_TX_CHUNK);

	app_close(sp);
}

/* --- configuration --------------------------------------------------------- */

/*
 * Settings are typed at the seam and applied as TNC text, so a guest's MYCALL
 * and a settings screen's MYCALL run the same validator. That means an invalid
 * value has to come back as a fault rather than being swallowed.
 */
static void test_config_shares_the_host_validator(void **state)
{
	(void)state;
	app_spine *sp = app_open(NULL);
	assert_non_null(sp);

	assert_true(app_submit_config(sp, APP_CFG_MYCALL,
				      (app_cfg_value){.str = "N0AAA-4"}));
	assert_true(app_submit_config(sp, APP_CFG_ARQBW,
				      (app_cfg_value){.str = "500MAX"}));
	assert_true(app_submit_config(sp, APP_CFG_LISTEN,
				      (app_cfg_value){.flag = true}));
	assert_true(app_submit_config(sp, APP_CFG_BUSYDET,
				      (app_cfg_value){.num = 7}));
	app_step(sp);

	ardop_runtime *rt = app_runtime(sp);
	assert_string_equal(rt->link.mycall.str, "N0AAA-4");
	assert_int_equal(rt->link.bw_setting, ARDOP_ARQ_BW_500_MAX);
	assert_true(rt->link.listening);
	assert_int_equal(rt->busy_det, 7);

	/* Four replies, each the host protocol's own wording. */
	app_event ev;
	int replies = 0;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_REPLY)
			replies++;
	assert_int_equal(replies, 4);

	/* A bad value is reported, not applied. */
	assert_true(app_submit_config(sp, APP_CFG_ARQBW,
				      (app_cfg_value){.str = "9999WIDE"}));
	app_step(sp);
	bool fault = false;
	while (app_events_pop(sp, &ev))
		if (ev.kind == APP_EV_REPLY && strncmp(ev.text, "FAULT", 5) == 0)
			fault = true;
	assert_true(fault);
	assert_int_equal(rt->link.bw_setting, ARDOP_ARQ_BW_500_MAX);

	/* SEND_DATA cannot be smuggled in as a typed command: it has no admission
	 * control there and would bypass the credit entirely. */
	ardop_host_cmd sd = {.kind = ARDOP_CMD_SEND_DATA};
	assert_false(app_submit_cmd(sp, &sd));

	app_close(sp);
}

/* --- the display queue ----------------------------------------------------- */

/*
 * The display queue is lossy on purpose, and has to say how lossy. Filling it
 * without draining must raise the drop count and must not cost a single event:
 * the two queues are separate precisely so that a wedged display cannot corrupt
 * a file transfer.
 */
static void test_display_drops_are_counted(void **state)
{
	(void)state;
	app_config cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.telemetry = true;
	/* Small enough that a short run overruns it. */
	cfg.display_bytes = ARDOP_TLM_MAX_RECORD + APP_RING_OVERHEAD + 64;

	app_loopback *lb = app_loopback_open(&cfg);
	assert_non_null(lb);
	app_spine *a = app_loopback_side(lb, 0);

	for (int i = 0; i < 200; i++)
		app_loopback_step(lb);

	app_status st;
	app_snapshot(a, &st);
	assert_true(st.display_drops > 0);
	assert_int_equal(st.event_lost, 0);

	/* What did survive is still well-formed, which is the property the
	 * all-or-nothing write buys. */
	ardop_tlm_decoded rec;
	int parsed = 0;
	while (app_display_pop(a, &rec))
		parsed++;
	assert_true(parsed > 0);

	app_loopback_close(lb);
}

/* --- a whole session ------------------------------------------------------- */

struct trace {
	char host[64][APP_TEXT_MAX];
	int n_host;
	uint8_t rx[65536];
	size_t rx_len;
};

/* Collect one side's events. */
static void collect(app_spine *sp, struct trace *t)
{
	app_event ev;
	while (app_events_pop(sp, &ev)) {
		switch (ev.kind) {
		case APP_EV_HOST_MSG:
			if (t->n_host < (int)(sizeof t->host / sizeof t->host[0]))
				snprintf(t->host[t->n_host++],
					 sizeof t->host[0], "%s", ev.text);
			break;
		case APP_EV_RX_DATA:
			if (t->rx_len + ev.data_len <= sizeof t->rx) {
				memcpy(t->rx + t->rx_len, ev.data, ev.data_len);
				t->rx_len += ev.data_len;
			}
			break;
		default:
			break;
		}
	}
}

static bool saw(const struct trace *t, const char *needle)
{
	for (int i = 0; i < t->n_host; i++)
		if (strstr(t->host[i], needle))
			return true;
	return false;
}

static void test_loopback_session(void **state)
{
	(void)state;
	app_loopback *lb = app_loopback_open(NULL);
	assert_non_null(lb);

	app_spine *a = app_loopback_side(lb, 0);
	app_spine *b = app_loopback_side(lb, 1);
	struct trace ta, tb;
	memset(&ta, 0, sizeof ta);
	memset(&tb, 0, sizeof tb);

	assert_true(app_submit_config(a, APP_CFG_MYCALL,
				      (app_cfg_value){.str = "N0AAA"}));
	assert_true(app_submit_config(a, APP_CFG_ARQBW,
				      (app_cfg_value){.str = "2000MAX"}));
	assert_true(app_submit_config(b, APP_CFG_MYCALL,
				      (app_cfg_value){.str = "N0BBB"}));
	assert_true(app_submit_config(b, APP_CFG_ARQBW,
				      (app_cfg_value){.str = "2000MAX"}));
	assert_true(app_submit_config(b, APP_CFG_LISTEN,
				      (app_cfg_value){.flag = true}));
	app_loopback_step(lb);

	/* Connect. */
	assert_true(app_submit_line(a, "ARQCALL N0BBB 2000"));
	int steps = 0;
	while (steps++ < MAX_STEPS && !saw(&ta, "CONNECTED")) {
		app_loopback_step(lb);
		collect(a, &ta);
		collect(b, &tb);
	}
	assert_true(saw(&ta, "CONNECTED"));
	assert_true(saw(&tb, "CONNECTED"));

	/* Send a payload larger than one frame and larger than one credit chunk,
	 * so the submission has to honour backpressure over many steps. */
	static uint8_t payload[6000];
	for (size_t i = 0; i < sizeof payload; i++)
		payload[i] = (uint8_t)(i * 31u + 7u);

	size_t sent = 0;
	while (steps++ < MAX_STEPS &&
	       (sent < sizeof payload || tb.rx_len < sizeof payload)) {
		if (sent < sizeof payload) {
			app_tx_status why = APP_TX_OK;
			sent += app_tx_submit(a, payload + sent,
					      sizeof payload - sent, &why);
		}
		app_loopback_step(lb);
		collect(a, &ta);
		collect(b, &tb);
	}

	assert_int_equal(sent, sizeof payload);
	assert_int_equal(tb.rx_len, sizeof payload);
	assert_memory_equal(tb.rx, payload, sizeof payload);

	/* Disconnect. */
	assert_true(app_submit_line(a, "DISCONNECT"));
	while (steps++ < MAX_STEPS && !saw(&ta, "DISCONNECTED")) {
		app_loopback_step(lb);
		collect(a, &ta);
		collect(b, &tb);
	}
	assert_true(saw(&ta, "DISCONNECTED"));
	assert_int_equal(app_runtime(a)->link.state, ARDOP_LINK_DISC);

	/* Nothing was lost anywhere, and the harness never dropped a sample --
	 * if it had, a decode failure here would be its fault, not the modem's. */
	app_status st;
	app_snapshot(a, &st);
	assert_int_equal(st.event_lost, 0);
	app_snapshot(b, &st);
	assert_int_equal(st.event_lost, 0);
	assert_int_equal(app_loopback_overruns(lb), 0);

	app_loopback_close(lb);
}

/*
 * Observation pointers are borrowed, and borrowed more briefly than the callback
 * contract suggests: `ardop_obs::text` addresses a 128-byte buffer inside the
 * link that the *next link step* overwrites, and one step performs several
 * actions. An observer that stored the pointer and copied later would therefore
 * hand the consumer several copies of the last message of each step.
 *
 * A session emits a long run of distinct notifications, so the fingerprint of
 * that bug is that they collapse to a handful of repeats. Assert on the
 * distinctness, which a one-message-per-step test could never see.
 */
static void test_host_messages_are_deep_copied(void **state)
{
	(void)state;
	app_loopback *lb = app_loopback_open(NULL);
	assert_non_null(lb);

	app_spine *a = app_loopback_side(lb, 0);
	app_spine *b = app_loopback_side(lb, 1);
	struct trace ta, tb;
	memset(&ta, 0, sizeof ta);
	memset(&tb, 0, sizeof tb);

	(void)app_submit_config(a, APP_CFG_MYCALL,
				(app_cfg_value){.str = "N0AAA"});
	(void)app_submit_config(b, APP_CFG_MYCALL,
				(app_cfg_value){.str = "N0BBB"});
	(void)app_submit_config(b, APP_CFG_LISTEN, (app_cfg_value){.flag = true});
	app_loopback_step(lb);

	(void)app_submit_line(a, "ARQCALL N0BBB 2000");
	int steps = 0;
	while (steps++ < MAX_STEPS && !saw(&ta, "CONNECTED")) {
		app_loopback_step(lb);
		collect(a, &ta);
		collect(b, &tb);
	}
	(void)app_submit_line(a, "DISCONNECT");
	while (steps++ < MAX_STEPS && !saw(&ta, "DISCONNECTED")) {
		app_loopback_step(lb);
		collect(a, &ta);
		collect(b, &tb);
	}

	/* Both ends of the exchange survived, with the connect notification still
	 * legible after every later step overwrote the buffer it came from. */
	assert_true(saw(&ta, "CONNECTED"));
	assert_true(saw(&ta, "DISCONNECTED"));

	int distinct = 0;
	for (int i = 0; i < ta.n_host; i++) {
		bool dup = false;
		for (int j = 0; j < i; j++)
			if (strcmp(ta.host[i], ta.host[j]) == 0)
				dup = true;
		if (!dup)
			distinct++;
	}
	assert_true(distinct >= 3);

	app_loopback_close(lb);
}

/* --- device lifetime ------------------------------------------------------- */

/*
 * PTT-down is emitted from inside ardop_runtime_pull_tx, against whatever
 * backend is bound when the frame ends. Swapping devices mid-transmission would
 * therefore leave the radio keyed with nothing able to unkey it, so the swap is
 * refused rather than documented as forbidden.
 */
static void test_device_change_refused_while_transmitting(void **state)
{
	(void)state;
	app_loopback *lb = app_loopback_open(NULL);
	assert_non_null(lb);
	app_spine *a = app_loopback_side(lb, 0);

	(void)app_submit_config(a, APP_CFG_MYCALL,
				(app_cfg_value){.str = "N0AAA"});
	app_loopback_step(lb);
	(void)app_submit_line(a, "SENDID");

	bool refused_once = false;
	for (int i = 0; i < 200; i++) {
		app_loopback_step(lb);
		if (ardop_runtime_tx_active(app_runtime(a))) {
			assert_false(app_set_platform(a, NULL, 0));
			refused_once = true;
			break;
		}
	}
	assert_true(refused_once);

	app_loopback_close(lb);
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ring_framing),
		cmocka_unit_test(test_ring_wrap),
		cmocka_unit_test(test_ring_refusal_is_atomic),
		cmocka_unit_test(test_ring_reserve),
		cmocka_unit_test(test_ring_oversize_is_discarded),
		cmocka_unit_test(test_credit_arithmetic),
		cmocka_unit_test(test_truncation_is_reported),
		cmocka_unit_test(test_fault_is_recoverable),
		cmocka_unit_test(test_closing_is_not_clearable),
		cmocka_unit_test(test_tnc_takeover),
		cmocka_unit_test(test_config_shares_the_host_validator),
		cmocka_unit_test(test_display_drops_are_counted),
		cmocka_unit_test(test_loopback_session),
		cmocka_unit_test(test_host_messages_are_deep_copied),
		cmocka_unit_test(test_device_change_refused_while_transmitting),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
