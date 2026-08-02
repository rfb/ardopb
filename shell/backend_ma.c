#include "shell/backend_ma.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/audio_devices.h"
#include "shell/loop.h"
#include "shell/ma_common.h"
#include "shell/resample.h"
#include "shell/ring.h"
#include "shell/sys.h"

#include "modem/modulate.h"   /* ARDOP_MOD_SAMPLE_RATE */

/**
 * @file backend_ma.c
 * @brief The miniaudio backend (see backend_ma.h).
 *
 * ## The shape
 *
 * ::ardop_platform_ops is a *pull* interface. Every audio API except ALSA's
 * blocking read/write *pushes*, and so does miniaudio, which normalises them
 * all to a data callback on an OS-owned real-time thread. So there is a ring in
 * each direction (analysis/15 §3):
 *
 *     device callback  --write-->  capture ring  --drain-->  read_audio
 *     write_audio      --write-->  playback ring --drain-->  device callback
 *
 * **Both rings hold device-rate samples, and both resamplers run on the modem
 * thread.** A 97-tap FIR inside the audio callback is avoidable work on a
 * thread where a missed deadline is an audible underrun, and keeping
 * `resample.c` off that thread also keeps it a pure function that can be tested
 * on its own. The callbacks do nothing but memcpy and post a semaphore.
 *
 * The clock rule survives exactly. `read_audio` consumes precisely `k * m`
 * device samples and produces `k`, with no accumulator anywhere: if the card
 * runs at 47988 Hz instead of 48000, the modem clock runs at 11997 Hz and every
 * protocol deadline stretches by the same 0.025%. That is the property that
 * dissolved `FixTiming`, and it is why a non-integer ratio is refused outright.
 *
 * ## Three hazards, none of which fail loudly
 *
 * They are each commented at the code that handles them, but together:
 *
 *  1. `write_audio` returns as soon as bytes are *queued*, so unkeying when the
 *     loop finishes cuts the tail of every transmission (§set_ptt).
 *  2. The interpolator additionally holds (ntaps-1)/2 samples of group delay
 *     that have been accepted and not emitted -- a second, independent tail cut
 *     that analysis/15 does not mention (§set_ptt, step 1).
 *  3. ARDOP is half-duplex and the loop does not call `read_audio` during TX,
 *     so a capture ring quietly accumulates the station's own transmission and
 *     feeds it to the demodulator afterwards (§capture callback). ALSA escapes
 *     this only by accident, via overrun recovery discarding the backlog.
 */

#define ARDOP_RATE ARDOP_MOD_SAMPLE_RATE   /* 12000 */

/* How long read_audio waits for the capture callback before declaring the
 * device gone. 2.5x the default block: long enough not to trip on a scheduling
 * hiccup, short enough that an unplugged device surfaces within one block. */
#define READ_TIMEOUT_MS 250

/* How long set_ptt(false) waits for the playback ring to empty. Generous: a
 * long ARQ frame legitimately takes seconds to play out. */
#define DRAIN_TIMEOUT_MS 5000

/* Extra settle after the ring is empty, for samples already inside the driver
 * and for the radio's own TX-to-RX changeover. */
#define TAIL_GUARD_MS 20

struct ardop_ma_backend {
	ma_context ctx;
	ma_device capture;
	ma_device playback;
	bool capture_started;
	bool playback_started;

	unsigned m;              /**< device rate / 12000 */
	unsigned device_rate;
	size_t block;            /**< modem-rate samples per loop iteration. */
	unsigned period_ms;

	ardop_ring cap_ring;
	ardop_ring play_ring;
	int16_t *cap_store;
	int16_t *play_store;

	ardop_signal cap_sig;
	ardop_signal play_sig;

	ardop_resampler rx_rs;   /**< decimate: device -> modem */
	ardop_resampler tx_rs;   /**< interpolate: modem -> device */

	/* Scratch at device rate. ARDOP_LOOP_BLOCK modem samples at the highest
	 * supported ratio. Allocated once; the callbacks never touch these. */
	int16_t *scratch;
	size_t scratch_len;

	ardop_ptt *ptt;          /**< Borrowed. */

