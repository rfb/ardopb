#include "shell/ptt_cat.h"

#include <string.h>

/**
 * @file ptt_cat.c
 * @brief CAT keying commands (see ptt_cat.h). Pure; no device, no I/O.
 */

/* CI-V framing, from Icom's CI-V reference guide. */
#define CIV_PREAMBLE 0xFEu
#define CIV_END      0xFDu
#define CIV_CTRL     0xE0u   /* this controller's address */
#define CIV_CMD_PTT  0x1Cu
#define CIV_SUB_PTT  0x00u
#define CIV_OK       0xFBu
#define CIV_NG       0xFAu

size_t ardop_cat_frame(ardop_cat_family family, uint8_t addr, bool key,
		       uint8_t *out, size_t cap)
{
	switch (family) {
	case ARDOP_CAT_CIV: {
		const uint8_t f[8] = {
			CIV_PREAMBLE, CIV_PREAMBLE, addr, CIV_CTRL,
			CIV_CMD_PTT, CIV_SUB_PTT, key ? 0x01u : 0x00u, CIV_END,
		};
		if (cap < sizeof f)
			return 0;
		memcpy(out, f, sizeof f);
		return sizeof f;
	}
	case ARDOP_CAT_KENWOOD: {
		const char *s = key ? "TX;" : "RX;";
		size_t n = strlen(s);
		if (cap < n)
			return 0;
		memcpy(out, s, n);
		return n;
	}
	case ARDOP_CAT_YAESU: {
		const char *s = key ? "TX1;" : "TX0;";
		size_t n = strlen(s);
		if (cap < n)
			return 0;
		memcpy(out, s, n);
		return n;
	}
	}
	return 0;
}

bool ardop_cat_is_acknowledged(ardop_cat_family family)
{
	/* Kenwood's TX;/RX; are set-only: the radio sends nothing back, so a
	 * missing reply there proves nothing about the link. Named rather than
	 * silently treated as success. */
	return family != ARDOP_CAT_KENWOOD;
}

/*
 * A CI-V reply. The frame is
 *
 *     FE FE <to> <from> <code> FD
 *
 * and the code is FB for accepted or FA for refused. Anything else complete is
 * ignored -- which matters, because a rig with transceive enabled echoes our own
 * command back, and a reader that took the echo for the answer would report
 * success before the radio had even parsed the request.
 */
static ardop_cat_reply parse_civ(uint8_t addr, const uint8_t *buf, size_t n,
				 size_t *consumed)
{
	*consumed = 0;

	/* Resynchronise: skip anything before a preamble pair. */
	size_t start = 0;
	while (start + 1 < n &&
	       !(buf[start] == CIV_PREAMBLE && buf[start + 1] == CIV_PREAMBLE))
		start++;
	if (start + 1 >= n)
		return ARDOP_CAT_INCOMPLETE;

	size_t end = start;
	while (end < n && buf[end] != CIV_END)
		end++;
	if (end >= n)
		return ARDOP_CAT_INCOMPLETE;

	*consumed = end + 1;

	/* FE FE to from code FD is the shortest useful frame. */
	size_t len = end + 1 - start;
	if (len < 6)
		return ARDOP_CAT_IGNORE;

	uint8_t to = buf[start + 2];
	uint8_t from = buf[start + 3];
	uint8_t code = buf[start + 4];

	/* Addressed to this controller, and from the rig we asked -- unless we
	 * are broadcasting, in which case any rig answering is ours. */
	if (to != CIV_CTRL)
		return ARDOP_CAT_IGNORE;
	if (addr != ARDOP_CIV_DEFAULT_ADDR && from != addr)
		return ARDOP_CAT_IGNORE;

	if (code == CIV_OK)
		return ARDOP_CAT_ACK;
	if (code == CIV_NG)
		return ARDOP_CAT_NAK;
	return ARDOP_CAT_IGNORE;
}

ardop_cat_reply ardop_cat_parse_reply(ardop_cat_family family, uint8_t addr,
				      const uint8_t *buf, size_t n,
				      size_t *consumed)
{
	size_t dummy = 0;
	if (!consumed)
		consumed = &dummy;
	*consumed = 0;

	switch (family) {
	case ARDOP_CAT_CIV:
		return parse_civ(addr, buf, n, consumed);

	case ARDOP_CAT_KENWOOD:
		/* Nothing comes back. Anything that does is unsolicited. */
		*consumed = n;
		return ARDOP_CAT_ACK;

	case ARDOP_CAT_YAESU: {
		/* Yaesu answers a *failed* set command with "?;" and says nothing
		 * on success, so silence is the good case and the only thing to
		 * look for is the complaint. */
		for (size_t i = 0; i + 1 < n; i++) {
			if (buf[i] == '?' && buf[i + 1] == ';') {
				*consumed = i + 2;
				return ARDOP_CAT_NAK;
			}
		}
		*consumed = n;
		return ARDOP_CAT_ACK;
	}
	}
	return ARDOP_CAT_IGNORE;
}

const char *ardop_cat_family_str(ardop_cat_family family)
{
	switch (family) {
	case ARDOP_CAT_CIV:     return "civ";
	case ARDOP_CAT_KENWOOD: return "kenwood";
	case ARDOP_CAT_YAESU:   return "yaesu";
	}
	return "unknown";
}

bool ardop_cat_family_from_str(const char *s, ardop_cat_family *out)
{
	if (!s)
		return false;
	if (strcmp(s, "civ") == 0 || strcmp(s, "icom") == 0 ||
	    strcmp(s, "xiegu") == 0) {
		*out = ARDOP_CAT_CIV;
		return true;
	}
	if (strcmp(s, "kenwood") == 0) {
		*out = ARDOP_CAT_KENWOOD;
		return true;
	}
	if (strcmp(s, "yaesu") == 0) {
		*out = ARDOP_CAT_YAESU;
		return true;
	}
	return false;
}
