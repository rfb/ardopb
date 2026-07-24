#include "codec/rs.h"

#include <stddef.h>

/*
 * Reed-Solomon over GF(2^8), transcribed from Simon Rockliff's 1991 reference
 * encoder/decoder as carried in the forked implementation. The algorithm is
 * unchanged and deliberately not "cleaned up" -- it is normative, and
 * test/core/test_rs.c pins every output to the original. What changed:
 *
 *   - the Galois-field tables (alpha_to, index_of), the generator polynomials
 *     (gg) and the length set moved from file-scope globals into the caller's
 *     ardop_rs, so encode and decode read only from a const context;
 *   - the diagnostic printf()s are gone; outcomes are the return values;
 *   - narrowing at the uint8_t boundary is spelled with explicit casts, and
 *     loop variables no longer shadow, to satisfy the strict core warnings.
 */

enum {
	NN = ARDOP_RS_SYMBOLS,  /* codeword length, 2^MM - 1 */
	MM = 8,                 /* symbol width in bits: GF(2^8) */
};

/* Generator polynomial table for a given parity length, or NULL if that length
 * was not one of those init'd. Read-only view into the const context. */
static const int *rs_gg(const ardop_rs *rs, int rslen)
{
	for (int i = 0; i < rs->rslen_count; i++)
		if (rs->rslen_set[i] == rslen)
			return rs->gg[i];
	return NULL;
}

/*
 * Build GF(2^8) from the primitive polynomial p(x) = x^8 + x^4 + x^3 + x^2 + 1
 * (the RFC5510 choice, 0x171). Fills the index<->polynomial lookup tables.
 */
static void generate_gf(ardop_rs *rs)
{
	const int pp[MM + 1] = {1, 0, 1, 1, 1, 0, 0, 0, 1};
	int mask = 1;

	rs->alpha_to[MM] = 0;
	for (int i = 0; i < MM; i++) {
		rs->alpha_to[i] = mask;
		rs->index_of[rs->alpha_to[i]] = i;
		if (pp[i] != 0)
			rs->alpha_to[MM] ^= mask;
		mask <<= 1;
	}
	rs->index_of[rs->alpha_to[MM]] = MM;
	mask >>= 1;
	for (int i = MM + 1; i < NN; i++) {
		if (rs->alpha_to[i - 1] >= mask)
			rs->alpha_to[i] = rs->alpha_to[MM]
				^ ((rs->alpha_to[i - 1] ^ mask) << 1);
		else
			rs->alpha_to[i] = rs->alpha_to[i - 1] << 1;
		rs->index_of[rs->alpha_to[i]] = i;
	}
	rs->index_of[0] = -1;
}

/*
 * Build the generator polynomial for each requested parity length as the
 * product of (X + alpha^i), i = 1..rslen, then store it in index form for
 * quicker encoding. The lengths must be distinct. Returns false if too many
 * lengths, or any length is too large, were requested.
 */
static bool gen_polys(ardop_rs *rs, const int *lengths, int count)
{
	if (count > ARDOP_RS_MAX_COUNT)
		return false;

	rs->rslen_count = count;
	for (int l = 0; l < count; l++) {
		if (lengths[l] > ARDOP_RS_MAX_RSLEN)
			return false;
		rs->rslen_set[l] = lengths[l];
	}

	for (int l = 0; l < rs->rslen_count; l++) {
		int rslen = rs->rslen_set[l];
		int *ggl = rs->gg[l];

		ggl[0] = 2;  /* primitive element alpha = 2 for GF(2^8) */
		ggl[1] = 1;  /* g(x) = (X + alpha) initially */
		for (int i = 2; i <= rslen; i++) {
			ggl[i] = 1;
			for (int j = i - 1; j > 0; j--) {
				if (ggl[j] != 0)
					ggl[j] = ggl[j - 1] ^ rs->alpha_to[
						(rs->index_of[ggl[j]] + i) % NN];
				else
					ggl[j] = ggl[j - 1];
			}
			/* gg[0] can never be zero */
			ggl[0] = rs->alpha_to[(rs->index_of[ggl[0]] + i) % NN];
		}
		for (int i = 0; i <= rslen; i++)
			ggl[i] = rs->index_of[ggl[i]];
	}
	return true;
}

