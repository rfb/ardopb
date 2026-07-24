# 03 — The timing model

This is the document that explains "issues on different hardware". The short
version:

> **The audio device is the system's real clock, but the protocol reads
> `CLOCK_MONOTONIC` and assumes the device runs at exactly 12000 Hz. Everything
> else follows from that one substitution.**

---

## Where time comes from

`Now` is a macro (`ARDOPC.h:46`) for `getTicks()` (`ALSASound.c:270`):

```c
unsigned int getTicks()
{
    // When decoding a WAV file, return WavNow, a measure of the offset
    // in ms from the start of the WAV file.
    if (DecodeWav[0][0])
        return WavNow;

    clock_gettime(CLOCK_MONOTONIC, &tp);
    return (tp.tv_sec - time_start.tv_sec) * 1000
         + (tp.tv_nsec - time_start.tv_nsec) / 1000000;
}
```

Two clocks, selected by a global that means "we were started with `--decodewav`":

| | Source | Resolution | Advances |
|---|---|---|---|
| Live | `CLOCK_MONOTONIC` | 1 ms | with wall time |
| WAV decode | `WavNow` (`ARDOPCommon.c:455`) | 20 ms | with samples consumed |

`WavNow` is incremented by `blocksize * 1000 / 12000` per block
(`ARDOPCommon.c:552`) — i.e. it *is* a sample counter, scaled to milliseconds.

**This is the single most important fact in the review.** The codebase already
contains a working abstract clock; it is just switched on by a command-line flag
rather than injected, and it only covers the receive path.

---

## What depends on `Now`

Everything the protocol does about time. All of these are `unsigned int`
milliseconds compared with `>`:

| Variable | Set at | Meaning |
|---|---|---|
| `dttNextPlay` | `ALSASound.c:1847` | when to repeat an unacknowledged frame |
| `tmrSendTimeout` | `ARQ.c` | ARQ session timeout → send `DISC` |
| `tmrIRSPendingTimeout` | `ARQ.c:1241` | 10 s to complete an inbound connection |
| `tmrFinalID` | `ARQ.c:1156` | when to send the closing ID frame |
| `tmrPollOBQueue` | `ARDOPC.c:1971` | 10 s outbound-queue poll |
| `DecodeCompleteTime` | `SoundInput.c:1101` | when the last frame finished decoding |
| `dttLastLeaderDetect` | `SoundInput.c:878` | starts the 1000 ms frame-sync deadline (`:978`) |
| `pttOnTime` | `Modulate.c:694` | when PTT was keyed — used to estimate TX end |
| `LastIDFrameTime` | `Modulate.c:695` | 10-minute regulatory ID timer |
| `PKTLEDTimer` | `ALSASound.c:292` | GUI LED decay |

Note the mixture: regulatory obligations (10-minute ID), protocol deadlines
(ARQ timeout), DSP deadlines (frame-sync window), and GUI cosmetics all read the
same global clock through the same macro. Nothing distinguishes "this must be
real elapsed time" from "this must track the audio stream".

---

## The core defect: two clocks compared against each other

`SoundFlush()`, `ALSASound.c:1786-1797`:

```c
// samples sent is is in SampleNo, time started in mS is in pttOnTime.
// calculate time to stop
txlenMs = SampleNo / 12 + 20;   // 12000 samples per sec. 20 mS TXTAIL

ZF_LOGD("Tx Time %d Time till end = %d", txlenMs, (pttOnTime + txlenMs) - Now);

if (strcmp(PlaybackDevice, "NOSOUND") != 0)
    while (Now < (pttOnTime + txlenMs))
    {
        usleep(2000);
    }
```

Read carefully, this:

1. counts **samples handed to ALSA** (`SampleNo`),
2. converts to milliseconds using the **nominal** rate (`/ 12`, i.e. 12000 Hz),
3. adds that to a **wall-clock** timestamp (`pttOnTime`),
4. and busy-waits on **wall-clock** until it elapses,
5. then immediately drops PTT (`ALSASound.c:1849`).

