#ifndef ARDOP_SHELL_WAVWRITER_H_
#define ARDOP_SHELL_WAVWRITER_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @file wavwriter.h
 * @brief A live recording of RX audio, at the modem's own rate.
 *
 * `shell/loop.c`'s `serve_rx()` hands 12 kHz mono samples to
 * `ardop_runtime_rx()` one block at a time; this writes the same samples to a
 * WAV file as they arrive, so a real session -- not just the golden corpus's
 * frozen cases -- can be replayed later against a demodulator change.
 *
 * 12 kHz, not the device's native rate: it is the modem's own working
 * domain, and it is the format `test/golden/shell_decode_wav`,
 * `core_decode_wav` and `resample_decode_wav` already read, so a recording
 * needs no conversion to become a new test case.
 *
 * Header format is `test/golden/shell_tx_wav.c`'s `write_wav`, byte for byte
 * -- the same 44-byte layout `ardopcf --writetxwav` emits -- so nothing that
 * already reads a golden-corpus WAV needs to know this one was recorded live
 * rather than modulated.
 */

typedef struct ardop_wav_writer ardop_wav_writer;

/**
 * @brief Open @p path and write a placeholder 44-byte header (12 kHz mono
 * 16-bit PCM). NULL on failure; a diagnostic is printed to stderr.
 */
ardop_wav_writer *ardop_wav_writer_open(const char *path);

/**
 * @brief Append samples, then re-patch the header's two length fields and
 * reposition to the end of the file.
 *
 * Every call, not just at close -- a process killed mid-recording (a crash,
 * SIGKILL, a lost radio) is exactly when this file matters most, and a
 * header patched only at a close that may never come leaves nothing playable.
 * The extra seek/write is a few dozen bytes at whatever rate ::ardop_loop
 * calls this (~10/s at the default block size); not worth economising on.
 *
 * NULL @p w is a silent no-op, so every call site can be unconditional.
 */
void ardop_wav_writer_append(ardop_wav_writer *w, const int16_t *samples,
			     size_t n);

/** @brief Close and free. NULL is fine. */
void ardop_wav_writer_close(ardop_wav_writer *w);

#endif /* ARDOP_SHELL_WAVWRITER_H_ */
