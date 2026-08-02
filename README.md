# ardopb: AI driven re-implementation of ARDOP

`------------------ START HUMAN MESSAGE ------------------`

The radio community deserves a high quality, open-source digital modem. 

In 2022 I set out to improve ardopc. I had been participating in Winlink Wednesdays, and I want to improve reliability with my local relay and perhaps introduce some automation and soon I discovered the timing issues which caused connections to fail on different hardware. 

For a moment I thought I could be the person to fix these bugs, but as peeled back the layers I discovered a complexity far beyond my soft human mind. I couldn't articulate it at the time, but I could tell mess of domains and state when I saw one. 

With AI I've started moving back through my mental checklist of projects which just seemed far to ambitious at the time. Last week I reached this project - I wanted to see how well Claude could articulate the problems and propose a solution. Claude did an excellent job. I encourage anyone looking at this project to review the ([`analysis/`](analysis/)) Claude produced.

It turns out this is an ideal project for an AI agent - it's well bounded and tested. Data -> Audio -> Data. This took a couple of days only because I kept hitting my Pro's session limit. I sure it would have been less than 6 hours if I had been running the Pro Max. 

This was built using Claude Opus 4.8

I've learn a lot about DSP in the process. Hopefully this freshly refactored code can help others understand the principles and even evolve the ARDOP protocol.

-Ryan

`------------------ END OF HUMAN MESSAGE ------------------`

