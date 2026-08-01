#ifndef ARDOP_SHELL_TELEMETRY_H_
#define ARDOP_SHELL_TELEMETRY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Only the spectrum geometry is needed, so a display can link this format
 * without dragging in the protocol headers. */
#include "modem/busy.h"

/**
 * @file telemetry.h
 * @brief The DSP observation channel: what the modem is hearing, for a display.
 *
 * ::ardop_obs (see runtime.h) is the presentation-agnostic *event* stream --
 * discrete, small, and meaningful to any consumer. This is the other half: bulk
 * per-block DSP data that exists only to be drawn. Spectrum rows and
 * constellation snapshots are high rate and display-shaped, so they get their
 * own channel rather than distending the event bus.
 *
 * That separation is the reason runtime.h's original refusal to carry
 * visualisation still holds in spirit: the core stays free of presentation, the
 * runtime hands out numbers it already computed, and nothing here can change
 * what goes on the air.
 *
 * @par Self-sufficiency.
 * A display is a separate process, and the host command port is already spoken
 * for by a mail client. So this channel also carries a ::ARDOP_TLM_STATUS
 * mirror of the discrete state a panel needs (link state, S/N, busy, PTT), and
 * a consumer needs exactly one connection to paint a complete picture.
 *
 * @par Wire format.
 * Little-endian throughout. A stream opens with ::ardop_tlm_hello (which pins
 * the version and the spectrum geometry, so a display need not hardcode bin
 * numbers), then carries a sequence of records, each a 3-byte header
 * (kind, u16 payload length) followed by its payload.
 */

/** @brief Stream magic, "ATLM" in ASCII. */
#define ARDOP_TLM_MAGIC 0x4D4C5441u

/** @brief Wire format version. Bumped on any incompatible layout change. */
#define ARDOP_TLM_VERSION 1u

/** @brief Bytes in the stream hello. */
#define ARDOP_TLM_HELLO_LEN 16

/** @brief Bytes in a record header (kind + u16 length). */
#define ARDOP_TLM_HEADER_LEN 3

/**
 * @brief Most constellation points carried in one snapshot.
 *
 * The demodulator can hold 8 carriers x 520 symbols. A scatter plot saturates
 * long before that -- past a couple of thousand points the extra ones only
 * overdraw -- so a snapshot is subsampled to this cap. Keeps the largest record
 * near 8 kB.
 */
#define ARDOP_TLM_MAX_POINTS 2048

/** @brief Largest payload any record can carry. */
#define ARDOP_TLM_MAX_PAYLOAD (6 + ARDOP_TLM_MAX_POINTS * 4)

/** @brief Largest whole record, header included. */
#define ARDOP_TLM_MAX_RECORD (ARDOP_TLM_HEADER_LEN + ARDOP_TLM_MAX_PAYLOAD)

/** @brief What a telemetry record carries. */
typedef enum {
	ARDOP_TLM_SPECTRUM = 1,      /**< One waterfall row. */
	ARDOP_TLM_CONSTELLATION = 2, /**< One frame's demodulated symbols. */
	ARDOP_TLM_AUDIO = 3,         /**< Capture level, RMS and peak. */
	ARDOP_TLM_STATUS = 4,        /**< Discrete state mirror (see above). */
} ardop_tlm_kind;

/**
 * @brief One observation. Flat like ::ardop_obs: read only the fields the
 *        @p kind documents. Pointers are borrowed and valid only for the call.
 */
