#include "shell/backend_null.h"

#include <stdio.h>
#include <string.h>

/**
 * @file backend_null.c
 * @brief The device-free backend (see backend_null.h).
 */

static size_t null_read_audio(void *ctx, int16_t *buf, size_t max)
{
	ardop_null_backend *nb = ctx;
	memset(buf, 0, max * sizeof(*buf));   /* silence. */
	nb->elapsed += max;
	return max;
}

static void null_write_audio(void *ctx, const int16_t *buf, size_t n)
{
	(void)buf;
	((ardop_null_backend *)ctx)->tx_samples += n;
}

static void null_set_ptt(void *ctx, bool key)
{
	ardop_null_backend *nb = ctx;
	nb->ptt = key;
	nb->ptt_edges++;
	if (nb->verbose)
		fprintf(stderr, "[ptt] %s\n", key ? "key" : "unkey");
}

static bool null_should_stop(void *ctx)
{
	ardop_null_backend *nb = ctx;
	return nb->stop_after != 0 && nb->elapsed >= nb->stop_after;
}

void ardop_backend_null_init(ardop_null_backend *nb, ardop_platform_ops *ops,
			     uint64_t stop_after)
{
	memset(nb, 0, sizeof(*nb));
	nb->stop_after = stop_after;

	memset(ops, 0, sizeof(*ops));
	ops->ctx = nb;
	ops->read_audio = null_read_audio;
	ops->write_audio = null_write_audio;
	ops->set_ptt = null_set_ptt;
	ops->should_stop = null_should_stop;
	/* wall_ms and poll_host stay NULL: no host input, sample-derived clock. */
}
