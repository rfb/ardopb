#include "shell/build.h"
#include "shell/host.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/frame.h"
#include "codec/locator.h"
#include "codec/stationid.h"
#include "link/link.h"

/**
 * @file host.c
 * @brief The host command protocol (see host.h). Ported command-by-command from
 *        HostInterface.c's ProcessCommandFromHost, preserving reply strings.
 */

/* The ARQ bandwidth names, in ardop_arq_bandwidth order (index == enum value).
 * Ported verbatim from ARQ.c's ARQBandwidths so the wire strings match. */
static const char *const kBandwidths[] = {
	"200FORCED", "500FORCED", "1000FORCED", "2000FORCED",
	"200MAX",    "500MAX",    "1000MAX",    "2000MAX",    "UNDEFINED",
};
#define NUM_BANDWIDTHS ((int)(sizeof kBandwidths / sizeof kBandwidths[0]))

const char *ardop_host_bandwidth_name(ardop_arq_bandwidth bw)
{
	if ((int)bw < 0 || (int)bw >= NUM_BANDWIDTHS)
		return NULL;
	return kBandwidths[bw];
}

bool ardop_host_bandwidth_for_name(const char *name, ardop_arq_bandwidth *out)
{
	if (!name)
		return false;
	for (int i = 0; i < NUM_BANDWIDTHS; i++)
		if (strcmp(name, kBandwidths[i]) == 0) {
			if (out)
				*out = (ardop_arq_bandwidth)i;
			return true;
		}
	return false;
}

/* The protocol mode names. Three of them, so a switch rather than a table --
 * and the switch is what makes the compiler complain if a mode is ever added. */
const char *ardop_host_mode_name(ardop_link_mode mode)
{
	switch (mode) {
	case ARDOP_MODE_ARQ: return "ARQ";
	case ARDOP_MODE_FEC: return "FEC";
	case ARDOP_MODE_RXO: return "RXO";
	}
	return NULL;
}

bool ardop_host_mode_for_name(const char *name, ardop_link_mode *out)
{
	static const ardop_link_mode kAll[] = {ARDOP_MODE_ARQ, ARDOP_MODE_FEC,
					       ARDOP_MODE_RXO};
	if (!name)
		return false;
	for (size_t i = 0; i < sizeof kAll / sizeof kAll[0]; i++)
		if (strcmp(name, ardop_host_mode_name(kAll[i])) == 0) {
			if (out)
				*out = kAll[i];
			return true;
		}
	return false;
}

/* The host-facing protocol state names, from ARDOPC.c's ARDOPStates. */
const char *ardop_host_state_name(ardop_link_state state)
{
	switch (state) {
	case ARDOP_LINK_DISC:        return "DISC";
	case ARDOP_LINK_ISS_CON_REQ:
	case ARDOP_LINK_ISS_CON_ACK:
	case ARDOP_LINK_ISS:         return "ISS";
	case ARDOP_LINK_IRS_CON_ACK:
	case ARDOP_LINK_IRS_DATA:
	case ARDOP_LINK_IRS_FROM_ISS:return "IRS";
	case ARDOP_LINK_IDLE:        return "IDLE";
	case ARDOP_LINK_IRS_TO_ISS:  return "IRStoISS";
	case ARDOP_LINK_FEC_SEND:    return "FECSEND";
	}
	return "DISC";
}

static const char *host_state_name(const ardop_runtime *rt)
{
	return ardop_host_state_name(rt->link.state);
}

/* Whether the current state is a live connection DISCONNECT can act on
 * (IDLE/ISS/IRS/IRStoISS), matching the inherited DISCONNECT guard. */
static bool state_is_connected(const ardop_runtime *rt)
{
	const char *s = host_state_name(rt);
	return strcmp(s, "DISC") != 0 && strcmp(s, "FECSEND") != 0;
}

/* printf into the reply buffer. */
static void replyf(char *reply, size_t cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)vsnprintf(reply, cap, fmt, ap);
	va_end(ap);
}

/* A boolean TRUE/FALSE command: query echoes state, set validates and applies.
 * Mirrors DoTrueFalseCmd, including its reply strings. */
static void do_bool(const char *kw, const char *params, bool *value,
		    char *reply, size_t cap)
{
	if (params == NULL) {
		replyf(reply, cap, "%s %s", kw, *value ? "TRUE" : "FALSE");
		return;
	}
	if (strcmp(params, "TRUE") == 0)
		*value = true;
	else if (strcmp(params, "FALSE") == 0)
		*value = false;
	else {
		replyf(reply, cap, "FAULT Syntax Err: %s %s", kw, params);
		return;
	}
	replyf(reply, cap, "%s now %s", kw, *value ? "TRUE" : "FALSE");
}

