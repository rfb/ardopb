/*
 * shell_tx_wav -- modulate one data frame to a WAV through the rebuilt core,
 * the way the assembled shell does.
 *
 * Given a frame-type name, session id and payload (from the golden manifest),
 * it encodes the frame with ardop_encode_data_frame and modulates it with
 * ardop_mod_init(30) / ardop_mod_begin(leader 240 ms) / ardop_mod_pull -- the
 * exact calls, drive level and leader the runtime's start_tx path uses -- and
 * writes a WAV byte-for-byte in the format ardopcf's --writetxwav emits (the
 * 44-byte header from src/common/wav.c, little-endian). test_golden_tx.py then
 * checks the file's SHA-256 against the manifest tx_sha256.
 *
 * This is the TX half of the W3.1 cutover proof: the assembled program produces
 * bit-identical on-air audio for every data modulation (4FSK/4PSK/8PSK/16QAM,
 * 1..8 carriers). Control-frame encoders are not yet exposed standalone, so this
 * covers the data frames; see test/golden/README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/dataframe.h"
#include "codec/frame.h"
#include "codec/rs.h"
#include "modem/modulate.h"

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

/* The runtime's start_tx uses drive level 30 and a 240 ms leader. */
#define TX_DRIVE 30
#define TX_LEADER_MS 240

/* Reverse the frame table: name -> type byte. Returns -1 if not found. */
static int frame_type_by_name(const char *name)
{
	for (int b = 0; b < 256; b++) {
		const ardop_frame_spec *s = ardop_frame_spec_for((uint8_t)b);
		if (s && strcmp(s->name, name) == 0)
			return b;
	}
	return -1;
}

/* Parse a hex string into bytes. Returns the byte count, or -1. */
static int parse_hex(const char *hex, uint8_t *out, int max)
{
	size_t len = strlen(hex);
	if (len % 2 != 0 || (int)(len / 2) > max)
		return -1;
	for (size_t i = 0; i < len; i += 2) {
		unsigned v;
		if (sscanf(hex + i, "%2x", &v) != 1)
			return -1;
		out[i / 2] = (uint8_t)v;
	}
	return (int)(len / 2);
}

/* Write a mono 16-bit 12 kHz WAV, header identical to src/common/wav.c. */
static int write_wav(const char *path, const int16_t *samples, int n)
{
	static const uint8_t header[44] = {
		'R','I','F','F', 0,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 0x10,0,0,0, 0x01,0, 0x01,0,
		0xE0,0x2E,0,0,   0xC0,0x5D,0,0, 0x02,0, 0x10,0,
		'd','a','t','a', 0,0,0,0,
	};
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	uint8_t h[44];
	memcpy(h, header, 44);
	uint32_t data_bytes = (uint32_t)n * 2u;
	uint32_t riff = data_bytes + 36u;   /* 2*NumSamples + 36. */
	h[4] = (uint8_t)riff; h[5] = (uint8_t)(riff >> 8);
	h[6] = (uint8_t)(riff >> 16); h[7] = (uint8_t)(riff >> 24);
	h[40] = (uint8_t)data_bytes; h[41] = (uint8_t)(data_bytes >> 8);
	h[42] = (uint8_t)(data_bytes >> 16); h[43] = (uint8_t)(data_bytes >> 24);
	if (fwrite(h, 1, 44, f) != 44
	    || fwrite(samples, 2, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	return fclose(f);
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr,
			"usage: %s FRAME_TYPE SESSION_HEX PAYLOAD_HEX OUT.wav\n",
			argv[0]);
		return 2;
	}
	const char *frame_name = argv[1];
	const char *session_hex = argv[2];
	const char *payload_hex = argv[3];
	const char *out_path = argv[4];

	int ft = frame_type_by_name(frame_name);
	if (ft < 0) {
		fprintf(stderr, "unknown frame type %s\n", frame_name);
		return 2;
	}
	unsigned session = 0;
	if (sscanf(session_hex, "%2x", &session) != 1) {
		fprintf(stderr, "bad session id %s\n", session_hex);
		return 2;
	}

	static uint8_t payload[8192];
	int plen = parse_hex(payload_hex, payload, sizeof(payload));
	if (plen < 0) {
		fprintf(stderr, "bad payload hex\n");
		return 2;
	}

	static ardop_rs rs;
	if (!ardop_rs_init(&rs, kRSLens, NUM_RSLENS)) {
		fprintf(stderr, "rs init failed\n");
		return 2;
	}

	static uint8_t encoded[4096];
	int enc = ardop_encode_data_frame(&rs, (uint8_t)ft, (uint8_t)session,
					  payload, plen, encoded);
	if (enc <= 0) {
		fprintf(stderr, "encode failed for %s\n", frame_name);
		return 2;
	}

	static ardop_mod mod;
	static int16_t samples[ARDOP_MOD_MAX_SAMPLES];
	ardop_mod_init(&mod, TX_DRIVE);
	if (!ardop_mod_begin(&mod, (uint8_t)ft, encoded, (size_t)enc,
			     TX_LEADER_MS, samples, ARDOP_MOD_MAX_SAMPLES)) {
		fprintf(stderr, "modulate failed for %s\n", frame_name);
		return 2;
	}

	/* Drain the whole frame -- the shell pulls in blocks; here in one go. */
	static int16_t out[ARDOP_MOD_MAX_SAMPLES];
	size_t total = 0, got;
	while ((got = ardop_mod_pull(&mod, out + total,
				     ARDOP_MOD_MAX_SAMPLES - total)) > 0)
		total += got;

	if (write_wav(out_path, out, (int)total) != 0) {
		fprintf(stderr, "cannot write %s\n", out_path);
		return 2;
	}
	fprintf(stderr, "%s: %zu samples\n", frame_name, total);
	return 0;
}
