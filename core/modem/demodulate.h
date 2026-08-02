#ifndef ARDOP_MODEM_DEMODULATE_H_
#define ARDOP_MODEM_DEMODULATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/frame.h"
#include "codec/rs.h"

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

/** Maximum simultaneous carriers (8-carrier PSK is the widest frame). */
#define ARDOP_DEMOD_MAX_CARRIERS 8

/** Maximum PSK symbols demodulated per carrier per frame. */
#define ARDOP_DEMOD_MAX_PSK_SYMBOLS 520

/** Maximum gross bytes in one carrier's block ([len][data][CRC][RS]). */
#define ARDOP_DEMOD_MAX_CARRIER_BYTES 256

/** Maximum decoded payload bytes handed out with a FRAME_DECODED event. */
#define ARDOP_DEMOD_MAX_PAYLOAD 1024

/**
 * @brief What the receiver reports. The whole outward vocabulary of the demod;
 *        the link layer routes these, the demod never acts on protocol state.
 */
typedef enum {
	ARDOP_EV_LEADER_DETECTED,  /**< Acquisition started: offset_hz, sn. */
	ARDOP_EV_FRAME_DECODED,    /**< Good frame: frame_type, data/data_len. */
	ARDOP_EV_FRAME_BAD,        /**< Frame acquired but failed CRC/RS. */
} ardop_event_kind;

