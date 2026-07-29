#ifndef ARDOP_SHELL_PLATFORM_H_
#define ARDOP_SHELL_PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link/link.h"   /* ardop_host_cmd */

/**
 * @file platform.h
 * @brief The impure boundary: the narrow set of device operations a backend
 *        provides, behind which ALSA, WinMM, or a test stub sit interchangeably.
 *
 * This is the `platform` layer of [analysis/06](../analysis/06-target-architecture.md):
 * audio in/out, PTT, the wall clock (logging and the regulatory ID only), and an
 * optional host-command source. Everything above it -- demod, link, mod -- is the
 * pure ::ardop_runtime, which reads no device and no clock. The driver loop
 * ([loop.h](loop.h)) is the only code that calls through this interface, so a
 * backend is a table of functions and nothing more; there is no per-OS copy of
 * the poll/flush spine that `ALSASound.c` and `Waveout.c` each carried.
 *
 * Rule (analysis/06 §2): **audio capture is the only clock.** ::ardop_platform_ops
 * ::read_audio returns the sample count that advances time; ::wall_ms is for human
 * timestamps and the FCC/regulatory ID interval, never for protocol deadlines.
 * That is what removes the `FixTiming` / drift / `txSleep` class of bugs by
 * construction.
 *
 * Half-duplex is the backend's business: a real radio cannot capture while it
 * transmits, so the loop drains a whole transmission through ::write_audio before
 * returning to ::read_audio, and the backend keys PTT (via the runtime's PTT
 * callback) around it. The interface does not model a capture handle -- opening,
 * closing and reopening the device is entirely inside the backend.
 */

/**
 * @brief A platform backend: a table of device operations plus its own context.
 *
 * @p ctx is passed back to every call; it holds the backend's private state
 * (device handles, buffers, config). The three audio/PTT hooks are required; the
 * rest may be NULL, in which case the loop supplies a sensible default (no host
 * input, a monotonic-from-samples clock, never stop).
 */
typedef struct ardop_platform_ops {
	void *ctx;

	/**
	 * @brief Capture up to @p max samples; the return value advances the clock.
	 *
	 * The single clock source (analysis/06 §2). May block until samples are
	 * available (a real device paces this); must return promptly with a full
	 * or partial block so the loop can service timers. Returning 0 is allowed
	 * (no data yet) and simply advances the clock by nothing.
	 *
	 * @return Number of samples written to @p buf (0..max).
	 */
	size_t (*read_audio)(void *ctx, int16_t *buf, size_t max);

	/**
	 * @brief Play @p n transmit samples. Called only between PTT key and unkey.
	 */
	void (*write_audio)(void *ctx, const int16_t *buf, size_t n);

	/**
	 * @brief Key (true) or unkey (false) the transmitter.
	 *
	 * Wired from the runtime's PTT callback; the loop itself does not call it.
	 * A backend that keys via the same path as audio may leave this to toggle
	 * a GPIO/CAT line.
	 */
	void (*set_ptt)(void *ctx, bool key);

	/**
	 * @brief Wall-clock milliseconds, for logging and the regulatory ID only.
	 *
	 * NEVER a protocol deadline (those are sample-counted in the core). NULL to
	 * let the loop derive a clock from the sample count.
	 */
	uint64_t (*wall_ms)(void *ctx);

	/**
	 * @brief Poll for one pending host command; false if none is ready.
	 *
	 * The seam W2.3 (the TCP host interface) plugs into. NULL means no host
	 * input -- e.g. a pure FEC beacon or a replay run.
	 */
	bool (*poll_host)(void *ctx, ardop_host_cmd *out);

	/** @brief True when the loop should stop. NULL means run forever. */
	bool (*should_stop)(void *ctx);
} ardop_platform_ops;

#endif /* ARDOP_SHELL_PLATFORM_H_ */
