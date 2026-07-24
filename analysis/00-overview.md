# 00 — Overview

> Analysis of `ardopcf` at commit `a7c9228` (v1.0.4.1.3), the point this hard
> fork inherits from. See [README](README.md) for the reading path and
> provenance.

## What ardopcf is

ARDOP (Amateur Radio Digital Open Protocol) carries digital data over HF radio
as audio. A station transmits audio into a radio's microphone input and receives
audio from its speaker output; the protocol turns that audio channel into a
reliable byte pipe. Its main consumer is Winlink email over HF — `ardopcf`'s
users mostly drive it from [Pat](https://getpat.io) or WoAD.

`ardopcf` is a soundcard modem plus a link-layer protocol engine plus a host
interface, in one single-threaded process:

```
 host program (Pat/WoAD)                     radio
        │  TCP 8515/8516                       ▲ ▼ audio + PTT
        ▼                                      │
   ┌────────────────────────────────────────────────┐
   │  ardopcf                                       │
   │   host interface  ·  ARQ/FEC protocol          │
   │   frame codec     ·  modulator/demodulator     │
   │   sound I/O       ·  PTT  ·  WebGui            │
   └────────────────────────────────────────────────┘
```

Two operating modes matter: **ARQ** (connected, acknowledged, retransmitting)
and **FEC** (broadcast, unacknowledged). A third, **RXO**, decodes everything
heard without participating.

### Lineage, and why it matters

    ARDOP_Win (Rick Muething, KN6KB, VB.NET, Windows only)
        └─→ ardopc (John Wiseman, G8BPQ) — hand translation to C
              └─→ ardopcf (Peter LaRue, AI7YN) — this fork

The VB heritage is legible throughout and explains most of the structure. VB
module-level variables became C file-scope globals; VB's `Timer` controls became
polled `Now > tmrX` comparisons; commented-out VB fragments survive in place
(e.g. `ARDOPC.c:1833-1836`, `FEC.c:105-107`). Hungarian-ish prefixes (`bln`,
`int`, `dbl`, `byt`, `str`, `tmr`, `dtt`) are still the naming convention, and
they no longer track the actual C types — `dbl`-prefixed variables are `float`,
`int`-prefixed ones are sometimes `short` arrays.

This is not a criticism of the translation, which preserved a working protocol
implementation and made it multi-platform. But it means the code's *shape* was
chosen by a different language's constraints, and the friction described in
[04](04-coupling-map.md) largely follows from that.

## Runtime shape

**Single process, single thread, cooperative polling.** Despite linking
`-lpthread`, there is no `pthread_create` (or `CreateThread`) anywhere in
`src/` or `lib/`. Everything is one loop, `ardopmain()`, `src/common/ARDOPC.c:643`:

```c
while (!blnClosing) {
    /* ... process --hostcommands ... */
    PollReceivedSamples();          // ALSASound.c:1622 — read audio, demodulate
    WebguiPoll();                   // Webgui.c        — websocket service
    if (ProtocolMode != RXO) {
        CheckTimers();              // ARDOPC.c:1799   — protocol deadlines
        TCPHostPoll();              // TCPHostInterface.c
        MainPoll();                 // ARDOPC.c:1979   — transmit if due
    }
    PlatformSleep(10);
}
```

Two consequences run through the whole review:

1. **There is no scheduler, so blocking is protocol.** Anything that needs to
   wait does so by calling `txSleep()`, which re-enters `TCPHostPoll()` and
   `WebguiPoll()` from inside the wait. That is the concurrency model: recursion
   through the poll functions. See [03](03-timing-model.md).
2. **The platform layer owns the program.** `platform_main()` lives in
   `src/linux/ALSASound.c:378` (2289-line file) and `src/windows/Waveout.c:333`
   (1292-line file). Each contains device enumeration, PTT, GPIO, WAV capture,
   signal handling *and* its own copy of the sample-polling and
   transmit-completion logic. `ardopmain()` is called *by* the platform, not the
   reverse.

## The four entangled concerns

The review's central claim is that four separable concerns are interleaved at
the statement level, and that this — not any individual bug — is what makes the
code hard to test and hard to port.

| Concern | Where it should live | Where it actually lives |
|---|---|---|
| **Signal processing** — modulate, demodulate, sync | pure functions over sample buffers | `SoundInput.c`, `Modulate.c` — but they also read `ProtocolState`, transmit, and draw GUI widgets |
| **Protocol** — ARQ/FEC state machines | a state machine over frame events | `ARQ.c`, `FEC.c` — but also inlined into the demodulator (`SoundInput.c:1070`) |
| **I/O** — audio devices, PTT, sockets | a thin platform shim | `ALSASound.c` / `Waveout.c`, which also own the main loop and the clock |
| **Presentation** — host protocol, WebGui | outermost layer | called from everywhere: 25 `wg_send_*` calls inside the demodulator |

The clearest single symptom: `ProcessNewSamples()` — the demodulator entry
point — performs an ARQ link turnover and **transmits** at `SoundInput.c:1070-1090`.

## Why the two reported symptoms follow

**"Issues on different hardware."** The audio device is the system's real clock,
but the protocol reads `CLOCK_MONOTONIC` and assumes the device runs at exactly
12000 Hz. Where those disagree, protocol timing drifts. `ardopcf` detects the
disagreement (the `FixTiming` check, `ALSASound.c:1050-1146`) and refuses to
start rather than misbehave on air — correct, but it turns a hardware variation
into a hard failure. Full treatment in [03](03-timing-model.md).

**"Hard to isolate and test."** There are six unit-test executables, and they
cover exactly the six modules that own no global state (`StationId`, `Locator`,
`Packed6`, `log`, plus two pure functions). Nothing touching protocol, DSP, or
I/O is unit-tested — not from neglect, but because those concerns communicate
through ~350 shared mutable globals rather than through parameters and returns.
Coverage tracks decoupling precisely. Evidence in [04](04-coupling-map.md).

## Source map

| Path | Lines | Role |
|---|---:|---|
| `src/common/SoundInput.c` | 5282 | Demodulator: leader search, sync, all demod, RS decode, Memory ARQ |
| `src/common/ARQ.c` | 2672 | ARQ link protocol FSM, gear-shifting, session management |
| `src/common/ARDOPC.c` | 2318 | Frame tables, encoders, timers, main loop, most globals |
| `src/linux/ALSASound.c` | 2289 | ALSA I/O, PTT, GPIO, clock, `platform_main`, WAV capture |
| `src/common/HostInterface.c` | 1608 | Host command dispatch (`strcmp` ladder) |
| `src/windows/Waveout.c` | 1292 | Windows counterpart to `ALSASound.c` |
| `src/common/Modulate.c` | 1052 | Modulators, TX filter, CW ID |
| `src/common/TCPHostInterface.c` | 1050 | TCP host sockets |
| `src/common/Webgui.c` | 877 | WebGui state push |
| `src/common/CalcTemplates.c` | 789 | Runtime waveform template generation |
| `src/common/ardopSampleArrays.c` | 661 | Precomputed waveform templates |
| `src/common/txframe.c` | 612 | `TXFRAME` test-frame generation |
| `src/common/FEC.c` | 393 | FEC mode |
| `src/common/RXO.c` | 332 | Receive-only decoding |
| `src/common/BusyDetect.c` | 233 | Channel-busy detection |
| **Well-factored** | | |
| `src/common/StationId.{c,h}` | 428+345 | Callsign type — validated, tested, no globals |
| `src/common/Locator.{c,h}` | 236+237 | Maidenhead grid square — ditto |
| `src/common/Packed6.{c,h}` | 135+122 | 6-byte callsign/grid compression — ditto |
| `src/common/log.c`, `log_file.c` | 269+168 | Logging over `lib/zf_log` |
| `src/common/sdft.c` | 455 | Sliding DFT decoder — exemplary comments |
| `src/common/wav.c` | 79 | WAV read/write |
| `lib/rockliff/rrs.c` | 642 | Reed-Solomon (vendored) |

The bottom group is the model for everything else: real interfaces, no globals,
and — not coincidentally — the only unit tests in the project. They should be
carried into any rebuild largely intact ([05](05-essential-vs-incidental.md)).

## Reading path

- [01 — Signal chain](01-signal-chain.md): TX and RX end to end.
- [02 — Protocol FSM](02-protocol-fsm.md): the ARQ state machine, recovered.
- [03 — Timing model](03-timing-model.md): the hardware-dependence problem.
- [04 — Coupling map](04-coupling-map.md): the testability problem, quantified.
- [05 — Essential vs incidental](05-essential-vs-incidental.md): what a rebuild must preserve.
- [06 — Target architecture](06-target-architecture.md): the proposed shape.
- [07 — Migration path](07-migration-path.md): how to get there safely.
