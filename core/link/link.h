#ifndef ARDOP_LINK_LINK_H_
#define ARDOP_LINK_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/locator.h"
#include "codec/rs.h"
#include "codec/stationid.h"
#include "link/bandwidth.h"
#include "modem/demodulate.h"

/**
 * @file link.h
 * @brief The ARQ/FEC/RXO link state machine.
 *
 * The top of the core: the protocol machine that decides what to transmit,
 * when, and what to tell the host, from the frames the demodulator recovers and
 * the commands the operator issues. It is the [analysis/02](../../analysis/02-protocol-fsm.md)
 * state table rebuilt as a pure step function per the
 * [analysis/10](../../analysis/10-modem-link-design.md) contract: inputs in,
 * actions out, no I/O, no transmitting from inside it, one sample clock.
 *
 * The pure protocol leaves it composes -- session id, callsign match, bandwidth
 * negotiation, ACK/NAK quality, control-frame builders -- are already ported and
 * proven (see this directory). This file adds the machine that sequences them.
 *
 * Built incrementally, one state at a time; ::ardop_link grows as states land.
 */

/** Convert a protocol millisecond constant to the sample clock (12 kHz). */
#define ARDOP_MS_TO_SAMPLES(ms) ((uint64_t)(ms) * 12u)

/** Maximum auxiliary callsigns a station can answer to. */
#define ARDOP_LINK_MAX_AUX 10

/** Scratch big enough for any single encoded frame the link hands out. */
#define ARDOP_LINK_OUT_FRAME_MAX 2048

/**
 * @brief The link protocol state.
 *
 * One enum in place of the inherited `ProtocolState`/`ARQState` pair
 * ([analysis/02](../../analysis/02-protocol-fsm.md) recommendation 2). Substates
 * are folded in as the states that need them are ported.
 */
typedef enum {
	ARDOP_LINK_DISC = 0,   /**< Not connected. */
	ARDOP_LINK_ISS_CON_REQ,/**< Sent a ConReq, awaiting ConAck. */
	ARDOP_LINK_ISS_CON_ACK,/**< Got the ConAck, replied, connection established. */
	ARDOP_LINK_ISS,        /**< Connected, this station is sending (ISS). */
	ARDOP_LINK_IRS_CON_ACK,/**< IRS, ConAck sent, awaiting first data. */
	ARDOP_LINK_IRS_DATA,   /**< IRS, receiving data (steady state). */
	ARDOP_LINK_IDLE,       /**< Connected ISS with nothing to send. */
	ARDOP_LINK_IRS_TO_ISS, /**< Sent BREAK, awaiting the ACK to become ISS. */
	ARDOP_LINK_IRS_FROM_ISS,/**< Just yielded the link; settling into IRS. */
	ARDOP_LINK_FEC_SEND,   /**< Broadcasting FEC data (no ACKs). */
} ardop_link_state;

/** @brief Which protocol is running. */
typedef enum {
	ARDOP_MODE_ARQ = 0,
	ARDOP_MODE_FEC,
	ARDOP_MODE_RXO,
} ardop_link_mode;

/* --- host commands (input) --- */

/** @brief A transition-driving command from the host/operator. */
typedef enum {
	ARDOP_CMD_CONNECT,     /**< Start an ARQ session. */
	ARDOP_CMD_DISCONNECT,  /**< Graceful disconnect. */
	ARDOP_CMD_ABORT,       /**< Immediate abort. */
	ARDOP_CMD_SEND_DATA,   /**< Append payload bytes to the TX queue. */
	ARDOP_CMD_SET_MODE,    /**< Switch ARQ/FEC/RXO. */
	ARDOP_CMD_FEC_SEND,    /**< Broadcast the queued data as FEC. */
	ARDOP_CMD_SEND_ID,     /**< Transmit an ID frame. */
	ARDOP_CMD_BREAK,       /**< Request the link (IRS -> ISS turnover). */
} ardop_host_cmd_kind;

/** @brief A host command and its parameters. */
typedef struct {
	ardop_host_cmd_kind kind;
	ardop_stationid target;       /**< CONNECT target. */
	ardop_arq_bandwidth bandwidth;/**< CONNECT bandwidth setting. */
	const uint8_t *data;          /**< SEND_DATA bytes (copied in). */
	size_t data_len;
	int arg;                      /**< SET_MODE value / bool flags. */
} ardop_host_cmd;

/* --- the unified input --- */

