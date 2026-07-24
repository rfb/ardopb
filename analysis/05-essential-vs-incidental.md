# 05 — Essential vs incidental

The extraction. What a rebuild **must** reproduce exactly, what it must
reproduce *behaviourally*, and what it is free to discard.

The compatibility target is full: on-air interoperation with ARDOP_Win and
ardopc per the 2017 specification, plus the TCP host interface so Pat and WoAD
work unchanged.

> **The specification is normative, this code is not.** Where
> `docs/refs/ARDOP_Specification_20171127.pdf` and `ardopcf` disagree, the spec
> wins. What follows identifies where the spec's requirements are *implemented*,
> so a rebuild knows what to read and what to test — it is not a substitute for
> reading the spec.

---

## Tier 1 — Normative: must be bit-exact

Any deviation breaks interoperation with other ARDOP implementations.

### Frame type codes

`ARDOPC.h:330-367`. A single byte, with ranges carrying encoded values:

| Range | Meaning |
|---|---|
| `0x00`–`0x1F` | `DataNAK`, low 5 bits carry quality |
| `0x23` | `BREAK` |
| `0x24` | `IDLEFRAME` |
| `0x29` | `DISCFRAME` |
| `0x2C` | `END` |
| `0x2D` / `0x2E` | `ConRejBusy` / `ConRejBW` |
| `0x30` | `IDFRAME` |
| `0x31`–`0x38` | `ConReq` — bandwidth 200/500/1000/2000, Max then Forced |
| `0x39`–`0x3C` | `ConAck` — bandwidth 200/500/1000/2000 |
| `0x3D` / `0x3E` | `PINGACK` / `PING` |
| `0x40`–`0x7D` | Data frames (not all values used) |
| `0xE0`–`0xFF` | `DataACK`, low 5 bits carry quality |

Names in `strFrameType[256][18]` (`ARDOPC.c:321`); valid-type tables at
`ARDOPC.c:226`.

### Quality encoding

`q = 38 + 2 * (frameType & 0x1F)` — range 38…100 step 2, clamped at ≤38.
Appears at `SoundInput.c:1062`, `Modulate.c:165`, and in `DecodeACKNAK()`
(`SoundInput.c:3340`). Three copies of the same magic numbers; a rebuild should
have one.

### Modulation parameters per frame type

`FrameInfo()` (`ARDOPC.c:739-1080`) is the authoritative table mapping frame type
→ (carriers, modulation, baud, data length, RS length, quality threshold). It is
~340 lines of `switch`. **This is a data table written as control flow** and is
the single most mechanical extraction available in the codebase.

The 18 data modes (`strAllDataModes`, `ARDOPC.c`):

```
4FSK.200.50S    4PSK.200.100S
4PSK.200.100    8PSK.200.100    16QAM.200.100
4FSK.500.100S   4FSK.500.100
4PSK.500.100    8PSK.500.100    16QAM.500.100
4PSK.1000.100   8PSK.1000.100   16QAM.1000.100
4PSK.2000.100   8PSK.2000.100   16QAM.2000.100
4FSK.2000.600   4FSK.2000.600S
```

Naming: `<modulation>.<bandwidth Hz>.<baud>[S]`, `S` = short/robust variant.
Four modulations (4FSK, 4PSK, 8PSK, 16QAM), three symbol rates (50, 100, 600),
four bandwidths.

### Waveform templates

- `intFSK50bdCarTemplate[4][240]` — 4FSK, 50 baud
- `intFSK100bdCarTemplate[20][120]` — 4FSK, 100 baud
- `intFSK600bdCarTemplate[4][20]` — 4FSK, 600 baud (FM only)
- `intPSK100bdCarTemplate[9][4][120]` — PSK, 9 carriers × 4 phases
- `intTwoToneLeaderTemplate[120]`, `int50BaudTwoToneLeaderTemplate[240]`

In `ardopSampleArrays.c` (precomputed) and `CalcTemplates.c` (generated). Note
the PSK table stores only positive phases and negates for the rest
(`ARDOPC.h:372-373`) — a size optimisation that a rebuild may drop, provided the
emitted samples are identical.

All at 12000 Hz. 240 samples = one 50-baud symbol = 20 ms.

### Leader and sync

