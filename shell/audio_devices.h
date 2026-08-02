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
 *
 * That rule is ::ardop_audio_match_device, and it is a pure function over a list
 * so that a settings screen, the backend and a unit test on a machine with no
 * sound card all get the same answer. It used to be a static function inside
 * `backend_ma.c` that printed a warning to stderr, which meant neither a user
 * interface nor a test could reach it and an operator with no terminal never
 * learned their selection had moved.
 *
 * ## Threading -- *guidance*, nothing enforces it
 *
 * ::ardop_audio_enumerate may be called from any thread **except the modem
 * thread**. It opens and closes its own device context and queries each device's
 * native formats, which blocks for as long as card probing takes; on the modem
 * thread that stall would trip the backend's 250 ms capture watchdog and
 * manufacture a device fault out of a device listing.
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

	/**
	 * @brief A native rate this device reports, or 0 when it will not say.
	 *
	 * Preferring one that ::rate_ok accepts, so a settings screen can show
	 * the rate that would actually be used.
	 *
	 * analysis/15 §4 specified a `native_12k` boolean here. It was a flag
	 * about the wrong thing: 24000, 48000 and 96000 are all equally usable
	 * and it would have read `false` for every one of them. Worse, it would
	 * be wrong on the two backends that matter most -- PulseAudio and
	 * shared-mode WASAPI report the mix format rather than what the hardware
	 * can do.
	 */
	unsigned native_rate;

	/**
	 * @brief Some native format is a whole multiple of 12000 Hz.
	 *
	 * False with a non-zero ::native_rate means "44100 Hz, and this device
	 * cannot be used". False with a zero ::native_rate means "not known
	 * until it is opened". Three honest answers where one boolean had two
	 * dishonest ones.
	 */
	bool rate_ok;
} ardop_audio_device;

/** @brief How a saved selection resolved against what is present. */
typedef enum {
	ARDOP_AUDIO_MATCH_ID = 0,  /**< Exact id: this is the operator's device. */
	ARDOP_AUDIO_MATCH_NAME,    /**< Exact name: renumbered, but recognisable. */
	ARDOP_AUDIO_MATCH_DEFAULT, /**< Nothing matched; the system default. */
	ARDOP_AUDIO_MATCH_NONE,    /**< Nothing matched and there is no default. */
} ardop_audio_match;

/**
 * @brief List up to @p max devices for @p dir into @p out.
 *
 * **Not the modem thread** -- see the file comment.
 *
 * @param backend_name Force one miniaudio backend ("alsa", "pulseaudio",
 *                     "null", ...), or NULL to auto-select.
 * @return Devices written (0..@p max). 0 also means "could not enumerate".
 */
size_t ardop_audio_enumerate(ardop_audio_dir dir, ardop_audio_device *out,
			     size_t max, const char *backend_name);

/**
 * @brief Apply the id-then-name-then-default rule to an enumerated list.
 *
 * Pure: no context, no device, no I/O, no globals, and it prints nothing. That
 * is the point -- the outcome is *returned* so a caller can put it in front of
 * an operator who has no terminal, and so it can be tested on a machine with no
 * sound card.
 *
 * An empty @p want_id and @p want_name is not a failure: it means "no saved
 * selection" and resolves to ::ARDOP_AUDIO_MATCH_DEFAULT.
 *
 * @param index Receives the index into @p devs, untouched on
 *              ::ARDOP_AUDIO_MATCH_NONE. When two devices share a name the
 *              first wins -- arbitrary, and documented rather than hidden.
 */
ardop_audio_match ardop_audio_match_device(const ardop_audio_device *devs,
					   size_t n, const char *want_id,
					   const char *want_name, size_t *index);

/** @brief A sentence for @p m, for a status line. Never NULL. */
const char *ardop_audio_match_str(ardop_audio_match m);

/** @brief True if @p m means the operator did not get what they asked for. */
bool ardop_audio_match_is_substitution(ardop_audio_match m);

/**
 * @brief Enumerate and resolve in one call. **Not the modem thread.**
 *
 * For previewing a saved selection without opening anything. The backend does
 * not use this -- it needs the platform's own device handle rather than the
 * rendered string, so it matches against its own enumeration.
 */
ardop_audio_match ardop_audio_resolve(ardop_audio_dir dir, const char *want_id,
				      const char *want_name,
				      const char *backend_name,
				      ardop_audio_device *out);

/**
 * @brief Print the device list to stdout, as `--list-devices` does.
 *
 * stdout, not stderr: this is the answer to a question the operator asked, not
 * a diagnostic, and `--list-devices | grep USB` should work. Enumeration
 * failures still go to stderr.
 *
 * @return false if neither direction produced a device.
 */
bool ardop_audio_print_devices(const char *backend_name);

#endif /* ARDOP_SHELL_AUDIO_DEVICES_H_ */
