#ifndef ARDOP_SHELL_PTT_H_
#define ARDOP_SHELL_PTT_H_

#include <stdbool.h>
#include <stdint.h>

#include "shell/fault.h"

/**
 * @file ptt.h
 * @brief Keying the transmitter, as its own composable object.
 *
 * PTT used to be a serial file descriptor inside the ALSA backend. It is split
 * out because the keying method and the audio device are independent in
 * reality -- an operator can pair any sound interface with any keying method --
 * and because two backends would otherwise each need their own copy
 * ([analysis/15](../analysis/15-platform-audio-and-ptt.md) §6).
 *
 * `set_ptt` stays in ::ardop_platform_ops, so the loop and observer paths do
 * not change; a backend is handed one of these and delegates the line toggle to
 * it, keeping the audio-drain ordering (which only the backend knows) where it
 * belongs.
 *
 * ## Specification grammar
 *
 * | Spec | Meaning |
 * |---|---|
 * | `none` | VOX, or no keying at all |
 * | `rts:DEVICE` | Assert RTS on a serial port |
 * | `dtr:DEVICE` | Assert DTR on a serial port |
 * | `civ:DEVICE[@ADDR]` | Icom CI-V, and every Xiegu. Address in hex; default 00 |
 * | `kenwood:DEVICE` | `TX;` / `RX;` |
 * | `yaesu:DEVICE` | `TX1;` / `TX0;` |
 * | `cm108[:PATH\|VID:PID\|auto][+N]` | C-Media GPIO, pin N (default 3) |
 * | `rigctld:HOST:PORT` | `T 1` / `T 0` to a running rigctld |
 * | `gpio:N` | Recognised and refused; not implemented yet |
 *
 * For a rigctld host that is an IPv6 literal, bracket it: `rigctld:[::1]:4532`.
 *
 * **The right method is a property of the radio, and getting it wrong is
 * silent.** A Xiegu or an Icom keys by CAT command and ignores RTS entirely; a
 * DigiRig Mobile keys by RTS; a DigiRig Lite keys by CM108 GPIO. All three look
 * identically connected and only one of them transmits.
 *
 * A bare device path is accepted as `rts:` for compatibility with the old
 * `--ptt /dev/ttyUSB0`. On Windows a port above COM9 **must** be written
 * `\\.\COM10` -- the bare form does not open -- and ::ardop_ptt_parse applies
 * that prefix itself so the operator does not have to know.
 */

/** @brief How the transmitter is keyed. */
typedef enum {
	ARDOP_PTT_NONE = 0,   /**< VOX, or nothing. Always available. */
	ARDOP_PTT_SERIAL_RTS,
	ARDOP_PTT_SERIAL_DTR,
	ARDOP_PTT_RIGCTLD,
	ARDOP_PTT_CAT,        /**< The radio's own command, over its serial port. */
	ARDOP_PTT_CM108,      /**< C-Media GPIO over raw HID. */
	ARDOP_PTT_GPIO        /**< Declared; not implemented (needs libgpiod). */
} ardop_ptt_method;

#define ARDOP_PTT_TARGET_MAX 160

/** @brief A parsed specification, before anything is opened. */
typedef struct {
	ardop_ptt_method method;
	char target[ARDOP_PTT_TARGET_MAX];   /**< Device path, hostname or HID path. */
	uint16_t port;                       /**< rigctld only; 4532 by default. */
	int cat_family;                      /**< ::ardop_cat_family, for CAT. */
	uint8_t civ_addr;                    /**< CI-V transceiver address. */
	unsigned baud;                       /**< CAT only; 19200 by default. */
	unsigned gpio;                       /**< CM108 pin, 1..8; 3 by default. */
	uint16_t hid_vid, hid_pid;           /**< CM108 `VID:PID` form; 0 for any. */
} ardop_ptt_config;

typedef struct ardop_ptt ardop_ptt;

/**
 * @brief Parse a specification string.
 * @return false if the syntax is wrong or the method is not implemented; a
 *         diagnostic naming the problem is printed to stderr.
 */
bool ardop_ptt_parse(const char *spec, ardop_ptt_config *out);

/**
 * @brief Open the keying device and leave it unkeyed.
 * @return NULL on failure. ::ARDOP_PTT_NONE always succeeds.
 */
ardop_ptt *ardop_ptt_open(const ardop_ptt_config *cfg);

/**
 * @brief Key or unkey.
 *
 * Returns `void` to match ::ardop_platform_ops. A failure that only shows up
 * asynchronously -- the rigctld socket dropping -- is latched and reported
 * through ::ardop_ptt_fault instead.
 */
void ardop_ptt_set(ardop_ptt *p, bool key);

/** @brief True if currently keyed (as far as this object knows). */
bool ardop_ptt_keyed(const ardop_ptt *p);

/** @brief The latched fault, or ::ARDOP_FAULT_NONE. */
ardop_fault ardop_ptt_fault(const ardop_ptt *p);

/**
 * @brief Unkey and release.
 *
 * Always unkeys first. A serial line falls when the handle closes, but a rig
 * keyed over a TCP CAT link does not -- it never learns the controller is
 * gone -- so closing without unkeying leaves a transmitter on the air.
 */
void ardop_ptt_close(ardop_ptt *p);

/** @brief A short description for logging, e.g. "rts:/dev/ttyUSB0". */
const char *ardop_ptt_describe(const ardop_ptt *p);

#endif /* ARDOP_SHELL_PTT_H_ */
