#include "app/asp.h"

#include <stdio.h>
#include <string.h>

/**
 * @file asp.c
 * @brief The ASP/1 session state machine (see asp.h).
 */

/*
 * How much file payload one DATA message carries.
 *
 * analysis/17 §6 fixes the trade: chat may interleave at message boundaries, so
 * the worst-case delay for a chat line during a transfer is one chunk. At a few
 * hundred bytes a second, 1024 is a few seconds -- long enough that the framing
 * overhead is negligible, short enough that a conversation stays a conversation.
 */
#define ASP_CHUNK 1024

/* --- outbound --------------------------------------------------------------- */

/* Push whatever is pending. Partial sends are normal: the transmit queue is
 * finite and admission belongs to the spine, so the remainder waits. */
static void flush(asp_session *s)
{
	while (s->out_sent < s->out_len) {
		size_t n = s->io->send(s->io->ctx, s->out + s->out_sent,
				       s->out_len - s->out_sent);
		if (n == 0)
			return;
		s->out_sent += n;
	}
	s->out_len = s->out_sent = 0;
}

/* True when the outbound buffer is empty and another message may be built. */
static bool can_queue(const asp_session *s)
{
	return s->out_len == s->out_sent;
}

static bool queue_msg(asp_session *s, asp_msg_type type, const void *payload,
		      size_t len)
{
	if (!can_queue(s))
		return false;
	size_t n = asp_frame_put(s->out, sizeof s->out, type, payload, len);
	if (n == 0)
		return false;
	s->out_len = n;
	s->out_sent = 0;
	flush(s);
	return true;
}

static void fail(asp_session *s, const char *why)
{
	s->state = ASP_LINK_IDLE;
	if (s->io->protocol_error)
		s->io->protocol_error(s->io->ctx, why);
}

/* --- lifecycle -------------------------------------------------------------- */

void asp_open(asp_session *s, const asp_io *io, const char *my_call)
{
	memset(s, 0, sizeof *s);
	s->io = io;
	s->next_id = 1;
	snprintf(s->my_call, sizeof s->my_call, "%s", my_call ? my_call : "");

	asp_hello h = {.version = ASP_VERSION,
		       .caps = ASP_CAP_FILES | ASP_CAP_RESUME};
	snprintf(h.call, sizeof h.call, "%s", s->my_call);

	uint8_t payload[ASP_MAX_PAYLOAD];
	size_t n = asp_hello_put(payload, sizeof payload, &h);

	s->state = ASP_LINK_HELLO_SENT;
	(void)queue_msg(s, ASP_MSG_HELLO, payload, n);
}

void asp_close(asp_session *s)
{
	/* No BYE: a link that is going away may not be able to carry one, and a
	 * transfer's resumability does not depend on a clean goodbye -- §8 keys
	 * resume on (peer, name, size, crc), which survives any ending. */
	s->state = ASP_LINK_IDLE;
	s->tx_state = ASP_XFER_NONE;
	s->rx_state = ASP_XFER_NONE;
	s->in_len = 0;
	s->out_len = s->out_sent = 0;
}

bool asp_can_send_files(const asp_session *s)
{
	return s->state == ASP_LINK_ASP && (s->peer_caps & ASP_CAP_FILES) &&
	       s->tx_state == ASP_XFER_NONE;
}

/* --- chat ------------------------------------------------------------------- */

bool asp_send_text(asp_session *s, const char *text, size_t len)
{
	if (len > ASP_MAX_PAYLOAD)
		return false;

	if (s->state == ASP_LINK_RAW) {
		/* Unframed, because the peer has no decoder. Whatever they are
		 * running shows it as typed. */
		if (!can_queue(s) || len > sizeof s->out)
			return false;
		memcpy(s->out, text, len);
		s->out_len = len;
		s->out_sent = 0;
		flush(s);
		return true;
	}
	if (s->state != ASP_LINK_ASP && s->state != ASP_LINK_HELLO_SENT)
		return false;
	return queue_msg(s, ASP_MSG_TEXT, text, len);
}

