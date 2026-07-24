# 04 — Coupling map

This document supports the claim that components cannot be isolated or tested,
with numbers rather than impressions.

---

## The headline number

**Test coverage tracks decoupling exactly.**

| Module | Globals | Interface? | Unit tested? |
|---|---:|---|---|
| `StationId.c` | 0 | yes — opaque type + functions | ✅ 487-line test |
| `Locator.c` | 0 | yes | ✅ 222-line test |
| `Packed6.c` | 0 | yes | ✅ 92-line test |
| `log.c` / `log_file.c` | 5 | yes | ✅ 245-line test (with `--wrap` mocks) |
| `ARDOPCommon.c` | 33 | no | ⚠️ 1 pure function only (`try_parse_long`) |
| `HostInterface.c` | 13 | no | ⚠️ 1 pure function only (`parse_station_and_nattempts`) |
| `SoundInput.c` | 108 | no | ❌ |
| `ARQ.c` | 118 | no | ❌ |
| `ARDOPC.c` | 123 | no | ❌ |
| `Modulate.c` | 27 | no | ❌ |
| `ALSASound.c` | 43 | no | ❌ |
| `FEC.c` | 29 | no | ❌ |

Every tested module has ~0 globals. Every untested module has dozens. The two
partially-tested files are tested *only* at their one pure function each.

This is not a discipline problem. `test/ardop/test_log.c` shows real care —
it uses `ld --wrap` to mock `fopen`/`fwrite`/`fflush` (`Makefile:184-189`), which
is exactly the right technique. The infrastructure exists and is used well. It
simply cannot reach code that communicates through ambient state.

---

## Globals

Counting file-scope mutable variable definitions:

```
123  src/common/ARDOPC.c
118  src/common/ARQ.c
108  src/common/SoundInput.c
 43  src/linux/ALSASound.c
 33  src/common/ARDOPCommon.c
 32  src/windows/Waveout.c
 29  src/common/FEC.c
 27  src/common/Modulate.c
 25  src/common/TCPHostInterface.c
 …
```

Roughly **350 mutable globals** in `src/`. Of these, **171** are re-exported
through `ARDOPC.h` as `extern`, making them part of the de-facto interface
between every pair of modules. A further **196** `extern` declarations appear
directly inside `.c` files — modules reaching into each other's variables without
even going through a header:

```
28  ARDOPC.c        21  ARQ.c         20  SoundInput.c    20  FEC.c
26  ARDOPCommon.c   14  ALSASound.c   14  HostInterface.c 12  Webgui.c
```

Some carry apologetic comments that document the coupling rather than remove it —
`RXO.c:7-11`:

```c
extern int stcLastPingintRcvdSN;   // defined in ARDOPC.c. updated in SoundInput.c
extern int stcLastPingintQuality;  // defined in ARDOPC.c. updated in SoundInput.c
extern int intSNdB;                // defined in SoundInput.c
```

A variable defined in one file, written by a second, read by a third, with the
data flow recorded in a comment because there is no other place to record it.
That is the coupling problem in five lines.

---

## The twin headers

`ARDOPC.h` (575 lines) and `ardopcommon.h` (531 lines) are not interfaces. They
are a shared namespace.

Measured overlap:

- **155 identical `extern` declarations** appear in both.
- **88 identical function prototypes** appear in both.
- `ardopcommon.h:1` includes `ARDOPC.h`, so everything in the former is
  additive to the latter — the duplication is pure redundancy.

Worse, `ardopcommon.h` contains a **stale divergent copy** of the four core enums
(`_ReceiveState`, `_ARDOPState`, `_ARQSubStates`, `_ProtocolMode`) wrapped in
`#ifndef ARDOPCHEADERDEFINED`. Since `ARDOPC.h` is included first and defines
that macro, the copies are **dead code** — confirmed by preprocessing: the
`GettingTone` member unique to the dead copy never appears in `gcc -E` output.

They have already drifted:

| Enum | `ARDOPC.h` (live) | `ardopcommon.h` (dead) |
|---|---|---|
| `_ReceiveState` | 7 members | 8 — extra `GettingTone` |
| `_ProtocolMode` | 4 — includes `RXO` | 3 — no `RXO` |

Not currently a bug, but a loaded gun: reordering two `#include` lines would
silently renumber `ProtocolMode` and change the meaning of every comparison
against it, with no diagnostic.

Neither header is minimal for any consumer. `Modulate.c` needs waveform templates
and sample buffers; it gets 171 externs including PTT port handles, the ARQ
bandwidth enum and Reed-Solomon statistics. There is no way to depend on part of
the system.

### Include graph

Only four leaf modules have narrow dependencies:

