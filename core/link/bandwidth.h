#ifndef ARDOP_LINK_BANDWIDTH_H_
#define ARDOP_LINK_BANDWIDTH_H_

#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file bandwidth.h
 * @brief ARQ session bandwidth negotiation.
 *
 * A connect request encodes both a bandwidth and whether that bandwidth is a
 * *maximum* the caller will accept or a *forced* value it insists on. The
 * receiving station holds its own bandwidth setting (also max-or-forced), and
 * the two are reconciled into a single session bandwidth, or the connection is
 * rejected. This is the pure decision; the FSM that acts on it is separate.
 * See [analysis/02](../../analysis/02-protocol-fsm.md).
 */

/**
 * @brief A station's ARQ bandwidth setting: a width plus a max/forced policy.
 *
 * The enumerator order mirrors the inherited `enum _ARQBandwidth` exactly
 * (forced 200/500/1000/2000, then max 200/500/1000/2000, then undefined), so it
 * maps one-to-one to the original for equivalence testing.
 */
typedef enum {
	ARDOP_ARQ_BW_200_FORCED = 0,
	ARDOP_ARQ_BW_500_FORCED,
	ARDOP_ARQ_BW_1000_FORCED,
	ARDOP_ARQ_BW_2000_FORCED,
	ARDOP_ARQ_BW_200_MAX,
	ARDOP_ARQ_BW_500_MAX,
	ARDOP_ARQ_BW_1000_MAX,
	ARDOP_ARQ_BW_2000_MAX,
	ARDOP_ARQ_BW_UNDEFINED,
} ardop_arq_bandwidth;

/**
 * @brief Negotiate the session bandwidth from a received ConReq.
 *
 * Ported from `IRSNegotiateBW`. Given this station's bandwidth setting @p local
 * and the connect-request frame type @p con_req_type, returns the ConAck frame
 * type that accepts the connection at the agreed width, or ARDOP_FT_CON_REJ_BW
 * if the request cannot be satisfied. On acceptance, @p session_hz is set to the
 * negotiated width in Hz (200, 500, 1000 or 2000); on rejection it is left
 * unchanged.
 *
 * @param[in]  local         This station's bandwidth setting.
 * @param[in]  con_req_type  The received ConReq frame type (ARDOP_FT_CON_REQ_*).
 * @param[out] session_hz    Negotiated width in Hz, written only on acceptance.
 * @return A ConAck frame type (ARDOP_FT_CON_ACK_*) on acceptance, or
 *         ARDOP_FT_CON_REJ_BW on rejection.
 */
ARDOP_MUSTUSE uint8_t ardop_negotiate_bandwidth(ardop_arq_bandwidth local,
						uint8_t con_req_type,
						int *session_hz);

#endif /* ARDOP_LINK_BANDWIDTH_H_ */