/*
 * Systematic encode: take message symbols data[0..kk-1] in polynomial form and
 * produce rslen parity symbols bb[0..rslen-1], via the feedback shift register
 * defined by the generator polynomial. Caller guarantees rslen was init'd.
 */
static void encode_rs(const ardop_rs *rs, const int *data, int *bb, int rslen)
{
	const int *alpha_to = rs->alpha_to;
	const int *index_of = rs->index_of;
	const int *ggl = rs_gg(rs, rslen);
	int kk = NN - rslen;

	for (int i = 0; i < rslen; i++)
		bb[i] = 0;
	for (int i = kk - 1; i >= 0; i--) {
		int feedback = index_of[data[i] ^ bb[rslen - 1]];
		if (feedback != -1) {
			for (int j = rslen - 1; j > 0; j--) {
				if (ggl[j] != -1)
					bb[j] = bb[j - 1] ^ alpha_to[
						(ggl[j] + feedback) % NN];
				else
					bb[j] = bb[j - 1];
			}
			bb[0] = alpha_to[(ggl[0] + feedback) % NN];
		} else {
			for (int j = rslen - 1; j > 0; j--)
				bb[j] = bb[j - 1];
			bb[0] = 0;
		}
	}
}

/*
 * Berlekamp decode of a length-NN block rcvd[] supplied in index form. Computes
 * the syndromes, the error-location polynomial, its roots, and the error
 * values, correcting rcvd[] in place (returned in polynomial form). See rs.h
 * for the return-value contract; test_only stops after error detection.
 */
