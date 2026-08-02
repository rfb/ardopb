#ifndef ARDOP_SHELL_SERIALPORTS_H_
#define ARDOP_SHELL_SERIALPORTS_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @file serialports.h
 * @brief Listing the serial ports an operator can key through.
 *
 * The audio devices have been selectable from a list since the platform layer
 * landed; the keying port was a text field an operator had to type `COM3` into
 * blind. That is the same problem the device pickers exist to remove -- worse,
 * in fact, because a wrong sound card is audible and a wrong serial port simply
 * does nothing.
 *
 * House style: caller-owned storage, no allocation, no globals.
 *
 * ## Two fields, and the reason is the same as for audio
 *
 * `path` is what a PTT specification takes. `name` is what an operator
 * recognises -- "Silicon Labs CP210x (COM3)" rather than `COM3`, or the USB
 * interface's own description on Linux. A list of bare device nodes is only
 * marginally better than a text field, because the question an operator has is
 * not "which nodes exist" but "which one is the radio".
 *
 * ## Threading -- *guidance*
 *
 * Like ::ardop_audio_enumerate, this walks a device tree and may block. Do not
 * call it on the modem thread.
 */

#define ARDOP_PORT_PATH_MAX 160
#define ARDOP_PORT_NAME_MAX 192

/** @brief One serial port, as offered to the operator. */
typedef struct {
	char path[ARDOP_PORT_PATH_MAX];  /**< What a `rts:` or `civ:` spec takes. */
	char name[ARDOP_PORT_NAME_MAX];  /**< Human-readable, with the path in it. */
} ardop_serial_port;

/**
 * @brief List up to @p max serial ports.
 *
 * Real ports only: an entry appears because the operating system says a driver
 * is bound to it, not because a device node might exist. On Linux that is what
 * keeps `ttyS0` through `ttyS31` -- which exist on every machine and answer to
 * nothing -- out of a list whose whole purpose is to be short enough to read.
 *
 * @return Ports written (0..@p max).
 */
size_t ardop_serial_ports(ardop_serial_port *out, size_t max);

#endif /* ARDOP_SHELL_SERIALPORTS_H_ */
