#ifndef ARDOP_LINK_SESSION_H_
#define ARDOP_LINK_SESSION_H_

#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file session.h
 * @brief ARQ session identity.
 *
 * The first leaf of the link layer. A connected ARQ session is tagged with a
 * one-byte session ID derived from the two callsigns; every control frame is
 * XORed with it so a station can tell its own session's frames from a
 * co-channel session's (see [analysis/02](../../analysis/02-protocol-fsm.md)).
 * This is a pure function of the callsigns, so it ports as a leaf before the
 * stateful machine that uses it.
 */

/**
 * @brief Derive the one-byte ARQ session ID from the two callsigns.
 *
 * Ported from `GenerateSessionID`: the CRC-8 of the calling callsign
 * concatenated with the target callsign. A computed value of 0xFF is remapped
 * to 0, because 0xFF is reserved to mark FEC-mode frames (which carry no
 * session). The inputs are the canonical callsign strings
 * (`ardop_stationid.str`); over-long inputs are truncated exactly as the
 * original's fixed buffer does, so the result stays bit-identical.
 *
 * @param calling The calling station's callsign, NUL-terminated.
 * @param target  The target station's callsign, NUL-terminated.
 * @return The session ID byte, 0x00..0xFE (never 0xFF).
 */
ARDOP_MUSTUSE uint8_t ardop_session_id(const char *calling, const char *target);

#endif /* ARDOP_LINK_SESSION_H_ */
