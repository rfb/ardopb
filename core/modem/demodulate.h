#ifndef ARDOP_MODEM_DEMODULATE_H_
#define ARDOP_MODEM_DEMODULATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file demodulate.h
 * @brief The receive chain: samples in, frames out. Sans-I/O.
 *
 * The demodulator is the inverse of the modulator, and the harder half of the
 * modem. It is one state machine that walks a sample stream through leader
 * detection, symbol and frame sync, frame-type classification, per-modulation
 * demodulation and Reed-Solomon decode, emitting an event when a frame is
 * recovered. It reads no clock but the sample count it is given, consults no
 * protocol state, and never transmits. See
 * [analysis/11](../../analysis/11-demod-design.md) for the field-level design
 * and the stage-by-stage port order.
 *
 * This is being ported one stage at a time; ::ardop_demod grows as stages land.
 * The DSP is transcribed from the inherited `SoundInput.c` and pinned to it, and
 * uses the same reduced-precision pi as the modulator (the receiver's bins must
 * line up with the transmitter's). See [[ardop-mpi-normative-accident]].
 */

/** @brief Acquisition state. Mirrors the inherited `enum _ReceiveState`. */
typedef enum {
	ARDOP_RX_SEARCHING_FOR_LEADER = 0,
	ARDOP_RX_ACQUIRE_SYMBOL_SYNC,
	ARDOP_RX_ACQUIRE_FRAME_SYNC,
	ARDOP_RX_ACQUIRE_FRAME_TYPE,
	ARDOP_RX_ACQUIRE_FRAME,
} ardop_rx_state;

/**
 * @brief Receiver state. Caller-owned; the members are an implementation
 *        detail and grow as demodulator stages are ported.
 */
typedef struct {
	/* --- configuration: set at init, then read-only --- */
	int tuning_range;  /**< Max tuning offset searched, Hz (default 100). */
	int squelch;       /**< Leader-detect S/N threshold, dB (default 5). */

	/* --- acquisition FSM state --- */
	ardop_rx_state state;
	float offset_hz;          /**< Current tuning offset estimate, Hz. */
	float nco_freq;           /**< Downmix oscillator frequency, Hz. */
	float nco_phase_inc;      /**< Downmix oscillator phase step per sample. */
	float prior_fine_offset;  /**< Offset from the previous leader probe. */
	float sn_db;              /**< S/N of the last leader detection, dB. */

	/* --- timestamps, in elapsed samples (the one clock) --- */
	uint64_t last_leader_detect;
	uint64_t start_rmt_leader_measure;
	uint64_t last_good_frametype_decode;
} ardop_demod;

/**
 * @brief Initialise a receiver with the two DSP-relevant host settings.
 *
 * @param d            Context to initialise.
 * @param tuning_range Max tuning offset to search, Hz (inherited default 100).
 * @param squelch      Leader-detect threshold, dB (inherited default 5).
 */
void ardop_demod_init(ardop_demod *d, int tuning_range, int squelch);

/**
 * @brief Search a sample window for the 50-baud two-tone leader.
 *
 * Stage 1 of the receive pipeline (ported from `SearchFor2ToneLeader3`). Detects
 * the leader, refines the tuning offset, and on success sets up the downmix
 * oscillator and advances @p d to ARDOP_RX_ACQUIRE_SYMBOL_SYNC. Uses
 * @p d->offset_hz as the prior estimate and updates it.
 *
 * @param[in,out] d           Receiver; reads config and prior offset, updates
 *                            offset/NCO/state/timestamps.
 * @param[in]     samples     At least @p length samples.
 * @param[in]     length      Samples available (needs >= 1200).
 * @param[in]     now_samples Current time as an elapsed sample count.
 * @param[out]    sn_out      S/N of the detection, dB (written on success).
 * @return true if a leader was detected.
 */
bool ardop_demod_leader_search(ardop_demod *d, const int16_t *samples,
			       int length, uint64_t now_samples, int *sn_out);

#endif /* ARDOP_MODEM_DEMODULATE_H_ */