/** @brief One receiver event, written into the caller's array by push. */
typedef struct {
	ardop_event_kind kind;
	uint8_t frame_type;   /**< FRAME_DECODED / FRAME_BAD. */
	float offset_hz;      /**< LEADER_DETECTED tuning offset. */
	int sn;               /**< LEADER_DETECTED signal/noise, dB. */
	int quality;          /**< Decode quality 0..100 (FRAME_DECODED); feeds the
	                       *   ACK/NAK and PingAck a receiver sends back. */
	int leader_ms;        /**< Received leader length, ms; feeds ConAck timing
	                       *   (FRAME_DECODED). */
	const uint8_t *data;  /**< FRAME_DECODED payload; points into the demod,
	                       *   valid until the next push. */
	int data_len;         /**< FRAME_DECODED payload length. */
} ardop_event;

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
	bool rxo;                   /**< Receive-only mode: session-independent
	                             *   frame-type decode (as `--decodewav`). */
} ardop_frametype_decode_ctx;

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

	/* --- stage 5: per-frame PSK demod state (group d) ---
	 * Set up by ardop_demod_psk_init at the start of a PSK frame, then filled
	 * by ardop_demod_psk_char one carrier at a time. Ported from InitDemodPSK
	 * and Demod1CarPSKChar. */
	int psk_mode;          /**< PSK order: 4 or 8 (also symbols per char). */
	int psk_samp_per_sym;  /**< Samples per PSK symbol (120). */
	float freq_bin[ARDOP_DEMOD_MAX_CARRIERS];  /**< Goertzel bin per carrier. */
	short n_for_goertzel[ARDOP_DEMOD_MAX_CARRIERS]; /**< Goertzel length. */
	short cp[ARDOP_DEMOD_MAX_CARRIERS];        /**< Cyclic-prefix offset. */
	short psk_phase_1[ARDOP_DEMOD_MAX_CARRIERS]; /**< Prior phase, milliradians. */
	short car_mag_threshold[ARDOP_DEMOD_MAX_CARRIERS]; /**< Ref magnitude*0.75. */
	short phases[ARDOP_DEMOD_MAX_CARRIERS][ARDOP_DEMOD_MAX_PSK_SYMBOLS];
	                       /**< Differential phases per carrier, milliradians. */
	short mags[ARDOP_DEMOD_MAX_CARRIERS][ARDOP_DEMOD_MAX_PSK_SYMBOLS];
	                       /**< Symbol magnitudes per carrier. */
	int phases_len;        /**< Symbols demodulated so far (index into above). */

	/* --- stage 6: streaming push FSM working state ---
	 * The push driver appends to raw[], runs the acquisition FSM, streams
	 * the frame's carriers, and emits events. Ported from ProcessNewSamples.
	 * Protocol decisions stay outside: the caller sets ft_ctx (the frame-type
	 * decode keys) and rs (the RS context), and routes the events. */
	const ardop_rs *rs;          /**< RS context for carrier decode (caller-set). */
	ardop_frametype_decode_ctx ft_ctx; /**< Frame-type decode keys (caller-set). */

	int16_t raw[2400];           /**< Pre-mix samples held for the leader search. */
	int raw_len;                 /**< Valid samples in raw. */

	uint8_t frame_type;          /**< Frame type being acquired. */
	int frame_num_car;           /**< Carriers in the current frame. */
	int frame_baud;              /**< Baud of the current frame's data. */
	int frame_data_len;          /**< Per-carrier gross data length. */
	int frame_rs_len;            /**< Per-carrier RS parity length. */
	ardop_modulation frame_mod;  /**< Modulation of the current frame. */
	int frame_samp_per_sym;      /**< Samples per data symbol (12000/baud). */
	bool frame_600;              /**< 4FSK.2000.600 (0x7A/0x7B): one long 4FSK
	                              *   carrier of three sequential [len][data]
	                              *   [crc][rs] sub-blocks, RS-decoded as three
	                              *   "carriers". See ProcessNewSamples/DecodeFrame. */
	int symbols_left;            /**< Bytes still to demodulate (all carriers). */
	int char_index;              /**< Next byte index within a carrier block. */
	bool psk_init_done;          /**< PSK/QAM training symbol consumed this frame. */
	bool carrier_ok[ARDOP_DEMOD_MAX_CARRIERS]; /**< Per-carrier decode success. */
	uint8_t frame_data[ARDOP_DEMOD_MAX_CARRIERS][ARDOP_DEMOD_MAX_CARRIER_BYTES];
	                             /**< Raw demodulated bytes, per carrier. */
	uint8_t payload[ARDOP_DEMOD_MAX_PAYLOAD]; /**< Decoded payload for the event. */

	/* 4FSK constellation tone magnitudes for the whole frame, four per
	 * symbol, accumulated while streaming so the decode quality can be
	 * scored at delivery. Sized for the widest 4FSK frame (600-baud, 759
	 * bytes x 16). PSK/QAM quality uses phases[]/mags[] directly. */
	int32_t frame_tone_mags[16 * 759];
	int tone_mags_len;           /**< Valid entries in frame_tone_mags. */

	/* --- Memory ARQ ---------------------------------------------------
	 *
	 * A frame that no single reception can decode may decode from several
	 * averaged together. ARDOP retransmits a failed frame unchanged, so
	 * repeated copies differ only in their noise; averaging them raises the
	 * effective S/N, and carriers that decoded in *different* copies can be
	 * combined into one good frame even when no single copy was complete.
	 *
	 * State lives here, in the caller's context, because core/ has no
	 * mutable globals (`make check-pure`). It makes the decoder stateful
	 * across frames, which any harness driving it has to account for.
	 *
	 * The accumulate-versus-reset decision is taken locally, with no hint
	 * from the link: ARDOP's even/odd frame-type alternation *is* the
	 * retransmission signal, so "same type as the frame I just failed to
	 * decode" means "retransmission" (analysis/10 Decision 2).
	 *
	 * @note The first copy is stored, never averaged, so a single reception
	 *       decodes by exactly the path it did before Memory ARQ existed.
	 *       `make golden-core` pins that.
	 */
	short marq_phase[ARDOP_DEMOD_MAX_CARRIERS][ARDOP_DEMOD_MAX_PSK_SYMBOLS];
	                             /**< Running mean phase, milliradians. */
	short marq_mag[ARDOP_DEMOD_MAX_CARRIERS][ARDOP_DEMOD_MAX_PSK_SYMBOLS];
	                             /**< Running mean magnitude (QAM only). */
	short marq_tone[16 * 759];   /**< Running mean 4FSK tone magnitudes,
	                              *   normalised per symbol to 0..1000. */
	int marq_tone_len;           /**< Valid entries in marq_tone. */
	uint16_t marq_count[ARDOP_DEMOD_MAX_CARRIERS];
	                             /**< Copies accumulated, per carrier. */
	int marq_type;               /**< Frame type being accumulated, -1 none. */
	int marq_len;                /**< phases_len the accumulator was built at. */
	uint64_t marq_since;         /**< Sample count when accumulation began. */
} ardop_demod;

