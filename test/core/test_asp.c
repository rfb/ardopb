#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "app/asp.h"

/*
 * The seven tests analysis/17 §10 asks for, against two sessions wired to each
 * other through memory.
 *
 * §10's point is that this layer needs no radio, no filesystem and no clock, so
 * none of them appear here: a "file" is an array, the "link" is a byte queue,
 * and time is the loop counter. That is what makes the interesting cases -- a
 * link dropping at 40%, a resume whose prefix does not match -- things a test
 * can simply do, rather than things somebody has to reproduce on the air.
 */

#define WIRE_MAX (1u << 20)
#define FILE_MAX (1u << 18)

/* One end of the pair. */
typedef struct station {
	asp_session s;
	asp_io io;
	struct station *peer;

	/* Bytes in flight towards the peer. */
	uint8_t wire[WIRE_MAX];
	size_t wire_len;

	/* Transmit admission, so flow control is exercised rather than assumed:
	 * a real link refuses, and a session that ignored the refusal would look
	 * fine here and lose data on the air. */
	size_t credit;

	/* The file being sent. */
	const uint8_t *src;
	uint32_t src_len;

	/* The file being received. */
	uint8_t dst[FILE_MAX];
	uint32_t dst_len;

	/* What arrived, for the assertions. */
	char text[64][256];
	size_t text_len[64];
	int texts;
	bool text_raw[64];

	int done_count;
	asp_result_code last_result;
	bool last_inbound;

	/* Resume: what this station already holds when an offer arrives. */
	bool auto_accept;
	uint32_t resume_have;
	uint32_t resume_crc;

	int errors;
	char last_error[128];
} station;

static size_t io_send(void *ctx, const void *data, size_t len)
{
	station *st = ctx;
	if (st->credit == 0)
		return 0;
	size_t n = len < st->credit ? len : st->credit;
	if (st->wire_len + n > WIRE_MAX)
		n = WIRE_MAX - st->wire_len;
	memcpy(st->wire + st->wire_len, data, n);
	st->wire_len += n;
	st->credit -= n;
	return n;
}

static size_t io_credit(void *ctx)
{
	return ((station *)ctx)->credit;
}

static size_t io_read(void *ctx, uint32_t offset, void *out, size_t len)
{
	station *st = ctx;
	if (offset >= st->src_len)
		return 0;
	size_t n = st->src_len - offset;
	if (n > len)
		n = len;
	memcpy(out, st->src + offset, n);
	return n;
}

static bool io_write(void *ctx, const void *data, size_t len)
{
	station *st = ctx;
	if (st->dst_len + len > FILE_MAX)
		return false;
	memcpy(st->dst + st->dst_len, data, len);
	st->dst_len += (uint32_t)len;
	return true;
}

static bool io_offer(void *ctx, const asp_offer *o, uint32_t *have,
		     uint32_t *prefix_crc)
{
	station *st = ctx;
	(void)o;
	*have = st->resume_have;
	*prefix_crc = st->resume_crc;
	return st->auto_accept;
}

static void io_truncate(void *ctx)
{
	((station *)ctx)->dst_len = 0;
}

static void io_text(void *ctx, const char *text, size_t len, bool raw)
{
	station *st = ctx;
	if (st->texts >= 64)
		return;
	size_t n = len < sizeof st->text[0] - 1 ? len : sizeof st->text[0] - 1;
	memcpy(st->text[st->texts], text, n);
	st->text[st->texts][n] = '\0';
	st->text_len[st->texts] = n;
	st->text_raw[st->texts] = raw;
	st->texts++;
}

static void io_done(void *ctx, bool inbound, uint16_t id, asp_result_code r)
{
	station *st = ctx;
	(void)id;
	st->done_count++;
	st->last_result = r;
	st->last_inbound = inbound;
}

static void io_error(void *ctx, const char *why)
{
	station *st = ctx;
	st->errors++;
	snprintf(st->last_error, sizeof st->last_error, "%s", why);
}

static void wire_up(station *a, station *b, const char *call_a,
		    const char *call_b)
{
	memset(a, 0, sizeof *a);
	memset(b, 0, sizeof *b);
	a->peer = b;
	b->peer = a;

	const asp_io tmpl = {.send = io_send,
			     .credit = io_credit,
			     .read_file = io_read,
			     .write_file = io_write,
			     .offer_arrived = io_offer,
			     .truncate_file = io_truncate,
			     .text_arrived = io_text,
			     .transfer_done = io_done,
			     .protocol_error = io_error};
	a->io = tmpl;
	a->io.ctx = a;
	b->io = tmpl;
	b->io.ctx = b;
	a->credit = b->credit = 8192;
	a->auto_accept = b->auto_accept = true;

	asp_open(&a->s, &a->io, call_a);
	asp_open(&b->s, &b->io, call_b);
}

