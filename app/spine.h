#ifndef ARDOP_APP_SPINE_H_
#define ARDOP_APP_SPINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/platform.h"
#include "shell/runtime.h"
#include "shell/telemetry.h"

/**
 * @file spine.h
 * @brief The station application's embedding seam.
 *
 * [analysis/14](../analysis/14-station-application.md) workstream A. This is the
 * whole interface between the modem and everything built on top of it: the user
 * interface (workstream C) and the chat/file protocol (workstream E) include this
 * header and nothing else from `app/`.
 *
 * The one rule everything here follows from:
 *
 * > **One runtime, one thread.** Nothing outside the modem thread touches
 * > ::ardop_runtime or anything reachable from it, for any reason, ever.
 *
 * That includes configuration, which is why there is no "set the callsign"
 * function that writes a struct field. Everything crossing the boundary crosses
 * it through one of three lock-free rings, and every function below is labelled
 * with the thread that may call it.
 *
 * ### The three queues
 *
 * | Direction | Loss | Carries |
 * |---|---|---|
 * | down | refuses when full | commands, config, submitted payload |
 * | up, display | **lossy** | spectrum, constellation, levels, state mirror |
 * | up, events | **lossless** | received payload, host text, ownership, faults |
 *
 * The split exists because the two upward channels have opposite failure modes.
 * A stale waterfall row is worse than a missing one, so the display queue drops
 * under pressure and says how much it dropped. ::ARDOP_OBS_RX_DATA carries
 * *payload*, and dropping that would corrupt a file transfer while looking like
 * a channel fault, so the event queue never drops -- it applies backpressure to
 * transmission instead (see ::app_tx_submit).
 *
 * ### What is deliberately not here
 *
 * No telemetry *server*. The application is the display, so it consumes the sink
 * in process; `ardopb --telemetry` is unaffected and still serves `gui/` over
 * TCP. No thread creation, no mutexes, no wakeup signal: the spine is
 * thread-agnostic, and whoever embeds it decides which thread runs ::app_step.
 */

/** Longest text an ::app_event carries, NUL included. `ardop_link::out_host` is
 *  128 bytes and host replies are short; the slack is for future reply text. */
#define APP_TEXT_MAX 256

/** Default ring sizes. Override per-field in ::app_config; 0 means "this". */
#define APP_CMD_BYTES_DEFAULT     (32u * 1024u)
#define APP_DISPLAY_BYTES_DEFAULT (128u * 1024u)
#define APP_EVENT_BYTES_DEFAULT   (64u * 1024u)

/**
 * Headroom in the event ring that ordinary records must leave free, so that a
 * queue which has just filled can still deliver the one record saying so.
 */
#define APP_EVENT_RESERVE 512u

/** Largest single ::app_tx_submit chunk, so one call cannot monopolise the ring. */
#define APP_TX_CHUNK 4096u

/* --- construction ---------------------------------------------------------- */

/**
 * @brief Construction-time knobs. Zero-initialise and set what you care about;
 *        every 0 means "the documented default".
 */
typedef struct {
	size_t cmd_bytes;      /**< Downward ring. Default ::APP_CMD_BYTES_DEFAULT. */
	size_t display_bytes;  /**< Lossy ring. Default ::APP_DISPLAY_BYTES_DEFAULT. */
	size_t event_bytes;    /**< Lossless ring. Default ::APP_EVENT_BYTES_DEFAULT. */

	/**
	 * Compute spectrum and constellation telemetry.
	 *
	 * A real application always wants this; a headless script run usually does
	 * not, and leaving it off means the FFT and the constellation gather never
	 * run at all (`shell/runtime.h:133-135`). It is not free.
	 */
	bool telemetry;
} app_config;

/** Opaque. Holds the heap ::ardop_runtime and the three rings. */
typedef struct app_spine app_spine;

/* --- transmission ---------------------------------------------------------- */

/** Why ::app_tx_submit accepted fewer bytes than it was offered. */
typedef enum {
	APP_TX_OK = 0,     /**< Everything offered was accepted. */
	APP_TX_FULL,       /**< The link's transmit queue has no room right now. */
	APP_TX_TNC_OWNS,   /**< A TNC client is attached and owns the link. */
	APP_TX_CONGESTED,  /**< The event queue is backed up; see the file comment. */
	APP_TX_NO_ROOM,    /**< The command ring itself is full. */
	APP_TX_FAULTED,    /**< A device fault is latched. Recover or reselect. */
	APP_TX_CLOSED,     /**< The spine is shutting down. */
} app_tx_status;

