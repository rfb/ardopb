#ifndef ARDOP_SHELL_RUNTIME_H_
#define ARDOP_SHELL_RUNTIME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/rs.h"
#include "link/link.h"
#include "modem/demodulate.h"
#include "modem/modulate.h"

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

/** Host-protocol text message out (NOTIFY_HOST). */
typedef void (*ardop_host_out_fn)(void *ctx, const char *msg);
/** Decoded payload out to the host (DELIVER_DATA). */
typedef void (*ardop_data_out_fn)(void *ctx, const uint8_t *data, size_t len);
/** Key or unkey the transmitter (SET_PTT / implied by SEND_FRAME). */
typedef void (*ardop_ptt_fn)(void *ctx, bool key);

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

	/* Host-facing callbacks. */
	ardop_host_out_fn on_host;
	ardop_data_out_fn on_data;
	ardop_ptt_fn on_ptt;
	void *ctx;
} ardop_runtime;

/**
 * @brief Initialise a runtime: RS context, demod, and a disconnected ARQ link.
 *
 * The caller then sets the link's configuration (`rt->link.mycall`, bandwidth,
 * `listening`, …) and the callbacks (`rt->on_host`/`on_data`/`on_ptt`/`ctx`)
 * before driving it. The demodulator runs receive-only (session-independent
 * frame-type decode), which needs no session-key threading for a single-session
 * channel.
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