	/* How each selection resolved, and what was actually opened. The pair is
	 * what lets an application write the *new* id back after a renumber; with
	 * only the match, the rule would degrade to name-matching forever. */
	ardop_audio_match cap_match, play_match;
	ardop_audio_device cap_dev, play_dev;

	_Atomic bool tx_active;
	_Atomic int fault;       /**< ardop_fault, latched. */

	_Atomic uint_least64_t played;          /* device samples out */
	_Atomic uint_least64_t capture_dropped; /* discarded during TX */
	uint64_t played_at_unkey;
	uint64_t written;                       /* modem samples in, this over */
};

static void fault_set(ardop_ma_backend *b, ardop_fault f)
{
	int none = (int)ARDOP_FAULT_NONE;
	/* First fault wins: it is the one with the explanation. */
	atomic_compare_exchange_strong(&b->fault, &none, (int)f);
}

/* --- device callbacks (OS audio thread; no allocation, no locks) ----------- */

static void capture_cb(ma_device *dev, void *out, const void *in,
		       ma_uint32 frames)
{
	(void)out;
	ardop_ma_backend *b = dev->pUserData;
	if (!b || !in || frames == 0)
		return;

	/*
	 * Hazard 3. During transmit the loop never calls read_audio, so
	 * anything enqueued here is the station's own signal and would be fed
	 * to the demodulator, stale by the length of the transmission, the
	 * moment we go back to receive. Drop it at the source and count it.
	 */
	if (atomic_load_explicit(&b->tx_active, memory_order_acquire)) {
		atomic_fetch_add_explicit(&b->capture_dropped,
					  (uint_least64_t)frames,
					  memory_order_relaxed);
		return;
	}

	(void)ardop_ring_write(&b->cap_ring, (const int16_t *)in, frames);
	ardop_signal_post(&b->cap_sig);
}

static void playback_cb(ma_device *dev, void *out, const void *in,
			ma_uint32 frames)
{
	(void)in;
	ardop_ma_backend *b = dev->pUserData;
	if (!b || !out || frames == 0)
		return;

	int16_t *dst = out;
	size_t got = ardop_ring_read(&b->play_ring, dst, frames);
	if (got < frames) {
		/* Silence rather than stale audio. Not flagged as a fault: the
		 * device runs continuously and is idle most of the time, so an
		 * underrun outside a transmission is the normal state. */
		memset(dst + got, 0, (frames - got) * sizeof(*dst));
	}
	atomic_fetch_add_explicit(&b->played, (uint_least64_t)got,
				  memory_order_relaxed);
	ardop_signal_post(&b->play_sig);
}

/* --- platform ops (modem thread) ------------------------------------------- */

static size_t ma_read_audio(void *ctx, int16_t *buf, size_t max)
{
	ardop_ma_backend *b = ctx;
	if (max > b->block)
		max = b->block;

	/* Wait for at least one modem sample's worth of device samples. Blocking
	 * here is what paces the whole program -- returning 0 immediately is
	 * legal but spins the loop at 100% CPU, since it has no pacing of its
	 * own. The timeout is not decoration: a device that has been unplugged
	 * simply stops calling the callback, and without it the modem thread
	 * would hang forever with no way to tell the operator. */
	while (ardop_ring_avail(&b->cap_ring) < b->m) {
		if (!ardop_signal_wait(&b->cap_sig, READ_TIMEOUT_MS)) {
			fault_set(b, ARDOP_FAULT_CAPTURE_LOST);
			return 0;
		}
		if (atomic_load_explicit(&b->tx_active, memory_order_acquire))
			return 0;
	}

	size_t avail = ardop_ring_avail(&b->cap_ring);
	size_t k = avail / b->m;      /* whole modem samples only */
	if (k > max)
		k = max;
	if (k == 0)
		return 0;

	size_t want = k * b->m;
	size_t got = ardop_ring_read(&b->cap_ring, b->scratch, want);
	/* Exactly k*m in, exactly k out. No accumulator, ever: this is the
	 * identity that keeps protocol time locked to device time. */
	return ardop_resample(&b->rx_rs, b->scratch, got, buf);
}

