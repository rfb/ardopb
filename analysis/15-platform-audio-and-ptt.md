# 15 — Platform audio, devices and PTT

[13](13-completing-the-rebuild.md)'s W2.2 asked for "one narrow interface behind
which ALSA and WinMM sit as thin backends". The interface was built and is good;
only one real backend was ever written. This document designs the rest of it, to
the requirements of [14](14-station-application.md): four platforms, device
selection from a UI, and PTT that works on hardware operators actually own.

---

## 1. Where we are

`ardop_platform_ops` (`shell/platform.h:44-91`) is six function pointers, three
of them required:

```c
size_t   (*read_audio)(void *ctx, int16_t *buf, size_t max);
void     (*write_audio)(void *ctx, const int16_t *buf, size_t n);
void     (*set_ptt)(void *ctx, bool key);
uint64_t (*wall_ms)(void *ctx);          /* optional */
bool     (*poll_host)(void *ctx, ardop_host_cmd *out);   /* optional */
bool     (*should_stop)(void *ctx);      /* optional */
```

No open/close in the interface — *"opening, closing and reopening the device is
entirely inside the backend"* (`shell/platform.h:32-33`). `shell/loop.c` is the
only file that calls through it. The abstraction is the right one and does not
change.

What exists behind it:

| | Capture/playback | PTT | Enumeration |
|---|---|---|---|
| Linux | ALSA, 12000 Hz mono S16_LE, blocking | serial RTS | **none** |
| Windows | — | — | — |
| macOS | — | — | — |
| Android | — | — | — |
| test | `backend_null.c`, silence + `usleep` pacing | flag | — |

**Device enumeration does not exist anywhere in the tree.** I grepped for
`snd_device_name_hint` and `snd_card_next` across `shell/`, `apps/` and `gui/`:
zero hits. Device selection is a bare string from `argv` handed to
`snd_pcm_open` (`shell/main.c:206`, `shell/backend_alsa.c:154`). Every part of
"select audio inputs / outputs from the UI" is new.

---

## 2. Decision — miniaudio for the app, keep ALSA for the daemon

