#include "shell/wavwriter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file wavwriter.c
 * @brief The live RX recording (see wavwriter.h).
 *
 * Ordinary buffered file I/O, no platform split needed -- matches
 * `test/golden/shell_tx_wav.c`'s own `write_wav`, which needs none either.
 */

struct ardop_wav_writer {
	FILE *f;
	uint32_t total_samples;   /* Capped, not wrapped, past ~99 hours (u32). */
};

/* test/golden/shell_tx_wav.c's write_wav header, adapted: 12 kHz, mono,
 * 16-bit PCM. RIFF size (offset 4) and data size (offset 40) start at the
 * correct values for zero samples appended so far -- 36 and 0, not a bare
 * placeholder -- so the file is spec-correct even if the process ends before
 * a single ardop_wav_writer_append() ever repatches them further. */
static const uint8_t kHeader[44] = {
	'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
	'f', 'm', 't', ' ', 0x10, 0, 0, 0, 0x01, 0, 0x01, 0,
	0xE0, 0x2E, 0, 0, 0xC0, 0x5D, 0, 0, 0x02, 0, 0x10, 0,
	'd', 'a', 't', 'a', 0, 0, 0, 0,
};

static void put_u32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Rewrite the RIFF and data length fields for total_samples so far, then
 * return to the end of the file, ready for the next append. */
static void repatch_header(ardop_wav_writer *w)
{
	uint32_t data_bytes = w->total_samples * 2u;
	uint32_t riff = data_bytes + 36u;   /* 2*NumSamples + 36, per the header. */
	uint8_t buf[4];

	put_u32_le(buf, riff);
	fseek(w->f, 4, SEEK_SET);
	fwrite(buf, 1, 4, w->f);

	put_u32_le(buf, data_bytes);
	fseek(w->f, 40, SEEK_SET);
	fwrite(buf, 1, 4, w->f);

	fseek(w->f, 0, SEEK_END);
	fflush(w->f);
}

ardop_wav_writer *ardop_wav_writer_open(const char *path)
{
	FILE *f = fopen(path, "wb+");
	if (!f) {
		perror("record: open");
		return NULL;
	}
	if (fwrite(kHeader, 1, sizeof(kHeader), f) != sizeof(kHeader)) {
		fclose(f);
		return NULL;
	}
	fflush(f);

	ardop_wav_writer *w = calloc(1, sizeof(*w));
	if (!w) {
		fclose(f);
		return NULL;
	}
	w->f = f;
	return w;
}

void ardop_wav_writer_append(ardop_wav_writer *w, const int16_t *samples,
			     size_t n)
{
	if (!w || n == 0)
		return;

	/* u32 sample count tops out around 99 hours at 12 kHz; a session that
	 * long has bigger problems than this cap, and refusing further samples
	 * (rather than wrapping the length field into a corrupt header) keeps
	 * everything already written valid. */
	uint32_t room = UINT32_MAX - w->total_samples;
	if ((uint32_t)n > room)
		n = room;
	if (n == 0)
		return;

	if (fwrite(samples, 2, n, w->f) != n)
		return;   /* A full disk: absorbed, same as the capture writer. */
	w->total_samples += (uint32_t)n;
	repatch_header(w);
}

void ardop_wav_writer_close(ardop_wav_writer *w)
{
	if (!w)
		return;
	if (w->f)
		fclose(w->f);
	free(w);
}
