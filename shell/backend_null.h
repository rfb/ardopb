#ifndef ARDOP_SHELL_BACKEND_NULL_H_
#define ARDOP_SHELL_BACKEND_NULL_H_

#include <stdbool.h>
#include <stdint.h>

#include "shell/platform.h"

/**
 * @file backend_null.h
 * @brief A device-free platform backend: silence in, samples counted and
 *        dropped on the way out.
 *
 * The equivalent of `NOSOUND`: capture returns silence (advancing the clock so
 * timers still run), transmit samples are counted and discarded, PTT is a
 * recorded flag. It lets the assembled program run and be smoke-tested with no
 * audio hardware -- a beacon or a scripted host session drives real modulation
 * through the whole core, and the counters/flag show it happened.
 */
typedef struct {
	uint64_t elapsed;      /**< samples "captured" (the clock). */
	uint64_t stop_after;   /**< stop once elapsed reaches this, when idle. */
	uint64_t tx_samples;   /**< transmit samples produced. */
	bool ptt;              /**< current PTT state. */
	int ptt_edges;         /**< key/unkey transitions. */
	bool verbose;          /**< log PTT edges to stderr. */
	bool realtime;         /**< pace capture to 12 kHz wall time (emulate a
				*   sound card), so --host sessions run live. */
} ardop_null_backend;

/**
 * @brief Initialise a null backend and fill @p ops to drive it.
 *
 * @param nb          Backend state (caller-owned; zeroed here).
 * @param ops         Platform ops table to populate (ctx points at @p nb).
 * @param stop_after  Capture-sample budget before should_stop() trips (0 = run
 *                    until interrupted; never stops on its own).
 */
void ardop_backend_null_init(ardop_null_backend *nb, ardop_platform_ops *ops,
			     uint64_t stop_after);

#endif /* ARDOP_SHELL_BACKEND_NULL_H_ */
