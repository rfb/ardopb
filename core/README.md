# `core/` — the pure core

The rebuild, layer by layer, following [`analysis/06`](../analysis/06-target-architecture.md).

Nothing in here opens a device, reads a clock, binds a socket, draws anything,
or blocks. It consumes data and produces data. The impure shell stays in `src/`
until Stage 4 replaces it.

The old tree keeps working throughout. `core/` is built and tested alongside it,
not in place of it, until a layer is complete and proven against the
[golden vectors](../test/golden/README.md).

---

## The rules, and how each one is enforced

A convention that relies on someone remembering it is a convention that decays.
Every rule below is checked by something that runs on its own — the compiler,
`make check-pure`, or CI. **If a rule here cannot be mechanically enforced, it is
written as guidance and labelled as such, so nobody mistakes it for a guarantee.**

### 1. No mutable global state — *enforced by `make check-pure`*

The single defect that shaped everything else in the old codebase: ~350 mutable
globals, which is why nothing below the leaf modules can be unit tested.

The check is a link-time property, not a code-review habit. It asks which ELF
*section* each defined symbol lands in:

```sh
objdump -t core/**/*.o | grep -E '[[:space:]](\.data|\.bss|\*COM\*)[[:space:]]'
#   must find nothing
```

`.data` and `.bss` are writable for the whole run, so a symbol there is mutable
global state and fails. `.rodata`, `.data.rel.ro` (mapped read-only after
relocation) and `.text` are fine, so a `static const` table passes and a
`static int counter` fails, whether or not anyone reviewed it.

The obvious check — `nm ... | grep ' [BbDd] '` — is *wrong*, and finding out why
is the reason this keys on the section instead. `nm` labels a `static const`
table that contains pointers as `d`, which reads as mutable data. In a
position-independent build such a table is emitted to `.data.rel.ro`, which the
loader maps read-only once its relocations are applied; it is not writable. The
nm-letter check rejects every const table with a string or pointer in it —
including this module's frame table. The section is the truth; the letter is not.

Run against the inherited tree, `.data`/`.bss` symbols number 117 for `ARDOPC.o`
and 0 for `Packed6.o` — it measures exactly the thing that made one testable and
the other not. State lives in caller-owned structs passed explicitly.

### 2. Every warning is an error — *enforced by `-Werror`*

`core/` builds with `-Wall -Wextra -Werror` from its first file, which is the
only moment it is free. The old tree carries 177 warnings and is exempt until
its files are replaced.

### 3. No hidden allocation — *guidance, partially enforced*

The core does not call `malloc`. Callers own storage and pass it in; sizes are
passed with pointers, never inferred. This makes the core usable from an
embedded shell and from another language without a shared allocator, and it
removes leak-shaped bugs by construction.

Partially enforced: `make check-pure` also greps the link map for `malloc`,
`calloc`, `realloc`, `strdup` and `free`. That catches direct calls, not
transitive ones.

### 4. Fixed-width types — *enforced by the compiler*

`<stdint.h>` and `<stdbool.h>` only. No `BOOL`, `UCHAR`, `VOID`, `HANDLE`. The
old typedefs are what produced the GCC 14 build failure in `lib/rawhid`.

### 5. `static` by default — *enforced by `-Wmissing-prototypes`*

Anything not in a header is `static`. `-Wmissing-prototypes` fails the build on
a non-static function with no prototype, so the default is enforced rather than
remembered.

### 6. `const`-correct — *enforced by `-Wdiscarded-qualifiers`*

Inputs are `const`. Lookup tables are `static const` (which is also what makes
rule 1 pass).

### 7. One header per module, self-contained — *enforced by CI*

Each header compiles standalone. CI compiles every header on its own; a header
that forgets an include fails there rather than mysteriously later.

### 8. Return values that must be checked — *enforced by `ARDOP_MUSTUSE`*

