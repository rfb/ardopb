# 11 — The demodulator, made concrete

[10](10-modem-link-design.md) fixed the modem/link *boundary* — `ardop_demod_push`
consumes samples and drains events, reads no clock but the sample count, and
never transmits. That was enough to build the transmitter against. The receiver
needs more: it is one tightly-coupled state machine over **119 file-scope
globals** (`SoundInput.o`), and porting it means designing the field-level home
for all of that — the `ardop_demod` context — and the push-driven FSM that walks
it. This document does that, so the receive stages can be ported into a shape
that is already decided rather than one discovered mid-port.

The transmitter is done and byte-exact; the Goertzel DSP primitives the receiver
rests on are done (`core/modem/goertzel`). This is the plan for everything above
them.

Scope: the `ardop_demod` fields, the FSM and its events, how the one clock and
the sample buffers thread through, and the order the stages are ported in with
the oracle for each. Non-goals: the DSP maths (transliterated, pinned to the
originals) and the link/host layers (later stages).

---

## 1. What the receiver is today, and the one thing to sever

`ProcessNewSamples()` (`SoundInput.c:810`) is the receive entry. It runs a
five-state acquisition machine (`enum _ReceiveState`, `ardopcommon.h:257`):

```
SearchingForLeader → AcquireSymbolSync → AcquireFrameSync
                   → AcquireFrameType → (AcquireFrame) → decode
```

Each state consumes buffered samples, advances shared state, and either advances
the FSM or falls back to `SearchingForLeader`. The stages hand each other work
through globals: the leader search writes the tuning offset and the NCO
frequency and sets `State = AcquireSymbolSync` itself (`SoundInput.c:1838-1842`);
the mixer downconverts to a baseband buffer; sync and frame-type walk that
buffer; the demodulator and `DecodeFrame()` turn it into bytes.

**The one illegal thing to remove.** At frame-type acquisition, on a short
control frame, the receiver reaches into the protocol and *transmits*:
`ProtocolState == IRStoISS` triggers `txSleep(250)` and the IRS→ISS turnover
inline (`SoundInput.c:1070-1079`). The demodulator's job ends at "a frame
arrived"; deciding what to do about it is the link's. In the port that becomes
an **event** the link consumes (§4), which is what lets the receiver be tested
with no transmitter and no protocol state at all.

---

## 2. One clock: elapsed samples

Every time the receiver reads is `Now` (`ARDOPC.h:46`, wall-clock ms). In the
port there is no `Now`; `ardop_demod_push` is given `t_end_samples`, the absolute
sample-count timestamp of the last sample in the pushed buffer, and every
deadline is derived from it:

| Inherited (ms, wall clock) | Port (samples, from `t_end_samples`) |
|---|---|
| 1000 ms frame-sync timeout (`SoundInput.c:978`) | 12000 samples |
| 20000 ms full-search gate (`SoundInput.c:1633`) | 240000 samples |
| `dttLastLeaderDetect`, `dttLastGoodFrameTypeDecode`, `MemarqTime` | sample-count fields in `ardop_demod` |

This is the generalisation of what `--decodewav` already does: it runs this
entire chain from a file on `WavNow`, a sample-derived clock (`ARDOPCommon.c:552`).
The receiver on a synthetic clock is not speculative — half of it ships today.

---

## 3. The `ardop_demod` context

All 119 globals sort into five groups. The struct is caller-owned (declared by
the shell, no allocation) and, unlike the modulator, holds its own sample
buffers because acquisition spans many `push` calls.

```c
typedef struct ardop_demod ardop_demod;   /* fields below, grouped */
```

**a. Configuration** — set once at construction, then read-only.
`tuning_range` (100 Hz, `ARDOPC.c:102`), `squelch` (5, `ARDOPC.c:109`),
`protocol_mode` (ARQ/FEC/RXO), `arq_timeout`, `fec_memarq_timeout`. These are the
handful of host settings the DSP consults; passing them in makes a demod
instance reproducible.

