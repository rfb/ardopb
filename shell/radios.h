#ifndef ARDOP_SHELL_RADIOS_H_
#define ARDOP_SHELL_RADIOS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file radios.h
 * @brief What we know about particular interfaces, and what to key them with.
 *
 * A table from USB vendor/product to a name and a suggested keying method, so a
 * detected radio reads as "Xiegu (CI-V)" rather than "USB Audio CODEC".
 *
 * **The suggested method is the load-bearing column and the name is the
 * decoration**, which is the opposite of how it looks. Every Xiegu emulates the
 * Icom CI-V protocol and hamlib gives the X6200, X6100 and G90
 * `ptt_type = RIG_PTT_RIG` -- they key by CAT command, and an RTS line does
 * nothing at all. Suggesting `rts:` for one would produce a radio that never
 * transmits and an operator with no way to tell why. A DigiRig Mobile keys by
 * RTS; a DigiRig Lite by CM108 GPIO. Three interfaces, three different answers.
 *
 * ## What a wrong row costs, and what limits it
 *
 * A wrong row looks authoritative, so four things bound the damage:
 *
 *  1. **The name is advisory and never changes behaviour.** The suggestion is
 *     confirmed by the operator with a PTT test; nothing is applied silently.
 *  2. **Rows that could not be sourced to a specific device are recorded at
 *     vendor level** with a generic name, rather than a guessed model.
 *  3. **An unknown device degrades** to the sound card's own name and the manual
 *     pickers -- exactly today's behaviour.
 *  4. **Every row carries its provenance**, so a bad one is traceable to
 *     whatever claimed it.
 *
 * And it is overridable without a rebuild: a `radios.conf` beside the settings
 * file is read first, so a correction is a text edit and can come back as a
 * one-line patch.
 */

/** @brief One entry. */
typedef struct {
	uint16_t vid;
	uint16_t pid;        /**< 0 matches any product from this vendor. */
	const char *model;
	const char *ptt;     /**< A spec fragment: "civ:@a4", "rts:", "cm108:+3". */
	const char *provenance;
} ardop_radio_entry;

/**
 * @brief Look up @p vid / @p pid.
 *
 * An exact product match wins over a vendor-level one, so a specific row can
 * always correct a general one.
 *
 * @return The entry, or NULL when it is not a device we know.
 */
const ardop_radio_entry *ardop_radio_lookup(uint16_t vid, uint16_t pid);

/**
 * @brief Compose a complete PTT spec from an entry and a device path.
 *
 * The table stores a fragment because it knows the *method* and the radio's
 * CI-V address but not which port the interface came up on: "civ:@a4" plus
 * "/dev/ttyUSB0" becomes "civ:/dev/ttyUSB0@a4".
 *
 * @return false if @p entry has no suggestion or the result would not fit.
 */
bool ardop_radio_ptt_spec(const ardop_radio_entry *entry, const char *devnode,
			  char *out, size_t cap);

/** @brief Entries in the built-in table, for a diagnostic listing. */
size_t ardop_radio_table(const ardop_radio_entry **out);

#endif /* ARDOP_SHELL_RADIOS_H_ */
