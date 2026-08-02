#ifndef ARDOP_APPS_FECCHAT_H_
#define ARDOP_APPS_FECCHAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/asp_wire.h"

/**
 * @file fecchat.h
 * @brief The FEC profile of the application protocol: broadcast chat.
 *
 * [analysis/17](../analysis/17-application-protocol.md) §6 specifies `TEXT_B`,
 * and until now nothing sent one. `ardop-chat --fec` broadcast bare lines, which
 * has two consequences on a channel where *anyone* may be transmitting:
 *
 * - **You cannot tell who spoke.** Every line reads `peer>`.
 * - **Duplicate suppression was left to `core/link.c`**, which drops a frame
 *   whose type and CRC match the one before it. Its own comment admits the
 *   limitation: *"identical consecutive payloads are indistinguishable from
 *   repeats and are dropped"*. So saying the same thing twice loses the second,
 *   and two stations interleaving break the consecutive assumption the other
 *   way, letting a real repeat through.
 *
 * §6's answer to both is the same eight bytes of payload: a callsign and a
 * per-sender message id, deduplicated on `(callsign, msg_id)` over a sliding
 * window.
 *
 * ## Everything here is pure
 *
 * No sockets and no clock of its own -- time is a parameter. That is what lets
 * the dedup window, which is the only stateful part, be tested by calling it
 * with the times you want rather than by waiting five minutes.
 */

/** @brief §6's window. A repeat arrives within seconds; this is ample. */
#define FECCHAT_DEDUP_MS (5u * 60u * 1000u)

/** @brief How many recent messages are remembered. A busy net, generously. */
#define FECCHAT_SEEN_MAX 64

/** @brief What has been heard lately, so a repeat can be recognised. */
typedef struct {
	struct {
		char call[ASP_MAX_CALL];
		uint16_t msg_id;
		uint64_t at_ms;
		bool used;
	} seen[FECCHAT_SEEN_MAX];
	uint16_t next_id;   /**< Our own counter. Wrap is harmless; see §6. */
} fecchat;

/** @brief Start with nothing heard and our counter at zero. */
void fecchat_init(fecchat *f);

/**
 * @brief Should this message be shown?
 *
 * @return false if `(call, msg_id)` was already seen inside the window, in
 *         which case it is a `FECREPEATS` copy and has been shown already.
 *         Recording happens here too, so calling it twice for one message
 *         answers false the second time -- which is the point.
 */
bool fecchat_is_new(fecchat *f, const char *call, uint16_t msg_id,
		    uint64_t now_ms);

/**
 * @brief How much text fits in one frame of @p frame_type, for @p call.
 *
 * The reason this exists at all: the most robust FEC mode carries **16 bytes**
 * per frame, of which a `TEXT_B` header for a five-character callsign takes ten:
 * six characters of text are left. §6 designed the message without reference to
 * what a frame holds, so the budget has to be computed rather than assumed.
 *
 * @return Bytes of UTF-8 that fit, possibly 0 if the callsign alone will not.
 */
size_t fecchat_text_capacity(uint8_t frame_type, const char *call);

/**
 * @brief Encode one broadcast message, consuming our next message id.
 *
 * @param text  At most ::fecchat_text_capacity bytes; longer is refused rather
 *              than truncated, because half a sentence sent as though it were
 *              whole is worse than a message that did not go.
 * @return Bytes written to @p out, or 0 if it does not fit.
 */
size_t fecchat_encode(fecchat *f, const char *call, const char *text,
		      size_t text_len, uint8_t *out, size_t cap);

/** @brief What ::fecchat_decode concluded. */
typedef enum {
	FECCHAT_MESSAGE = 0,  /**< A TEXT_B: @p call and @p text are set. */
	FECCHAT_RAW,          /**< Not one. §6: show it as text anyway. */
} fecchat_kind;

/**
 * @brief Decode one received FEC payload.
 *
 * §6: *"a `TEXT_B` that fails to parse is displayed as raw text rather than
 * discarded"* -- broadcast is where this station is most likely to meet one
 * running something else, and a net where our messages are the only visible
 * ones would be worse than no framing at all.
 *
 * @param[out] call  The sender, or empty for ::FECCHAT_RAW.
 * @param[out] text  Points into @p data. Borrowed.
 */
fecchat_kind fecchat_decode(const uint8_t *data, size_t len, char *call,
			    size_t call_cap, uint16_t *msg_id,
			    const char **text, size_t *text_len);

#endif /* ARDOP_APPS_FECCHAT_H_ */