/**
 * @brief What a fault was, without naming a device.
 *
 * The spine owns no devices and does not include `shell/fault.h`, so this is its
 * own vocabulary; whoever owns the backend maps ::ardop_fault onto it. Text
 * alone was not enough: an interface that wants to offer "choose another capture
 * device" cannot get there by matching on a sentence.
 */
typedef enum {
	APP_FAULT_NONE = 0,
	APP_FAULT_CAPTURE,   /**< The capture device stopped delivering. */
	APP_FAULT_PLAYBACK,  /**< Playback stalled; the ring never drained. */
	APP_FAULT_PTT,       /**< Keying failed. The rig may still be keyed. */
	APP_FAULT_QUEUE,     /**< The lossless event queue overflowed. */
	APP_FAULT_OTHER,
} app_fault;

/** @brief What happened to a device. Rides ::APP_EV_DEVICE in @c code. */
typedef enum {
	APP_DEV_EV_OPENED = 0,
	APP_DEV_EV_SUBSTITUTED,  /**< Opened, but not the device that was asked for. */
	APP_DEV_EV_OPEN_FAILED,
	APP_DEV_EV_CLOSED,
	APP_DEV_EV_RECOVERED,    /**< Rebuilt after a fault; transmit is allowed again. */
	APP_DEV_EV_PTT_TEST,
} app_device_code;

/**
 * @brief Whether transmission is permitted, and why not.
 *
 * ::APP_RUN_FAULTED replaces what used to be a one-way `stopping` flag.
 * `shell/fault.h` names the intended reaction to a device fault as "abort the
 * ARQ session, unkey, tell the operator, offer reselection" -- and reselection
 * needs the reverse edge. Without it a spine that had seen one fault could never
 * transmit again, which made a device-selection screen structurally impossible:
 * unplugging a USB interface would mean restarting the program.
 */
typedef enum {
	APP_RUN_OK = 0,
	APP_RUN_FAULTED,
	APP_RUN_CLOSING,   /**< Terminal. Only ::app_close sets this. */
} app_run_state;

/** @brief Human-readable ::app_tx_status, for logs and status lines. */
const char *app_tx_status_str(app_tx_status s);

/* --- events ---------------------------------------------------------------- */

/** What an ::app_event can be. */
typedef enum {
	APP_EV_HOST_MSG,  /**< @c text: an unsolicited notification from the link. */
	APP_EV_REPLY,     /**< @c text: the answer to a submitted command. */
	APP_EV_RX_DATA,   /**< @c tag + @c data + @c data_len: received payload. */
	APP_EV_OWNER,     /**< @c flag: a TNC client attached (true) or left. */
	APP_EV_FAULT,     /**< @c text + @c code (an ::app_fault). */
	APP_EV_DEVICE,    /**< @c text + @c code (an ::app_device_code). */
} app_event_kind;

/**
 * @brief One event, fully owned by the caller.
 *
 * Deliberately by value and not by pointer. The observations this is built from
 * borrow pointers into the link that are overwritten by the *next link step* --
 * not merely by the next callback -- so a queue of pointers would hand the
 * consumer several copies of the last message of each step. Everything is copied
 * at the callback; this struct is what survives.
 */
typedef struct {
	app_event_kind kind;
	char text[APP_TEXT_MAX];  /**< HOST_MSG / REPLY / FAULT. NUL-terminated. */
	char tag[4];              /**< RX_DATA: "ARQ"/"FEC"/... NUL-terminated. */
	bool flag;                /**< OWNER: attached. */
	int code;                 /**< FAULT: ::app_fault. DEVICE: ::app_device_code. */
	size_t data_len;          /**< RX_DATA. */
	uint8_t data[ARDOP_DEMOD_MAX_PAYLOAD];  /**< RX_DATA. */
} app_event;

/* --- configuration --------------------------------------------------------- */

/**
 * @brief A setting.
 *
 * The seam is typed so no user interface ever formats a protocol string. The
 * *implementation* is the frozen TNC text protocol: the modem thread turns each
 * of these into the equivalent line and runs it through ::ardop_host_command,
 * which already validates callsigns, bandwidth names and ranges and produces the
 * canonical reply. One validator means a guest's `MYCALL` and a settings screen's
 * `MYCALL` cannot drift apart. The reply arrives as ::APP_EV_REPLY.
 */
