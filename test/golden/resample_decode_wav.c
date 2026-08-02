/*
 * resample_decode_wav -- decode the golden corpus the way a 48 kHz sound card
 * would deliver it.
 *
 * analysis/15's exit criteria ask that "a 48 kHz device decimated to 12 kHz
 * decodes the golden corpus at the same rates as a native 12 kHz device". That
 * was never tested: test/core/test_resample.c has eight good cases and every one
 * of them is a synthetic tone, so the anti-alias filter had never met a
 * modulated ARDOP frame.
 *
 * This interpolates the frozen 12 kHz recording up by m -- which is what a card
 * running at 12000*m delivers when it captures that same analogue signal,
 * band-limited -- then decimates it back and decodes. Output is byte-for-byte
 * the format core_decode_wav prints, so test_golden_core.py judges it unchanged
 * via GOLDEN_DECODE_BIN.
 *
 *     resample_decode_wav [-m N] [-alias] file.wav
 *
 * With -alias an out-of-band interferer is added at the high rate, at a
 * frequency naive decimation folds to exactly 1500 Hz -- dead centre of the
 * ARDOP passband. A decimator that just takes every mth sample passes the plain
 * run and fails this one; the Kaiser low-pass puts the interferer more than
 * 60 dB down. That is the test that actually exercises the anti-alias claim.
 *
 * That the interferer is *loud enough to matter* was checked rather than
 * assumed: moving the same -10 dBFS tone to 1500 Hz, where no filter can remove
 * it, drops the corpus from 17 passing checks to 4. So the out-of-band version
 * passing is the filter doing its job, not the tone being inaudible.
 *
 * ## What this does not prove
 *
 * The high-rate stream is synthesised by *our own* interpolator, so this shows
 * the decimator recovers what that interpolator produced and rejects a known
 * out-of-band tone. It does not show that a real sound card's 48 kHz stream
 * decodes. That claim belongs to a loopback run over real cables, and it stays
 * manual.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "codec/frame.h"
#include "shell/resample.h"
#include "shell/runtime.h"

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

/* Read a whole file into a malloc'd buffer. Returns bytes read, or -1. */
static long read_file(const char *path, uint8_t **out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	long n = ftell(f);
	if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	uint8_t *buf = malloc((size_t)n);
	if (!buf) {
		fclose(f);
		return -1;
	}
	if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = buf;
	return n;
}

