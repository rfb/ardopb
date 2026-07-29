# Golden vectors

A frozen record of what ardopcf's modulator emits and what its demodulator
recovers, captured so that any later change — a refactor, a restructure, a
rewrite in another language — has an objective pass/fail instead of "it still
seems to work on my radio".

This is Stage 0 of [`analysis/07-migration-path.md`](../../analysis/07-migration-path.md).
It is worth having even if no rebuild ever happens: it is a regression test for
the parts of ardopcf that are hardest to eyeball and most expensive to get
wrong.

```sh
make golden          # check this build against the corpus
make golden-regen    # rewrite the corpus from this build (moves the baseline)
```

Both need only `python3`. No libraries, no audio hardware, ~6 seconds.

---

## What is in here

| | |
|---|---|
| `manifest.json` | The corpus. 130 cases: expected TX hash, and expected decode result for each. |
| `audio/*.wav.gz` | 17 frozen recordings, gzipped. Real audio for a subset of cases, including degraded copies. |
| `ardop_golden.py` | Shared machinery: the case matrix, the ardopcf drivers, the output parser. |
| `gen_golden.py` | Writes the corpus. |
| `test_golden.py` | Checks the inherited `ardopcf` build against the corpus. Never writes to it. |
| `core_decode_wav.c` | Decodes a WAV through the **rebuilt core** (`ardop_demod_push`, receive-only). Links only `core/`. |
| `test_golden_core.py` | Checks the core demodulator against the frozen recordings. Never writes to the corpus. |

Generation and checking are separate scripts on purpose. A checker that can
quietly rewrite its own expectations is not a checker.

### The 130 cases

- **22 control frames** — every non-data frame type, with fixed example
  parameters.
- **108 data frames** — all 18 data frame types × even/odd × three fill levels
  (100%, 80%, 10% of capacity). Partially filled frames are zero-padded, which
  produces a visibly different audio pattern, so they are worth covering
  separately.

Each case is decoded twice, through the standard demodulator and through the
experimental `--sdft` one, since those are genuinely different code paths.

---

## What is asserted, and how strongly

Not every recorded number deserves to fail a build, so checks are tiered.

**Tier 1 — always enforced**

- *The modulator is bit-exact.* Re-modulating a case must reproduce
  `tx_sha256` exactly. This is the strongest available statement of on-air
  compatibility that can be made without a radio.
- *The receiver is correct.* Every case must decode, and must report the frame
  type, session ID, frame length and payload the manifest records. Payload is
  checked against the bytes originally fed to the modulator, not merely against
  the recorded output — a corpus that only agrees with itself would keep
  passing even if the recorded value were wrong to begin with.
- *Frozen audio still decodes*, including degraded copies at or above the
  strict SNR floor (15 dB).

**Tier 2 — reported, enforced only with `--strict`**

- Quality scores and Reed–Solomon correction counts. These are heuristics, not
  protocol. Drift in them is worth seeing — it is an early warning that the
  demodulator is working harder — but it is not by itself a conformance
  failure.

**Corpus integrity — always enforced**

- Every committed audio file must hash to what the manifest says, so a
  corrupted or partial checkout is reported as such instead of surfacing as a
  hundred mysterious decode failures.

### The core conformance check (`make golden-core`)

`test_golden_core.py` runs the same frozen recordings through the rebuilt
demodulator instead of the inherited one, via the `core_decode_wav` harness.
This is the definitive external oracle for `core/modem/demodulate.c`: real,
recorded ardopcf audio — not the core's own modulator output — decoded by
`core/` alone, including the noise-degraded copies.

Its rule is deliberately asymmetric. A *wrong* decode (wrong frame type or
wrong payload) is always a failure. A *miss* is judged by context: tolerated
below the strict SNR floor (where the inherited decoder misses too), and an
expected gap for the frame types the core does not yet decode (`IDFrame` /
`ConReq` content decode), which the driver lists explicitly rather than
hiding. The ground truth for every variant is its
clean `decode` payload, so a degraded copy that decodes to anything else is
caught.

Getting the right bytes where the inherited *standard* decoder gave up is not
a violation — the core's DSP and RS are proven bit-identical to the standard
reference functions, so a threshold frame the inherited SDFT decoder also
recovers, matched byte-for-byte, is within tolerance. One recorded case
(`16QAM.2000.100.E` at 20 dB) is exactly this: the standard decoder records a
`data` failure, the SDFT decoder recovers it, and the core recovers the same
bytes.

### The assembled-shell cutover checks (`make golden-shell`, `make golden-tx`)

These prove the rebuilt **program** — not just isolated core functions —
reproduces the corpus, the first cutover milestone (analysis/13 W3.1).

- **`make golden-shell`** decodes every frozen recording through the assembled
  shell: `shell_decode_wav` feeds each WAV to `ardop_runtime_rx` and rebuilds
  each frame from the observation stream (`RX_FRAME` + `RX_DATA`), exercising the
  runtime, the receive-only link step and the observer. It reuses
  `test_golden_core.py`'s judging verbatim (via `GOLDEN_DECODE_BIN`) and passes
  identically to `golden-core` — the shell wires the demodulator exactly as the
  core does.

- **`make golden-tx`** proves the shell's *transmit* audio is bit-identical.
  `shell_tx_wav` re-encodes each data case (`ardop_encode_data_frame`) and
  modulates it with the same drive level (30) and leader (240 ms) the runtime's
  `start_tx` uses, writing a WAV in `--writetxwav` byte format; the SHA-256 must
  equal the manifest `tx_sha256`. All 108 data cases match — every modulation
  (4FSK / 4PSK / 8PSK / 16QAM), 1–8 carriers, 200–2000 Hz. Control-frame
  encoders are not yet exposed standalone, so control cases are out of scope for
  the TX check (they are still covered on the RX side).

