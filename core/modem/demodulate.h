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

	/* --- stage 2: downmix + 2 kHz frequency-selective filter ---
	 * The NCO downmixes to a 1500 Hz centre (inverting the sideband); the
	 * 23-resonator filter rejects the local-oscillator image. State that
	 * must carry across calls lives here. Ported from MixNCOFilter and
	 * FSMixFilter2000Hz; the resonator coefficients use the reduced pi. */
	float nco_phase;          /**< Downmix oscillator phase (radians). */
	float filt_coef[27];      /**< Resonator coefficients; set at init. */
	float filt_zin_1;         /**< Comb input delayed one sample. */
	float filt_zin_2;         /**< Comb input delayed two samples. */
	float filt_zcomb;         /**< Comb output. */
	float filt_zout_0[27];    /**< Resonator outputs. */
	float filt_zout_1[27];    /**< Resonator outputs delayed one sample. */
	float filt_zout_2[27];    /**< Resonator outputs delayed two samples. */
	int16_t prior_mixed[120]; /**< Last 120 mixed samples for the next call. */
	int16_t filtered_mixed[5000]; /**< Baseband output; later stages read it. */
	int filtered_mixed_len;   /**< Valid samples in filtered_mixed. */
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

/**
 * @brief Downmix a sample block to baseband and filter it (stage 2).
 *
 * Ported from `MixNCOFilter` + `FSMixFilter2000Hz`. Mixes @p samples with the
 * numerically-controlled oscillator (tuned to 3000 + @p offset_hz, which shifts
 * the signal to a 1500 Hz centre and inverts the sideband), runs the result
 * through the 23-section frequency-selective filter, and appends the filtered
 * baseband to @p d->filtered_mixed. The NCO phase and the filter's resonator
 * state carry across calls, so successive blocks filter continuously.
 *
 * @param[in,out] d         Receiver; updates the NCO phase, filter state and
 *                          the filtered_mixed buffer.
 * @param[in]     samples   @p length input samples.
 * @param[in]     length    Samples to process (0 is a no-op; max 2400).
 * @param[in]     offset_hz Tuning offset for the downmix oscillator, Hz.
 */
void ardop_demod_mix_filter(ardop_demod *d, const int16_t *samples, int length,
			    float offset_hz);

#endif /* ARDOP_MODEM_DEMODULATE_H_ */
