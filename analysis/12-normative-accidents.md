# Normative accidents — the bugs we preserve for parity

A running catalog of places where the inherited implementation does something
*wrong* — or at least arbitrary and unintended — but where the wrongness has
become part of the observable behaviour we must match. Each of these was found
while porting a module to `core/` and proving it bit-identical to the original.

## Why this file exists

Protocol compatibility is a hard commitment (see the
[README](README.md)): the rebuild must interoperate on air with ARDOP_Win /
ardopc and every other ardopcf. That means we reproduce the reference DSP
**bit-for-bit**, accidents included. A cleaner or more precise calculation that
produced a different sample or a different decision would be *more correct* and
*less compatible*, and compatibility wins until we have parity.

But some of these accidents almost certainly cost performance — a reduced
precision here, an un-reset accumulator there — and once the rebuild is at full
parity we want to revisit them deliberately. The plan is two modes:

- **`compat` (default)** — bit-identical to the reference. What this catalog
  guarantees today.
- **`improved` (opt-in, future)** — fixes the accidents that are worth fixing,
  accepting that it changes the waveform or the decisions.

The crucial split is **where** an accident lives:

- **TX-normative** — baked into the *transmitted* waveform. Fixing it changes
  what goes on the air, so it only helps if the far end also changes: an
  **over-the-air-breaking** change, negotiated or mode-gated.
- **RX-local** — only affects how *this* receiver decodes. Fixing it changes
  which frames we recover, not what any protocol peer sees, so it can be
  improved unilaterally without breaking interop (it still decodes the same
  on-air frames — just, we hope, more of them).
- **Benign** — an oddity with no measurable effect; recorded so nobody "fixes"
  it into a real divergence, but not worth a mode.

None of these may be changed in `compat` mode. This file is the checklist for
what `improved` mode gets to reconsider.

---

## Catalog

### 1. Reduced-precision `M_PI` (`3.1415926f`) — TX-normative

`M_PI` is redefined as a single-precision literal `3.1415926f` in three places
(`ARDOPC.h:60`, `ardopcommon.h:49`, `FFT.c:73`), overriding the `math.h` double.
It is short by ~5e-8 of true pi and is a `float`, so every trig-derived quantity
in the modem is computed at reduced precision: the resonator coefficients of the
TX filter and the RX 2 kHz / 75 Hz filters, the NCO phase increments, the
Goertzel and window basis functions.

Because the *transmitter's* tone frequencies and pulse shapes are derived from
it, this value is literally in the air. The receiver uses the same reduced pi so
its bins line up with the transmitter's — that part is self-consistent — but the
absolute frequencies are all slightly off true.

- **Handled in `core/` by** `ARDOP_PI = 3.1415926f` and `ARDOP_2PI = 2 * 3.1415926f`
  in `core/modem/{modulate,demodulate,goertzel}.c`, used everywhere the original
  used `M_PI` / `dbl2Pi`.
- **`improved` opportunity** — use true double `M_PI` throughout for accurate
  tone placement and filter coefficients. **OTA-breaking**: both ends must agree,
  so this belongs behind a negotiated capability, not a unilateral flag.
- **Memory:** `ardop-mpi-normative-accident`.

### 2. `PlayPSKSymbols` scales in double precision — TX-normative

`PlayPSKSymbols` (`Modulate.c:413`) declares its scaling parameter
`double dblCarScalingFactor`. The per-carrier scaling constants are `float`
literals, so they widen to double and the sample scaling multiply happens in
double, then truncates back to `int`. For multi-carrier PSK this changes
individual output samples by ±1 versus doing the multiply in float — an
arbitrary consequence of one parameter's type, not a design choice, but it is in
the transmitted samples.

- **Handled in `core/` by** doing the multiply in double:
  `sample = (int)((double)sample * (double)scaling);` in `core/modem/modulate.c`.
  This is what made all 108 PSK/QAM golden data cases byte-exact.