If the card's true rate is 12000 × (1 + ε), the audio takes
`txlenMs × (1 − ε)` of real time but the loop waits `txlenMs`. For ε > 0 the
radio is held keyed slightly too long (harmless); for ε < 0 **PTT drops while
audio is still queued** and the tail of the frame is truncated. Either way the
turnaround timing that the far end measures is wrong by ε × frame length.

There is no feedback from the device here at all. `snd_pcm_delay()` /
`snd_pcm_avail_update()` would report the true drain state — the commented-out
block immediately below (`ALSASound.c:1799-1830`) shows a previous attempt to do
exactly that, abandoned with the note *"Some cards seem to stop sending but not
report not running"*.

Observed live: the log line above prints `Tx Time 1770 Time till end = 1770` for
an ID frame under `NOSOUND` — 1.77 s of audio whose completion is predicted
purely arithmetically.

---

## `FixTiming`: detecting the mismatch, and failing hard

`OpenSoundPlayback()` verifies ALSA's own numbers are self-consistent
(`ALSASound.c:1111`):

```c
if (FixTiming && (intPeriodTime * intRate != periodSize * 1000000)) {
    /* "ERROR: Inconsistent playback settings: %d * %d != %lu * 1000000.
        ... You may find that ardopcf is usable with your hardware/operating
        system by using the -A command line option." */
    return false;
}
```

`period_time` (µs) × `rate` (Hz) should equal `period_size` (frames) × 10⁶. When
it doesn't, the device's effective rate is `periodSize × 10⁶ / intPeriodTime`,
not `intRate` — and the code says so precisely in the `-A` warning path
(`ALSASound.c:1132-1146`), including the ppm error and the spec tolerance
(±100 ppm fine, ±1000 ppm degraded).

**The check is correct and well-reasoned.** The problem is what it implies: the
program cannot tolerate a sample rate it did not choose, so it refuses to run.
`-A` ("DO NOT use except for testing/debugging") disables the check without
fixing anything — the drift is simply accepted.

An architecture that derived time *from the sample stream* would not need this
check, because a rate error would no longer be a timing error. It would remain a
DSP concern (the demodulator must tolerate the far end's ±ppm, which it already
does via `dblOffsetHz` tracking), rather than a protocol correctness concern.

**On this machine:** `hw:3,0` (ALC892) rejects 12000 Hz outright — `cannot set
playback sample rate (Invalid argument)`, since the card supports 44100–192000.
Via `plughw:3,0` ALSA's rate-converting plug layer supplies a consistent period
and the `FixTiming` check passes. So this hardware falls on the passing side; the
check exists for configurations where the plug layer or driver reports otherwise.

---

## `SlowCPU`: a CPU-speed knob that is wired to nothing

**Corrected.** An earlier draft of this document described `SlowCPU` as a live
host command making decode outcome depend on the operator's machine. Checked
against the tree, that is wrong, and the truth is more mundane: the flag is
unreachable.

`SoundInput.c:886-897`, inside the leader search:

```c
if (SlowCPU)
{
    nSamples -= 480;
    Samples += 480;  // advance pointer 2 symbols (40 ms)  // reduce CPU loading
}
else
{
    nSamples -= 240;
    Samples += 240;
}
```

Were it set, the leader search would step in 40 ms hops instead of 20 ms — half
the correlation attempts, so a marginal signal acquired on a fast machine could
be missed on a slow one.

It is never set. The complete set of references in the tree is four:

```
src/common/ARDOPC.c:115      BOOL SlowCPU = FALSE;     <- the only assignment
src/common/ARDOPC.h:397      extern BOOL SlowCPU;
src/common/ardopcommon.h:345 extern BOOL SlowCPU;      <- the duplicate header
src/common/SoundInput.c:888  if (SlowCPU)
```

There is no host command, no command-line option, and no other assignment
anywhere in `src/` or `lib/` — verified case-insensitively across the whole
repository. `git log -S` finds no commit that ever removed a setter; the flag
arrived with the directory reorganisation and appears to be inherited from
`ardopc`/`ARDOP_Win`. So the `else` branch always runs, and the 480-sample hop
is dead code.

Two consequences, both smaller than the original claim:

- **Decode outcome does not depend on CPU speed.** This is not a live source of
  the hardware-dependence symptom, and it should not be counted as one.