**Two backends coexist behind the one interface.** `backend_alsa.c` stays exactly
as it is and remains what `ardopb` uses on Linux; a new `backend_ma.c` built on
[miniaudio](https://miniaud.io) serves the application on all four platforms.

Rationale:

- **Android is the constraint that decides it.** PortAudio's Android support is
  an unofficial OpenSL ES port; RtAudio has none. miniaudio covers
  WASAPI/CoreAudio/AAudio/OpenSL ES/ALSA/PulseAudio in one file, and its iOS
  support is the same CoreAudio path — which matters because 14 requires iOS not
  be precluded.
- **It brings enumeration**, which we would otherwise write four times.
- **It is one file with no build system.** For a project whose app must build
  under MSVC, Xcode, the NDK and gcc, a dependency with no CMake/pkg-config
  surface of its own is worth a great deal.
- **The Pi deployment gains nothing new to depend on.** `ardopb` keeps needing
  only `libasound`. That is not sentimentality: the headless daemon is the
  configuration most likely to run unattended for months, and its dependency
  footprint is a feature.

**The tension worth naming:** this project holds `core/` to zero mutable globals,
zero allocation, and mechanical proof of both. miniaudio is ~90k lines of
third-party code that honours none of that. The answer is that it sits in the
platform layer, where impurity is the point, behind six function pointers — the
same place ALSA already sits. `make check-standalone` will still prove that no
core object can reach it. If it disappoints, it is one file to replace.

**Rejected — four native backends.** Correct, and what a mature project ends up
with. It is four times the work and three of the four are platforms nobody here
can test on daily. Revisit per-platform if miniaudio's behaviour on a specific
target proves unacceptable; the interface makes that a local change.

---

## 3. Decision — the push/pull bridge

`read_audio` is a **pull**. CoreAudio, AAudio and WASAPI **push**, and so does
miniaudio, which normalises them all to a data callback. The bridge:

```
device callback (OS audio thread)  ──write──▶  capture ring  ──drain──▶  read_audio
read_audio                                                                (modem thread)

write_audio  ──write──▶  playback ring  ──drain──▶  device callback
(modem thread)                                       (OS audio thread)
```

Two SPSC rings, no locks on the audio thread, no allocation after open.

### `read_audio` blocks, with a timeout

Returning 0 immediately is legal (`shell/platform.h:52-53`) but then the app loop
spins at 100% CPU, and the loop has no pacing of its own. So `read_audio` waits
on a semaphore the device callback posts, until either a block is available or a
timeout expires.

The timeout is not decoration: a device that has been unplugged stops calling the
callback, and without it the modem thread hangs forever with no way to tell the
UI. On timeout, return 0 and raise a device-fault flag the app polls.

This preserves [06](06-target-architecture.md) Rule 2 exactly — capture is still
the only clock, and the count `read_audio` returns is still what advances time.

### `set_ptt(false)` must not return until the audio has left

This is the sharpest correctness hazard in the port, and it is invisible until
you hear it on the air.

`shell/loop.c` drains a whole transmission through `write_audio` and the observer
then unkeys PTT. With ALSA's blocking `snd_pcm_writei`, the samples are in the
device buffer when `write_audio` returns, and `alsa_set_ptt` calls
`snd_pcm_drain()` before dropping RTS (`shell/backend_alsa.c:139`) so the buffer
empties first. **With a ring, `write_audio` returns as soon as the bytes are
queued** — the last few hundred milliseconds of the transmission have not been
played, let alone radiated, when PTT drops. The tail of every frame gets cut.

So the ring backend's `set_ptt(false)` must wait for the playback ring to empty
*and* for one device period beyond that. Blocking there is acceptable: PTT is
routed from the observer on the modem thread (`shell/main.c:59-64`), not from the
audio callback.

The symmetric case: `set_ptt(true)` should ensure the playback ring is empty of
stale samples before keying, as `alsa_set_ptt`'s `snd_pcm_prepare` does.

---

## 4. Decision — the device enumeration API

New, in `shell/`, matching house style: caller-owned storage, no allocation, no
globals.

```c
typedef enum { ARDOP_AUDIO_CAPTURE, ARDOP_AUDIO_PLAYBACK } ardop_audio_dir;

#define ARDOP_DEV_ID_MAX   128
#define ARDOP_DEV_NAME_MAX 128

typedef struct {
	char id[ARDOP_DEV_ID_MAX];      /**< Opaque, stable, persistable. */
	char name[ARDOP_DEV_NAME_MAX];  /**< Human-readable, UTF-8. */
	bool is_default;
	bool native_12k;                /**< True if 12000 Hz needs no resampling. */
} ardop_audio_device;

/** @return Devices written to @p out (0..max). */
size_t ardop_audio_enumerate(ardop_audio_dir dir, ardop_audio_device *out,
			     size_t max);
```

### Identity across replug

The hard part, and the reason `id` and `name` are separate fields.

ALSA's `plughw:0,0` is a card *index*, and indices renumber when USB devices are
plugged in a different order — the operator's radio interface silently becomes
their webcam. Prefer the `hw:CARD=Device,DEV=0` form, which is stable per card
name. Windows endpoint IDs and macOS UIDs are stable; Android device IDs are not
guaranteed across reboots.

So: **persist both `id` and `name`, and resolve in that order** — exact `id`,
then exact `name`, then fall back to the system default *and tell the operator
the selection changed*. Silently transmitting through the wrong device is worse
than a dialogue.

### Disappearance mid-session

The backend raises the device-fault flag (§3). The app aborts any ARQ session,
unkeys PTT, surfaces the fault, and offers reselection. It does not silently
reopen the default device.

---

## 5. Decision — rate conversion, and why it is a correctness question

`shell/backend_alsa.c:53-56` asks for exactly 12000 Hz with `dir = 0`, and fails
the open if the device will not do it. That works because Linux has `plughw`.
**Android will not give you 12000 Hz**, and Windows shared-mode WASAPI gives you
the mix format. Resampling becomes mandatory.

[06](06-target-architecture.md) Rule 2 makes this delicate. The sample clock *is*
the protocol clock. If the card actually runs at 11997 Hz, protocol deadlines
stretch by 0.025% and everything stays consistent — that is the whole point, and
it is what dissolved `FixTiming`. A resampler can break that property.

**Integer decimation preserves it. Asynchronous resampling does not.**

- 48000 → 12000 is exactly 4:1. Every 4 device samples become 1 modem sample,
  forever, with no accumulator. If the device clock drifts, the modem clock
  drifts identically. Protocol time still tracks physical time.
- 44100 → 12000 is 3.675:1. A resampler with its own rational accumulator will
  insert or drop samples relative to the device clock on its own schedule. That
  decouples protocol time from device time and reintroduces exactly the class of
  bug this architecture was built to delete.

> **This second bullet is wrong, and amendment 30 says why.** It conflates a
> *rational* resampler with an *asynchronous* one. 44100 → 12000 is exactly
> 147:40, and a fixed-ratio polyphase resampler at that ratio inserts and drops
> nothing. The conclusion — refuse fractional rates for now — still stands, but
> on grounds of scope rather than correctness.

**Rule: prefer a device rate that is an integer multiple of 12000** (48000,
24000, 96000) and decimate. Accept a fractional rate only when the platform
offers nothing else, mark the session degraded in the UI, and make sure the
resampler is driven *by* the device callback so it can never invent samples the
device did not deliver.

**Anti-aliasing is not optional.** Decimating 48 kHz to 12 kHz by taking every
fourth sample folds everything from 6 kHz to 24 kHz down into the passband. That
is noise added directly to the busy detector's spectrum and to the demodulator's
S/N estimate, for free, on every platform that resamples. A low-pass at ~5.5 kHz
before decimation is required. The transmit direction needs the mirror image, or
we radiate images of our own signal.

**Where it lives:** a new `shell/resample.c` — pure, no I/O, no allocation,
shared by every backend, and unit-testable in `test/core/` like everything else
in that set. Not in `core/`: it is device adaptation, not protocol.

---

## 6. Decision — PTT becomes its own object

Today PTT is a serial fd inside the ALSA backend, toggling RTS with
`TIOCMGET`/`TIOCMSET` (`shell/backend_alsa.c:67-92`), and that is the entire
story. `backend_alsa.h:22-24` already flags CAT/GPIO/rigctl as follow-ups.

`set_ptt` stays in `ardop_platform_ops` — the loop and observer paths do not
change — but it is implemented by a separate composable object the app builds and
hands to the backend, so PTT method and audio device are configured
independently. They are independent in reality; an operator can pair any
interface with any keying method.

| Method | Platforms | Notes |
|---|---|---|
| VOX / none | all | The only option on iOS, and the fallback on Android |
| Serial RTS / DTR | Linux, Windows, macOS | What exists today; add DTR, which some interfaces use |
| **CM108 HID GPIO** | Linux, Windows, macOS | The keying method in most cheap USB sound-card interfaces (DigiRig, RIM, the common CM108/CM119 dongles). Arguably higher-value than CAT for the target operator. |
| CAT via `rigctld` | Linux, Windows, macOS | **Over TCP to a running rigctld, not by linking hamlib.** No link dependency, no LGPL entanglement, no exposure to hamlib API churn, and most operators already run it. |
| GPIO | Linux (libgpiod) | Raspberry Pi builds |
| Android USB serial | Android | Requires the USB host API and a permission prompt from the Java/Kotlin side; no `/dev/ttyUSB0` exists |

CAT-over-rigctld means PTT can now fail asynchronously (the socket drops). The
`set_ptt` signature returns `void`, so a failure has to reach the app another
way — the same device-fault flag as §3. A modem that thinks it is transmitting
and is not must not keep feeding the link.

---

## 7. Android

The platform with the most ways to be quietly wrong.

**Input processing must be off.** This is the one that will waste a week if it is
not known in advance. Android applies AGC, noise suppression and echo
cancellation to `VOICE_COMMUNICATION` and `MIC` sources by default. AGC in
particular is time-varying gain *within* a frame, which directly attacks the
16QAM amplitude decision — the decoder's `car_mag_threshold` adapts, but it
adapts to a signal whose gain the OS is moving underneath it. Noise suppression
is worse: it is tuned to remove exactly the kind of stationary tonal content an
HF modem consists of.

Request `MediaRecorder.AudioSource.UNPROCESSED`, check
`PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED` first, fall back to
`VOICE_RECOGNITION` (historically the least-processed source), and **surface the
result in the UI** — an operator on a device that will not give an unprocessed
stream needs to know why their decodes are poor.

Other requirements:

- `RECORD_AUDIO` runtime permission; the app is useless without it, so ask at
  first run with an explanation.
- **A foreground service to survive backgrounding.** Android 14+ needs
  `FOREGROUND_SERVICE_MICROPHONE` and `android:foregroundServiceType="microphone"`.
  Without it, an ARQ session ends when the user looks at another app.
- Audio focus: request it, and decide what happens when it is lost mid-session.
  Recommended: abort the session and unkey rather than transmit blind.
- `INTERNET` permission for the hosted TNC ports; USB host permission for serial
  PTT and USB audio interfaces.
- Doze and battery optimisation exemptions for long unattended sessions.

**iOS, for the deferred design only:** no serial PTT at all (VOX only), a
listening socket that is not dependable in the background, and background audio
requiring the `audio` background mode with review scrutiny. None of this changes
a decision here; it is why 14 makes the TNC server a per-platform capability.

---

## 8. Latency and the block size

`ARDOP_LOOP_BLOCK` is a fixed 1200 samples — 100 ms at 12 kHz — chosen to
"amortise call overhead while staying well inside one frame's leader"
(`shell/loop.h:28-30`). The ARQ turnaround budget is **250 ms**
([09](09-embedding-and-bindings.md), `ARQ.c:1127` in the inherited tree).

Capture-to-link latency is one block plus the device buffer. At 100 ms of block
alone, the loop spends 40% of the far end's turnaround budget before the link has
even seen the frame — and the reply then costs the same again on the way out,
plus PTT rise and leader. On ALSA with small periods this evidently works. On a
platform whose device buffer is larger, it may not, and the failure mode is not a
crash — it is a peer that retries, gear-shifts down, and blames the channel.

**Make the block size a backend-declared parameter.** Keep `ardop_loop`'s arrays
at `ARDOP_LOOP_BLOCK` and add a `size_t block` field used as the read size, so
the struct layout and the existing tests are unaffected. Default 1200; a backend
with small device periods declares less.

**Then measure it**, per platform, before assuming any of the above. The number
that matters is wall-clock from last received sample to first transmitted sample,
and it can be measured on the virtual cable with `tools/loopback.sh` without a
radio.

---

## 9. What this adds to the tree

| File | Status |
|---|---|
| `shell/audio_devices.h` | New — the enumeration API (§4) |
| `shell/backend_ma.{h,c}` | New — miniaudio backend, all four platforms |
| `shell/resample.{h,c}` | New — pure, testable, anti-aliased integer decimation (§5) |
| `shell/ptt.{h,c}` | New — the PTT object and its methods (§6) |
| `shell/backend_alsa.c` | Unchanged. Still `ardopb`'s Linux backend |
| `shell/backend_null.c` | `usleep` → portable sleep |
| `shell/loop.{h,c}` | Add the `block` field (§8) |
| `third_party/miniaudio.h` | Vendored, pinned to a specific release |

---

## Open decisions

1. **Whether `backend_alsa.c` survives long-term.** Keeping it means two Linux
   audio paths and a class of bug that reproduces on one and not the other. The
   argument for keeping it is `ardopb`'s dependency footprint; the argument
   against is that we will test the miniaudio path far more.
2. **Duplex handling.** miniaudio can open a duplex device. ARDOP is half-duplex
   and the loop enforces it (`shell/loop.c:52-55`), so two independent devices is
   the natural mapping — but a single duplex device gives one clock for both
   directions, which is closer to what the architecture wants. Worth a look.
3. **Whether the fractional resampler is built at all**, or unsupported device
   rates are simply refused with a clear message. Refusing is more honest and far
   less code; it may also make the app unusable on some Android hardware.
4. **CAT beyond PTT.** Once rigctld is connected for keying, frequency readout
   and mode setting are nearly free, and an operator will expect them. Scope
   creep, but cheap scope creep.
5. **Whether `wall_ms` gets a real implementation.** It is optional and unused by
   `ardopb`; the regulatory 10-minute ID needs it, and that is a legal
   obligation, not a nicety.

## Exit criteria

- The app captures and plays through the miniaudio backend on Linux, Windows,
  macOS and Android, with device selection from the UI on each.
- **A transmission's tail is not cut.** Verified by recording the app's transmit
  audio and comparing frame length against `golden-tx`'s reference, not by ear.
- `make golden-tx` is still bit-identical — the resampler is a device concern and
  must not have moved anything the modulator produces.
- A 48 kHz device decimated to 12 kHz decodes the golden corpus at the same rates
  as a native 12 kHz device, proving the anti-alias filter is doing its job.
- Unplugging the capture device mid-session raises a fault, unkeys PTT, and
  surfaces in the UI within one block period — it does not hang the modem thread.
- On Android, an unprocessed input source is obtained and confirmed, or the
  operator is told it could not be.

---

## Amendments made during implementation

Recorded here rather than silently worked around, because each is a place the
design above and the code as it stood disagreed.

1. **§9's table lists `backend_alsa.c` as "Unchanged". It could not be.** §6
   makes PTT its own object, and PTT lived *inside* that file as a serial fd
   (`ptt_fd`, `ptt_open`, `ptt_set`). Leaving it would mean two serial-RTS
   implementations and a bug that reproduces on one backend and not the other —
   exactly what Open decision 1 already worries about. The change is a net
   deletion of ~30 lines; `alsa_set_ptt` keeps its `snd_pcm_drain` /
   `snd_pcm_prepare`, which is the part only that file knows, and delegates the
   line toggle to `shell/ptt.c`.

2. **§1 says the interface "does not change", but §3 and §6 both need a fault to
   reach the application** — and `set_ptt` returns `void`. Resolution:
   `shell/platform.h` stays frozen; faults are latched on the concrete backend
   (`shell/fault.h`) and polled by `shell/main.c`, which already holds the
   backend pointer. Fault *reaction* — abort the session, unkey, tell the
   operator — is application policy and must not enter `loop.c`.

3. **§4's `ARDOP_DEV_ID_MAX 128` is too small.** miniaudio's `ma_device_id.alsa`
   is `char[256]`. At 128 the "persist the id and resolve it exactly" rule
   truncates and then never re-matches — precisely the failure §4 exists to
   prevent. Raised to 264 (256 plus a terminator and rounding).

4. **The interpolator's group delay is a second tail-cut, independent of the
   ring.** §3 identifies the ring: `write_audio` returns when samples are
   *queued*, so unkeying immediately cuts the transmission. But a linear-phase
   FIR also holds `(ntaps-1)/2` samples that were accepted and never emitted.
   `ardop_resample_flush()` must run *before* the ring drain, or the last
   milliseconds of every frame are lost regardless. Not mentioned in §3.

5. **Capture must be discarded during transmit.** §3 does not raise this. ARDOP
   is half-duplex and `loop.c` does not call `read_audio` while transmitting, so
   a capture ring silently banks the station's own signal for the whole over and
   feeds it to the demodulator afterwards. ALSA escapes this only by accident —
   `snd_pcm_readi` returns `-EPIPE` on overrun and `snd_pcm_prepare` throws the
   backlog away. Fixed with an atomic `tx_active` that makes the capture
   callback drop rather than enqueue, plus a ring reset at unkey.

6. **§5's stated reason for building a resampler does not distinguish the
   alternatives.** miniaudio would do integer-ratio conversion itself, driven by
   the device callback, and that would *not* break Rule 2 — §5's argument is
   against *fractional* resampling, which neither option uses. `shell/resample.c`
   is still worth writing, for control over the anti-alias filter, for
   unit-testability, and to satisfy §Exit criteria's corpus requirement. But
   that is the honest reason and §5 should say so.

7. **§8's "backend-declared" block size cannot live in `ardop_platform_ops`**
   without changing `platform.h`, which §1 forbids. The application reads it
   from the concrete backend and assigns `lp.block` — one line in `main.c`.

8. **Open decision 3 is settled as *refuse*.** A device rate that is not a whole
   multiple of 12000 fails the open with a message naming the rate, rather than
   being approximated. More honest and far less code; Windows shared-mode WASAPI
   gives 48000 in practice. *(The decision holds. The reasoning behind it was
   partly wrong — see amendment 30.)*

9. **§Exit criteria's "`make golden-tx` is still bit-identical" is true by
   construction**, not by care: `test/golden/shell_tx_wav` links `$(CORE_OBJS)`
   and `$(TEMPLATES)` only, so nothing in `shell/` can reach it. The genuinely
   new result is that it now also passes under MinGW — the same audio, byte for
   byte, from a different toolchain on a different OS.

10. **PTT scope for this pass**: VOX/none, serial RTS *and* DTR on both
    platforms, and rigctld over TCP (nearly free once `shell/net.h` existed).
    CM108 HID and libgpiod are declared in the enum and refused with a message —
    CM108 needs a HID dependency that lands on *users* plus hardware to test
    against, and libgpiod is Linux-only with no bearing on this port.

11. **Not in this document at all**: `apps/ardop_chat.c` selected on
    `STDIN_FILENO` alongside two sockets, and Winsock's `select()` takes sockets
    only. Solved with `ardop_stdin_ready()` (console / pipe / file) polled next
    to a short socket wait, rather than a reader thread.

12. **A safety item neither 14 nor 15 raises**: with rigctld keying, killing the
    process leaves the rig **transmitting**. A serial RTS line falls when the
    handle closes; a TCP CAT link does not, because the rig never learns the
    controller is gone. `ardop_install_signal_handlers()` plus unkey-before-close
    ordering in `ardop_ptt_close()` covers it.

13. **Open decision 1 is settled: `backend_alsa.c` is removed.** The document
    weighed "two Linux audio paths and a class of bug that reproduces on one and
    not the other" against `ardopb`'s dependency footprint. Two things decided
    it once both backends existed:

    - The footprint argument turned out to point the *other* way. miniaudio
      opens ALSA, PulseAudio and JACK with `dlopen`, so removing the dedicated
      backend removed the `-lasound` link and the `libasound2-dev` build
      dependency. `ardopb` went from five shared-library dependencies to four.
    - The duplication was not cosmetic. Each backend carried its own answer to
      the transmit-tail question -- `snd_pcm_drain()` in one, ring drain plus
      resampler flush plus guard interval in the other -- and only the
      miniaudio one is covered by `test/core/test_backend_ma`. Keeping a second,
      untested implementation of the hazard the design calls "the sharpest
      correctness hazard in the port" is the wrong trade.

    The one capability that was genuinely ALSA-only -- bypassing the sound
    server, which auto-selection will not do because it prefers PulseAudio --
    is preserved as `--audio-backend alsa`.

    Validated before removing, over the `tools/loopback.sh` virtual cables:
    two stations completed an ARQ connect and a bit-exact file transfer through
    the miniaudio backend at 12 kHz native (m = 1) and again at 48 kHz with
    4:1 decimation (m = 4), the latter decoding at quality 100 and S/N 23-24 dB.
    `tools/loopback.sh` lost its generated `asound.conf` and `ALSA_CONFIG_PATH`
    machinery in the process, which is a simplification in its own right.

14. **§4's resolution rule shipped private, and printing to stderr.** It was a
    `static` function inside `backend_ma.c`, so a settings screen could not
    reuse it and workstream C would have written a second copy; and an operator
    running a graphical program never sees stderr, so the one failure the rule
    exists to prevent — transmitting through the laptop speakers after a
    replug — happened silently anyway. Lifted to `ardop_audio_match_device`:
    pure, over an enumerated list, returning *how* it matched instead of
    printing. The backend keeps a thin adapter.

    A name match is deliberately **not** a substitution. It is the operator's
    device, found after a renumber, so the honest thing is to use it and write
    the new id back — which is why the backend now also reports the device it
    actually opened. Without that, the rule degrades to name-matching forever.

15. **`native_12k` (§4) was specified and never built, and should not be.** It
    was a boolean about the wrong thing: 24000, 48000 and 96000 are all equally
    usable and it would have read `false` for every one of them. It would also
    have been wrong on the two backends that matter most, since PulseAudio and
    shared-mode WASAPI report the mix format rather than the hardware's
    capability. Replaced by `native_rate` + `rate_ok`, which give three honest
    answers where one boolean had two dishonest ones: "48000 Hz", "44100 Hz,
    cannot be used", and "not known until opened".

16. **`ardop_audio_enumerate` grew a fourth parameter, `backend_name`.**
    Auto-selection prefers PulseAudio on Linux, and `--audio-backend alsa`
    (kept by amendment 13) has to enumerate the backend it will open — otherwise
    the list offers devices the open cannot select.

17. **`ARDOP_DEV_NAME_MAX` went 128 → 256.** Amendment 3 raised the *id* for
    exactly this reason and stopped one field short: the name is the second
    resolution key, and a truncated name never re-matches.

18. **`ardop_audio_print_devices` writes to stdout and reports failure.** It is
    the answer to a question the operator asked, not a diagnostic, and
    `--list-devices | grep USB` should work.

19. **§3's fault latch was one-way at the application layer.** A spine that had
    seen one fault could never transmit again — while `fault.h` says the
    application "offers reselection", which requires the reverse edge. That made
    a device-selection screen structurally impossible: unplugging a USB
    interface meant restarting the program. Faults are now a recoverable state
    with a typed code, and recovery is close+open of both objects, which works
    because [14](14-station-application.md) Decision 5 already makes the runtime
    outlive the backend.

20. **`ardop_backend_ma_clear_fault` is deleted.** It cleared a latch and did
    nothing to the device, so calling it only arranged to re-latch on the next
    read. A function whose name promises recovery it does not perform is worse
    than none; it had no callers. Same answer for the PTT latch: a keying fault
    means the path is not trustworthy, and clearing a flag does not make it so.

21. **`backend_ma.h` claimed `ardop_backend_ma_close` "unkeys on the way out"
    and it did not.** Harmless while `ardopb` closed the PTT immediately
    afterwards; not harmless once a device rebuild closes the backend while the
    same PTT object stays open, which is exactly the case the promise was for.
    Made true.

22. **§8's backend-declared block size was right for `ardopb` and wrong for the
    application.** `app_set_platform` saved and restored the *old* block, and
    since `ardop_loop_init` always leaves it non-zero the restore always fired —
    so a new backend's smaller preferred block was overwritten every time and
    every device change silently reverted to 100 ms. Amendment 7's "one line in
    `main.c`" holds only where the platform is fixed. The block is now a
    parameter of the bind, because a backend and the block it wants are one fact
    and two calls means a window in which one is wrong.

23. **CM108 (deferred by amendment 10) is implemented, with no new user-facing
    dependency**: `/dev/hidraw` on Linux, `hid.dll` on Windows, no hidapi and no
    libusb. Two details from direwolf's implementation matter more than they
    look. The report is **five bytes**, not the four the CMedia datasheet
    implies — direwolf writes the fifth with the comment *"Writing 5 bytes
    works. I have no idea why."*, every interface in the field has been tested
    against that, and with no hardware here preferring the datasheet would be a
    guess. And the chip table is load-bearing: members of this family have
    different GPIO counts (CM108 four, CM108B three, CM119 eight, SSS1621/1623
    only **two**), so the usual default of pin 3 sets a bit an SSS162x does not
    have and keys nothing, and a vendor-only match would have missed vendor
    `0x0c76` entirely. `-lsetupapi` is a new *link* library on Windows; it ships
    with the operating system.

24. **Native CAT keying is implemented, and rigctld is no longer the only CAT
    path.** For a modern transceiver the keying line *is* the CAT link, and
    hamlib gives Icom, Yaesu, Kenwood and every Xiegu `ptt_type = RIG_PTT_RIG` —
    they key by command and ignore RTS entirely. The command is one acknowledged
    frame (Icom CI-V `1C 00`, answered `FB`/`FA`), so speaking it directly needs
    no hamlib installed, no daemon to spawn and reap on two platforms, and no
    exposure to a command line we do not control.

    It is also safer than managing rigctld would be: killing rigctld does **not**
    unkey a CAT-keyed rig, because closing a serial handle sends no command, so
    owning that process would mean a crash on our side could orphan a daemon
    holding a transmitter up. `rigctld:` remains as the escape hatch.

25. **Open decision 4 ("CAT beyond PTT") stays closed, and open decision 5 (the
    10-minute ID) is still open.** `wall_ms` is implemented and plumbed into
    `ardop_platform_ops`, and nothing reads it: `core/link/` has the 3-second
    closing ID and the explicit `SENDID`, but no periodic obligation. That is
    link work rather than platform work and it touches the one part of the tree
    the golden corpus exists to freeze, so it is raised separately rather than
    smuggled in here. `ardopcf`'s behaviour should be checked first —
    interoperability matters more than anyone's reading of Part 97.

26. **§4's rigctld host parser could not take an IPv6 literal.** It split on the
    last colon, so `rigctld:::1` became host `":"` port 1. Bracketed literals
    (`rigctld:[::1]:4532`) now work.

27. **`ptt.c` claimed the rigctld path "is tested against a scripted fake server
    on localhost".** It was not; there was no PTT test of any kind. There is now
    one covering the whole grammar, both CAT frame sets byte for byte, the reply
    classification including the CI-V transceive echo, the CM108 report for
    every pin, the chip table and the auto-selection policy. The rigctld byte
    exchange is still uncovered — it needs a server answering while `open()`
    blocks, and therefore a thread `test-core` does not have — so the comment now
    says what is and is not tested rather than something untrue.

28. **The exit criterion "a 48 kHz device decimated to 12 kHz decodes the golden
    corpus at the same rates as a native 12 kHz device" was untested**, and so
    was the anti-alias filter against anything but synthetic tones. Worse,
    `tools/loopback.sh` created its cables at 12000 Hz, so `m == 1` and the
    resampler was the identity path in **every** loopback validation ever
    performed — including amendment 13's. Both are fixed:
    `make golden-resample` runs the corpus through interpolate-then-decimate at
    m = 2, 4 and 8, `golden-resample-alias` adds an interferer that naive
    decimation folds into the middle of the passband, and
    `RATE=48000 tools/loopback.sh pipe` carries a bit-exact transfer at m = 4.

29. **A note on what §7 (Android) and the macOS parts of §2 now mean.** This pass
    was scoped to Linux and Windows. Nothing here precludes the other two — the
    ops-table discipline is what keeps it so — but the device-detection reader
    (§4's successor, `shell/usbtopo.c`) is Linux-only today; Windows groups a
    physical device's functions under a shared Container ID, which is the same
    idea with none of the walking, and is a reader to write rather than a design
    to redo.

30. **§5's argument against fractional rates does not hold, and the decision to
    refuse them rests on scope instead.** Recorded properly because a wrong
    reason attached to a right decision is how a future reader talks themselves
    out of revisiting it.

    §5 says a resampler for 44100 → 12000 "will insert or drop samples relative
    to the device clock on its own schedule". That is true of an **asynchronous**
    resampler — one whose ratio is adjusted at run time to track two independent
    clocks, as a sound server does when synchronising several clients. It is not
    true of a **fixed rational** one, and 44100 → 12000 is a fixed rational ratio:

        gcd(44100, 12000) = 300  ->  44100/300 = 147,  12000/300 = 40

    A polyphase 147:40 resampler produces exactly 40 output samples for every 147
    input samples, forever, with a filter phase that cycles with period 40.
    Nothing is inserted and nothing is dropped. [06](06-target-architecture.md)
    Rule 2 survives intact: a card actually running at 44097 Hz gives a modem
    clock of 40/147 × 44097 = 11999.2 Hz, and every protocol deadline stretches
    by the same 0.007% — which is the identical property 4:1 decimation has.
    147:40 is no less deterministic than 4:1.

    So the honest cost is not correctness, it is work:

    | | |
    |---|---|
    | Filter | 24 taps per phase × L = 40 phases = 961 coefficients, ~4 kB. The existing design is already `24*m + 1`; this is the same structure with a larger L. |
    | Compute | Still 24 multiply-accumulates per *output* sample. Polyphase never computes the samples it would discard, so it is no worse than today. |
    | **The actual work** | The framing contract. `ardop_resample(DECIMATE, m)` requires `n % m == 0` and returns `n / m`, and `shell/backend_ma.c`'s `read_audio` is built on that — exactly `k*m` in, exactly `k` out. A rational ratio makes the output count vary with the input block, so both change. |

    **Postponed, not rejected.** The practical need is thin: every real sound
    card and every radio codec does 48000 natively, and the case that prompted
    this was an RDP virtual sink under WSL — a remote-desktop audio bridge, not a
    radio interface, and not a path anyone should run a modem over even with
    rational support. The genuine cases are a sound server an operator cannot
    reconfigure and a handful of 44100-only USB codecs.

    **What would justify building it**: an operator with real radio hardware that
    only offers a fractional rate, or a platform whose sound server cannot be
    moved off 44100. Until one of those turns up, the refusal message telling the
    operator how to set 48000 is the better use of the same effort — and that
    message now names the setting for PipeWire, PulseAudio and Windows rather
    than saying "pick a device or a system format".

    **What is still refused on correctness grounds, not scope**: letting the
    *sound server* do the conversion for us. It looks free, and for a fixed-ratio
    conversion it would even be right — but a server doing adaptive resampling to
    synchronise several clients is precisely the asynchronous case, and there is
    no way to tell from the outside which one is running. A resampler we do not
    control, in the protocol clock path, is the one thing worth refusing outright.

31. **The transmit credit was reported as available with no audio backend
    bound.** With no platform the loop never runs, so anything accepted sat in
    the link's 16 kB queue forever — an application would submit into a black
    hole and then wonder why nothing transmitted. `APP_TX_NO_DEVICE` says so.
