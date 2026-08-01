#include "shell/loop.h"

#include <string.h>

/**
 * @file loop.c
 * @brief The single-clocked driver loop (see loop.h).
 */

void ardop_loop_init(ardop_loop *lp, ardop_runtime *rt,
		     const ardop_platform_ops *plat)
{
	memset(lp, 0, sizeof(*lp));
	lp->rt = rt;
	lp->plat = plat;
	lp->block = ARDOP_LOOP_BLOCK;
}

/* Drain a whole transmission to the output, then poll for host input. Called
 * only while the runtime is transmitting. Advances the clock by the samples
 * played, so the link's sample-counted timers keep running across the burst. */
static void serve_tx(ardop_loop *lp)
{
	size_t n = ardop_runtime_pull_tx(lp->rt, lp->out, lp->block);
	if (n == 0)
		return;   /* just finished; PTT was dropped inside pull_tx. */
	lp->t += n;
	lp->rt->now = lp->t;
	if (lp->plat->write_audio)
		lp->plat->write_audio(lp->plat->ctx, lp->out, n);
}

/* Capture one block and step the link: deliver host commands, feed samples,
 * service timers. Called while idle (not transmitting). */
static void serve_rx(ardop_loop *lp)
{
	ardop_host_cmd cmd;
	while (lp->plat->poll_host
	       && lp->plat->poll_host(lp->plat->ctx, &cmd))
		ardop_runtime_host(lp->rt, &cmd, lp->t);

	size_t n = lp->plat->read_audio(lp->plat->ctx, lp->in, lp->block);
	lp->t += n;
	if (n > 0) {
		ardop_runtime_telemetry_audio(lp->rt, lp->in, n);
		ardop_runtime_rx(lp->rt, lp->in, n, lp->t);
	}
	ardop_runtime_timer(lp->rt, lp->t);
}

void ardop_loop_step(ardop_loop *lp)
{
	if (ardop_runtime_tx_active(lp->rt))
		serve_tx(lp);
	else
		serve_rx(lp);
}

void ardop_loop_run(ardop_loop *lp)
{
	while (!(lp->plat->should_stop
		 && lp->plat->should_stop(lp->plat->ctx)))
		ardop_loop_step(lp);
}
