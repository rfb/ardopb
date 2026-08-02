#ifndef ARDOP_SHELL_PTT_CAT_H_
#define ARDOP_SHELL_PTT_CAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file ptt_cat.h
 * @brief Keying a radio with its own CAT command, over the serial port.
 *
 * For a modern transceiver the keying line *is* the CAT link: an IC-7300, an
 * FT-991A or a Xiegu X6200 carries audio and control down one USB cable, and
 * hamlib gives that whole family `ptt_type = RIG_PTT_RIG` -- **PTT is a command,
 * not an RTS line**. Asserting RTS on such a radio does nothing at all, which is
 * the failure operators find hardest to diagnose, because everything looks
 * connected and nothing transmits.
 *
 * The command is small enough to speak directly. From Icom's own CI-V reference
 * guide, command `1C` sub-command `00`, "Send/read the transceiver's status":
 *
 *     FE FE <rig> E0 1C 00 01 FD   ->  FE FE E0 <rig> FB FD     key,   acked
 *     FE FE <rig> E0 1C 00 00 FD   ->  FE FE E0 <rig> FA FD     unkey, refused
 *
 * ## Why not drive rigctld for this
 *
 * `rigctld:` still exists and is still the escape hatch for anything exotic,
 * remote, or already configured. What this avoids is *managing* that daemon:
 * eight bytes down a port we already know how to open needs no hamlib install,
 * no process to spawn and reap on two platforms, no stderr to capture, and no
 * exposure to a command-line interface we do not control.
 *
 * It is also safer. Killing rigctld does **not** unkey a CAT-keyed rig, because
 * closing a serial handle sends no command -- so owning that process would mean
 * a crash on our side could orphan a daemon holding a transmitter up. Fewer
 * moving parts is the right answer here, not more.
 *
 * ## The acknowledgement is kept
 *
 * For the reason `shell/ptt.c`'s rigctld path already waits for `RPRT 0`: a CAT
 * link that has died has to become a *fault* rather than a silent
 * no-transmission. CI-V's `FB`/`FA` gives exactly that. Kenwood's `TX;` has no
 * reply at all, so that family is unacknowledged -- an honest gap, named here
 * rather than hidden.
 *
 * Everything below is a pure function. There is no radio behind this project, so
 * the only things that *can* be verified are the bytes and the classification,
 * and they are, exhaustively, in `test/core/test_ptt.c`.
 */

/** @brief Which CAT dialect. */
typedef enum {
	ARDOP_CAT_CIV = 0,   /**< Icom, and every Xiegu, which emulates it. */
	ARDOP_CAT_KENWOOD,   /**< `TX;` / `RX;`. No reply. */
	ARDOP_CAT_YAESU,     /**< `TX1;` / `TX0;`. */
} ardop_cat_family;

/** @brief The CI-V address a radio answers on when none is given. */
#define ARDOP_CIV_DEFAULT_ADDR 0x00u   /* broadcast; many rigs accept it */

/** @brief Longest frame any family produces. */
#define ARDOP_CAT_FRAME_MAX 16

/**
 * @brief Build the key/unkey command for @p family.
 *
 * @param addr CI-V transceiver address; ignored by the other families.
 * @param out  At least ::ARDOP_CAT_FRAME_MAX bytes.
 * @return Bytes written, or 0 if @p cap is too small or @p family is unknown.
 */
size_t ardop_cat_frame(ardop_cat_family family, uint8_t addr, bool key,
		       uint8_t *out, size_t cap);

/** @brief How a reply read so far should be treated. */
typedef enum {
	ARDOP_CAT_ACK = 0,    /**< The radio accepted it. */
	ARDOP_CAT_NAK,        /**< The radio refused it. */
	ARDOP_CAT_INCOMPLETE, /**< Not enough bytes yet; read more. */
	ARDOP_CAT_IGNORE,     /**< A complete frame, but not an answer to us. */
} ardop_cat_reply;

/**
 * @brief Classify @p n bytes of reply.
 *
 * @param consumed Receives the bytes this frame occupied, so a caller can skip
 *                 an ::ARDOP_CAT_IGNORE and keep reading. Set to 0 when the
 *                 answer is ::ARDOP_CAT_INCOMPLETE.
 *
 * ::ARDOP_CAT_IGNORE is not a curiosity: a rig with CI-V *transceive* enabled
 * echoes our own command back before answering it, and a reader that took the
 * echo for the reply would conclude the radio had accepted a frame it has not
 * seen yet. Kenwood, having no reply, reports ::ARDOP_CAT_ACK for zero bytes.
 */
ardop_cat_reply ardop_cat_parse_reply(ardop_cat_family family, uint8_t addr,
				      const uint8_t *buf, size_t n,
				      size_t *consumed);

/** @brief Whether @p family answers at all. Kenwood does not. */
bool ardop_cat_is_acknowledged(ardop_cat_family family);

/** @brief The spec keyword for @p family: "civ", "kenwood", "yaesu". */
const char *ardop_cat_family_str(ardop_cat_family family);

/** @brief Parse a keyword. @return false if it names no family. */
bool ardop_cat_family_from_str(const char *s, ardop_cat_family *out);

#endif /* ARDOP_SHELL_PTT_CAT_H_ */