/** @brief Which kind of input a ::ardop_link_input carries. */
typedef enum {
	ARDOP_IN_NONE = 0,  /**< Timer service only. */
	ARDOP_IN_RX,        /**< A demodulator event. */
	ARDOP_IN_HOST,      /**< A host command. */
	ARDOP_IN_TX_DONE,   /**< A transmission just finished (shell observed it). */
} ardop_link_input_kind;

/**
 * @brief One input to the machine: a demod event, a host command, or nothing.
 *
 * All three input sources share one ordered stream so their relative order is
 * unambiguous ([analysis/10](../../analysis/10-modem-link-design.md) decision 6).
 */
typedef struct {
	ardop_link_input_kind kind;
	union {
		ardop_event rx;
		ardop_host_cmd host;
	} as;
} ardop_link_input;

/* --- actions (output) --- */

/** @brief What the machine asks the shell to do. */
typedef enum {
	ARDOP_ACT_SEND_FRAME,   /**< Modulate and transmit an encoded frame. */
	ARDOP_ACT_SET_TIMER,    /**< Wake the machine no later than a deadline. */
	ARDOP_ACT_NOTIFY_HOST,  /**< A host-protocol message (text in data). */
	ARDOP_ACT_DELIVER_DATA, /**< Decoded payload for the host. */
} ardop_action_kind;

/**
 * @brief One action the shell performs. Plain data, never a callback.
 *
 * For SEND_FRAME, @p data/@p data_len are the encoded bytes to hand to
 * `ardop_mod_begin` and point into the link's own storage, valid until the next
 * ardop_link_step(). SET_TIMER's @p deadline is an absolute sample count; the
 * shell need only call a NONE step at or after it (the machine re-checks its own
 * deadlines against the time it is given).
 */
typedef struct {
	ardop_action_kind kind;
	uint8_t frame_type;    /**< SEND_FRAME: the frame type. */
	const uint8_t *data;   /**< SEND_FRAME bytes / host text / payload. */
	size_t data_len;
	uint16_t leader_ms;    /**< SEND_FRAME: leader length. */
	uint64_t deadline;     /**< SET_TIMER: absolute sample count. */
} ardop_action;

/* --- the machine --- */

/**
 * @brief Link state. Caller-owned. Config fields are set directly by the
 *        caller; the rest is the machine's and evolves through ardop_link_step().
 */
