#include "link/session.h"

#include <stdio.h>
#include <string.h>

#include "codec/crc.h"
#include "codec/stationid.h"

/**
 * @file session.c
 * @brief ARQ session ID, ported from GenerateSessionID (ARQ.c).
 */

uint8_t ardop_session_id(const char *calling, const char *target)
{
	/* The original concatenates into a fixed STATIONID_BUF_SIZE*2 + 1 buffer
	 * with snprintf, which truncates rather than overflows; mirror that so an
	 * over-long input yields the same (truncated) CRC input it does today. */
	char buf[ARDOP_STATIONID_BUF_SIZE * 2 + 1];
	snprintf(buf, sizeof(buf), "%s%s", calling, target);

	uint8_t id = ardop_crc8((const uint8_t *)buf, strlen(buf));

	/* 0xFF is reserved to mark FEC frames, which carry no session. */
	if (id == 0xFFu)
		return 0;
	return id;
}

bool ardop_call_to_me(const ardop_stationid *caller,
		      const ardop_stationid *target,
		      const ardop_stationid *mycall,
		      const ardop_stationid *auxcalls, size_t n_aux,
		      uint8_t *session_id)
{
	if (ardop_stationid_eq(target, mycall)) {
		*session_id = ardop_session_id(caller->str, mycall->str);
		return true;
	}

	for (size_t i = 0; i < n_aux; i++) {
		if (ardop_stationid_eq(target, &auxcalls[i])) {
			*session_id = ardop_session_id(caller->str,
						       auxcalls[i].str);
			return true;
		}
	}

	return false;
}

bool ardop_ping_to_me(const ardop_stationid *caller,
		      const ardop_stationid *target,
		      const ardop_stationid *mycall,
		      const ardop_stationid *auxcalls, size_t n_aux)
{
	uint8_t ignored = 0;
	return ardop_call_to_me(caller, target, mycall, auxcalls, n_aux,
				&ignored);
}
