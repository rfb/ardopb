#ifndef ARDOP_SHELL_RUNTIME_H_
#define ARDOP_SHELL_RUNTIME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/rs.h"
#include "link/link.h"
#include "modem/busy.h"
#include "modem/demodulate.h"
#include "modem/modulate.h"
#include "shell/telemetry.h"

/**
 * @file runtime.h
 * @brief The main-loop body: the core layers wired together, driven by samples.
 *
 * This is [analysis/10](../analysis/10-modem-link-design.md) §7 without the
 * device I/O -- the orchestration a platform backend wraps. It owns one
 * station's demodulator, link machine and modulator, and turns the link's
 * actions into effects: a SEND_FRAME becomes a modulated transmission the caller
 * pulls samples from; NOTIFY_HOST / DELIVER_DATA / SET_PTT become callbacks.
 *
 * It reads no clock (the caller passes the elapsed sample count), opens no
 * device, and never blocks -- the caller feeds it captured samples, drains
 * transmit samples, and delivers host commands. All state is in the caller-owned
 * ::ardop_runtime; W2 (the platform shell) adds ALSA/WinMM, sockets and the
 * wall clock around it.
 */

/**
 * @brief What an observer can be told. One presentation-agnostic event stream
 *        that replaces the old on_host/on_data/on_ptt callbacks and subsumes the
 *        WebGui's wg_send_* surface (analysis/13 W2.4).
 *
 * The runtime is the single point that sees every demod event, link action, and
 * state change, so it is where observation is generalised. A consumer -- a
 * logger, the TCP host interface, a metrics sink, a future UI, a test -- reads
 * this instead of reaching into the DSP/protocol.
 *
 * Bulk DSP visualisation (spectrum, constellation) is deliberately *not* here.
 * It is high rate, display-shaped, and meaningless to every consumer but a
 * panel, so it travels on its own channel -- see telemetry.h and
 * ardop_runtime_set_telemetry(). Keeping the two apart is what lets this remain
 * a small, presentation-agnostic event bus.
 */
typedef enum {
	ARDOP_OBS_STATE,        /**< Link state changed (state, remote). */
	ARDOP_OBS_MODE,         /**< Protocol mode changed (mode). */
	ARDOP_OBS_BANDWIDTH,    /**< Negotiated session width changed (bandwidth Hz). */
	ARDOP_OBS_PTT,          /**< Transmitter keyed/unkeyed (key). */
	ARDOP_OBS_LEADER,       /**< Acquisition started (offset_hz, sn). */
	ARDOP_OBS_RX_FRAME,     /**< Frame decoded (frame_type, quality, sn). */
	ARDOP_OBS_RX_FRAME_BAD, /**< Frame acquired but failed (frame_type). */
	ARDOP_OBS_TX_FRAME,     /**< Frame handed to the modulator (frame_type). */
	ARDOP_OBS_BUSY,         /**< Channel-busy state changed (busy). */
	ARDOP_OBS_BUFFER,       /**< Outbound TX buffer length changed (buffer_len). */
	ARDOP_OBS_HOST_MSG,     /**< Host-protocol text (text) -- was on_host. */
	ARDOP_OBS_RX_DATA,      /**< Decoded payload (tag, data, data_len) -- was on_data. */
} ardop_obs_kind;

/**
 * @brief One observation. Flat like ::ardop_action: read only the fields the
 *        @p kind documents. Pointers are borrowed and valid only for the call.
 */
typedef struct {
	ardop_obs_kind kind;
	ardop_link_state state;   /**< STATE. */
	const char *remote;       /**< STATE: the other station, or "". */
	ardop_link_mode mode;     /**< MODE. */
	int bandwidth;            /**< BANDWIDTH: session width in Hz. */
	bool key;                 /**< PTT. */
	bool busy;                /**< BUSY. */
	uint8_t frame_type;       /**< RX_FRAME / RX_FRAME_BAD / TX_FRAME. */
	int quality;              /**< RX_FRAME: decode quality 0..100. */
	int sn;                   /**< LEADER / RX_FRAME: signal/noise, dB. */
	float offset_hz;          /**< LEADER: measured tuning error, Hz. */
	size_t buffer_len;        /**< BUFFER: bytes queued to send. */
	const char *text;         /**< HOST_MSG: the message. */
	const char *tag;          /**< RX_DATA: "ARQ"/"FEC". */
	const uint8_t *data;      /**< RX_DATA: payload. */
	size_t data_len;          /**< RX_DATA: payload length. */
} ardop_obs;

/** @brief Observer callback: receives one ::ardop_obs. Must not block or reenter. */
typedef void (*ardop_observer_fn)(void *ctx, const ardop_obs *o);

/** Maximum registered observers. */
#define ARDOP_MAX_OBSERVERS 4

/**
 * @brief One station's core runtime. Caller-owned; large (embeds the demod,
 *        link and a full transmit sample buffer). Zero then ardop_runtime_init().
 */
