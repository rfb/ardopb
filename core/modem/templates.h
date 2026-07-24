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
 * The definitions live in `templates.c` (relocated from the inherited
 * `src/common/ardopSampleArrays.c`). The inherited tree still references these
 * symbols by their original names via `ARDOPC.h`, so the names are unprefixed
 * for now; they move behind the `ardop_` convention when the old tree retires.
 * Being `extern const`, they contribute nothing to any translation unit's
 * `.data`/`.bss`, so `make check-pure` stays clean.
 */

#include <stdint.h>

/** @brief One 20 ms symbol of the 50-baud two-tone leader (1475/1525 Hz). */
extern const short int50BaudTwoToneLeaderTemplate[240];

/** @brief PSK carrier templates: 9 carriers x 4 phases x 120 samples. */
extern const short intPSK100bdCarTemplate[9][4][120];

/** @brief 4FSK 50-baud carrier templates: 4 tones x 240 samples. */
extern const short intFSK50bdCarTemplate[4][240];

/**
 * @brief 4FSK 100-baud carrier templates: 4 tones x 120 samples.
 *
 * `ARDOPC.h` over-declares this `[20][120]`, but only four tones are defined and
 * only four are ever used; the real extent is `[4][120]`.
 */
extern const short intFSK100bdCarTemplate[4][120];

/** @brief 4FSK 600-baud carrier templates: 4 tones x 20 samples. */
extern const short intFSK600bdCarTemplate[4][20];

#endif /* ARDOP_MODEM_TEMPLATES_H_ */
