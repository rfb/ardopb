#include "app/ring.h"

#include <string.h>

/**
 * @file ring.c
 * @brief The SPSC record ring (see ring.h).
 *
 * The ordering rule is `shell/ring.c`'s, and for the same reason:
 *
 *   - The producer reads `r` with *acquire* and publishes `w` with *release*.
 *     The release pairs with the consumer's acquire on `w`, so the bytes the
 *     producer copied are visible to the consumer before the count that
 *     advertises them.
 *   - The consumer does the mirror image.
 *   - Each side reads *its own* counter relaxed: nobody else writes it.
 *
 * The one thing added here is atomicity of a whole record. The producer computes
 * the space it needs *before* copying anything and publishes `w` exactly once, at
 * the end, so a consumer either sees the entire record or none of it. There is no
 * intermediate state in which a length prefix is visible but its body is not.
 */

/* Copy @p n bytes into the ring at byte offset @p at (an absolute counter),
 * wrapping. Does not touch either counter -- the caller publishes. */
static void put(app_ring *rg, size_t at, const void *src, size_t n)
{
	if (n == 0)
		return;
	size_t off = at % rg->cap;
	size_t first = rg->cap - off;
	if (first > n)
		first = n;
	memcpy(rg->buf + off, src, first);
	if (n > first)
		memcpy(rg->buf, (const uint8_t *)src + first, n - first);
}

/* The mirror of put(): copy @p n bytes out from absolute offset @p at. */
static void get(app_ring *rg, size_t at, void *dst, size_t n)
{
	if (n == 0)
		return;
	size_t off = at % rg->cap;
	size_t first = rg->cap - off;
	if (first > n)
		first = n;
	memcpy(dst, rg->buf + off, first);
	if (n > first)
		memcpy((uint8_t *)dst + first, rg->buf, n - first);
}

bool app_ring_init(app_ring *rg, uint8_t *storage, size_t cap)
{
	if (!rg || !storage || cap == 0)
		return false;
	rg->buf = storage;
	rg->cap = cap;
	atomic_store_explicit(&rg->w, 0, memory_order_relaxed);
	atomic_store_explicit(&rg->r, 0, memory_order_relaxed);
	return true;
}

bool app_ring_write(app_ring *rg, const void *a, size_t na, const void *b,
		    size_t nb, size_t reserve)
{
	size_t body = na + nb;
	if (body < na || body > APP_RING_MAX_RECORD)
		return false;               /* overflowed, or too big to prefix */

	size_t need = body + APP_RING_OVERHEAD;
	if (need < body)
		return false;

	size_t w = atomic_load_explicit(&rg->w, memory_order_relaxed);
	size_t r = atomic_load_explicit(&rg->r, memory_order_acquire);

	size_t space = rg->cap - (w - r);
	if (space < reserve || space - reserve < need)
		return false;

	uint8_t hdr[APP_RING_OVERHEAD];
	uint32_t len = (uint32_t)body;
	hdr[0] = (uint8_t)(len & 0xFFu);
	hdr[1] = (uint8_t)((len >> 8) & 0xFFu);
	hdr[2] = (uint8_t)((len >> 16) & 0xFFu);
	hdr[3] = (uint8_t)((len >> 24) & 0xFFu);

	put(rg, w, hdr, sizeof hdr);
	put(rg, w + APP_RING_OVERHEAD, a, na);
	put(rg, w + APP_RING_OVERHEAD + na, b, nb);

	/* One release, after every byte is in place: the record becomes visible
	 * whole or not at all. */
	atomic_store_explicit(&rg->w, w + need, memory_order_release);
	return true;
}

/* Read the oldest record's length prefix. False when the ring holds no complete
 * header, which for a well-formed ring means it is empty. */
static bool peek_len(app_ring *rg, size_t r, size_t avail, size_t *body)
{
	if (avail < APP_RING_OVERHEAD)
		return false;

	uint8_t hdr[APP_RING_OVERHEAD];
	get(rg, r, hdr, sizeof hdr);

	uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
		       ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);

	/* A record the ring does not wholly contain cannot happen: the producer
	 * publishes header and body in one release. Refuse rather than trust it,
	 * because the alternative is a wild memcpy. */
	if ((size_t)len > avail - APP_RING_OVERHEAD)
		return false;

	*body = (size_t)len;
	return true;
}

size_t app_ring_peek(app_ring *rg)
{
	size_t r = atomic_load_explicit(&rg->r, memory_order_relaxed);
	size_t w = atomic_load_explicit(&rg->w, memory_order_acquire);

	size_t body;
	if (!peek_len(rg, r, w - r, &body))
		return 0;
	return body;
}

size_t app_ring_read(app_ring *rg, void *dst, size_t cap)
{
	size_t r = atomic_load_explicit(&rg->r, memory_order_relaxed);
	size_t w = atomic_load_explicit(&rg->w, memory_order_acquire);

	size_t body;
	if (!peek_len(rg, r, w - r, &body))
		return 0;

	size_t taken = body;
	if (taken > cap) {
		/* Undeliverable. Drop it rather than block the queue behind it --
		 * see ring.h. */
		taken = 0;
	} else {
		get(rg, r + APP_RING_OVERHEAD, dst, body);
	}

	atomic_store_explicit(&rg->r, r + APP_RING_OVERHEAD + body,
			      memory_order_release);
	return taken;
}

size_t app_ring_used(app_ring *rg)
{
	size_t w = atomic_load_explicit(&rg->w, memory_order_acquire);
	size_t r = atomic_load_explicit(&rg->r, memory_order_acquire);
	return w - r;
}

size_t app_ring_space(app_ring *rg)
{
	return rg->cap - app_ring_used(rg);
}

void app_ring_reset(app_ring *rg)
{
	atomic_store_explicit(&rg->r, 0, memory_order_relaxed);
	atomic_store_explicit(&rg->w, 0, memory_order_relaxed);
}