```
Locator.c   → Locator.h, log.h
Packed6.c   → Packed6.h
StationId.c → StationId.h, log.h
log_file.c  → log_file.h, log.h
```

Everything else pulls in `ARDOPC.h` and/or `ardopcommon.h` — i.e. everything.
The dependency graph is a star with the global namespace at the centre, which is
another way of saying there is no dependency graph.

---

## Presentation embedded in DSP and protocol

`wg_send_*` (WebGui push) call counts by file:

```
77  Webgui.c        ← expected
25  SoundInput.c    ← the demodulator
15  HostInterface.c
15  ARQ.c           ← the protocol FSM
 7  Modulate.c
 7  ARDOPC.c
 4  RXO.c
```

The demodulator makes 25 GUI calls, interleaved with signal processing — e.g.
`SoundInput.c:1058-1067`, where computing a quality value for display sits in the
middle of frame-type acquisition. Prototypes are declared ad hoc at the top of
each consumer (`ARQ.c:31-34`, `RXO.c:12-13`, `Modulate.c:29`) rather than in a
header, so even the GUI interface is informal.

Consequence: the demodulator cannot be linked without the WebGui, the websocket
server (`lib/ws_server`), and the generated HTML/JS blobs
(`src/common/gen-webgui.html.o`). Which is why every unit test executable links
**the entire program** — see `Makefile:169-181`, where each test links all 28
object files plus ALSA, pthread and librt.

---

## Layering violations

Where the call direction is backwards:

| Violation | Location |
|---|---|
| Demodulator transmits | `SoundInput.c:1070-1090` — `SetARDOPProtocolState(ISS)`, `SendData()` |
| Demodulator reads protocol state | `SoundInput.c:822`, `:864` — early return on `FECSend` |
| Demodulator services a protocol timer | `SoundInput.c:820` — `CheckMemarqTime()`, with a comment explaining it had to move here because RXO skips `CheckTimers()` |
| Modulator keys the radio | `Modulate.c:693` — `initFilter()` calls `KeyPTT(TRUE)` |
| Modulator schedules the regulatory ID | `Modulate.c:695-698` |
| Platform layer owns the main loop | `ALSASound.c:378` / `Waveout.c:333` call `ardopmain()` |
| Platform layer owns the clock | `getTicks()` in `ALSASound.c:270` |
| Protocol blocks on wall-clock inside an RX callback | `ARQ.c:1127-1130` |
| Host command handler transmits synchronously | `txframe.c:115`, `HostInterface.c:543` |

---

## Duplicated platform logic

`ALSASound.c` (2289 lines) and `Waveout.c` (1292 lines) are not two backends
behind one interface — they are two forks of the program's spine. Both contain
their own `platform_main`, `PollReceivedSamples`, `SoundFlush`, `txSleep`,
`getTicks`, PTT handling and WAV capture.

They have diverged in ways that produce different behaviour:

| Behaviour | Linux | Windows |
|---|---|---|
| `-L`/`-R` RX channel select | implemented (incorrectly — below) | **flags declared and never read** (`Waveout.c:61-62`); options silently do nothing |
| Repeat scheduling | `dttNextPlay = Now + intFrameRepeatInterval + extraDelay` (`ALSASound.c:1847`) | `dttNextPlay = Now + intFrameRepeatInterval` — **`extraDelay` omitted** (`Waveout.c:866`) |
| TX completion | busy-wait on `Now` (`ALSASound.c:1794`) | `txSleep(10)` loop (`Waveout.c:855-857`) |

The `extraDelay` divergence is a real interoperability difference: the
`EXTRADELAY` host command (documented for satellite/long-path work,
`HostInterface.c:689`) affects retry timing on Linux and not on Windows.

---

## Type-safety hazards

**Cross-TU type mismatch on the shared audio buffer.** The same object is
declared with different types in two translation units:

```c
short          buffer[2][1200];   // ALSASound.c:92   (definition)
extern unsigned short buffer[2][1200];   // Modulate.c:667  (declaration)
```

C's separate compilation makes this undiagnosable *by default* — the linker
matches on name only. **`-flto` does diagnose it**, and finds four such
mismatches across the tree including `SendtoCard`, whose signature is
`unsigned short *(unsigned short *, int)` in `Modulate.c:671` and
`short *(short *, int)` in `ALSASound.c:1581`. See
[08](08-style-and-tooling.md) for the full list.

The mismatch is currently benign because the code only moves bits through, but
`SampleSink` does arithmetic on the values it produces before storing them, and
any future comparison or shift on the buffer contents would behave differently
depending on which file it was written in. `SoundInput.c:70-73` similarly declares
sample buffers as `short` while `Modulate.c` treats the TX side as unsigned.

