# 02 — The protocol state machine, recovered

The ARQ link protocol is implemented as one ~1200-line nested `switch` inside
`ProcessRcvdARQFrame()` (`ARQ.c:1113-2316`), with pieces also living in
`GetNextARQFrame()` (`ARQ.c:347`), `CheckTimers()` (`ARDOPC.c:1799`), and — as
noted in [01](01-signal-chain.md) — `ProcessNewSamples()` (`SoundInput.c:1070`).

This document reassembles it into the table it wants to be. That table is the
main thing a rebuild needs to carry across, and it is the artifact this project
most conspicuously lacks.

The `// (Handles protocol rule N.N)` comments scattered through `ARQ.c` refer to
numbered rules in `docs/refs/ARDOP_Specification_20171127.pdf`. **The spec, not
this code, is normative** — where they disagree the spec wins, and any rebuild
should be validated against the spec rather than against these comments.

---

## The three state variables

ARDOP's link state is spread across three globals with no invariant tying them
together:

| Variable | Type | Meaning |
|---|---|---|
| `ProtocolMode` | `enum _ProtocolMode` | `ARQ`, `FEC`, `RXO`, `Undef` — which protocol is running |
| `ProtocolState` | `enum _ARDOPState` | `OFFLINE, DISC, ISS, IRS, IDLE, IRStoISS, FECSend, FECRcv` |
| `ARQState` | `enum _ARQSubStates` | `None, ISSConReq, ISSConAck, ISSData, ISSId, IRSConAck, IRSData, IRSBreak, IRSfromISS, DISCArqEnd` |

Mode and state overlap: `FECSend`/`FECRcv` are `ProtocolState` values, but FEC is
also a `ProtocolMode`. `RXO` exists as a `ProtocolMode` only.

`ardopcommon.h` carries a second, *divergent* copy of all three enums
(`:257`, `:276`, `:295`, `:313`) — its `_ReceiveState` has an extra `GettingTone`
member and its `_ProtocolMode` is missing `RXO`. That copy is **dead code**:
`ardopcommon.h:1` includes `ARDOPC.h`, which sets `ARDOPCHEADERDEFINED`, so the
`#ifndef ARDOPCHEADERDEFINED` blocks guarding the copies are never compiled.
Verified by preprocessing — `GettingTone` does not appear in the output of
`gcc -E` on any translation unit.

So it is not currently a correctness hazard. It is worse in a different way: a
stale fork of the core type definitions, sitting in the tree looking
authoritative, that would silently change the meaning of every state comparison
if anyone ever reordered the includes. See [04](04-coupling-map.md).

Vocabulary: **ISS** = Information Sending Station, **IRS** = Information
Receiving Station. A connected session always has exactly one of each; `IDLE` is
an ISS with nothing to send, and `IRStoISS` is a transition state.

---

## State/event table

Transitions from `ProcessRcvdARQFrame()`. Line numbers are absolute in `ARQ.c`.

### `DISC` — not connected

| Event | Action | New state |
|---|---|---|
| `DISCFRAME` decoded OK | Send `END` with the *previous* session ID; set `tmrFinalID = Now + 3000` | `DISC` (`:1138`) |
| `PING` decoded OK | `ProcessPingFrame()`; reply `PINGACK` if `EnablePingAck` | `DISC` (`:1167`) |
| `ConReq*` addressed to us, bandwidth compatible | `InitializeConnection()`, `blnPending = TRUE`, `tmrIRSPendingTimeout = Now + 10000`, latch callsigns, reply `ConAck` with timing | **`IRS` / `IRSConAck`** (`:1247`) |
| `ConReq*` addressed to us, bandwidth incompatible | Reply `ConRejBW`, notify host `REJECTEDBW` | `DISC` (`:1271`) |
| `ConReq*` not for us | `CANCELPENDING` to host | `DISC` (`:1290`) |

### `IRS` — receiving

Substate `IRSConAck` (`:1304`) — awaiting first data, still tolerating a repeated
`ConReq` in case our `ConAck` was lost:

| Event | Action | New state |
|---|---|---|
| `ConReq*` again | Re-send `ConAck` (ISS missed ours) | `IRS` / `IRSConAck` (`:1309`) |
| First data frame | Accept, pass to host | `IRS` / **`IRSData`** (`:1414`) |
| decode failed | *no reply at all* (`:1306`) | unchanged |