static uint32_t rd_u32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static uint16_t rd_u16(const uint8_t *p) {
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

/* Locate the 12 kHz mono 16-bit PCM samples in a RIFF/WAVE file. */
static int find_pcm(const uint8_t *buf, long len, const int16_t **pcm,
		    long *nsamp)
{
	if (len < 12 || memcmp(buf, "RIFF", 4) != 0 ||
	    memcmp(buf + 8, "WAVE", 4) != 0)
		return -1;

	uint16_t channels = 0, bits = 0;
	uint32_t rate = 0;
	long pos = 12;
	while (pos + 8 <= len) {
		const uint8_t *ck = buf + pos;
		uint32_t sz = rd_u32(ck + 4);
		long body = pos + 8;
		if (memcmp(ck, "fmt ", 4) == 0 && sz >= 16 && body + 16 <= len) {
			channels = rd_u16(buf + body + 2);
			rate = rd_u32(buf + body + 4);
			bits = rd_u16(buf + body + 14);
		} else if (memcmp(ck, "data", 4) == 0) {
			long avail = len - body;
			long n = (long)sz < avail ? (long)sz : avail;
			if (channels != 1 || bits != 16 || rate != 12000)
				return -1;
			*pcm = (const int16_t *)(buf + body);
			*nsamp = n / 2;
			return 0;
		}
		/*
		 * Chunks are word-aligned.
		 *
		 * Widen deliberately instead of letting the usual arithmetic
		 * conversions run. `sz` is uint32_t and `long` is 32 bits on
		 * Windows (LLP64), so `body + sz` would be evaluated unsigned
		 * and converted back to signed -- which is a warning there and
		 * silently fine on Linux, where `long` is 64 bits and uint32_t
		 * widens harmlessly. Rejecting a chunk that runs past the end
		 * of the file catches a malformed header and keeps the widened
		 * value in range on both.
		 */
		long remain = len - body;   /* >= 0: body <= len above. */
		if ((uint64_t)sz > (uint64_t)remain)
			return -1;
		pos = body + (long)sz + (long)(sz & 1u);
	}
	return -1;
}

/* One decoded frame, assembled from the observation stream. */
struct pending {
	bool have;
	uint8_t frame_type;
	int quality;
	char payload_hex[2 * 1024 + 1];
};

static void flush(struct pending *p)
{
	if (!p->have)
		return;
	const ardop_frame_spec *spec = ardop_frame_spec_for(p->frame_type);
	printf("%s\t%d\t%s\n", spec ? spec->name : "?", p->quality,
	       p->payload_hex);
	p->have = false;
	p->payload_hex[0] = '\0';
}

/* Observer: a RX_FRAME starts a frame (flushing the previous one); a RX_DATA
 * attaches its payload. Runtime emits RX_FRAME before the link step that emits
 * RX_DATA, so each data frame's payload lands on its own frame. */
static void observe(void *ctx, const ardop_obs *o)
{
	struct pending *p = ctx;
	switch (o->kind) {
	case ARDOP_OBS_RX_FRAME:
		flush(p);
		p->have = true;
		p->frame_type = o->frame_type;
		p->quality = o->quality;
		p->payload_hex[0] = '\0';
		break;
	case ARDOP_OBS_RX_DATA: {
		size_t off = 0;
		for (size_t i = 0; i < o->data_len
		     && off + 2 < sizeof(p->payload_hex); i++)
			off += (size_t)snprintf(p->payload_hex + off,
						sizeof(p->payload_hex) - off,
						"%02x", o->data[i]);
		break;
	}
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	unsigned m = 4;
	bool alias = false;
	const char *path = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
			m = (unsigned)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-alias") == 0)
			alias = true;
		else
			path = argv[i];
	}
	if (!path || m < 1 || m > ARDOP_RESAMPLE_MAX_M) {
		fprintf(stderr, "usage: %s [-m 1..%d] [-alias] file.wav\n",
			argv[0], ARDOP_RESAMPLE_MAX_M);
		return 2;
	}

	uint8_t *buf = NULL;
	long len = read_file(path, &buf);
	if (len < 0) {
		fprintf(stderr, "cannot read %s\n", path);
		return 2;
	}

	const int16_t *pcm = NULL;
	long nsamp = 0;
	if (find_pcm(buf, len, &pcm, &nsamp) != 0) {
		fprintf(stderr, "%s: not a mono 16-bit 12 kHz WAV\n", path);
		free(buf);
		return 2;
	}

	/* Pad with trailing silence so the streaming demod can flush the last
	 * frame's look-ahead (as a live capture always would). */
	long padded = nsamp + 4800;
	int16_t *samples = calloc((size_t)padded, sizeof *samples);
	if (!samples) {
		fprintf(stderr, "out of memory\n");
		free(buf);
		return 2;
	}
	memcpy(samples, pcm, (size_t)nsamp * sizeof *samples);
	nsamp = padded;

	if (m > 1) {
		int16_t *high = calloc((size_t)nsamp * m, sizeof *high);
		int16_t *back = calloc((size_t)nsamp, sizeof *back);
		if (!high || !back) {
			fprintf(stderr, "out of memory\n");
			free(high); free(back); free(samples); free(buf);
			return 2;
		}

		/* Up: what a card at 12000*m delivers, band-limited. */
		static ardop_resampler up;
		if (!ardop_resample_init(&up, ARDOP_RESAMPLE_INTERPOLATE, m)) {
			fprintf(stderr, "resampler init failed\n");
			free(high); free(back); free(samples); free(buf);
			return 2;
		}
		size_t nhigh = ardop_resample(&up, samples, (size_t)nsamp, high);

		if (alias) {
			/*
			 * An interferer outside the modem's band that naive
			 * decimation folds to 1500 Hz -- the middle of the
			 * passband. For m = 4 that is 13.5 kHz: 48000/4 = 12000,
			 * and 13500 aliases to |13500 - 12000| = 1500.
			 */
			double fs = 12000.0 * m;
			double f = fs / (double)m - 1500.0;
			double amp = 32767.0 * 0.316;   /* -10 dBFS */
			for (size_t i = 0; i < nhigh; i++) {
				double v = high[i] +
					   amp * sin(2.0 * 3.14159265358979 * f
						     * (double)i / fs);
				if (v > 32767.0) v = 32767.0;
				if (v < -32768.0) v = -32768.0;
				high[i] = (int16_t)v;
			}
		}

		/* Down: what the backend does with it. */
		static ardop_resampler down;
		if (!ardop_resample_init(&down, ARDOP_RESAMPLE_DECIMATE, m)) {
			fprintf(stderr, "resampler init failed\n");
			free(high); free(back); free(samples); free(buf);
			return 2;
		}
		/* Decimating, the input count must be a whole multiple of m. */
		size_t nback = ardop_resample(&down, high, nhigh - (nhigh % m),
					      back);

		memcpy(samples, back, nback * sizeof *samples);
		if ((long)nback < nsamp)
			memset(samples + nback, 0,
			       (size_t)(nsamp - (long)nback) * sizeof *samples);
		free(high);
		free(back);
	}

	static ardop_runtime rt;
	if (!ardop_runtime_init(&rt, kRSLens, NUM_RSLENS)) {
		fprintf(stderr, "runtime init failed\n");
		free(samples);
		free(buf);
		return 2;
	}
	rt.link.mode = ARDOP_MODE_RXO;   /* --decodewav: receive-only monitor. */

	static struct pending pending;
	ardop_runtime_observe(&rt, observe, &pending);

	uint64_t now = 1000000ull;
	for (long off = 0; off < nsamp; off += 1200) {
		long chunk = nsamp - off < 1200 ? nsamp - off : 1200;
		ardop_runtime_rx(&rt, samples + off, (size_t)chunk, now);
		now += (uint64_t)chunk;
	}
	flush(&pending);

	free(samples);
	free(buf);
	return 0;
}