typedef struct {
	/* --- configuration: set by the caller after ardop_link_init() --- */
	ardop_stationid mycall;                        /**< Our callsign. */
	ardop_stationid auxcalls[ARDOP_LINK_MAX_AUX];  /**< Aux callsigns. */
	size_t n_aux;
	ardop_locator grid;         /**< Our grid square (for ID frames). */
	const ardop_rs *rs;         /**< RS context for building/decoding frames. */
	uint8_t *tx_data;           /**< Caller-owned outbound queue (bytDataToSend). */
	size_t tx_cap;              /**< Capacity of @p tx_data. */
	ardop_arq_bandwidth bw_setting; /**< Our bandwidth setting. */
	uint16_t leader_ms;         /**< Leader length for transmissions. */
	uint16_t reply_leader_ms;   /**< Leader for quick ARQ replies (intARQDefaultDlyMs). */
	bool listening;             /**< Answer incoming connections. */
	bool enable_ping_ack;       /**< Reply to pings addressed to us (EnablePingAck). */
	bool fsk_only;              /**< Restrict data modes to 4FSK (FSKONLY). */
	bool use_600_modes;         /**< 600-baud FM data modes in play (Use600Modes). */
	int tuning_range;           /**< Tuning range, Hz; 0 selects the 2000 FM modes. */
	bool auto_break;            /**< Break automatically when the ISS idles (AutoBreak). */
	int arq_timeout;            /**< Seconds of silence before giving up, 30..240
	                             *   (ARQTimeout). Reset on every decoded frame
	                             *   while connected -- an active IDLE/ACK
	                             *   exchange never trips it, by design (rule
	                             *   1.7): this catches a genuinely dead link,
	                             *   not an unproductive but alive one. */
	uint8_t fec_frame_type;     /**< Data frame type used for FEC broadcast. */
	int fec_repeats;            /**< Extra sends of each FEC frame (0..5, FECRepeats). */

	/* --- machine state --- */
	ardop_link_mode mode;       /**< ARQ / FEC / RXO. */
	ardop_link_state state;     /**< Protocol state. */
	uint8_t session_id;         /**< Current session id. */
	uint8_t last_session_id;    /**< Retained after disconnect (for late DISC). */
	int session_bw;             /**< Negotiated session width, Hz. */
	bool pending;               /**< Connection pending (blnPending). */
	ardop_stationid remote;     /**< The other station (ARQStationRemote). */
	ardop_stationid local;      /**< The local call this session uses. */
	ardop_stationid final_id_call; /**< Callsign for the closing ID frame. */
	int avg_quality;            /**< Running decode-quality average. */
	int last_data_to_host;      /**< Type of the last data frame delivered
	                             *   (intLastARQDataFrameToHost); -1 = none. */
	int last_acked_type;        /**< Type of the last data frame ACKed
	                             *   (bytLastACKedDataFrameType); -1 = none. */
	bool break_pending;         /**< Host asked to BREAK; send it next chance (blnBREAKCmd). */

	/* --- ISS send state --- */
	size_t tx_len;              /**< Bytes queued in tx_data. */
	const uint8_t *modes;       /**< Gear-shift mode ladder (bytFrameTypesForBW). */
	const uint8_t *thresholds;  /**< Shift-up thresholds (bytShiftUpThresholds). */
	size_t modes_len;           /**< Number of modes. */
	int mode_ptr;               /**< Current mode index (intFrameTypePtr). */
	uint8_t last_data_sent;     /**< Type of the outstanding data frame. */
	uint8_t last_data_acked;    /**< Parity anchor for toggling (bytLastARQDataFrameAcked). */
	int outstanding_len;        /**< Payload bytes in the outstanding frame. */
	int fec_reps_sent;          /**< Repeats of the current FEC frame sent so far. */
	uint16_t last_fec_crc;      /**< CRC of the last FEC payload delivered (dedup). */
	bool fec_have_last;         /**< Whether last_fec_crc/last_data_to_host are set. */

	/* --- gear-shift state (Gearshift_9) --- */
	int ack_ctr;                /**< ACKs since the last shift (intACKctr). */
	int nak_ctr;                /**< NAKs since the last shift (intNAKctr). */
	int shift_up_dn;            /**< Pending shift: -1 down, +1 up, 0 none. */
	uint16_t mode_has_worked[16];   /**< Per-mode success count (ModeHasWorked). */
	uint16_t mode_has_been_tried[16];/**< Per-mode tried flag (ModeHasBeenTried). */
	uint16_t mode_naks[16];          /**< Per-mode NAK count (ModeNAKS). */

	/* --- timers, as absolute sample deadlines (0 = inactive) --- */
	uint64_t final_id_deadline; /**< tmrFinalID: when the closing ID may go. */
	uint64_t pending_deadline;  /**< tmrIRSPendingTimeout: auto-abort a stuck pending. */
	uint64_t stall_deadline;    /**< dttTimeoutTrip + ARQTimeout: give up on silence. */
	uint64_t repeat_deadline;   /**< Next resend of the current repeated frame. */
	uint16_t repeat_interval_ms;/**< Resend interval while repeating (0 = none). */
	uint8_t repeat_frame_type;  /**< Frame type to resend on the repeat timer. */
	size_t repeat_frame_len;    /**< Length of the frame in out_frame to resend. */
	int disc_repeat_count;      /**< DISC resends so far while disconnecting (0 = not). */

	/* --- scratch the emitted actions point into (valid until next step) --- */
	uint8_t out_frame[ARDOP_LINK_OUT_FRAME_MAX];  /**< SEND_FRAME bytes. */
	uint8_t out_data[ARDOP_DEMOD_MAX_PAYLOAD];    /**< DELIVER_DATA payload. */
	char out_host[128];         /**< NOTIFY_HOST text. */
} ardop_link;

/**
 * @brief Initialise a link to a clean disconnected ARQ state.
 *
 * Zeroes the machine and sets safe defaults (ARQ mode, DISC state, a 240 ms
 * leader). The caller then fills the configuration fields (mycall, auxcalls,
 * grid, bw_setting, listening) before stepping.
 */
void ardop_link_init(ardop_link *l);

/**
 * @brief Advance the machine by one input, returning the resulting actions.
 *
 * Pure: the same state and input always produce the same actions and next
 * state. Never performs I/O and never transmits -- transmission is a returned
 * ARDOP_ACT_SEND_FRAME the shell carries out.
 *
 * @param[in]  l           The link.
 * @param[in]  in          The input (demod event, host command, or NONE for
 *                         timer service). Must not be NULL; use kind NONE.
 * @param[in]  now_samples Current time as an elapsed sample count.
 * @param[out] actions     Caller array for emitted actions.
 * @param[in]  max_actions Capacity of @p actions.
 * @return The number of actions written.
 */
size_t ardop_link_step(ardop_link *l, const ardop_link_input *in,
		       uint64_t now_samples, ardop_action *actions,
		       size_t max_actions);

#endif /* ARDOP_LINK_LINK_H_ */