Substate `IRSData` — the steady receive state:

| Event | Action | New state |
|---|---|---|
| Data frame, CRC OK, **new** frame | Pass data to host; reply `DataACK` carrying a quality value | `IRS` / `IRSData` |
| Data frame, CRC OK, **repeat** of last | Re-`ACK`, do **not** re-deliver to host (dedup via `intLastARQDataFrameToHost`) | unchanged |
| Data frame, CRC bad | Reply `DataNAK` with quality; retain for Memory ARQ | unchanged |
| `IDLEFRAME` | Reply `DataACK`; if host has data queued and `AutoBreak`, send `BREAK` instead | `IRS` → **`IRStoISS`** (`:1601`, `:1653`) |
| `DISCFRAME` | Reply `END`, notify host `DISCONNECTED` | **`DISC`** (`:1519`) |
| `END` | Notify host, send ID | **`DISC`** (`:1546`) |

### `IRStoISS` — requested the link, awaiting confirmation

**This state does nothing here.** `ARQ.c:1752-1761`:

```c
case IRStoISS:  // In this state answer any data frame with a BREAK. If ACK received go to Protocol State ISS
    ZF_LOGD("In ProtocolState == IRStoISS, Nothing is done in"
            " ProcessRcvdARQFrame() ...");
    // TODO: Review why this is done in ProcessNewSamples() rather than
    // here, and verify that it is done reliably/correctly.
    return;
```

The actual transition lives in the demodulator, `SoundInput.c:1070`. The
maintainer's own `TODO` names the problem. Worth stressing that this is not a
finding the review is contributing — it is already known and already flagged in
the source; what the review adds is that it is *structural* rather than a local
oversight, and that the layering in [06](06-target-architecture.md) is what
makes it fixable.

### `IDLE` — sending station with nothing to send

| Event | Action | New state |
|---|---|---|
| `DataACK` **and** host data queued | — | **`ISS` / `ISSData`** (`:1773`) |
| `DataACK`, no data, >9 min since last ID | Send ID frame, then repeating `IDLEFRAME` | `IDLE` (`:1787`) |
| `DataACK`, no data | Ignore; keep repeating `IDLEFRAME` | `IDLE` |
| `BREAK` | Reply `DataACK`, hand over the link | **`IRS` / `IRSfromISS`** (`:1814`) |
| `DISCFRAME` | Reply `END` | **`DISC`** |

### `ISS` — sending

| Event | Action | New state |
|---|---|---|
| `ConAck` (substate `ISSConReq`) | Connection established; `CalculateOptimumLeader()` from reported timing | `ISS` / **`ISSConAck`** (`:1958`) |
| `DataACK` | `ComputeQualityAvg()`, `intACKctr++`, `Gearshift_9()`, send next data | `ISS` / `ISSData` (`:2033`) |
| `DataNAK` | `ComputeQualityAvg()`, `intNAKctr++`, `Gearshift_9()`, repeat frame | unchanged |
| `BREAK` | Reply `DataACK`, yield the link | **`IRS` / `IRSfromISS`** (`:2055`, `:2193`) |
| no data left | send repeating `IDLEFRAME` | **`IDLE`** |
| `DISCFRAME` / host `DISCONNECT` | Send `DISC` repeatedly (5 tries, `ARQ.c:373`), then `END` | **`DISC`** (`:2079`) |

---

## Cross-cutting mechanisms

### Session ID

`GenerateSessionID()` (`ARQ.c:507`) derives a byte from the calling and target
callsigns. Every control frame is XORed with it, so stations can tell their own
session's frames from a co-channel session's. `bytPendingSessionID` holds the
value during connection setup; `bytLastARQSessionID` is retained after
disconnect so a late `DISC` can be answered (`ARQ.c:1142`).

RXO mode has no session ID — it recovers frame types *without* it, which is why
`RXO.c` needs its own `RxoComputeDecodeDistance()` (`RXO.c:26`) that ignores
tones 5-8. A neat, well-commented piece of work and a good example of the
codebase at its best.

### Bandwidth negotiation

`ConReq` frame types encode both bandwidth and whether it is a maximum or forced:
`ConReq200M`…`ConReq2000M` (max) and `ConReq200F`…`ConReq2000F` (forced),
`ARDOPC.h:343-352`. `IRSNegotiateBW()` (`ARQ.c:2318`) picks the session
bandwidth or rejects with `ConRejBW`. Rules encoded in `enum _ARQBandwidth`
(`ARDOPC.h:254`).

