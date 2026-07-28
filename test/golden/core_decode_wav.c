/*
 * core_decode_wav -- decode a WAV through the rebuilt core demodulator.
 *
 * Reads a mono 16-bit PCM WAV at 12000 Hz, pushes it through
 * `ardop_demod_push` in receive-only (RXO) mode -- the same session-independent
 * frame-type acceptance that `--decodewav` uses -- and prints every recovered
 * frame as one line:
 *
 *     <frame_type_name>\t<payload_hex>
 *
 * A control frame with no payload prints an empty hex field. If nothing
 * decodes, nothing is printed. This is the C half of the golden-WAV
 * conformance check: `test_golden_core.py` gunzips each frozen recording,
 * feeds it here, and compares the output to the manifest's decode result --
 * making the real, recorded ardopcf audio an external oracle for the core RX
 * chain. See test/golden/README.md.
 *
 * The harness is pure core: it links only core/ objects. The candidate
 * frame-type set is built from the core frame table (every byte that names a
 * valid type), which is exactly the inherited `bytValidFrameTypesALL` set --
 * RXO decode minimises distance over it, so only the set matters, not order.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/frame.h"
#include "codec/rs.h"
#include "modem/demodulate.h"

/* Every RS parity length any frame type uses (from ALSASound.c's init call). */
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

/* Little-endian readers over the WAV header. */
static uint32_t rd_u32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static uint16_t rd_u16(const uint8_t *p) {
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

/*
 * Locate the PCM samples in a RIFF/WAVE file. Walks the chunk list rather than
 * assuming a fixed 44-byte header, so a file with extra chunks still parses.
 * On success sets pcm/nsamp and returns 0; on a malformed or wrong-format
 * file returns -1.
 */
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
		if (memcmp(ck, "fmt ", 4) == 0 && sz >= 16 &&
		    body + 16 <= len) {
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
		/* Chunks are word-aligned. */
		pos = body + sz + (sz & 1);
	}
	return -1;
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

	/* The frozen recordings are trimmed to the frame with little trailing
	 * silence, but the streaming demod holds back ~1.5 characters of
	 * look-ahead, so the last symbol of a frame cannot be flushed until more
	 * samples arrive. A live capture always provides them (silence after the
	 * frame); reproduce that by copying the PCM into a zero-padded buffer. */
	long padded = nsamp + 4800;
	int16_t *samples = calloc((size_t)padded, sizeof *samples);
	if (!samples) {
		fprintf(stderr, "out of memory\n");
		free(buf);
		return 2;
	}
	memcpy(samples, pcm, (size_t)nsamp * sizeof *samples);
	nsamp = padded;

	static ardop_rs rs;
	if (!ardop_rs_init(&rs, kRSLens, NUM_RSLENS)) {
		fprintf(stderr, "rs init failed\n");
		free(buf);
		return 2;
	}

	/* Candidate frame types: every byte the core frame table names as valid.
	 * This is the bytValidFrameTypesALL set; RXO decode minimises distance
	 * over it, so the set is what matters, not the order. */
	static uint8_t valid_types[256];
	int valid_len = 0;
	for (int b = 0; b < 256; b++)
		if (ardop_frame_spec_for((uint8_t)b))
			valid_types[valid_len++] = (uint8_t)b;

	static ardop_demod d;
	ardop_demod_init(&d, 100, 5);
	d.rs = &rs;
	d.ft_ctx.valid_types = valid_types;
	d.ft_ctx.valid_len = valid_len;
	d.ft_ctx.session_id = 0;
	d.ft_ctx.pending = false;
	d.ft_ctx.arq_connected = false;
	d.ft_ctx.last_arq_session_id = 0;
	d.ft_ctx.rxo = true;   /* --decodewav mode: session-independent decode */

	/* Push in 1200-sample blocks, the inherited capture granularity. Time
	 * advances as a sample count (the one clock); start past the
	 * full-search gate so the leader search runs on the first block. */
	int debug = getenv("CORE_DECODE_DEBUG") != NULL;
	uint64_t now = 1000000ull;
	for (long off = 0; off < nsamp; off += 1200) {
		long chunk = nsamp - off < 1200 ? nsamp - off : 1200;
		ardop_event evs[8];
		size_t ne = ardop_demod_push(&d, samples + off, (size_t)chunk,
					     now, evs, 8);
		now += (uint64_t)chunk;
		if (debug)
			fprintf(stderr, "push off=%ld state=%d fmlen=%d rawlen=%d "
				"symleft=%d ne=%zu\n", off, d.state,
				d.filtered_mixed_len, d.raw_len, d.symbols_left,
				ne);
		for (size_t e = 0; e < ne; e++) {
			if (debug) {
				const char *k =
					evs[e].kind == ARDOP_EV_LEADER_DETECTED
						? "LEADER"
					: evs[e].kind == ARDOP_EV_FRAME_DECODED
						? "DECODED"
						: "BAD";
				const ardop_frame_spec *s =
					ardop_frame_spec_for(evs[e].frame_type);
				fprintf(stderr, "  [%s] type=%02x %s off=%.1f "
					"sn=%d len=%d\n", k, evs[e].frame_type,
					s ? s->name : "?", (double)evs[e].offset_hz,
					evs[e].sn, evs[e].data_len);
			}
			if (evs[e].kind != ARDOP_EV_FRAME_DECODED)
				continue;
			const ardop_frame_spec *spec =
				ardop_frame_spec_for(evs[e].frame_type);
			printf("%s\t", spec ? spec->name : "?");
			for (int i = 0; i < evs[e].data_len; i++)
				printf("%02x", evs[e].data[i]);
			printf("\n");
		}
	}

	free(samples);
	free(buf);
	return 0;
}