/* Deliver everything queued in one direction. Returns bytes moved. */
static size_t deliver(station *from)
{
	if (from->wire_len == 0)
		return 0;
	size_t n = from->wire_len;
	/* Copied out first: the peer may queue a reply while consuming this,
	 * and the buffers must not alias. */
	static uint8_t tmp[WIRE_MAX];
	memcpy(tmp, from->wire, n);
	from->wire_len = 0;
	asp_recv(&from->peer->s, tmp, n);
	return n;
}

/* Run both ends until nothing more moves, or the budget runs out. */
static int settle(station *a, station *b, int budget)
{
	int steps = 0;
	for (; steps < budget; steps++) {
		a->credit = b->credit = 8192;
		asp_service(&a->s);
		asp_service(&b->s);
		size_t moved = deliver(a) + deliver(b);
		if (moved == 0)
			break;
	}
	return steps;
}

static void fill_pattern(uint8_t *buf, size_t n)
{
	/* Not random and not constant: a pattern that catches a chunk delivered
	 * twice or out of order, which constant bytes would hide. */
	for (size_t i = 0; i < n; i++)
		buf[i] = (uint8_t)((i * 31u + (i >> 8) * 7u) & 0xffu);
}

/* --- 1: a file arrives byte-identical --------------------------------------- */

static void test_transfer_is_byte_identical(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[40000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);   /* HELLO both ways */

	assert_int_equal(a.s.state, ASP_LINK_ASP);
	assert_int_equal(b.s.state, ASP_LINK_ASP);
	assert_string_equal(a.s.peer_call, "N0BBB");

	a.src = src;
	a.src_len = sizeof src;
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);

	assert_true(asp_offer_file(&a.s, "payload.bin", "application/octet-stream",
				   sizeof src, crc));
	settle(&a, &b, 4000);

	assert_int_equal(b.dst_len, sizeof src);
	assert_memory_equal(b.dst, src, sizeof src);
	assert_int_equal(b.last_result, ASP_RESULT_OK);
	assert_int_equal(a.last_result, ASP_RESULT_OK);
	assert_int_equal(a.errors, 0);
	assert_int_equal(b.errors, 0);
}

/* --- 2: interrupted at 40%, resumed, still byte-identical ------------------- */

static void test_transfer_resumes_across_a_drop(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[40000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);

	a.src = src;
	a.src_len = sizeof src;
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);
	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src, crc));

	/* Run until roughly 40% has landed, then drop the link mid-flight --
	 * including whatever was still in the air, which is what a real drop
	 * does and what a clean stop would not test. */
	for (int i = 0; i < 4000 && b.dst_len < sizeof src * 2 / 5; i++) {
		a.credit = b.credit = 8192;
		asp_service(&a.s);
		asp_service(&b.s);
		deliver(&a);
		deliver(&b);
	}
	const uint32_t held = b.dst_len;
	assert_true(held > 0);
	assert_true(held < sizeof src);

	a.wire_len = b.wire_len = 0;
	asp_close(&a.s);
	asp_close(&b.s);

	/*
	 * A new connection. §5's identity rule: the id is meaningless once the
	 * link drops, and resume is keyed on (peer, name, size, crc) -- so the
	 * receiver offers what it holds and the sender checks the prefix.
	 */
	const uint32_t prefix_crc = asp_crc32(ASP_CRC32_INIT, b.dst, held);
	station *keep_b_dst = &b;
	static uint8_t saved[FILE_MAX];
	memcpy(saved, keep_b_dst->dst, held);

	wire_up(&a, &b, "N0AAA", "N0BBB");
	memcpy(b.dst, saved, held);
	b.dst_len = held;
	b.resume_have = held;
	b.resume_crc = prefix_crc;
	a.src = src;
	a.src_len = sizeof src;
	settle(&a, &b, 20);

	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src, crc));
	settle(&a, &b, 4000);

	assert_int_equal(b.dst_len, sizeof src);
	assert_memory_equal(b.dst, src, sizeof src);
	assert_int_equal(b.last_result, ASP_RESULT_OK);

	/* And it really resumed rather than starting over: the file is whole,
	 * and the second connection carried less than the whole of it. */
	assert_true(held > 1024);
}

/* --- 3: a prefix that does not match restarts from zero --------------------- */

