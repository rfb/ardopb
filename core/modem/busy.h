#ifndef ARDOP_MODEM_BUSY_H_
#define ARDOP_MODEM_BUSY_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @file busy.h
 * @brief Channel-busy detector: is someone else already using the frequency?
 *
 * Ported from `BusyDetect3` in the inherited `BusyDetect.c`. Given a magnitude
 * spectrum, it decides whether the channel is occupied, so the link layer can
 * hold off transmitting. It is consulted only while searching for a leader --
 * once a leader is found the receiver is committed and no longer asks.
 *
 * Sans-I/O: all state lives in a caller-owned ::ardop_busy_detector, the clock
 * is passed in, and configuration (bandwidth, sensitivity) is passed in rather
 * than read from globals.
 *
 * @par A note on time.
 * The detector's thresholds are defined by the protocol in milliseconds (a
 * 5-second busy hold, a 610 ms clear delay), and the inherited code compares
 * them against an `unsigned int` millisecond clock that wraps at 2^32. To stay
 * bit-identical -- including that wrap -- this port keeps time as a
 * `uint32_t` millisecond value rather than the sample count the rest of the
 * core uses. The caller derives it from the one sample clock (`samples / 12`
 * at 12 kHz); doing so here rather than reading a wall clock is what closes the
 * drift problem `analysis/03` describes, while the ms unit keeps the busy
 * arithmetic faithful.
 *
 * @par A preserved accident.
 * `BusyDetect3` intends a slow rolling average of the signal-to-noise ratio,
 * but reads a function-local that is re-zeroed every call (the persistent
 * `dblAvgStoNSlow*` globals are declared and never used). The average therefore
 * never accumulates: it is just a 0.2 gain on the current ratio, except on the
 * first call after a bandwidth change, where it is the full ratio -- a 5x
 * sensitivity step. This is preserved bit-for-bit; see
 * [[normative-accidents-catalog]].
 */

/** @brief Channel bandwidth, selecting the busy thresholds. */
typedef enum {
	ARDOP_BW_200 = 0,
	ARDOP_BW_500,
	ARDOP_BW_1000,
	ARDOP_BW_2000,
} ardop_bandwidth;

/**
 * @brief Busy-detector state. Caller-owned; zero-initialise before first use.
 *
 * A zeroed struct matches the inherited globals' initial state. ardop_busy_clear()
 * applies the `ClearBusy()` reset used at protocol transitions.
 */
typedef struct {
	int last_start;             /**< Bin range of the previous call (intLastStart). */
	int last_stop;              /**< (intLastStop). */
	int busy_on_count;          /**< Consecutive busy detections (intBusyOnCnt). */
	int busy_off_count;         /**< Consecutive idle detections (intBusyOffCnt). */
	bool last_busy;             /**< Last reported busy state (blnLastBusy). */
	uint32_t last_trip;         /**< ms of the last confirmed busy (dttLastTrip). */
	uint32_t last_busy_trip;    /**< ms busy last asserted (dttLastBusyTrip). */
	uint32_t prior_last_busy_trip; /**< prior of the above (dttPriorLastBusyTrip). */
	uint32_t last_busy_clear;   /**< ms busy last cleared (dttLastBusyClear). */
} ardop_busy_detector;

/**
 * @brief Apply the `ClearBusy()` reset: forget history and clear busy now.
 *
 * Used when the detector is (re)enabled or the protocol state changes. Forces
 * the rolling averages to reinitialise on the next call (by zeroing the bin
 * range) and clears any asserted busy immediately.
 *
 * @param b       Detector to reset.
 * @param now_ms  Current time, milliseconds.
 */
void ardop_busy_clear(ardop_busy_detector *b, uint32_t now_ms);

/**
 * @brief Decide whether the channel is busy from a magnitude spectrum.
 *
 * Ported from `BusyDetect3`. Sorts the bins in [@p start, @p stop] to separate
 * signal peaks from the noise baseline over a narrow (~94 Hz) and a wide (~2/3
 * bandwidth) window, forms a signal-to-noise ratio for each, and trips busy
 * when either exceeds a bandwidth- and sensitivity-dependent threshold. The
 * result is filtered: busy must persist ~3 calls (~250 ms) to be reported, and
 * once reported it is held for 5 s past the last trip before clearing.
 *
 * @param b         Detector state, updated in place.
 * @param mag       Magnitude spectrum; bins @p start..@p stop are read.
 * @param start     First bin of the search range.
 * @param stop      Last bin of the search range (@p stop - @p start + 1 <= 200).
 * @param bw        Channel bandwidth (selects thresholds).
 * @param busy_det  Sensitivity 0..10 (host `BusyDet`); 0 disables (never busy).
 * @param now_ms    Current time, milliseconds (see the file note on time).
 * @return true if the channel is currently reported busy.
 */
bool ardop_busy_detect(ardop_busy_detector *b, const float *mag, int start,
		       int stop, ardop_bandwidth bw, int busy_det,
		       uint32_t now_ms);

#endif /* ARDOP_MODEM_BUSY_H_ */
