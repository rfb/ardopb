#include "shell/resample.h"

#include <math.h>
#include <string.h>

/**
 * @file resample.c
 * @brief Anti-aliased integer rate conversion (see resample.h).
 *
 * ## Why a linear-phase FIR and not something cheaper
 *
 * An elliptic IIR of the same stopband costs a fraction of the multiplies. It
 * also has frequency-dependent group delay, and that is disqualifying here:
 * `core/modem/demodulate.c` tracks symbol timing *per carrier* across up to
 * eight carriers spanning ~2 kHz. A filter that delays 2500 Hz differently from
 * 500 Hz smears symbol timing between carriers of the same frame. It would show
 * up as a quiet loss of 16QAM margin, on wideband modes only, on resampling
 * platforms only -- about the least attributable defect available. A symmetric
 * FIR has exactly constant delay by construction, so the whole class is gone.
 *
 * ## Why the passband is 5.4 kHz and not 3 kHz
 *
 * An ARDOP signal lives between 300 and 2600 Hz, so a much narrower filter would
 * pass every carrier. But `core/modem/busy.c` and the telemetry waterfall both
 * look at the *whole* band, and narrowing the passband would change what the
 * busy detector decides and what the operator sees -- on resampling platforms
 * only. The filter's job is to remove what aliases, not to shape the band.
 *
 * ## Float taps, int16 samples
 *
 * The core's DSP is float already. Q15 taps would add a quantisation noise floor
 * that then has to be argued about. The cost is that the unit tests assert
 * within a tolerance rather than bit-equality, which is acceptable precisely
 * because this is device adaptation and is deliberately *not* on the golden
 * path: `test/golden/shell_tx_wav` links the core and the templates only, so
 * nothing here can move the audio `make golden-tx` checks.
 */

/* Our own, rather than <math.h>'s. Some core/ headers redefine M_PI as a float
 * literal because the inherited waveform depends on that exact value; this file
 * has nothing to do with the waveform and should not inherit the accident. */
static const double kPi = 3.14159265358979323846;

/* Passband edge, Hz. The transition lands inside the 6 kHz modem Nyquist. */
#define ARDOP_RESAMPLE_FC 5400.0

/* Kaiser beta. ~60 dB stopband, which puts folded energy below the quantisation
 * floor of the 16-bit samples it would otherwise contaminate. */
#define ARDOP_RESAMPLE_BETA 5.65

/* Modified Bessel function of the first kind, order zero. */
static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0;
	for (int k = 1; k < 64; k++) {
		double d = x / (2.0 * (double)k);
		term *= d * d;
		sum += term;
		if (term < sum * 1e-17)
			break;
	}
	return sum;
}

/* sin(pi*x) / (pi*x), with the removable singularity filled in. */
static double sinc_pi(double x)
{
	if (fabs(x) < 1e-12)
		return 1.0;
	double px = kPi * x;
	return sin(px) / px;
}

/* Saturating float -> int16. Interpolation overshoots the source peaks (Gibbs),
 * and a wrapping conversion would turn an overshoot into a full-scale
 * discontinuity -- splatter, radiated. -Wconversion does not catch that, so the
 * clamp is written out. */
static int16_t sat16(float v)
{
	if (v >= 32767.0f)
		return 32767;
	if (v <= -32768.0f)
		return -32768;
	return (int16_t)lrintf(v);
}

/* Windowed-sinc prototype at the high rate, normalised to DC gain `gain`. */
static void design(ardop_resampler *rs, double gain)
{
	double fs_high = 12000.0 * (double)rs->m;
	double fcn = ARDOP_RESAMPLE_FC / fs_high;   /* cycles/sample */
	double centre = (double)(rs->ntaps - 1u) / 2.0;
	double i0b = bessel_i0(ARDOP_RESAMPLE_BETA);

	double sum = 0.0;
	for (unsigned i = 0; i < rs->ntaps; i++) {
		double t = (double)i - centre;
		double u = t / centre;                  /* -1 .. +1 */
		double w = bessel_i0(ARDOP_RESAMPLE_BETA * sqrt(1.0 - u * u)) / i0b;
		double v = 2.0 * fcn * sinc_pi(2.0 * fcn * t) * w;
		rs->h[i] = (float)v;
		sum += v;
	}
	/* Force exact unity (or `gain`) at DC. The analytic sum is close but not
	 * exact once windowed and truncated, and a DC error is a level error on
	 * every sample. */
	double scale = gain / sum;
	for (unsigned i = 0; i < rs->ntaps; i++)
		rs->h[i] = (float)((double)rs->h[i] * scale);
}