/**
 * @brief How long an unused Memory-ARQ accumulator survives, in samples.
 *
 * Sample-counted, not wall-clock: the sample count is the only clock the core
 * has, and tying this to it means a slow sound card slows the timeout by the
 * same fraction it slows everything else (analysis/06 Rule 2). The inherited
 * implementation used `1000 * ARQTimeout` against a millisecond clock, and a
 * separate 36-second cap in FEC; 60 s at 12 kHz is comfortably longer than the
 * longest frame plus a turnaround, and short enough that an unrelated frame of
 * the same type minutes later cannot be averaged into a stale accumulator.
 */
#define ARDOP_MEMARQ_MAX_AGE_SAMPLES (60ull * 12000ull)

/**
 * @brief Discard any accumulated Memory-ARQ state.
 *
 * Called on a mode change, a new session, or when the link decides the
 * accumulator is stale. The demodulator also does this itself when the frame
 * type changes or the accumulator ages out.
 */
void ardop_demod_memarq_reset(ardop_demod *d);

/**
 * @brief Copies currently accumulated for @p carrier (0 if none). For tests
 *        and telemetry; the decode path does not need it.
 */
unsigned ardop_demod_memarq_count(const ardop_demod *d, int carrier);

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

/**
 * @brief Session-independent distance of a frame type (RXO mode).
 *
 * Ported from `RxoComputeDecodeDistance`. Scores only the tones that do not
 * depend on the session id: the first byte's four dibits and parity, plus the
 * second byte's parity (a second copy of the same frame-type parity). The
 * second byte's dibits are skipped. Normalised over the six symbols used.
 */
float ardop_frametype_rxo_distance(const int32_t *mags, uint8_t frame_type);

/**
 * @brief Decide the frame type without a session id (RXO mode).
 *
 * Ported from `RxoMinimalDistanceFrameType`. Minimises
 * ardop_frametype_rxo_distance() over the valid types and accepts if the
 * distance is below 0.3. Used by `--decodewav` / receive-only decoding, where
 * no connection state is available. Session-id grouping (RxoDecodeSessionID) is
 * a separate concern and does not affect the returned type.
 *
 * @return frame type 0..255, or -1 on a poor decode.
 */
int ardop_frametype_rxo_minimal_distance(const int32_t *mags,
					 const uint8_t *valid_types,
					 int valid_len);

/** One 4FSK character is four 2-bit symbols; each yields four tone mags. */
#define ARDOP_4FSK_CHAR_TONE_MAGS 16

/**
 * @brief Demodulate one 4FSK character: four symbols into one byte (stage 5).
 *
 * Ported from `Demod1Car4FSKChar`. For each of the four 4FSK symbols it runs a
 * Goertzel at the four tones (highest first, since the sideband is reversed),
 * takes the strongest as the 2-bit symbol, and packs the four symbols MSB-first
 * into the returned byte. The 16 tone magnitudes (four per symbol) are written
 * to @p tone_mags for the constellation / Memory-ARQ tracking.
 *
 * @param[in]  d             Receiver; reads filtered_mixed.
 * @param[in]  start         Sample offset of the first symbol.
 * @param[in]  center_freq   Carrier centre frequency, Hz (nominally 1500).
 * @param[in]  baud          Symbol rate (50/100/600).
 * @param[in]  samp_per_sym  Samples per symbol (12000/baud).
 * @param[out] tone_mags     ARDOP_4FSK_CHAR_TONE_MAGS magnitudes.
 * @return The demodulated byte.
 */