static void ma_write_audio(void *ctx, const int16_t *buf, size_t n)
{
	ardop_ma_backend *b = ctx;
	b->written += n;

	while (n > 0) {
		size_t chunk = n;
		if (chunk > b->block)
			chunk = b->block;

		size_t nd = ardop_resample(&b->tx_rs, buf, chunk, b->scratch);
		size_t off = 0;
		uint64_t deadline = ardop_mono_ms() + DRAIN_TIMEOUT_MS;
		while (off < nd) {
			off += ardop_ring_write(&b->play_ring, b->scratch + off,
						nd - off);
			if (off < nd) {
				/* Ring full: the device has not consumed it
				 * yet. Blocking mirrors ALSA's blocking
				 * snd_pcm_writei and is what keeps transmit
				 * paced by the hardware. */
				if (!ardop_signal_wait(&b->play_sig, 100)
				    && ardop_mono_ms() > deadline) {
					fault_set(b, ARDOP_FAULT_PLAYBACK_LOST);
					return;
				}
			}
		}
		buf += chunk;
		n -= chunk;
	}
}

static void ma_set_ptt(void *ctx, bool key)
{
	ardop_ma_backend *b = ctx;

	if (key) {
		/* Discard anything stale before keying, so the first thing
		 * radiated is the leader and not the tail of a previous over. */
		ardop_ring_reset(&b->play_ring);
		ardop_resample_reset(&b->tx_rs);
		b->written = 0;
		atomic_store_explicit(&b->tx_active, true,
				      memory_order_release);
		ardop_ptt_set(b->ptt, true);
		return;
	}

	/*
	 * Unkeying is the sharpest correctness hazard in the whole port, and it
	 * is inaudible until it is on the air.
	 *
	 * Blocking here is safe: loop.c's serve_tx writes the final block, and
	 * the *next* ardop_runtime_pull_tx returns 0 and drops PTT from inside
	 * itself, so this always runs on the modem thread via the observer --
	 * never from an audio callback.
	 */

	/* 1. The interpolator is still holding (ntaps-1)/2 device samples that
	 *    were accepted and never emitted. This is a tail cut entirely
	 *    separate from the ring, and it lands on the last symbol. */
	size_t tail = ardop_resample_flush(&b->tx_rs, b->scratch,
					   b->scratch_len);
	size_t off = 0;
	while (off < tail)
		off += ardop_ring_write(&b->play_ring, b->scratch + off,
					tail - off);

	/* 2. Wait for the device to actually consume the ring. write_audio
	 *    returned as soon as the samples were queued; several hundred
	 *    milliseconds of the transmission may not have been played, let
	 *    alone radiated. */
	uint64_t deadline = ardop_mono_ms() + DRAIN_TIMEOUT_MS;
	while (ardop_ring_avail(&b->play_ring) > 0) {
		if (!ardop_signal_wait(&b->play_sig, 50)
		    && ardop_mono_ms() > deadline) {
			fault_set(b, ARDOP_FAULT_PLAYBACK_LOST);
			break;
		}
	}

	/* 3. One device period beyond that, for what is already inside the
	 *    driver and for the radio's own changeover. */
	ardop_sleep_ms(b->period_ms + TAIL_GUARD_MS);

	b->played_at_unkey = (uint64_t)atomic_load_explicit(
		&b->played, memory_order_relaxed);

	ardop_ptt_set(b->ptt, false);
	if (ardop_ptt_fault(b->ptt) != ARDOP_FAULT_NONE)
		fault_set(b, ARDOP_FAULT_PTT_LOST);

	atomic_store_explicit(&b->tx_active, false, memory_order_release);

	/* Whatever the capture callback managed to enqueue around the edges of
	 * the transmission is our own signal. Start receive from clean. */
	ardop_ring_reset(&b->cap_ring);
	ardop_resample_reset(&b->rx_rs);
}

static uint64_t ma_wall_ms(void *ctx)
{
	(void)ctx;
	return ardop_wall_ms();
}

/* --- open / close ---------------------------------------------------------- */

