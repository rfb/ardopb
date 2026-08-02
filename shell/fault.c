#include "shell/fault.h"

/**
 * @file fault.c
 * @brief Fault names (see fault.h).
 *
 * A five-line table in its own translation unit, because it lived in `ptt.c`
 * and that made every consumer of a fault *name* link the whole PTT object --
 * serial ports, rigctld sockets and all. The device manager wants to render a
 * fault without owning a keying line, and a test wants to assert on one without
 * linking `MA_OBJS`.
 */
const char *ardop_fault_str(ardop_fault f)
{
	switch (f) {
	case ARDOP_FAULT_NONE:              return "none";
	case ARDOP_FAULT_CAPTURE_LOST:      return "capture device lost";
	case ARDOP_FAULT_PLAYBACK_LOST:     return "playback device lost";
	case ARDOP_FAULT_PLAYBACK_UNDERRUN: return "playback underrun";
	case ARDOP_FAULT_PTT_LOST:          return "PTT control lost";
	}
	return "unknown";
}
