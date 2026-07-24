#ifndef ARDOP_MODEM_MODULATE_H_
#define ARDOP_MODEM_MODULATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file modulate.h
 * @brief Turning an encoded frame into transmit audio.
 *
 * The modulator is a pure, sans-I/O producer of samples (see
 * [analysis/10](../../analysis/10-modem-link-design.md)). It opens no device,
 * keys no radio, reads no clock, and never blocks: ardop_mod_begin() lays down a
 * whole frame's worth of 12000 Hz samples into a caller-owned buffer, and
 * ardop_mod_pull() hands them back in chunks on demand.
 *
 * This replaces the inherited `Mod*DataAndPlay` functions, which streamed
 * filtered samples straight to the sound card (keying PTT and busy-waiting on
 * the wall clock along the way). The DSP -- the two-tone leader, the comb and
 * resonator filter, the 4FSK/PSK carriers, the trailer -- is transcribed
 * unchanged and is normative; the golden vectors pin every sample.
 *
 * The output is bit-identical to the inherited modulator for the same encoded
 * bytes and drive level. The "fill the whole frame at begin, drain at pull"
 * strategy is an implementation choice behind this API: a streaming generator
 * could replace it later without any caller change.
 */

/**
 * @brief Nominal sample rate. Time is always this many samples per second.
 */
#define ARDOP_MOD_SAMPLE_RATE 12000

/**
 * @brief A safe upper bound on the samples one frame can produce.
 *
 * The longest frame in the golden corpus is 66300 samples (~5.5 s); this leaves
 * headroom for longer-than-default leaders. A caller sizing the buffer it passes
 * to ardop_mod_begin() to this value cannot be told the frame did not fit.
 */
#define ARDOP_MOD_MAX_SAMPLES 96000

/**
 * @brief Modulator state: the comb/resonator filter and the current frame.
 *
 * Exposed only so the caller can own the storage; the members are an
 * implementation detail. Small enough to hold anywhere. The sample buffer is
 * *not* here -- the caller passes it to ardop_mod_begin() -- so this struct
 * stays a few kilobytes.
 */
typedef struct {
	/* Filter configuration for the frame in progress. */
	int fwidth;         /* bandwidth: 200/500/1000/2000 Hz */
	int first, last;    /* resonator slot range */
	float coef[32];     /* resonator coefficients, per slot */
	float rn, r2;       /* comb/feedback constants derived from r */

	/* Filter running state. */
	float zin, zin_1, zin_2, zcomb;
	float zout_0[32], zout_1[32], zout_2[32];
	int sample_no;      /* input samples fed so far this frame */
	short last120[128]; /* cyclic history for the comb */
	int last120_get, last120_put;

	uint8_t drive_level;  /* 0..100, applied to every input sample */

	/* The frame being drained, owned by the caller. */
	int16_t *frame;
	size_t frame_cap;   /* capacity of frame in samples */
	size_t frame_len;   /* samples written by begin() */
	size_t frame_pos;   /* samples already pulled */
	bool overflow;      /* set if the frame did not fit frame_cap */
} ardop_mod;

/**
 * @brief Initialise a modulator.
 *
 * @param m           Context to initialise.
 * @param drive_level Output level, 0..100, as the inherited `DriveLevel`.
 */
void ardop_mod_init(ardop_mod *m, uint8_t drive_level);

/**
 * @brief Modulate a whole frame into @p sample_buf.
 *
 * Lays down leader, frame-type sync, data carriers and trailer as 12000 Hz
 * int16 samples, ready to be drained by ardop_mod_pull().
 *
 * @param m          An initialised context.
 * @param frame_type The frame type byte (selects modulation via the frame spec).
 * @param encoded    The encoded frame: two frame-type bytes followed by data.
 * @param len        Length of @p encoded.
 * @param leader_ms  Leader length in ms, or 0 for the protocol default (240).
 * @param sample_buf Caller storage for the samples; see ARDOP_MOD_MAX_SAMPLES.
 * @param sample_cap Capacity of @p sample_buf in samples.
 * @return true on success; false if @p frame_type is unknown, its modulation is
 *         not yet supported, or the frame did not fit in @p sample_cap.
 */
ARDOP_MUSTUSE bool ardop_mod_begin(ardop_mod *m, uint8_t frame_type,
				   const uint8_t *encoded, size_t len,
				   uint16_t leader_ms,
				   int16_t *sample_buf, size_t sample_cap);

/**
 * @brief Pull up to @p max samples of the frame in progress.
 *
 * @param m   A context with a frame begun.
 * @param out Destination for the samples.
 * @param max Maximum samples to write.
 * @return The number written; 0 when the frame is fully drained. Never blocks.
 */
size_t ardop_mod_pull(ardop_mod *m, int16_t *out, size_t max);

/** @brief Whether a frame is begun and not yet fully drained. */
bool ardop_mod_busy(const ardop_mod *m);

#endif /* ARDOP_MODEM_MODULATE_H_ */
