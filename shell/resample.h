#ifndef ARDOP_SHELL_RESAMPLE_H_
#define ARDOP_SHELL_RESAMPLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file resample.h
 * @brief Anti-aliased integer rate conversion between a device and the modem.
 *
 * Sound cards do not generally run at 12000 Hz. Windows shared-mode WASAPI
 * hands you the mix format, Android will not give you 12000 Hz at all, and on
 * Linux only the ALSA `plug` layer hides it -- at the cost of a resampler
 * whose quality varies by configuration. So conversion is ours to do
 * ([analysis/15](../analysis/15-platform-audio-and-ptt.md) §5).
 *
 * **Only integer ratios, deliberately.** [analysis/06](../analysis/06-target-architecture.md)
 * Rule 2 makes the sample clock *the* protocol clock. 48000 -> 12000 is exactly
 * 4:1 forever, with no accumulator: if the card really runs at 47988 Hz the
 * modem runs at 11997 Hz and every protocol deadline stretches by the same
 * 0.025%, which is the property that dissolved `FixTiming`. A fractional
 * resampler carries its own rational accumulator and inserts or drops samples
 * on *its* schedule, decoupling protocol time from device time -- reintroducing
 * exactly the class of bug this architecture was built to delete. A device rate
 * that is not a multiple of 12000 is therefore refused, not approximated.
 *
 * **The anti-alias filter is not optional.** Taking every fourth sample of a
 * 48 kHz stream folds everything from 6 kHz to 24 kHz into the passband -- noise
 * added directly to the busy detector's spectrum and the demodulator's S/N
 * estimate, on every platform that resamples. The transmit direction needs the
 * mirror image or we radiate images of our own signal.
 *
 * Pure: no I/O, no clock, no globals, no allocation. Unit-tested in
 * `test/core/test_resample.c` like the rest of that set.
 */

/** @brief Highest supported ratio (96000 Hz / 12000 Hz). */
#define ARDOP_RESAMPLE_MAX_M 8u

/** @brief Prototype length for ratio @p m. 24 taps per phase. */
#define ARDOP_RESAMPLE_TAPS(m) (24u * (unsigned)(m) + 1u)

/** @brief Prototype length at ::ARDOP_RESAMPLE_MAX_M. */
#define ARDOP_RESAMPLE_MAX_TAPS ARDOP_RESAMPLE_TAPS(ARDOP_RESAMPLE_MAX_M)

/** @brief Which way the converter runs. */
typedef enum {
	ARDOP_RESAMPLE_DECIMATE,     /**< Device -> modem: m samples in, 1 out. */
	ARDOP_RESAMPLE_INTERPOLATE   /**< Modem -> device: 1 sample in, m out. */
} ardop_resample_dir;

/**
 * @brief One direction of conversion. Zero-initialise then ::ardop_resample_init.
 *
 * Two of these live in a backend: one decimating capture, one interpolating
 * playback. They do not share state.
 */
typedef struct {
	ardop_resample_dir dir;
	unsigned m;        /**< Ratio, 1..::ARDOP_RESAMPLE_MAX_M. */
	unsigned ntaps;    /**< ::ARDOP_RESAMPLE_TAPS(m). */
	unsigned nhist;    /**< History depth in samples at the *input* rate. */
	float h[ARDOP_RESAMPLE_MAX_TAPS];   /**< Prototype low-pass. */
	/* Written twice, at i and i+nhist, so any window of nhist samples is
	 * contiguous and the inner loop needs no modulo. */
	float hist[2 * ARDOP_RESAMPLE_MAX_TAPS];
	unsigned hpos;     /**< Next write index, 0..nhist-1. */
} ardop_resampler;

/**
 * @brief Build the filter for ratio @p m and clear the delay line.
 *
 * @p m == 1 is the identity: ::ardop_resample copies bit-for-bit, so a native
 * 12 kHz device pays nothing and cannot be perturbed.
 *
 * @return false if @p m is 0 or above ::ARDOP_RESAMPLE_MAX_M.
 */
bool ardop_resample_init(ardop_resampler *rs, ardop_resample_dir dir, unsigned m);

/**
 * @brief Convert @p n input samples into @p out.
 *
 * Decimating, @p n must be a multiple of `m` (the caller owns the framing --
 * see ::ardop_resample_in_for) and the result is `n / m`. Interpolating, the
 * result is `n * m`. @p out must have room for the return value; @p in and
 * @p out must not overlap.
 *
 * @return Samples written to @p out.
 */
size_t ardop_resample(ardop_resampler *rs, const int16_t *in, size_t n,
		      int16_t *out);

/**
 * @brief Largest input count that yields at most @p out_max output samples.
 *
 * Decimating this is `out_max * m` rounded to the ratio; interpolating it is
 * `out_max / m`.
 */
size_t ardop_resample_in_for(const ardop_resampler *rs, size_t out_max);

/**
 * @brief Push the delay line out and reset.
 *
 * **This is a tail-cut hazard in its own right.** A linear-phase FIR holds
 * `(ntaps - 1) / 2` samples of group delay, so when a transmission ends, that
 * much audio has been handed to ::ardop_resample and has *not* come out the
 * other side. A backend that unkeys PTT without flushing first clips the end of
 * every frame -- separately from, and in addition to, the playback ring not
 * having drained. Call this before waiting on the ring.
 *
 * @return Samples written to @p out (at most @p max).
 */
size_t ardop_resample_flush(ardop_resampler *rs, int16_t *out, size_t max);

/** @brief Output samples ::ardop_resample_flush would produce given room. */
size_t ardop_resample_pending(const ardop_resampler *rs);

/** @brief Clear the delay line without emitting it (PTT key, device restart). */
void ardop_resample_reset(ardop_resampler *rs);

#endif /* ARDOP_SHELL_RESAMPLE_H_ */