- It is still worth deleting, but as dead code rather than as a defect — and
  deleting it is now a provably behaviour-preserving change, which makes it a
  good first exercise for the Stage 0 corpus.

Recorded rather than quietly amended because the original claim is the kind an
architecture review is prone to: a plausible reading of one `if` statement,
generalised without checking whether the condition can ever be true.

---

## Blocking waits inside protocol logic

There is no scheduler, so waiting is done by calling `txSleep()`
(`ALSASound.c:587`):

```c
void txSleep(int mS)
{
    TCPHostPoll();
    WebguiPoll();
    if (strcmp(PlaybackDevice, "NOSOUND") != 0)
        Sleep(mS);
    if (PKTLEDTimer && Now > PKTLEDTimer) { ... }
}
```

Call sites that matter:

| Site | Wait | Why |
|---|---|---|
| `ARQ.c:1127` | `250 + extraDelay − timeSinceDecoded` | enforce link turnaround before replying |
| `SoundInput.c:1074` | 250 ms | same, on the IRStoISS fast path |
| `ALSASound.c:1391` | 100 ms, looping | ALSA playback buffer full |
| `FEC.c:285` | 400 ms | inter-frame gap |

### The reentrancy chain

`txSleep()` calls `TCPHostPoll()`, which calls `ProcessCommandFromHost()`
(`TCPHostInterface.c:279`), which can transmit synchronously:

- `TXFRAME` → `txframe()` → `Mod4FSKDataAndPlay()` (`txframe.c:115` and ~10 more)
- `FECSEND TRUE` → `StartFEC()` (`HostInterface.c:543`)

and `Mod4FSKDataAndPlay()` → `SoundCardWrite()` → `txSleep()`. So:

```
Mod4FSKDataAndPlay → SoundCardWrite → txSleep → TCPHostPoll
                  → ProcessCommandFromHost → txframe → Mod4FSKDataAndPlay
```

**A transmission can begin inside a transmission.** The modulator's state is all
file-scope globals (`SampleNo`, `Number`, `DMABuffer`, the resonator arrays
`dblZout_0/1/2[32]`, `Last120[]`) with no reentrancy guard, so the inner call
overwrites the outer call's filter state and buffer position.

