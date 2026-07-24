# 06 — Target architecture

What to build toward, and why. The compatibility target is unchanged: bit-exact
on air, and the existing TCP host interface.

---

## Three rules

Everything below follows from these. They are stated first because they are the
whole design; the rest is consequence.

### Rule 1 — The core is sans-I/O

The modem and protocol are a **pure function of their inputs**. They open no
device, read no clock, bind no socket, draw nothing, and never block. They
consume samples and events, and produce samples and actions.

I/O lives in a shell that is deliberately dull, and therefore the only part that
needs a real radio to test.

### Rule 2 — One clock, and it is the sample counter

Time in the core is `uint64_t` **samples elapsed**, threaded through explicitly.
Not a global. Not `CLOCK_MONOTONIC`. Not milliseconds.

This single change dissolves most of [03](03-timing-model.md):

- `SoundFlush()`'s wall-clock/sample-count comparison stops existing — TX
  completion *is* a sample count.
- `FixTiming` becomes unnecessary: if the card runs at 11997 Hz, protocol
  deadlines stretch by the same 0.025%, exactly as they should. Rate error stays
  a DSP concern (which the demodulator already handles via `dblOffsetHz`) instead
  of becoming a protocol correctness concern.
- Tests get a deterministic clock for free — which `--decodewav` already proves
  works, for the receive half.

Wall-clock survives in exactly two places: log timestamps, and the regulatory
10-minute ID (a legal obligation measured in real time, not audio time).

### Rule 3 — Transitions return actions; they do not perform them

`ProcessRcvdARQFrame()` currently calls `Mod4FSKDataAndPlay()` and blocks until
the audio has played. Instead, a transition **returns** "send frame X" and the
shell decides when. That is what makes the FSM testable without a sound card,
and it is what removes `txSleep()` and with it the TX-inside-TX reentrancy.

---

## Layers

```
┌──────────────────────────────────────────────────────┐
│ platform   ALSA/WASAPI, PTT, serial, sockets, files  │  ← impure, thin, per-OS
├──────────────────────────────────────────────────────┤
│ app        host protocol, WebGui, CLI, logging       │  ← impure, portable
├──────────────────────────────────────────────────────┤
│ link       ARQ + FEC + RXO state machines            │  ┐
├──────────────────────────────────────────────────────┤  │ pure core
│ modem      modulate / demodulate / sync / busy       │  │ no I/O, no clock
├──────────────────────────────────────────────────────┤  │ no globals
│ codec      frame tables, RS, CRC, Packed6, StationId │  ┘
└──────────────────────────────────────────────────────┘
```

Dependencies point **down only**. Today they point in every direction — the
demodulator calls the protocol, the protocol calls the modulator, the platform
owns the main loop, and the GUI is called from all of them.

| Layer | Contains | Testable by |
|---|---|---|
| `codec` | frame type tables, `FrameInfo` as **data**, RS, CRC, Packed6, StationId, Locator | pure unit tests, property tests |
| `modem` | modulator, demodulator, leader search, sync, tracking, busy detect, Memory ARQ | WAV in / WAV out, golden vectors |
| `link` | the [02](02-protocol-fsm.md) FSM, gear-shifting, session state | scripted event sequences; two instances talking to each other |
| `app` | host command parse/format, WebGui, CLI | text in / text out |
| `platform` | devices, PTT, sockets, signals, clock | needs hardware — kept as small as possible |

---

## Core interfaces

Sketch, to make the shape concrete rather than to fix an API.

```c
/* ---- codec: pure, no state ---- */
const ardop_frame_spec *ardop_frame_spec_for(uint8_t frame_type);
int  ardop_frame_encode(uint8_t frame_type, const uint8_t *payload, size_t len,
                        uint8_t session_id, uint8_t *out, size_t out_cap);
int  ardop_frame_decode(const ardop_symbols *syms, uint8_t session_id,
                        ardop_frame *out);

/* ---- modem: explicit state, no I/O, no clock ---- */
typedef struct ardop_demod ardop_demod;
ardop_demod *ardop_demod_new(const ardop_modem_cfg *cfg);

/* Push samples; drain zero or more events. Never blocks, never transmits. */
size_t ardop_demod_push(ardop_demod *d,
                        const int16_t *samples, size_t n,
                        uint64_t t_samples,          /* the only clock */
                        ardop_event *events, size_t max_events);

typedef struct ardop_mod ardop_mod;
void   ardop_mod_begin(ardop_mod *m, uint8_t frame_type,
                       const uint8_t *encoded, size_t len, int leader_ms);
/* Pull samples on demand. Returns 0 when the frame is complete. */
size_t ardop_mod_pull(ardop_mod *m, int16_t *out, size_t max);

/* ---- link: pure FSM ---- */
typedef struct ardop_link ardop_link;
/* Returns an action for the caller to perform. Does not transmit. */
ardop_action ardop_link_step(ardop_link *l,
                             const ardop_event *ev,
                             uint64_t t_samples);
```

`ardop_action` is data, not a call:

```c
typedef enum {
    ARDOP_ACT_NONE,
    ARDOP_ACT_SEND_FRAME,     /* frame_type + payload  */
    ARDOP_ACT_SET_TIMER,      /* deadline in samples   */
    ARDOP_ACT_NOTIFY_HOST,    /* host protocol message */
    ARDOP_ACT_DELIVER_DATA,   /* decoded payload → host */
    ARDOP_ACT_SET_PTT,
} ardop_action_kind;
```

The main loop becomes something a person can read in one sitting:

```c
while (running) {
    n = platform_read_audio(buf, N);          /* the only clock source */
    t += n;
    nev = ardop_demod_push(demod, buf, n, t, events, MAX_EV);
    for (i = 0; i < nev; i++)
        perform(ardop_link_step(link, &events[i], t));
    perform(ardop_link_step(link, NULL, t));  /* timers */
    if (transmitting)
        platform_write_audio(txbuf, ardop_mod_pull(mod, txbuf, N));
}
```

Note what is absent: no `txSleep`, no blocking transmit, no reentrancy, no
`ProtocolState` consulted from inside the demodulator, and one clock.

---

## What this buys, mapped to the two symptoms

**Hardware timing.** Rule 2 removes the wall-clock/sample-count conflation
entirely — the failure mode is gone rather than detected. `FixTiming`, `-A` and
`SlowCPU` all stop being needed. TX completion is observed (samples drained)
rather than predicted.

**Testability.** Each row of the layer table is testable without a radio.
Concretely, the things that are impossible today become routine:

- decode a WAV and assert the exact frame sequence — *already possible*, via
  `--decodewav`; generalise it
- drive the ARQ FSM through a full connect/data/disconnect with no audio at all
- run two `ardop_link` instances against each other in-process, testing turnover,
  gear-shifting and timeouts deterministically
- property-test `FrameInfo` as a table
- fuzz `ardop_frame_decode` on arbitrary bytes
- assert that a modulated frame is bit-identical to a stored golden vector

---

## Language

The evaluation you asked for, since this was left open.

### Criteria

1. **Risk to the DSP port.** ~7000 lines of demodulator/modulator must keep
   decoding real signals. This is the dominant risk in any rebuild.
2. **The observed bug classes.** From [04](04-coupling-map.md): cross-TU type
   mismatch, buffer indexing errors, ambient mutation, reentrancy, platform type
   confusion. Which of these does the language prevent?
3. **Build simplicity.** The target users are amateur operators on Raspberry Pi
   Zeros. `CONTRIBUTING.md` makes "no additional build tools" an explicit goal.
4. **Upstream mergeability.** Whether work can flow back to `pflarue/ardop`.

### Assessment

| | C | Rust |
|---|---|---|
| DSP port risk | **low** — transliteration, diff against golden vectors | high — genuine rewrite; no line-by-line step |
| Type mismatch across TUs | not prevented | prevented |
| Buffer indexing | not prevented | prevented |
| Ambient mutation | not prevented (but architecture fixes it) | prevented by construction |
| Reentrancy / TX-inside-TX | not prevented | prevented (`&mut` aliasing) |
| Platform type confusion | not prevented | prevented |
| Build simplicity | `apt install build-essential` | needs rustup; fine on Pi Zero 2, awkward on ARMv6 Pi Zero 1 |
| Upstream mergeable | yes | no |

Rust wins decisively on criterion 2 — it eliminates, at compile time, essentially
every defect class this review found. C wins decisively on criteria 1, 3 and 4.

### Recommendation: restructure in C first, decide on Rust afterwards

Not a fence-sit — a sequencing argument. Two claims:

**1. The valuable change is architectural, not linguistic.** Every problem in
this review — the two clocks, the blocking transmit, the demodulator that
transmits, the 350 globals, the untestable core — is fixable in C. None of them
requires a new language. Rust would *also* prevent them recurring, but it does
not do the work of separating the concerns; you would still have to do that,
only while simultaneously rewriting the DSP.

**2. Doing both at once compounds the two riskiest changes.** If a rebuilt
demodulator fails to decode a marginal signal, you want to know whether that is
the restructure or the port. Doing them in sequence keeps that question
answerable.

The sequencing also has a compounding benefit: after the restructure, the core is
pure, has explicit state, and is covered by golden vectors. That is *precisely*
the input a language port wants — a port then becomes mechanical and verifiable
rather than exploratory. Doing Rust second is much cheaper than doing it first.

**What would change this recommendation:**

- If the goal is a genuinely independent project with no interest in upstream, and
  you are willing to accept a longer path to a working radio, going straight to
  Rust is defensible — the end state is better.
- If **concurrency** is on the roadmap (parallel demodulation of multiple
  carriers, or separating audio I/O onto its own thread), go to Rust sooner. This
  codebase is single-threaded and its global state makes threading unsafe; Rust
  makes that transition tractable and C does not.
- If the DSP is going to be substantially rewritten anyway rather than ported,
  criterion 1 disappears and Rust wins outright.

### If C, then modern C

C11, opaque handles instead of shared state, `static` by default, `const`
correctness, fixed-width types from `<stdint.h>`, one minimal header per module.

The style and the tooling that enforces it are a substantial topic in their own
right — see **[08 — Style and tooling](08-style-and-tooling.md)**, which proposes
a deliberate style (not an inherited one), measures the current 177-warning debt
under `-Wall -Wextra`, and shows that `-flto` alone surfaces four cross-TU type
mismatches that normal compilation cannot diagnose.

---

## Embedding

A sans-I/O core with explicit state, no clock of its own and no blocking is —
without further effort — a core that can be driven from another language. That
question is developed in **[09 — Embedding and bindings](09-embedding-and-bindings.md)**:
where to cut, what it costs, and why the biggest payoff is being able to drive
the modem from Python for DSP work.

## What stays

Worth restating from [05](05-essential-vs-incidental.md): `StationId`, `Locator`,
`Packed6`, `log`, `wav`, `sdft`, `FFT`, `noise` and vendored `rrs.c` carry across
largely as-is. They already satisfy the rules above. The rebuild is not
starting from zero — roughly 2500 lines of it already exist and are already
tested.
