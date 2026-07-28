#include "link/bandwidth.h"

#include "codec/frame.h"

/**
 * @file bandwidth.c
 * @brief ARQ bandwidth negotiation, ported from IRSNegotiateBW (ARQ.c).
 *
 * The structure follows the original's per-setting switch exactly. A forced
 * setting accepts only requests that include its own width; a max setting
 * accepts the largest offered width that does not exceed its own. The
 * ConReq/ConAck type bytes run in width order, so the range comparisons below
 * mean the same thing they do in the original.
 */

uint8_t ardop_negotiate_bandwidth(ardop_arq_bandwidth local,
				  uint8_t con_req_type, int *session_hz)
{
	switch (local) {
	case ARDOP_ARQ_BW_200_FORCED:
		if ((con_req_type >= ARDOP_FT_CON_REQ_200M
		     && con_req_type <= ARDOP_FT_CON_REQ_2000M)
		    || con_req_type == ARDOP_FT_CON_REQ_200F) {
			*session_hz = 200;
			return ARDOP_FT_CON_ACK_200;
		}
		break;

	case ARDOP_ARQ_BW_500_FORCED:
		if ((con_req_type >= ARDOP_FT_CON_REQ_500M
		     && con_req_type <= ARDOP_FT_CON_REQ_2000M)
		    || con_req_type == ARDOP_FT_CON_REQ_500F) {
			*session_hz = 500;
			return ARDOP_FT_CON_ACK_500;
		}
		break;

	case ARDOP_ARQ_BW_1000_FORCED:
		if ((con_req_type >= ARDOP_FT_CON_REQ_1000M
		     && con_req_type <= ARDOP_FT_CON_REQ_2000M)
		    || con_req_type == ARDOP_FT_CON_REQ_1000F) {
			*session_hz = 1000;
			return ARDOP_FT_CON_ACK_1000;
		}
		break;

	case ARDOP_ARQ_BW_2000_FORCED:
		if (con_req_type == ARDOP_FT_CON_REQ_2000M
		    || con_req_type == ARDOP_FT_CON_REQ_2000F) {
			*session_hz = 2000;
			return ARDOP_FT_CON_ACK_2000;
		}
		break;

	case ARDOP_ARQ_BW_200_MAX:
		if (con_req_type >= ARDOP_FT_CON_REQ_200M
		    && con_req_type <= ARDOP_FT_CON_REQ_200F) {
			*session_hz = 200;
			return ARDOP_FT_CON_ACK_200;
		}
		break;

	case ARDOP_ARQ_BW_500_MAX:
		if (con_req_type == ARDOP_FT_CON_REQ_200M
		    || con_req_type == ARDOP_FT_CON_REQ_200F) {
			*session_hz = 200;
			return ARDOP_FT_CON_ACK_200;
		}
		if ((con_req_type >= ARDOP_FT_CON_REQ_500M
		     && con_req_type <= ARDOP_FT_CON_REQ_2000M)
		    || con_req_type == ARDOP_FT_CON_REQ_500F) {
			*session_hz = 500;
			return ARDOP_FT_CON_ACK_500;
		}
		break;

	case ARDOP_ARQ_BW_1000_MAX:
		if (con_req_type == ARDOP_FT_CON_REQ_200M
		    || con_req_type == ARDOP_FT_CON_REQ_200F) {
			*session_hz = 200;
			return ARDOP_FT_CON_ACK_200;
		}
		if (con_req_type == ARDOP_FT_CON_REQ_500M
		    || con_req_type == ARDOP_FT_CON_REQ_500F) {
			*session_hz = 500;
			return ARDOP_FT_CON_ACK_500;
		}
		if ((con_req_type >= ARDOP_FT_CON_REQ_1000M
		     && con_req_type <= ARDOP_FT_CON_REQ_2000M)
		    || con_req_type == ARDOP_FT_CON_REQ_1000F) {
			*session_hz = 1000;
			return ARDOP_FT_CON_ACK_1000;
		}
		break;

	case ARDOP_ARQ_BW_2000_MAX:
		if (con_req_type == ARDOP_FT_CON_REQ_200M
		    || con_req_type == ARDOP_FT_CON_REQ_200F) {
			*session_hz = 200;
			return ARDOP_FT_CON_ACK_200;
		}
		if (con_req_type == ARDOP_FT_CON_REQ_500M
		    || con_req_type == ARDOP_FT_CON_REQ_500F) {
			*session_hz = 500;
			return ARDOP_FT_CON_ACK_500;
		}
		if (con_req_type == ARDOP_FT_CON_REQ_1000M
		    || con_req_type == ARDOP_FT_CON_REQ_1000F) {
			*session_hz = 1000;
			return ARDOP_FT_CON_ACK_1000;
		}
		if (con_req_type == ARDOP_FT_CON_REQ_2000M
		    || con_req_type == ARDOP_FT_CON_REQ_2000F) {
			*session_hz = 2000;
			return ARDOP_FT_CON_ACK_2000;
		}
		break;

	case ARDOP_ARQ_BW_UNDEFINED:
		break;
	}

	return ARDOP_FT_CON_REJ_BW;
}
