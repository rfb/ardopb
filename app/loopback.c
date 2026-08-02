#include "app/loopback.h"

#include <stdlib.h>
#include <string.h>

#include "shell/loop.h"

/**
 * @file loopback.c
 * @brief The in-memory two-station channel (see loopback.h).
 */

/*
 * One direction of the channel: a circular FIFO of samples.
 *
 * Circular rather than the linear buffer test/core/test_loop.c uses, because
 * this one has to survive a transfer of arbitrary length. A linear buffer sized
 * for a megabyte of audio holds 83 seconds and then stops, which is shorter than
 * the file transfers this harness exists to exercise.
 */
#define AIR_SAMPLES (256u * 1024u)

struct air {
	int16_t s[AIR_SAMPLES];
	size_t head;   /* next sample to read */
	size_t len;    /* samples queued */
	uint64_t overruns;
};

static void air_write(struct air *a, const int16_t *b, size_t n)
{
	if (n > AIR_SAMPLES - a->len) {
		size_t lost = n - (AIR_SAMPLES - a->len);
		a->overruns += lost;
		n -= lost;
	}
	size_t tail = (a->head + a->len) % AIR_SAMPLES;
	size_t first = AIR_SAMPLES - tail;
	if (first > n)
		first = n;
	memcpy(a->s + tail, b, first * sizeof(*b));
	if (n > first)
		memcpy(a->s, b + first, (n - first) * sizeof(*b));
	a->len += n;
}

static size_t air_read(struct air *a, int16_t *b, size_t max)
{
	size_t n = a->len < max ? a->len : max;
	size_t first = AIR_SAMPLES - a->head;
	if (first > n)
		first = n;
	memcpy(b, a->s + a->head, first * sizeof(*b));
	if (n > first)
		memcpy(b + first, a->s, (n - first) * sizeof(*b));
	a->head = (a->head + n) % AIR_SAMPLES;
	a->len -= n;
	return n;
}

struct side {
	app_spine *sp;
	ardop_platform_ops ops;
	struct air *tx;   /* write_audio deposits here */
	struct air *rx;   /* read_audio draws from here */
	bool ptt;
	struct side *peer;      /* who hears what we write */
	uint64_t collided;      /* samples destroyed by a collision */
};

struct app_loopback {
	struct side side[2];
	struct air air[2];
	uint64_t elapsed;
};

/* Always a full block, padded with silence.
 *
 * This is what makes the clock tick uniformly. Returning only what is queued
 * would make time advance faster while a station is talking than while it is
 * listening, and every protocol timeout is measured in samples. */
static size_t read_audio(void *ctx, int16_t *buf, size_t max)
{
	struct side *s = ctx;
	size_t n = air_read(s->rx, buf, max);
	if (n < max)
		memset(buf + n, 0, (max - n) * sizeof(*buf));
	return max;
}

/*
 * Deposit into the air the peer reads -- unless the peer is transmitting.
 *
 * ARDOP is half duplex: a station with its PTT up is deaf, and two stations
 * transmitting at once is a collision in which neither hears the other. This
 * harness used to queue both transmissions and deliver them intact a moment
 * later, which turns a collision into a delayed-but-perfect exchange. That is
 * not a conservative simplification -- it is a channel no pair of radios can
 * meet, and it let the two ends' state machines take sequences that cannot
 * happen on the air.
 *
 * Dropping the samples is the honest model, and it is what makes ARQ do the job
 * it exists for: the frame fails to arrive, the sender repeats it, and the
 * exchange recovers exactly as it would on a real channel.
 */
static void write_audio(void *ctx, const int16_t *buf, size_t n)
{
	struct side *s = ctx;
	if (s->peer && s->peer->ptt) {
		s->collided += n;
		return;
	}
	air_write(s->tx, buf, n);
}

static void set_ptt(void *ctx, bool key)
{
	((struct side *)ctx)->ptt = key;
}

app_loopback *app_loopback_open(const app_config *cfg)
{
	app_loopback *lb = calloc(1, sizeof *lb);
	if (!lb)
		return NULL;

	for (int i = 0; i < 2; i++) {
		struct side *s = &lb->side[i];
		s->sp = app_open(cfg);
		if (!s->sp) {
			app_loopback_close(lb);
			return NULL;
		}
		s->tx = &lb->air[i];
		s->rx = &lb->air[1 - i];
		s->ops.ctx = s;
		s->ops.read_audio = read_audio;
		s->ops.write_audio = write_audio;
		s->ops.set_ptt = set_ptt;
		app_set_platform(s->sp, &s->ops, 0);
	}
	/* After both exist: each needs to know who is listening to it. */
	lb->side[0].peer = &lb->side[1];
	lb->side[1].peer = &lb->side[0];
	return lb;
}

void app_loopback_close(app_loopback *lb)
{
	if (!lb)
		return;
	for (int i = 0; i < 2; i++)
		app_close(lb->side[i].sp);
	free(lb);
}

app_spine *app_loopback_side(app_loopback *lb, int which)
{
	if (!lb || which < 0 || which > 1)
		return NULL;
	return lb->side[which].sp;
}

void app_loopback_step(app_loopback *lb)
{
	app_step(lb->side[0].sp);
	app_step(lb->side[1].sp);

	/* Both sides advance by one block per step whether transmitting or
	 * receiving, so either one reports the same elapsed time. */
	lb->elapsed += ARDOP_LOOP_BLOCK;
}

uint64_t app_loopback_elapsed(const app_loopback *lb)
{
	return lb->elapsed;
}

uint64_t app_loopback_overruns(const app_loopback *lb)
{
	return lb->air[0].overruns + lb->air[1].overruns;
}

uint64_t app_loopback_collisions(const app_loopback *lb)
{
	return lb->side[0].collided + lb->side[1].collided;
}