**b. Acquisition FSM state.**
`state` (the five-state enum), `offset_hz` (tuning offset), `nco_freq` /
`nco_phase` / `nco_phase_inc` (the downmix oscillator, `SoundInput.c:601`),
`frame_type` and its decoded `ardop_frame_spec`, `prior_fine_offset`
(`SoundInput.c:1602`), and the timestamps from §2, all in samples.

**c. Sample buffers** — context-owned, fixed capacity, bounded by the longest
frame.
`raw[]` + `raw_len` (pre-mix samples held when a leader search comes up short,
`SoundInput.c:904`), `filtered_mixed[]` + `len` + `read_ptr` (the baseband buffer
the stages walk, `SoundInput.c:965-976`), `prior_mixed[]` (mixer history),
`tone_mags[]` + `tone_mags_index`.

**d. Per-frame demod / tracking** — reset at each new frame.
Phase and magnitude histories, the SDFT resonator state, the constellation
accumulators, the running S/N. These are the working set of `DemodPSK` /
`DemodQAM` / `Demod1Car4FSK*`.

**e. Memory ARQ** — see §5.
`carrier_ok[]`, `tone_mags_avg[]`, `car_phase_avg[]`, `car_mag_avg[]`,
`sum_counts[]`, `last_data_frame_type`, `memarq_time`.

---

## 4. The push API and the events

```c
size_t ardop_demod_push(ardop_demod *d,
                        const int16_t *samples, size_t n,
                        uint64_t t_end_samples,
                        ardop_event *events, size_t max_events);
```

`push` appends the samples to `raw[]` and runs the FSM as far as the available
data allows, writing any events into the caller's array (capacity bounded by a
provable per-call maximum, §10 of [10](10-modem-link-design.md)). It reads no
protocol state and never transmits.

The events are the receiver's whole outward vocabulary, drawn from what
`ProcessNewSamples` does at each terminal point today:

```c
typedef enum {
    ARDOP_EV_LEADER_DETECTED,  /* acquisition started; offset, S/N          */
    ARDOP_EV_FRAME_DECODED,    /* good frame: type, session, quality, bytes */
    ARDOP_EV_FRAME_BAD,        /* frame detected, failed CRC/RS             */
    ARDOP_EV_BUSY_CHANGED,     /* channel busy/clear (from BusyDetect)      */
    ARDOP_EV_DELIVER_ERR_DATA, /* stale Memory-ARQ data, flushed as ERR     */
} ardop_event_kind;
```

`ARDOP_EV_FRAME_DECODED` replaces the four decode-delivery calls the receiver
makes inline today — `ProcessRcvdARQFrame` / `ProcessRcvdFECDataFrame` /
`ProcessRXOFrame` / `AddTagToDataAndSendToHost` (`SoundInput.c:1294-1328`). The
link layer routes it; the demodulator just reports it. The `IRStoISS` transmit
(§1) disappears entirely: the link sees `FRAME_DECODED` with an ACK type and
decides the turnover.

---

## 5. Memory ARQ — settled: demod-local, keyed on the frame type

The deferred decision from [10](10-modem-link-design.md) §8.2. The receiver
averages the *soft* per-carrier values across retransmissions of the same data
frame — `tone_mags_avg`, `car_phase_avg`, `car_mag_avg`, `sum_counts` — to pull a
marginal frame out of the noise (`ResetMemoryARQ`, `SoundInput.c:295`). The code
confirms the lean: the averaging is keyed entirely on `LastDataFrameType`
(`SoundInput.c:301`), which is the even/odd frame type the acquisition stage
already recovers. A retransmission is "same data frame type again", decidable
inside the demod with no help from the link.

So the buffers live in `ardop_demod` (group e), keyed on `last_data_frame_type`.
The only outward-facing part is staleness: `CheckMemarqTime` (`SoundInput.c:306`)
resets the buffers after `arq_timeout` / `fec_memarq_timeout` and today calls
`PassFECErrDataToHost()` directly. In the port that call becomes an
`ARDOP_EV_DELIVER_ERR_DATA` event, and the staleness check runs on the sample
clock. No link involvement in the averaging; the link only learns of it through
the same event stream as everything else.

