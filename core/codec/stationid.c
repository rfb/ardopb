#include "codec/stationid.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Callsign length bounds. */
#define CALLSIGN_MIN 2
#define CALLSIGN_MAX 7

static ardop_stationid_err validate_callsign(const ardop_stationid *station)
{
	/* Bounded length without strnlen (hidden under strict -std=c11). */
	size_t len = 0;
	while (len < sizeof(station->call) && station->call[len] != '\0')
		len++;

	if (len < CALLSIGN_MIN)
		return ARDOP_STATIONID_ERR_CALLSIGN_SHORT;
	if (len > CALLSIGN_MAX)
		return ARDOP_STATIONID_ERR_CALLSIGN_LONG;

	/* Embedded spaces are rejected; other junk is caught by the packer. */
	for (size_t i = 0; i < len; i++)
		if (station->call[i] == ' ')
			return ARDOP_STATIONID_ERR_CALLSIGN_CHARS;

	return ARDOP_STATIONID_OK;
}

/*
 * SSID text to its one-byte code: '0'-'9' -> '0'-'9', 10-35 -> 'A'-'Z',
 * 36-41 (i.e. "10".."15") -> ':'..'?'. Parsed as base 36. Returns 0 on any
 * unrepresentable SSID.
 */
static char ssid_pack(const ardop_stationid *station)
{
	char *end = NULL;
	long value = strtol(station->ssid, &end, 36);

	if (end == station->ssid || *end != '\0')
		return 0;  /* empty or not entirely numeric */
	if (value >= 0 && value <= 9)
		return (char)('0' + value);
	if (value >= 10 && value <= 35)
		return (char)('A' + (value - 10));
	if (value >= 36 && value <= 41)
		return (char)(':' + (value - 36));
	return 0;
}

/* Inverse of ssid_pack() for the wire code byte. */
static ardop_stationid_err ssid_unpack(char ssid_byte, ardop_stationid *station)
{
	if (ssid_byte >= '0' && ssid_byte <= '?') {
		snprintf(station->ssid, sizeof(station->ssid), "%d",
			 (int)(uint8_t)(ssid_byte - '0'));
		return ARDOP_STATIONID_OK;
	}
	if (ssid_byte >= 'A' && ssid_byte <= 'Z') {
		snprintf(station->ssid, sizeof(station->ssid), "%c", ssid_byte);
		return ARDOP_STATIONID_OK;
	}
	return ARDOP_STATIONID_ERR_SSID_INVALID;
}

/* Parse "CALL-SSID" text into the call/ssid fields. */
static ardop_stationid_err parse_str(const char *id_str, size_t id_str_len,
				     ardop_stationid *station)
{
	ardop_stationid_init(station);

	bool copying_callsign = true;
	char *out = &station->call[0];
	size_t opos = 0;
	size_t osize = sizeof(station->call);

	for (size_t ipos = 0; ipos < id_str_len; ipos++) {
		if (id_str[ipos] == '\0')
			return ARDOP_STATIONID_OK;
		if (id_str[ipos] == '-' && copying_callsign) {
			copying_callsign = false;
			out = &station->ssid[0];
			opos = 0;
			osize = sizeof(station->ssid);
			continue;
		}

		if (!(opos + 1 < osize))
			return ARDOP_STATIONID_ERR_TOOLONG;
		out[opos] = id_str[ipos];
		opos += 1;
	}

	return ARDOP_STATIONID_OK;
}

/* Validate the text fields and produce the wire bytes. */
static ardop_stationid_err compress(const ardop_stationid *station,
				    ardop_packed6 *out)
{
	ardop_stationid_err res = validate_callsign(station);
	if (res)
		return res;

	char ssid_byte = ssid_pack(station);
	if (ssid_byte == 0)
		return ARDOP_STATIONID_ERR_SSID_INVALID;

	/* "N0CALL 0": callsign padded to 7, last char the SSID code. */
	char work[ARDOP_PACKED6_MAX + 1];
	snprintf(work, sizeof(work), "%-7.7s%c", station->call, ssid_byte);

	if (ardop_packed6_from_str_slice(work, sizeof(work) - 1, out))
		return ARDOP_STATIONID_OK;
	return ARDOP_STATIONID_ERR_CALLSIGN_CHARS;
}

/* Recover the text fields from wire bytes and build the canonical string. */
static ardop_stationid_err uncompress(const ardop_packed6 *in,
				      ardop_stationid *station)
{
	char work[ARDOP_PACKED6_MAX + 1] = "";
	ardop_packed6_to_fixed_str(in, work);

	/* Trailing spaces truncate the callsign. */
	for (size_t i = 0; i < sizeof(work); i++)
		if (work[i] == ' ')
			work[i] = '\0';

	snprintf(station->call, sizeof(station->call), "%.7s", work);
	ardop_stationid_err res = validate_callsign(station);
	if (res)
		return res;

	res = ssid_unpack(work[CALLSIGN_MAX], station);
	if (res)
		return res;

	if (strncmp(station->ssid, "0", sizeof(station->ssid)) == 0)
		snprintf(station->str, sizeof(station->str), "%s",
			 station->call);
	else
		snprintf(station->str, sizeof(station->str), "%s-%s",
			 station->call, station->ssid);

	return ARDOP_STATIONID_OK;
}