bool ardop_resample_init(ardop_resampler *rs, ardop_resample_dir dir, unsigned m)
{
	if (!rs || m == 0u || m > ARDOP_RESAMPLE_MAX_M)
		return false;

	memset(rs, 0, sizeof(*rs));
	rs->dir = dir;
	rs->m = m;

	if (m == 1u) {
		/* Identity. No rate change means nothing to alias and nothing to
		 * image, so a native 12 kHz device is passed through untouched
		 * and cannot be perturbed by anything in this file. */
		rs->ntaps = 0u;
		rs->nhist = 0u;
		return true;
	}

	rs->ntaps = ARDOP_RESAMPLE_TAPS(m);
	if (dir == ARDOP_RESAMPLE_DECIMATE) {
		/* The filter runs at the input (high) rate, one output per m. */
		rs->nhist = rs->ntaps;
		design(rs, 1.0);
	} else {
		/* Polyphase: the history is at the input (low) rate, and the
		 * gain of m compensates the zero-stuffing. */
		rs->nhist = (rs->ntaps + m - 1u) / m;
		design(rs, (double)m);
	}
	return true;
}

void ardop_resample_reset(ardop_resampler *rs)
{
	memset(rs->hist, 0, sizeof(rs->hist));
	rs->hpos = 0u;
}

/* Append one input sample. Written twice so that the window of the last nhist
 * samples is always contiguous at &hist[hpos] -- oldest first, newest last --
 * and the inner loops need no modulo. */
static void push(ardop_resampler *rs, float x)
{
	rs->hist[rs->hpos] = x;
	rs->hist[rs->hpos + rs->nhist] = x;
	rs->hpos = (rs->hpos + 1u == rs->nhist) ? 0u : rs->hpos + 1u;
}

size_t ardop_resample(ardop_resampler *rs, const int16_t *in, size_t n,
		      int16_t *out)
{
	if (rs->m == 1u) {
		memcpy(out, in, n * sizeof(*out));
		return n;
	}

	if (rs->dir == ARDOP_RESAMPLE_DECIMATE) {
		size_t nout = n / rs->m;
		for (size_t j = 0; j < nout; j++) {
			/* y[j] = sum_i h[i] * x[j*m - i]: the output is taken on
			 * the *first* sample of each group of m, not the last.
			 * Aligning to the last instead would shift the output by
			 * (m-1)/m of a sample -- a fractional delay the group
			 * delay figure does not account for, and enough on its
			 * own to cost ~20 dB of round-trip fidelity. */
			push(rs, (float)in[j * rs->m]);
			/* h is symmetric, so convolution needs no reversal. */
			const float *win = &rs->hist[rs->hpos];
			float acc = 0.0f;
			for (unsigned i = 0; i < rs->ntaps; i++)
				acc += rs->h[i] * win[i];
			out[j] = sat16(acc);
			for (unsigned k = 1; k < rs->m; k++)
				push(rs, (float)in[j * rs->m + k]);
		}
		return nout;
	}

	/* Interpolate. y[n*m + p] = sum_j h[p + j*m] * x[n - j]. */
	size_t w = 0;
	for (size_t j = 0; j < n; j++) {
		push(rs, (float)in[j]);
		const float *win = &rs->hist[rs->hpos];   /* newest at nhist-1 */
		for (unsigned p = 0; p < rs->m; p++) {
			float acc = 0.0f;
			unsigned d = 0;
			for (unsigned i = p; i < rs->ntaps; i += rs->m, d++)
				acc += rs->h[i] * win[rs->nhist - 1u - d];
			out[w++] = sat16(acc);
		}
	}
	return w;
}

size_t ardop_resample_in_for(const ardop_resampler *rs, size_t out_max)
{
	if (rs->dir == ARDOP_RESAMPLE_DECIMATE)
		return out_max * rs->m;
	return out_max / rs->m;
}

size_t ardop_resample_pending(const ardop_resampler *rs)
{
	if (rs->m == 1u)
		return 0u;
	/* Group delay is (ntaps - 1) / 2 samples at the high rate. With
	 * ntaps = 24m + 1 that is 12m, so both directions come to 12 output
	 * samples for decimation and 12m for interpolation. */
	size_t high = (rs->ntaps - 1u) / 2u;
	return (rs->dir == ARDOP_RESAMPLE_DECIMATE) ? high / rs->m : high;
}

size_t ardop_resample_flush(ardop_resampler *rs, int16_t *out, size_t max)
{
	size_t pending = ardop_resample_pending(rs);
	if (pending == 0u)
		return 0u;
	if (pending > max)
		pending = max;

	/* Run zeros through until the delay line has been pushed out. Bounded:
	 * pending <= 12m <= 96, so both buffers fit comfortably. */
	int16_t zeros[ARDOP_RESAMPLE_MAX_TAPS] = {0};
	int16_t tmp[ARDOP_RESAMPLE_MAX_TAPS] = {0};

	size_t nin = (rs->dir == ARDOP_RESAMPLE_DECIMATE)
			     ? pending * rs->m
			     : (pending + rs->m - 1u) / rs->m;
	if (nin > ARDOP_RESAMPLE_MAX_TAPS)
		nin = ARDOP_RESAMPLE_MAX_TAPS;

	size_t got = ardop_resample(rs, zeros, nin, tmp);
	if (got > pending)
		got = pending;
	memcpy(out, tmp, got * sizeof(*out));

	ardop_resample_reset(rs);
	return got;
}