---

## 6. Sample buffering and the clock, threaded

`push` owns the accumulation the globals do today: append to `raw[]`; when a
leader is found, `MixNCOFilter` (`SoundInput.c:587`) downconverts `raw[]` into
`filtered_mixed[]` and `raw[]` is drained; the sync/frame-type stages advance
`read_ptr` and periodically compact `filtered_mixed[]` (`SoundInput.c:973`).
Insufficient-data points return cleanly and wait for the next `push` — exactly
the `intFrameType == -2` early return today (`SoundInput.c:994-998`), now
expressed as "no event yet, call me again". The buffers are fixed-capacity
fields sized to the longest frame; no allocation, matching rule 3.

---

## 7. Porting order, and the oracle for each stage

Bottom-up, each stage proven before the next builds on it. The oracle shifts
from "the original function" for the leaf DSP to "the golden decode result" once
whole frames come out.

| # | Stage | Source | Oracle |
|---|---|---|---|
| 0 | Goertzel + peak locator | `GoertzelRealImag*` | **done** — bit-exact vs originals |
| 1 | Leader search | `SearchFor2ToneLeader3` | vs original on random + on the modulator's own leader (a real TX→RX join) |
| 2 | Mix / filter to baseband | `MixNCOFilter`, `FSMixFilter2000Hz` | vs original, sample-for-sample |
| 3 | Symbol + frame sync | `Acquire2ToneLeaderSymbolFraming`, `AcquireFrameSyncRSB` | vs original on the golden WAVs |
| 4 | Frame-type acquisition | `Acquire4FSKFrameType`, `MinimalDistanceFrameType` | golden `decode.frame_type` for every frozen WAV |
| 5 | Per-modulation demod + RS decode | `Demod1Car4FSK*`, `DemodPSK`, `DemodQAM`, `DecodeFrame`, `CorrectRawDataWithRS` | golden `decode` result: type, session, payload bytes |
| 6 | `ardop_demod_push` FSM assembly | `ProcessNewSamples` | decode every golden WAV end-to-end; match `--decodewav` frame-for-frame |

Stage 1 is the natural start and the satisfying one: feed the sample stream
`ardop_mod` produces for a leader straight into `ardop_leader_search` and require
detection at the right offset — the transmitter and receiver meeting for the
first time, both proven against the same reference.

The `test/golden` corpus already carries the decode oracle for stages 4–6: every
case stores `decode.frame_type`, `session_id` and `expected_payload`, and the
frozen/degraded WAVs exist to decode. Stage 6's exit criterion is that the core
receiver decodes them to the same results the inherited `--decodewav` does.

---

## 8. Decisions

- **Memory ARQ placement — settled** (§5): demod-local, keyed on
  `last_data_frame_type`; staleness on the sample clock; err-flush via
  `ARDOP_EV_DELIVER_ERR_DATA`.
- **Sample buffers live in the context, not the caller** — unlike the modulator.
  Acquisition spans many `push` calls with retained partial state, so the buffers
  are `ardop_demod` fields, fixed-capacity, no allocation.
- **Window caches stay recomputed for now** (as in `goertzel`); a caller-owned
  window cache is the optimisation once stage 6 exists and the hot loop is
  measurable. Deferred deliberately, not forgotten.
- **`SlowCPU` is dropped, not ported** — it changed the leader-search hop by CPU
  speed (`SoundInput.c:888`), making decode outcome machine-dependent. The port
  uses the one hop; see [03](03-timing-model.md).

---

## Status

Design only. Complete below this line: `goertzel` (stage 0). The transmitter is
byte-exact end to end, which is what makes stage 1's TX→RX join a test rather
than a hope. This note is the contract stages 1–6 are written against.
