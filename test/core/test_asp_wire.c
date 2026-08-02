#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "app/asp_wire.h"

/*
 * ASP/1's wire format.
 *
 * analysis/17's exit criteria ask for something specific and unusual: *"Two
 * independent implementations of the framing -- the app's and the test
 * harness's -- agree"*, and *"the spec is sufficient to write the second
 * implementation without reading the first one's source"*.
 *
 * So the decoder below is written from the document, not from asp_wire.c. Where
 * it and the implementation disagree, one of them is wrong and it is worth
 * finding out which -- which is a stronger property than a round trip through a
 * single codec, since a round trip is equally happy if both halves are wrong in
 * the same way.
 */

/* --- the second implementation, from analysis/17 §3 ------------------------- */

/*
 * §3: "varint is unsigned LEB128: 7 bits of value per byte, least-significant
 * group first, high bit set on all but the last. Capped at 4 bytes."
 */
static int ref_varint(const uint8_t *b, size_t n, uint32_t *out)
{
	uint32_t v = 0;
	for (size_t i = 0; i < n && i < 4; i++) {
		v |= (uint32_t)(b[i] & 0x7f) << (7 * i);
		if ((b[i] & 0x80) == 0) {
			*out = v;
			return (int)i + 1;
		}
	}
	return -1;
}

/*
 * §3: "type u8, length varint, payload length bytes"; maximum payload 4096;
 * 0x00 reserved.
 */
static int ref_frame(const uint8_t *b, size_t n, uint8_t *type,
		     const uint8_t **payload, uint32_t *len)
{
	if (n < 1 || b[0] == 0x00)
		return -1;
	uint32_t plen;
	int vn = ref_varint(b + 1, n - 1, &plen);
	if (vn < 0 || plen > 4096)
		return -1;
	if (n < 1 + (size_t)vn + plen)
		return 0;   /* incomplete */
	*type = b[0];
	*payload = b + 1 + vn;
	*len = plen;
	return 1 + vn + (int)plen;
}

/* --- varints ---------------------------------------------------------------- */

static void test_varint_round_trip(void **state)
{
	(void)state;

	/* The boundaries where the encoding grows a byte, which is where an
	 * off-by-one in the shift or the continuation bit shows up. */
	static const uint32_t kValues[] = {
		0, 1, 63, 127, 128, 129, 255, 16383, 16384, 65535,
		2097151, 2097152, 4096, 268435455,
	};

	for (size_t i = 0; i < sizeof kValues / sizeof kValues[0]; i++) {
		uint8_t buf[8] = {0};
		size_t n = asp_varint_put(buf, sizeof buf, kValues[i]);
		assert_true(n >= 1 && n <= 4);

		uint32_t back = 0;
		size_t used = 0;
		assert_true(asp_varint_get(buf, n, &back, &used));
		assert_int_equal(back, kValues[i]);
		assert_int_equal(used, n);

		/* And the independent decoder agrees on both value and length. */
		uint32_t ref = 0;
		assert_int_equal(ref_varint(buf, n, &ref), (int)n);
		assert_int_equal(ref, kValues[i]);
	}
}

