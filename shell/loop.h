#ifndef ARDOP_SHELL_LOOP_H_
#define ARDOP_SHELL_LOOP_H_

#include <stdint.h>

#include "shell/platform.h"
#include "shell/runtime.h"

/**
 * @file loop.h
 * @brief The single-clocked driver: platform audio in/out around the runtime.
 *
 * This is the main-loop body of [analysis/06](../analysis/06-target-architecture.md)
 * §main-loop and [analysis/10](../analysis/10-modem-link-design.md) §7, made
 * concrete: capture samples (the only clock), advance time by the count, push
 * them through the runtime, service timers, and -- while a transmission is in
 * flight -- drain modulated samples to the output device instead of capturing.
 * It replaces the two divergent poll/flush spines (`ALSASound.c`, `Waveout.c`)
 * with one that knows nothing about any specific device: all hardware lives
 * behind ::ardop_platform_ops.
 *
 * It owns no state of its own beyond a clock and scratch buffers; the runtime
 * holds the protocol state and the platform holds the devices. There is no
 * blocking transmit and no wall-clock in the path -- TX ends when the modulator
 * drains (observed), not when a nominal duration elapses (predicted).
 */

/** Upper bound on samples moved per iteration, and the size of the scratch
 *  buffers. One ARDOP symbol block is 240; a larger chunk amortises call
 *  overhead while staying well inside one frame's leader. */
#define ARDOP_LOOP_BLOCK 1200

/**
 * @brief The driver: a runtime, a platform, a sample clock, and scratch buffers.
 *        Caller-owned; zero then ardop_loop_init().
 */
typedef struct {
	ardop_runtime *rt;
	const ardop_platform_ops *plat;
	uint64_t t;                       /**< elapsed samples: the one clock. */
	/**
	 * @brief Samples per iteration; 1..::ARDOP_LOOP_BLOCK, default the max.
	 *
	 * A backend may lower this after ::ardop_loop_init. At 12 kHz the
	 * default is 100 ms, and capture-to-link latency is one block plus the
	 * device buffer -- already 40% of the 250 ms ARQ turnaround budget
	 * before the link has seen the frame. On a platform whose device buffer
	 * is larger that can push a peer into retrying and gear-shifting down,
	 * which looks like a bad channel rather than a bad setting.
	 *
	 * The buffers stay at ::ARDOP_LOOP_BLOCK so the struct layout and the
	 * existing tests are unaffected (analysis/15 §8).
	 */
	size_t block;
	int16_t in[ARDOP_LOOP_BLOCK];
	int16_t out[ARDOP_LOOP_BLOCK];
} ardop_loop;

/**
 * @brief Bind a driver to a runtime and platform, with ::ardop_loop::block at
 *        the default ::ARDOP_LOOP_BLOCK.
 *
 * Does not touch the runtime's callbacks. PTT is routed by the observer
 * installed with ardop_runtime_observe() -- see the ARDOP_OBS_PTT case in
 * shell/main.c -- not by a field on the runtime.
 */
void ardop_loop_init(ardop_loop *lp, ardop_runtime *rt,
		     const ardop_platform_ops *plat);

/**
 * @brief Run one iteration: either drain one block of a transmission to the
 *        output, or capture one block and step the link (RX + host + timers).
 *
 * Transmitting takes priority (half-duplex): while the runtime has samples to
 * send, the loop writes them and does not capture. The clock advances by the
 * samples moved in either direction, keeping one continuous sample timeline
 * across TX and RX.
 */
void ardop_loop_step(ardop_loop *lp);

/**
 * @brief Run ardop_loop_step() until the platform's should_stop() returns true
 *        (or forever, if the backend supplies none).
 */
void ardop_loop_run(ardop_loop *lp);

#endif /* ARDOP_SHELL_LOOP_H_ */
