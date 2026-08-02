#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "apps/fecchat.h"
#include "core/codec/frame.h"

/*
 * The FEC broadcast profile: analysis/17 §6.
 *
 * Everything here is pure, which is the point -- the dedup window is the only
 * stateful part and time is a parameter, so "does a repeat five minutes later
 * get through" is a function call rather than a five-minute wait.
 */

#define CALL_A "N0AAA"
#define CALL_B "N0BBB-4"

/* --- deduplication ---------------------------------------------------------- */

/*
 * The reason TEXT_B carries a message id at all.
 *
 * FECREPEATS sends each frame N extra times. core/link.c drops a repeat whose
 * type and CRC match the frame before it -- and its own comment admits what that
 * cannot do: two stations interleaving break the "consecutive" assumption, and
 * a line legitimately sent twice is silently swallowed. (callsign, msg_id) has
 * neither problem, and this asserts both halves.
 */
static void test_repeats_are_suppressed(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	assert_true(fecchat_is_new(&f, CALL_A, 1, 1000));
	assert_false(fecchat_is_new(&f, CALL_A, 1, 1200));
	assert_false(fecchat_is_new(&f, CALL_A, 1, 4000));
}

static void test_two_stations_do_not_shadow_each_other(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	/* Same id from different stations: two messages, not a repeat. Every
	 * sender counts from zero, so this collides constantly. */
	assert_true(fecchat_is_new(&f, CALL_A, 7, 1000));
	assert_true(fecchat_is_new(&f, CALL_B, 7, 1100));

	/* And interleaved repeats are still repeats, which is exactly the case
	 * a consecutive-payload check gets wrong. */
	assert_false(fecchat_is_new(&f, CALL_A, 7, 1200));
	assert_false(fecchat_is_new(&f, CALL_B, 7, 1300));
}

static void test_the_same_line_twice_is_two_messages(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	/* "QRZ?" ... "QRZ?" -- identical text, different id, both shown. The
	 * consecutive-CRC check in core drops the second; this must not. */
	assert_true(fecchat_is_new(&f, CALL_A, 1, 1000));
	assert_true(fecchat_is_new(&f, CALL_A, 2, 9000));
}

static void test_the_window_expires(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	assert_true(fecchat_is_new(&f, CALL_A, 3, 1000));
	assert_false(fecchat_is_new(&f, CALL_A, 3, 1000 + FECCHAT_DEDUP_MS));

	/* Past the window it is a new message. §6 chooses five minutes because
	 * a repeat arrives within seconds, so anything later is someone saying
	 * something again -- and a wrapped u16 counter after that long is not a
	 * collision worth protecting against. */
	assert_true(fecchat_is_new(&f, CALL_A, 3, 1001 + FECCHAT_DEDUP_MS));
}

static void test_a_busy_net_does_not_lose_the_recent(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	/* Overflow the table, then check the newest is still remembered: an
	 * eviction policy that dropped the most recent would defeat the whole
	 * mechanism precisely when the channel is busiest. */
	for (int i = 0; i < FECCHAT_SEEN_MAX + 10; i++)
		assert_true(fecchat_is_new(&f, CALL_A, (uint16_t)i,
					   1000 + (uint64_t)i));

	const uint16_t newest = FECCHAT_SEEN_MAX + 9;
	assert_false(fecchat_is_new(&f, CALL_A, newest,
				    1000 + (uint64_t)newest));
}

/* --- the frame budget ------------------------------------------------------- */

/*
 * §6 designed TEXT_B without reference to what a frame holds, and the gap is
 * wide enough to matter: the most robust FEC mode carries sixteen bytes.
 */
static void test_capacity_matches_the_frame(void **state)
{
	(void)state;

	uint8_t robust = 0xff, normal = 0xff;
	for (int t = 0; t < 256; t++) {
		char n[32];
		if (!ardop_data_frame_name((uint8_t)t, n, sizeof n))
			continue;
		if (strcmp(n, "4FSK.200.50S") == 0)
			robust = (uint8_t)t;
		if (strcmp(n, "4PSK.200.100") == 0)
			normal = (uint8_t)t;
	}
	assert_int_not_equal(robust, 0xff);
	assert_int_not_equal(normal, 0xff);

	/* A 64-byte frame, less ten bytes of header for a five-character
	 * callsign: type, length, calllen, the call itself, and the id. */
	assert_int_equal(fecchat_text_capacity(normal, CALL_A), 54);

	/* And the arithmetic is checked against actual bytes rather than
	 * trusted: a full message must come out at exactly the frame size, so
	 * one more character would need a second frame. */
	fecchat f;
	fecchat_init(&f);
	static char full[64];
	memset(full, 'x', 54);
	uint8_t wire[ASP_MAX_MESSAGE];
	assert_int_equal(fecchat_encode(&f, CALL_A, full, 54, wire, sizeof wire),
			 64);

	/* The 16-byte frame leaves six characters. Small enough to be a
	 * surprise, which is why the tool prints it at startup rather than
	 * letting an operator discover it by having a sentence split ten ways. */
	const size_t tight = fecchat_text_capacity(robust, CALL_A);
	assert_int_equal(tight, 6);

	/* A long callsign eats into it, which is why capacity takes the call. */
	assert_true(fecchat_text_capacity(robust, "VK2ABC-15") < tight);

	assert_int_equal(fecchat_text_capacity(0xff, CALL_A), 0);
}