/*
 * Resolve a saved selection: exact id, then exact name, then the default.
 *
 * The order is the point (audio_devices.h). Falling straight back to the
 * default when a USB interface has been renumbered would transmit through
 * whatever is now first in the list, which on a laptop is usually the built-in
 * speakers -- silently, and into a radio.
 *
 * The rule itself lives in ardop_audio_match_device so that a settings screen
 * and a unit test get the same answer; this is the adapter that renders
 * miniaudio's list into the form that rule takes and maps the answer back to a
 * device handle. It prints nothing: the outcome is recorded and reported
 * upward, because an operator running a graphical program never sees stderr.
 */
static bool resolve_device(ma_context *ctx, ardop_audio_dir dir,
			   const char *want, ma_device_id *out,
			   ardop_audio_match *how, ardop_audio_device *chosen)
{
	static ardop_audio_device devs[64];   /* open-time only, one thread */

	*how = ARDOP_AUDIO_MATCH_NONE;
	memset(chosen, 0, sizeof(*chosen));

	ma_device_info *play = NULL, *cap = NULL;
	ma_uint32 nplay = 0, ncap = 0;
	if (ma_context_get_devices(ctx, &play, &nplay, &cap, &ncap)
	    != MA_SUCCESS) {
		fprintf(stderr, "audio: cannot enumerate devices\n");
		return false;
	}

	const ma_device_info *list = (dir == ARDOP_AUDIO_CAPTURE) ? cap : play;
	size_t n = (dir == ARDOP_AUDIO_CAPTURE) ? ncap : nplay;
	if (n > sizeof(devs) / sizeof(devs[0]))
		n = sizeof(devs) / sizeof(devs[0]);

	for (size_t i = 0; i < n; i++) {
		memset(&devs[i], 0, sizeof(devs[i]));
		ardop_ma_id_to_str(&list[i].id, ctx->backend, devs[i].id,
				   sizeof(devs[i].id));
		snprintf(devs[i].name, sizeof(devs[i].name), "%s", list[i].name);
		devs[i].is_default = list[i].isDefault ? true : false;
	}

	size_t idx = 0;
	*how = ardop_audio_match_device(devs, n, want, want, &idx);
	if (*how == ARDOP_AUDIO_MATCH_NONE)
		return false;

	/* Both fields come from the enumeration entry even in the default case:
	 * ma_device.capture.id is zeroed when the default was used, so asking the
	 * opened device afterwards would give an empty id and the application
	 * would have nothing to persist. */
	*chosen = devs[idx];
	*out = list[idx].id;
	return true;
}

void ardop_backend_ma_close(ardop_ma_backend *b)
{
	if (!b)
		return;
	/* Unkey first, and mean it. The header has always said this happens; it
	 * did not, and got away with it only because ardopb closes the PTT object
	 * immediately afterwards. The device manager rebuilds a backend while the
	 * same PTT object stays open, which is exactly the case the promise was
	 * for -- without this, a device change during a transmission would leave
	 * the line asserted with the backend that raised it already gone. */
	if (b->ptt)
		ardop_ptt_set(b->ptt, false);

	if (b->capture_started)
		ma_device_uninit(&b->capture);
	if (b->playback_started)
		ma_device_uninit(&b->playback);
	ma_context_uninit(&b->ctx);

	ardop_signal_destroy(&b->cap_sig);
	ardop_signal_destroy(&b->play_sig);
	free(b->cap_store);
	free(b->play_store);
	free(b->scratch);
	free(b);
}

