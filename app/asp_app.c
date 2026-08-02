#include "app/asp_app.h"

#include "shell/sys.h"

#include <string.h>

/**
 * @file asp_app.c
 * @brief ASP over a spine and a filesystem (see asp_app.h).
 */

#define IO_CHUNK 4096

static void note(asp_app *a, const char *text)
{
	if (a->hooks.note)
		a->hooks.note(a->hooks.ctx, text);
}

/* --- the asp_io, over the spine and stdio ----------------------------------- */

static size_t io_send(void *ctx, const void *data, size_t len)
{
	asp_app *a = ctx;
	app_tx_status why = APP_TX_OK;
	return app_tx_submit(a->spine, data, len, &why);
}

static size_t io_credit(void *ctx)
{
	return app_tx_credit(((asp_app *)ctx)->spine);
}

static size_t io_read_file(void *ctx, uint32_t offset, void *out, size_t len)
{
	asp_app *a = ctx;
	if (!a->send_fp)
		return 0;
	if (fseek(a->send_fp, (long)offset, SEEK_SET) != 0)
		return 0;
	return fread(out, 1, len, a->send_fp);
}

static bool io_write_file(void *ctx, const void *data, size_t len)
{
	asp_app *a = ctx;
	if (!a->recv_fp)
		return false;
	if (fwrite(data, 1, len, a->recv_fp) != len)
		return false;
	/*
	 * Flushed as it goes, because the point of a partial is that it survives
	 * whatever ended the connection -- and what ends a connection on a boat
	 * or in a field is often the same thing that ends the process.
	 */
	return fflush(a->recv_fp) == 0;
}

static void io_truncate_file(void *ctx)
{
	asp_app *a = ctx;
	if (!a->recv_fp)
		return;
	fclose(a->recv_fp);
	a->recv_fp = fopen(a->part_path, "wb");
	note(a, "the partial did not match; starting again from the beginning");
}

static void io_text(void *ctx, const char *text, size_t len, bool raw)
{
	asp_app *a = ctx;
	if (a->hooks.text_arrived)
		a->hooks.text_arrived(a->hooks.ctx, text, len, raw);
}

static void io_progress(void *ctx, bool inbound, uint32_t done, uint32_t total)
{
	asp_app *a = ctx;
	if (a->hooks.progress)
		a->hooks.progress(a->hooks.ctx, inbound, done, total);
}

static void io_error(void *ctx, const char *why)
{
	asp_app *a = ctx;
	char msg[160];
	snprintf(msg, sizeof msg, "protocol error: %s", why);
	note(a, msg);
}

/* Join the receive directory to a bare name. The name has already been through
 * asp_safe_name, so it has no separators left to escape with. */
static void join(char *out, size_t cap, const char *dir, const char *name,
		 const char *suffix)
{
	snprintf(out, cap, "%s%s%s%s", dir,
#ifdef _WIN32
		 "\\",
#else
		 "/",
#endif
		 name, suffix ? suffix : "");
}

/* How much of this file we already hold, and the CRC of exactly that much. */
static uint32_t partial_state(const char *path, uint32_t *crc)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;

	uint32_t have = 0, running = ASP_CRC32_INIT;
	uint8_t buf[IO_CHUNK];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		running = asp_crc32(running, buf, n);
		have += (uint32_t)n;
	}
	fclose(f);
	*crc = running;
	return have;
}

static bool io_offer(void *ctx, const asp_offer *o, uint32_t *have,
		     uint32_t *prefix_crc)
{
	asp_app *a = ctx;

	/* The session has already sanitised the name into its own buffer; this
	 * is the same value, and it is what every path here is built from. */
	char safe[ASP_MAX_NAME + 1];
	if (!asp_safe_name(o->name, safe, sizeof safe))
		return false;

	join(a->part_path, sizeof a->part_path, a->recv_dir, safe, ".part");
	join(a->final_path, sizeof a->final_path, a->recv_dir, safe, NULL);

	*have = partial_state(a->part_path, prefix_crc);

	if (a->recv_fp) {
		fclose(a->recv_fp);
		a->recv_fp = NULL;
	}
	/* Append, so a resume continues the partial rather than replacing it.
	 * If the sender's START says zero, io_truncate_file reopens for write. */
	a->recv_fp = fopen(a->part_path, *have ? "ab" : "wb");
	if (!a->recv_fp) {
		note(a, "cannot open the receive file");
		return false;
	}

	if (a->hooks.offer_arrived)
		a->hooks.offer_arrived(a->hooks.ctx, o, safe, *have);

	return a->auto_accept;
}

