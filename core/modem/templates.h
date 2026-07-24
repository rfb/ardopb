#ifndef ARDOP_MODEM_TEMPLATES_H_
#define ARDOP_MODEM_TEMPLATES_H_

/**
 * @file templates.h
 * @brief The precomputed carrier-waveform sample templates.
 *
 * These are the one-symbol sample templates every modulated frame is built
 * from: the two-tone leader, and the 4FSK/PSK carrier waveforms. They are
 * normative -- the exact samples are what makes a transmission decodable by
 * another ARDOP station -- and they are generated data (from `CalcTemplates`),
 * not logic.
 *
 * TEMPORARY BRIDGE. The definitions still live in the inherited
 * `src/common/ardopSampleArrays.c`; this header only re-declares them so the
 * core modem can use them without including the 575-line `ARDOPC.h`. Relocating
 * the generated template data into `core/` is tracked as a follow-up (see
 * core/README.md). Because these are `extern const`, they contribute nothing to
 * the modem's own `.data`/`.bss`, so `make check-pure` stays clean.
 */

#include <stdint.h>

/** @brief One 20 ms symbol of the 50-baud two-tone leader (1475/1525 Hz). */
extern const short int50BaudTwoToneLeaderTemplate[240];

/** @brief PSK carrier templates: 9 carriers x 4 phases x 120 samples. */
extern const short intPSK100bdCarTemplate[9][4][120];

/** @brief 4FSK 50-baud carrier templates: 4 tones x 240 samples. */
extern const short intFSK50bdCarTemplate[4][240];

/** @brief 4FSK 100-baud carrier templates: 20 tones x 120 samples. */
extern const short intFSK100bdCarTemplate[20][120];

#endif /* ARDOP_MODEM_TEMPLATES_H_ */
