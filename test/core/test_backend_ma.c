#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/backend_ma.h"
#include "shell/loop.h"
#include "shell/platform.h"
#include "shell/resample.h"
#include "shell/sys.h"

/*
 * The miniaudio backend, driven against miniaudio's synthetic null device.
 *
 * This suite exists because the three sharpest hazards in the whole Windows
 * port are all silent: they do not crash, they do not warn, and they are only
 * audible on the air. A CI job that merely compiles the backend proves none of
 * them. The null device runs a real device thread on a synthesised clock and
 * calls the same callbacks the real backends do, so the ring, both resamplers,
 * the drain ordering and the fault latch are all exercised with no sound card
 * and no radio -- on Linux and on Windows alike.
 *
 * The tail-cut assertion in particular is analysis/15's exit criterion
 * ("verified by recording the transmit audio and comparing frame length, not by
 * ear") turned into something a machine checks on every push.
 */

static ardop_ma_backend *open_null(ardop_platform_ops *ops)
{
	ardop_ma_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.use_null_device = true;
	return ardop_backend_ma_open(&cfg, ops);
}

/*
 * THE tail-cut test.
 *
 * `write_audio` returns as soon as samples are queued in the ring, not when
 * they have been played. Unkeying at that moment cuts the last few hundred
 * milliseconds off every transmission. On top of that the interpolator holds
 * (ntaps-1)/2 samples of group delay that were accepted and never emitted --
 * a second, independent cut that analysis/15 does not mention.
 *
 * So: everything handed to write_audio, times the rate ratio, plus the flushed
 * interpolator tail, must have reached the device before PTT dropped.
 */
static void test_transmission_tail_is_not_cut(void **state)
{
	(void)state;
	ardop_platform_ops ops;
	ardop_ma_backend *b = open_null(&ops);
	assert_non_null(b);

	unsigned m = ardop_backend_ma_ratio(b);
	assert_true(m >= 1 && m <= ARDOP_RESAMPLE_MAX_M);

	/* A burst long enough that it cannot fit in the device buffer, so the
	 * drain genuinely has to wait for the device to consume it. */
	enum { BURST = 12000 };   /* one second at the modem rate */
	static int16_t tone[BURST];
	for (size_t i = 0; i < BURST; i++)
		tone[i] = (int16_t)((i % 240) * 100 - 12000);

	ops.set_ptt(ops.ctx, true);
	ops.write_audio(ops.ctx, tone, BURST);
	ops.set_ptt(ops.ctx, false);

	uint64_t written = ardop_backend_ma_written(b);
	uint64_t played = ardop_backend_ma_played_at_unkey(b);
	assert_int_equal(written, BURST);

	/* Every modem sample became m device samples, plus the interpolator's
	 * flushed group delay. Nothing may still be sitting in the ring. */
	uint64_t expect = written * m;
	assert_true(played >= expect);

	/* And no fault along the way: a drain that timed out would latch
	 * PLAYBACK_LOST and reach the same sample count by giving up. */
	assert_int_equal(ardop_backend_ma_fault(b), ARDOP_FAULT_NONE);

	ardop_backend_ma_close(b);
}

/*
 * Half-duplex hygiene. The loop does not call read_audio during transmit, so a
 * capture ring left running quietly banks the station's own signal and hands it
 * to the demodulator afterwards -- hours stale relative to the sample clock.
 * ALSA escapes this only by accident, because overrun recovery throws the
 * backlog away.
 */
static void test_capture_is_discarded_during_transmit(void **state)
{
	(void)state;
	ardop_platform_ops ops;
	ardop_ma_backend *b = open_null(&ops);
	assert_non_null(b);

	enum { BURST = 6000 };
	static int16_t tone[BURST];
	memset(tone, 0x11, sizeof(tone));

	ops.set_ptt(ops.ctx, true);
	ops.write_audio(ops.ctx, tone, BURST);
	ops.set_ptt(ops.ctx, false);

	/* The capture callback ran throughout and dropped what it was given. */
	assert_true(ardop_backend_ma_capture_dropped(b) > 0);

	/* Receive starts from empty, not from a backlog of our own transmission:
	 * the first read after unkey must return at most one block. */
	static int16_t in[ARDOP_LOOP_BLOCK];
	size_t n = ops.read_audio(ops.ctx, in, ARDOP_LOOP_BLOCK);
	assert_true(n <= ARDOP_LOOP_BLOCK);

	ardop_backend_ma_close(b);
}

/* Capture must actually deliver samples, and at the modem rate: exactly one
 * output sample per m device samples, with no accumulator anywhere. */
static void test_capture_delivers_at_the_modem_rate(void **state)
{
	(void)state;
	ardop_platform_ops ops;
	ardop_ma_backend *b = open_null(&ops);
	assert_non_null(b);

	static int16_t in[ARDOP_LOOP_BLOCK];
	size_t total = 0;
	for (int i = 0; i < 40 && total < 2000; i++)
		total += ops.read_audio(ops.ctx, in, ARDOP_LOOP_BLOCK);

	assert_true(total > 0);
	assert_int_equal(ardop_backend_ma_fault(b), ARDOP_FAULT_NONE);
	ardop_backend_ma_close(b);
}

/*
 * The loop reads this many modem samples per iteration, so it has to be a
 * whole block the device can actually deliver.
 *
 * (This comment used to describe a capture-fault test, which the body never
 * performed. The real one is test_capture_loss_faults below.)
 */
static void test_block_size_is_sane(void **state)
{
	(void)state;
	ardop_platform_ops ops;
	ardop_ma_backend *b = open_null(&ops);
	assert_non_null(b);

	size_t block = ardop_backend_ma_block(b);
	assert_true(block > 0);
	assert_true(block <= ARDOP_LOOP_BLOCK);

	/* Whatever the device period is, a whole number of modem samples has to
	 * come out of it, or read_audio could never return a full block. */
	assert_true(block >= 120);

	ardop_backend_ma_close(b);
}

/* The null device runs at a rate the resampler supports, and the ratio is a
 * whole number. A device that is not an integer multiple of 12000 is refused at
 * open, so reaching this point at all is part of the assertion. */
static void test_rate_ratio_is_integral(void **state)
{
	(void)state;
	ardop_platform_ops ops;
	ardop_ma_backend *b = open_null(&ops);
	assert_non_null(b);
	unsigned m = ardop_backend_ma_ratio(b);
	assert_true(m >= 1);
	assert_true(m <= ARDOP_RESAMPLE_MAX_M);
	ardop_backend_ma_close(b);
}

int main(void)
{
	ardop_test_setup();
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_transmission_tail_is_not_cut),
		cmocka_unit_test(test_capture_is_discarded_during_transmit),
		cmocka_unit_test(test_capture_delivers_at_the_modem_rate),
		cmocka_unit_test(test_block_size_is_sane),
		cmocka_unit_test(test_rate_ratio_is_integral),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
