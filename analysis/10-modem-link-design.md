# 10 — The modem and link boundary, made concrete

[06](06-target-architecture.md) fixed the *shape* of the rebuild — a sans-I/O
core, one sample clock, transitions that return actions — and gave a sketch API.
This document turns that sketch into a buildable contract for the two hard
layers, `modem` and `link`, because that is where the sketch stops being obvious.
The `codec` layer is done (six modules under `core/codec`, each proven against
the original); it needed no design note because it was pure functions over
bytes. `modem` and `link` own state and stand where the clock and the FSMs are
entangled, so the interface between them, and between them and the shell, has to
be decided before code is written rather than discovered during it.

Scope: the data types that cross each boundary, who owns which buffer, how the
clock is threaded, and the order the pieces are built in. Non-goals: the DSP maths
(transliterated, not redesigned — [06](06-target-architecture.md) criterion 1),
and the platform shell (deferred to migration Stage 4).

The compatibility target is unchanged and still binds: bit-exact on air, and the
existing TCP host interface.

---

## 1. The sample-count clock

The single decision everything else hangs off. Time in the core is `uint64_t`
**samples elapsed at the nominal 12000 Hz**, passed in explicitly. There is no
`Now`, no `getTicks()` (`ARDOPC.h:46`), no millisecond global.

This is not speculative. The receive path already runs on exactly this clock when
decoding a WAV: `WavNow += blocksize * 1000 / 12000` (`ARDOPCommon.c:552`) is a
sample-derived millisecond counter, and `--decodewav` decodes real frames with it
today. The design generalises what already works for files to the whole core, and
keeps samples rather than reducing to milliseconds so no resolution is lost.

**Converting the protocol's millisecond constants.** The normative timing values
stay as they are, converted at the point of use:

```
samples = ms * 12000 / 1000 = ms * 12
```

So the 250 ms turnaround becomes 3000 samples; `ComputeInterFrameInterval`'s
1500–2100 ms repeat intervals (`ARQ.c:879-947`) become 18000–25200 samples. The
values are unchanged; only their unit is. A deadline is an absolute sample count,
compared with `>=`, never a wall-clock instant.

**Why this removes a whole failure class.** If the card actually runs at 11997
Hz, every deadline stretches by the same 0.025% because they are all measured in
that card's own samples. Protocol timing stays self-consistent. `FixTiming`
(`ALSASound.c:1050-1146`) exists only to detect the case where it would *not*
have been consistent — that case no longer exists, so the check, `-A`, and
`SlowCPU` (`SoundInput.c:888`) all stop being needed. Rate error stays a DSP
concern (the demodulator already tracks it via `dblOffsetHz`), not a protocol
one.

**The two survivors.** Wall-clock time remains for exactly two things, both in
the shell, never in the core: log timestamps, and the regulatory 10-minute ID,
which is a legal obligation measured in real seconds regardless of audio rate.

---

## 2. TX — the modulator produces samples on demand

Today the modulator *plays*: `Mod4FSKDataAndPlay()` (`Modulate.c:119`) runs the
whole frame through `SampleSink()` (`Modulate.c`), which filters each sample
through a resonator bank held in file-scope state and hands it to the sound card,
keying the radio on the way (`Modulate.c:693`). It returns when the audio has
been queued. The core version inverts this: it *yields* samples when asked, holds
its filter state in a caller-owned context, and touches no device.

```c
typedef struct ardop_mod ardop_mod;   /* opaque; caller owns the storage */

/* Begin a frame. Encoded bytes are the codec-layer output; leader in ms.
 * Copies what it needs; the caller may reuse `encoded` after return. */
void ardop_mod_begin(ardop_mod *m, uint8_t frame_type,
                     const uint8_t *encoded, size_t len, uint16_t leader_ms);

/* Yield up to `max` samples into `out`. Returns the count written; 0 means the
 * frame (leader + data + any trailer) is fully drained. Never blocks. */
size_t ardop_mod_pull(ardop_mod *m, int16_t *out, size_t max);

bool ardop_mod_busy(const ardop_mod *m);   /* frame in progress? */
```

**Ownership.** `out` is the caller's buffer; the modulator writes into it and
never allocates. The resonator/comb state (`dblZout_*`, `Last120`, `DriveLevel`
and friends, all file-scope in `Modulate.c` today) moves into `ardop_mod`. Drive
level is set once at construction, not read from a global mid-stream.

**Why TX is first and easiest.** It has no clock dependence at all — a frame is a
deterministic function of its bytes and the drive level — and it is exactly what
the [golden vectors](../test/golden/README.md) freeze byte-for-byte. The exit
criterion writes itself: for every frozen case, the concatenation of
`ardop_mod_pull()` output is SHA-256-identical to the stored modulator output.
That is the strongest possible conformance check and it needs no audio hardware.

**PTT is not the modulator's job.** Keying the radio becomes an *action* the
shell performs (§4), removing the `Modulate.c:693` keying-from-inside-DSP.

---

## 3. RX — the demodulator consumes samples, emits events