typedef struct {
	ardop_tlm_kind kind;

	/* SPECTRUM. */
	const float *mag;       /**< ::ARDOP_BUSY_MAG_BINS power magnitudes. */

	/*
	 * CONSTELLATION.
	 *
	 * The two point arrays are interpreted by @p modulation, because the
	 * decision a display must show differs by mode:
	 *
	 *   4PSK/8PSK/16QAM  phase_mrad = differential phase, milliradians
	 *                    point_mag  = symbol magnitude, demodulator units
	 *
	 *   4FSK             phase_mrad = winning tone, 0..3
	 *                    point_mag  = decision margin, per mille of the
	 *                                 winning tone -- gain-independent, so
	 *                                 a display can show confidence without
	 *                                 knowing the audio level
	 *
	 * 4FSK has no I/Q plane at all (it is detected by tone magnitude, not
	 * phase), so plotting it on one would be a fiction. Carrying the tone
	 * decision instead lets a display show the same thing the mode actually
	 * decides on.
	 */
	uint8_t frame_type;     /**< Frame the symbols came from. */
	uint8_t modulation;     /**< ::ardop_modulation, selecting the overlay. */
	uint16_t n_points;      /**< Points in @p phase_mrad / @p point_mag. */
	const int16_t *phase_mrad; /**< See above; mode-dependent. */
	const int16_t *point_mag;  /**< See above; mode-dependent. */
	/**
	 * @brief 16QAM inner/outer ring boundary, in @p point_mag units.
	 *
	 * The decoder's own adaptive magnitude threshold (car_mag_threshold),
	 * averaged over the frame's carriers. Zero for the modes that do not
	 * decide on amplitude, so a display draws no ring.
	 */
	int16_t mag_threshold;

	/* AUDIO. */
	float rms;              /**< Capture RMS, 0..1 of full scale. */
	float peak;             /**< Capture peak, 0..1 of full scale. */

	/* STATUS. */
	uint8_t state;          /**< ::ardop_link_state. */
	uint8_t mode;           /**< ::ardop_link_mode. */
	bool busy;              /**< Channel busy. */
	bool ptt;               /**< Transmitter keyed. */
	int16_t sn;             /**< Last frame's leader S/N, dB. */
	int16_t quality;        /**< Last frame's decode quality, 0..100. */
	int16_t bandwidth;      /**< Negotiated session width, Hz. */
	uint32_t buffer_len;    /**< Bytes queued to send. */
} ardop_telemetry;

/** @brief Telemetry sink: receives one ::ardop_telemetry. Must not block. */
typedef void (*ardop_telemetry_fn)(void *ctx, const ardop_telemetry *t);

/**
 * @brief Write the stream hello a consumer reads first.
 *
 * Carries the magic, the version, and the spectrum geometry
 * (::ARDOP_BUSY_MAG_BINS, ::ARDOP_BUSY_FIRST_BIN, ::ARDOP_BUSY_BIN_HZ) so a
 * display can label a frequency axis from the stream rather than from a
 * hardcoded copy of the modem's constants.
 *
 * @param out  Buffer of at least ::ARDOP_TLM_HELLO_LEN bytes.
 * @return Bytes written (::ARDOP_TLM_HELLO_LEN).
 */
size_t ardop_tlm_encode_hello(uint8_t *out);

/**
 * @brief Serialise one record.
 *
 * @param t    Record to encode.
 * @param out  Buffer of at least ::ARDOP_TLM_MAX_RECORD bytes.
 * @param cap  Bytes available in @p out.
 * @return Bytes written, or 0 if the record does not fit or its kind is
 *         unknown.
 */
size_t ardop_tlm_encode(const ardop_telemetry *t, uint8_t *out, size_t cap);

/**
 * @brief A decoded record plus the storage its arrays point into.
 *
 * Decoding into caller-owned arrays rather than into the receive buffer keeps
 * ardop_tlm_parse() free of alignment constraints on that buffer, and leaves
 * the decoded record valid after the buffer is compacted.
 */
typedef struct {
	ardop_telemetry rec;   /**< The record; its pointers address the below. */
	float mag[ARDOP_BUSY_MAG_BINS];
	int16_t phase_mrad[ARDOP_TLM_MAX_POINTS];
	int16_t point_mag[ARDOP_TLM_MAX_POINTS];
} ardop_tlm_decoded;

/**
 * @brief Read and validate the stream hello.
 *
 * @param buf        Bytes received so far.
 * @param avail      Bytes in @p buf.
 * @param bins       Receives the spectrum bin count.
 * @param first_bin  Receives the first transform bin.
 * @param bin_hz     Receives the bin width in Hz.
 * @return true if a well-formed hello of a supported version was read; false if
 *         @p avail is short, or the magic or version does not match.
 */
bool ardop_tlm_parse_hello(const uint8_t *buf, size_t avail, uint16_t *bins,
			   uint16_t *first_bin, float *bin_hz);

/**
 * @brief Parse one record from a byte stream.
 *
 * Mirrors ardop_host_data_parse(): returns false when the buffer does not yet
 * hold a complete record, so a reader can accumulate and retry.
 *
 * @param buf       Bytes received so far.
 * @param avail     Bytes in @p buf.
 * @param out       Receives the decoded record and its array storage.
 * @param consumed  Receives the byte count this record occupied. Set even for a
 *                  record whose kind is unknown, so a reader can skip it.
 * @return true if a complete, known record was decoded. False with @p consumed
 *         set to a non-zero value means "skip these bytes and continue".
 */
bool ardop_tlm_parse(const uint8_t *buf, size_t avail, ardop_tlm_decoded *out,
		     size_t *consumed);

#endif /* ARDOP_SHELL_TELEMETRY_H_ */