void ardop_stationid_init(ardop_stationid *station)
{
	memset(station, 0, sizeof(*station));
	station->ssid[0] = '0';
}

ardop_stationid_err ardop_stationid_from_str(const char *str,
					     ardop_stationid *station)
{
	const char *inp = str ? str : "";
	return ardop_stationid_from_str_slice(inp, strlen(inp), station);
}

ardop_stationid_err ardop_stationid_from_str_slice(const char *str, size_t len,
						   ardop_stationid *station)
{
	ardop_stationid_err out = parse_str(str, len, station);
	if (out) {
		ardop_stationid_init(station);
		return out;
	}

	out = compress(station, &station->wire);
	if (out) {
		ardop_stationid_init(station);
		return out;
	}

	/* Canonicalise by uncompressing what we just packed. */
	out = uncompress(&station->wire, station);
	if (out) {
		ardop_stationid_init(station);
		return out;
	}

	return out;
}

ardop_stationid_err ardop_stationid_from_str_to_array(const char *str,
						      ardop_stationid *stations,
						      size_t capacity,
						      size_t *len)
{
	const char *const last = str + strlen(str) + 1;  /* past the NUL */

	*len = 0;
	const char *r_start = NULL;
	const char *state = str;
	while (state < last && *len < capacity) {
		switch (*state) {  /* C-locale isspace() plus comma */
		case '\0':
		case ',':
		case '\x20':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
		case '\v':
			if (r_start && state >= r_start) {
				ardop_stationid_err e =
					ardop_stationid_from_str_slice(
						r_start,
						(size_t)(state - r_start),
						&stations[*len]);
				if (e)
					return e;
				r_start = NULL;
				*len += 1;
				break;
			}
			if (*state == '\0')
				return ARDOP_STATIONID_OK;
			break;
		default:
			if (!r_start)
				r_start = state;
		}
		state += 1;
	}

	return ARDOP_STATIONID_OK;
}

ardop_stationid_err ardop_stationid_from_bytes(
	const uint8_t bytes[ARDOP_PACKED6_SIZE], ardop_stationid *station)
{
	ardop_stationid_init(station);
	ardop_packed6_from_bytes(bytes, &station->wire);

	ardop_stationid_err out = uncompress(&station->wire, station);
	if (out) {
		/* The original logged the rejected wire bytes here; the core
		 * does no I/O, and the return code already reports the failure. */
		ardop_stationid_init(station);
		return out;
	}

	return out;
}

bool ardop_stationid_to_buffer(const ardop_stationid *station,
			       uint8_t dest[ARDOP_PACKED6_SIZE])
{
	if (!station || !ardop_stationid_ok(station))
		return false;

	memcpy(dest, &station->wire, sizeof(station->wire));
	return true;
}

const ardop_packed6 *ardop_stationid_as_bytes(const ardop_stationid *station)
{
	if (!station || !ardop_stationid_ok(station))
		return NULL;

	return &station->wire;
}

bool ardop_stationid_array_to_str(const ardop_stationid *stations, size_t len,
				  char *dest, size_t dest_size,
				  const char *delim, const char *header,
				  const char *separator)
{
	size_t pos = 0;

	int nwrite = snprintf(dest, dest_size, "%s%s",
			      header ? header : "",
			      (separator && len > 0) ? separator : "");
	if (nwrite < 0)
		return false;
	pos += (size_t)nwrite;

	for (size_t i = 0; i < len && pos < dest_size; i++) {
		int n = snprintf(&dest[pos], dest_size - pos, "%s%s",
				 i > 0 ? delim : "", stations[i].str);
		if (n <= 0)
			return false;
		pos += (size_t)n;
	}

	return dest_size > 0 && pos < dest_size;
}

bool ardop_stationid_ok(const ardop_stationid *station)
{
	/* Valid iff the wire representation is non-zero. */
	for (size_t i = 0; i < sizeof(station->wire); i++)
		if (station->wire.b[i])
			return true;
	return false;
}

bool ardop_stationid_eq(const ardop_stationid *a, const ardop_stationid *b)
{
	return memcmp(a->wire.b, b->wire.b, sizeof(a->wire.b)) == 0;
}

const char *ardop_stationid_strerror(ardop_stationid_err err)
{
	static const char *const MSGS[] = {
		"unknown error",
		"maximum length exceeded or unsupported format",
		"callsign uses unsupported characters",
		"callsign too short",
		"callsign too long",
		"SSID unsupported or too long",
	};

	static_assert(sizeof(MSGS) / sizeof(MSGS[0]) == ARDOP_STATIONID_ERR_MAX_,
		      "error string count must match the error enum");

	/*
	 * The original's bound was `>= MAX_ - 1`, an off-by-one that returned
	 * "unknown error" for ARDOP_STATIONID_ERR_SSID_INVALID instead of its
	 * real message. strerror is a diagnostic, not on-air behaviour, so this
	 * is corrected here; test_stationid.c asserts the difference.
	 */
	if (err >= ARDOP_STATIONID_ERR_MAX_)
		return MSGS[0];
	return MSGS[err];
}