Today `ProcessNewSamples(short *Samples, int nSamples)` (`SoundInput.c:810`) is
the receive entry, and it is the worst offender in the codebase: it consults
`ProtocolState`, performs the IRS→ISS turnover inline (`SoundInput.c:1070`), and
calls `SendData()` — the demodulator transmits. The core version consumes samples
against explicit acquisition state and emits **events as data**. It never inspects
protocol state and never transmits.

```c
typedef struct ardop_demod ardop_demod;   /* opaque; holds all acquisition state */

/* Push n samples captured ending at absolute time t_end_samples. Drains zero or
 * more events into `events`; returns how many were written (capped at max_events,
 * with the rest available on the next call). Never blocks, never transmits. */
size_t ardop_demod_push(ardop_demod *d,
                        const int16_t *samples, size_t n,
                        uint64_t t_end_samples,
                        ardop_event *events, size_t max_events);
```

**What moves into `ardop_demod`.** The acquisition FSM (`SearchingForLeader` →
`AcquireSymbolSync` → `AcquireFrameSync` → `AcquireFrameType` → `AcquireFrame`),
the NCO/filter/tracking state, the busy detector, and Memory ARQ's averaged
symbol buffers (`SoundInput.c:295-340`) — all file-scope today. The key deletion
is the `ProtocolState` check: the demodulator's job ends at "a frame arrived, or
the channel is busy," expressed as an event. What to *do* about it is the link
layer's decision, made from an event, not a callback into the demodulator.

**The clock enters here.** `t_end_samples` is the timestamp of the last sample in
the buffer; the demodulator derives every internal deadline (the 1000 ms
frame-sync timeout at `SoundInput.c:978` becomes 12000 samples) from it. This is
the generalisation of `WavNow`.

---

## 4. The two data types that cross the boundaries

Events go up (demod → link); actions come out (link → shell). Both are plain
tagged structs, never calls. The enumerations below are grounded in what
`ProcessRcvdARQFrame()` (`ARQ.c:1113`) actually does today, recast as data.

```c
typedef enum {
    ARDOP_EV_FRAME_DECODED,   /* a good frame: type, payload, session, quality */
    ARDOP_EV_FRAME_BAD,       /* frame detected but failed CRC/RS             */
    ARDOP_EV_BUSY_CHANGED,    /* channel busy/clear transition                */
    ARDOP_EV_LEADER_DETECTED, /* acquisition started (for timers/UI)          */
} ardop_event_kind;

typedef struct {
    ardop_event_kind kind;
    uint64_t         t_samples;   /* when it happened, in the one clock */
    uint8_t          frame_type;
    uint8_t          session_id;
    uint8_t          quality;
    uint16_t         payload_len;
    uint8_t          payload[ARDOP_MAX_PAYLOAD];  /* fixed; no allocation */
} ardop_event;
```

```c
typedef enum {
    ARDOP_ACT_NONE,
    ARDOP_ACT_SEND_FRAME,    /* modulate & transmit: frame_type + payload    */
    ARDOP_ACT_SET_PTT,       /* key/unkey (bool in `arg`)                    */
    ARDOP_ACT_SET_TIMER,     /* absolute deadline in samples                 */
    ARDOP_ACT_NOTIFY_HOST,   /* a host-protocol message (string in payload)  */
    ARDOP_ACT_DELIVER_DATA,  /* decoded payload up to the host               */
} ardop_action_kind;
```

`ARDOP_ACT_SEND_FRAME` is the load-bearing one: it is how the link asks for a
transmission *without transmitting*, which is what removes `txSleep()`
(`ALSASound.c:587`) and with it the TX-inside-TX reentrancy chain
([03](03-timing-model.md)). The shell receives the action, calls
`ardop_mod_begin`/`ardop_mod_pull`, and observes completion by the pull draining
to zero — TX end is *observed*, never *predicted* by dividing a sample count by a
nominal rate as `SoundFlush()` does today (`ALSASound.c:1789`).

---

## 5. link — the FSM steps, and returns actions

```c
typedef struct ardop_link ardop_link;

/* Advance the machine by one event (or NULL to service timers only) as of
 * t_samples. Writes any resulting actions into `actions`; returns the count.
 * Pure: same state + same input => same output. Never performs I/O. */
size_t ardop_link_step(ardop_link *l,
                       const ardop_event *ev,     /* may be NULL */
                       uint64_t t_samples,
                       ardop_action *actions, size_t max_actions);
```

This is the [02](02-protocol-fsm.md) machine — session establishment, bandwidth
negotiation, ACK/NAK quality, IRS/ISS turnover, gear-shifting, FEC and RXO — with
the `Mod*AndPlay` calls in `ProcessRcvdARQFrame()` replaced by returned
`ARDOP_ACT_SEND_FRAME` actions and its wall-clock timers replaced by
`ARDOP_ACT_SET_TIMER` deadlines the shell fires back as `ev == NULL` steps.