typedef enum {
	APP_CFG_MYCALL,        /**< @c str, e.g. "N0CALL-4". */
	APP_CFG_GRIDSQUARE,    /**< @c str, e.g. "FN31pr". */
	APP_CFG_ARQBW,         /**< @c str, one of the frozen names ("500MAX"). */
	APP_CFG_PROTOCOLMODE,  /**< @c str: "ARQ", "FEC" or "RXO". */
	APP_CFG_FECMODE,       /**< @c str: a data frame type name. */
	APP_CFG_FECREPEATS,    /**< @c num: 0..5. */
	APP_CFG_LISTEN,        /**< @c flag. */
	APP_CFG_AUTOBREAK,     /**< @c flag. */
	APP_CFG_FSKONLY,       /**< @c flag. */
	APP_CFG_USE600MODES,   /**< @c flag. */
	APP_CFG_ENABLEPINGACK, /**< @c flag. */

	/**
	 * Channel-busy sensitivity, 0..10; 0 disables detection.
	 *
	 * The one setting with no text equivalent: it lives on the runtime rather
	 * than the link (`shell/runtime.h:119`) and `shell/host.c` has no
	 * `BUSYDET` command, so this is applied by direct write on the modem
	 * thread. If a host command is ever added, move it to the text path with
	 * the rest.
	 */
	APP_CFG_BUSYDET,       /**< @c num: 0..10. */
} app_cfg_key;

/**
 * Longest string a setting may carry, NUL included.
 *
 * Every one of them is short by nature -- the longest callsign is ten
 * characters, a grid square six, the widest bandwidth name nine -- so a value
 * anywhere near this is a bug in the caller rather than an unusual station.
 * ::app_submit_config rejects a longer one instead of truncating it, because a
 * silently shortened callsign is a station identifying itself as somebody else.
 */
#define APP_CFG_STR_MAX 64

/** @brief The value for an ::app_cfg_key. Read the member the key documents. */
typedef union {
	const char *str;  /**< Copied into the queue slot; need not outlive the call. */
	long num;
	bool flag;
} app_cfg_value;

/* --- the TNC transport ----------------------------------------------------- */

/**
 * @brief The seam between the spine and a TCP TNC server.
 *
 * Held behind an ops table for the same reason ::ardop_platform_ops is: it keeps
 * every socket out of `app/spine.c`, so the spine needs no feature-test macro,
 * links nothing platform-specific, and can be tested and sanitised with no
 * network at all. `app/tnc_host_tcp.c` is the real implementation over
 * `shell/host_tcp.c`; a test supplies a fake; a platform with no listening
 * sockets passes NULL and everything else still works.
 */
typedef struct {
	void *ctx;

	/** Accept clients and process pending traffic. Called once per ::app_step. */
	void (*service)(void *ctx, ardop_runtime *rt, uint64_t now);

	/**
	 * True while a client is attached.
	 *
	 * This is the ownership signal. There is one link, one session and one
	 * 16 kB transmit queue; if a guest's Pat session and the application's own
	 * file transfer both append to it the two byte streams interleave and both
	 * are destroyed. So a connected client *takes over*: the application's own
	 * transmission and session commands are refused for as long as it stays.
	 * Presence rather than a claim protocol means there is no state to get
	 * stuck in -- the TCP disconnect is the release.
	 */
	bool (*attached)(void *ctx);

	/** Forward an unsolicited notification to the client. */
	void (*notify)(void *ctx, const char *msg);

	/** Forward received payload to the client's data channel. */
	void (*send_data)(void *ctx, const char *tag, const uint8_t *data,
			  size_t len);
} app_tnc_ops;

/* --- status ---------------------------------------------------------------- */

/**
 * @brief The bits of state that do not ride the display queue.
 *
 * Deliberately small. Link state, mode, bandwidth, S/N, quality, busy and PTT
 * all arrive as ::ARDOP_TLM_STATUS records through ::app_display_pop -- the
 * runtime mirrors them there on every observation -- so duplicating them here
 * would mean two sources of the same truth, published at different instants.
 *
 * **For display only, never for a decision.** Each field is an independent
 * atomic load, so this is not a snapshot of one instant. ::app_tx_credit is the
 * only correct source of transmit credit; @ref app_status::tx_credit here is a
 * copy of it for putting on screen.
 */
typedef struct {
	bool tnc_attached;         /**< A guest owns the link. */
	app_run_state run;         /**< Whether transmission is permitted. */
	app_fault fault;           /**< The latched fault, for a banner. */
	size_t tx_credit;          /**< Bytes ::app_tx_submit would take now. */
	app_tx_status tx_reason;   /**< Why, when @ref tx_credit is 0. */
	uint64_t display_drops;    /**< Display records dropped since open. */
	uint64_t event_lost;       /**< Lossless records lost. Must stay 0. */
	size_t event_used;         /**< Bytes queued in the event ring. */
	size_t event_cap;          /**< Its capacity, so a UI can draw a gauge. */
	size_t display_used;
	size_t display_cap;
} app_status;