uint8_t ardop_demod_4fsk_char(const ardop_demod *d, int start, int center_freq,
			      int baud, int samp_per_sym, int32_t *tone_mags);

/**
 * @brief Reed-Solomon correct and CRC-check one carrier's raw bytes (stage 5).
 *
 * Ported from `CorrectRawDataWithRS`. @p raw is one carrier's received block,
 * laid out as `[length][data (data_len)][CRC-16 (2)][RS parity (rs_len)]`
 * (@p data_len + @p rs_len + 3 bytes total). It is RS-corrected in place; if the
 * correction succeeds, the reported length is sane and the CRC (frame type mixed
 * in) checks out, the @p data_len net bytes are copied to @p corrected and
 * @p decoded_ok is set. Otherwise the uncorrected data bytes are copied and
 * @p decoded_ok stays false.
 *
 * The per-carrier "already decoded" cache and the Memory-ARQ update are the
 * caller's to keep: pass @p carrier_already_ok to short-circuit, and act on
 * @p decoded_ok. Stats and logging are dropped.
 *
 * @param[in]  rs                 RS context whose rslen set includes @p rs_len.
 * @param[in,out] raw             Received block; corrected in place.
 * @param[out] corrected          Net data bytes (caller storage, >= data_len).
 * @param[in]  data_len           Gross data length (per the frame spec).
 * @param[in]  rs_len             RS parity length (per the frame spec).
 * @param[in]  frame_type         Frame type, mixed into the CRC.
 * @param[in]  carrier_already_ok Skip decoding; just re-emit the net bytes.
 * @param[out] decoded_ok         True iff this call produced a good decode.
 * @return The number of bytes written to @p corrected: the net length on a good
 *         decode (or when already ok), else @p data_len.
 */
int ardop_decode_carrier_rs(const ardop_rs *rs, uint8_t *raw,
			    uint8_t *corrected, int data_len, int rs_len,
			    uint8_t frame_type, bool carrier_already_ok,
			    bool *decoded_ok);

/**
 * @brief Set up per-frame PSK demodulation state (stage 5).
 *
 * Ported from `InitDemodPSK` (the demod-relevant part; the disabled
 * symbol-tracking init is dropped). Assigns each carrier its frequency bin and
 * Goertzel length, and measures an initial reference phase (and magnitude
 * threshold) from the training symbol at the start of @p d->filtered_mixed.
 * Carriers run highest frequency first (the sideband is reversed).
 *
 * @param d        Receiver; reads filtered_mixed, initialises the PSK state.
 * @param num_car  Number of carriers (1, 2, 4 or 8).
 * @param psk_mode PSK order: 4 (4PSK) or 8 (8PSK).
 */
void ardop_demod_psk_init(ardop_demod *d, int num_car, int psk_mode);

/**
 * @brief Demodulate one PSK character for one carrier (stage 5).
 *
 * Ported from `Demod1CarPSKChar`. Demodulates @p d->psk_mode symbols starting at
 * @p start: for each it takes a (Hanning-windowed) Goertzel, stores the
 * magnitude and the differential phase from the prior symbol (in milliradians),
 * and advances @p d->phases_len. Returns the number of samples consumed.
 *
 * @param d                  Receiver; reads filtered_mixed, updates phases/mags.
 * @param start              Sample offset of the first symbol.
 * @param carrier            Carrier index (0-based).
 * @param carrier_already_ok If true, skip the DSP (this carrier already decoded)
 *                           but still advance phases_len, as the original does.
 * @return Samples consumed (psk_mode * samples-per-symbol).
 */
int ardop_demod_psk_char(ardop_demod *d, int start, int carrier,
			 bool carrier_already_ok);

