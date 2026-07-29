#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "link/link.h"
#include "codec/frame.h"

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

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_disc_discframe_sends_end),
		cmocka_unit_test(test_disc_ignores_other_frames),
		cmocka_unit_test(test_disc_ignores_bad_frame),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