/* --- lifetime (modem thread) ----------------------------------------------- */

/**
 * @brief Allocate and initialise a spine.
 *
 * Heap, not static: ::ardop_runtime is around 300 kB, which is too much for a
 * thread stack on some targets, and a process may want a second one later.
 *
 * @param cfg Knobs, or NULL for all defaults.
 * @return The spine, or NULL if allocation or runtime init failed.
 */
app_spine *app_open(const app_config *cfg);

/**
 * @brief Tear down. Unkeys the transmitter. Safe on NULL.
 *
 * **Call this while the audio backend is still open**, then close the backend,
 * then the PTT. Unkeying goes out through the bound platform's `set_ptt`, so
 * closing the backend first would key into freed memory; and PTT dies last
 * because unkeying has to outlive the device that keyed. `app_devices_close`
 * does the whole sequence so nobody has to remember it.
 */
void app_close(app_spine *sp);

/**
 * @brief Bind the audio backend, or NULL to detach. **Modem thread only.**
 *
 * The runtime outlives the backend: changing devices tears this down and builds
 * another while the link, its configuration and the sample clock continue.
 *
 * @param block Modem samples per loop iteration, or 0 for the
 *              ::ARDOP_LOOP_BLOCK default. A miniaudio backend passes
 *              ::ardop_backend_ma_block.
 *
 *              It is a parameter of *this* call rather than a setter of its own
 *              because a backend and the block it wants are one fact: two calls
 *              means an iteration in which the block belongs to the previous
 *              device. That was a real defect -- the block used to be preserved
 *              across a swap, so a new backend's smaller preferred block was
 *              always overwritten by the old one and every device change
 *              silently reverted to 100 ms.
 *
 * @return false, changing nothing, if a transmission is in progress -- a device
 *         swap mid-transmission would leave the radio keyed with nothing able to
 *         unkey it. Disconnect first.
 */
bool app_set_platform(app_spine *sp, const ardop_platform_ops *ops, size_t block);

/** @brief Bind the TNC transport, or NULL to detach. **Modem thread only.** */
void app_set_tnc(app_spine *sp, const app_tnc_ops *tnc);

/**
 * @brief Run one iteration: drain commands, service the TNC, step the modem,
 *        republish transmit credit. **Modem thread only.**
 *
 * Blocks for as long as the backend's capture call does -- around 100 ms with a
 * real sound card, which is what paces the whole loop.
 */
void app_step(app_spine *sp);

/**
 * @brief Report a device fault. **Modem thread only.** First fault wins.
 *
 * Detection belongs to whoever owns the backend (only it knows to call
 * ::ardop_backend_ma_fault); the reaction belongs here, so that a user interface
 * sees it. Disconnects the session, forces the transmitter down, tells any guest,
 * emits ::APP_EV_FAULT carrying @p code, and refuses transmit data until
 * ::app_clear_fault.
 *
 * **The in-flight ARQ session never survives, and is not resumed.** Say so in
 * @p what, not only in a header -- an operator who sees "capture device lost"
 * and nothing else will ask whether their transfer continued.
 */
void app_report_fault(app_spine *sp, app_fault code, const char *what);

/** @brief Emit an ::APP_EV_DEVICE. **Modem thread only.** */
void app_report_device(app_spine *sp, app_device_code code, const char *text);

/**
 * @brief Permit transmission again. **Modem thread only.**
 * @return The state that was in force before the call.
 *
 * The spine owns no device and therefore cannot repair one. **The caller must
 * have made the device good before calling this** -- which, for both the audio
 * backend and the PTT object, means close and open, because neither has a reopen
 * and neither needs one: analysis/14 Decision 5 already makes the runtime
 * outlive the backend, so a rebuilt device inherits the link, the configuration
 * and the sample clock.
 */
app_run_state app_clear_fault(app_spine *sp);

/** @brief The latched fault, or ::APP_FAULT_NONE. **Modem thread only.** */
app_fault app_fault_state(const app_spine *sp);

/** @brief Whether transmission is currently permitted. **Modem thread only.** */
app_run_state app_run_state_of(const app_spine *sp);

