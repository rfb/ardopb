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

### 6. Busy detector's rolling average is dead — RX-local

`BusyDetect3` (`SoundInput.c` → `BusyDetect.c:60,71,86`) means to keep a slow
rolling average of the signal-to-noise ratio: `dblAvgStoNNarrow = (1 - alpha) *
dblAvgStoNNarrow + alpha * ratio`. But `dblAvgStoNNarrow` is a function-local
re-initialised to `0` every call — the persistent `dblAvgStoNSlowNarrow` /
`dblAvgStoNFastNarrow` / `...Wide` globals it presumably meant to use are
declared (`BusyDetect.c:20-23`) and never read or written anywhere. So the
`(1 - alpha) * dblAvgStoNNarrow` term is always `(1 - alpha) * 0`, and the
"average" collapses to a plain `alpha` (0.2) gain on the current ratio — except
on the first call after a bandwidth change, which takes the other branch and
uses the *full* ratio. The result is a 5x sensitivity step between the first
post-change call and every call after it, and no temporal smoothing at all.

- **Category.** **RX-local**: busy detection only gates whether *this* station
  decides to transmit; it never touches the waveform. Two stations can run
  different busy logic and still interoperate.
- **Handled in `core/` by** reproducing it exactly (`core/modem/busy.c`,
  `ardop_busy_detect`): `ston_narrow`/`ston_wide` are locals zeroed each call,
  with a comment marking the preserved accident. `test/core/test_busy.c` pins
  20 000 randomised steps to `BusyDetect3` bit-for-bit, so the behaviour cannot
  drift while it is preserved.
- **`improved` opportunity** — give the average real persistence (store
  `ston_narrow`/`ston_wide` in the detector state across calls). That would add
  the intended temporal smoothing and remove the 5x first-call step, plausibly
  reducing both nuisance trips and missed detections. The thresholds (the `3`
  and `5` constants) were calibrated against the accidental 0.2 gain, so they
  would need re-tuning together with the fix — a good candidate for `improved`
  mode once there is a way to measure busy-detector quality.

### 7. 4FSK quality carries plot geometry into its own maths — RX-local

`Update4FSKConstellation` (`SoundInput.c:3823`) computes the decode quality that
is reported back to the sender (it rides in the ACK/NAK type and drives
gear-shifting). Its running `intRad` — the distance of a symbol from the
constellation centre — is rescaled by `PLOTRADIUS / 50` (= `42/50`) *after* each
symbol's contribution is accumulated, and that rescaled value is a plot
coordinate, not a distance. Because `intRad` is a single variable reused across
symbols, a symbol whose four tones sum to zero skips the recompute and
accumulates the *previous* symbol's plot-scaled radius instead of a real
distance. So the quality of a frame containing a dead symbol depends on a number
that exists only for drawing the OLED constellation.

- **Category.** **RX-local**: quality is a heuristic the receiver reports; it
  affects the *sender's* gear-shifting, not the wire format. It is a
  [05](05-essential-vs-incidental.md) "normative-ish" value — preserve and
  measure — not a free parameter.
- **Handled in `core/` by** reproducing it exactly (`core/modem/rxquality.c`,
  `ardop_quality_4fsk`): the `rad = rad * 42 / 50` rescale is kept and carries
  across symbols, and the `max(5, …)` floor stays inside the `sum > 0` guard so a
  dead symbol reuses the scaled value. `test/core/test_rxquality.c` pins 20 000
  randomised patterns (including all-zero symbols) to `Update4FSKConstellation`
  bit-for-bit.
- **`improved` opportunity** — compute the distance without the plot rescale and
  reset it per symbol, so a dead symbol contributes a defined value rather than a
  leftover coordinate. The `2.7` calibration constant assumes the current
  distribution, so it would move with the fix; worth it only alongside a
  quality-vs-SNR measurement.

---

## Not the modem: the CM108 keying report

An accident of the same shape, in the platform layer rather than the waveform,
recorded here because the reasoning is identical.

`shell/ptt_cm108.c` writes a **five-byte** HID output report to key a C-Media
GPIO pin. The chip's datasheet describes four. direwolf's `cm108.c` — the
implementation every interface in the field has actually been tested against —
writes five, with the comment:

> *"Writing 5 bytes works. I have no idea why. From the CMedia datasheet it
> looks like we need 4."*

- **Category** — not TX-normative in the on-air sense; nothing about the waveform
  depends on it. But it is *interop-normative with hardware*, which is the same
  problem pointed at a device instead of a peer: the behaviour that matters is
  what the chips in operators' hands do, not what the document says they do.
- **How `shell/` reproduces it** — `ardop_cm108_report` emits five bytes, and
  `test/core/test_ptt.c` pins them for every GPIO pin and both states.
- **`improved` opportunity** — none until somebody with hardware can compare. This
  project has no CM108 interface, so a four-byte report would be a change made on
  the strength of a datasheet against an implementation with a decade of field
  use. If a device is ever found that rejects five bytes, that is the evidence to
  act on.

The general rule this illustrates: when a reference implementation and its
documentation disagree, and you cannot test, follow the implementation and write
down that you did.

---

## How to add to this catalog

When a port turns up another one, add a numbered entry with: where it lives
(`file:line`), what the accident is, the **category** (TX-normative / RX-local /
benign), how `core/` currently reproduces it, and what `improved` mode might do
instead. Keep the TX-normative vs RX-local distinction sharp — it decides
whether a fix needs the far end's cooperation or not.

---

## A port decision that lost data: `SaveQueueOnBreak`

`iss_yield_on_break()` used to clear `tx_len` when the IRS took the link, with
the note that ardopcf's `SaveQueueOnBreak` -- the option that let the application
restore the data -- had been dropped in the port.

Dropping the *option* is fine. Dropping the *data* was not, and it contradicted
this machine's own rule 3.4 a thousand lines further down, where the IRS
deliberately sends its BREAK on a **still-unacked** frame *"so the ISS keeps that
frame for after the turnover"*. The ISS cannot keep a frame it has just thrown
away.

The cost was silent: bytes the host had been told were accepted vanished with no
NAK, no fault and no counter, and the sender saw the frame acknowledged. A file
transfer over a link that turns over -- which is every ARQ file transfer, because
the receiver has to acknowledge at its own level -- lost a block per turnover.

**This is not a normative accident.** Nothing about it is observable on the air:
the discarded bytes had not been transmitted, or had been transmitted and
deliberately left unacknowledged so that they would be sent again. It is recorded
here because it is the same *shape* as one -- a port decision, written down as
deliberate, that turned out to change behaviour nobody intended -- and because
"we dropped that option" is exactly the kind of note that reads as harmless twice
and costs a day the third time.

Fixed, with `test_loopback_turnover_loses_nothing` as the regression: it fails
without the fix and passes with it.
