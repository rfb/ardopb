#ifndef ARDOP_SHELL_AUDIO_DEVICES_H_
#define ARDOP_SHELL_AUDIO_DEVICES_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @file audio_devices.h
 * @brief Listing the sound devices an operator can choose between.
 *
 * House style: caller-owned storage, no allocation, no globals.
 *
 * ## Identity across a replug, and why there are two fields
 *
 * ALSA's `plughw:0,0` is a card *index*, and indices renumber when USB devices
 * come up in a different order -- the operator's radio interface silently
 * becomes their webcam, and the first they know of it is a transmission into
 * the wrong device. Windows endpoint IDs and macOS UIDs are stable; Android
 * device IDs are not guaranteed across reboots.
 *
 * So a saved selection persists **both** fields and resolves in this order:
 * exact ::ardop_audio_device::id, then exact ::ardop_audio_device::name, then
 * the system default *while telling the operator the selection changed*.
 * Silently transmitting through the wrong device is worse than a dialogue.
 */

/** @brief Enough for ALSA's `char[256]` device name plus its terminator. */
#define ARDOP_DEV_ID_MAX 264
#define ARDOP_DEV_NAME_MAX 256

/** @brief Which direction a device is being listed for. */
typedef enum {
	ARDOP_AUDIO_CAPTURE,
	ARDOP_AUDIO_PLAYBACK
} ardop_audio_dir;

/** @brief One device, as offered to the operator. */
typedef struct {
	char id[ARDOP_DEV_ID_MAX];      /**< Opaque, stable, persistable. */
	char name[ARDOP_DEV_NAME_MAX];  /**< Human-readable, UTF-8. */
	bool is_default;
} ardop_audio_device;

/**
 * @brief List up to @p max devices for @p dir into @p out.
 *
 * @param use_null_backend Enumerate miniaudio's synthetic null device instead
 *                         of real hardware. For tests and CI.
 * @return Devices written (0..@p max). 0 also means "could not enumerate".
 */
size_t ardop_audio_enumerate(ardop_audio_dir dir, ardop_audio_device *out,
			     size_t max, bool use_null_backend);

/**
 * @brief Print the device list to stderr, as `--list-devices` does.
 * @return true if enumeration worked.
 */
bool ardop_audio_print_devices(bool use_null_backend);

#endif /* ARDOP_SHELL_AUDIO_DEVICES_H_ */
