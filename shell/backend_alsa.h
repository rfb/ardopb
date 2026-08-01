#ifndef ARDOP_SHELL_BACKEND_ALSA_H_
#define ARDOP_SHELL_BACKEND_ALSA_H_

#include <stdbool.h>

#include "shell/platform.h"
#include "shell/ptt.h"

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
 * juggling in the protocol path. Keying is delegated to shell/ptt.h, so serial
 * RTS/DTR and rigctld all work here; the miniaudio backend (shell/backend_ma.h)
 * covers Windows, macOS and Android.
 *
 * @par Running under WSL (WSLg)
 * There is no separate PulseAudio backend: on WSL the Windows audio devices are
 * reached through the ALSA -> PulseAudio plugin. Install `libasound2-plugins`
 * and point ALSA's `default` device at pulse in `~/.asoundrc`
 * (`pcm.!default { type pulse } ctl.!default { type pulse }`); then run with
 * `--alsa default default`. The same works against a native PipeWire/PulseAudio
 * server, and against a virtual loopback (a null sink and its monitor) for
 * two-station interop tests without hardware.
 */
typedef struct ardop_alsa_backend ardop_alsa_backend;

/**
 * @brief Open the devices and fill @p ops to drive them.
 *
 * @param capture_dev   ALSA capture device (e.g. "plughw:0,0" or "default").
 * @param playback_dev  ALSA playback device.
 * @param ptt           Keying object from shell/ptt.h; NULL for VOX/none.
 * @param ops           Platform ops table to populate on success.
 * @return An opaque backend handle, or NULL on failure (a message is logged).
 *         Free it with ardop_backend_alsa_close() after the loop ends.
 */
ardop_alsa_backend *ardop_backend_alsa_open(const char *capture_dev,
					    const char *playback_dev,
					    ardop_ptt *ptt,
					    ardop_platform_ops *ops);

/** @brief Close the devices and free the backend. */
void ardop_backend_alsa_close(ardop_alsa_backend *ab);

#endif /* ARDOP_SHELL_BACKEND_ALSA_H_ */
