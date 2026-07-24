# 09 — Embedding: driving the core from a higher-level language

**Short answer: yes — and the sans-I/O core in [06](06-target-architecture.md) is
already the design that makes it work.** Bindability is not an extra requirement
to engineer in; it is what you get for free once the core stops owning the clock,
the devices, and the control flow.

This document identifies *where* to cut, what each cut costs, and what the C API
must avoid.

---

## This is not hypothetical — it already happens

Worth stating first, because it reframes the question. `ardopcf` is **already**
driven by higher-level applications:

- **Pat** (Go) drives it over the TCP host interface on port 8515/8516.
- **WoAD** (Android/Java) does the same over the network.

The host interface *is* a language binding — a line-oriented text protocol over
TCP, documented in `docs/Host_Interface_Commands.md` (~90 commands). It works,
and it is why ARDOP has any ecosystem at all.

So the real question is not "can this be done" but **"where should the seam be,
and is the current one in the right place?"** The current seam sits above
everything — the host program gets `CONNECT`/`DISCONNECT`/data and nothing else.
Anything more ambitious (a custom UI, a test harness that inspects
constellations, an experiment that alters gear-shifting) requires patching C.

---

## The one hard constraint: the sample path

Everything else is negotiable; this is not.

Audio runs at 12000 Hz in 240-sample buffers — a **20 ms** budget per period. Miss
it and ALSA underruns, which corrupts a transmission or drops received audio.

| Layer | Real-time budget | Safe in a managed language? |
|---|---|---|
| audio I/O + modem | 20 ms, hard | risky — no allocation, no GC pause, no GIL |
| link FSM (ARQ) | 250 ms turnaround, soft | **yes, comfortably** |
| host protocol | ~seconds | yes |
| WebGui / CLI / config | none | yes |

The ARQ turnaround budget is 250 ms (`ARQ.c:1127`) and repeat intervals are
1.5–2.1 s ([03](03-timing-model.md)). That is **four orders of magnitude** more
slack than the audio path. This is the fact that makes a high cut point viable:
the protocol is not real-time in any demanding sense, it just currently *lives*
next to code that is.

---

## Three candidate seams

### Cut A — above `app`: keep today's boundary, improve the protocol

```
┌─ C ──────────────────────────────────┐   ┌─ any language ─┐
│ platform · modem · codec · link · app │←→ │ Pat, WoAD, …   │
└──────────────────────────────────────┘   └────────────────┘
                                 TCP text protocol
```

What exists now. Keep it — Pat and WoAD depend on it and compatibility is
non-negotiable — but it does not answer the question, because the interesting
layers stay locked in C.

### Cut B — above `link`: the C engine is a modem, the app is upstairs ★

```
┌─ C engine ───────────────────────┐   ┌─ higher-level app ──────┐
│ platform · modem · codec · link  │←→ │ host protocol (Pat/WoAD)│
│                                  │   │ WebGui · CLI · config   │
└──────────────────────────────────┘   │ logging · scripting     │
        events ↑    ↓ commands         └─────────────────────────┘
```

The C side owns everything with a timing constraint: audio, PTT, modulation,
demodulation, and the ARQ state machine. The higher-level side owns everything
that is really *application* work — speaking the Pat/WoAD protocol, serving the
WebGui, parsing config, structured logging, and any scripting or automation.

- **Real-time risk:** none. The managed language never touches a sample.
- **Effort:** moderate — a modest event/command API, maybe 30 calls.
- **Payoff:** immediately removes the WebGui coupling from `SoundInput.c`,
  `ARQ.c` and `Modulate.c` (25 + 15 + 7 calls, [04](04-coupling-map.md)), and
  replaces the 1300-line `strcmp` ladder in `HostInterface.c:241` with whatever
  the host language does well.

**This is the recommended cut.** It is the smallest change that moves the
genuinely awkward code out of C, and it carries no real-time risk.

### Cut C — above `modem`: the FSM moves upstairs too

```
┌─ C engine ─────────────┐   ┌─ higher-level app ───────────┐
│ platform · modem · codec│←→│ link FSM · host protocol · UI │
└────────────────────────┘  └──────────────────────────────┘
     frame events ↑  ↓ send-frame commands
```

C becomes purely a modem: samples in → decoded frames out, frames in → samples
out. The ARQ protocol — the [02](02-protocol-fsm.md) table, gear-shifting,
session management — lives in the higher-level language.

Tempting, because the FSM is the most complex, most likely to change, and most
improved by an expressive language (real sum types, exhaustive matching,
property-based testing).

- **Real-time risk:** low but non-zero. The 250 ms turnaround now includes a
  round trip through the binding. At IPC latencies (microseconds) this is fine;
  the risk is a GC pause or a scheduler hiccup landing in that window. A 250 ms
  budget tolerates a lot, but it is no longer *structurally* impossible to miss.
- **Effort:** high. The FSM must be ported and revalidated on air.
- **Payoff:** large — the hardest-to-test component becomes testable in a
  language with good test tooling.

Reasonable as a **later** step, once Stage 3 of [07](07-migration-path.md) has
already extracted the FSM into a pure action-returning form. At that point moving
it across a boundary is nearly free, because it is already a pure function.

---

## Recommendation

**Cut B now; design the API so Cut C stays open.**

Concretely: put the seam above `link`, but make the `link` layer's dependency on
`modem` flow through the same explicit event/action types the binding uses. Then
relocating the FSM later is a matter of moving a file, not redesigning an
interface.

This falls out of [06](06-target-architecture.md) Rule 3 anyway — transitions
return actions rather than performing them. A layer that already communicates in
serializable data is one that can already be moved across a process or language
boundary.

