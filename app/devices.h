#ifndef ARDOP_APP_DEVICES_H_
#define ARDOP_APP_DEVICES_H_

#include <stdbool.h>
#include <stddef.h>

#include "app/spine.h"
#include "shell/audio_devices.h"
#include "shell/ptt.h"
#include "shell/settings.h"

/**
 * @file devices.h
 * @brief Who owns the sound card and the keying line, and when they are rebuilt.
 *
 * The spine owns the modem and refuses to know what a device is
 * (`app/README.md` rule 2, enforced by the build). This object owns both
 * devices, the selection an operator made, and the state machine that gets from
 * one to the other -- and it is the only file in the tree that includes both
 * `app/spine.h` and `shell/backend_ma.h`.
 *
 * ## The thread split, which is the whole design
 *
 * A settings screen runs on the interface thread and must not touch
 * ::ardop_runtime, the audio backend or the PTT object. So it *requests* a
 * selection and ::app_devices_service applies it on the modem thread between
 * ::app_step calls. The request crosses on an `app_ring` -- the same
 * single-producer/single-consumer record ring the spine's three queues are
 * built from, so the ordering guarantee is the one `make test-ring-tsan`
 * already proves.
 *
 * **The cross-thread surface is exactly one ring and two atomics.** A third
 * shared field needs its own argument. *Guidance*: nothing enforces it.
 *
 * ## Why a peer object rather than a fourth ops table inside the spine
 *
 * Unlike a transport, a device has a lifecycle the spine participates in --
 * ::app_set_platform, ::app_report_fault, the transmit credit. Putting the
 * policy (when to rebuild, what a fault means, whether a substitution happened)
 * behind function pointers would bury it in the one file that must stay free of
 * devices. ::app_set_platform is already the entire seam the spine needs; the
 * state machine belongs above it.
 */

typedef struct app_devices app_devices;

/**
 * @brief A complete device choice.
 *
 * Plain data: comparable, copyable, and the unit of persistence.
 */
typedef struct {
	char capture_id[ARDOP_DEV_ID_MAX];
	char capture_name[ARDOP_DEV_NAME_MAX];
	char playback_id[ARDOP_DEV_ID_MAX];
	char playback_name[ARDOP_DEV_NAME_MAX];
	char backend[32];                       /**< "", "alsa", "wasapi", ... */
	char ptt_spec[ARDOP_PTT_TARGET_MAX + 24]; /**< "rts:/dev/ttyUSB0", "none" */
	bool null_device;                       /**< The synthetic device, for tests. */
} app_device_selection;

/** @brief Where the devices are. */
typedef enum {
	APP_DEV_CLOSED = 0, /**< Nothing bound; the spine has no platform. */
	APP_DEV_RUNNING,    /**< Open and healthy. */
	APP_DEV_FAULTED,    /**< A fault was latched and reported. Awaiting a decision. */
	APP_DEV_FAILED,     /**< The last open attempt failed. Awaiting a decision. */
} app_dev_state;

/** @brief What is bound right now, for display. */
typedef struct {
	app_dev_state state;
	app_device_selection requested;   /**< What was asked for. */
	app_device_selection in_use;      /**< What was actually opened. */
	ardop_audio_match capture_match;  /**< How the capture selection resolved. */
	ardop_audio_match playback_match;
	unsigned device_rate;             /**< 0 when not running. */
	unsigned ratio;                   /**< device_rate / 12000. */
	size_t block;                     /**< Modem samples per loop iteration. */
	char ptt_describe[64];
	char detail[APP_TEXT_MAX];        /**< The last failure or substitution. */
} app_device_status;

/* --- any thread ------------------------------------------------------------ */

/** @brief Allocate. @return NULL on failure. */
app_devices *app_devices_new(void);

/** @brief Free. Detach from the spine with ::app_devices_close first. */
void app_devices_free(app_devices *dv);

/**
 * @brief Ask for @p sel. Nothing is opened here.
 *
 * The request is queued; ::app_devices_service applies it on the modem thread.
 * A second request before the first is applied supersedes it -- service drains
 * the whole ring and acts on the last, because an operator clicking down a
 * device list must not open five sound cards in a row.
 *
 * This is also how to retry after a fault: request the same selection again.
 * There is deliberately no automatic retry (analysis/15 §4: "It does not
 * silently reopen the default device") -- a device that vanished because a cable
 * fell out will fail the retry, and one that vanished because the operator
 * changed profiles deserves a decision rather than a surprise.
 *
 * @return false only if the request ring is full, which needs several requests
 *         inside one audio block.
 */
bool app_devices_request(app_devices *dv, const app_device_selection *sel);

/** @brief Ask for everything to be closed and the spine detached. */
bool app_devices_request_close(app_devices *dv);

/**
 * @brief Key for @p ms milliseconds, then unkey. The "Test PTT" button.
 *
 * Applied on the modem thread and refused there unless the devices are running
 * and the link is disconnected: keying under a live ARQ session would transmit
 * noise over somebody's frame. The outcome arrives as ::APP_EV_DEVICE with
 * ::APP_DEV_EV_PTT_TEST.
 *
 * This exists because CM108 and CI-V keying have no feedback path -- a write
 * that succeeds proves the command reached the interface, not that the radio
 * keyed. A human listening is the only available confirmation.
 */
bool app_devices_request_ptt_test(app_devices *dv, unsigned ms);

/** @brief Current state. Cheap and lock-free, for a status light. */
app_dev_state app_devices_state(const app_devices *dv);

/**
 * @brief Fill @p out.
 *
 * Like ::app_snapshot, each field is an independent load: for display, never for
 * a decision.
 */
void app_devices_status(app_devices *dv, app_device_status *out);

/* --- modem thread only ----------------------------------------------------- */

/**
 * @brief Apply pending requests, poll both fault latches, recover.
 *
 * Call once per iteration, **before** ::app_step.
 *
 * Blocks. An open is a device open -- hundreds of milliseconds on a cold WASAPI
 * endpoint, and a rebuild is a close and an open. Nothing is starved by that,
 * because during a rebuild there is no capture stream to starve.
 *
 * It is also the pacer while nothing is bound: with no platform, ::app_step
 * returns instantly and a caller's loop would spin at 100% CPU, so this sleeps
 * briefly in every state except ::APP_DEV_RUNNING.
 */
void app_devices_service(app_devices *dv, app_spine *sp);

/**
 * @brief Close everything, in the order that cannot leave a keyed transmitter.
 *
 * Detach the spine, close the audio backend, then the PTT -- unkeying goes out
 * through the bound platform, so the spine must be detached before the backend
 * dies, and PTT dies last because unkeying has to outlive the device that
 * keyed. Doing it here means nobody has to remember the order.
 */
void app_devices_close(app_devices *dv, app_spine *sp);

/* --- persistence ----------------------------------------------------------- */

/** @brief Read `audio.*` and `ptt.*` out of @p s. Missing keys leave defaults. */
void app_devices_selection_load(app_device_selection *sel,
				const ardop_settings *s);

/** @brief Write `audio.*` and `ptt.*` into @p s. @return false if @p s is full. */
bool app_devices_selection_store(const app_device_selection *sel,
				 ardop_settings *s);

#endif /* ARDOP_APP_DEVICES_H_ */