**Large stack allocations.**

```c
unsigned short samples[256000];  // ALSASound.c:1405 — 512 KB, PackSamplesAndSend
short          samples[65536];   // ALSASound.c:1498 — 128 KB, SoundCardRead
```

Fine on a desktop's 8 MB main-thread stack; less comfortable on the small
embedded targets this project explicitly serves, and unnecessary in both cases
(the actual transfer is 1200 and 240 frames respectively).

**Build failure on current toolchains.** `lib/rawhid/rawhid.c` declares
`hid_device *CM108Handle` (`:341`) but on Linux assigns it from `rawhid_open()`,
which returns `HANDLE` = `int` (`:223`, and `HANDLE` is `#define HANDLE int` on
non-Windows), then passes it to POSIX `read`/`write`/`close` (`:361`, `:414`,
`:419`). Three int↔pointer conversions. GCC 14 makes `-Wint-conversion` an error
by default, so **ardopcf does not build on Debian 13 / GCC 14 without
`-Wno-int-conversion`**. The root cause is the same one throughout: a type whose
meaning differs per platform, with no platform abstraction to mediate.

---

## Confirmed defect: `-L` / `-R` are broken on Linux

Included because it is a *prediction* of the architecture rather than an
independent bug, and because it lands precisely on "issues on different hardware".

`SoundCardRead()`, `ALSASound.c:1549-1567`:

```c
if (m_recchannels == 1)
{
    for (n = 0; n < ret; n++)
    {
        memcpy(input, samples, nSamples*sizeof(short));   // ← loop body is invariant
    }
}
else
{
    if (UseLeftRX) start = 0; else start = 1;

    for (n = start; n < (ret * 2); n+=2)  // return alternate
    {
        input[n] = samples[n];                            // ← does not compact
    }
}
```

**Mono branch**: the loop body does not depend on `n`. It performs the identical
480-byte `memcpy` 240 times. Harmless, but nobody has read this code in a long
time.

**Stereo branch**: it copies left-channel samples but leaves them at their
*interleaved* positions instead of compacting them to `input[0..ret-1]`. The
caller then hands `input[0..239]` to `ProcessNewSamples()` as consecutive mono
samples (`ALSASound.c:1703`).

Reachability — `ALSASound.c:1235`:

```c
if (UseLeftRX == 0 || UseRightRX == 0)
    m_recchannels = 2;  // L/R implies stereo
```

So **any use of the documented `-L` or `-R` option** takes this path, as does any
capture device that cannot do mono (the fallback at `:1245`).

Verified two ways:

1. `./ardopcf --nologfile -L 8515 plughw:3,0 plughw:3,0` logs
   `Using Left Channel of soundcard for RX` and opens a 2-channel capture.
2. Extracting the loop verbatim and running it on a known interleaved buffer,
   with the destination pre-filled with `-1` to mark slots the copy never writes:

```
card gave 8 frames of interleaved stereo:
    1000 -1000  1001 -1001  1002 -1002  1003 -1003  1004 -1004 …

what the demodulator then reads as 8 consecutive mono samples:
    1000    -1  1001    -1  1002    -1  1003    -1
                ^^          ^^          ^^   never written

expected (left channel, compacted):
    1000  1001  1002  1003  1004  1005  1006  1007
```

In the real program those odd slots are not stale-but-plausible audio — `inbuffer`
is a zero-initialised static global (`ALSASound.c:93`) and the stereo branch never
writes an odd index, so they are **permanently 0**.

The consequence is therefore worse than "half the samples are wrong": of 240 frames read,
only the first 120 left-channel samples reach the demodulator, spread across 240
slots with zeros between. That is a **2× time dilation plus zero-stuffing** — a
1500 Hz tone arrives as 750 Hz with an image, and half the audio is discarded
outright. Decoding cannot work at all.

Not mentioned anywhere in `changelog.md`. On Windows the same options are silent
no-ops. The reason this can persist undetected is structural: there is no level
at which "audio input" can be tested independently of a radio.

---

## Why this composition resists testing

To unit-test `DemodulateFrame()` today you would need to: link the whole program
including ALSA and the websocket server; initialise ~350 globals to a consistent
state with no function that does so; ensure `ProtocolState` is right because the
demodulator branches on it; accept that a successful decode may key PTT and
transmit; and account for `intCarPhaseAvg`/`intCarMagAvg` carrying state between
calls.

The existing tests avoid all of this by testing only functions that touch none of
it. That is the correct local decision, and it is why coverage stops where it
does.

Everything in [06](06-target-architecture.md) is aimed at exactly this list.
