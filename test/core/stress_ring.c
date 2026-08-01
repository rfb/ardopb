#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "shell/ring.h"

/**
 * @file stress_ring.c
 * @brief Two-thread ThreadSanitizer stress for the SPSC ring.
 *
 * The single-threaded contract is covered by `test/core/test_ring.c`. What that
 * cannot show is the property the ring exists for: that the producer's release
 * on `w` makes the samples it copied visible to the consumer *before* the count
 * advertising them. Get that wrong and the audio callback hands the modem
 * uninitialised memory -- occasionally, on one CPU architecture, under load.
 *
 * MinGW has no ThreadSanitizer. But nothing in ring.c is platform-specific, so
 * proving the ordering once on Linux proves it for the Windows build too --
 * exactly the argument `make check-pure` makes about ELF.
 *
 * Not part of `make test-core`: it wants -fsanitize=thread on the whole link,
 * which the shared test rule does not do. Run it with `make test-ring-tsan`.
 *
 * The payload is a counter rather than a constant so that a torn or reordered
 * publish shows up as a value mismatch, not just as a TSan report -- the test
 * fails even in a build without the sanitizer.
 */

#define CAP 512
#define TOTAL 4000000

static int16_t storage[CAP];
static ardop_ring rg;

static void *producer(void *arg)
{
	(void)arg;
	int16_t buf[97];   /* deliberately coprime with CAP, to hit every offset */
	uint32_t n = 0;
	while (n < TOTAL) {
		size_t chunk = (size_t)(n % 97u) + 1u;
		for (size_t i = 0; i < chunk; i++)
			buf[i] = (int16_t)((n + (uint32_t)i) & 0x7FFF);
		size_t wrote = ardop_ring_write(&rg, buf, chunk);
		n += (uint32_t)wrote;
	}
	return NULL;
}

int main(void)
{
	if (!ardop_ring_init(&rg, storage, CAP)) {
		fprintf(stderr, "ring init failed\n");
		return 1;
	}

	pthread_t th;
	if (pthread_create(&th, NULL, producer, NULL) != 0) {
		fprintf(stderr, "pthread_create failed\n");
		return 1;
	}

	int16_t buf[131];
	uint32_t n = 0;
	while (n < TOTAL) {
		size_t got = ardop_ring_read(&rg, buf, sizeof(buf) / sizeof(*buf));
		for (size_t i = 0; i < got; i++) {
			int16_t want = (int16_t)((n + (uint32_t)i) & 0x7FFF);
			if (buf[i] != want) {
				fprintf(stderr,
					"corrupt at %u: got %d want %d\n",
					n + (uint32_t)i, buf[i], want);
				return 1;
			}
		}
		n += (uint32_t)got;
	}

	pthread_join(th, NULL);
	printf("stress_ring: %d samples through a %d-sample ring, in order\n",
	       TOTAL, CAP);
	return 0;
}
