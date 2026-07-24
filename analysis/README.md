# ardopcf — architecture review

Local technical review of `ardopcf`, written to answer two questions:

1. **How does this software actually work?** — enough to rebuild it.
2. **Why is it hard to build on different hardware and hard to test?**

## Provenance and status

- Written against commit `a7c9228` (v1.0.4.1.3), inherited from
  `pflarue/ardop`.
- **This is a hard fork.** The review was originally written under the
  assumption that changes might flow back upstream; that is no longer the plan,
  and `docs/CONTRIBUTING.md` no longer binds. Where a conclusion rested on
  upstream mergeability it has been revisited — see [06](06-target-architecture.md).
  Protocol compatibility is a separate commitment and **still binds**: on-air
  interop with ARDOP_Win/ardopc per the 2017 spec, and the existing TCP host
  interface.
- Kept out of `docs/` on its own merits: `docs/` is documentation of how to
  build and use the program, this is a point-in-time review of how it works.
  Different lifetimes, different audiences.
- **Produced with AI assistance and not line-by-line reviewed by a human.**
  This is stated because it is true and load-bearing, not because a policy
  requires it. Claims were checked against a live build where they could be;
  where a claim is reasoning rather than observation it says so. At least one
  claim was wrong on first writing and has been corrected in place with the
  error left visible (`SlowCPU`, in [03](03-timing-model.md)).
- Claims were checked against a live build where possible. Where a claim is
  reasoning rather than observation, it says so. Verification notes are at the
  end of [01](01-signal-chain.md) and inline elsewhere.

## Reading path

| | Document | For |
|---|---|---|
| 00 | [Overview](00-overview.md) | Orientation: what ARDOP is, the runtime shape, the four entangled concerns, source map |
| 01 | [Signal chain](01-signal-chain.md) | TX and RX end to end with `file:line` traces; where the two chains illegally touch |
| 02 | [Protocol FSM](02-protocol-fsm.md) | The ARQ state machine recovered as a state/event table |
| 03 | [Timing model](03-timing-model.md) | **The hardware-dependence problem.** Start here if that's the pressing question |
| 04 | [Coupling map](04-coupling-map.md) | **The testability problem**, quantified. Includes a confirmed silent RX defect |
| 05 | [Essential vs incidental](05-essential-vs-incidental.md) | What a rebuild must preserve bit-exactly, what it may reimplement, what it may discard |
| 06 | [Target architecture](06-target-architecture.md) | Proposed shape; sans-I/O core; the language evaluation |
| 07 | [Migration path](07-migration-path.md) | Staged route, working radio at each step |
| 08 | [Style and tooling](08-style-and-tooling.md) | A deliberate style; warnings, LTO, sanitizers, CI |
| 09 | [Embedding and bindings](09-embedding-and-bindings.md) | Driving the core from a higher-level language |

**Short on time?** Read 03 and 04 — they contain the answers to the two
motivating questions.

**Stage 0 is built.** The golden-vector corpus that 07 argued for now exists in
[`test/golden/`](../test/golden/README.md): `make golden` checks this build
against 130 frozen cases in about six seconds, with no audio hardware. It was
the one item worth doing whether or not a rebuild ever happens, and it is done.

## The findings in one page

**The timing problem has one root cause.** The audio device is the system's real
clock, but the protocol reads `CLOCK_MONOTONIC` and assumes the device runs at
exactly 12000 Hz. `SoundFlush()` (`ALSASound.c:1789`) predicts transmission end
by dividing a sample count by a nominal rate and then busy-waits on wall time.
Where nominal and actual disagree, protocol timing drifts. `ardopcf` detects the
disagreement (`FixTiming`, `ALSASound.c:1050-1146`) and refuses to start rather
than misbehave on air — correct, but it converts hardware variation into a hard
failure. Deriving time from the sample stream removes the failure mode rather
than detecting it.

**The testability problem is measurable.** Six unit-test executables cover
exactly the six modules that own no global state. ~350 mutable globals; 171
re-exported as `extern` through `ARDOPC.h`; 196 more `extern` declarations
directly inside `.c` files. The two "headers" duplicate 155 externs and 88
prototypes between them and contain a stale, divergent, dead copy of the core
enums. The demodulator transmits (`SoundInput.c:1070`), the modulator keys the
radio (`Modulate.c:693`), and 25 GUI calls sit inside the DSP.

**The encouraging part.** The normative surface — what must be exactly right for
on-air compatibility — is small, and much of it is data rather than logic. About
2500 lines already meet the target architecture's standards and carry across
unchanged. And the sans-I/O design is not speculative: `--decodewav` already runs
the entire receive chain from a file on a synthetic clock, which is half the
proposed architecture, shipping today.

**The compiler is not being listened to.** 177 warnings under
`-Wall -Wextra`, and no linter or formatter config. (There was also no CI; a
minimal workflow now builds, runs the unit tests and runs the golden vectors.)
Two thirds of those
warnings are tidiness, but the rest include a `fabsf()` applied to a `double` in
the demodulator, three dangling-`else`s in PTT control, and a genuine
pointer-arithmetic bug (`*intPtr--` where `(*intPtr)--` was meant) in the 4FSK
symbol-timing tracker — dormant, because its only call site is commented out.
Separately, `-flto` surfaces four cross-translation-unit type mismatches that
normal compilation cannot diagnose. See [08](08-style-and-tooling.md).

**One confirmed defect, verified not asserted.** The documented `-L`/`-R` options
are silently broken on Linux: the stereo de-interleave at `ALSASound.c:1563`
scatters samples instead of compacting them, delivering half the audio at 2×
time dilation with zeros interleaved. Decoding cannot work. On Windows the same
options are no-ops. Details and reproduction in [04](04-coupling-map.md).

## Reproducing the checks

```sh
# Build (note: fails on GCC 14+ without the flag — see 01, Verification notes)
make CFLAGS="-g -MMD -Wno-int-conversion"

# Unit tests (needs libcmocka-dev) — 26 tests, all passing
make CFLAGS="-g -MMD -Wno-int-conversion" test

# Golden vectors: 130 frozen cases, 424 checks, ~6 s, python3 only
make CFLAGS="-g -MMD -Wno-int-conversion" golden

# Full TX -> WAV -> RX round trip for every frame type, no hardware needed
cd test/python && python3 test_wav_io.py

# Observe the TX chain with no sound card
./ardopcf --nologfile 8515 NOSOUND NOSOUND \
    -H "CONSOLELOG 1;MYCALL N0CALL;TXFRAME IDFrame"
```