Two-tone leader, then a 10-symbol 4FSK frame type field (8 data + 2 parity)
XORed with the session ID. `SendLeaderAndSYNC()` (`Modulate.c:66-115`) with the
sign-alternation trick at `:104-107` that avoids phase discontinuity at symbol
boundaries — subtle, load-bearing, and easy to lose in a rewrite.

### FEC and integrity

- **Reed-Solomon**: `lib/rockliff/rrs.c`, parameters per frame type via
  `FrameInfo()`. Vendored; carry it across unchanged.
- **CRC-16**: `GenCRC16()` (`ARDOPC.c`), plus `GenCRC16FrameType()` /
  `CheckCRC16FrameType()` which fold the frame type in.
- **Frame type parity**: `ComputeTypeParity()`.
- **CRC-8**: `GenCRC8()` (`ARQ.c:200`) for callsign-derived session IDs.

### Callsign and grid encoding

`Packed6.c` — 6-byte compression used in `ConReq`, `ID` and `PING` frames.
Already clean and tested; carry it across.

### Session ID

`GenerateSessionID()` (`ARQ.c:507`) from the callsign pair. Control frames are
XORed with it.

### Protocol timing

Table in [03](03-timing-model.md). The 250 ms turnaround and the 1500–2100 ms
repeat intervals are observable by the far end and part of interoperation.

---

## Tier 2 — Behavioural: must work equivalently, need not match internally

Free to reimplement, but must be measured against the current implementation.

**The ARQ state machine** ([02](02-protocol-fsm.md)). ~60 transitions. The
*rules* are normative (they are in the spec); this particular nest of `switch`
is not.

**Bandwidth negotiation** — `IRSNegotiateBW()` (`ARQ.c:2318`). Which side wins,
and when `ConRejBW` is sent, is protocol. The implementation is not.

**Gear-shifting** — `Gearshift_9()` (`ARQ.c:717`), thresholds from
`GetShiftUpThresholds()` (`ARQ.c:684`), quality EMA with α = 0.5
(`ARQ.c:793`). Does not affect wire format, but determines whether a link
converges against a real peer. Treat the constants as behaviour to preserve and
measure, not as free parameters.

**Memory ARQ** — accumulating phase/magnitude across repeats
(`SoundInput.c:149-150`, `:295-340`). Not on the wire, but a real sensitivity
advantage; dropping it would look like a regression in weak-signal performance.

**Demodulator algorithms** — leader search (`SearchFor2ToneLeader3`), symbol
sync, frame sync, tuning-offset tracking via `dblOffsetHz`, the Goertzel
variants, `sdft.c`. Any implementation that decodes at least as well is
acceptable; the golden-vector corpus ([07](07-migration-path.md)) is how you
prove that.

**The TX bandwidth-shaping filter** — the resonator bank in `SampleSink()`
(`Modulate.c:771-914`). Its *output spectrum* matters (occupied bandwidth, spectral
mask); the sample-at-a-time IIR structure does not. This is where the biggest
clarity win is available, since the current form was chosen for a memory
constraint that no longer applies (`Modulate.c:775-776`).

**The host interface** — the command set in `docs/Host_Interface_Commands.md`
(818 lines, ~90 commands). The *protocol* is normative for Pat/WoAD; the
1300-line `strcmp` ladder in `HostInterface.c:241` implementing it is not.

---

## Tier 3 — Incidental: discard freely

| Thing | Why it's incidental |
|---|---|
| ~350 mutable globals | Consequence of the VB translation |
| `ARDOPC.h` / `ardopcommon.h` | Shared namespaces, not interfaces; 155 duplicated externs, 88 duplicated prototypes, plus dead divergent enum copies |
| Hungarian prefixes (`bln`, `dbl`, `int`, `byt`) | No longer track actual C types |
| The cooperative poll loop | An artifact of having no scheduler |
| `txSleep()` and its reentrancy | Should not exist in a non-blocking core |
| `SlowCPU` | Dead: declared, never assigned, one unreachable `if` ([03](03-timing-model.md)) |
| `FixTiming` / `-A` | Becomes unnecessary once time derives from samples |
| `SendSize` 1200, `ReceiveSize` 240, `NumberofinBuffers` | Buffer sizes with stale comments (`ardopcommon.h:14` claims 1024 is required) |
| Sample-at-a-time `SampleSink()` | Chosen for Teensy-class RAM limits |
| `#ifdef SHARECAPTURE` | Never defined in the Makefile; dead |
| `CalculateOptimumLeader()` | Entirely commented out (`ARQ.c:531-535`) |
| `intRmtLeaderMeas` / `intRmtLeaderMeasure` | Never assigned; `ComputeInterFrameInterval` reduces to `max(1000, x)` |
| `intCalcLeader` | Never assigned; always 0, meaning "use default" |
| WebGui coupling into DSP/protocol | Presentation belongs at the edge |
| `lib/rawhid` type confusion | Needs a real platform abstraction |
| Duplicated `ALSASound.c` / `Waveout.c` spine | Two forks of the main loop |