/* Feed one host action command through the runtime. */
static void inject(ardop_runtime *rt, const ardop_host_cmd *cmd, uint64_t now)
{
	ardop_runtime_host(rt, cmd, now);
}

void ardop_host_command(ardop_runtime *rt, const char *line, uint64_t now,
			char *reply, size_t cap)
{
	if (cap == 0)
		return;
	reply[0] = '\0';

	/* Keep the original-case line (for the ARQCALL echo), and an uppercased
	 * copy to match and split -- the inherited code uppercases the whole
	 * command, params included. */
	char orig[3000];
	char up[3000];
	snprintf(orig, sizeof(orig), "%s", line);
	size_t n = 0;
	for (; orig[n] && n < sizeof(up) - 1; n++)
		up[n] = (char)toupper((unsigned char)orig[n]);
	up[n] = '\0';

	char *kw = up;
	char *params = strchr(up, ' ');
	if (params) {
		*params++ = '\0';
		while (*params == ' ')
			params++;
		if (*params == '\0')
			params = NULL;
	}

	ardop_link *l = &rt->link;

	/* --- actions ----------------------------------------------------- */

	if (strcmp(kw, "ABORT") == 0 || strcmp(kw, "DD") == 0) {
		ardop_host_cmd c = {.kind = ARDOP_CMD_ABORT};
		inject(rt, &c, now);
		replyf(reply, cap, "ABORT");
		return;
	}
	if (strcmp(kw, "ARQCALL") == 0) {
		if (params == NULL) {
			replyf(reply, cap,
			       "FAULT Syntax Err: ARQCALL: expected \"TARGET NATTEMPTS\"");
			return;
		}
		char *sp = strchr(params, ' ');
		if (sp)
			*sp = '\0';   /* NATTEMPTS is accepted but the link
				       * repeats on its own schedule. */
		ardop_stationid target;
		ardop_stationid_err e = ardop_stationid_from_str(params, &target);
		if (e) {
			replyf(reply, cap, "FAULT Syntax Err: ARQCALL %s: %s",
			       params, ardop_stationid_strerror(e));
			return;
		}
		if (l->mycall.str[0] == '\0') {
			replyf(reply, cap, "FAULT MYCALL not set");
			return;
		}
		if (l->mode != ARDOP_MODE_ARQ) {
			replyf(reply, cap, "FAULT Not from mode %s",
			       l->mode == ARDOP_MODE_FEC ? "FEC" : "RXO");
			return;
		}
		ardop_host_cmd c = {.kind = ARDOP_CMD_CONNECT, .target = target,
				    .bandwidth = ARDOP_ARQ_BW_UNDEFINED};
		inject(rt, &c, now);
		replyf(reply, cap, "%s", orig);   /* echo the original command. */
		return;
	}
	if (strcmp(kw, "DISCONNECT") == 0) {
		if (state_is_connected(rt)) {
			ardop_host_cmd c = {.kind = ARDOP_CMD_DISCONNECT};
			inject(rt, &c, now);
			replyf(reply, cap, "DISCONNECT NOW TRUE");
		} else {
			replyf(reply, cap, "DISCONNECT IGNORED");
		}
		return;
	}
	if (strcmp(kw, "BREAK") == 0) {
		ardop_host_cmd c = {.kind = ARDOP_CMD_BREAK};
		inject(rt, &c, now);
		return;   /* no reply, as in the original. */
	}
	if (strcmp(kw, "SENDID") == 0) {
		if (l->mycall.str[0] == '\0') {
			replyf(reply, cap, "FAULT MYCALL not set");
			return;
		}
		if (rt->link.state != ARDOP_LINK_DISC) {
			replyf(reply, cap, "FAULT Not from State %s",
			       host_state_name(rt));
			return;
		}
		ardop_host_cmd c = {.kind = ARDOP_CMD_SEND_ID};
		inject(rt, &c, now);
		replyf(reply, cap, "SENDID");
		return;
	}
	if (strcmp(kw, "FECSEND") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "FAULT Syntax Err: FECSEND");
			return;
		}
		if (strcmp(params, "TRUE") != 0) {
			replyf(reply, cap, "FECSEND now FALSE");
			return;
		}
		if (l->mycall.str[0] == '\0') {
			replyf(reply, cap, "FAULT MYCALL not set");
			return;
		}
		if (l->tx_len == 0 || !ardop_frame_is_data(l->fec_frame_type)) {
			replyf(reply, cap,
			       "FAULT StartFEC failed for FECSEND TRUE.");
			return;
		}
		ardop_host_cmd c = {.kind = ARDOP_CMD_FEC_SEND,
				    .arg = l->fec_frame_type};
		inject(rt, &c, now);
		replyf(reply, cap, "FECSEND now TRUE");
		return;
	}

	/* --- configuration ----------------------------------------------- */

	if (strcmp(kw, "MYCALL") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "MYCALL %s", l->mycall.str);
			return;
		}
		ardop_stationid id;
		ardop_stationid_err e = ardop_stationid_from_str(params, &id);
		if (e)
			replyf(reply, cap, "FAULT Syntax Err: MYCALL %s: %s",
			       params, ardop_stationid_strerror(e));
		else {
			l->mycall = id;
			replyf(reply, cap, "MYCALL now %s", l->mycall.str);
		}
		return;
	}
	if (strcmp(kw, "GRIDSQUARE") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "GRIDSQUARE %s", l->grid.grid);
			return;
		}
		ardop_locator loc;
		ardop_locator_err e = ardop_locator_from_str(params, &loc);
		if (e)
			replyf(reply, cap, "FAULT Syntax Err: GRIDSQUARE %s: %s",
			       params, ardop_locator_strerror(e));
		else {
			l->grid = loc;
			replyf(reply, cap, "GRIDSQUARE now %s", l->grid.grid);
		}
		return;
	}
	if (strcmp(kw, "MYAUX") == 0) {
		if (params == NULL) {
			int off = snprintf(reply, cap, "MYAUX");
			for (size_t i = 0; i < l->n_aux && off > 0
			     && (size_t)off < cap; i++)
				off += snprintf(reply + off, cap - (size_t)off,
						"%s%s", i == 0 ? " " : ",",
						l->auxcalls[i].str);
			return;
		}
		size_t count = 0;
		ardop_stationid_err e = ardop_stationid_from_str_to_array(
			params, l->auxcalls, ARDOP_LINK_MAX_AUX, &count);
		if (e) {
			l->n_aux = 0;
			replyf(reply, cap, "FAULT Syntax Err: at callsign %zu: %s",
			       count, ardop_stationid_strerror(e));
			return;
		}
		l->n_aux = count;
		int off = snprintf(reply, cap, "MYAUX now");
		for (size_t i = 0; i < l->n_aux && off > 0
		     && (size_t)off < cap; i++)
			off += snprintf(reply + off, cap - (size_t)off, "%s%s",
					i == 0 ? " " : ",", l->auxcalls[i].str);
		return;
	}
	if (strcmp(kw, "ARQBW") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "ARQBW %s",
			       kBandwidths[l->bw_setting]);
			return;
		}
		ardop_arq_bandwidth bw;
		if (!ardop_host_bandwidth_for_name(params, &bw))
			replyf(reply, cap, "FAULT Syntax Err: ARQBW %s", params);
		else {
			l->bw_setting = bw;
			replyf(reply, cap, "ARQBW now %s",
			       ardop_host_bandwidth_name(bw));
		}
		return;
	}
	if (strcmp(kw, "PROTOCOLMODE") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "PROTOCOLMODE %s",
			       ardop_host_mode_name(l->mode));
			return;
		}
		ardop_link_mode m;
		if (!ardop_host_mode_for_name(params, &m)) {
			replyf(reply, cap, "FAULT Syntax Err: PROTOCOLMODE %s",
			       params);
			return;
		}
		ardop_host_cmd c = {.kind = ARDOP_CMD_SET_MODE, .arg = (int)m};
		inject(rt, &c, now);
		replyf(reply, cap, "PROTOCOLMODE now %s", params);
		return;
	}
	if (strcmp(kw, "FECMODE") == 0) {
		char name[32];
		if (params == NULL) {
			if (ardop_data_frame_name(l->fec_frame_type, name,
						  sizeof(name)))
				replyf(reply, cap, "FECMODE %s", name);
			else
				replyf(reply, cap, "FECMODE UNKNOWN");
			return;
		}
		uint8_t ft;
		if (!ardop_data_frame_type_for_name(params, &ft))
			replyf(reply, cap, "FAULT Syntax Err: FECMODE %s",
			       params);
		else {
			l->fec_frame_type = ft;
			replyf(reply, cap, "FECMODE now %s", params);
		}
		return;
	}
	if (strcmp(kw, "FECREPEATS") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "FECREPEATS %d", l->fec_repeats);
			return;
		}
		int v = atoi(params);
		if (v < 0 || v > 5)
			replyf(reply, cap, "FAULT Syntax Err: FECREPEATS %s",
			       params);
		else {
			l->fec_repeats = v;
			replyf(reply, cap, "FECREPEATS now %d", l->fec_repeats);
		}
		return;
	}
	if (strcmp(kw, "DATATOSEND") == 0 || strcmp(kw, "BUFFER") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "%s %zu", kw, l->tx_len);
			return;
		}
		if (atoi(params) == 0) {
			l->tx_len = 0;
			replyf(reply, cap, "%s now 0", kw);
		} else {
			replyf(reply, cap, "FAULT Syntax Err: %s %s", kw, params);
		}
		return;
	}
	if (strcmp(kw, "PURGEBUFFER") == 0) {
		l->tx_len = 0;
		replyf(reply, cap, "PURGEBUFFER");
		return;
	}
	if (strcmp(kw, "LISTEN") == 0) {
		do_bool("LISTEN", params, &l->listening, reply, cap);
		return;
	}
	if (strcmp(kw, "AUTOBREAK") == 0) {
		do_bool("AUTOBREAK", params, &l->auto_break, reply, cap);
		return;
	}
	if (strcmp(kw, "FSKONLY") == 0) {
		do_bool("FSKONLY", params, &l->fsk_only, reply, cap);
		return;
	}
	if (strcmp(kw, "USE600MODES") == 0) {
		do_bool("USE600MODES", params, &l->use_600_modes, reply, cap);
		return;
	}
	if (strcmp(kw, "ENABLEPINGACK") == 0) {
		do_bool("ENABLEPINGACK", params, &l->enable_ping_ack, reply, cap);
		return;
	}

	/* --- queries / info ---------------------------------------------- */

	if (strcmp(kw, "STATE") == 0) {
		if (params == NULL)
			replyf(reply, cap, "STATE %s", host_state_name(rt));
		else
			replyf(reply, cap, "FAULT Syntax Err: STATE %s", params);
		return;
	}
	if (strcmp(kw, "VERSION") == 0) {
		replyf(reply, cap, "VERSION %s_%s", ARDOP_HOST_PRODUCT,
		       ardop_build_id());
		return;
	}
	if (strcmp(kw, "RDY") == 0)
		return;   /* no reply required. */

	/*
	 * INITIALIZE. docs/Host_Interface_Commands.md: "Performs initialization
	 * of the modem. The first command that needs to be issued to the modem",
	 * and it is the first line in every one of that document's three example
	 * handshakes -- so a station that faults on it faults on the first thing
	 * a client says to it.
	 *
	 * There is nothing left to do -- the runtime is already initialised by
	 * the time any socket exists -- but the reply is not optional. The docs
	 * say "None"; the reference source (src/common/HostInterface.c) says
	 * SendReplyToHost("INITIALIZE") -- the bare keyword, echoed back -- and
	 * that is the one that is right: Pat's TNC client (wl2k-go/transport/
	 * ardop) blocks on its response channel for a reply whose command word
	 * is INITIALIZE before it sends anything else, so a station that stays
	 * silent here never gets a second command. Observed directly: Pat's
	 * activity log stops dead at "INITIALIZE" and never reaches PROTOCOLMODE,
	 * MYCALL or a connect attempt.
	 */
	if (strcmp(kw, "INITIALIZE") == 0) {
		replyf(reply, cap, "INITIALIZE");
		return;
	}

	/*
	 * BUSYDET, which was reachable from this program's own settings and not
	 * from a guest's.
	 *
	 * app/spine.c carried a private direct write with the note "there is no
	 * host command for it -- if BUSYDET is ever added to shell/host.c,
	 * delete this case and let it join the rest". This is that, and the
	 * spine's special case is now gone: one validator, one path, and a
	 * client can set it exactly as ARIM expects to.
	 */
	if (strcmp(kw, "BUSYDET") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "BUSYDET %d", rt->busy_det);
			return;
		}
		char *end = NULL;
		long v = strtol(params, &end, 10);
		if (end == params || *end != '\0' || v < 0 || v > 10) {
			replyf(reply, cap, "FAULT Syntax Err: BUSYDET %s",
			       params);
			return;
		}
		rt->busy_det = (int)v;
		replyf(reply, cap, "BUSYDET now %ld", v);
		return;
	}

	/*
	 * ARQTIMEOUT: seconds of silence before giving up (link.h's
	 * ardop_link::arq_timeout, enforced by rule 1.7 in core/link/link.c --
	 * an active but unproductive IDLE/ACK exchange does not trip it, only
	 * genuine silence does). Lives on the link, not the runtime, matching
	 * AUTOBREAK just above: it is link.c's own timer that reads it.
	 *
	 * Pat's TNC client (wl2k-go/transport/ardop) sends this immediately
	 * after PROTOCOLMODE during its own INITIALIZE handshake and, like
	 * INITIALIZE itself, waits on the reply -- an unrecognised-command
	 * FAULT here stopped a client's connect sequence dead, one step further
	 * into it than the INITIALIZE gap.
	 */
	if (strcmp(kw, "ARQTIMEOUT") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "ARQTIMEOUT %d", l->arq_timeout);
			return;
		}
		char *end = NULL;
		long v = strtol(params, &end, 10);
		if (end == params || *end != '\0' || v < 30 || v > 240) {
			replyf(reply, cap, "FAULT Syntax Err: ARQTIMEOUT %s",
			       params);
			return;
		}
		l->arq_timeout = (int)v;
		replyf(reply, cap, "ARQTIMEOUT now %ld", v);
		return;
	}

	/*
	 * CWID: a Morse identifier appended after an IDFRAME, in one of two
	 * styles (TRUE: FSK mark/space tones; ONOFF: traditional on-off keying)
	 * or disabled (FALSE). Three states, not a boolean, matching the
	 * reference exactly. Like ARQTIMEOUT, this is on Pat's handshake path
	 * and had to answer before the gap in what it *does* was reachable: the
	 * value is stored and reported, but nothing generates the actual CW
	 * audio yet (core/modem/modulate.c has no Morse keyer).
	 */
	if (strcmp(kw, "CWID") == 0) {
		if (params == NULL) {
			replyf(reply, cap, "CWID %s",
			       !rt->want_cwid ? "FALSE"
					      : rt->cwid_onoff ? "ONOFF" : "TRUE");
			return;
		}
		if (strcmp(params, "TRUE") == 0) {
			rt->want_cwid = true;
			rt->cwid_onoff = false;
		} else if (strcmp(params, "FALSE") == 0) {
			rt->want_cwid = false;
		} else if (strcmp(params, "ONOFF") == 0) {
			rt->want_cwid = true;
			rt->cwid_onoff = true;
		} else {
			replyf(reply, cap, "FAULT Syntax Err: CWID %s", params);
			return;
		}
		replyf(reply, cap, "CWID now %s",
		       !rt->want_cwid ? "FALSE"
				      : rt->cwid_onoff ? "ONOFF" : "TRUE");
		return;
	}

	/* Unknown command. The reply text (typo and all) is the frozen wire
	 * string the inherited TNC sends. */
	replyf(reply, cap, "FAULT CMD %s not recoginized", kw);
}

/* --- data channel framing ------------------------------------------------ */

size_t ardop_host_data_frame(uint8_t *out, size_t cap, const char *tag,
			     const uint8_t *data, size_t len)
{
	size_t total = 2 + ARDOP_HOST_TAG_LEN + len;
	if (total > cap)
		return 0;
	size_t tagged = ARDOP_HOST_TAG_LEN + len;   /* the counted length. */
	out[0] = (uint8_t)(tagged >> 8);
	out[1] = (uint8_t)(tagged & 0xFF);
	memcpy(out + 2, tag, ARDOP_HOST_TAG_LEN);
	memcpy(out + 2 + ARDOP_HOST_TAG_LEN, data, len);
	return total;
}

bool ardop_host_data_parse(const uint8_t *buf, size_t avail,
			   const uint8_t **payload, size_t *payload_len,
			   size_t *consumed)
{
	if (avail < 2)
		return false;   /* need the length prefix. */
	size_t len = ((size_t)buf[0] << 8) | buf[1];
	if (avail < 2 + len)
		return false;   /* payload not fully arrived. */
	*payload = buf + 2;
	*payload_len = len;
	*consumed = 2 + len;
	return true;
}