static int decode_rs(const ardop_rs *rs, int *rcvd, int rslen, bool test_only)
{
	const int *alpha_to = rs->alpha_to;
	const int *index_of = rs->index_of;

	int retval = 0;
	int i, j, u, q;
	int elp[ARDOP_RS_MAX_RSLEN + 2][ARDOP_RS_MAX_RSLEN];
	int d[ARDOP_RS_MAX_RSLEN + 2];
	int l[ARDOP_RS_MAX_RSLEN + 2];
	int u_lu[ARDOP_RS_MAX_RSLEN + 2];
	int s[ARDOP_RS_MAX_RSLEN + 1];
	int count = 0;
	int syn_error = 0;
	int root[ARDOP_RS_MAX_RSLEN / 2];
	int loc[ARDOP_RS_MAX_RSLEN / 2];
	int z[ARDOP_RS_MAX_RSLEN / 2 + 1];
	int err[NN];
	int reg[ARDOP_RS_MAX_RSLEN / 2 + 1];

	/* form the syndromes */
	for (i = 1; i <= rslen; i++) {
		s[i] = 0;
		for (j = 0; j < NN; j++)
			if (rcvd[j] != -1)
				s[i] ^= alpha_to[(rcvd[j] + i * j) % NN];
		if (s[i] != 0) {
			syn_error = 1;  /* non-zero syndrome => error */
			retval = 1;
			if (test_only)
				return retval;
		}
		s[i] = index_of[s[i]];  /* convert to index form */
	}
	if (test_only)
		return retval;

	if (!syn_error) {
		/* no non-zero syndromes: output received codeword unchanged */
		for (i = 0; i < NN; i++)
			rcvd[i] = (rcvd[i] != -1) ? alpha_to[rcvd[i]] : 0;
		return retval;
	}

	/* Berlekamp iteration for the error-location polynomial elp. */
	d[0] = 0;
	d[1] = s[1];
	elp[0][0] = 0;
	elp[1][0] = 1;
	for (i = 1; i < rslen; i++) {
		elp[0][i] = -1;
		elp[1][i] = 0;
	}
	l[0] = 0;
	l[1] = 0;
	u_lu[0] = -1;
	u_lu[1] = 0;
	u = 0;

	do {
		u++;
		if (d[u] == -1) {
			l[u + 1] = l[u];
			for (i = 0; i <= l[u]; i++) {
				elp[u + 1][i] = elp[u][i];
				elp[u][i] = index_of[elp[u][i]];
			}
		} else {
			/* find the word with the greatest u_lu[q] for which d[q] != 0 */
			q = u - 1;
			while ((d[q] == -1) && (q > 0))
				q--;
			if (q > 0) {
				j = q;
				do {
					j--;
					if ((d[j] != -1) && (u_lu[q] < u_lu[j]))
						q = j;
				} while (j > 0);
			}

			if (l[u] > l[q] + u - q)
				l[u + 1] = l[u];
			else
				l[u + 1] = l[q] + u - q;

			for (i = 0; i < rslen; i++)
				elp[u + 1][i] = 0;
			for (i = 0; i <= l[q]; i++)
				if (elp[q][i] != -1)
					elp[u + 1][i + u - q] = alpha_to[
						(d[u] + NN - d[q] + elp[q][i]) % NN];
			for (i = 0; i <= l[u]; i++) {
				elp[u + 1][i] ^= elp[u][i];
				elp[u][i] = index_of[elp[u][i]];
			}
		}
		u_lu[u + 1] = u - l[u + 1];

		/* form the (u+1)th discrepancy (none on the last iteration) */
		if (u < rslen) {
			if (s[u + 1] != -1)
				d[u + 1] = alpha_to[s[u + 1]];
			else
				d[u + 1] = 0;
			for (i = 1; i <= l[u + 1]; i++)
				if ((s[u + 1 - i] != -1) && (elp[u + 1][i] != 0))
					d[u + 1] ^= alpha_to[(s[u + 1 - i]
						+ index_of[elp[u + 1][i]]) % NN];
			d[u + 1] = index_of[d[u + 1]];
		}
	} while ((u < rslen) && (l[u + 1] <= rslen / 2));

	u++;
	if (l[u] > rslen / 2) {
		/* elp degree exceeds tt: cannot solve, output received as is */
		retval = -1;
		for (i = 0; i < NN; i++)
			rcvd[i] = (rcvd[i] != -1) ? alpha_to[rcvd[i]] : 0;
		return retval;
	}

	/* put elp into index form and find its roots by Chien search */
	for (i = 0; i <= l[u]; i++)
		elp[u][i] = index_of[elp[u][i]];
	for (i = 1; i <= l[u]; i++)
		reg[i] = elp[u][i];
	count = 0;
	for (i = 1; i <= NN; i++) {
		q = 1;
		for (j = 1; j <= l[u]; j++)
			if (reg[j] != -1) {
				reg[j] = (reg[j] + j) % NN;
				q ^= alpha_to[reg[j]];
			}
		if (!q) {  /* root found: store the error-location index */
			root[count] = i;
			loc[count] = NN - i;
			count++;
		}
	}

	if (count != l[u]) {
		/* root count != elp degree: more than tt errors, cannot solve */
		retval = -1;
		for (i = 0; i < NN; i++)
			rcvd[i] = (rcvd[i] != -1) ? alpha_to[rcvd[i]] : 0;
		return retval;
	}

	/* form the error-evaluator polynomial z(x) */
	for (i = 1; i <= l[u]; i++) {  /* z[0] = 1 always, not needed */
		if ((s[i] != -1) && (elp[u][i] != -1))
			z[i] = alpha_to[s[i]] ^ alpha_to[elp[u][i]];
		else if ((s[i] != -1) && (elp[u][i] == -1))
			z[i] = alpha_to[s[i]];
		else if ((s[i] == -1) && (elp[u][i] != -1))
			z[i] = alpha_to[elp[u][i]];
		else
			z[i] = 0;
		for (j = 1; j < i; j++)
			if ((s[j] != -1) && (elp[u][i - j] != -1))
				z[i] ^= alpha_to[(elp[u][i - j] + s[j]) % NN];
		z[i] = index_of[z[i]];
	}

	/* convert rcvd[] to polynomial form so errors can be applied */
	for (i = 0; i < NN; i++) {
		err[i] = 0;
		rcvd[i] = (rcvd[i] != -1) ? alpha_to[rcvd[i]] : 0;
	}

	/* evaluate and apply the error at each located position */
	for (i = 0; i < l[u]; i++) {
		err[loc[i]] = 1;  /* accounts for z[0] */
		for (j = 1; j <= l[u]; j++)
			if (z[j] != -1)
				err[loc[i]] ^= alpha_to[(z[j] + j * root[i]) % NN];
		if (err[loc[i]] != 0) {
			err[loc[i]] = index_of[err[loc[i]]];
			q = 0;  /* denominator of the error term */
			for (j = 0; j < l[u]; j++)
				if (j != i)
					q += index_of[1 ^ alpha_to[
						(loc[j] + root[i]) % NN]];
			q = q % NN;
			err[loc[i]] = alpha_to[(err[loc[i]] - q + NN) % NN];
			rcvd[loc[i]] ^= err[loc[i]];
		}
	}
	return retval;
}

