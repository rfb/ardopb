#ifndef ARDOP_SHELL_RING_H_
#define ARDOP_SHELL_RING_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file ring.h
 * @brief A single-producer / single-consumer sample ring.
 *
 * This is the bridge between an OS audio callback and the modem thread. Every
 * platform audio API except ALSA's blocking read/write *pushes* -- CoreAudio,
 * AAudio and WASAPI all call you from their own thread -- while
 * ::ardop_platform_ops is a *pull* interface. One of these rings sits in each
 * direction ([analysis/15](../analysis/15-platform-audio-and-ptt.md) §3).
 *
 * Exactly one thread may write and exactly one thread may read; that is what
 * makes this lock-free without a CAS. The audio callback must never block, so
 * there is no mutex here and no allocation: storage is caller-provided and the
 * backend allocates it once, at open.
 *
 * The counters are monotonically increasing rather than wrapped indices, so
 * "full" and "empty" are distinguishable without sacrificing a slot. They wrap
 * at SIZE_MAX, which unsigned arithmetic handles correctly as long as @ref
 * ardop_ring::cap is under SIZE_MAX/2 -- at 12 kHz, SIZE_MAX/2 samples is over
 * a million years on 64-bit and a little under 24 hours on 32-bit, and the
 * difference `w - r` is bounded by `cap` regardless, so the wrap is benign.
 *
 * MinGW has no ThreadSanitizer, but nothing in this file is platform-specific,
 * so `make test-ring-tsan` proves the ordering on Linux and the property
 * carries -- the same argument `check-pure` makes.
 */
typedef struct {
	int16_t *buf;       /**< Caller-owned storage, @ref cap samples. */
	size_t cap;         /**< Capacity in samples. */
	_Atomic size_t w;   /**< Total samples ever written (producer owns). */
	_Atomic size_t r;   /**< Total samples ever read (consumer owns). */
} ardop_ring;

/**
 * @brief Bind @p storage to @p rg. Not thread-safe; call before either side runs.
 * @return false if @p storage is NULL or @p cap is 0.
 */
bool ardop_ring_init(ardop_ring *rg, int16_t *storage, size_t cap);

/**
 * @brief Append up to @p n samples. **Producer thread only.**
 * @return Samples actually written (< @p n when the ring is full).
 */
size_t ardop_ring_write(ardop_ring *rg, const int16_t *src, size_t n);

/**
 * @brief Remove up to @p n samples. **Consumer thread only.**
 * @return Samples actually read (< @p n when the ring is empty).
 */
size_t ardop_ring_read(ardop_ring *rg, int16_t *dst, size_t n);

/** @brief Samples readable now. Safe from either side (a lower bound to the
 *         producer, exact to the consumer).
 *
 * Not `const`-qualified: C11's atomic_load_explicit takes a non-const pointer,
 * so a const parameter would only be discarded again inside. */
size_t ardop_ring_avail(ardop_ring *rg);

/** @brief Samples writable now. Safe from either side (a lower bound to the
 *         consumer, exact to the producer). */
size_t ardop_ring_space(ardop_ring *rg);

/**
 * @brief Discard everything.
 *
 * Precondition: both sides are quiesced. The backend calls this at PTT edges,
 * where the audio callback is stopped or known to be discarding.
 */
void ardop_ring_reset(ardop_ring *rg);

#endif /* ARDOP_SHELL_RING_H_ */