/**
 * @brief Decode one carrier's PSK differential phases into bytes (stage 5).
 *
 * Ported from `Decode1CarPSK`. Slices @p d->phases[carrier] into symbols by
 * milliradian decision boundaries: 4PSK maps four phases to one byte (2 bits
 * each), 8PSK maps eight phases to three bytes (3 bits each). Consumes all
 * @p d->phases_len phases. The result feeds ardop_decode_carrier_rs().
 *
 * @param[in]  d                  Receiver; reads phases[carrier] and psk_mode.
 * @param[in]  carrier            Carrier index.
 * @param[out] decoded            Decoded bytes (caller storage).
 * @param[in]  carrier_already_ok If true, decode nothing (already decoded).
 * @return The number of bytes written to @p decoded.
 */
int ardop_decode_psk_char(const ardop_demod *d, int carrier, uint8_t *decoded,
			  bool carrier_already_ok);

/**
 * @brief Demodulate one 16QAM character for one carrier (stage 5).
 *
 * Ported from `Demod1CarQAMChar`. Like ardop_demod_psk_char but always two
 * symbols and always Hanning-windowed; per-frame setup is ardop_demod_psk_init
 * with @p psk_mode 8 (as `InitDemodQAM` does). Stores differential phase and
 * magnitude per symbol; the magnitude carries the QAM amplitude bit.
 *
 * @param d                  Receiver; reads filtered_mixed, updates phases/mags.
 * @param start              Sample offset of the first symbol.
 * @param carrier            Carrier index.
 * @param carrier_already_ok If true, skip the DSP but still advance phases_len.
 * @return Samples consumed (2 * samples-per-symbol).
 */
int ardop_demod_qam_char(ardop_demod *d, int start, int carrier,
			 bool carrier_already_ok);

/**
 * @brief Decode one carrier's 16QAM phases + magnitudes into bytes (stage 5).
 *
 * Ported from `Decode1CarQAM`. Each symbol is a nibble: three bits from the
 * differential phase (8 sectors, same boundaries as 8PSK) plus one amplitude
 * bit set when the magnitude is below a rolling threshold (inner constellation
 * ring). Two symbols pack into one byte. The threshold adapts per symbol and is
 * kept in @p d->car_mag_threshold[carrier].
 *
 * @param[in,out] d                  Receiver; reads phases/mags, updates the
 *                                   per-carrier magnitude threshold.
 * @param[in]     carrier            Carrier index.
 * @param[out]    decoded            Decoded bytes (caller storage).
 * @param[in]     carrier_already_ok If true, decode nothing.
 * @return The number of bytes written to @p decoded.
 */
int ardop_decode_qam_char(ardop_demod *d, int carrier, uint8_t *decoded,
			  bool carrier_already_ok);

/**
 * @brief Drive the whole receiver on a block of samples (stage 6).
 *
 * Ported from `ProcessNewSamples`, stripped of its protocol/transmit/GUI
 * coupling. Appends @p samples to the internal pre-mix buffer and runs the
 * acquisition state machine as far as the available data allows: leader search,
 * downmix/filter, symbol and frame sync, frame-type acquisition, then streaming
 * per-carrier demodulation and RS decode. Terminal points are written to
 * @p events as ::ardop_event values rather than acted on.
 *
 * The caller owns the two pieces of protocol context the DSP needs: set
 * @p d->rs (RS context) and @p d->ft_ctx (frame-type decode keys) before
 * pushing. Payload pointers in FRAME_DECODED events point into @p d and stay
 * valid until the next push.
 *
 * @param[in]  d           Receiver.
 * @param[in]  samples     Newly captured samples.
 * @param[in]  n           Number of samples.
 * @param[in]  now_samples Current time as an elapsed sample count.
 * @param[out] events      Caller array for emitted events.
 * @param[in]  max_events  Capacity of @p events.
 * @return The number of events written.
 */
size_t ardop_demod_push(ardop_demod *d, const int16_t *samples, size_t n,
			uint64_t now_samples, ardop_event *events,
			size_t max_events);

#endif /* ARDOP_MODEM_DEMODULATE_H_ */
