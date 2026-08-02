#include "fecchat.h"

#include "core/codec/frame.h"

#include <stdio.h>
#include <string.h>

/**
 * @file fecchat.c
 * @brief The FEC broadcast profile (see fecchat.h).
 */

void fecchat_init(fecchat *f)
{
	memset(f, 0, sizeof *f);
}

bool fecchat_is_new(fecchat *f, const char *call, uint16_t msg_id,
		    uint64_t now_ms)
{
	int free_slot = -1, oldest = 0;

	for (int i = 0; i < FECCHAT_SEEN_MAX; i++) {
		if (!f->seen[i].used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		/* Expired entries are reusable and are not matches: §6's window
		 * is what makes a wrapped msg_id harmless. */
		if (now_ms - f->seen[i].at_ms > FECCHAT_DEDUP_MS) {
			f->seen[i].used = false;
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (f->seen[i].msg_id == msg_id &&
		    strcmp(f->seen[i].call, call) == 0)
			return false;
		if (f->seen[i].at_ms < f->seen[oldest].at_ms)
			oldest = i;
	}

	/* Full: evict the oldest. A net busy enough to fill this is one where
	 * the oldest entry is well past being a plausible repeat anyway. */
	const int slot = free_slot >= 0 ? free_slot : oldest;
	snprintf(f->seen[slot].call, sizeof f->seen[slot].call, "%s", call);
	f->seen[slot].msg_id = msg_id;
	f->seen[slot].at_ms = now_ms;
	f->seen[slot].used = true;
	return true;
}

/* The bytes a TEXT_B costs before any text: the frame's type and length, then
 * the callsign length prefix, the callsign, and the message id. */
static size_t overhead(const char *call)
{
	return 1 /* type */ + 1 /* varint length, one byte at these sizes */
	       + 1 /* calllen */ + strlen(call) + 2 /* msg_id */;
}

size_t fecchat_text_capacity(uint8_t frame_type, const char *call)
{
	const ardop_frame_spec *spec = ardop_frame_spec_for(frame_type);
	if (!spec)
		return 0;

	const size_t frame = ardop_frame_payload_bytes(spec);
	const size_t cost = overhead(call);
	return frame > cost ? frame - cost : 0;
}

size_t fecchat_encode(fecchat *f, const char *call, const char *text,
		      size_t text_len, uint8_t *out, size_t cap)
{
	asp_text_b t;
	memset(&t, 0, sizeof t);
	snprintf(t.call, sizeof t.call, "%s", call);
	t.msg_id = f->next_id;
	if (text_len > sizeof t.text)
		return 0;
	memcpy(t.text, text, text_len);
	t.text_len = text_len;

	uint8_t payload[ASP_MAX_PAYLOAD];
	const size_t plen = asp_text_b_put(payload, sizeof payload, &t);
	if (plen == 0)
		return 0;

	const size_t n = asp_frame_put(out, cap, ASP_MSG_TEXT_B, payload, plen);
	if (n == 0)
		return 0;

	/* Consumed only on success, so a refused message does not leave a hole
	 * in the sequence a receiver might read as a lost one. */
	f->next_id++;
	return n;
}

fecchat_kind fecchat_decode(const uint8_t *data, size_t len, char *call,
			    size_t call_cap, uint16_t *msg_id,
			    const char **text, size_t *text_len)
{
	call[0] = '\0';
	*msg_id = 0;
	*text = (const char *)data;
	*text_len = len;

	asp_msg_type type;
	const uint8_t *payload;
	size_t plen, consumed;
	if (asp_frame_get(data, len, &type, &payload, &plen, &consumed) !=
	    ASP_FRAME_OK)
		return FECCHAT_RAW;
	if (type != ASP_MSG_TEXT_B)
		return FECCHAT_RAW;

	/* Held here rather than pointed into, because asp_text_b_get copies the
	 * text into its own buffer and the caller borrows ours. Static is safe:
	 * this is a single-threaded tool and the caller uses it before the next
	 * call, which is the same contract asp_frame_get already has. */
	static asp_text_b t;
	if (!asp_text_b_get(payload, plen, &t))
		return FECCHAT_RAW;

	snprintf(call, call_cap, "%s", t.call);
	*msg_id = t.msg_id;
	*text = t.text;
	*text_len = t.text_len;
	return FECCHAT_MESSAGE;
}
