#include "shell/ring.h"

#include <string.h>

/**
 * @file ring.c
 * @brief The SPSC sample ring (see ring.h).
 *
 * The ordering rule, in full:
 *
 *   - The producer reads `r` with *acquire* and publishes `w` with *release*.
 *     The release pairs with the consumer's acquire on `w`, so the samples the
 *     producer memcpy'd are visible to the consumer before the count that
 *     advertises them.
 *   - The consumer does the mirror image.
 *   - Each side reads *its own* counter relaxed: nobody else writes it.
 */

bool ardop_ring_init(ardop_ring *rg, int16_t *storage, size_t cap)
{
	if (!rg || !storage || cap == 0)
		return false;
	rg->buf = storage;
	rg->cap = cap;
	atomic_store_explicit(&rg->w, 0, memory_order_relaxed);
	atomic_store_explicit(&rg->r, 0, memory_order_relaxed);
	return true;
}

size_t ardop_ring_write(ardop_ring *rg, const int16_t *src, size_t n)
{
	size_t w = atomic_load_explicit(&rg->w, memory_order_relaxed);
	size_t r = atomic_load_explicit(&rg->r, memory_order_acquire);

	size_t space = rg->cap - (w - r);
	if (n > space)
		n = space;
	if (n == 0)
		return 0;

	/* Up to two runs: to the end of the buffer, then from the start. */
	size_t off = w % rg->cap;
	size_t first = rg->cap - off;
	if (first > n)
		first = n;
	memcpy(rg->buf + off, src, first * sizeof(*src));
	if (n > first)
		memcpy(rg->buf, src + first, (n - first) * sizeof(*src));

	atomic_store_explicit(&rg->w, w + n, memory_order_release);
	return n;
}

size_t ardop_ring_read(ardop_ring *rg, int16_t *dst, size_t n)
{
	size_t r = atomic_load_explicit(&rg->r, memory_order_relaxed);
	size_t w = atomic_load_explicit(&rg->w, memory_order_acquire);

	size_t avail = w - r;
	if (n > avail)
		n = avail;
	if (n == 0)
		return 0;

	size_t off = r % rg->cap;
	size_t first = rg->cap - off;
	if (first > n)
		first = n;
	memcpy(dst, rg->buf + off, first * sizeof(*dst));
	if (n > first)
		memcpy(dst + first, rg->buf, (n - first) * sizeof(*dst));

	atomic_store_explicit(&rg->r, r + n, memory_order_release);
	return n;
}

size_t ardop_ring_avail(ardop_ring *rg)
{
	size_t w = atomic_load_explicit(&rg->w, memory_order_acquire);
	size_t r = atomic_load_explicit(&rg->r, memory_order_acquire);
	return w - r;
}

size_t ardop_ring_space(ardop_ring *rg)
{
	return rg->cap - ardop_ring_avail(rg);
}

void ardop_ring_reset(ardop_ring *rg)
{
	atomic_store_explicit(&rg->r, 0, memory_order_relaxed);
	atomic_store_explicit(&rg->w, 0, memory_order_relaxed);
}
