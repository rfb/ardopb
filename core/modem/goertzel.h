#ifndef ARDOP_MODEM_GOERTZEL_H_
#define ARDOP_MODEM_GOERTZEL_H_

#include <stdint.h>

/**
 * @file goertzel.h
 * @brief Single-bin DFT (Goertzel) and spectral-peak interpolation.
 *
 * The Goertzel algorithm evaluates one frequency bin of a DFT far more cheaply
 * than a full FFT, which is why the demodulator uses it everywhere it needs the
 * energy at a particular tone: leader detection, symbol timing, frame-type
 * classification. This is the pure DSP foundation of the receive chain -- no
 * state, no I/O -- ported from the `GoertzelRealImag*` and `SpectralPeakLocator`
 * functions of the inherited demodulator. `test/core/test_goertzel.c` checks it
 * against them.
 *
 * The bin index @p m and window results are computed with the reduced-precision
 * pi the inherited headers define (3.1415926f, not the true value); this is
 * normative -- the demodulator's numbers must match the transmitter's -- and is
 * preserved deliberately. See [[ardop-mpi-normative-accident]].
 */

/**
 * @brief Real and imaginary parts of DFT bin @p m over @p n samples.
 *
 * @param[in]  in   Sample buffer; @p n samples are read starting at @p ptr.
 * @param[in]  ptr  Index of the first sample to use.
 * @param[in]  n    Number of samples (need not be a power of two).
 * @param[in]  m    Bin index (need not be an integer).
 * @param[out] real Real part, scaled by 2/n.
 * @param[out] imag Imaginary part, scaled by 2/n.
 */
void ardop_goertzel(const int16_t *in, int ptr, int n, float m,
		    float *real, float *imag);

/** @brief As ardop_goertzel(), with a Hanning window applied (n <= 120). */
void ardop_goertzel_hanning(const int16_t *in, int ptr, int n, float m,
			    float *real, float *imag);

/** @brief As ardop_goertzel(), with a Hamming window applied (n <= 1200). */
void ardop_goertzel_hamming(const int16_t *in, int ptr, int n, float m,
			    float *real, float *imag);

/**
 * @brief Interpolate a spectral peak from three adjacent windowed bins.
 *
 * Returns the fractional bin offset of the true peak from the centre bin, tuned
 * for a Hamming window. Also reports the centre bin's magnitude.
 *
 * @param[in]  km1_re,km1_im Bin below the centre (real, imag).
 * @param[in]  k_re,k_im     Centre bin.
 * @param[in]  kp1_re,kp1_im Bin above the centre.
 * @param[out] cent_mag      Magnitude of the centre bin.
 * @return Fractional offset of the peak, in bins.
 */
float ardop_spectral_peak_locator(float km1_re, float km1_im,
				  float k_re, float k_im,
				  float kp1_re, float kp1_im,
				  float *cent_mag);

#endif /* ARDOP_MODEM_GOERTZEL_H_ */