ardop_ma_backend *ardop_backend_ma_open(const ardop_ma_config *cfg,
					ardop_platform_ops *ops)
{
	ardop_ma_backend *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->ptt = cfg->ptt;
	atomic_store(&b->fault, (int)ARDOP_FAULT_NONE);

	ma_backend forced[1];
	bool have_forced = false;
	if (cfg->use_null_device) {
		forced[0] = ma_backend_null;
		have_forced = true;
	} else if (cfg->backend_name && *cfg->backend_name) {
		if (!ardop_ma_backend_from_name(cfg->backend_name, &forced[0])) {
			free(b);
			return NULL;
		}
		have_forced = true;
	}

	if (ma_context_init(have_forced ? forced : NULL, have_forced ? 1u : 0u,
			    NULL, &b->ctx) != MA_SUCCESS) {
		fprintf(stderr, "audio: no usable audio backend on this "
			"system\n");
		free(b);
		return NULL;
	}

	ma_device_id cap_id, play_id;
	bool have_cap = resolve_device(&b->ctx, ARDOP_AUDIO_CAPTURE,
				       cfg->capture_id, &cap_id,
				       &b->cap_match, &b->cap_dev);
	bool have_play = resolve_device(&b->ctx, ARDOP_AUDIO_PLAYBACK,
					cfg->playback_id, &play_id,
					&b->play_match, &b->play_dev);

	/* sampleRate 0 asks for the device's native rate: we would rather see
	 * the truth and refuse than have miniaudio quietly convert for us. */
	ma_device_config cc = ma_device_config_init(ma_device_type_capture);
	cc.capture.pDeviceID = have_cap ? &cap_id : NULL;
	cc.capture.format = ma_format_s16;
	cc.capture.channels = 1;
	cc.sampleRate = 0;
	cc.dataCallback = capture_cb;
	cc.pUserData = b;
	if (ma_device_init(&b->ctx, &cc, &b->capture) != MA_SUCCESS) {
		fprintf(stderr, "audio: cannot open the capture device\n");
		ardop_backend_ma_close(b);
		return NULL;
	}
	b->capture_started = true;

	ma_device_config pc = ma_device_config_init(ma_device_type_playback);
	pc.playback.pDeviceID = have_play ? &play_id : NULL;
	pc.playback.format = ma_format_s16;
	pc.playback.channels = 1;
	pc.sampleRate = b->capture.sampleRate;   /* one rate for both */
	pc.dataCallback = playback_cb;
	pc.pUserData = b;
	if (ma_device_init(&b->ctx, &pc, &b->playback) != MA_SUCCESS) {
		fprintf(stderr, "audio: cannot open the playback device\n");
		ardop_backend_ma_close(b);
		return NULL;
	}
	b->playback_started = true;

	b->device_rate = (unsigned)b->capture.sampleRate;
	if (b->device_rate == 0
	    || b->device_rate % (unsigned)ARDOP_RATE != 0
	    || b->device_rate / (unsigned)ARDOP_RATE > ARDOP_RESAMPLE_MAX_M) {
		/*
		 * Refused, not approximated. A fractional resampler carries its
		 * own accumulator and inserts or drops samples on its own
		 * schedule, which decouples protocol time from device time --
		 * exactly the class of bug this architecture deletes. Saying so
		 * is more honest than a session that mysteriously underperforms.
		 */
		fprintf(stderr,
			"audio: device runs at %u Hz, which is not a whole "
			"multiple of %d Hz\n"
			"       (up to %ux). Pick a device or a system format "
			"of 12000, 24000,\n"
			"       48000 or 96000 Hz. Rates like 44100 are "
			"refused rather than\n"
			"       resampled, because approximate timing here "
			"breaks the protocol\n"
			"       clock rather than just the audio.\n",
			b->device_rate, ARDOP_RATE, ARDOP_RESAMPLE_MAX_M);
		ardop_backend_ma_close(b);
		return NULL;
	}
	b->m = b->device_rate / (unsigned)ARDOP_RATE;

	if (!ardop_resample_init(&b->rx_rs, ARDOP_RESAMPLE_DECIMATE, b->m)
	    || !ardop_resample_init(&b->tx_rs, ARDOP_RESAMPLE_INTERPOLATE,
				    b->m)) {
		fprintf(stderr, "audio: resampler init failed\n");
		ardop_backend_ma_close(b);
		return NULL;
	}

	/* Drive the loop in whole device periods where that is smaller than the
	 * default, so capture-to-link latency tracks the hardware rather than a
	 * fixed 100 ms. */
	ma_uint32 period = b->capture.capture.internalPeriodSizeInFrames;
	b->period_ms = period ? (unsigned)((period * 1000u) / b->device_rate)
			      : 10u;
	if (b->period_ms == 0)
		b->period_ms = 1;
	b->block = ARDOP_LOOP_BLOCK;
	if (period) {
		size_t modem_period = period / b->m;
		if (modem_period >= 120 && modem_period < b->block)
			b->block = modem_period;
	}

	/* One second of device audio each way. write_audio blocks when the
	 * playback ring is full, so this is slack, not a transmit budget. */
	size_t ring_cap = (size_t)b->device_rate;
	b->cap_store = malloc(ring_cap * sizeof(int16_t));
	b->play_store = malloc(ring_cap * sizeof(int16_t));
	b->scratch_len = (size_t)ARDOP_LOOP_BLOCK * ARDOP_RESAMPLE_MAX_M;
	b->scratch = malloc(b->scratch_len * sizeof(int16_t));
	if (!b->cap_store || !b->play_store || !b->scratch
	    || !ardop_ring_init(&b->cap_ring, b->cap_store, ring_cap)
	    || !ardop_ring_init(&b->play_ring, b->play_store, ring_cap)
	    || !ardop_signal_init(&b->cap_sig)
	    || !ardop_signal_init(&b->play_sig)) {
		fprintf(stderr, "audio: out of memory\n");
		ardop_backend_ma_close(b);
		return NULL;
	}

	if (ma_device_start(&b->capture) != MA_SUCCESS
	    || ma_device_start(&b->playback) != MA_SUCCESS) {
		fprintf(stderr, "audio: cannot start the devices\n");
		ardop_backend_ma_close(b);
		return NULL;
	}

	fprintf(stderr,
		"backend: miniaudio %s, %u Hz (%ux %d Hz), %zu-sample block, "
		"ptt %s\n",
		ma_get_backend_name(b->ctx.backend), b->device_rate, b->m,
		ARDOP_RATE, b->block, ardop_ptt_describe(b->ptt));

	memset(ops, 0, sizeof(*ops));
	ops->ctx = b;
	ops->read_audio = ma_read_audio;
	ops->write_audio = ma_write_audio;
	ops->set_ptt = ma_set_ptt;
	ops->wall_ms = ma_wall_ms;
	return b;
}

