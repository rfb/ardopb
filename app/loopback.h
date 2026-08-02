#ifndef ARDOP_APP_LOOPBACK_H_
#define ARDOP_APP_LOOPBACK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/spine.h"

/**
 * @file loopback.h
 * @brief Two spines wired to each other over an in-memory channel.
 *
 * The null backend cannot demonstrate a session. It returns silence, so there is
 * nobody on the other end and a connect attempt can only time out -- which makes
 * [analysis/14](../analysis/14-station-application.md)'s exit criterion ("runs
 * headless on the null backend ... and completes a connect, data, disconnect ARQ
 * session") unsatisfiable as literally written. Two stations sharing a channel is
 * the only way to satisfy it without a radio, and it is what
 * `test/core/test_loop.c` already builds by hand to test the loop.
 *
 * This is that, promoted so the unit test, the script driver and the sanitiser
 * stress test share one copy.
 *
 * What it models: every sample either station transmits, delivered in order to
 * the other, and a transmit clock that advances uniformly whether or not anyone
 * is talking. What it does not model: propagation delay, noise, fading,
 * collisions, or any reason a frame would fail. It is a *wiring* harness -- for
 * channel impairment there is `analysis/18`'s measurement rig.
 *
 * Both sides are stepped from one thread; the channel has no synchronisation of
 * its own.
 */

typedef struct app_loopback app_loopback;

/**
 * @brief Build two spines, each hearing the other.
 *
 * @param cfg Applied to both sides, or NULL for defaults.
 * @return The pair, or NULL on failure.
 */
app_loopback *app_loopback_open(const app_config *cfg);

/** @brief Tear both sides down. Safe on NULL. */
void app_loopback_close(app_loopback *lb);

/** @brief Side @p which, 0 or 1. NULL if out of range. */
app_spine *app_loopback_side(app_loopback *lb, int which);

/**
 * @brief Step both sides once.
 *
 * Neither side blocks -- there is no device to wait for -- so this runs as fast
 * as the host allows and a caller wanting wall-clock pacing has to add it. Time
 * inside the modem still advances at 12000 samples per second of simulated
 * air, which is the clock any timeout in a test should be measured against.
 */
void app_loopback_step(app_loopback *lb);

/** @brief Elapsed simulated samples, the same on both sides. */
uint64_t app_loopback_elapsed(const app_loopback *lb);

/**
 * @brief Samples a side could not deliver because the channel backed up.
 *
 * Always 0 in a correct run: the channel is drained every step and holds seconds
 * of audio. A non-zero value means the caller stepped one side far more than the
 * other, and any decode failure after that point is the harness's fault rather
 * than the modem's.
 */
uint64_t app_loopback_overruns(const app_loopback *lb);

/**
 * @brief Samples destroyed because both stations transmitted at once.
 *
 * A collision is a real event on a real channel and ARQ is what recovers from
 * it. Non-zero here is not a fault; it is the harness declining to pretend that
 * two simultaneous transmissions both arrive intact.
 */
uint64_t app_loopback_collisions(const app_loopback *lb);

#endif /* ARDOP_APP_LOOPBACK_H_ */
