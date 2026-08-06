#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/wavwriter.h"

/*
 * Unlike shell/capture.c's pure/impure split, there is no pure half here to
 * test in isolation -- the whole point of this module is the header staying
 * byte-exact across real file I/O and repeated re-patching, so the file I/O
 * itself is what these tests exercise, against a real temp file.
 */

static void read_file(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
	FILE *f = fopen(path, "rb");
	assert_non_null(f);
	*len = fread(buf, 1, cap, f);
	fclose(f);
}

static uint32_t get_u32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
	       | ((uint32_t)p[3] << 24);
}

/* The header is exactly test/golden/shell_tx_wav.c's write_wav, so a reader
 * built for the golden corpus needs no changes to read a live recording. */
static void test_wav_header_matches_golden_format(void **state)
{
	(void)state;
	const char *path = "test-wavwriter.tmp.wav";

	ardop_wav_writer *w = ardop_wav_writer_open(path);
	assert_non_null(w);
	ardop_wav_writer_close(w);

	uint8_t buf[64];
	size_t len = 0;
	read_file(path, buf, sizeof buf, &len);
	assert_int_equal(len, 44);   /* header only: zero samples appended. */

	assert_memory_equal(buf, "RIFF", 4);
	assert_int_equal(get_u32_le(buf + 4), 36);   /* 0 data bytes + 36. */
	assert_memory_equal(buf + 8, "WAVE", 4);
	assert_memory_equal(buf + 12, "fmt ", 4);
	assert_int_equal(get_u32_le(buf + 16), 16);   /* fmt chunk size. */
	assert_int_equal(buf[20] | (buf[21] << 8), 1);   /* PCM. */
	assert_int_equal(buf[22] | (buf[23] << 8), 1);   /* mono. */
	assert_int_equal(get_u32_le(buf + 24), 12000);   /* sample rate. */
	assert_int_equal(get_u32_le(buf + 28), 24000);   /* byte rate. */
	assert_int_equal(buf[32] | (buf[33] << 8), 2);   /* block align. */
	assert_int_equal(buf[34] | (buf[35] << 8), 16);   /* bits/sample. */
	assert_memory_equal(buf + 36, "data", 4);
	assert_int_equal(get_u32_le(buf + 40), 0);

	remove(path);
}

/* The header is re-patched after every append, not just at close -- proving
 * this is what makes a killed-mid-recording file still valid (verified live
 * against the real ardopb binary; this test proves the mechanism the field
 * test relies on). */
static void test_header_repatches_after_every_append(void **state)
{
	(void)state;
	const char *path = "test-wavwriter.tmp2.wav";

	ardop_wav_writer *w = ardop_wav_writer_open(path);
	assert_non_null(w);

	int16_t chunk1[5] = {1, 2, 3, 4, 5};
	ardop_wav_writer_append(w, chunk1, 5);

	/* Read back without closing -- proves the header is already correct on
	 * disk, not only buffered in the writer, at exactly this point. */
	uint8_t buf[64];
	size_t len = 0;
	read_file(path, buf, sizeof buf, &len);
	assert_int_equal(len, 44 + 5 * 2);
	assert_int_equal(get_u32_le(buf + 4), 36 + 5 * 2);
	assert_int_equal(get_u32_le(buf + 40), 5 * 2);
	/* The five samples landed right after the header, byte-exact. */
	assert_memory_equal(buf + 44, chunk1, 10);

	int16_t chunk2[3] = {-1, -2, -3};
	ardop_wav_writer_append(w, chunk2, 3);

	read_file(path, buf, sizeof buf, &len);
	assert_int_equal(len, 44 + 8 * 2);
	assert_int_equal(get_u32_le(buf + 4), 36 + 8 * 2);
	assert_int_equal(get_u32_le(buf + 40), 8 * 2);
	assert_memory_equal(buf + 44, chunk1, 10);
	assert_memory_equal(buf + 54, chunk2, 6);   /* appended, not overwritten. */

	ardop_wav_writer_close(w);
	remove(path);
}

/* NULL and zero-length calls are no-ops, so every loop.c call site can be
 * unconditional. */
static void test_null_and_empty_are_safe(void **state)
{
	(void)state;
	ardop_wav_writer_append(NULL, (int16_t[]){1, 2, 3}, 3);
	ardop_wav_writer_close(NULL);

	const char *path = "test-wavwriter.tmp3.wav";
	ardop_wav_writer *w = ardop_wav_writer_open(path);
	assert_non_null(w);
	ardop_wav_writer_append(w, NULL, 0);

	uint8_t buf[64];
	size_t len = 0;
	read_file(path, buf, sizeof buf, &len);
	assert_int_equal(len, 44);   /* untouched by the no-op append. */

	ardop_wav_writer_close(w);
	remove(path);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_wav_header_matches_golden_format),
		cmocka_unit_test(test_header_repatches_after_every_append),
		cmocka_unit_test(test_null_and_empty_are_safe),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