/* --- observation ----------------------------------------------------------- */

ardop_fault ardop_backend_ma_fault(const ardop_ma_backend *b)
{
	if (!b)
		return ARDOP_FAULT_NONE;
	return (ardop_fault)atomic_load_explicit(
		&((ardop_ma_backend *)(uintptr_t)b)->fault,
		memory_order_relaxed);
}

void ardop_backend_ma_stall_capture(ardop_ma_backend *b)
{
	if (!b || !b->capture_started)
		return;
	/* Stopping the device stops the callback, which is precisely what an
	 * unplug does. The handle stays valid so close() still works. */
	ma_device_stop(&b->capture);
}

ardop_audio_match ardop_backend_ma_capture_match(const ardop_ma_backend *b)
{
	return b ? b->cap_match : ARDOP_AUDIO_MATCH_NONE;
}

ardop_audio_match ardop_backend_ma_playback_match(const ardop_ma_backend *b)
{
	return b ? b->play_match : ARDOP_AUDIO_MATCH_NONE;
}

void ardop_backend_ma_capture_device(const ardop_ma_backend *b,
				     ardop_audio_device *out)
{
	memset(out, 0, sizeof(*out));
	if (b)
		*out = b->cap_dev;
}

void ardop_backend_ma_playback_device(const ardop_ma_backend *b,
				      ardop_audio_device *out)
{
	memset(out, 0, sizeof(*out));
	if (b)
		*out = b->play_dev;
}


size_t ardop_backend_ma_block(const ardop_ma_backend *b)
{
	return b ? b->block : (size_t)ARDOP_LOOP_BLOCK;
}

unsigned ardop_backend_ma_ratio(const ardop_ma_backend *b)
{
	return b ? b->m : 1u;
}

uint64_t ardop_backend_ma_played(const ardop_ma_backend *b)
{
	return (uint64_t)atomic_load_explicit(
		&((ardop_ma_backend *)(uintptr_t)b)->played,
		memory_order_relaxed);
}

uint64_t ardop_backend_ma_played_at_unkey(const ardop_ma_backend *b)
{
	return b->played_at_unkey;
}

uint64_t ardop_backend_ma_written(const ardop_ma_backend *b)
{
	return b->written;
}

uint64_t ardop_backend_ma_capture_dropped(const ardop_ma_backend *b)
{
	return (uint64_t)atomic_load_explicit(
		&((ardop_ma_backend *)(uintptr_t)b)->capture_dropped,
		memory_order_relaxed);
}