/**
 * @brief Elapsed samples since the spine opened. **Modem thread only.**
 *
 * The modem's only clock, and the one every protocol deadline is measured
 * against. It counts audio, not wall time -- with the in-memory loopback the two
 * are unrelated, and even with a real device the sample clock is the authority.
 * At 12000 samples per second, divide by 12000 for seconds on the air.
 */
uint64_t app_elapsed(const app_spine *sp);

/**
 * @brief The runtime.
 *
 * **MODEM THREAD ONLY, AND ONLY BETWEEN ::app_step CALLS.** It exists because a
 * platform layer needs to reach the link to react to a device fault and because
 * tests assert on link state. Everything an application needs is above this line;
 * if you are reaching for this from a user interface, the seam is missing
 * something and this is not the fix.
 */
ardop_runtime *app_runtime(app_spine *sp);

/* --- submission (one thread, not the modem thread) -------------------------- */

/**
 * The functions below may be called from exactly **one** thread, which must not
 * be the modem thread while ::app_step is running. The rings are
 * single-producer / single-consumer; a second submitting thread is a data race,
 * not a slow path. There is only ever one in practice -- guests submit nothing,
 * because the TNC transport is serviced *on the modem thread* inside ::app_step.
 */

/**
 * @brief Offer @p len bytes for transmission.
 *
 * **Partial by design.** The return value is 0..@p len; the caller advances its
 * own cursor by it and offers the remainder later. Nothing is queued on the
 * caller's behalf, there is no completion callback, and there is no blocking
 * variant -- a zero return means "not now", and the answer is to come back on
 * whatever cadence the caller already has. There is no credit-available event
 * either: credit changes ten or more times a second as frames drain, so an event
 * per change would be noise. Poll ::app_tx_credit.
 *
 * Returns 0 for all of: the link's queue is full, a guest owns the link, the
 * event queue is backed up, the command ring is full, or the spine is stopping.
 * @p why (may be NULL) tells them apart.
 */
size_t app_tx_submit(app_spine *sp, const void *data, size_t len,
		     app_tx_status *why);

/** @brief Bytes ::app_tx_submit would accept right now.
 *
 * Not `const`-qualified, for `shell/ring.h`'s reason: C11's atomic_load_explicit
 * takes a non-const pointer, so a const parameter would only be discarded again
 * inside. */
size_t app_tx_credit(app_spine *sp);

/**
 * @brief Queue a raw TNC command line, as if a client had typed it.
 *
 * For a debug console, and for scripts. Ordinary settings should go through
 * ::app_submit_config, which cannot produce a malformed line. The reply arrives
 * as ::APP_EV_REPLY.
 *
 * @return false if the command ring is full.
 */
bool app_submit_line(app_spine *sp, const char *line);

/**
 * @brief Queue a typed link command (connect, disconnect, abort, break, ...).
 *
 * ::ARDOP_CMD_SEND_DATA is rejected here: it has no admission control and would
 * bypass the credit. Use ::app_tx_submit.
 *
 * @return false if the ring is full or the command cannot be carried.
 */
bool app_submit_cmd(app_spine *sp, const ardop_host_cmd *cmd);

/** @brief Queue a setting. @return false if the ring is full. */
bool app_submit_config(app_spine *sp, app_cfg_key key, app_cfg_value value);

/* --- draining (the same thread as submission) ------------------------------- */

/**
 * @brief Take the oldest event. @return true if @p out was filled.
 *
 * Drain until this returns false; the queue is lossless and nothing else empties
 * it.
 */
bool app_events_pop(app_spine *sp, app_event *out);

/** @brief Whether ::app_events_pop would return true. */
bool app_events_pending(app_spine *sp);

/**
 * @brief Take the oldest display record. @return true if @p out was filled.
 *
 * Drain the whole queue every frame, then draw. Coalesce by kind as you go:
 * ::ARDOP_TLM_CONSTELLATION, ::ARDOP_TLM_AUDIO and ::ARDOP_TLM_STATUS are
 * whole-value replacements so only the last matters, but every
 * ::ARDOP_TLM_SPECTRUM is one scan line of the waterfall and skipping them tears
 * the picture.
 *
 * The records are the same bytes `gui/` already parses off a modem's telemetry
 * port, so a source that is a *remote* station rather than the embedded modem
 * substitutes here without the consumer changing.
 */
bool app_display_pop(app_spine *sp, ardop_tlm_decoded *out);

/** @brief Fill @p out. See ::app_status -- for display, not for decisions. */
void app_snapshot(app_spine *sp, app_status *out);

#endif /* ARDOP_APP_SPINE_H_ */
