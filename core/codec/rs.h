#ifndef ARDOP_CODEC_RS_H_
#define ARDOP_CODEC_RS_H_

#include <stdbool.h>
#include <stdint.h>

#include "common/mustuse.h"

/**
 * @file rs.h
 * @brief Reed-Solomon forward error correction over GF(2^8).
 *
 * This is the error-correcting code ARDOP puts on every data frame: a
 * systematic RS(255, 255-r) code over GF(2^8), which appends r parity bytes
 * that let a receiver repair up to r/2 corrupted bytes. It is normative -- the
 * field, its generator polynomial and the encoding are what let a receiver
 * repair what a transmitter sent -- so it may not change without breaking
 * interop. The algorithm is Simon Rockliff's 1991 encoder/decoder as carried in
 * the implementation this project forked from; `test/core/test_rs.c` checks this
 * version against that one so the port is proven faithful.
 *
 * The one substantive change from the original is state ownership. The original
 * kept its Galois-field and generator-polynomial tables in file-scope globals,
 * which made it impossible to use from more than one context and impossible to
 * unit test in isolation. Here that state lives in a caller-owned ::ardop_rs,
 * initialised once and then used read-only, so encoding and decoding are pure
 * functions of their inputs. The diagnostic printf()s the original emitted are
 * gone; the outcome is carried entirely by the return values.
 */

/** @brief Codeword length: 2^8 - 1. A block is always this many symbols. */
#define ARDOP_RS_SYMBOLS 255

/** @brief Largest parity length (r) that ::ardop_rs_init will accept. */
#define ARDOP_RS_MAX_RSLEN 64

/** @brief Most distinct parity lengths a single ::ardop_rs may hold. */
#define ARDOP_RS_MAX_COUNT 10

/**
 * @brief Precomputed Reed-Solomon tables for a fixed set of parity lengths.
 *
 * Fill it once with ardop_rs_init(), then pass it (by const pointer) to
 * ardop_rs_append() and ardop_rs_correct(). The members are an implementation
 * detail -- the struct is exposed only so a caller can own the storage without
 * a heap allocation -- and must not be modified after init.
 */
typedef struct {
	int alpha_to[ARDOP_RS_SYMBOLS + 1];  /**< GF antilog: index -> polynomial. */
	int index_of[ARDOP_RS_SYMBOLS + 1];  /**< GF log: polynomial -> index. */
	int gg[ARDOP_RS_MAX_COUNT][ARDOP_RS_MAX_RSLEN + 1];  /**< Generator polys. */
	int rslen_set[ARDOP_RS_MAX_COUNT];   /**< The parity lengths init'd. */
	int rslen_count;                     /**< How many of rslen_set are valid. */
} ardop_rs;

/**
 * @brief Precompute the tables for every parity length that will be used.
 *
 * Every @p rslen later passed to ardop_rs_append() or ardop_rs_correct() must
 * appear in @p rslens, because the generator polynomial for it is built here.
 *
 * @param[out] rs     Context to fill.
 * @param[in]  rslens The distinct parity lengths, each in 1..ARDOP_RS_MAX_RSLEN.
 * @param[in]  count  How many entries @p rslens has, at most ARDOP_RS_MAX_COUNT.
 * @return true on success; false if @p count or any length is out of range.
 */
ARDOP_MUSTUSE bool ardop_rs_init(ardop_rs *rs, const int *rslens, int count);

/**
 * @brief Append @p rslen Reed-Solomon parity bytes to @p data.
 *
 * Encodes the first @p datalen bytes of @p data and writes @p rslen parity
 * bytes at data[datalen .. datalen+rslen-1]. The buffer must therefore hold at
 * least datalen + rslen bytes.
 *
 * @param[in]     rs      An initialised context whose rslen set includes @p rslen.
 * @param[in,out] data    Input bytes in [0, datalen); parity written after them.
 * @param[in]     datalen Number of input bytes.
 * @param[in]     rslen   Parity length; must be one of the init'd lengths.
 * @return 0 on success, -1 if the context is uninitialised, @p rslen was not
 *         init'd, or datalen + rslen exceeds ARDOP_RS_SYMBOLS.
 */
ARDOP_MUSTUSE int ardop_rs_append(const ardop_rs *rs, uint8_t *data,
				  int datalen, int rslen);

/**
 * @brief Detect and (optionally) correct errors in a received block.
 *
 * @p data is @p combinedlen bytes: the message followed by its @p rslen parity
 * bytes. On a correctable block the message bytes are repaired in place.
 *
 * @param[in]     rs          An initialised context whose rslen set includes
 *                            @p rslen.
 * @param[in,out] data        Received message+parity; corrected in place.
 * @param[in]     combinedlen Total length including parity; <= ARDOP_RS_SYMBOLS.
 * @param[in]     rslen       Parity length; must be one of the init'd lengths.
 * @param[in]     test_only   If true, only report, never modify @p data.
 * @return 0 if no errors were detected; a positive count of corrected bytes if
 *         errors were repaired; -1 if errors were detected but could not be
 *         corrected; -2 if the inputs or context were invalid.
 */
ARDOP_MUSTUSE int ardop_rs_correct(const ardop_rs *rs, uint8_t *data,
				   int combinedlen, int rslen, bool test_only);

#endif /* ARDOP_CODEC_RS_H_ */