A rebuilt implementation of **ARDOP** — the Amateur Radio Digital Open Protocol,
which carries digital data as audio over an HF radio channel. `ardopb` is a hard
fork of [`ardopcf`](https://github.com/pflarue/ardop), re-architected around a
pure, testable core with the device I/O pushed to the edges.

It is **wire-compatible** with the existing ARDOP ecosystem: it interoperates
over the air with `ARDOP_Win`/`ardopc`/`ardopcf` per the 2017 specification, and
it speaks the same TCP host protocol, so host programs (Pat, WoAD, …) work
unchanged.

```
make                 # builds ardopb + the host-client apps
./ardopb N0CALL --audio --host 8515
```

## Why this fork exists

`ardopcf` is a working, actively maintained modem, but it descends from a
VB→C translation (`ARDOP_Win` → G8BPQ's `ardopc` → `ardopcf`) and the lineage
shows: the program is organised as one flat namespace of ~350 mutable globals
rather than as modules with interfaces. Two concrete consequences motivated the
rebuild:

- **Hardware-dependent behaviour.** Protocol timing was derived from a wall
  clock while the audio device is the real clock, so sound-card quirks could
  make the program refuse to start or degrade on air (the `FixTiming`/`-A`,
  `SlowCPU` machinery).
- **Untestability.** The only unit-tested code was the handful of leaf modules
  that owned no global state. Nothing touching protocol, DSP, or I/O was
  separable enough to test.

The rebuild treats those as architectural problems and fixes them by
construction. It began as a written analysis of the inherited code
([`analysis/`](analysis/)) — separating the *normative* protocol (what must be
preserved bit-for-bit to stay interoperable) from *implementation accident* —
and proceeded module by module, each new piece proven equivalent to the original
before the original was retired.

## Architecture

A **sans-I/O** design: the modem and protocol are a pure library that opens no
device, reads no clock of its own, binds no socket, and never blocks. All state
lives in caller-owned structs. Dependencies point one way.

```
  apps/     ardop-tx  ardop-rx  ardop-chat        TCP host-protocol clients
    |  (host protocol over TCP)                   app/   the station app's spine
  shell/    runtime . driver loop . host iface      |    -- embeds the modem
    |        . miniaudio / null backends           /        instead of dialling it
  core/     codec -> modem -> link                  pure: no I/O, no clock,
             (frames/RS/CRC) (mod/demod) (ARQ/FEC)   no allocation, no globals
```

- **One clock: the sample counter.** Time enters the core as an elapsed audio
  sample count. This removes the wall-clock/sample-count conflation — the entire
  `FixTiming`/drift class of bugs is gone rather than detected.
- **No blocking transmit.** TX is "produce N samples on demand", so transmission
  completion is *observed* (the modulator drains) rather than *predicted* (a
  nominal duration), which also removes the TX-inside-TX reentrancy the old tree
  had.
- **Mechanically enforced.** `make check-pure` proves the core has no mutable
  globals and no allocation; `make check-standalone` proves it links with only
  `libm`; everything builds at `-Wall -Wextra -Werror -Wconversion` and more.

See [`analysis/06-target-architecture.md`](analysis/06-target-architecture.md)
and [`core/README.md`](core/README.md) for the full rationale and the rules the
core is held to.

## The tree

| Path | What |
|---|---|
| [`core/`](core/) | The pure modem + protocol library: `codec` (frame table, Reed–Solomon, CRC, callsign/grid coding), `modem` (modulator, demodulator, sync, busy detector, FFT), `link` (the ARQ/FEC state machine). No I/O, no globals. |
| [`shell/`](shell/) | The impure program around the core: the sans-I/O `runtime`, the single-clocked driver `loop`, the TCP host interface, the portable socket/system layer (`net`, `sys`), settings, and the platform layer — miniaudio, keying, device enumeration. See [`shell/README.md`](shell/README.md). |
| [`apps/`](apps/) | Host-client CLI tools — see [`apps/README.md`](apps/README.md). |
| [`app/`](app/) | The station application's embedding spine: the modem embedded rather than talked to, with backpressure and a TNC-ownership rule. Phase 1 of [`analysis/14`](analysis/14-station-application.md); see [`app/README.md`](app/README.md). |
| [`test/`](test/) | In-process tests (`test/core`) and the frozen golden-vector corpus (`test/golden`). |
| [`tools/`](tools/) | `loopback.sh` — a virtual-audio-cable harness for running two stations against each other with no radio; `package-windows.sh` and `package-linux.sh` — assemble the release downloads. |
| [`third_party/`](third_party/) | Vendored dependencies, pinned by version and checksum. Currently miniaudio. |
| [`analysis/`](analysis/) | The written architecture review and rebuild design — *how we got here*. |
| [`docs/refs/`](docs/refs/) | The ARDOP specification and host-interface reference PDFs. |

## Building and running

Requirements (Debian/Ubuntu):

```
sudo apt install build-essential                     # ardopb + apps
sudo apt install libcmocka-dev                        # to run the tests
```

`make` builds `ardopb` and the apps. The modem is a long-running process that
owns the sound card and exposes the TCP host interface:

```
ardopb MYCALL [--listen] [--host PORT] [--telemetry [PORT]]
       [--null [SECONDS] | --audio [CAPTURE PLAYBACK]]
       [--audio-backend NAME] [--ptt SPEC] [--list-devices]
```

### Keying

| Spec | Method |
|---|---|
| `none`, `vox` | Nothing. Always available. |
| `rts:DEV`, `dtr:DEV` | Assert a serial control line. |
| `civ:DEV[@ADDR]` | Icom CI-V, **and every Xiegu**, which emulates it. |
| `kenwood:DEV`, `yaesu:DEV` | The radio's own CAT command. |
| `cm108[:PATH\|auto][+N]` | C-Media GPIO over raw HID (DigiRig Lite and similar). |
| `rigctld:HOST:PORT` | To a running rigctld. |

The right method is a property of the radio, and picking the wrong one fails
silently: a Xiegu or an Icom keys by CAT command and ignores RTS entirely, while
a DigiRig Mobile keys by RTS and a DigiRig Lite by CM108 GPIO. All three look
identically connected and only one of them transmits.

`app/ardop-spine --detect` works it out for you where it can, by pairing a sound
card with the keying interface on the same USB hardware. It prints what it found
and applies nothing — confirm it keys before trusting it.

**The keying paths have never been run against real hardware.** If you own a
radio and can help, [`analysis/19`](analysis/19-field-testing.md) says exactly
what to try and what to send back.

CM108 on Linux needs a udev rule; the diagnostic prints it, and it is also in
[`shell/README.md`](shell/README.md).

### Settings

`$XDG_CONFIG_HOME/ardop/station.conf` (or `$HOME/.config/ardop/...`), and
`%APPDATA%\ardop\station.conf` on Windows. Plain `key=value`, safe to edit by
hand, and unknown keys are preserved.

`--audio` is the sound card, on every platform: one backend (miniaudio) over
WASAPI, CoreAudio, AAudio, ALSA, PulseAudio and JACK. `--list-devices` prints
what is available with the id to select it by.

There is deliberately no second Linux audio path. A dedicated ALSA backend
existed and was removed: two implementations meant two versions of the
transmit-tail drain and a class of bug that reproduces on one and not the
other, and only one of them was covered by the tests. It also cost more than
it saved — miniaudio opens ALSA, PulseAudio and JACK with `dlopen`, so
`ardopb` no longer links `libasound` at all and no longer needs
`libasound2-dev` to build. Where the choice matters, `--audio-backend alsa`
pins it and bypasses the sound server.

`--ptt` takes `none` (VOX), `rts:DEVICE`, `dtr:DEVICE`, or
`rigctld:HOST:PORT` — the last keying through a running `rigctld` over TCP
rather than by linking hamlib.

**Sound-card rates must be a whole multiple of 12000 Hz** (12000, 24000, 48000,
96000). A rate like 44100 is refused with a message rather than resampled: the
sample clock *is* the protocol clock here, so an approximate conversion would
break the link's timing rather than merely its audio. See
[`shell/resample.h`](shell/resample.h).

Under WSL (WSLg) the Windows audio devices appear through WSLg's PulseAudio
server and need no extra configuration — `--audio` finds them, and
`--list-devices` names them.

### Prebuilt binaries

Fresh builds are published automatically from `main` — no GitHub account
needed:

| download | what it is |
| --- | --- |
| **[ardopb-windows-x86_64.zip](../../releases/download/continuous/ardopb-windows-x86_64.zip)** | the modem and the host-client apps, for Windows |
| **[ardopb-linux-x86_64.tar.gz](../../releases/download/continuous/ardopb-linux-x86_64.tar.gz)** | the same, for a Linux PC |
| **[ardopb-linux-aarch64.tar.gz](../../releases/download/continuous/ardopb-linux-aarch64.tar.gz)** | the same, for a 64-bit Raspberry Pi |
| **[ardop-gui-windows-x86_64.zip](../../releases/download/continuous/ardop-gui-windows-x86_64.zip)** | the instrument panel, and its Qt libraries |

The modem download is under 300 KB; the panel is a separate one because Qt and
ICU are two orders of magnitude larger than the modem, and you do not need the
panel to run a link. On Linux, build the panel from source (see
[`gui/`](gui/)) — distro Qt is one `apt` line.

The Linux tarballs need glibc 2.34 or newer — Debian 12, Ubuntu 22.04 and
later, Raspberry Pi OS bookworm. Nothing in them links an audio library:
ALSA, PulseAudio and JACK are opened at run time if present.

### Windows

`ardopb.exe` and the apps are statically linked single files; `ardop-gui.exe`
ships with its Qt DLLs beside it. Start with `ardopb.exe --list-devices`, then

```
ardopb.exe MYCALL --audio --ptt rts:COM3 --host 8515 --telemetry
ardop-gui.exe --host 127.0.0.1:8517
```

COM ports above COM9 work as written — the `\\.\` prefix is applied for you.

To build it yourself, use an [MSYS2](https://www.msys2.org/) **MINGW64** shell
(the package list is in [`.github/workflows/test.yml`](.github/workflows/test.yml))
and run `make`. Binaries get a `.exe` suffix. `check-pure`, `check-headers` and
`check-standalone` read ELF with `objdump`/`nm` and are Linux-hosted; they
refuse to run there rather than passing vacuously, because they prove properties
of source both platforms share and are run once on the Linux CI job.

The **apps** are thin clients that connect to a running modem's host port:

```
cat file | ardop-tx --host 127.0.0.1:8515 N0DEST     # reliable pipe over ARQ
ardop-rx --host 127.0.0.1:8515 > file                 # the receiving end
ardop-chat --host 127.0.0.1:8515 --call N0DEST        # basic two-way chat
```

## Testing without a radio

`tools/loopback.sh` creates a pair of virtual audio cables (PulseAudio/PipeWire
null sinks) so two `ardopb` instances can talk over real audio on one machine:

```
tools/loopback.sh check     # confirm the virtual cable carries audio
tools/loopback.sh demo      # run a full ARQ connect + data exchange
tools/loopback.sh pipe      # transfer a file with ardop-tx/ardop-rx, verify it
```

## Verification and guarantees

Every core module was proven equivalent to the inherited implementation before
that implementation was removed. The reference code is gone now, so the ongoing
regression net is:

- **`make test-core`** — in-process protocol/integration tests (two stations
  through the real modem, the runtime, the host command surface).
- **`make golden-core` / `golden-shell` / `golden-tx`** — the frozen golden
  corpus: real recorded ARDOP audio is decoded through the core and the assembled
  shell and checked against a manifest, and the modulator's transmit audio is
  verified **bit-for-bit** (SHA-256) against the corpus for every data mode.
- **`make check-pure` / `check-headers` / `check-standalone`** — the mechanical
  guarantees on the core.
- **`make test-ring-tsan`** — the audio ring's lock-free ordering, under
  ThreadSanitizer. Linux-hosted for the same reason `check-pure` is: the ring is
  portable C11 with no platform code, so proving the ordering once proves it for
  the Windows build too.

CI runs the whole sweep on Linux and, apart from the ELF- and sanitizer-based
checks, on Windows as well. `golden-tx` passing on both is the strongest single
statement in the pipeline: the modulator emits byte-identical on-air audio
compiled by a different toolchain on a different operating system.

See [`test/golden/README.md`](test/golden/README.md) for what the corpus asserts
and how strongly.

## Status

Interoperable ARQ and FEC sessions, the full host command + data interface, and
the modulator/demodulator for every 2017-spec data mode are working and covered.
Linux and Windows are both built and tested on every push, with Windows binaries
published automatically.

Known gaps, both of which matter to an operator:

- **Memory-ARQ combining is not yet ported**, so low-SNR performance is below
  the reference implementation's.
- **On-air interop against a real `ardopcf` peer has not been validated.** The
  golden corpus proves the waveform and the decode; it does not prove a live
  negotiated session with a foreign implementation.

That is why the Windows builds are published as a *prerelease*. macOS and
Android are reachable through the same miniaudio backend but are not yet built
or tested in CI.

## Provenance and license

Forked from [`pflarue/ardop`](https://github.com/pflarue/ardop) (`ardopcf`),
itself descended from John Wiseman's `ardopc` and Rick Muething's `ARDOP_Win`.
MIT licensed — see [`LICENSE`](LICENSE); copyright © 2014–2024 Rick Muething,
John Wiseman, Peter LaRue, and contributors to this fork.

> **On AI assistance.** This rebuild was carried out with substantial AI
> assistance. That is a deliberate divergence from upstream `ardopcf`, whose
> maintainer asks that AI-assisted contributions receive line-by-line human
> review before submission and does not use such tools directly. This fork is an
> independent line of development and is **not** intended as a stream of pull
> requests back to `pflarue/ardop`; the normative protocol behaviour is
> preserved (and checked against the golden corpus) precisely so that the two can
> still talk to each other on the air.