---

## Mechanism: FFI or IPC?

Two ways to bind, and the choice matters less than the API shape.

| | C ABI + FFI | IPC (socket / pipe / shared memory) |
|---|---|---|
| Latency | ~ns | ~µs — still 1000× inside budget |
| Crash isolation | none — a segfault kills the app | full |
| Language reach | anything with an FFI | anything with a socket |
| Debugging | mixed-language stacks, awkward | trivial — inspect the wire |
| Deployment | one process | two, needs supervision |
| Hot reload of app layer | no | yes |

**IPC is underrated here.** At ARDOP's data rates (a few hundred bytes per
second) the overhead is irrelevant, and it gives crash isolation, trivially
inspectable traffic, and the ability to restart the app layer without dropping
the radio. It is also *exactly what the existing host interface does*, so the
operational model is already proven for this project.

**FFI is better if** the app layer needs to inspect DSP internals at rate — a
test harness pulling constellation points or spectra per symbol, for example.

Suggested: **expose a C ABI, and ship an IPC daemon built on top of it.** FFI
consumers who want tight coupling get it; everyone else gets the safer path. The
daemon is then just another consumer of the same API, which keeps it honest.

---

## What the C API must avoid to be bindable

Every one of these is already required by [06](06-target-architecture.md)'s
rules — which is the point. Listing them as binding requirements shows how much
overlap there is.

| Requirement | Why FFI needs it | Already required by |
|---|---|---|
| **Opaque handles, no globals** | Two instances in one process; no hidden state a binding can't see | Rule 1 |
| **Never block** | A blocked call freezes the host's event loop / holds the GIL | Rule 3 |
| **Clock passed in** | Host may drive from a file, a test, or a simulation | Rule 2 |
| **Caller-allocated buffers with explicit capacity** | Managed languages own their memory; no C-side allocator to free from | — |
| **Plain-data structs, no function pointers in hot paths** | Callbacks into managed code are the hardest part of any FFI | Rule 3 |
| **Return codes, never `exit()` / `abort()` / `longjmp`** | Must not kill the host process | new |
| **No varargs in the API** | Poorly supported by most FFIs | new |
| **Stable, documented struct layout** | Bindings hard-code offsets, or need accessors | new |
| **Fixed-width types only** | `BOOL`/`UCHAR`/`HANDLE` do not map cleanly | [08](08-style-and-tooling.md) |
| **Documented thread-safety** | Even "single-threaded, one handle per thread" must be stated | new |
| **No output to stdout/stderr** | Host owns its own logging; route through a sink | new |

Only four are genuinely new; the rest the architecture demands regardless.

On process termination specifically, the news is better than expected: all 8
`exit()`/`abort()` calls in `src/` are in `ARDOPCommon.c`, and all of them are in
`processargs()` — command-line handling, which belongs in the app layer anyway.
The core does not terminate the process. So this requirement is satisfied mostly
by *moving* argument parsing upstairs rather than by auditing and rewriting error
paths throughout.

---

## What this buys beyond flexibility

Three things that matter to the goals in this review, not just to elegance.

**1. Testing gets dramatically better.** A modem callable from Python means DSP
work gets numpy, scipy and matplotlib. Instead of squinting at the WebGui
waterfall, you can plot constellations per symbol, sweep SNR and produce
BER curves per data mode, and diff two demodulator implementations sample by
sample. For a project whose stated goal is improving over-the-air performance,
this is probably the largest practical win in this entire review — bigger than
any refactor, because it changes what you can *see*.

**2. It de-risks the language question.** [06](06-target-architecture.md)
recommends restructuring in C and reassessing Rust later. A stable C ABI makes
that reassessment cheap and incremental: port one layer, keep the ABI, run the
golden vectors. The app layer never notices.

**3. The WebGui stops being a coupling problem.** Today it is 77 calls in
`Webgui.c` plus 54 scattered through DSP and protocol code, and every unit test
links a websocket server and generated HTML blobs. Upstairs, it is just another
consumer of the event stream.

---

## Sketch

Building on the [06](06-target-architecture.md) interfaces — Cut B, event/command
shaped so it works over either FFI or a socket:

```c
/* ---- lifecycle ---- */
ardop_engine *ardop_engine_new(const ardop_config *cfg, ardop_status *err);
void          ardop_engine_free(ardop_engine *e);

/* ---- pump: called by the C shell, or by the host if it owns the loop ---- */
int  ardop_engine_tick(ardop_engine *e);

/* ---- events out: drain a queue, never blocks ---- */
size_t ardop_engine_poll(ardop_engine *e, ardop_event *out, size_t max);

/* ---- commands in: mirrors the host protocol, but typed ---- */
ardop_status ardop_engine_connect(ardop_engine *e, const char *target,
                                  ardop_bandwidth bw);
ardop_status ardop_engine_send(ardop_engine *e, const uint8_t *data, size_t len);
ardop_status ardop_engine_disconnect(ardop_engine *e);

/* ---- logging: host-owned sink, no stdio ---- */
void ardop_engine_set_log_sink(ardop_engine *e,
                               void (*sink)(void *ctx, int level, const char *msg),
                               void *ctx);
```

`ardop_event` is a tagged union covering state changes, decoded data, quality and
SNR reports, PTT transitions, and — for the DSP-inspection use above —
constellation and spectrum frames, which is precisely what `wg_send_*` pushes
today.

Every field is plain data. Serialize it and you have the IPC protocol; expose it
directly and you have the FFI. Same API either way.