### Why bit-exact TX matters

While building this, the modulator was perturbed with a plausible refactoring
slip — `Sample * DriveLevel / 100` rewritten as `Sample / 100 * DriveLevel`,
which changes integer truncation. The result:

- **130 Tier 1 failures** — every case's TX hash changed.
- **0 decode failures** — every frame still decoded, with the correct payload.
- **25 Tier 2 drifts** — Reed–Solomon had to correct more errors.

A test that only checks "does it still decode" would have passed that change
silently, while the transmitted signal had measurably degraded. That is the
case for hashing the audio rather than only round-tripping it.

---

## What this corpus does **not** cover

State these plainly, because the corpus is easy to over-trust.

- **No ARQ.** `--decodewav` forces RXO mode, in which `CheckTimers()`,
  `TCPHostPoll()` and `MainPoll()` never run. Nothing here exercises the
  connection state machine, ACK/NAK handling, turnover, gear-shifting or
  timeouts. That gap is Stage 3's two-instance test.
- **No timing.** `--decodewav` substitutes a sample-derived clock for the
  system clock, so none of the hardware-timing problems in
  [`analysis/03-timing-model.md`](../../analysis/03-timing-model.md) can appear
  here. This is a feature — it is what makes the corpus deterministic — but it
  means the corpus cannot defend Stage 2.
- **No real channels.** Additive white Gaussian noise only. No multipath, no
  fading, no frequency offset, no interference.
- **Self-consistency, not interoperability.** These vectors were produced by
  ardopcf, so they prove a rebuild matches *ardopcf*, not that ardopcf matches
  ARDOP_Win. Recordings from another implementation would be strictly better
  evidence and would be a valuable addition.

---

## Reproducing the corpus in another language

The corpus is meant to outlive this Python harness, so everything it depends on
is specified here rather than only in code.

**Payloads** come from xorshift32, taking the low byte of each state update:

```c
uint32_t x = seed;            /* manifest: payload_seed, never 0 */
for (i = 0; i < payload_len; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = x & 0xFF;
}
```

Chosen over a language's built-in RNG precisely because it is four lines in any
language. Seeds are stored explicitly per case, derived from the case ID, so
adding or reordering cases does not disturb existing payloads.

**Audio** is 16-bit signed mono PCM at 12000 Hz, generated at `DRIVELEVEL 30`.
The drive level was verified not to matter to decoding — `rs_fixed` and
`quality` are identical from `DRIVELEVEL 10` through `100`, since the
demodulator normalises amplitude — so 30 is kept for the headroom it leaves
when noise is added without clipping.

**Noise** is AWGN at a stated SNR relative to the RMS of the non-zero samples,
with Gaussian values from Box–Muller over the same xorshift32 stream. The
generated `.wav.gz` is the authoritative artifact; that recipe is recorded for
provenance, not as something a reimplementation must reproduce bit-for-bit,
because `log`/`cos`/`sin` differ in the last place between libm
implementations.

---

## Why so little frozen audio

Hashes cover all 130 cases and cost nothing. Real audio is committed only where
it buys something a hash cannot:

- a receiver under development can be tested before its own transmitter is
  trustworthy;
- degraded copies cannot be regenerated at all without reproducing the noise
  bit-for-bit.

The set is kept small deliberately. Regenerating rewrites every file, and git
stores a fresh blob each time, so a large frozen set costs repository size on
*every* regeneration rather than once. Noise is also incompressible: a noisy
vector is roughly 7× the size of the clean one it came from (100 KiB vs 14 KiB
for `4PSK.200.100.E`), which is why degraded copies exist for only four frame
types at two SNRs.

Current cost: ~1.0 MiB of audio plus a 225 KiB manifest.

---

## Findings from building it

Two things fell out of generating the corpus that are worth recording.

**The 8-carrier modes have far less margin than their FEC budget suggests.** On
a clean, noise-free, hardware-free signal:

| Frame type | Carriers | RS corrections used | Quality |
|---|---|---|---|
| `4FSK.200.50S.E` | 1 | 0 / 2 | 100 |
| `4PSK.200.100.E` | 1 | 0 / 16 | 85 |
| `16QAM.200.100.E` | 1 | 0 / 32 | 97 |
| `8PSK.2000.100.E` | 8 | 26 / 144 | 68 |
| `16QAM.2000.100.E` | 8 | 105–127 / 256 | 56 |

The widest, densest mode consumes roughly half its error-correction capacity
before any channel impairment exists at all, and the effect scales with carrier
count and constellation density — consistent with inter-carrier interference in
the modulator/demodulator pair. It is not a level or quantisation artifact: the
figures are unchanged across drive levels 10–100.

**The two demodulators disagree at usable signal levels.**
`16QAM.2000.100.E` at 20 dB SNR fails on the standard demodulator and succeeds
on the experimental SDFT one. Both outcomes are frozen in the corpus.

Neither is a defect this corpus was looking for; both are the kind of thing a
frozen baseline exists to make visible.

---

## Maintaining it

Regenerate only when you intend to move the baseline, and read the diff. The
generator reports added, removed and TX-changed cases, and refuses to hide a
case that stopped decoding.

`manifest.json` is sorted and one-key-per-line so its diffs are reviewable. The
gzip containers are written with `mtime=0`, so an unchanged vector produces no
diff at all.

The corpus records the ardopcf version and git commit that produced it;
`test_golden.py` prints a note when the build under test differs.
