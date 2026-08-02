"""
HF channel impairments for the Memory-ARQ measurements in analysis/18.

Three models, because "does averaging help?" has a different answer under each:

  awgn        Additive white Gaussian noise. The easy case, and the one a
              synthetic test reaches for first. Copies are independent, so
              averaging N of them buys the textbook improvement.

  watterson   The standard HF ionospheric model (ITU-R F.1487): two Rayleigh-
              fading paths separated by a delay, each with a Gaussian Doppler
              spectrum. This is what actually happens on a sky-wave path, and
              it is qualitatively different from AWGN -- the channel has memory,
              the amplitude varies, and crucially the *phase* rotates, which is
              what a PSK decoder cares about.

  impulsive   Static crashes: short, very loud bursts at Poisson arrivals.
              Included specifically to test the assumption behind the 4FSK
              path's per-symbol normalisation -- that one loud copy must not be
              allowed to swamp several clean ones.

The fading is applied to the *analytic* signal, so a path's tap gain rotates
phase as well as scaling amplitude. Applying a real envelope instead would be a
much gentler channel than the real thing and would flatter a PSK decoder.
"""

import numpy as np
from scipy.signal import hilbert

FS = 12000.0

# ITU-R F.1487 / CCIR reference conditions: (path delay in seconds,
# Doppler spread in Hz). "Spread" is the 2-sigma width of the Gaussian
# Doppler power spectrum.
WATTERSON_CONDITIONS = {
    "good": (0.5e-3, 0.1),
    "moderate": (1.0e-3, 0.5),
    "poor": (2.0e-3, 1.0),
}


def _rayleigh_tap(n, doppler_hz, rng):
    """One complex Rayleigh tap gain with a Gaussian Doppler spectrum.

    Built in the frequency domain: white complex Gaussian shaped by the Doppler
    power spectrum, then back to the time domain. Normalised to unit mean power
    so the channel neither adds nor removes average signal energy -- otherwise a
    "worse" channel would also be a quieter one and the S/N axis would lie.
    """
    f = np.fft.fftfreq(n, 1.0 / FS)
    sigma_f = max(doppler_hz, 1e-6) / 2.0
    shape = np.exp(-(f ** 2) / (2.0 * sigma_f ** 2))

    w = rng.normal(size=n) + 1j * rng.normal(size=n)
    g = np.fft.ifft(np.fft.fft(w) * np.sqrt(shape))
    power = np.mean(np.abs(g) ** 2)
    if power <= 0:
        return np.ones(n, dtype=complex)
    return g / np.sqrt(power)


def watterson(x, condition, rng):
    """Two-path Watterson fading. `condition` is a WATTERSON_CONDITIONS key."""
    delay_s, doppler_hz = WATTERSON_CONDITIONS[condition]
    n = len(x)
    xa = hilbert(x.astype(float))

    out = np.zeros(n, dtype=complex)
    for delay in (0.0, delay_s):
        d = int(round(delay * FS))
        shifted = np.zeros(n, dtype=complex)
        if d == 0:
            shifted = xa
        elif d < n:
            shifted[d:] = xa[: n - d]
        out += _rayleigh_tap(n, doppler_hz, rng) * shifted

    # Two equal-power paths: keep the average power the same as the input's.
    return np.real(out) / np.sqrt(2.0)


def impulsive(x, rng, rate_per_sec=3.0, amp_ratio=8.0, dur_ms=2.0):
    """Poisson-arrival static crashes.

    Defaults are deliberately harsh: three crashes a second at eight times the
    signal RMS. The point is not realism at a particular site, it is to make the
    failure mode visible if it exists.
    """
    y = x.astype(float).copy()
    n = len(y)
    sig = np.sqrt(np.mean(y ** 2)) or 1.0
    length = max(1, int(dur_ms * 1e-3 * FS))

    count = rng.poisson(rate_per_sec * n / FS)
    for _ in range(int(count)):
        pos = int(rng.integers(0, max(1, n - length)))
        y[pos:pos + length] += rng.normal(0.0, sig * amp_ratio, length)
    return y


def add_awgn(x, snr_db, rng):
    """Additive white Gaussian noise at `snr_db` relative to mean signal power.

    The S/N is wideband -- noise across the whole 6 kHz Nyquist against a signal
    occupying a few hundred Hz -- so it reads roughly 15 dB below the in-band
    figure an operator would quote for a 200 Hz frame. What matters for the
    comparisons here is that it is the same definition everywhere.
    """
    sig = np.sqrt(np.mean(x.astype(float) ** 2)) or 1.0
    noise = sig / (10.0 ** (snr_db / 20.0))
    return x.astype(float) + rng.normal(0.0, noise, len(x))


def apply_channel(clean_i16, channel, snr_db, seed):
    """Impair one copy. Returns int16, same length as the input.

    Order is physical: the signal fades on the path, then noise is added at the
    receiver. Doing it the other way would fade the receiver's own noise.
    """
    rng = np.random.default_rng(seed)
    x = clean_i16.astype(float)

    if channel.startswith("watterson-"):
        x = watterson(x, channel.split("-", 1)[1], rng)
    elif channel == "impulsive":
        x = impulsive(x, rng)
    elif channel != "awgn":
        raise ValueError("unknown channel %r" % channel)

    x = add_awgn(x, snr_db, rng)
    return np.clip(np.rint(x), -32768, 32767).astype(np.int16)


CHANNELS = ["awgn", "watterson-good", "watterson-moderate", "watterson-poor",
            "impulsive"]
