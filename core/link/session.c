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