static void test_bad_prefix_restarts_from_zero(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[20000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);

	/*
	 * The receiver claims 5000 bytes it does not really have -- a partial
	 * from a truncated earlier attempt, or a different file with the same
	 * name, which is exactly the case §5 says a bare byte count cannot
	 * distinguish.
	 */
	memset(b.dst, 0xaa, 5000);
	b.dst_len = 5000;
	b.resume_have = 5000;
	b.resume_crc = asp_crc32(ASP_CRC32_INIT, b.dst, 5000);

	a.src = src;
	a.src_len = sizeof src;
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);
	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src, crc));
	settle(&a, &b, 4000);

	/* Truncated and resent in full, and the result is correct rather than
	 * 5000 bytes of someone else's file followed by the remainder. */
	assert_int_equal(b.dst_len, sizeof src);
	assert_memory_equal(b.dst, src, sizeof src);
	assert_int_equal(b.last_result, ASP_RESULT_OK);
}

/* --- 4: chat interleaved into a transfer ------------------------------------ */

static void test_chat_interleaves_with_a_transfer(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[30000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);

	a.src = src;
	a.src_len = sizeof src;
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);
	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src, crc));

	static const char *const kLines[] = {
		"how's the path?", "solid copy here", "73",
	};
	size_t sent_lines = 0;

	for (int i = 0; i < 4000; i++) {
		a.credit = b.credit = 8192;
		asp_service(&a.s);

		/* Slipped in between chunks, which is where §6 says it is legal
		 * -- and the test would not be worth much if it only sent chat
		 * before or after the file. */
		if (sent_lines < 3 && i > 0 && i % 7 == 0 &&
		    a.s.out_len == a.s.out_sent) {
			if (asp_send_text(&a.s, kLines[sent_lines],
					  strlen(kLines[sent_lines])))
				sent_lines++;
		}
		asp_service(&b.s);
		if (deliver(&a) + deliver(&b) == 0 && a.s.tx_state == ASP_XFER_NONE)
			break;
	}

	assert_int_equal(sent_lines, 3);
	assert_int_equal(b.dst_len, sizeof src);
	assert_memory_equal(b.dst, src, sizeof src);

	/* In order, and unaffected by the file flowing around them. */
	assert_int_equal(b.texts, 3);
	for (int i = 0; i < 3; i++) {
		assert_string_equal(b.text[i], kLines[i]);
		assert_false(b.text_raw[i]);
	}
}

/* --- 5: a peer that does not speak ASP -------------------------------------- */

static void test_plain_peer_degrades_to_raw(void **state)
{
	(void)state;
	static station a, b;

	wire_up(&a, &b, "N0AAA", "N0BBB");

	/* B is not an ASP station: throw away its HELLO and let it say
	 * something a person would type. */
	b.wire_len = 0;
	const char *plain = "hello there, running plain ardopcf here\r\n";
	asp_recv(&a.s, plain, strlen(plain));

	assert_int_equal(a.s.state, ASP_LINK_RAW);
	assert_int_equal(a.texts, 1);
	assert_true(a.text_raw[0]);
	assert_string_equal(a.text[0], plain);
	assert_int_equal(a.errors, 0);   /* not an error: §2 calls it a feature */

	/* Files are unavailable and the app can ask rather than guess. */
	assert_false(asp_can_send_files(&a.s));

	/* Chat still works, and goes out unframed so their terminal shows it.
	 * The wire is cleared first: our own HELLO is still sitting in it,
	 * having been sent to a peer that never read it. */
	a.wire_len = 0;
	a.credit = 8192;
	assert_true(asp_send_text(&a.s, "hi! ASP station here", 20));
	assert_int_equal(a.wire_len, 20);
	assert_memory_equal(a.wire, "hi! ASP station here", 20);

	/* And the decision is not revisited: a later well-formed HELLO does not
	 * flip us back mid-conversation. */
	uint8_t hello[64];
	asp_hello h = {.version = ASP_VERSION, .caps = ASP_CAP_FILES};
	snprintf(h.call, sizeof h.call, "N0BBB");
	uint8_t payload[64];
	size_t pn = asp_hello_put(payload, sizeof payload, &h);
	size_t n = asp_frame_put(hello, sizeof hello, ASP_MSG_HELLO, payload, pn);
	asp_recv(&a.s, hello, n);
	assert_int_equal(a.s.state, ASP_LINK_RAW);
}

/* --- 6: an unknown type mid-stream ------------------------------------------ */

