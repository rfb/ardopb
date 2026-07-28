#ifndef ARDOP_LINK_SESSION_H_
#define ARDOP_LINK_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"
#include "codec/stationid.h"

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

/**
 * @brief Is a frame addressed to us, and if so, under what session ID?
 *
 * Ported from `IsCallToMe`. A frame is for us if its target matches our own
 * callsign or any of our auxiliary callsigns. On a match, the session ID for
 * the reply is derived from the caller and the *matched* local callsign
 * (ardop_session_id()), so aux-call sessions get their own ID. Unlike the
 * original, the local callsigns are passed in rather than read from globals.
 *
 * @param[in]  caller      The calling station.
 * @param[in]  target      The station the frame is addressed to.
 * @param[in]  mycall      Our primary callsign.
 * @param[in]  auxcalls    Our auxiliary callsigns (may be NULL if @p n_aux 0).
 * @param[in]  n_aux       Number of auxiliary callsigns.
 * @param[out] session_id  Reply session ID, written only on a match.
 * @return true if the frame is addressed to us.
 */
ARDOP_MUSTUSE bool ardop_call_to_me(const ardop_stationid *caller,
				    const ardop_stationid *target,
				    const ardop_stationid *mycall,
				    const ardop_stationid *auxcalls,
				    size_t n_aux, uint8_t *session_id);

/**
 * @brief Is a Ping addressed to us? (ardop_call_to_me without needing the ID.)
 *
 * Ported from `IsPingToMe`.
 */
ARDOP_MUSTUSE bool ardop_ping_to_me(const ardop_stationid *caller,
				    const ardop_stationid *target,
				    const ardop_stationid *mycall,
				    const ardop_stationid *auxcalls,
				    size_t n_aux);

#endif /* ARDOP_LINK_SESSION_H_ */