- **`improved` opportunity** — low value. It is a precision detail of the TX
  path with no obvious performance cost; changing it would break OTA sample
  parity for no clear gain. Recorded mainly so the double cast is never
  "simplified" away in `compat`.

### 3. `Filter75Hz` accumulator is never reset per sample — RX-local

In `Filter75Hz` (`SoundInput.c:513`, the envelope-correlator pre-filter), the
output accumulator `FilterOut` is declared once *before* the sample loop and is
**not** zeroed at the top of each iteration (contrast `FSMixFilter2000Hz`, which
does reset its per-sample accumulator). So each output sample carries the running
sum of every resonator contribution from every prior sample in the window — a
growing, mostly-DC pedestal on top of the intended filtered value. This is
almost certainly a copy/paste omission, and it feeds `EnvelopeCorrelator`, which
establishes symbol framing.

- **Handled in `core/` by** faithfully *not* resetting `filter_out` across the
  window in `core/modem/demodulate.c`'s `filter_75hz`, with a comment marking it
  as the inherited quirk. Proven identical to the original over the modulator's
  own frame and noise.
- **`improved` opportunity** — reset per sample so the envelope correlation sees
  a clean filtered signal; plausibly improves symbol-sync robustness at low S/N.
  **RX-local**: the correlator only affects where *this* receiver places symbol
  boundaries, not the transmitted signal, so this can be fixed without breaking
  interop. A candidate for a receiver-quality improvement even ahead of a formal
  `improved` mode — to be measured against the golden/degraded WAVs.

### 4. Frame-type tone magnitude truncates only the real part — RX-local

In `DemodFrameType4FSK` (`SoundInput.c:2073`), each 4FSK tone magnitude is
`intToneMags[...] = (int)powf(dblReal, 2) + powf(dblImag, 2)`. The cast binds to
the first term only, so the real part's square is truncated to `int` *before*
the (still-float) imaginary square is added, and the sum is truncated again on
store. The magnitude is therefore `trunc(trunc(re²) + im²)` rather than
`trunc(re² + im²)` — an asymmetry with no reason behind it, almost certainly a
missing pair of parentheses. It perturbs the tone magnitudes that feed the
minimal-distance frame-type decoder by up to ~1 count.

- **Handled in `core/` by** reproducing the asymmetric truncation exactly in
  `core/modem/demodulate.c`'s `ardop_demod_frametype_tonemags`
  (`(int32_t)((float)(int)powf(real,2) + powf(imag,2))`), with a comment.
- **`improved` opportunity** — compute the magnitude symmetrically in float.
  **RX-local**: tone magnitudes only affect this receiver's frame-type scoring,
  never the transmitted signal, so it is fixable unilaterally. Marginal effect;
  bundle it with any `improved`-mode receiver rework.

### 5. Leader-length `ceil()` over integer division — benign

In `AcquireFrameSyncRSB` (`SoundInput.c:2027`), the received leader length is
`intLeaderRcvdMs = (int)ceil((intLocalPtr - 30) / 12)`. The division is
integer, so it has already truncated before `ceil` sees it; `ceil` is a no-op
and the result floors rather than rounds. The value feeds ARQ timing
optimization (`EncodeConACKwTiming`), so it is mildly under-reported, but well
within tolerance.

- **Handled in `core/` by** reproducing the value exactly (`core/modem/demodulate.c`,
  `ardop_demod_frame_sync`), with a comment noting the `ceil` is a no-op.
- **`improved` opportunity** — trivial (`round((local_ptr - 30) / 12.0)`), but
  the effect is sub-millisecond. Recorded for completeness; not worth a mode.

---

## How to add to this catalog

When a port turns up another one, add a numbered entry with: where it lives
(`file:line`), what the accident is, the **category** (TX-normative / RX-local /
benign), how `core/` currently reproduces it, and what `improved` mode might do
instead. Keep the TX-normative vs RX-local distinction sharp — it decides
whether a fix needs the far end's cooperation or not.