/* --- file transfer, outbound ------------------------------------------------ */

bool asp_offer_file(asp_session *s, const char *name, const char *content_type,
		    uint32_t size, uint32_t crc32)
{
	if (!asp_can_send_files(s))
		return false;

	asp_offer o = {.id = s->next_id++, .size = size, .crc32 = crc32};
	snprintf(o.name, sizeof o.name, "%s", name ? name : "file");
	snprintf(o.content_type, sizeof o.content_type, "%s",
		 content_type ? content_type : "application/octet-stream");

	uint8_t payload[ASP_MAX_PAYLOAD];
	size_t n = asp_offer_put(payload, sizeof payload, &o);
	if (n == 0 || !queue_msg(s, ASP_MSG_OFFER, payload, n))
		return false;

	s->tx_state = ASP_XFER_OFFERED;
	s->tx_id = o.id;
	s->tx_size = size;
	s->tx_crc = crc32;
	s->tx_offset = 0;
	return true;
}

/* Move one chunk of the active outbound transfer, if there is room. */
static void pump_tx(asp_session *s)
{
	if (s->tx_state != ASP_XFER_RUNNING || !can_queue(s))
		return;

	/*
	 * §7: keep the queue fed but not stuffed. Asking for credit before
	 * building the chunk means a full queue costs a check rather than a
	 * read of the file and a memcpy that is then thrown away.
	 */
	if (s->io->credit(s->io->ctx) < ASP_CHUNK + 8)
		return;

	if (s->tx_offset >= s->tx_size) {
		uint8_t payload[8];
		size_t n = asp_id_put(payload, sizeof payload, s->tx_id);
		if (queue_msg(s, ASP_MSG_DONE, payload, n))
			s->tx_state = ASP_XFER_AWAIT_RESULT;
		return;
	}

	uint8_t chunk[ASP_CHUNK];
	uint32_t want = s->tx_size - s->tx_offset;
	if (want > ASP_CHUNK)
		want = ASP_CHUNK;

	size_t got = s->io->read_file(s->io->ctx, s->tx_offset, chunk, want);
	if (got == 0) {
		/* The file shrank or could not be read. Cancelling is honest;
		 * sending short would fail the receiver's CRC after the whole
		 * remainder had been sent. */
		uint8_t payload[8];
		size_t n = asp_id_code_put(payload, sizeof payload, s->tx_id,
					   ASP_REJECT_REFUSED);
		(void)queue_msg(s, ASP_MSG_CANCEL, payload, n);
		s->tx_state = ASP_XFER_NONE;
		if (s->io->transfer_done)
			s->io->transfer_done(s->io->ctx, false, s->tx_id,
					     ASP_RESULT_SHORT);
		return;
	}

	if (queue_msg(s, ASP_MSG_DATA, chunk, got)) {
		s->tx_offset += (uint32_t)got;
		if (s->io->progress)
			s->io->progress(s->io->ctx, false, s->tx_offset,
					s->tx_size);
	}
}

bool asp_cancel(asp_session *s, bool inbound, asp_reject_code reason)
{
	uint16_t id = inbound ? s->rx_id : s->tx_id;
	if ((inbound ? s->rx_state : s->tx_state) == ASP_XFER_NONE)
		return false;

	uint8_t payload[8];
	size_t n = asp_id_code_put(payload, sizeof payload, id, (uint8_t)reason);
	if (!queue_msg(s, ASP_MSG_CANCEL, payload, n))
		return false;

	/* §8: partial state is kept, so a cancel is resumable. */
	if (inbound)
		s->rx_state = ASP_XFER_NONE;
	else
		s->tx_state = ASP_XFER_NONE;
	return true;
}

/* --- file transfer, inbound ------------------------------------------------- */

