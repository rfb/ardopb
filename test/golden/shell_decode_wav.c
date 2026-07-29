/*
 * shell_decode_wav -- decode a WAV through the assembled rebuilt *shell*.
 *
 * Where core_decode_wav pushes samples straight through `ardop_demod_push`,
 * this drives the whole shell path the running program uses: it feeds the WAV
 * to `ardop_runtime_rx` (which runs the demod, steps the link in receive-only
 * mode, and emits observations) and reconstructs each decoded frame from the
 * ARDOP_OBS_RX_FRAME / ARDOP_OBS_RX_DATA stream. The output is byte-for-byte
 * the same one-line-per-frame format core_decode_wav prints:
 *
 *     <frame_type_name>\t<quality>\t<payload_hex>
 *
 * so test_golden_core.py judges it against the manifest unchanged (run with
 * GOLDEN_DECODE_BIN=shell_decode_wav). Passing proves the shell wires the
 * demodulator -- runtime, link (RXO), observer -- exactly as the core does, on
 * real recorded ardopcf audio. See test/golden/README.md and analysis/13 W3.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/frame.h"
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
		pos = body + sz + (sz & 1);
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
	if (argc != 2) {
		fprintf(stderr, "usage: %s file.wav\n", argv[0]);
		return 2;
	}

	uint8_t *buf = NULL;
	long len = read_file(argv[1], &buf);
	if (len < 0) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}

	const int16_t *pcm = NULL;
	long nsamp = 0;
	if (find_pcm(buf, len, &pcm, &nsamp) != 0) {
		fprintf(stderr, "%s: not a mono 16-bit 12 kHz WAV\n", argv[1]);
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
