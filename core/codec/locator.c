#include "codec/locator.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Legacy ardopc transmitted "No GS" in ID frames for an unset grid square.
 * That was actually encoded with a lowercase 'o', so it is not a valid
 * Packed6; both observed byte sequences are accepted here as "unset".
 */
static const ardop_packed6 LOCATOR_NOGS[] = {
	{{0xbc, 0xf0, 0x27, 0xcc, 0x00, 0x00}},
	{{0xba, 0xf0, 0x27, 0xcc, 0x00, 0x00}},
};

/*
 * ctype.h is deliberately avoided: its classifications depend on the current
 * locale, and grid-square parsing must be the same everywhere.
 */

/* True if s is an ASCII letter in A..end (case-insensitive); end uppercase. */
static inline bool is_ascii_alpha_range(char s, char end)
{
	const char lower_add = 'a' - 'A';
	bool upper = s >= 'A' && s <= end;
	bool lower = s >= (char)('A' + lower_add) && s <= (char)(end + lower_add);
	return upper || lower;
}

static inline char to_ascii_lowercase(char s)
{
	const char lower_add = 'a' - 'A';
	if (s >= 'A' && s <= 'Z')
		return (char)(s + lower_add);
	return s;
}

static inline bool is_ascii_digit(char s)
{
	return s >= '0' && s <= '9';
}

static ardop_locator_err validate_grid(const ardop_locator *locator)
{
	/* Bounded length without strnlen (hidden under strict -std=c11). */
	size_t len = 0;
	while (len < sizeof(locator->grid) && locator->grid[len] != '\0')
		len++;

	if (len % 2 != 0)
		return ARDOP_LOCATOR_ERR_FMT_LENGTH;

	/*
	 * Every pair is checked and the *last* failing one decides the error
	 * code -- not the first. This matches the original, whose loop kept
	 * overwriting a single `err`; the accept/reject decision is the same
	 * either way, but the specific code on a multiply-invalid grid is not.
	 */
	ardop_locator_err err = ARDOP_LOCATOR_OK;
	for (size_t i = 0; i < len; i++) {
		switch (i / 2) {
		case 0:  /* field: A..R */
			if (!is_ascii_alpha_range(locator->grid[i], 'R'))
				err = ARDOP_LOCATOR_ERR_FMT_FIELD;
			break;
		case 1:  /* square: 0..9 */
			if (!is_ascii_digit(locator->grid[i]))
				err = ARDOP_LOCATOR_ERR_FMT_SQUARE;
			break;
		case 2:  /* subsquare: A..X */
			if (!is_ascii_alpha_range(locator->grid[i], 'X'))
				err = ARDOP_LOCATOR_ERR_FMT_SUBSQUARE;
			break;
		default:  /* extended square: 0..9 */
			if (!is_ascii_digit(locator->grid[i]))
				err = ARDOP_LOCATOR_ERR_FMT_EXTSQUARE;
			break;
		}
	}

	return err;
}

static ardop_locator_err compress(const ardop_locator *locator,
				  ardop_packed6 *out)
{
	ardop_locator_err res = validate_grid(locator);
	if (res)
		return res;

	if (!ardop_packed6_from_str_slice(locator->grid,
					  sizeof(locator->grid) - 1, out))
		return ARDOP_LOCATOR_ERR_FMT_CHARSET;

	return ARDOP_LOCATOR_OK;
}

static ardop_locator_err uncompress(const ardop_packed6 *in,
				    ardop_locator *locator)
{
	/* Accept the legacy "unset grid square" byte sequences. */
	for (size_t i = 0; i < sizeof(LOCATOR_NOGS) / sizeof(LOCATOR_NOGS[0]);
	     i++) {
		if (memcmp(in, &LOCATOR_NOGS[i], sizeof(*in)) == 0) {
			ardop_locator_init(locator);
			return ARDOP_LOCATOR_OK;
		}
	}

	ardop_packed6_to_fixed_str(in, locator->grid);

	/* The subsquare pair is carried lowercase. */
	for (size_t i = 4; i < 6; i++)
		locator->grid[i] = to_ascii_lowercase(locator->grid[i]);

	/* Trailing spaces truncate the grid square. */
	for (size_t i = 0; i < sizeof(locator->grid); i++)
		if (locator->grid[i] == ' ')
			locator->grid[i] = '\0';

	return validate_grid(locator);
}

void ardop_locator_init(ardop_locator *locator)
{
	memset(locator, 0, sizeof(*locator));
}

ardop_locator_err ardop_locator_from_str(const char *str,
					 ardop_locator *locator)
{
	const char *inp = str ? str : "";
	return ardop_locator_from_str_slice(inp, strlen(inp), locator);
}

ardop_locator_err ardop_locator_from_str_slice(const char *str, size_t len,
					       ardop_locator *locator)
{
	/*
	 * Always define the output. The original returned early on empty input
	 * without initialising, leaving the caller's locator untouched; a
	 * caller that had not zeroed it would then read an "empty" grid that
	 * was actually garbage. Zeroing first closes that hole.
	 */
	ardop_locator_init(locator);

	if (len == 0)
		return ARDOP_LOCATOR_OK;  /* empty grid is fine */

	int ck = snprintf(locator->grid, sizeof(locator->grid), "%.*s",
			  (int)len, str);
	if (ck <= 0 || ck >= (int)sizeof(locator->grid)) {
		ardop_locator_init(locator);
		return ARDOP_LOCATOR_ERR_TOOLONG;
	}

	ardop_locator_err out = compress(locator, &locator->wire);
	if (out) {
		ardop_locator_init(locator);
		return out;
	}

	/* Canonicalise by uncompressing what we just packed. */
	out = uncompress(&locator->wire, locator);
	if (out) {
		ardop_locator_init(locator);
		return out;
	}

	return out;
}

const ardop_packed6 *ardop_locator_as_bytes(const ardop_locator *locator)
{
	if (locator && ardop_locator_is_populated(locator))
		return &locator->wire;
	return &LOCATOR_NOGS[1];
}

ardop_locator_err ardop_locator_from_bytes(
	const uint8_t bytes[ARDOP_PACKED6_SIZE], ardop_locator *locator)
{
	ardop_locator_init(locator);
	ardop_packed6_from_bytes(bytes, &locator->wire);

	ardop_locator_err out = uncompress(&locator->wire, locator);
	if (out) {
		ardop_locator_init(locator);
		return out;
	}

	return out;
}

bool ardop_locator_is_populated(const ardop_locator *locator)
{
	return locator->grid[0] != '\0';
}

const char *ardop_locator_strerror(ardop_locator_err err)
{
	static const char *const MSGS[] = {
		"unknown error",
		"length exceeded",
		"locator must be 2, 4, 6, or 8 characters",
		"locator uses invalid characters",
		"locator has invalid field (first pair)",
		"locator has invalid square (second pair)",
		"locator has invalid subsquare (third pair)",
		"locator has invalid extended square (fourth pair)",
	};

	static_assert(sizeof(MSGS) / sizeof(MSGS[0]) == ARDOP_LOCATOR_ERR_MAX_,
		      "error string count must match the error enum");

	if (err >= ARDOP_LOCATOR_ERR_MAX_)
		return MSGS[0];
	return MSGS[err];
}