bool ardop_rs_init(ardop_rs *rs, const int *rslens, int count)
{
	generate_gf(rs);
	return gen_polys(rs, rslens, count);
}

int ardop_rs_append(const ardop_rs *rs, uint8_t *data, int datalen, int rslen)
{
	int bb[ARDOP_RS_MAX_RSLEN];
	int padded[NN];

	if (rs->rslen_count == 0)
		return -1;
	if (rs_gg(rs, rslen) == NULL)
		return -1;
	if (datalen + rslen > NN)
		return -1;

	for (int i = 0; i < NN; i++)
		padded[i] = 0;
	for (int i = 0; i < datalen; i++)
		padded[i + NN - rslen - datalen] = data[i];

	encode_rs(rs, padded, bb, rslen);

	for (int i = 0; i < rslen; i++)
		data[datalen + i] = (uint8_t)bb[i];
	return 0;
}

int ardop_rs_correct(const ardop_rs *rs, uint8_t *data, int combinedlen,
		     int rslen, bool test_only)
{
	int padded[NN];
	int corrcount = 0;
	int retval;

	if (rs->rslen_count == 0)
		return -2;
	if (rs_gg(rs, rslen) == NULL)
		return -2;
	if (combinedlen > NN)
		return -2;

	for (int i = 0; i < NN; i++)
		padded[i] = 0;
	for (int i = 0; i < combinedlen; i++)
		padded[i + NN - combinedlen] = data[i];
	for (int i = 0; i < NN; i++)
		padded[i] = rs->index_of[padded[i]];  /* to index form */

	retval = decode_rs(rs, padded, rslen, test_only);
	if (retval == 0)
		return 0;  /* no errors: data untouched */
	if (test_only)
		return retval;
	if (retval == -1)
		return -1;  /* unrecoverable */

	/* errors detected and possibly corrected: copy back the differences */
	for (int i = 0; i < combinedlen; i++) {
		if (padded[i + NN - combinedlen] != data[i]) {
			corrcount++;
			data[i] = (uint8_t)padded[i + NN - combinedlen];
		}
	}

	/*
	 * Non-zero values in what should be zero padding mean invalid
	 * corrections were made -- the "corrected" data is probably wrong.
	 * Rejecting these cuts false positives sharply for small rslen. See the
	 * original's notes for the measured rates.
	 */
	if (retval == 1) {
		for (int i = 0; i < NN - combinedlen; i++)
			if (padded[i] != 0)
				return -1;
	}
	return corrcount;
}