Functions that can fail are marked `ARDOP_MUSTUSE` (`warn_unused_result`), so
discarding a status is a build failure. Reused from `src/common/mustuse.h`,
which the old tree already got right.

---

## Style

Settled in [`analysis/08`](../analysis/08-style-and-tooling.md), which records
why each choice was made rather than inherited. Briefly: tabs; K&R braces,
always braced; `snake_case`; `ardop_` prefix on anything public; no Hungarian
notation; `ARDOP_<MODULE>_H_` include guards; Doxygen on exported functions;
comments that explain *why*.

The last one matters more here than usual. Most of
[`analysis/05`](../analysis/05-essential-vs-incidental.md) exists because nobody
recorded why a constant had its value, and separating protocol from accident
meant reverse-engineering a VB translation. **Every normative constant carries a
comment saying where it comes from.**

---

## Layout

```
core/
  codec/     frame tables, RS, CRC, Packed6, StationId   — pure, no state
  modem/     modulate / demodulate / sync / busy detect  — explicit state
  link/      ARQ + FEC + RXO state machines              — explicit state
```

Dependencies point down only: `link` → `modem` → `codec`. The only thing any
`core/` module takes from `src/` is the compile-time-only `common/mustuse.h`
macro header (rule 8); no core module includes `src/` code or links a
`src/`-defined symbol.

Include paths are rooted at `core/`, so a module is included as
`#include "codec/frame.h"` — matching the existing `-Isrc` convention.

---

## Building

```sh
make core          # build core objects with -Werror
make check-pure    # assert no mutable globals, no allocation
make test-core     # run core unit tests
```

`make` still builds `ardopcf` exactly as before. `core/` does not yet
participate in the program.

---

## Status

