#ifndef ARDOP_CODEC_LOCATOR_H_
#define ARDOP_CODEC_LOCATOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mustuse.h"
#include "codec/packed6.h"

/**
 * @file locator.h
 * @brief Maidenhead grid squares and their wire encoding.
 *
 * A station may advertise its Maidenhead locator (e.g. "BL11bh16"). It is
 * optional -- a missing or undecodable grid square never voids a frame -- but
 * when present it travels as a six-byte ::ardop_packed6, and the format and
 * canonicalisation rules here are what a receiver relies on to recover it, so
 * they are normative.
 *
 * A port of the original `Locator`, which was already well factored; the
 * changes are the `ardop_` naming, the ::ardop_packed6 dependency, dropping the
 * debug log call, and the stricter build. `test/core/test_locator.c` checks it
 * against the original.
 */

/** @brief Grid-square string buffer length, including the trailing NUL. */
#define ARDOP_LOCATOR_SIZE 9

/** @brief Result of a locator operation. */
typedef enum {
	ARDOP_LOCATOR_OK = 0,               /**< Accepted (may be empty). */
	ARDOP_LOCATOR_ERR_TOOLONG = 1,      /**< Longer than a grid square. */
	ARDOP_LOCATOR_ERR_FMT_LENGTH = 2,   /**< Not 2, 4, 6 or 8 characters. */
	ARDOP_LOCATOR_ERR_FMT_CHARSET = 3,  /**< Uses invalid characters. */
	ARDOP_LOCATOR_ERR_FMT_FIELD = 4,    /**< Bad field (first pair). */
	ARDOP_LOCATOR_ERR_FMT_SQUARE = 5,   /**< Bad square (second pair). */
	ARDOP_LOCATOR_ERR_FMT_SUBSQUARE = 6,/**< Bad subsquare (third pair). */
	ARDOP_LOCATOR_ERR_FMT_EXTSQUARE = 7,/**< Bad extended square (fourth). */
	ARDOP_LOCATOR_ERR_MAX_ = 8,
} ardop_locator_err;

/**
 * @brief A Maidenhead grid square and its wire encoding.
 *
 * ::grid is the human-readable form (empty if none); ::wire is the normative
 * six-byte form. Use ardop_locator_as_bytes() rather than reading ::wire.
 */
typedef struct {
	char grid[ARDOP_LOCATOR_SIZE];  /**< e.g. "BL11bh16", or empty. */
	ardop_packed6 wire;             /**< Six-byte wire representation. */
} ardop_locator;

/** @brief Empty a locator. The empty locator is valid but conveys nothing. */
void ardop_locator_init(ardop_locator *locator);

/**
 * @brief Parse and canonicalise a grid-square string.
 *
 * Length must be 0, 2, 4, 6 or 8; lowercase is folded and the canonical form
 * lowercases the subsquare pair. On error @p locator becomes the empty square.
 */
ARDOP_MUSTUSE ardop_locator_err
ardop_locator_from_str(const char *str, ardop_locator *locator);

/** @brief As ardop_locator_from_str(), with an explicit length. */
ARDOP_MUSTUSE ardop_locator_err
ardop_locator_from_str_slice(const char *str, size_t len,
			     ardop_locator *locator);

/** @brief Decode a locator from its six wire bytes. */
ARDOP_MUSTUSE ardop_locator_err
ardop_locator_from_bytes(const uint8_t bytes[ARDOP_PACKED6_SIZE],
			 ardop_locator *locator);

/**
 * @brief The wire representation for transmission.
 * @return A pointer to @p locator's bytes, or to the legacy "no grid square"
 *         sequence if it is NULL or empty. Never NULL.
 */
const ardop_packed6 *ardop_locator_as_bytes(const ardop_locator *locator);

/** @brief Whether a locator carries a grid square. */
bool ardop_locator_is_populated(const ardop_locator *locator);

/** @brief A static, human-readable message for an error code. Never NULL. */
const char *ardop_locator_strerror(ardop_locator_err err);

#endif /* ARDOP_CODEC_LOCATOR_H_ */
