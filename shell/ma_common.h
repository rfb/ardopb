#ifndef ARDOP_SHELL_MA_COMMON_H_
#define ARDOP_SHELL_MA_COMMON_H_

/**
 * @file ma_common.h
 * @brief Includes miniaudio's *declarations* for the two files that need them.
 *
 * The implementation lives in `shell/ma_impl.c`, which is the only translation
 * unit compiled without the project's warning flags. `backend_ma.c` and
 * `audio_devices.c` are held to the full strict bar, so the vendored header --
 * which was not written to that bar and never will be -- is wrapped here rather
 * than at each include site.
 *
 * This header must not be included by anything outside those two files:
 * `backend_ma.h` deliberately exposes an opaque handle so that no other
 * translation unit ever names a miniaudio type.
 */

#include <stdbool.h>
#include <stddef.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/miniaudio.h"
#pragma GCC diagnostic pop

/** @brief Render a device ID as a stable, human-readable, persistable string. */
void ardop_ma_id_to_str(const ma_device_id *id, ma_backend backend, char *out,
			size_t cap);

/**
 * @brief Look up a backend by name, forgivingly.
 *
 * miniaudio's own ma_get_backend_from_name is an exact match against display
 * names -- "ALSA", "PulseAudio", "Core Audio". Those are labels, not things to
 * type, so this ignores case, spaces, dashes and underscores. On failure it
 * prints the backends actually compiled in on this platform.
 */
bool ardop_ma_backend_from_name(const char *name, ma_backend *out);

#endif /* ARDOP_SHELL_MA_COMMON_H_ */
