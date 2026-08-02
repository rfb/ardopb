#ifndef ARDOP_SHELL_BACKEND_MA_H_
#define ARDOP_SHELL_BACKEND_MA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/audio_devices.h"
#include "shell/fault.h"
#include "shell/platform.h"
#include "shell/ptt.h"

/**
 * @file backend_ma.h
 * @brief The cross-platform audio backend: WASAPI, CoreAudio, AAudio, ALSA.
 *
 * Built on the vendored miniaudio (`third_party/README.md`), behind the six
 * function pointers of ::ardop_platform_ops. This is the only audio backend:
 * a dedicated ALSA one existed and was removed, because two Linux paths meant
 * two implementations of the transmit-tail drain and only one of them was
 * covered by the tests (analysis/15 Open decision 1).
 *
 * The handle is opaque so that no translation unit outside `backend_ma.c` and
 * `audio_devices.c` ever names a miniaudio type.
 *
 * ## Two things a caller must know
 *
 * 1. **Device rates must be an integer multiple of 12000.** The sample clock is
 *    the protocol clock (analysis/06 Rule 2), and only integer decimation
 *    preserves that -- if the card runs slightly fast, the modem runs slightly
 *    fast and every deadline stretches identically. A rate like 44100 is
 *    refused with a message rather than approximated, because a fractional
 *    resampler would decouple protocol time from device time.
 * 2. **Faults are latched and must be polled.** See fault.h: `set_ptt` returns
 *    void and `read_audio` returns a count, so an unplugged device or a dropped
 *    CAT link reaches the application through ::ardop_backend_ma_fault.
 */

typedef struct ardop_ma_backend ardop_ma_backend;

/** @brief What to open. NULL device selectors mean "the system default". */
typedef struct {
	const char *capture_id;    /**< Device id or name; NULL for default. */
	const char *playback_id;
	ardop_ptt *ptt;            /**< Borrowed, may be NULL (VOX). */
	bool use_null_device;      /**< miniaudio's synthetic device, for tests. */
	/**
	 * @brief Force one miniaudio backend by name, or NULL to auto-select.
	 *
	 * "alsa", "pulseaudio", "wasapi", "coreaudio", "jack" ... The reason
	 * this exists: auto-selection prefers PulseAudio on Linux, and an
	 * operator who wants to bypass the sound server -- for latency, or
	 * because their interface is exposed more directly by ALSA -- needs a
	 * way to say so. That was the one thing the dedicated ALSA backend
	 * could do that this could not.
	 */
	const char *backend_name;
} ardop_ma_config;

/**
 * @brief Open capture and playback and fill in @p ops.
 * @return NULL on failure, with a diagnostic already printed.
 */
ardop_ma_backend *ardop_backend_ma_open(const ardop_ma_config *cfg,
					ardop_platform_ops *ops);

/** @brief Stop the devices and release everything. Unkeys on the way out. */
void ardop_backend_ma_close(ardop_ma_backend *b);

/** @brief The latched fault, or ::ARDOP_FAULT_NONE. */
ardop_fault ardop_backend_ma_fault(const ardop_ma_backend *b);

/*
 * There is deliberately no clear-fault and no reopen.
 *
 * A latched fault means the device is gone, not that a flag is set: after
 * ::ARDOP_FAULT_CAPTURE_LOST the underlying device is untouched and
 * unrecovered, so clearing the latch would only arrange to re-latch it on the
 * next read. A function whose name promises recovery it does not perform is
 * worse than no function at all -- and this one had no callers.
 *
 * Recovery is ardop_backend_ma_close() then ardop_backend_ma_open(), which works
 * because the runtime outlives the backend by design (analysis/14 Decision 5):
 * a rebuilt device inherits the link, the configuration and the sample clock.
 * That is also the path `--audio` takes at start-up, so it is exercised on every
 * run rather than only after a failure.
 */

/** @brief How the capture selection resolved. ::ARDOP_AUDIO_MATCH_ID is clean. */
ardop_audio_match ardop_backend_ma_capture_match(const ardop_ma_backend *b);

/** @brief How the playback selection resolved. */
ardop_audio_match ardop_backend_ma_playback_match(const ardop_ma_backend *b);

/**
 * @brief The capture device actually opened, as it would be persisted.
 *
 * The point of returning the whole device rather than only the match: when a
 * selection resolved by *name* because the id moved, this carries the new id, so
 * an application can write it back and the next start matches exactly. Without
 * it the resolution rule degrades to name-matching forever.
 */
void ardop_backend_ma_capture_device(const ardop_ma_backend *b,
				     ardop_audio_device *out);

/** @brief The playback device actually opened. */
void ardop_backend_ma_playback_device(const ardop_ma_backend *b,
				      ardop_audio_device *out);

/**
 * @brief Samples the driver loop should move per iteration.
 *
 * Derived from the device period, clamped to ::ARDOP_LOOP_BLOCK. Assign it to
 * ::ardop_loop::block: a device with small periods should not be driven in
 * 100 ms lumps, because capture-to-link latency comes straight off the 250 ms
 * ARQ turnaround budget.
 */
size_t ardop_backend_ma_block(const ardop_ma_backend *b);

/** @brief The device's sample rate divided by 12000. */
unsigned ardop_backend_ma_ratio(const ardop_ma_backend *b);

/* --- observation, for tests ------------------------------------------------ */

/**
 * @brief **For tests only.** Stop the capture device so `read_audio` times out.
 *
 * The strongest available stand-in for the cable being pulled: the callback
 * simply stops, exactly as it does when a USB interface is unplugged. It exists
 * because analysis/15's exit criterion -- "unplugging the capture device
 * mid-session raises a fault ... it does not hang the modem thread" -- is
 * otherwise a claim no automated test can reach, and a claim about a fault path
 * that has never been executed is not worth much.
 */
void ardop_backend_ma_stall_capture(ardop_ma_backend *b);

/** @brief Device-rate samples the playback callback has actually consumed. */
uint64_t ardop_backend_ma_played(const ardop_ma_backend *b);

/** @brief Device-rate samples ::ardop_backend_ma_played held at the last unkey. */
uint64_t ardop_backend_ma_played_at_unkey(const ardop_ma_backend *b);

/** @brief Modem-rate samples handed to `write_audio` since the last key-up. */
uint64_t ardop_backend_ma_written(const ardop_ma_backend *b);

/** @brief Capture samples discarded because the station was transmitting. */
uint64_t ardop_backend_ma_capture_dropped(const ardop_ma_backend *b);

#endif /* ARDOP_SHELL_BACKEND_MA_H_ */