This is latent rather than routinely hit — it needs a host command to arrive
during the narrow window when the ALSA buffer is full. But nothing prevents it,
and the failure would be a corrupted transmission with no diagnostic. The
`SoundIsPlaying` flag exists (`Modulate.c:700`) and `MainPoll()` checks it
(`ARDOPC.c:1982`, with the telling comment *"check playing in case we call from
txSleep()"*) — but the host-command path does not check it.

---

## Half-duplex device juggling

ARDOP is half duplex, but the *device handling* is more aggressive than that
requires:

| Step | Code | Effect |
|---|---|---|
| TX starts | `initFilter()` → `StopCapture()` | `Capturing = FALSE` |
| First buffer write | `SoundCardWrite()` `ALSASound.c:1366` | **`snd_pcm_close(rechandle)`** — capture device closed |
| TX ends | `SoundFlush()` `ALSASound.c:1863` | `OpenSoundCapture()` — reopened from scratch |
| | `ALSASound.c:1864` | `StartCapture()` → `DiscardOldSamples()`, `ClearAllMixedSamples()` |

Closing and reopening the capture device per transmission means: the reopen can
fail (the `strFault` from `OpenSoundCapture` at `ALSASound.c:1863` is **collected
and never checked**), device reopen latency is added to every turnaround, and any
audio arriving during the gap is gone.

`#ifdef SHARECAPTURE` blocks (`ALSASound.c:1714`, `:1835`) suggest this was to let
another application share the device. The define is not set in the `Makefile`, so
that path is dead — but the unconditional `snd_pcm_close` at `:1366` remains.

---

## Protocol timing constants

These affect on-air interoperability and are **not** free to change in a rebuild.

| Constant | Value | Where |
|---|---|---|
| Link turnaround before reply | 250 ms (+ `extraDelay`) | `ARQ.c:1127`, `SoundInput.c:1074` |
| Frame repeat interval, data | 1500/1700/1900/2100 ms by bandwidth | `ARQ.c:879-891` |
| Frame repeat interval, IDLE | 2000 ms | `ARQ.c:947`, `:1795` |
| Repeat interval floor | 1000 ms | `ARQ.c:250` |
| FEC repeat interval | 400 ms | `FEC.c:112` |
| DISC repeat limit | 5 | `ARQ.c:373` |
| Inbound connection timeout | 10 s | `ARQ.c:1241` |
| Frame-sync window after leader | 1000 ms | `SoundInput.c:978` |
| Final-ID delay after END | 3000 ms | `ARQ.c:1156` |
| Regulatory ID interval | 540000 ms (9 min, for a 10 min rule) | `ARDOPC.c:1965`, `ARQ.c:1787` |
| TX tail | 20 ms | `ALSASound.c:1789` |

### Vestigial adaptive timing

Two mechanisms that the code appears to have but does not:

**`CalculateOptimumLeader()` (`ARQ.c:531-535`) is entirely commented out.** Its
output, `intCalcLeader`, is therefore never assigned and stays 0. It is passed as
the leader length for FEC data frames (`FEC.c:261-272`) and for
`RemodulateLastFrame()` (`Modulate.c:626-632`). This is harmless only because
`SendLeaderAndSYNC()` treats 0 as "use the default":

```c
if (intLeaderLen == 0)
    intLeaderLenMS = LeaderLength;      // Modulate.c:74-77
```

The maintainer's TODO at `HostInterface.c:862-867` notes this and asks whether
re-implementing it would improve reliability. Flagging it here because a naive
rebuild that "cleans up" the 0 default would silently change on-air behaviour.

**`ComputeInterFrameInterval()` (`ARQ.c:248`) is `max(1000, requested + intRmtLeaderMeas)`,
and `intRmtLeaderMeas` is never assigned** — it is defined twice in the same
translation unit (`ARQ.c:44` initialised to 0, `ARQ.c:124` as a tentative
definition) and read once. There is also a near-homonym `intRmtLeaderMeasure`
(`ARDOPC.c:175`, externed at `ARQ.c:21`) which is likewise never assigned. So the
function reduces to `max(1000, requested)`, and the repeat intervals are the
literal constants in the table above.

**`Track1Car4FSK()` (`SoundInput.c:4063`) — a sample-rate-offset tracker that is
both broken and disabled.** Its own comment states the intent:

> *"This should handle sample rate offsets (sender to receiver) up to about 2000
> ppm"*

That is precisely the problem this document is about — and there is a mechanism
for it. Except:

1. Its symbol-timing adjustment is a no-op. `*intPtr --` (`:4081`) parses as
   `*(intPtr--)`: dereference and discard, then decrement a by-value pointer
   parameter whose change is lost at return. `(*intPtr)--` was meant. GCC's
   `-Wunused-value` reports it ([08](08-style-and-tooling.md)).
2. Its only call site (`SoundInput.c:2724`) is commented out, so it is dead code.

Understand this before designing rate-offset handling in a rebuild — someone
already concluded it was needed and wrote it. Whether it was disabled because of
the bug, because it was too costly, or for some third reason is not recoverable
from the source; the users' group archives or `git log` on the upstream `ardopc`
lineage may say.

All three are cases where the *intended* design is adaptive and the *actual*
behaviour is fixed. A rebuild should decide deliberately which it wants rather
than inheriting the ambiguity.

---

## Summary: the five timing problems

1. **Two clocks compared to each other.** Sample-count time and wall-clock time
   are added together in `SoundFlush()`. Fixing this is the single highest-value
   change available.
2. **No feedback from the device.** TX completion is predicted, never observed.
3. **Rate variation is fatal rather than absorbed.** `FixTiming` converts a
   hardware property into a startup failure.
4. **Waiting re-enters the program.** `txSleep()` makes every wait a potential
   reentrancy, including TX-inside-TX.

(`SlowCPU` was listed here in an earlier draft. It is dead code, not a live
hazard — see above.)

Every one of these dissolves under the "one clock, derived from the sample
stream, and no blocking in the core" rule proposed in
[06](06-target-architecture.md). None of them require changing the protocol.
