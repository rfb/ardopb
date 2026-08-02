/*
 * memarq_bench -- measure Memory ARQ against impaired copies of a frame.
 *
 * Two jobs, so the channel simulation can live in Python (where Watterson
 * fading is a few lines of numpy) and the decode can live here (where the
 * demodulator is):
 *
 *   memarq_bench --modulate TYPE PAYLOAD_HEX out.raw
 *       Modulate one frame to clean 12 kHz mono int16, little-endian, raw.
 *
 *   memarq_bench --decode [--no-accumulate] copy1.raw copy2.raw ...
 *       Push each copy through ONE demodulator in order and print the 1-based
 *       index of the copy that first decoded, or 0 if none did.
 *
 * `--no-accumulate` drops the Memory-ARQ state between copies, which is exactly
 * "Memory ARQ off" while leaving everything else -- the same audio, the same
 * demodulator, the same order -- identical. That is the control every
 * measurement in analysis/18 is reported against.
 *
 * Not a test: it is slow, it is driven by a sweep script, and its output is a
 * number rather than a pass or a fail. The fast in-process version of the same
 * idea is test/core/test_memarq.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/dataframe.h"
#include "codec/frame.h"
#include "codec/rs.h"
#include "modem/demodulate.h"
#include "modem/modulate.h"

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

#define PAD_SAMPLES 4800

static uint8_t g_valid_types[256];
static int g_valid_len;

static void build_valid_types(void)
{
	for (int b = 0; b < 256; b++)
		if (ardop_frame_spec_for((uint8_t)b))
			g_valid_types[g_valid_len++] = (uint8_t)b;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex(const char *s, uint8_t *out, int cap)
{
	int n = 0;
	while (*s && n < cap) {
		int hi = hexval(*s++);
		if (hi < 0)
			return -1;
		int lo = hexval(*s++);
		if (lo < 0)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
	}
	return n;
}

static int do_modulate(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: --modulate TYPE PAYLOAD_HEX out.raw\n");
		return 2;
	}
	long ft = strtol(argv[2], NULL, 0);
	static uint8_t payload[4096];
	int plen = parse_hex(argv[3], payload, sizeof(payload));
	if (plen < 0) {
		fprintf(stderr, "bad payload hex\n");
		return 2;
	}

	static ardop_rs rs;
	if (!ardop_rs_init(&rs, kRSLens, NUM_RSLENS)) {
		fprintf(stderr, "rs init failed\n");
		return 1;
	}

	static uint8_t encoded[4096];
	int enc = ardop_encode_data_frame(&rs, (uint8_t)ft, 0x00, payload, plen,
					  encoded);
	if (enc <= 0) {
		fprintf(stderr, "encode failed for type 0x%02lx\n", ft);
		return 1;
	}

	static ardop_mod mod;
	static int16_t samples[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod_init(&mod, 30);
	if (!ardop_mod_begin(&mod, (uint8_t)ft, encoded, (size_t)enc, 240,
			     samples, ARDOP_MOD_MAX_SAMPLES)) {
		fprintf(stderr, "modulate failed for type 0x%02lx\n", ft);
		return 1;
	}
	size_t n = ardop_mod_pull(&mod, samples, ARDOP_MOD_MAX_SAMPLES);

	/* Trailing silence so the streaming demodulator flushes its
	 * look-ahead; the channel models see it too, which is correct -- noise
	 * does not stop when the transmission does. */
	if (n + PAD_SAMPLES > ARDOP_MOD_MAX_SAMPLES) {
		fprintf(stderr, "frame too long to pad\n");
		return 1;
	}
	memset(&samples[n], 0, PAD_SAMPLES * sizeof(*samples));
	n += PAD_SAMPLES;

	FILE *f = fopen(argv[4], "wb");
	if (!f) {
		perror("open output");
		return 1;
	}
	if (fwrite(samples, sizeof(*samples), n, f) != n) {
		perror("write");
		fclose(f);
		return 1;
	}
	fclose(f);
	fprintf(stderr, "wrote %zu samples to %s\n", n, argv[4]);
	return 0;
}

static int16_t g_buf[ARDOP_MOD_MAX_SAMPLES];

static long read_raw(const char *path, int16_t *out, size_t cap)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return -1;
	}
	size_t n = fread(out, sizeof(*out), cap, f);
	fclose(f);
	return (long)n;
}

static int do_decode(int argc, char **argv)
{
	bool accumulate = true;
	int first = 2;
	if (first < argc && strcmp(argv[first], "--no-accumulate") == 0) {
		accumulate = false;
		first++;
	}
	if (first >= argc) {
		fprintf(stderr,
			"usage: --decode [--no-accumulate] copy1.raw ...\n");
		return 2;
	}

	static ardop_rs rs;
	static ardop_demod d;
	if (!ardop_rs_init(&rs, kRSLens, NUM_RSLENS)) {
		fprintf(stderr, "rs init failed\n");
		return 1;
	}
	build_valid_types();
	ardop_demod_init(&d, 100, 5);
	d.rs = &rs;
	d.ft_ctx.valid_types = g_valid_types;
	d.ft_ctx.valid_len = g_valid_len;
	d.ft_ctx.rxo = true;

	uint64_t now = 1000000;
	for (int i = first; i < argc; i++) {
		long n = read_raw(argv[i], g_buf, ARDOP_MOD_MAX_SAMPLES);
		if (n < 0)
			return 1;

		if (!accumulate)
			ardop_demod_memarq_reset(&d);

		bool decoded = false;
		for (long off = 0; off < n; off += 1200) {
			long chunk = n - off < 1200 ? n - off : 1200;
			ardop_event evs[8];
			size_t ne = ardop_demod_push(&d, &g_buf[off],
						     (size_t)chunk, now, evs,
						     8);
			now += (uint64_t)chunk;
			for (size_t e = 0; e < ne; e++)
				if (evs[e].kind == ARDOP_EV_FRAME_DECODED)
					decoded = true;
		}
		if (decoded) {
			printf("%d\n", i - first + 1);
			return 0;
		}
	}
	printf("0\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage:\n"
			"  %s --modulate TYPE PAYLOAD_HEX out.raw\n"
			"  %s --decode [--no-accumulate] copy1.raw ...\n",
			argv[0], argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "--modulate") == 0)
		return do_modulate(argc, argv);
	if (strcmp(argv[1], "--decode") == 0)
		return do_decode(argc, argv);
	fprintf(stderr, "unknown mode %s\n", argv[1]);
	return 2;
}