bool asp_answer_offer(asp_session *s, bool accept, uint32_t have,
		      uint32_t prefix_crc, asp_reject_code reason)
{
	if (s->rx_state != ASP_XFER_RECEIVING)
		return false;

	if (!accept) {
		uint8_t payload[8];
		size_t n = asp_id_code_put(payload, sizeof payload, s->rx_id,
					   (uint8_t)reason);
		s->rx_state = ASP_XFER_NONE;
		return queue_msg(s, ASP_MSG_REJECT, payload, n);
	}

	asp_accept a = {.id = s->rx_id, .have = have, .prefix_crc = prefix_crc};
	uint8_t payload[16];
	size_t n = asp_accept_put(payload, sizeof payload, &a);
	if (!queue_msg(s, ASP_MSG_ACCEPT, payload, n))
		return false;

	s->rx_have = have;
	s->rx_running_crc = prefix_crc;
	return true;
}

static void on_offer(asp_session *s, const uint8_t *p, size_t len)
{
	asp_offer o;
	if (!asp_offer_get(p, len, &o)) {
		fail(s, "malformed OFFER");
		return;
	}

	/* One inbound transfer at a time; a second offer is refused rather than
	 * silently replacing the first. */
	if (s->rx_state != ASP_XFER_NONE) {
		uint8_t payload[8];
		size_t n = asp_id_code_put(payload, sizeof payload, o.id,
					   ASP_REJECT_REFUSED);
		(void)queue_msg(s, ASP_MSG_REJECT, payload, n);
		return;
	}

	/* The name is sanitised before anything else looks at it, and a name
	 * that cannot be made safe is refused outright. */
	if (!asp_safe_name(o.name, s->rx_name, sizeof s->rx_name)) {
		uint8_t payload[8];
		size_t n = asp_id_code_put(payload, sizeof payload, o.id,
					   ASP_REJECT_BAD_NAME);
		(void)queue_msg(s, ASP_MSG_REJECT, payload, n);
		return;
	}

	s->rx_state = ASP_XFER_RECEIVING;
	s->rx_id = o.id;
	s->rx_size = o.size;
	s->rx_crc = o.crc32;
	s->rx_have = 0;
	s->rx_running_crc = ASP_CRC32_INIT;

	uint32_t have = 0, prefix = ASP_CRC32_INIT;
	if (s->io->offer_arrived &&
	    s->io->offer_arrived(s->io->ctx, &o, &have, &prefix))
		(void)asp_answer_offer(s, true, have, prefix,
				       ASP_REJECT_REFUSED);
	/* Otherwise the operator is being asked; asp_answer_offer comes later. */
}

static void on_accept(asp_session *s, const uint8_t *p, size_t len)
{
	asp_accept a;
	if (!asp_accept_get(p, len, &a)) {
		fail(s, "malformed ACCEPT");
		return;
	}
	if (s->tx_state != ASP_XFER_OFFERED || a.id != s->tx_id)
		return;

	/*
	 * §5's whole reason for a prefix CRC. The receiver holding N bytes is
	 * not evidence they are *this* file's first N. Checking here costs one
	 * read; discovering it from the whole-file CRC costs the entire
	 * remainder of the transfer first.
	 */
	uint32_t from = 0;
	if (a.have > 0 && a.have <= s->tx_size) {
		uint32_t crc = ASP_CRC32_INIT;
		uint8_t buf[ASP_CHUNK];
		uint32_t off = 0;
		bool ok = true;
		while (off < a.have) {
			uint32_t want = a.have - off;
			if (want > sizeof buf)
				want = sizeof buf;
			size_t got = s->io->read_file(s->io->ctx, off, buf, want);
			if (got == 0) {
				ok = false;
				break;
			}
			crc = asp_crc32(crc, buf, got);
			off += (uint32_t)got;
		}
		if (ok && crc == a.prefix_crc)
			from = a.have;
	}

	uint8_t payload[8];
	size_t n = asp_start_put(payload, sizeof payload, s->tx_id, from);
	if (!queue_msg(s, ASP_MSG_START, payload, n))
		return;

	s->tx_offset = from;
	s->tx_state = ASP_XFER_RUNNING;
	if (s->io->progress)
		s->io->progress(s->io->ctx, false, s->tx_offset, s->tx_size);
}

