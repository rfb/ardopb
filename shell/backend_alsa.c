#include "shell/backend_alsa.h"

#include <alloca.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "shell/ptt.h"

/**
 * @file backend_alsa.c
 * @brief The ALSA backend (see backend_alsa.h).
 *
 * NOTE: audio and PTT here talk to real hardware, so this file is exercised on a
 * radio, not in the in-process test suite (which uses the fake platform). It is
 * held to the same warning bar as the core (-Wall -Wextra -Werror) and kept
 * deliberately small; the protocol correctness is proven above it, device-free.
 */

#define ARDOP_RATE 12000

struct ardop_alsa_backend {
	snd_pcm_t *capture;
	snd_pcm_t *playback;
	ardop_ptt *ptt;    /* borrowed; may be NULL (VOX). */
	bool keyed;
};

/* Configure one PCM for 12000 Hz mono S16_LE. Returns 0 on success. */
static int configure(snd_pcm_t *pcm, const char *what)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);

	int err = snd_pcm_hw_params_any(pcm, hw);
	if (err < 0)
		goto fail;
	err = snd_pcm_hw_params_set_access(pcm, hw,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
		goto fail;
	err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	if (err < 0)
		goto fail;
	err = snd_pcm_hw_params_set_channels(pcm, hw, 1);
	if (err < 0)
		goto fail;
	unsigned int rate = ARDOP_RATE;
	err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
	if (err < 0)
		goto fail;
	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0)
		goto fail;
	return 0;
fail:
	fprintf(stderr, "alsa: %s config: %s\n", what, snd_strerror(err));
	return err;
}

/*
 * PTT used to be a serial fd right here, toggling RTS with TIOCMGET/TIOCMSET.
 * It moved to shell/ptt.c when the miniaudio backend arrived: two backends each
 * carrying their own copy of serial keying is precisely the "a bug reproduces
 * on one and not the other" problem analysis/15's own open questions worry
 * about. This is a deliberate amendment to that document's §9, which lists this
 * file as unchanged -- it could not be.
 *
 * What stays here is the part only this file knows: draining the ALSA buffer
 * before the line drops, so the tail of a transmission is not cut.
 */

/* --- platform ops -------------------------------------------------------- */

static size_t alsa_read_audio(void *ctx, int16_t *buf, size_t max)
{
	struct ardop_alsa_backend *ab = ctx;
	snd_pcm_sframes_t got = snd_pcm_readi(ab->capture, buf,
					      (snd_pcm_uframes_t)max);
	if (got == -EPIPE) {
		snd_pcm_prepare(ab->capture);   /* recover from overrun. */
		return 0;
	}
	if (got < 0) {
		got = snd_pcm_recover(ab->capture, (int)got, 1);
		return 0;
	}
	return (size_t)got;
}

static void alsa_write_audio(void *ctx, const int16_t *buf, size_t n)
{
	struct ardop_alsa_backend *ab = ctx;
	while (n > 0) {
		snd_pcm_sframes_t put = snd_pcm_writei(ab->playback, buf,
						       (snd_pcm_uframes_t)n);
		if (put == -EPIPE) {
			snd_pcm_prepare(ab->playback);   /* underrun. */
			continue;
		}
		if (put < 0) {
			put = snd_pcm_recover(ab->playback, (int)put, 1);
			if (put < 0)
				return;
			continue;
		}
		buf += put;
		n -= (size_t)put;
	}
}

static void alsa_set_ptt(void *ctx, bool key)
{
	struct ardop_alsa_backend *ab = ctx;
	ab->keyed = key;
	if (!key)
		snd_pcm_drain(ab->playback);   /* flush before unkeying. */
	ardop_ptt_set(ab->ptt, key);
	if (key)
		snd_pcm_prepare(ab->playback);
}

ardop_alsa_backend *ardop_backend_alsa_open(const char *capture_dev,
					    const char *playback_dev,
					    ardop_ptt *ptt,
					    ardop_platform_ops *ops)
{
	struct ardop_alsa_backend *ab = calloc(1, sizeof(*ab));
	if (!ab)
		return NULL;
	ab->ptt = ptt;

	int err = snd_pcm_open(&ab->capture, capture_dev,
			       SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		fprintf(stderr, "alsa: open capture %s: %s\n", capture_dev,
			snd_strerror(err));
		goto fail;
	}
	err = snd_pcm_open(&ab->playback, playback_dev,
			   SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "alsa: open playback %s: %s\n", playback_dev,
			snd_strerror(err));
		goto fail;
	}
	if (configure(ab->capture, "capture") < 0
	    || configure(ab->playback, "playback") < 0)
		goto fail;
	snd_pcm_prepare(ab->capture);
	snd_pcm_start(ab->capture);

	memset(ops, 0, sizeof(*ops));
	ops->ctx = ab;
	ops->read_audio = alsa_read_audio;
	ops->write_audio = alsa_write_audio;
	ops->set_ptt = alsa_set_ptt;
	return ab;
fail:
	ardop_backend_alsa_close(ab);
	return NULL;
}

void ardop_backend_alsa_close(ardop_alsa_backend *ab)
{
	if (!ab)
		return;
	if (ab->capture)
		snd_pcm_close(ab->capture);
	if (ab->playback)
		snd_pcm_close(ab->playback);
	free(ab);
}