/* --- the round trip --------------------------------------------------------- */

static void test_encode_decode_round_trip(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	uint8_t wire[ASP_MAX_MESSAGE];
	const char *msg = "net starts at 0200z on 7.102";
	const size_t n = fecchat_encode(&f, CALL_B, msg, strlen(msg), wire,
					sizeof wire);
	assert_true(n > 0);

	char call[ASP_MAX_CALL];
	uint16_t id;
	const char *text;
	size_t text_len;
	assert_int_equal(fecchat_decode(wire, n, call, sizeof call, &id, &text,
					&text_len),
			 FECCHAT_MESSAGE);
	assert_string_equal(call, CALL_B);
	assert_int_equal(text_len, strlen(msg));
	assert_memory_equal(text, msg, text_len);

	/* The counter advances, so the next message is not read as this one. */
	const size_t n2 = fecchat_encode(&f, CALL_B, msg, strlen(msg), wire,
					 sizeof wire);
	uint16_t id2;
	assert_true(n2 > 0);
	fecchat_decode(wire, n2, call, sizeof call, &id2, &text, &text_len);
	assert_int_not_equal(id, id2);
}

/*
 * §6: "a TEXT_B that fails to parse is displayed as raw text rather than
 * discarded" -- broadcast is where we most often meet a station running
 * something else, and a net showing only our own messages would be worse than
 * no framing at all.
 */
static void test_a_plain_broadcast_is_shown_not_dropped(void **state)
{
	(void)state;

	char call[ASP_MAX_CALL];
	uint16_t id;
	const char *text;
	size_t text_len;

	const char *plain = "anyone about on 40 this evening?\n";
	assert_int_equal(fecchat_decode((const uint8_t *)plain, strlen(plain),
					call, sizeof call, &id, &text,
					&text_len),
			 FECCHAT_RAW);
	assert_int_equal(text_len, strlen(plain));
	assert_memory_equal(text, plain, text_len);
	assert_string_equal(call, "");

	/* A truncated TEXT_B -- a frame lost mid-message, which FEC does all the
	 * time -- is raw rather than an error or a crash. */
	fecchat f;
	fecchat_init(&f);
	uint8_t wire[ASP_MAX_MESSAGE];
	const size_t n = fecchat_encode(&f, CALL_A, "hello there", 11, wire,
					sizeof wire);
	assert_true(n > 4);
	assert_int_equal(fecchat_decode(wire, n - 3, call, sizeof call, &id,
					&text, &text_len),
			 FECCHAT_RAW);
}

static void test_a_message_too_long_is_refused(void **state)
{
	(void)state;
	fecchat f;
	fecchat_init(&f);

	static char huge[ASP_MAX_PAYLOAD * 2];
	memset(huge, 'x', sizeof huge);

	uint8_t wire[ASP_MAX_MESSAGE];
	assert_int_equal(fecchat_encode(&f, CALL_A, huge, sizeof huge, wire,
					sizeof wire),
			 0);

	/* And the id was not consumed: a refused message must not leave a hole
	 * a receiver could read as something lost. */
	const size_t n = fecchat_encode(&f, CALL_A, "ok", 2, wire, sizeof wire);
	char call[ASP_MAX_CALL];
	uint16_t id;
	const char *text;
	size_t text_len;
	assert_true(n > 0);
	fecchat_decode(wire, n, call, sizeof call, &id, &text, &text_len);
	assert_int_equal(id, 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_repeats_are_suppressed),
		cmocka_unit_test(test_two_stations_do_not_shadow_each_other),
		cmocka_unit_test(test_the_same_line_twice_is_two_messages),
		cmocka_unit_test(test_the_window_expires),
		cmocka_unit_test(test_a_busy_net_does_not_lose_the_recent),
		cmocka_unit_test(test_capacity_matches_the_frame),
		cmocka_unit_test(test_encode_decode_round_trip),
		cmocka_unit_test(test_a_plain_broadcast_is_shown_not_dropped),
		cmocka_unit_test(test_a_message_too_long_is_refused),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
