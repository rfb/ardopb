#ifndef ARDOP_MODEM_FFT_H_
#define ARDOP_MODEM_FFT_H_

#include <stdbool.h>

/**
 * @file fft.h
 * @brief A radix-2 FFT, ported bit-for-bit from the inherited FourierTransform.
 *
 * Used by the channel-busy detector, which needs a full-passband spectrum the
 * targeted Goertzels of the leader search do not provide (see busy.h). The
 * transform is the in-place decimation-in-time radix-2 FFT from `FFT.c`, kept
 * arithmetically identical -- including the reduced `M_PI` (3.1415926f) the
 * inherited headers bake in, an [[ardop-mpi-normative-accident]] that shifts the
 * twiddle factors and so the resulting bins. @p n must be a power of two.
 *
 * Sans-I/O: no globals, no allocation; the caller owns every buffer.
 */

/**
 * @brief Forward or inverse FFT of @p n real samples.
 *
 * @param n         Number of samples; must be a power of two.
 * @param real_in   Input samples (@p n of them). Imaginary input is taken as 0.
 * @param real_out  Output real parts (@p n). May not alias @p real_in.
 * @param imag_out  Output imaginary parts (@p n).
 * @param inverse   true for the inverse transform (negated angle, 1/n scaling).
 */
void ardop_fft(int n, const float *real_in, float *real_out, float *imag_out,
	       bool inverse);

#endif /* ARDOP_MODEM_FFT_H_ */