static void test_unknown_type_mid_transfer(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[20000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);

	a.src = src;
	a.src_len = sizeof src;
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);
	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src, crc));

	bool injected = false;
	for (int i = 0; i < 4000; i++) {
		a.credit = b.credit = 8192;
		asp_service(&a.s);
		asp_service(&b.s);

		/* A message from a future version, in the middle of the data
		 * stream, delivered to the receiver exactly as if the sender had
		 * emitted it. */
		if (!injected && b.dst_len > sizeof src / 3) {
			uint8_t msg[64];
			size_t n = asp_frame_put(msg, sizeof msg,
						 (asp_msg_type)0x7f,
						 "version 2 was here", 18);
			asp_recv(&b.s, msg, n);
			injected = true;
		}

		if (deliver(&a) + deliver(&b) == 0 && a.s.tx_state == ASP_XFER_NONE)
			break;
	}

	assert_true(injected);
	assert_int_equal(b.errors, 0);   /* skipped, never an error */
	assert_int_equal(b.dst_len, sizeof src);
	assert_memory_equal(b.dst, src, sizeof src);
	assert_int_equal(b.last_result, ASP_RESULT_OK);
}

/* --- 7: hostile filenames land inert ---------------------------------------- */

static void test_hostile_offer_names(void **state)
{
	(void)state;

	/* The pure function is tested exhaustively in test_asp_wire.c; what is
	 * checked here is that the session actually applies it, and refuses an
	 * offer whose name cannot be made safe rather than inventing one. */
	static station a, b;
	static uint8_t src[64];
	fill_pattern(src, sizeof src);
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);

	struct {
		const char *offered;
		const char *expected;   /* NULL: the offer must be refused */
	} cases[] = {
		{"../../etc/passwd", "passwd"},
		{"C:\\Windows\\system32\\x", "x"},
		{"CON", "CON_"},
		{"log\nfaked line.txt", "log_faked line.txt"},
		{"../../", NULL},
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		wire_up(&a, &b, "N0AAA", "N0BBB");
		settle(&a, &b, 20);
		a.src = src;
		a.src_len = sizeof src;

		assert_true(asp_offer_file(&a.s, cases[i].offered, NULL,
					   sizeof src, crc));
		settle(&a, &b, 200);

		if (cases[i].expected) {
			assert_string_equal(b.s.rx_name, cases[i].expected);
			assert_int_equal(b.dst_len, sizeof src);
		} else {
			/* Refused: nothing was written and the sender was told. */
			assert_int_equal(b.dst_len, 0);
			assert_int_equal(a.s.tx_state, ASP_XFER_NONE);
		}
		assert_int_equal(b.errors, 0);
	}
}

/* --- flow control ----------------------------------------------------------- */

static void test_transfer_survives_a_stingy_link(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[20000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);

	a.src = src;
	a.src_len = sizeof src;
	const uint32_t crc = asp_crc32(ASP_CRC32_INIT, src, sizeof src);
	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src, crc));

	/*
	 * A link that mostly says no. §7 puts admission in the spine's hands,
	 * so a session that assumed its send always succeeded would look correct
	 * against a generous test and lose bytes on the air.
	 */
	for (int i = 0; i < 200000; i++) {
		a.credit = (i % 5 == 0) ? 1100 : 0;
		b.credit = 4096;
		asp_service(&a.s);
		asp_service(&b.s);
		deliver(&a);
		deliver(&b);
		if (a.s.tx_state == ASP_XFER_NONE && a.done_count > 0)
			break;
	}

	assert_int_equal(b.dst_len, sizeof src);
	assert_memory_equal(b.dst, src, sizeof src);
	assert_int_equal(b.last_result, ASP_RESULT_OK);
}

/* --- corruption ------------------------------------------------------------- */

static void test_corrupt_content_is_caught_by_crc(void **state)
{
	(void)state;
	static station a, b;
	static uint8_t src[8000];

	fill_pattern(src, sizeof src);
	wire_up(&a, &b, "N0AAA", "N0BBB");
	settle(&a, &b, 20);

	a.src = src;
	a.src_len = sizeof src;

	/* Offer a CRC that does not match the bytes, which is what a resume bug
	 * or a framing bug looks like from the receiver's side. */
	assert_true(asp_offer_file(&a.s, "payload.bin", NULL, sizeof src,
				   0xdeadbeefu));
	settle(&a, &b, 4000);

	assert_int_equal(b.last_result, ASP_RESULT_CRC_MISMATCH);
	assert_int_equal(a.last_result, ASP_RESULT_CRC_MISMATCH);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_transfer_is_byte_identical),
		cmocka_unit_test(test_transfer_resumes_across_a_drop),
		cmocka_unit_test(test_bad_prefix_restarts_from_zero),
		cmocka_unit_test(test_chat_interleaves_with_a_transfer),
		cmocka_unit_test(test_plain_peer_degrades_to_raw),
		cmocka_unit_test(test_unknown_type_mid_transfer),
		cmocka_unit_test(test_hostile_offer_names),
		cmocka_unit_test(test_transfer_survives_a_stingy_link),
		cmocka_unit_test(test_corrupt_content_is_caught_by_crc),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
