#ifndef ARDOP_SHELL_PTT_CM108_H_
#define ARDOP_SHELL_PTT_CM108_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/ptt.h"

/**
 * @file ptt_cm108.h
 * @brief Keying through a C-Media USB audio chip's GPIO pin.
 *
 * This is how most cheap USB sound-card interfaces key: DigiRig Lite, the RA
 * boards, and the common CM108/CM119 dongles all bring a GPIO pin out to a
 * transistor. analysis/15 §6 calls it "arguably higher-value than CAT for the
 * target operator", and it was deferred in the miniaudio pass for wanting a HID
 * dependency and hardware to test against.
 *
 * The dependency half is gone: Linux writes an output report to `/dev/hidraw*`
 * and Windows uses `hid.dll`, which ships with the operating system. There is no
 * hidapi and no libusb anywhere in this tree.
 *
 * **The hardware half has not gone anywhere.** Nothing here has been run against
 * a real interface. So everything that *can* be decided without one -- the report
 * bytes, the chip table, the `auto` policy, the sysfs parser -- is a pure
 * function with a test, and the part that touches a device is as thin as it can
 * be made. A write that succeeds proves the report reached the chip, **not that
 * the radio keyed**: this hardware has no feedback path, which is why
 * `app_devices_request_ptt_test` exists and why a human with a receiver is the
 * only real confirmation.
 *
 * ## The report
 *
 * Five bytes: `{report id, reserved, GPIO levels, GPIO direction mask,
 * reserved}`. The fifth is a normative accident in this tree's sense
 * (analysis/12) -- direwolf's `cm108.c` writes it with the comment *"Writing 5
 * bytes works. I have no idea why. From the CMedia datasheet it looks like we
 * need 4."* Every CM108 interface in the field has been tested against that
 * implementation, and with no hardware here, agreeing with the datasheet over
 * the thing that demonstrably works would be a guess.
 *
 * ## The chip table is load-bearing
 *
 * Members of this family have **different GPIO counts**: CM108 has 4, CM108AH
 * and CM108B have 3, CM109 and CM119 have 8, and SSS1621/SSS1623 have only
 * **2**. A blanket default of GPIO3 does nothing at all on an SSS162x -- it sets
 * a bit the chip does not have -- and a vendor-only match would miss vendor
 * `0x0c76` entirely. So the table carries the count and ::ardop_cm108_report
 * refuses a pin the chip cannot drive, rather than writing into the void.
 */

/** @brief Bytes in a GPIO output report. */
#define ARDOP_CM108_REPORT_LEN 5

/** @brief The GPIO pin almost every interface wires PTT to. */
#define ARDOP_CM108_DEFAULT_GPIO 3

/**
 * @brief Build the GPIO output report.
 *
 * `{0x00, 0x00, levels, direction, 0x00}` where both masks carry
 * `1 << (gpio - 1)`. For the usual GPIO3 that is `{0, 0, 0x04, 0x04, 0}` to key
 * and `{0, 0, 0x00, 0x04, 0}` to unkey -- the pin stays an output either way.
 *
 * @param gpio 1..8, and it must exist on the chip; see the file comment.
 * @return false if @p gpio is out of range, leaving @p out untouched.
 */
bool ardop_cm108_report(unsigned gpio, bool key,
			uint8_t out[ARDOP_CM108_REPORT_LEN]);

/**
 * @brief How many GPIO pins the chip at @p vid / @p pid has, or 0 if unknown.
 *
 * From direwolf's table, which is field-verified against hardware this project
 * does not have.
 */
unsigned ardop_cm108_gpio_count(uint16_t vid, uint16_t pid);

/** @brief A name for the chip, or NULL when it is not one we know. */
const char *ardop_cm108_chip_name(uint16_t vid, uint16_t pid);

/** @brief Whether @p vid / @p pid is a chip in this family. */
bool ardop_cm108_is_known(uint16_t vid, uint16_t pid);

/** @brief One HID device a scan turned up. */
typedef struct {
	char path[ARDOP_PTT_TARGET_MAX];
	uint16_t vid, pid;
} ardop_cm108_candidate;

/**
 * @brief Decide which candidate `cm108:auto` should use. Pure; the whole policy.
 *
 * Requires **exactly one** match. Two identical dongles is precisely the case
 * where guessing would key the wrong radio, so it refuses and names both, and
 * the operator pastes one back as `cm108:/dev/hidrawN`.
 *
 * @param want_vid 0 to accept any chip ::ardop_cm108_is_known recognises.
 * @param why      Receives a sentence when the answer is not 1.
 * @return 1 chosen, 0 none matched, -1 ambiguous.
 */
int ardop_cm108_choose(const ardop_cm108_candidate *cands, size_t n,
		       uint16_t want_vid, uint16_t want_pid, size_t *index,
		       char *why, size_t why_cap);

/**
 * @brief Parse a sysfs `HID_ID=0003:00000D8C:0000000C` line.
 *
 * Linux-shaped, but a parser has no platform, so it is built and tested
 * everywhere. The leading field is the bus: `0003` is USB, and anything else
 * (`0005` is Bluetooth) is rejected rather than treated as a candidate.
 */
bool ardop_cm108_parse_hid_id(const char *line, uint16_t *vid, uint16_t *pid);

/* --- the device half (the untested part) ----------------------------------- */

/**
 * @brief Find C-Media HID devices.
 *
 * On Linux this reads sysfs rather than opening anything, deliberately: a device
 * the operator cannot yet write to is still *found*, so the diagnostic can be
 * "found your CM108 at /dev/hidraw3 but cannot open it" rather than the useless
 * "no CM108 found".
 *
 * @return Candidates written (0..@p max).
 */
size_t ardop_cm108_scan(ardop_cm108_candidate *out, size_t max);

typedef struct ardop_cm108 ardop_cm108;

/**
 * @brief Open @p path for writing reports.
 * @return NULL on failure, with a diagnostic naming the fix where one exists.
 */
ardop_cm108 *ardop_cm108_open(const char *path, unsigned gpio);

/** @brief Write the key/unkey report. @return false on a write failure. */
bool ardop_cm108_set(ardop_cm108 *c, bool key);

void ardop_cm108_close(ardop_cm108 *c);

#endif /* ARDOP_SHELL_PTT_CM108_H_ */