static void on_start(asp_session *s, const uint8_t *p, size_t len)
{
	uint16_t id;
	uint32_t from;
	if (!asp_start_get(p, len, &id, &from)) {
		fail(s, "malformed START");
		return;
	}
	if (s->rx_state != ASP_XFER_RECEIVING || id != s->rx_id)
		return;

	/* from == 0 means our prefix did not match: throw it away and start
	 * again, which is what §8 says and what keeps a resumed file honest. */
	if (from == 0 && s->rx_have > 0) {
		if (s->io->truncate_file)
			s->io->truncate_file(s->io->ctx);
		s->rx_running_crc = ASP_CRC32_INIT;
	}
	s->rx_have = from;
}

static void on_data(asp_session *s, const uint8_t *p, size_t len)
{
	if (s->rx_state != ASP_XFER_RECEIVING)
		return;   /* nothing is being received; §3 says skip, not fail. */

	if (s->io->write_file && !s->io->write_file(s->io->ctx, p, len)) {
		uint8_t payload[8];
		size_t n = asp_id_code_put(payload, sizeof payload, s->rx_id,
					   ASP_RESULT_WRITE_FAILED);
		(void)queue_msg(s, ASP_MSG_RESULT, payload, n);
		s->rx_state = ASP_XFER_NONE;
		return;
	}

	s->rx_running_crc = asp_crc32(s->rx_running_crc, p, len);
	s->rx_have += (uint32_t)len;
	if (s->io->progress)
		s->io->progress(s->io->ctx, true, s->rx_have, s->rx_size);
}

static void on_done(asp_session *s, const uint8_t *p, size_t len)
{
	uint16_t id;
	if (!asp_id_get(p, len, &id)) {
		fail(s, "malformed DONE");
		return;
	}
	if (s->rx_state != ASP_XFER_RECEIVING || id != s->rx_id)
		return;

	asp_result_code result = ASP_RESULT_OK;
	if (s->rx_have != s->rx_size)
		result = ASP_RESULT_SHORT;
	else if (s->rx_running_crc != s->rx_crc)
		result = ASP_RESULT_CRC_MISMATCH;

	uint8_t payload[8];
	size_t n = asp_id_code_put(payload, sizeof payload, id, (uint8_t)result);
	(void)queue_msg(s, ASP_MSG_RESULT, payload, n);

	s->rx_state = ASP_XFER_NONE;
	if (s->io->transfer_done)
		s->io->transfer_done(s->io->ctx, true, id, result);
}

static void on_result(asp_session *s, const uint8_t *p, size_t len)
{
	uint16_t id;
	uint8_t code;
	if (!asp_id_code_get(p, len, &id, &code)) {
		fail(s, "malformed RESULT");
		return;
	}
	if (s->tx_state == ASP_XFER_NONE || id != s->tx_id)
		return;

	s->tx_state = ASP_XFER_NONE;
	if (s->io->transfer_done)
		s->io->transfer_done(s->io->ctx, false, id,
				     (asp_result_code)code);
}

/* --- inbound framing -------------------------------------------------------- */

