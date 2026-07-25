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

	/* --- stage 3: symbol + frame sync ---
	 * Both walk the baseband in filtered_mixed from mfs_read_ptr, advancing
	 * it as symbols are consumed. Ported from Acquire2ToneLeaderSymbolFraming
	 * and AcquireFrameSyncRSB. */
	int mfs_read_ptr;   /**< Read cursor into filtered_mixed. */
	int leader_rcvd_ms; /**< Measured leader length, ms (ARQ timing input). */
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

/**
 * @brief Establish symbol framing on the two-tone leader (stage 3a).
 *
 * Ported from `Acquire2ToneLeaderSymbolFraming`. Correlates the baseband
 * against the leader template to find the symbol boundary, then refines it to
 * minimum 1500 Hz phase error, positioning @p d->mfs_read_ptr at a symbol
 * start. On success advances @p d to ARDOP_RX_ACQUIRE_FRAME_SYNC.
 *
 * @param[in,out] d Receiver; reads filtered_mixed from mfs_read_ptr, updates
 *                  mfs_read_ptr and state.
 * @return true once framing is established (needs >= 860 samples buffered).
 */
bool ardop_demod_symbol_framing(ardop_demod *d);

/**
 * @brief Find the BPSK frame-sync symbol (stage 3b).
 *
 * Ported from `AcquireFrameSyncRSB`. Scans symbols for the sync signature (a
 * large phase step into the reference symbol followed by none), and on a match
 * positions @p d->mfs_read_ptr at the first frame-type symbol and records the
 * received leader length. On no match, backs the cursor up two symbols for the
 * next call.
 *
 * @param[in,out] d Receiver; reads filtered_mixed from mfs_read_ptr, updates
 *                  mfs_read_ptr and leader_rcvd_ms.
 * @return true when the sync symbol is found.
 */
bool ardop_demod_frame_sync(ardop_demod *d);

/** Tone magnitudes for a frame type: 10 symbols x 4 tones = 40 entries. */
#define ARDOP_FRAMETYPE_TONE_MAGS 40

/**
 * @brief Measure the 4FSK tone magnitudes of the frame-type field (stage 4).
 *
 * Ported from the non-SDFT path of `DemodFrameType4FSK`. Runs a Goertzel at each
 * of the four 50-baud tones (1425/1475/1525/1575 Hz) over each of the 10
 * frame-type symbols starting at @p ptr, writing 40 magnitudes to @p mags in
 * symbol-major, tone-minor order. These feed ardop_frametype_decode_distance().
 *
 * @param[in]  d    Receiver; reads filtered_mixed.
 * @param[in]  ptr  Sample offset of the first frame-type symbol.
 * @param[out] mags ARDOP_FRAMETYPE_TONE_MAGS magnitudes.
 * @return true if enough baseband is buffered (>= 2400 samples from @p ptr).
 */
bool ardop_demod_frametype_tonemags(const ardop_demod *d, int ptr,
				    int32_t *mags);

/**
 * @brief Distance of one frame-type byte from a candidate type (stage 4).
 *
 * Ported from `ComputeDecodeDistance`. Scores five symbols (four dibits of the
 * candidate @p frame_type XORed with the decode key @p id, plus the parity
 * symbol) against the measured tones, returning a normalised 0..1 distance --
 * lower is a better match. The caller minimises this over the valid frame types.
 *
 * @param mags       Tone magnitudes from ardop_demod_frametype_tonemags().
 * @param tone_ptr   0 for the first frame-type byte, 20 for the second.
 * @param frame_type Candidate frame type byte.
 * @param id         Decode key XORed into the byte (session id, or 0 / 0xFF).
 * @return Normalised distance in [0, 1].
 */
float ardop_frametype_decode_distance(const int32_t *mags, int tone_ptr,
				      uint8_t frame_type, uint8_t id);

/**
 * @brief Protocol context for the frame-type acceptance decision (stage 4b).
 *
 * The frame-type decision is protocol-coupled: the second byte is XORed with
 * the session id (a decode key you cannot omit), and how confident a match must
 * be depends on the connection state. Rather than read that state from globals,
 * the demodulator is handed it explicitly here, keeping the core free of
 * protocol state. The caller (the link layer, eventually) fills this in.
 */
typedef struct {
	const uint8_t *valid_types; /**< Allowed types (bytValidFrameTypes{ISS,ALL}). */
	int valid_len;              /**< Number of entries in valid_types. */
	uint8_t session_id;         /**< Decode key for the 2nd byte (bytSessionID). */
	bool pending;               /**< ARQ connection pending (blnPending). */
	bool arq_connected;         /**< ARQ session up (blnARQConnected). */
	uint8_t last_arq_session_id;/**< Prior session id (bytLastARQSessionID). */
} ardop_frametype_decode_ctx;

/**
 * @brief Decide the frame type by minimal distance over the valid types (stage 4b).
 *
 * Ported from `MinimalDistanceFrameType`. Scores every valid type against the
 * measured tones for byte 1 (key 0), byte 2 (key @p ctx->session_id) and a
 * third variant (key 0xFF when pending, else @p ctx->last_arq_session_id), then
 * applies the connection-state-dependent acceptance rules.
 *
 * @param[in]  mags          Tone magnitudes from ardop_demod_frametype_tonemags().
 * @param[in]  ctx           Protocol context (valid types, session id, flags).
 * @param[out] set_last_good True if this is a confident decode that should
 *                           refresh the last-good-decode tuning timestamp
 *                           (mirrors the original setting dttLastGoodFrameTypeDecode).
 * @return frame type 0..255, or -1 on a poor-quality decode.
 */
int ardop_frametype_minimal_distance(const int32_t *mags,
				     const ardop_frametype_decode_ctx *ctx,
				     bool *set_last_good);

#endif /* ARDOP_MODEM_DEMODULATE_H_ */
