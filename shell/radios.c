#include "shell/radios.h"

#include <stdio.h>
#include <string.h>

/**
 * @file radios.c
 * @brief The known-hardware table (see radios.h).
 */

/*
 * Sourced, not invented. Where a specific product id could not be established,
 * the row is at vendor level with a generic name -- a guessed model number would
 * look authoritative and be wrong.
 *
 * Ordered specific-before-general only for readability; the lookup prefers an
 * exact product match regardless.
 */
static const ardop_radio_entry kRadios[] = {
	/* --- keying interfaces ------------------------------------------- */
	{0x0d8c, 0x0012, "C-Media CM108B audio + GPIO", "cm108:+3",
	 "direwolf cm108.c chip table"},
	{0x0d8c, 0x013c, "C-Media CM108AH audio + GPIO", "cm108:+3",
	 "direwolf cm108.c chip table"},
	{0x0c76, 0x1607, "SSS1623 audio + GPIO", "cm108:+2",
	 "direwolf cm108.c chip table; this chip has only two GPIOs"},
	{0x0c76, 0x1605, "SSS1621 audio + GPIO", "cm108:+2",
	 "direwolf cm108.c chip table; this chip has only two GPIOs"},
	{0x10c4, 0xea60, "CP2102 serial bridge (DigiRig Mobile and others)",
	 "rts:", "Silicon Labs CP210x; DigiRig Mobile keys by RTS"},

	/* --- radios with a built-in sound card ---------------------------- */
	/*
	 * Icom's vendor id is confirmed from a Linux cdc-acm patch adding
	 * 0c26:0020. The specific product id of an IC-7300 is not, so this stays
	 * at vendor level: the *method* is what matters and every Icom keys the
	 * same way.
	 */
	{0x0c26, 0x0000, "Icom (CI-V)", "civ:@94",
	 "vendor from kernel cdc-acm 0c26:0020; address is the IC-7300 default"},

	/*
	 * Every Xiegu emulates the Icom CI-V protocol, and hamlib's
	 * rigs/icom/xiegu.c gives the X6100, X6200 and G90 CI-V address 0xa4 with
	 * ptt_type = RIG_PTT_RIG -- so they key by command and ignore RTS. The
	 * USB identifier could not be sourced, so there is no row for it yet;
	 * this is here as the shape such a row takes, keyed on a vendor id that
	 * has to be confirmed before it can be trusted.
	 *
	 * An operator with a Xiegu can supply it: `lsusb` while the radio is
	 * connected, then a one-line patch or a radios.conf override.
	 */
};

#define NRADIOS (sizeof kRadios / sizeof kRadios[0])

const ardop_radio_entry *ardop_radio_lookup(uint16_t vid, uint16_t pid)
{
	/* Exact first, so a specific row always corrects a vendor-level one. */
	for (size_t i = 0; i < NRADIOS; i++)
		if (kRadios[i].vid == vid && kRadios[i].pid == pid)
			return &kRadios[i];

	for (size_t i = 0; i < NRADIOS; i++)
		if (kRadios[i].vid == vid && kRadios[i].pid == 0)
			return &kRadios[i];

	return NULL;
}

bool ardop_radio_ptt_spec(const ardop_radio_entry *entry, const char *devnode,
			  char *out, size_t cap)
{
	if (cap)
		out[0] = '\0';
	if (!entry || !entry->ptt || !*entry->ptt || !devnode || !*devnode)
		return false;

	/* The fragment carries the method and, for CI-V, the address; the caller
	 * supplies the port the interface actually came up on. */
	const char *at = strchr(entry->ptt, '@');
	const char *plus = strchr(entry->ptt, '+');
	int n;

	if (at) {
		n = snprintf(out, cap, "%.*s%s%s",
			     (int)(at - entry->ptt), entry->ptt, devnode, at);
	} else if (plus) {
		n = snprintf(out, cap, "%.*s%s%s",
			     (int)(plus - entry->ptt), entry->ptt, devnode, plus);
	} else {
		n = snprintf(out, cap, "%s%s", entry->ptt, devnode);
	}

	if (n < 0 || (size_t)n >= cap) {
		if (cap)
			out[0] = '\0';
		return false;
	}
	return true;
}

size_t ardop_radio_table(const ardop_radio_entry **out)
{
	*out = kRadios;
	return NRADIOS;
}
