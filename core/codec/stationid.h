#ifndef ARDOP_CODEC_STATIONID_H_
#define ARDOP_CODEC_STATIONID_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"
#include "codec/packed6.h"

/**
 * @file stationid.h
 * @brief A station's callsign and SSID, and its wire encoding.
 *
 * ARDOP identifies each station by a callsign and an optional SSID, written
 * "N0CALL-15". On the air the pair travels as a six-byte ::ardop_packed6. This
 * module parses and validates the text form, canonicalises it through a
 * pack/unpack round trip, and converts to and from the wire bytes -- the rules
 * a receiver relies on to recover the same identity a sender put on the air, so
 * they are normative.
 *
 * This is a port of the original `StationId`, which was already well factored;
 * the changes are the `ardop_` naming, the ::ardop_packed6 dependency, dropping
 * the one debug log call (the return code already reports the failure), and the
 * stricter build. `test/core/test_stationid.c` checks it against the original.
 */

/** @brief Callsign buffer length, including the trailing NUL. */
#define ARDOP_STATIONID_CALL_SIZE 8
/** @brief SSID buffer length, including the trailing NUL. */
#define ARDOP_STATIONID_SSID_SIZE 3
/** @brief Canonical string buffer length, including the trailing NUL. */
#define ARDOP_STATIONID_BUF_SIZE 11

/** @brief Result of a station-ID operation. */
typedef enum {
	ARDOP_STATIONID_OK = 0,                   /**< Valid. */
	ARDOP_STATIONID_ERR_TOOLONG = 1,          /**< Callsign or SSID too long. */
	ARDOP_STATIONID_ERR_CALLSIGN_CHARS = 2,   /**< Invalid callsign character. */
	ARDOP_STATIONID_ERR_CALLSIGN_SHORT = 3,   /**< Callsign too short. */
	ARDOP_STATIONID_ERR_CALLSIGN_LONG = 4,    /**< Callsign too long. */
	ARDOP_STATIONID_ERR_SSID_INVALID = 5,     /**< Invalid SSID. */
	ARDOP_STATIONID_ERR_MAX_ = 6,
} ardop_stationid_err;

/**
 * @brief A callsign, its SSID, and their wire encoding.
 *
 * The text fields are conveniences kept in sync by the parser; ::wire is the
 * normative form. Different SSIDs -- including SSID 0 -- are entirely separate
 * nodes for all protocol purposes.
 */
typedef struct {
	char call[ARDOP_STATIONID_CALL_SIZE];  /**< Callsign only, e.g. "N0CALL". */
	char ssid[ARDOP_STATIONID_SSID_SIZE];  /**< SSID, e.g. "15"; "0" if none. */
	char str[ARDOP_STATIONID_BUF_SIZE];    /**< Canonical "N0CALL-15" text. */
	ardop_packed6 wire;                    /**< Six-byte wire representation. */
} ardop_stationid;

/** @brief Zeroise a station ID. The zero ID is not valid. */
void ardop_stationid_init(ardop_stationid *station);

/**
 * @brief Parse a "CALL-SSID" string into a validated, canonical station ID.
 *
 * Lowercase is folded to uppercase and a missing SSID defaults to "0". On
 * error the contents of @p station are reset and must not be transmitted.
 *
 * @return ARDOP_STATIONID_OK, or the specific error.
 */
ARDOP_MUSTUSE ardop_stationid_err
ardop_stationid_from_str(const char *str, ardop_stationid *station);

/** @brief As ardop_stationid_from_str(), with an explicit length. */
ARDOP_MUSTUSE ardop_stationid_err
ardop_stationid_from_str_slice(const char *str, size_t len,
			       ardop_stationid *station);

/**
 * @brief Parse a whitespace/comma-delimited list of station IDs.
 *
 * @param[in]  str      Delimited list.
 * @param[out] stations Output array.
 * @param[in]  capacity Maximum entries to write.
 * @param[out] len      Number parsed, or the index of the failing entry.
 * @return ARDOP_STATIONID_OK, or the error from the first bad entry.
 */
ARDOP_MUSTUSE ardop_stationid_err
ardop_stationid_from_str_to_array(const char *str, ardop_stationid *stations,
				  size_t capacity, size_t *len);

/** @brief Decode a station ID from its six wire bytes. */
ARDOP_MUSTUSE ardop_stationid_err
ardop_stationid_from_bytes(const uint8_t bytes[ARDOP_PACKED6_SIZE],
			   ardop_stationid *station);

/**
 * @brief Copy the wire bytes of a valid station ID into @p dest.
 * @return true if @p station was valid and copied; false otherwise.
 */
ARDOP_MUSTUSE bool
ardop_stationid_to_buffer(const ardop_stationid *station,
			  uint8_t dest[ARDOP_PACKED6_SIZE]);

/**
 * @brief The wire representation of a valid station ID.
 * @return A pointer into @p station, or NULL if it is NULL or invalid.
 */
const ardop_packed6 *ardop_stationid_as_bytes(const ardop_stationid *station);

/**
 * @brief Join an array of station IDs into a delimited string.
 *
 * Always NUL-terminates @p dest when @p dest_size > 0; truncates rather than
 * overflow.
 *
 * @return true if the whole output fit.
 */
bool ardop_stationid_array_to_str(const ardop_stationid *stations, size_t len,
				  char *dest, size_t dest_size,
				  const char *delim, const char *header,
				  const char *separator);

/** @brief Whether a station ID is populated and valid. */
bool ardop_stationid_ok(const ardop_stationid *station);

/** @brief Whether two station IDs denote the same node (same wire bytes). */
bool ardop_stationid_eq(const ardop_stationid *a, const ardop_stationid *b);

/** @brief A static, human-readable message for an error code. Never NULL. */
const char *ardop_stationid_strerror(ardop_stationid_err err);

#endif /* ARDOP_CODEC_STATIONID_H_ */
