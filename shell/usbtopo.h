#ifndef ARDOP_SHELL_USBTOPO_H_
#define ARDOP_SHELL_USBTOPO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/audio_devices.h"
#include "shell/ptt.h"

/**
 * @file usbtopo.h
 * @brief Working out which keying interface belongs to which sound card.
 *
 * The step operators actually get stuck on is not choosing a sound card. It is
 * working out which of `ttyUSB0`, `ttyUSB1` and `ttyUSB2` is the radio, and the
 * usual method is to key each one and listen.
 *
 * In the common case the answer is knowable, because the radio is **one USB
 * cable carrying both audio and keying**. So given a sound card, this walks up
 * to the USB device it belongs to and looks for a serial port or a HID interface
 * on the same piece of plastic.
 *
 * ## Two topologies, because the two commonest interfaces differ
 *
 * - **A composite device** -- an IC-7300, an FT-991A, a Xiegu X6200: the audio
 *   and the CDC serial are two *interfaces* of one USB device, so their common
 *   ancestor is the device node itself.
 * - **An internal hub** -- a DigiRig Mobile is a hub containing two separate
 *   devices, a C-Media sound card and a CP2102 serial bridge. Their common
 *   ancestor is the hub, one level further up.
 *
 * So the rule is "nearest common USB ancestor", and a same-device-only match
 * would find nothing on a DigiRig -- the interface this most needs to serve.
 * Candidates are ranked ::ARDOP_USB_SAME_DEVICE before ::ARDOP_USB_SAME_HUB
 * because the first is certain and the second is an inference.
 *
 * ## Nothing here is applied on its own
 *
 * A detected pairing is a *suggestion*. The application shows it, fills in the
 * pickers, and waits for the operator to confirm with a PTT test. A wrong guess
 * that keys a transmitter is worse than no guess, so this never chooses.
 */

/** @brief How confidently a keying interface is paired with a sound card. */
typedef enum {
	ARDOP_USB_SAME_DEVICE = 0, /**< Two interfaces of one USB device. */
	ARDOP_USB_SAME_HUB,        /**< Two devices behind one hub. */
	ARDOP_USB_NONE,            /**< Nothing found on the same hardware. */
} ardop_usb_link;

/** @brief A radio the application can offer as one entry. */
typedef struct {
	char audio_id[ARDOP_DEV_ID_MAX];    /**< The sound card this belongs to. */
	char audio_name[ARDOP_DEV_NAME_MAX];
	char ptt_spec[ARDOP_PTT_TARGET_MAX + 24]; /**< Ready to hand to the parser. */
	ardop_usb_link link;
	uint16_t vid, pid;                  /**< Of the keying interface. */
	char model[64];                     /**< From the radio table, or "". */
	bool ptt_is_guess;                  /**< The method came from the table. */
} ardop_radio_candidate;

/**
 * @brief One node of a USB device tree, as this module sees it.
 *
 * Exposed so the walk can be tested against fabricated trees. The machine this
 * was written on has no USB audio at all -- every sound card is PCI -- so the
 * traversal is verified against fixtures and the real reader is thin.
 */
typedef struct {
	char syspath[256];   /**< The USB device node, e.g. "3-2.4". */
	char devnode[128];   /**< "/dev/ttyUSB0", "/dev/hidraw3", or "". */
	uint16_t vid, pid;
	bool is_audio;
	bool is_serial;
	bool is_hid;
} ardop_usb_node;

/**
 * @brief The nearest common ancestor of two USB device paths.
 *
 * A path is the kernel's `bus-port[.port...]` form. Two interfaces of one device
 * share the whole path; two devices behind a hub share everything but the last
 * component. Pure.
 */
ardop_usb_link ardop_usb_relate(const char *a, const char *b);

/**
 * @brief Pair audio nodes with keying nodes. Pure; the whole policy.
 *
 * @return Candidates written (0..@p max), best-ranked first.
 */
size_t ardop_usb_pair(const ardop_usb_node *nodes, size_t n,
		      ardop_radio_candidate *out, size_t max);

/**
 * @brief Read the real device tree. **Not the modem thread** -- it walks sysfs.
 * @param root Filesystem prefix, or NULL for the live one. Tests pass a fixture.
 * @return Nodes written.
 */
size_t ardop_usb_scan(const char *root, ardop_usb_node *out, size_t max);

/**
 * @brief Enumerate audio devices, walk the tree, and pair them.
 *
 * The one call an application makes. **Not the modem thread.**
 */
size_t ardop_radio_detect(ardop_radio_candidate *out, size_t max,
			  const char *backend_name);

#endif /* ARDOP_SHELL_USBTOPO_H_ */