static void io_done(void *ctx, bool inbound, uint16_t id, asp_result_code result)
{
	asp_app *a = ctx;
	(void)id;
	const char *path = NULL;

	if (inbound) {
		if (a->recv_fp) {
			fclose(a->recv_fp);
			a->recv_fp = NULL;
		}
		if (result == ASP_RESULT_OK) {
			/* Only now does it lose the .part suffix. A file that
			 * appears under its real name is one the operator can
			 * assume is whole. */
			if (ardop_replace_file(a->part_path, a->final_path))
				path = a->final_path;
			else
				note(a, "received, but could not be renamed");
		}
		/* On failure the partial is kept: §8 says a mismatch is
		 * resumable, and throwing it away would make the next attempt
		 * start from zero for no reason. */
	} else {
		if (a->send_fp) {
			fclose(a->send_fp);
			a->send_fp = NULL;
		}
	}

	if (a->hooks.transfer_done)
		a->hooks.transfer_done(a->hooks.ctx, inbound, result, path);
}

/* --- lifecycle -------------------------------------------------------------- */

bool asp_app_open(asp_app *a, app_spine *spine, const char *my_call,
		  const char *recv_dir, const asp_app_hooks *hooks)
{
	memset(a, 0, sizeof *a);
	a->spine = spine;
	if (hooks)
		a->hooks = *hooks;
	snprintf(a->my_call, sizeof a->my_call, "%s", my_call ? my_call : "");
	snprintf(a->recv_dir, sizeof a->recv_dir, "%s", recv_dir ? recv_dir : ".");

	if (!ardop_mkdir_p(a->recv_dir))
		return false;

	a->io = (asp_io){.ctx = a,
			 .send = io_send,
			 .credit = io_credit,
			 .read_file = io_read_file,
			 .write_file = io_write_file,
			 .offer_arrived = io_offer,
			 .truncate_file = io_truncate_file,
			 .text_arrived = io_text,
			 .transfer_done = io_done,
			 .progress = io_progress,
			 .protocol_error = io_error};

	asp_open(&a->session, &a->io, a->my_call);
	a->last_state = a->session.state;
	return true;
}

void asp_app_close(asp_app *a)
{
	asp_close(&a->session);
	if (a->send_fp) {
		fclose(a->send_fp);
		a->send_fp = NULL;
	}
	if (a->recv_fp) {
		/* Left on disk under .part, which is the whole resume story. */
		fclose(a->recv_fp);
		a->recv_fp = NULL;
	}
}

void asp_app_rx(asp_app *a, const char *tag, const void *data, size_t len)
{
	/* §1: only ARQ-tagged payload is protocol. An ERR marker or a station ID
	 * parsed as a message is how apps/ardop_rx.c corrupts files. */
	if (!tag || strcmp(tag, "ARQ") != 0)
		return;

	asp_recv(&a->session, data, len);

	if (a->session.state != a->last_state) {
		a->last_state = a->session.state;
		if (a->hooks.link_changed)
			a->hooks.link_changed(a->hooks.ctx, a->session.state,
					      a->session.peer_call);
	}
}

void asp_app_service(asp_app *a)
{
	asp_service(&a->session);
}

bool asp_app_send_text(asp_app *a, const char *text)
{
	return asp_send_text(&a->session, text, strlen(text));
}

bool asp_app_send_file(asp_app *a, const char *path)
{
	if (a->send_fp)
		return false;

	FILE *f = fopen(path, "rb");
	if (!f) {
		note(a, "cannot open that file");
		return false;
	}

	/* The whole file, once, for the CRC. There is no way to fill in OFFER
	 * without it, and doing it now means the receiver's answer is
	 * meaningful the moment it arrives.
	 *
	 * Counted in 64 bits even though the wire field is 32, so that a file
	 * too large to offer is *detected* rather than wrapped. */
	uint32_t crc = ASP_CRC32_INIT;
	uint64_t size = 0;
	uint8_t buf[IO_CHUNK];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		crc = asp_crc32(crc, buf, n);
		size += n;
	}
	if (size == 0) {
		fclose(f);
		note(a, "that file is empty");
		return false;
	}
	if (size > ASP_APP_MAX_FILE) {
		fclose(f);
		note(a, "that file is too large to offer; see ASP_APP_MAX_FILE");
		return false;
	}

	/* Our own name is reduced too, so we never offer a peer something we
	 * would refuse ourselves. */
	char safe[ASP_MAX_NAME + 1];
	if (!asp_safe_name(path, safe, sizeof safe)) {
		fclose(f);
		note(a, "that filename cannot be sent");
		return false;
	}

	a->send_fp = f;
	snprintf(a->send_name, sizeof a->send_name, "%s", safe);

	if (!asp_offer_file(&a->session, safe, NULL, (uint32_t)size, crc)) {
		fclose(a->send_fp);
		a->send_fp = NULL;
		return false;
	}
	return true;
}

bool asp_app_answer(asp_app *a, bool accept)
{
	uint32_t have = 0, crc = ASP_CRC32_INIT;
	if (accept)
		have = partial_state(a->part_path, &crc);
	return asp_answer_offer(&a->session, accept, have, crc,
				ASP_REJECT_REFUSED);
}

bool asp_app_cancel(asp_app *a, bool inbound)
{
	return asp_cancel(&a->session, inbound, ASP_REJECT_REFUSED);
}
