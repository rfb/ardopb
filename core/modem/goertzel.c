#include "modem/goertzel.h"

#include <math.h>

/*
 * Ported from GoertzelRealImag / GoertzelRealImagHanning /
 * GoertzelRealImagHamming / SpectralPeakLocator in the inherited demodulator.
 * The maths is unchanged and normative; test/core/test_goertzel.c pins every
 * result to the originals.
 *
 * The originals cached each window in a file-scope array to avoid recomputing
 * cosines. Here the window is recomputed per call so the functions stay pure
 * (no globals -> check-pure clean). The values are identical, so the results
 * are bit-for-bit the same; a caller-owned window cache can restore the
 * micro-optimisation later, at the point the demodulator's hot loops need it.
 */

/*
 * NOT the mathematical pi: the inherited headers define M_PI as this
 * reduced-precision float, and the demodulator's bins are computed with it. It
 * must match the transmitter. See [[ardop-mpi-normative-accident]].
 */
static const float ARDOP_PI = 3.1415926f;

void ardop_goertzel(const int16_t *in, int ptr, int n, float m,
		    float *real, float *imag)
{
	float coeff = 2 * cosf(2 * ARDOP_PI * m / (float)n);
	float z1 = 0.0f, z2 = 0.0f, w = 0.0f;

	for (int i = 0; i <= n; i++) {
		if (i == n)
			w = z1 * coeff - z2;
		else
			w = in[ptr] + z1 * coeff - z2;
		z2 = z1;
		z1 = w;
		ptr++;
	}

	*real = 2 * (w - cosf(2 * ARDOP_PI * m / (float)n) * z2) / (float)n;
	*imag = 2 * (sinf(2 * ARDOP_PI * m / (float)n) * z2) / (float)n;
}

void ardop_goertzel_hanning(const int16_t *in, int ptr, int n, float m,
			    float *real, float *imag)
{
	float win[120];
	float ang = 2 * ARDOP_PI / (float)(n - 1);
	for (int i = 0; i < n; i++)
		win[i] = (float)(0.5 - 0.5 * cosf((float)i * ang));

	float coeff = 2 * cosf(2 * ARDOP_PI * m / (float)n);
	float z1 = 0.0f, z2 = 0.0f, w = 0.0f;

	for (int i = 0; i <= n; i++) {
		if (i == n)
			w = z1 * coeff - z2;
		else
			w = in[ptr] * win[i] + z1 * coeff - z2;
		z2 = z1;
		z1 = w;
		ptr++;
	}

	*real = 2 * (w - cosf(2 * ARDOP_PI * m / (float)n) * z2) / (float)n;
	*imag = 2 * (sinf(2 * ARDOP_PI * m / (float)n) * z2) / (float)n;
}

void ardop_goertzel_hamming(const int16_t *in, int ptr, int n, float m,
			    float *real, float *imag)
{
	float win[1200];
	float ang = 2 * ARDOP_PI / (float)(n - 1);
	for (int i = 0; i < n; i++)
		win[i] = 0.54f - 0.46f * cosf((float)i * ang);

	float coeff = 2 * cosf(2 * ARDOP_PI * m / (float)n);
	float z1 = 0.0f, z2 = 0.0f, w = 0.0f;

	for (int i = 0; i <= n; i++) {
		if (i == n)
			w = z1 * coeff - z2;
		else
			w = in[ptr] * win[i] + z1 * coeff - z2;
		z2 = z1;
		z1 = w;
		ptr++;
	}

	*real = 2 * (w - cosf(2 * ARDOP_PI * m / (float)n) * z2) / (float)n;
	*imag = 2 * (sinf(2 * ARDOP_PI * m / (float)n) * z2) / (float)n;
}

float ardop_spectral_peak_locator(float km1_re, float km1_im,
				  float k_re, float k_im,
				  float kp1_re, float kp1_im,
				  float *cent_mag)
{
	*cent_mag = sqrtf(powf(k_re, 2) + powf(k_im, 2));

	float left = sqrtf(powf(km1_re, 2) + powf(km1_im, 2));
	float right = sqrtf(powf(kp1_re, 2) + powf(kp1_im, 2));

	/* 1.22 is the empirical factor for a Hamming window. */
	return (float)(1.22 * (right - left) / (left + *cent_mag + right));
}
