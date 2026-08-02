#ifndef ARDOP_APP_RING_H_
#define ARDOP_APP_RING_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file ring.h
 * @brief A single-producer / single-consumer ring of variable-length records.
 *
 * The three queues that cross the modem thread
 * ([analysis/14](../analysis/14-station-application.md) Decision 2) are all this
 * one type: commands going down, display records and events coming up. They
 * differ only in what their producer does when a write is refused.
 *
 * The memory model is `shell/ring.h`'s, verbatim -- monotonic counters, producer
 * loads `r` acquire and stores `w` release, consumer mirrors it, each side reads
 * its own counter relaxed. What is different is the payload: that ring carries a
 * *stream* of samples and a short read is normal; this one carries *records* and
 * a short read would be corruption. So writes here are all-or-nothing and each
 * record is prefixed with its own length.
 *
 * This is a separate file from `shell/ring.h` rather than a generalisation of it
 * for three reasons, in order:
 *
 *   1. Record semantics are not something an element-type parameter gives you.
 *      Even a generic ring would need this length-prefix layer written on top,
 *      so genericising buys one file and costs another.
 *   2. Genericising over element size in C means `void *` plus an `elem_size`,
 *      which puts a multiply on the audio callback's hot path and loses
 *      `int16_t` type safety in `shell/backend_ma.c`.
 *   3. `shell/ring.c` sits inside the audio callback and is proved by
 *      `make test-ring-tsan`. Editing proven audio-path code for the benefit of
 *      a consumer that does not need the change is a bad trade.
 *
 * The length prefix is read back through the same wrapping copy as any record
 * body, so a header straddling the end of the buffer needs no special case.
 * `shell/telemetry_tcp.c:58-62` parses its length bytewise for exactly the want
 * of that; this ring does not have to.
 */
typedef struct {
	uint8_t *buf;       /**< Caller-owned storage, @ref cap bytes. */
	size_t cap;         /**< Capacity in bytes, including per-record headers. */
	_Atomic size_t w;   /**< Total bytes ever written (producer owns). */
	_Atomic size_t r;   /**< Total bytes ever read (consumer owns). */
} app_ring;

/** Bytes of overhead each record costs on top of its body. */
#define APP_RING_OVERHEAD 4u

/** The largest body a single record may carry, so the u32 prefix cannot lie. */
#define APP_RING_MAX_RECORD 0x00FFFFFFu

/**
 * @brief Bind @p storage to @p rg. Not thread-safe; call before either side runs.
 * @return false if @p storage is NULL or @p cap is 0.
 */
bool app_ring_init(app_ring *rg, uint8_t *storage, size_t cap);

/**
 * @brief Append one record, formed by concatenating @p a and @p b.
 *        **Producer thread only.**
 *
 * All-or-nothing: on refusal nothing is written and nothing already queued is
 * disturbed. In particular this ring never drops the *oldest* record to make
 * room, the way `shell/telemetry_tcp.c:54-72` does. That transport can, because
 * it is single-threaded; here the read index belongs exclusively to the consumer,
 * and a producer advancing it could resurrect bytes the consumer has already
 * copied but not yet published. The drop policy therefore lives at the call site:
 * a lossy queue counts the refusal and moves on, a lossless one treats it as a
 * fault.
 *
 * Two parts rather than one so a caller can prepend a fixed header to a borrowed
 * payload without a staging copy -- which matters, because every one of these
 * writes happens on the audio path. Either part may be NULL with a zero length.
 *
 * @param rg      Ring.
 * @param a       First part, or NULL when @p na is 0.
 * @param na      Bytes of @p a.
 * @param b       Second part, or NULL when @p nb is 0.
 * @param nb      Bytes of @p b.
 * @param reserve Bytes of headroom the write must leave free. Ordinary records
 *                pass a non-zero value so that a queue which has just filled can
 *                still accept the one record that says so; only that record
 *                passes 0.
 * @return true if the record was written.
 */
bool app_ring_write(app_ring *rg, const void *a, size_t na, const void *b,
		    size_t nb, size_t reserve);

/**
 * @brief Body length of the oldest record, or 0 when empty.
 *        **Consumer thread only.**
 *
 * For sizing a destination before app_ring_read(). A record whose body is
 * genuinely zero bytes long is indistinguishable from an empty ring here; no
 * caller in the tree writes one, and app_ring_read() reports the difference.
 */
size_t app_ring_peek(app_ring *rg);

/**
 * @brief Remove the oldest record and copy its body to @p dst.
 *        **Consumer thread only.**
 *
 * A record too large for @p cap is **discarded** rather than left in place: it
 * can never be delivered, and leaving it would wedge the queue permanently. That
 * is reported as a 0 return, same as empty -- use app_ring_peek() first if the
 * two need telling apart.
 *
 * @return Bytes copied; 0 when the ring is empty or the record did not fit.
 */
size_t app_ring_read(app_ring *rg, void *dst, size_t cap);

/** @brief Bytes queued now, headers included. Safe from either side (a lower
 *         bound to the producer, exact to the consumer).
 *
 * Not `const`-qualified: C11's atomic_load_explicit takes a non-const pointer,
 * so a const parameter would only be discarded again inside. */
size_t app_ring_used(app_ring *rg);

/** @brief Bytes free now, of which any record costs its body plus
 *         ::APP_RING_OVERHEAD. */
size_t app_ring_space(app_ring *rg);

/**
 * @brief Discard everything. Precondition: both sides are quiesced.
 */
void app_ring_reset(app_ring *rg);

#endif /* ARDOP_APP_RING_H_ */