| Layer | Module | State |
|---|---|---|
| `codec` | `frame` — frame type table | **done**, all 256 types proven equivalent to `FrameInfo()`; frame-type parity symbol proven equivalent to `ComputeTypeParity()` |
| `codec` | `crc` — CRC-16 and CRC-8 | **done**, proven equivalent to `GenCRC16()`/`GenCRC8()` over a random corpus |
| `codec` | `packed6` — 6-bit callsign/grid packing | **done**, proven equivalent to the original `Packed6` over a random corpus |
| `codec` | `rs` — Reed-Solomon FEC | **done**, 5 globals moved into a caller-owned context; tables and encode/decode proven equivalent to `lib/rockliff/rrs` |
| `codec` | `stationid` — callsign + SSID | **done**, proven equivalent to the original `StationId` (one non-normative `strerror` off-by-one fixed) |
| `codec` | `locator` — Maidenhead grid square | **done**, proven equivalent to the original `Locator` (empty-input now zeroes its output) |
| `codec` | `dataframe` — data-frame byte encoder | **done**, the TX counterpart to the demod's per-carrier decode; lays a payload out as `[type][type^session]` + per-carrier `[len][data][CRC][RS]` (and the 4FSK.2000.600 3-part layout). Proven byte-exact to `EncodePSKData`/`EncodeFSKData` across every modulation, carrier count and fill level |
| **`codec`** | **layer complete** | frame, crc, packed6, rs, stationid, locator, dataframe |
| `modem` | `modulate` — TX (all modulations) | **done**, every sample of the whole golden corpus (22 control + 108 data cases: 4FSK 50/100/600, 4PSK/8PSK/16QAM at 1–8 carriers) proven bit-identical to ardopcf |
| `modem` | `goertzel` — single-bin DFT + peak locator | **done**, the RX DSP foundation; proven bit-identical to the `GoertzelRealImag*`/`SpectralPeakLocator` originals |
| `modem` | `demodulate` — RX pipeline (stages 1–4) | in progress — leader search, downmix/2 kHz filter, symbol + frame sync, and the whole frame-type stage (tone demod, per-candidate decode distance, and the minimal-distance acceptance decision) done, each bit-identical to its `SoundInput.c` original (`SearchFor2ToneLeader3`, `MixNCOFilter`/`FSMixFilter2000Hz`, `Acquire2ToneLeaderSymbolFraming`/`AcquireFrameSyncRSB`, `DemodFrameType4FSK`/`ComputeDecodeDistance`/`MinimalDistanceFrameType`); the acceptance decision takes the protocol context (session id, connection flags) as explicit inputs rather than reading globals, keeping the core pure. The modulator's own frame is downmixed, both sync searches lock onto its sync symbol, and its frame-type field scores a small decode distance for its own type (TX→RX join through stage 4). Stage 5 in progress: all three modulations' per-carrier demod + decode are done and bit-identical — 4FSK (`Demod1Car4FSKChar`), PSK (`InitDemodPSK`/`Demod1CarPSKChar`/`Decode1CarPSK`), and 16QAM (`Demod1CarQAMChar`/`Decode1CarQAM`, phase sector + rolling-threshold amplitude bit) — plus the per-carrier RS-correct + CRC-check backend (`CorrectRawDataWithRS`, composing the already-ported core `rs`/`crc`). A carrier of any modulation now goes samples→symbols→bytes→RS-corrected payload entirely in core. **Stage 6 started — `ardop_demod_push` (the streaming FSM, ported from `ProcessNewSamples`) drives the whole receiver:** it buffers samples, runs leader search → downmix → sym/frame sync → frame-type acquisition → streaming per-byte demod with buffer compaction, and emits events (`LEADER_DETECTED`, `FRAME_DECODED`, `FRAME_BAD`) instead of the inherited inline transmit/host/GUI calls. Protocol context (session keys, RS) is passed in, keeping the core pure. **Full core TX→RX round trips work end-to-end for every modulation** — 4FSK, 4PSK, 8PSK and 16QAM, across 1/2/4/8 carriers: a data frame modulated by the core modulator, pushed through `ardop_demod_push` in chunks, decodes back to its exact payload. The `AcquireFrame` streaming is wired for all three demod families (4FSK byte loop; PSK/QAM training-symbol skip, per-carrier realignment, phase→bits, and QAM's tuning-offset phase correction). **Validated against the golden WAV corpus** (`make golden-core`): the frozen ardopcf recordings — real recorded audio, not the core's own modulator output, including noise-degraded copies — are pushed through `ardop_demod_push` in receive-only mode and decode byte-exact to the manifest for every supported frame type; the core even recovers `16QAM.2000.100.E` at 20 dB, which the inherited *standard* decoder records as a failure and only its SDFT decoder recovers. The **4FSK.2000.600 3-part frame** is decoded too: one long 600-baud 4FSK carrier whose 759 bytes are three independent `[len][data][crc][rs]` sub-blocks, streamed sequentially and RS-corrected per part (ported from the `ProcessNewSamples`/`DecodeFrame` 0x7A/0x7B path); it decodes byte-exact under `make golden-core`. Remaining (surfaced as explicit gaps by the golden-core check): MemARQ retries and `IDFrame`/`ConReq` content decode (Packed6 + host formatting) ([analysis/11](../analysis/11-demod-design.md)) |
| `modem` | `busy` — channel-busy detector | **done**, ported from `BusyDetect3`; caller-owned state, ms clock and bandwidth/sensitivity passed in. `test/core/test_busy.c` pins 20 000 randomised steps bit-for-bit to the original, including the hysteresis bookkeeping. Preserves the dead rolling-average accident ([analysis/12](../analysis/12-normative-accidents.md) #6) |
| `modem` | `rxquality` — decode-quality metric | **in progress** — the constellation quality a receiver reports (rides in the ACK/NAK, drives gear-shift). the pure metrics are **done** — 4FSK (`Update4FSKConstellation`, preserving the plot-geometry accident, [analysis/12](../analysis/12-normative-accidents.md) #7) and PSK/QAM (`UpdatePhaseConstellation`), each proven bit-for-bit over 20 000 patterns. Remaining: wire them into the demod's deliver path to fill `ardop_event.quality` |
| **`modem`** | **core RX/TX complete** | modulate, goertzel, demodulate, busy, rxquality |
| `link` | `session` — ARQ session ID + call match | **done**; `ardop_session_id` (from `GenerateSessionID`, CRC-8 of the two callsigns, 0xFF→0 remap) and `ardop_call_to_me`/`ardop_ping_to_me` (from `IsCallToMe`/`IsPingToMe`, with the local callsign set passed in rather than global). `test/core/test_session.c` proves the ID against the original over 200 000 random pairs and the match (yes/no + reply session ID, incl. aux calls) against `IsCallToMe` |
| `link` | `quality` — ACK/NAK quality + averaging | **done**, ported from `EncodeDATAACK`/`EncodeDATANAK`, the `38 + 2·(type&0x1F)` decode, and `ComputeQualityAvg` (the α=0.5 gear-shift average). `test/core/test_quality.c` matches all three over their ranges (preserving that the NAK encoder, unlike the ACK, does not clamp `q > 100`) |
| `link` | `bandwidth` — session BW negotiation | **done**, ported from `IRSNegotiateBW`; reconciles a received ConReq (width + max/forced) against this station's setting into a ConAck at the agreed width, or ConRejBW. `test/core/test_bandwidth.c` matches the original over all 9 settings × 256 type bytes. Added the canonical named control frame-type constants (`ARDOP_FT_*`) to [`codec/frame.h`](codec/frame.h), their proper home |
| `link` | `frames` — control-frame builders | **done**, ported from `Encode4FSKControl`/`EncodeConACKwTiming`/`EncodePingAck`; the encoded bytes the FSM attaches to a send-frame action. `test/core/test_frames.c` matches the originals byte-for-byte across their input ranges (the timing cap/clamp and the PingAck S/N-and-quality packing) |
| `link` | `datamodes` — gear-shift mode ladder | **done**, the ordered data-mode lists and shift-up thresholds per bandwidth (`GetDataModes`/`GetShiftUpThresholds`), which the sender walks to trade throughput for robustness. Proven equivalent to the originals across every bandwidth and the FSK-only / tuning-range / 600-mode table splits |
| `link` | `link` — the FSM (`ardop_link_step`) | **in progress** — the machine is on the board: one unified input (demod event / host command / timer per [analysis/10](../analysis/10-modem-link-design.md) decision 6), actions out, one collapsed state enum replacing the inherited `ProtocolState`/`ARQState` pair. DISC-state transitions ported so far: a straggler `DISCFRAME` → `END` (rule 1.5); and the connection-setup **`ConReq` → `ConAck`/`ConRejBW`** (rules 1.2/1.3), and **`PING` → `PINGACK`** (reply with the measured S/N and quality, arm a closing ID) — decode the caller/target from the payload, `ardop_call_to_me` + `ardop_negotiate_bandwidth`, then accept (ConAck with timing, become IRS pending, arm the 10 s timeout) / reject-bandwidth / not-for-us (`CANCELPENDING`), composing four of the link leaves. The first **host command** is in too — `CONNECT` (`ARQCALL`) builds and sends a ConReq (bandwidth→type via a new `ardop_bandwidth_conreq_type`, RS-appended through a caller-supplied `rs` context), becomes ISS awaiting ConAck, and arms the 2 s resend — exercising the `ARDOP_IN_HOST` input path end-to-end. The outbound handshake then completes: **ISS awaiting ConAck** adopts the negotiated width and replies with its own timing ConAck (rule 1.4), or aborts to DISC on ConRejBusy/ConRejBW — session validation is implicit in the decode, so no session check is needed. On the IRS side the handshake then finishes — receiving the ISS's ConAck commits the session, tells the host `CONNECTED`, sends a DataACK carrying the decode quality, and enters IRS_DATA (rule 1.4). Both stations now reach their connected data states; the full four-message ARQ connect handshake is ported and tested. The IRS then **receives data**: a good data frame is delivered to the host (`DELIVER_DATA`) and always ACKed with the decode quality, deduplicated by frame type against retransmissions; a failed decode is re-ACKed if already delivered else NAKed; a `DISCFRAME` ends the session (`DISCONNECTED` + `END`). On the **ISS side**, the handshake completes on the IRS's DataACK (`CONNECTED`), and the station starts **sending data**: `SEND_DATA` queues into a caller-owned buffer, and the ISS emits the first data frame at the most-robust mode for the bandwidth (parity-toggled for retransmit detection), encoded via `dataframe`, or an IDLE keep-alive when the queue is empty. On each **DataACK** the ISS confirms the outstanding frame (dropping its bytes from the queue), folds the reported quality into the running average, runs the **gear-shift** (`Gearshift_9`: shift down after consecutive NAKs, up when quality clears the mode's threshold with data remaining), and sends the next frame (alternating parity) or IDLE; a **DataNAK** shifts down and resends. This closes the ARQ data loop — the core now drives a full connect → data-both-ways → disconnect session. A **timer service** on the `ARDOP_IN_NONE` step resends the outstanding frame when no response arrives within the repeat interval, and gives up on a pending connection that never completes (`DISCONNECTED` → DISC). Host **DISCONNECT** (send DISC, retry up to five times, complete on the peer's END) and **ABORT** (drop immediately) round out the host-facing surface, and an **END** received in any connected state tears the link down (a shared teardown path). Proven by scripted input→action tests (`test/core/test_link.c`), the method [analysis/10](../analysis/10-modem-link-design.md) §9 3a defines. Remaining: IRS/ISS data states, gear-shift (`Gearshift_9` + mode ladder, stateful, land here), the busy-block guard, and the two-instance loopback (3b) |

The carrier-waveform templates (`int50BaudTwoToneLeaderTemplate` etc.) now live
in `core/modem/templates.c`, declared by `modem/templates.h`. They were
relocated from `src/common/ardopSampleArrays.c`; the inherited tree still links
this core object for them during the transition. With that move, no core object
links against a `src/`-defined symbol — the only remaining reference into `src/`
is the compile-time-only `common/mustuse.h` macro header (rule 8).

### A note on "these modules already qualify"

[`analysis/05`](../analysis/05-essential-vs-incidental.md) listed nine modules as
carrying across largely as-is because they "already have interfaces, no
globals". Running rule 1's check against them shows that claim was too
generous:

| Module | Mutable globals |
|---|---|
| `Packed6.o`, `FFT.o`, `log_file.o`, `StationId.o`, `Locator.o` | 0 — genuinely clean |
| `wav.o` | 1 — a global header struct; WAV writing is not reentrant |
| `noise.o` | 2 — Box-Muller spare-value cache |
| `sdft.o` | 5 — real module state |
| `lib/rockliff/rrs.o` | 5 — Galois field tables, in the most normative code in the system |
| `log.o` | 21 — a global service by design |

An earlier version of this table listed `StationId.o` and `Locator.o` as having
one global each, "a non-`const` string table". That was the `nm`-letter false
positive that also shaped rule 1's check (see rule 1): the `strerror` message
tables are `const` and land in `.data.rel.ro`, which `nm` mislabels `d`. The
section-based `objdump` check reports them correctly as clean, which the
completed `stationid`/`locator` ports confirm. `sdft.o` and `rrs.o` are the real
module state; `rrs.o` is now moved.

So some of them still need a state handle before they can move, and the
"~2500 lines carry across unchanged" estimate is optimistic — but fewer than the
first pass feared. This is exactly the kind of thing rule 1 exists to surface.