static void dispatch(asp_session *s, asp_msg_type type, const uint8_t *p,
		     size_t len)
{
	switch (type) {
	case ASP_MSG_HELLO: {
		asp_hello h;
		if (asp_hello_get(p, len, &h)) {
			s->peer_caps = h.caps;
			snprintf(s->peer_call, sizeof s->peer_call, "%s",
				 h.call);
			s->state = ASP_LINK_ASP;
		}
		return;
	}
	case ASP_MSG_BYE:
		s->state = ASP_LINK_IDLE;
		return;
	case ASP_MSG_TEXT:
		if (s->io->text_arrived)
			s->io->text_arrived(s->io->ctx, (const char *)p, len,
					    false);
		return;
	case ASP_MSG_TEXT_B: {
		/* §6: a TEXT_B that fails to parse is shown as raw text rather
		 * than discarded -- broadcast is where we most often meet a
		 * station running something else. */
		asp_text_b t;
		if (asp_text_b_get(p, len, &t)) {
			if (s->io->text_arrived)
				s->io->text_arrived(s->io->ctx, t.text,
						    t.text_len, false);
		} else if (s->io->text_arrived) {
			s->io->text_arrived(s->io->ctx, (const char *)p, len,
					    true);
		}
		return;
	}
	case ASP_MSG_OFFER:  on_offer(s, p, len); return;
	case ASP_MSG_ACCEPT: on_accept(s, p, len); return;
	case ASP_MSG_START:  on_start(s, p, len); return;
	case ASP_MSG_DATA:   on_data(s, p, len); return;
	case ASP_MSG_DONE:   on_done(s, p, len); return;
	case ASP_MSG_RESULT: on_result(s, p, len); return;

	case ASP_MSG_REJECT:
	case ASP_MSG_CANCEL: {
		uint16_t id;
		uint8_t code;
		if (!asp_id_code_get(p, len, &id, &code))
			return;
		if (s->tx_state != ASP_XFER_NONE && id == s->tx_id) {
			s->tx_state = ASP_XFER_NONE;
			if (s->io->transfer_done)
				s->io->transfer_done(s->io->ctx, false, id,
						     ASP_RESULT_SHORT);
		} else if (s->rx_state != ASP_XFER_NONE && id == s->rx_id) {
			s->rx_state = ASP_XFER_NONE;
			if (s->io->transfer_done)
				s->io->transfer_done(s->io->ctx, true, id,
						     ASP_RESULT_SHORT);
		}
		return;
	}
	}

	/* §3: an unknown type is skipped by its length and is never an error.
	 * Reaching here means exactly that -- the frame was well-formed and its
	 * length has already been consumed by the caller. */
}

void asp_recv(asp_session *s, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0) {
		/*
		 * Raw mode is decided on the first byte and never revisited
		 * (§2). Deciding late would mean a peer could flip us into ASP
		 * halfway through a conversation, and a decision that can change
		 * is one every later branch has to re-check.
		 */
		if (s->state == ASP_LINK_HELLO_SENT && s->in_len == 0 &&
		    p[0] != ASP_MSG_HELLO) {
			s->state = ASP_LINK_RAW;
		}

		if (s->state == ASP_LINK_RAW) {
			if (s->io->text_arrived)
				s->io->text_arrived(s->io->ctx,
						    (const char *)p, len, true);
			return;
		}

		size_t room = sizeof s->in - s->in_len;
		size_t take = len < room ? len : room;
		if (take == 0) {
			/* A message larger than the maximum cannot be
			 * assembled, and §8 says not to resynchronise. */
			fail(s, "message exceeds the maximum");
			return;
		}
		memcpy(s->in + s->in_len, p, take);
		s->in_len += take;
		p += take;
		len -= take;

		for (;;) {
			asp_msg_type type;
			const uint8_t *payload;
			size_t plen, consumed;
			asp_frame_status st =
				asp_frame_get(s->in, s->in_len, &type, &payload,
					      &plen, &consumed);
			if (st == ASP_FRAME_SHORT)
				break;
			if (st == ASP_FRAME_ERROR) {
				fail(s, "malformed message");
				return;
			}
			dispatch(s, type, payload, plen);
			memmove(s->in, s->in + consumed, s->in_len - consumed);
			s->in_len -= consumed;
			if (s->state == ASP_LINK_IDLE)
				return;   /* dispatch ended the session. */
		}
	}
}

void asp_service(asp_session *s)
{
	flush(s);
	pump_tx(s);
}
