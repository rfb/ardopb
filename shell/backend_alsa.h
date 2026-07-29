#ifndef ARDOP_SHELL_BACKEND_ALSA_H_
#define ARDOP_SHELL_BACKEND_ALSA_H_

#include <stdbool.h>

#include "shell/platform.h"

/**
 * @file backend_alsa.h
 * @brief The ALSA platform backend: real capture/playback + optional serial PTT.
 *
 * A thin wrapper: it opens a capture and a playback PCM at 12000 Hz mono
 * S16_LE, moves samples through ::ardop_platform_ops, and keys PTT by toggling
 * RTS on an optional serial line. It is the concrete backend the narrow
 * interface exists for -- all the device juggling that `ALSASound.c` spread
 * through the poll/flush spine lives here and nowhere else, and the loop above
 * it is device-agnostic.
 *
 * Deliberately narrow relative to the inherited backend: no `FixTiming`/`-A`
 * (the sample clock removed the need), no `SlowCPU`, no half-duplex handle
 * juggling in the protocol path. CAT/GPIO/rigctl PTT and WinMM are follow-ups;
 * this covers the common ALSA + serial-RTS case.
 */
typedef struct ardop_alsa_backend ardop_alsa_backend;

/**
 * @brief Open the devices and fill @p ops to drive them.
 *
 * @param capture_dev   ALSA capture device (e.g. "plughw:0,0" or "default").
 * @param playback_dev  ALSA playback device.
 * @param ptt_serial    Serial device to key PTT via RTS (e.g. "/dev/ttyUSB0"),
 *                      or NULL for no hardware PTT (VOX).
 * @param ops           Platform ops table to populate on success.
 * @return An opaque backend handle, or NULL on failure (a message is logged).
 *         Free it with ardop_backend_alsa_close() after the loop ends.
 */
ardop_alsa_backend *ardop_backend_alsa_open(const char *capture_dev,
					    const char *playback_dev,
					    const char *ptt_serial,
					    ardop_platform_ops *ops);

/** @brief Close the devices and free the backend. */
void ardop_backend_alsa_close(ardop_alsa_backend *ab);

#endif /* ARDOP_SHELL_BACKEND_ALSA_H_ */
