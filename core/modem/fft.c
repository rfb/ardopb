#include "modem/fft.h"

#include <math.h>

/**
 * @file fft.c
 * @brief Radix-2 FFT (see fft.h). Ported from FourierTransform in FFT.c,
 *        arithmetic preserved exactly.
 */

/* The reduced pi the inherited build bakes into the waveform and the FFT.
 * Preserved deliberately; see [[ardop-mpi-normative-accident]]. */
#define ARDOP_FFT_PI 3.1415926f

/* Bits needed to index @p power_of_two, i.e. log2 for an exact power of two. */
static int bits_needed(int power_of_two)
{
	for (int i = 0; i <= 16; i++)
		if ((power_of_two & (1 << i)) != 0)
			return i;
	return 0;
}

/* Reverse the low @p num_bits bits of @p index (bit-reversal permutation). */
static int reverse_bits(int index, int num_bits)
{
	int rev = 0;
	for (int i = 0; i < num_bits; i++) {
		rev = (rev * 2) | (index & 1);
		index = index / 2;
	}
	return rev;
}

void ardop_fft(int n, const float *real_in, float *real_out, float *imag_out,
	       bool inverse)
{
	float angle_numerator = inverse ? -2.0f * ARDOP_FFT_PI
					: 2.0f * ARDOP_FFT_PI;
	int num_bits = bits_needed(n);

	for (int i = 0; i < n; i++) {
		int j = reverse_bits(i, num_bits);
		real_out[j] = real_in[i];
		imag_out[j] = 0.0f;   /* imaginary input taken as zero. */
	}

	int block_end = 1;
	for (int block_size = 2; block_size <= n; block_size *= 2) {
		float delta_angle = angle_numerator / (float)block_size;
		float alpha = sinf(0.5f * delta_angle);
		alpha = 2.0f * alpha * alpha;
		float beta = sinf(delta_angle);

		for (int i = 0; i < n; i += block_size) {
			float ar = 1.0f, ai = 0.0f;
			int j = i;
			for (int k = 0; k < block_end; k++) {
				int kk = j + block_end;
				float tr = ar * real_out[kk] - ai * imag_out[kk];
				float ti = ai * real_out[kk] + ar * imag_out[kk];
				real_out[kk] = real_out[j] - tr;
				imag_out[kk] = imag_out[j] - ti;
				real_out[j] = real_out[j] + tr;
				imag_out[j] = imag_out[j] + ti;
				float delta_ar = alpha * ar + beta * ai;
				ai = ai - (alpha * ai - beta * ar);
				ar = ar - delta_ar;
				j = j + 1;
			}
		}
		block_end = block_size;
	}

	if (inverse)
		for (int i = 0; i < n; i++) {
			real_out[i] = real_out[i] / (float)n;
			imag_out[i] = imag_out[i] / (float)n;
		}
}