typedef struct {
	ardop_demod demod;
	ardop_link link;
	ardop_rs rs;

	/* transmit in progress: begun into tx_samples, drained by pull_tx. */
	ardop_mod mod;
	int16_t tx_samples[ARDOP_MOD_MAX_SAMPLES];
	bool tx_active;
	bool ptt_keyed;   /* PTT edge state, so back-to-back frames don't re-key. */
	uint64_t now;     /* last time seen, so pull_tx can step the link (TX_DONE). */

	/* Outbound data queue the link drains (host SEND_DATA fills it). */
	uint8_t tx_queue[16384];

	/* Candidate frame types for the (receive-only) frame-type decode. */
	uint8_t valid_types[256];
	int valid_len;

	/* Channel-busy detection: RX samples are accumulated into 1024-sample
	 * windows and analysed while idle (DISC); a change emits ARDOP_OBS_BUSY. */
	ardop_busy_detector busy;
	float busy_window[513];
	int16_t busy_accum[ARDOP_BUSY_WINDOW];
	size_t busy_accum_len;
	int busy_det;         /* sensitivity 0..10 (host BUSYDET); 0 disables. */
	bool busy_state;      /* last reported busy, for change detection. */

	/* Observers, and the last values state-diffs are compared against. */
	struct {
		ardop_observer_fn fn;
		void *ctx;
	} observers[ARDOP_MAX_OBSERVERS];
	int n_observers;
	ardop_link_state last_state;
	ardop_link_mode last_mode;
	int last_bw;
	size_t last_buffer;

	/* Telemetry sink (see telemetry.h). NULL -- the default -- means the
	 * spectrum is never computed outside the busy detector's own use and
	 * no constellation is gathered, so a headless daemon pays nothing. */
	ardop_telemetry_fn tlm_fn;
	void *tlm_ctx;
	int16_t tlm_accum[ARDOP_BUSY_WINDOW]; /* own accumulator: see feed_busy. */
	size_t tlm_accum_len;
	float tlm_mag[ARDOP_BUSY_MAG_BINS];   /* scratch for one spectrum row. */
	int16_t tlm_phase[ARDOP_TLM_MAX_POINTS];
	int16_t tlm_pmag[ARDOP_TLM_MAX_POINTS];
	int16_t last_sn;      /* last frame's S/N, for the status mirror. */
	int16_t last_quality; /* last frame's decode quality. */
} ardop_runtime;

/**
 * @brief Register the telemetry sink, or NULL to disable.
 *
 * Enabling it makes ardop_runtime_rx() compute a spectrum row per 1024 samples
 * and gather a constellation snapshot per decoded frame. The sink is called
 * from the audio path and must not block; ::ardop_telemetry pointers are
 * borrowed and valid only for the call.
 */
void ardop_runtime_set_telemetry(ardop_runtime *rt, ardop_telemetry_fn fn,
				 void *ctx);

/**
 * @brief Emit a ::ARDOP_TLM_STATUS record summarising the current state.
 *
 * The discrete-state mirror the telemetry stream carries so a display needs
 * only one connection (see telemetry.h). Call after state may have changed, and
 * periodically so a newly attached display fills in promptly.
 */
void ardop_runtime_telemetry_status(ardop_runtime *rt);

/**
 * @brief Report the capture level to the telemetry sink.
 *
 * Computed by the caller's loop over each captured block rather than inside the
 * runtime, because it describes the *device*, not the protocol: a level that is
 * clipping or inaudible is a mis-set mixer, and the runtime never sees the gain
 * staging that caused it.
 *
 * @param rt       Runtime.
 * @param samples  Captured samples.
 * @param n        Number of samples.
 */
void ardop_runtime_telemetry_audio(ardop_runtime *rt, const int16_t *samples,
				   size_t n);

/**
 * @brief Register an observer. Up to ::ARDOP_MAX_OBSERVERS; extras are ignored.
 */
void ardop_runtime_observe(ardop_runtime *rt, ardop_observer_fn fn, void *ctx);

/**
 * @brief Initialise a runtime: RS context, demod, and a disconnected ARQ link.
 *
 * The caller then sets the link's configuration (`rt->link.mycall`, bandwidth,
 * `listening`, …) and registers observers with ardop_runtime_observe() before
 * driving it. The demodulator runs receive-only (session-independent frame-type
 * decode), which needs no session-key threading for a single-session channel.
 *
 * @param rt      Runtime to initialise (zeroed on return, then set up).
 * @param rslens  RS parity lengths the frames use (as ardop_rs_init).
 * @param n_rs    Number of entries in @p rslens.
 * @return true on success.
 */
ARDOP_MUSTUSE bool ardop_runtime_init(ardop_runtime *rt, const int *rslens,
				      int n_rs);

/**
 * @brief Feed captured samples: demodulate, step the link, perform its actions.
 *
 * @param rt          Runtime.
 * @param samples     Captured samples.
 * @param n           Number of samples.
 * @param now_samples Current time as an elapsed sample count.
 */
void ardop_runtime_rx(ardop_runtime *rt, const int16_t *samples, size_t n,
		      uint64_t now_samples);

/** @brief Service the link's timers (the ARDOP_IN_NONE step). */
void ardop_runtime_timer(ardop_runtime *rt, uint64_t now_samples);

/** @brief Inject a host command (connect, send-data, disconnect, break, …). */
void ardop_runtime_host(ardop_runtime *rt, const ardop_host_cmd *cmd,
			uint64_t now_samples);

/**
 * @brief Drain up to @p max transmit samples, if a frame is being sent.
 *
 * @return The count written; 0 when idle or the frame just finished (on which it
 *         also unkeys PTT).
 */
size_t ardop_runtime_pull_tx(ardop_runtime *rt, int16_t *out, size_t max);

/** @brief Whether a transmission is in progress. */
bool ardop_runtime_tx_active(const ardop_runtime *rt);

#endif /* ARDOP_SHELL_RUNTIME_H_ */