**Why this is the testable prize.** A pure step function with explicit state can
be driven with no audio at all: feed it a scripted event sequence and assert the
actions. Better, two `ardop_link` instances can be wired to each other in-process
— one's `SEND_FRAME` action, run through `ardop_mod` then `ardop_demod`, becomes
the other's `FRAME_DECODED` event — exercising connect/data/turnover/gear-shift/
timeout deterministically and without hardware. None of that is possible today.

---

## 6. State ownership and buffers

Consistent with the codec layer and rule 1 (`core/README.md`):

- **All state in caller-owned structs.** `ardop_mod`, `ardop_demod`, `ardop_link`
  are declared by the shell (on the stack or in a static arena) and passed in. No
  module holds file-scope mutable state, so `make check-pure` stays green for
  `modem` and `link` exactly as it does for `codec`.
- **No hidden allocation.** Every buffer is caller-provided with its size; events
  and actions land in caller arrays with an explicit cap and the overflow carried
  to the next call. Payloads are fixed-size (`ARDOP_MAX_PAYLOAD`), sized to the
  largest frame the [frame table](../core/codec/frame.h) allows.
- **Samples are `int16_t` at the boundary.** Internally the DSP uses `float`
  where it does today; the boundary type is the 16-bit PCM the device speaks, so
  no conversion policy leaks into the API. (Open question §8.)

---

## 7. The main loop, and what it deletes

The shell becomes readable in one sitting, and single-clocked:

```c
while (running) {
    n = platform_read_audio(buf, N);            /* the ONLY clock source */
    t += n;
    nev = ardop_demod_push(demod, buf, n, t, events, MAX_EV);
    for (i = 0; i < nev; i++)
        for (na = ardop_link_step(link, &events[i], t, acts, MAX_ACT), a = 0; a < na; a++)
            perform(&acts[a]);                  /* SEND_FRAME, PTT, host… */
    for (na = ardop_link_step(link, NULL, t, acts, MAX_ACT), a = 0; a < na; a++)
        perform(&acts[a]);                      /* service timers */
    if (tx_in_progress)
        platform_write_audio(txbuf, ardop_mod_pull(mod, txbuf, N));
}
```

Absent from it: `txSleep`, blocking transmit, reentrancy, `ProtocolState` read
inside the demodulator, `SlowCPU`, `FixTiming`, and any clock but `t`.

---

## 8. Decisions to settle before/while building

These are genuine forks, called out so they are chosen deliberately:

1. **Boundary sample type — `int16_t` vs `float`.** `int16_t` matches the device
   and the golden vectors (which are 16-bit WAV) and makes TX conformance a
   byte-compare. Float would defer one conversion. **Leaning `int16_t`** for the
   exact-compare property; revisit only if a demod stage needs sub-LSB input.
2. **Where Memory ARQ lives.** It averages *symbols* across retransmissions
   (`SoundInput.c:295-340`), which is demod state, but it is triggered by a
   *protocol* retransmission, which is link knowledge. **Leaning:** keep the
   averaging buffers in `ardop_demod`, let the link enable/reset it via a small
   control call or an action. Needs confirming against the retransmit flow.
3. **Event/action queue sizing.** A single decoded frame can produce several
   actions (PTT, send, timer). Fixed caps with carry-over (above) vs a
   caller-drained ring. **Leaning** fixed caps; they are simplest and the counts
   are small and bounded.
4. **Modem granularity.** One `ardop_mod` that switches on `frame_type`, or a
   per-modulation family split. **Leaning** one context, dispatching on the
   [frame spec](../core/codec/frame.h), mirroring how `FrameInfo` already keys
   everything off the type byte.
5. **Busy detector placement.** It is DSP (`BusyDetect.c`) but its output is a
   protocol input. It stays in `modem`, surfacing `ARDOP_EV_BUSY_CHANGED`; the
   link consumes it. Low-risk, noted for completeness.

---

## 9. Build order and exit criteria (migration Stages 2–3)

Each step keeps the inherited tree building and adds a `core/` module proven
before the next starts — the discipline the codec layer used.

| Step | Module | Proven by |
|---|---|---|
| 2a | `modem` TX (`ardop_mod`) | golden vectors: pulled samples SHA-256-identical to frozen modulator output, every case |
| 2b | `modem` RX (`ardop_demod`) | decode each frozen/AWGN WAV via `push`; frame sequence matches the golden decode |
| 3a | `link` FSM (`ardop_link`) | scripted event → action sequences for connect/data/disconnect/turnover/gear-shift |
| 3b | two-instance loopback | two `ardop_link`s through `ardop_mod`+`ardop_demod` complete a full ARQ session in-process |

Step 2a is the recommended start: no clock, no FSM, and the byte-exact oracle
already exists. It is the smallest step that puts a `modem` module on the board
under the same enforcement as `codec`.

---

## Status

Design only — no `modem`/`link` code exists yet. The codec layer it builds on is
complete: `frame`, `crc`, `packed6`, `rs`, `stationid`, `locator` under
`core/codec`, each `-Werror`/`check-pure`-clean and proven equivalent to the
original. This note is the contract the next layer is written against.