### Gear-shifting

`Gearshift_9()` (`ARQ.c:717`) walks an ordered list of data frame types for the
negotiated bandwidth (`GetDataModes()`, `ARQ.c:610`), trading throughput against
robustness:

- **Down** after `DownNAKS` consecutive NAKs — normally 2, but **1** if the
  current mode has never once succeeded (`ModeHasWorked[]`, `ARQ.c:729-730`).
- **Up** when `intAvgQuality` exceeds a per-mode threshold
  (`GetShiftUpThresholds()`, `ARQ.c:684`) **and** ≥2 ACKs since the last shift.
- Suppressed if the remaining data fits in the current mode (`:758`).
- A mode that was tried and failed immediately is not retried until 5 consecutive
  ACKs (`:767`).

Quality is an exponential moving average with α = 0.5 (`ComputeQualityAvg()`,
`ARQ.c:793`), reset to 0 on every shift so the next report reseeds it.

These constants are **normative-ish**: they do not affect wire format, but they
do affect whether a link converges against a real ARDOP peer. Treat them as
behaviour to preserve and measure, not as free parameters ([05](05-essential-vs-incidental.md)).

### Memory ARQ

Failed frames are not discarded. `intCarPhaseAvg[8][520]` and
`intCarMagAvg[8][520]` (`SoundInput.c:149-150`) accumulate phase and magnitude
across repeats so that a frame no single copy could decode may decode from the
average. `CheckMemarqTime()` (`SoundInput.c:306`) ages the accumulator out;
`ResetMemoryARQ()` (`:295`) clears it on mode change or new session.

This is why `test_wav_io.py` can decode repeated noisy copies of a frame that a
single copy fails on — and why the decoder is **stateful across frames**, which
any test harness has to account for.

### ACK/NAK quality encoding

Quality rides in the low 5 bits of the frame type itself: `DataACKmin` = `0xE0`
through `0xFF`, `DataNAKmin` = `0x00` through `0x1F`. Decoded as
`q = 38 + 2 * (frameType & 0x1F)` (`SoundInput.c:1062`, `Modulate.c:165`) — so
the representable range is 38…100 in steps of 2, and anything ≤38 is clamped.
The formula appears in at least three places with the same magic numbers.

---

## The other two modes

**FEC** (`FEC.c`, 393 lines): broadcast, no ACKs, each frame optionally repeated
`FECRepeats` times. `ProtocolState` becomes `FECSend`/`FECRcv`. Note
`ProcessNewSamples` returns immediately when `ProtocolState == FECSend`
(`SoundInput.c:822`) — the FEC sender is deaf by construction, not by device
contention.

**RXO** (`RXO.c`, 332 lines): decodes everything, participates in nothing.
`ardopmain()` skips `CheckTimers()`, `TCPHostPoll()` and `MainPoll()` entirely in
this mode (`ARDOPC.c:690`), which is why `CheckMemarqTime()` had to be moved into
`ProcessNewSamples` — the comment at `SoundInput.c:814-819` explains exactly this.
That comment is a small monument to the coupling problem: a DSP function acquired
a timer-servicing responsibility because the mode dispatch lives in the main loop.

RXO is also the mode `--decodewav` forces (`ARDOPCommon.c:491`), and therefore the
mode all offline testing runs in — worth knowing when reading test results.

---

## What this means for a rebuild

The table above is ~60 transitions over 5 states and ~10 frame classes. As a data
structure that is small; as 1200 lines of nested `switch` interleaved with
modulation calls, host notifications and GUI updates, it is not reviewable.

Three properties a rebuilt FSM should have, all absent today:

1. **Transitions return actions, they don't perform them.** Today every arm calls
   `Mod4FSKDataAndPlay(...)` directly and blocks until the audio has played. If a
   transition instead returned "send frame X", the whole machine becomes a pure
   function testable without a sound card.
2. **One state variable, not three.** `ProtocolMode`/`ProtocolState`/`ARQState`
   admit combinations that are meaningless; nothing enforces the valid set.
3. **No transitions outside the machine.** The `IRStoISS` case is currently a
   comment pointing at `SoundInput.c`.