static void test_varint_refuses_a_fifth_byte(void **state)
{
	(void)state;

	/* Five continuation bytes: the cap exists so a decoder's bound is
	 * static, so this must fail rather than read on. */
	const uint8_t overlong[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
	uint32_t v = 0;
	size_t used = 0;
	assert_false(asp_varint_get(overlong, sizeof overlong, &v, &used));
	assert_int_equal(ref_varint(overlong, sizeof overlong, &v), -1);

	/* A value needing five bytes cannot be encoded either. */
	assert_int_equal(asp_varint_put((uint8_t[8]){0}, 8, 0xffffffffu), 0);
}

/* --- framing ---------------------------------------------------------------- */

static void test_frame_round_trip(void **state)
{
	(void)state;

	uint8_t buf[ASP_MAX_MESSAGE];
	const char *body = "hello, this is a message";
	size_t n = asp_frame_put(buf, sizeof buf, ASP_MSG_TEXT, body,
				 strlen(body));
	assert_true(n > 0);

	asp_msg_type type;
	const uint8_t *payload;
	size_t len, consumed;
	assert_int_equal(asp_frame_get(buf, n, &type, &payload, &len, &consumed),
			 ASP_FRAME_OK);
	assert_int_equal(type, ASP_MSG_TEXT);
	assert_int_equal(len, strlen(body));
	assert_memory_equal(payload, body, len);
	assert_int_equal(consumed, n);

	/* The independent decoder reads the same bytes the same way. */
	uint8_t rtype;
	const uint8_t *rpay;
	uint32_t rlen;
	assert_int_equal(ref_frame(buf, n, &rtype, &rpay, &rlen), (int)n);
	assert_int_equal(rtype, ASP_MSG_TEXT);
	assert_int_equal(rlen, strlen(body));
	assert_memory_equal(rpay, body, rlen);
}

static void test_frame_incomplete_is_not_an_error(void **state)
{
	(void)state;

	uint8_t buf[ASP_MAX_MESSAGE];
	const char *body = "a message long enough to arrive in pieces";
	size_t n = asp_frame_put(buf, sizeof buf, ASP_MSG_TEXT, body,
				 strlen(body));

	/* Every proper prefix must say "wait", never "broken" -- a stream
	 * arriving a few bytes at a time is the normal case, and a decoder that
	 * called that a protocol error would disconnect on the first message. */
	for (size_t avail = 0; avail < n; avail++) {
		asp_msg_type type;
		const uint8_t *payload;
		size_t len, consumed;
		assert_int_equal(asp_frame_get(buf, avail, &type, &payload,
					       &len, &consumed),
				 ASP_FRAME_SHORT);
	}
}

static void test_frame_rejects_the_malformed(void **state)
{
	(void)state;

	asp_msg_type type;
	const uint8_t *payload;
	size_t len, consumed;

	/* 0x00 is reserved so a run of zeros can never parse. */
	const uint8_t zeros[8] = {0};
	assert_int_equal(asp_frame_get(zeros, sizeof zeros, &type, &payload,
				       &len, &consumed),
			 ASP_FRAME_ERROR);

	/* A declared length beyond the 4096 cap. 0x8020 varint == 4096+... */
	const uint8_t huge[] = {ASP_MSG_TEXT, 0x81, 0xa0, 0x01};
	assert_int_equal(asp_frame_get(huge, sizeof huge, &type, &payload, &len,
				       &consumed),
			 ASP_FRAME_ERROR);

	/* An overlong varint in the length field. */
	const uint8_t over[] = {ASP_MSG_TEXT, 0x80, 0x80, 0x80, 0x80, 0x01};
	assert_int_equal(asp_frame_get(over, sizeof over, &type, &payload, &len,
				       &consumed),
			 ASP_FRAME_ERROR);

	/* And the encoder refuses to produce an oversize payload rather than
	 * emitting something no conforming decoder would accept. */
	static uint8_t big[ASP_MAX_PAYLOAD + 1];
	uint8_t out[ASP_MAX_MESSAGE + 16];
	assert_int_equal(asp_frame_put(out, sizeof out, ASP_MSG_DATA, big,
				       sizeof big),
			 0);
}

static void test_unknown_type_is_skipped_by_length(void **state)
{
	(void)state;

	/*
	 * §3's forward-compatibility rule, and the reason length precedes
	 * payload for every type: a version 2 station's new message must cost a
	 * version 1 station nothing but a skip.
	 */
	uint8_t buf[256];
	size_t n = 0;
	const char *before = "before";
	const char *after = "after";

	n += asp_frame_put(buf + n, sizeof buf - n, ASP_MSG_TEXT, before,
			   strlen(before));
	n += asp_frame_put(buf + n, sizeof buf - n, (asp_msg_type)0x7e,
			   "a type from the future", 22);
	n += asp_frame_put(buf + n, sizeof buf - n, ASP_MSG_TEXT, after,
			   strlen(after));

	size_t off = 0;
	int texts = 0, unknown = 0;
	while (off < n) {
		asp_msg_type type;
		const uint8_t *payload;
		size_t len, consumed;
		assert_int_equal(asp_frame_get(buf + off, n - off, &type,
					       &payload, &len, &consumed),
				 ASP_FRAME_OK);
		if (type == ASP_MSG_TEXT)
			texts++;
		else
			unknown++;
		off += consumed;
	}
	assert_int_equal(texts, 2);
	assert_int_equal(unknown, 1);
	assert_int_equal(off, n);
}

/* --- CRC-32 ----------------------------------------------------------------- */

static void test_crc32_known_answers(void **state)
{
	(void)state;

	/*
	 * The published check values for CRC-32/ISO-HDLC (the zlib polynomial),
	 * which is what §5 names. Fixed constants rather than a self-consistency
	 * check, because "our CRC agrees with itself" would pass with the wrong
	 * polynomial and then disagree with every other implementation on the
	 * air.
	 */
	assert_int_equal(asp_crc32(ASP_CRC32_INIT, "123456789", 9), 0xcbf43926u);
	assert_int_equal(asp_crc32(ASP_CRC32_INIT, "", 0), 0x00000000u);
	assert_int_equal(asp_crc32(ASP_CRC32_INIT, "a", 1), 0xe8b7be43u);
	assert_int_equal(asp_crc32(ASP_CRC32_INIT, "The quick brown fox jumps "
					     "over the lazy dog",
				   43),
			 0x414fa339u);
}

static void test_crc32_streams(void **state)
{
	(void)state;

	/* A file is checksummed as it is written, so the split must not matter. */
	const char *whole = "the quick brown fox jumps over the lazy dog";
	const size_t n = strlen(whole);
	const uint32_t once = asp_crc32(ASP_CRC32_INIT, whole, n);

	for (size_t cut = 0; cut <= n; cut++) {
		uint32_t crc = asp_crc32(ASP_CRC32_INIT, whole, cut);
		crc = asp_crc32(crc, whole + cut, n - cut);
		assert_int_equal(crc, once);
	}
}

/* --- payloads --------------------------------------------------------------- */

static void test_hello_round_trip(void **state)
{
	(void)state;

	uint8_t buf[ASP_MAX_PAYLOAD];
	asp_hello out = {.version = ASP_VERSION,
			 .caps = ASP_CAP_FILES | ASP_CAP_RESUME};
	snprintf(out.call, sizeof out.call, "N0CALL-4");

	size_t n = asp_hello_put(buf, sizeof buf, &out);
	assert_true(n > 0);

	asp_hello in = {0};
	assert_true(asp_hello_get(buf, n, &in));
	assert_int_equal(in.version, ASP_VERSION);
	assert_int_equal(in.caps, ASP_CAP_FILES | ASP_CAP_RESUME);
	assert_string_equal(in.call, "N0CALL-4");

	/* §2: the decision is made on the magic. Anything else is not ASP, and
	 * must not be coaxed into looking like it. */
	const char *plain = "hello, is anyone there?";
	assert_false(asp_hello_get((const uint8_t *)plain, strlen(plain), &in));
}

static void test_offer_round_trip(void **state)
{
	(void)state;

	uint8_t buf[ASP_MAX_PAYLOAD];
	asp_offer out = {.id = 0x1234, .size = 1048577, .crc32 = 0xdeadbeefu};
	snprintf(out.name, sizeof out.name, "winlink-inbox.b2f");
	snprintf(out.content_type, sizeof out.content_type,
		 "application/octet-stream");

	size_t n = asp_offer_put(buf, sizeof buf, &out);
	assert_true(n > 0);

	asp_offer in = {0};
	assert_true(asp_offer_get(buf, n, &in));
	assert_int_equal(in.id, 0x1234);
	assert_int_equal(in.size, 1048577);
	assert_int_equal(in.crc32, 0xdeadbeefu);
	assert_string_equal(in.name, "winlink-inbox.b2f");
	assert_string_equal(in.content_type, "application/octet-stream");

	/* Truncated at every length: none may be read as a valid offer. */
	for (size_t cut = 0; cut < n; cut++)
		assert_false(asp_offer_get(buf, cut, &in));
}

static void test_small_payloads_round_trip(void **state)
{
	(void)state;

	uint8_t buf[64];

	asp_accept acc = {.id = 7, .have = 65536, .prefix_crc = 0x01020304u};
	size_t n = asp_accept_put(buf, sizeof buf, &acc);
	asp_accept back = {0};
	assert_true(asp_accept_get(buf, n, &back));
	assert_int_equal(back.id, 7);
	assert_int_equal(back.have, 65536);
	assert_int_equal(back.prefix_crc, 0x01020304u);

	n = asp_start_put(buf, sizeof buf, 9, 4096);
	uint16_t id = 0;
	uint32_t from = 0;
	assert_true(asp_start_get(buf, n, &id, &from));
	assert_int_equal(id, 9);
	assert_int_equal(from, 4096);

	n = asp_id_code_put(buf, sizeof buf, 11, ASP_RESULT_CRC_MISMATCH);
	uint8_t code = 0;
	assert_true(asp_id_code_get(buf, n, &id, &code));
	assert_int_equal(id, 11);
	assert_int_equal(code, ASP_RESULT_CRC_MISMATCH);

	n = asp_id_put(buf, sizeof buf, 0xffff);
	assert_true(asp_id_get(buf, n, &id));
	assert_int_equal(id, 0xffff);
}

static void test_text_b_round_trip(void **state)
{
	(void)state;

	uint8_t buf[ASP_MAX_PAYLOAD];
	asp_text_b out = {.msg_id = 42};
	snprintf(out.call, sizeof out.call, "W1AW");
	const char *msg = "CQ CQ de W1AW, anyone about?";
	out.text_len = strlen(msg);
	memcpy(out.text, msg, out.text_len);

	size_t n = asp_text_b_put(buf, sizeof buf, &out);
	assert_true(n > 0);

	asp_text_b in = {0};
	assert_true(asp_text_b_get(buf, n, &in));
	assert_string_equal(in.call, "W1AW");
	assert_int_equal(in.msg_id, 42);
	assert_int_equal(in.text_len, strlen(msg));
	assert_memory_equal(in.text, msg, in.text_len);

	/* §6: a TEXT_B that fails to parse is displayed as raw text rather than
	 * discarded, so failing cleanly matters. */
	const char *garbage = "\xff\xfe not a broadcast";
	assert_false(asp_text_b_get((const uint8_t *)garbage, 2, &in));
}

/* --- filenames -------------------------------------------------------------- */

static void test_hostile_filenames(void **state)
{
	(void)state;

	char out[ASP_MAX_NAME + 1];

	/* §10 test 7, verbatim: the four names the document names. */
	assert_true(asp_safe_name("../../etc/passwd", out, sizeof out));
	assert_string_equal(out, "passwd");

	assert_true(asp_safe_name("C:\\Windows\\x", out, sizeof out));
	assert_string_equal(out, "x");

	assert_true(asp_safe_name("CON", out, sizeof out));
	assert_string_equal(out, "CON_");

	assert_true(asp_safe_name("two\nlines.txt", out, sizeof out));
	assert_string_equal(out, "two_lines.txt");

	/* Mixed separators, because a Windows peer's name reaches a Unix host
	 * unchanged and "handle my own platform's separator" is how ..\..\
	 * survives. */
	assert_true(asp_safe_name("..\\..\\..\\etc/shadow", out, sizeof out));
	assert_string_equal(out, "shadow");

	/* A reserved device with an extension is still a device. */
	assert_true(asp_safe_name("nul.txt", out, sizeof out));
	assert_string_equal(out, "nul.txt_");

	/* Leading dots hide it; trailing dots and spaces are stripped by
	 * Windows, so the name on disk would differ from the one reported. */
	assert_true(asp_safe_name(".hidden", out, sizeof out));
	assert_string_equal(out, "hidden");
	assert_true(asp_safe_name("report.txt.  ", out, sizeof out));
	assert_string_equal(out, "report.txt");

	/* Nothing usable left: refused rather than given a name nobody chose. */
	assert_false(asp_safe_name("", out, sizeof out));
	assert_false(asp_safe_name("/", out, sizeof out));
	assert_false(asp_safe_name("../../", out, sizeof out));
	assert_false(asp_safe_name("...", out, sizeof out));

	/* An ordinary name survives untouched, which is the case that matters
	 * most and the one a paranoid sanitiser gets wrong. */
	assert_true(asp_safe_name("Field Day 2026 log.adi", out, sizeof out));
	assert_string_equal(out, "Field Day 2026 log.adi");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_varint_round_trip),
		cmocka_unit_test(test_varint_refuses_a_fifth_byte),
		cmocka_unit_test(test_frame_round_trip),
		cmocka_unit_test(test_frame_incomplete_is_not_an_error),
		cmocka_unit_test(test_frame_rejects_the_malformed),
		cmocka_unit_test(test_unknown_type_is_skipped_by_length),
		cmocka_unit_test(test_crc32_known_answers),
		cmocka_unit_test(test_crc32_streams),
		cmocka_unit_test(test_hello_round_trip),
		cmocka_unit_test(test_offer_round_trip),
		cmocka_unit_test(test_small_payloads_round_trip),
		cmocka_unit_test(test_text_b_round_trip),
		cmocka_unit_test(test_hostile_filenames),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