---

## Traps: looks incidental, is load-bearing

Things a well-intentioned cleanup would break.

1. **`intCalcLeader == 0` is meaningful.** `SendLeaderAndSYNC()` treats 0 as
   "use `LeaderLength`" (`Modulate.c:74-77`). "Fixing" the never-assigned global
   by giving it a real value changes the leader length on FEC frames and
   retransmissions.

2. **The 250 ms turnaround is protocol, not politeness.** `ARQ.c:1127`. The far
   end is listening. Removing it because it looks like an arbitrary sleep breaks
   links.

3. **The sign alternation in leader generation.** `Modulate.c:104-107` negates
   alternate symbols "to insure no phase discontinuity at symbol boundaries".
   Dropping it produces spectral splatter that still decodes locally.

4. **Memory ARQ makes the decoder stateful across frames.** Any test harness
   that assumes frame decoding is a pure function will get different results on
   the second identical input. `test_wav_io.py` depends on this deliberately.

5. **Quality reset to 0 on gear shift is intentional.** `ARQ.c:747`, `:783` —
   `intAvgQuality = 0` makes the next report reseed the average
   (`ComputeQualityAvg` special-cases 0). It is not a "clear the counter" bug.

6. **`ProcessNewSamples` early-returns on `FECSend`.** `SoundInput.c:822`. The
   FEC sender is deaf *by design*, not by device contention.

7. **RS "correction" can silently produce wrong data.** `CorrectRawDataWithRS()`
   (`SoundInput.c:692`) — beyond the correction limit RS may decode to a valid
   but wrong codeword. The CRC is the real guard. Preserve both.

8. **`--decodewav` forces RXO mode** (`ARDOPCommon.c:491`). All offline testing
   therefore runs in a mode where `CheckTimers`, `TCPHostPoll` and `MainPoll`
   never run (`ARDOPC.c:690`). Golden vectors capture RXO behaviour, not ARQ
   behaviour — an important limit on what that corpus proves.

---

## Carry across largely intact

Already well-factored, already tested, no reason to rewrite:

| Module | Note |
|---|---|
| `StationId.{c,h}` | Validated callsign type. The model for the rest. |
| `Locator.{c,h}` | Maidenhead grid squares. |
| `Packed6.{c,h}` | 6-byte compression; also Tier 1 normative. |
| `log.c`, `log_file.c` | Over `lib/zf_log`; tested with `--wrap` mocks. |
| `wav.c` | 79 lines, clean. |
| `sdft.c` | Sliding DFT. Exemplary comments — the standard the project asks for in `CONTRIBUTING.md`. |
| `lib/rockliff/rrs.c` | Vendored RS. Do not touch. |
| `FFT.c` | Self-contained. |
| `noise.c` | Test-support noise injection. |

---

## Size estimate

Rough guide to what a rebuild is actually taking on:

| Tier | Content | Approx. lines today |
|---|---|---:|
| 1 — normative | tables, templates, CRC/RS/Packed6, leader/sync | ~2500 (mostly data) |
| 2 — behavioural | ARQ FSM, demodulators, modulators, host protocol | ~12000 |
| 3 — incidental | globals, poll loop, platform duplication, GUI coupling | ~8000 |
| carry across | leaf modules + vendored libs | ~2500 |

The normative surface — the part that must be exactly right — is small, and much
of it is data rather than logic. That is the encouraging finding of this review:
the hard-to-get-right part of ARDOP is not large. It is currently just difficult
to *see*.
