# 14 — The station application: scope and architecture

[13](13-completing-the-rebuild.md) finished the rebuild: `core/` became the
running program, `src/` is gone, and `ardopb` speaks the frozen host protocol.
That produced a *daemon*. This document designs the *application* — one
installable program, on five platforms, that an operator who does not own a
terminal can use.

It is the umbrella for three companion documents:

| | Document | Owns |
|---|---|---|
| 15 | [Platform audio, devices and PTT](15-platform-audio-and-ptt.md) | The backends, device enumeration, PTT, Android |
| 16 | [The user interface](16-user-interface.md) | Toolkit choice, panels, screens, responsive layout |
| 17 | [The application protocol](17-application-protocol.md) | Chat and file transfer, as a wire spec |

This one owns the seam, the concurrency model, the TNC multiplexer, and the
sequencing.

---

## What is being built

A single program that:

- **embeds the modem** — it is `ardopb`'s shell, not a client of it;
- **selects audio devices from the UI**, with no command line;
- **shows the instrument panel** already built in [`gui/`](../gui/);
- **hosts the TCP TNC interface** on 8515/8516, so Pat and WoAD attach to it
  exactly as they attach to `ardopcf` today;
- **transfers files and carries chat** over a new application protocol (17).

Targets: Linux, Windows, macOS, Android. **iOS is deferred but must not be
architecturally precluded** — no decision here may depend on a listening socket
being reachable, on a writable filesystem path outside the sandbox, on serial
port access, or on the process staying alive when backgrounded.

### What it is not

- Not a replacement for `ardopb`. The headless daemon keeps existing and keeps
  being the thing you run on a Pi in the shack. The app is a second consumer of
  the same core, not a successor.
- Not a change to anything on the air. `make golden-tx` is bit-exact today and
  stays bit-exact; nothing in this document or its companions touches the
  modulator, the frame tables, or the link FSM.
- Not a rewrite of `gui/`'s algorithms. See 16 §2.

---

## The shape today

Four processes and three transports, all of which the app collapses:

```
  ardopb ──8515 cmd──┐
    │    ──8516 data─┼── ardop-cat / ardop-chat             (apps/, 216 ln client)
    │    ──8517 tlm──┴── ardop-gui                          (gui/, read-only)
    └─ ALSA (Linux only), serial-RTS PTT (Linux only)
```

The pieces that survive into the app, unchanged:

| Layer | Status |
|---|---|
| `core/*` | Portable C11. No POSIX includes anywhere. Links with `-lm`. |
| `shell/runtime.c`, `loop.c`, `host.c`, `telemetry.c` | Portable C11. No feature-test macros. This is exactly `SHELL_OBJS` in the Makefile. |
| `gui/`'s drawing algorithms | Toolkit-shaped, but the maths carries (16 §2). |

The pieces that do not:

| File | Why |
|---|---|
| `shell/backend_alsa.c` | ALSA, `alloca`, `termios`, `TIOCMSET` — Linux only |
| `shell/backend_null.c` | `usleep` |
| `shell/host_tcp.c` | BSD sockets, `fcntl`, int fds, no `SIGPIPE` handling |
| `shell/telemetry_tcp.c` | same |
| `shell/main.c` | An argv parser and a `while` loop; the app writes its own |
| `apps/*` | CLI tools over TCP; unaffected, and stay |

**That is the whole portability bill.** Four device/transport files and a
`main`. It is small because [06](06-target-architecture.md) Rule 1 was enforced
mechanically rather than by intention — `make check-standalone` fails if a core
object ever grows a dependency on a socket.

---

## Decision 1 — The seam: link the portable set, supply our own edges

The app links `core/*` plus `shell/{runtime,loop,host,telemetry}.c` and provides
its own `ardop_platform_ops`, its own transports, and its own `main`. It is
[09](09-embedding-and-bindings.md)'s **Cut B**, with the app tier written in
whatever 16 chooses rather than in C.

```
┌─ app (16's toolkit) ────────────────────────────────────┐
│  UI · settings · chat/file protocol (17) · TNC server   │
├─ C, portable ───────────────────────────────────────────┤
│  shell/runtime · shell/loop · shell/host · shell/telemetry│
│  core/link · core/modem · core/codec                     │
├─ C, per-platform (15) ──────────────────────────────────┤
│  audio backend · PTT · sockets                           │
└─────────────────────────────────────────────────────────┘
```

**Rejected — keep `ardopb` as a child process and talk TCP** (what `gui/` does
today). It is genuinely tempting: it is already built, it gives crash isolation,
and [09 §IPC](09-embedding-and-bindings.md) argues for it. It fails on the
requirement that made this an application in the first place — *audio device
selection from the UI*. A separate daemon owns the sound card, so every device
change is a process restart with argv rewritten, and on Android there is no
second process to spawn. It also doubles the install.

**Rejected — link `core/` only and rewrite `shell/runtime.c`.** That file is 514
lines of demod/modulate/RS/PTT orchestration with `test_runtime.c` and
`test_loop.c` behind it. Reimplementing it in another language buys nothing and
loses the tests.

**Consequence to accept:** a crash in the DSP takes the UI down with it. This is
the real cost of Cut B and it is worth stating plainly. The mitigations are the
ones already in place — `-Werror`, no allocation in core, `check-pure`, the
golden corpus — plus keeping `ardopb` as the deployment for anyone who wants
isolation.

**Not precluded:** the app attaching to a *remote* `ardopb` instead of its
embedded modem. `gui/`'s `TelemetryClient` is already a clean seam — five
signals over three transport-free structs — so a second data source drops in
behind the same interface. It is simply not in scope now.

---

## Decision 2 — One modem thread, and two queues that cross it

`ardop_runtime` has no locks and no thread-safety documentation. `ardop_loop_step()`
blocks for ~100 ms inside the capture device. Therefore:

> **One runtime, one thread. Nothing outside the modem thread touches
> `ardop_runtime` or anything reachable from it, for any reason, ever.**

That includes configuration. Today `main.c` sets `rt.link.bw_setting` and
`rt.link.listening` by direct struct write (`shell/main.c:191-192`) before the
loop starts. From a UI those writes arrive at arbitrary times on the UI thread,
which is a data race against the audio path.

### The loop the app runs

Not `ardop_loop_run()`. `main.c:256-263` already demonstrates the pattern —
hand-roll the `while` so you can interleave your own servicing:

```c
while (running) {
        app_drain_commands(app, rt, lp.t);   /* UI + guests + config */
        ardop_host_tcp_service(app->host, rt, lp.t);
        ardop_telemetry_tcp_service(app->tlm, rt);
        ardop_loop_step(&lp);
}
```

**`poll_host` stays NULL.** It is the documented seam for host input
(`shell/platform.h:81-87`) and the obvious candidate, but it is typed to emit an
`ardop_host_cmd`, and `ardop_host_cmd` covers only the eight *transition-driving*
commands (`core/link/link.h:70-79`). It cannot carry `MYCALL`, `ARQBW`,
`LISTEN`, `BUSYDET`, `FECMODE`, `FECREPEATS`, or "restart the audio device" —
which is most of what a settings screen produces. Rather than widen a core-facing
type to carry app concerns, the app drains its own richer queue in its own loop.
`platform.h` is left alone.

### Downward: one MPSC command queue

A tagged union, drained on the modem thread:

| Variant | Applied by |
|---|---|
| `ardop_host_cmd` | `ardop_runtime_host(rt, &cmd, now)` |
| config set (callsign, grid, bandwidth, listen, busydet, FEC mode/repeats) | direct write to `rt->link.*` / `rt->busy_det`, **on the modem thread** |
| raw host line (debug console, and guests — see Decision 4) | `ardop_host_command(rt, line, now, reply, cap)` |
| control (start/stop/restart backend) | the app's own state machine |

Note `ardop_host_cmd.data` is `const uint8_t *` and is copied in by the link, but
must stay alive for the duration of the call — so a `SEND_DATA` enqueued from the
UI carries its payload inline in the queue slot, not by pointer.

### Upward: two queues, because the two channels have opposite failure modes

Both the observer callback and the telemetry sink fire **on the audio path**, are
documented "must not block or reenter" (`shell/runtime.h:85`,
`shell/telemetry.h:136`), and hand out pointers that are borrowed and valid only
for the call. Everything crossing to the UI must be deep-copied at the callback.

**Display data — lossy, drop-oldest.** Spectrum rows, constellation snapshots,
audio levels, status mirrors. A stale waterfall row is worse than a missing one.
This is exactly the design `shell/telemetry_tcp.c` already implements over a
256 kB ring, minus the socket; records are self-delimiting so the oldest can be
dropped without a side index.

**Events — lossless.** `ARDOP_OBS_RX_DATA` carries *payload*. Dropping it
silently corrupts a file transfer, and 17's integrity checks would then fire on
a fault the modem never had. `ARDOP_OBS_HOST_MSG` carries `CONNECTED`/`DISCONNECTED`,
which the session state machine cannot miss.

The lossless queue cannot block (audio path) and cannot allocate (audio path), so
it must be sized for the worst case and treat overflow as a hard fault. The
headroom is enormous — the link delivers a few hundred bytes per second while the
UI drains at frame rate — so a queue sized for a few seconds of full-rate
delivery is already unreachable in practice. If it is ever reached, abort the
session and surface it; do not lose bytes quietly.

### Exactly one observer

`ARDOP_MAX_OBSERVERS` is 4 (`shell/runtime.h:88`), and the app plausibly wants
four consumers: UI, TNC server, logger, and 17's protocol layer. **Do not raise
the constant.** Register one app-owned observer that copies into the event queue;
fan out on the far side, where a slow consumer costs nothing. Observers run on
the audio path — the right number of callbacks to make there is the smallest one.

---

## Decision 3 — The UI drives the typed API, not the text protocol

`shell/host.c` is socket-free by construction, so `ardop_host_command(rt, "ARQBW 500MAX", …)`
works in-process with no transport. It is still the wrong path for the app's own
UI: the text protocol flattens structured state into strings and back, and the
`ardop_obs` bus carries typed state, quality and S/N that the wire format
discards. Round-tripping through `snprintf` and `atoi` is pure loss.

The text path is retained for exactly two things: **guests** (Decision 4) and a
**debug console pane** that lets an operator type raw TNC commands — which is
genuinely useful for diagnosing a Pat interaction, and costs nothing since the
parser is already linked.

---

## Decision 4 — Hosting the TNC interface: the app owns the modem, clients are guests

`shell/host_tcp.h:21-22` says what it is: *"Non-blocking and single-client per
port — enough to drive and observe the assembled program, not the production
multiplexer."* Hosting it inside an application that is *itself* driving the same
modem makes that understatement load-bearing.

### The hard constraint

There is one link, one session, and **one 16 KB transmit queue**
(`rt->tx_queue`, `shell/runtime.h:107`). If the app's file transfer and a guest's
Pat session both append to it, the two byte streams interleave and both are
destroyed. This is not a policy question; it is a correctness one.

### The model: single session owner

- The app is the **owner** of the modem. It owns configuration, the audio device,
  and the lifecycle.
- At most one **session owner** exists at a time: either the app's own protocol
  layer (17) or one attached guest.
- Ownership is claimed by the first session-affecting command — `ARQCALL`,
  `FECSEND`, `LISTEN TRUE` — and released on `DISCONNECT`, on link drop, or when
  the claiming client disconnects.
- A session-affecting command from a non-owner is refused with a `FAULT` line
  rather than silently interleaved. The UI shows who holds the link.

### Guest configuration commands are accepted, not refused

Pat sets `MYCALL` during its startup handshake. Refusing it to protect the app's
settings would break the primary client we are trying to support. So: a guest's
config commands are **applied and surfaced** — the UI shows the value changed and
which client changed it. Interoperability wins; the operator gets visibility
instead of protection.

### Three defects in the transport to fix while porting it

1. **A second client is never told it lost.** `accept` is only called when the
   slot is free, so a second Pat sits connected-but-unserved forever. Accept and
   close with a reason instead.
2. **No `SIGPIPE` handling anywhere in the tree**, and `shell/host_tcp.c:123,214,225`
   use bare `write()`. A guest disconnecting mid-write kills the process — which
   in `ardopb` loses a session and in the app takes the whole UI down.
   `MSG_NOSIGNAL` / `SO_NOSIGPIPE` / Winsock's absence of the problem.
3. **Short writes are discarded.** `(void)!write(...)` on a non-blocking socket
   silently truncates a long reply. Loop, or buffer.

### iOS

A listening socket on iOS is awkward and a background one is not guaranteed. The
TNC server is therefore a **per-platform capability**, present on desktop and
Android, absent on iOS — and nothing else in the app may depend on it. This is
the concrete reason the UI drives the typed API (Decision 3): on a platform with
no TNC server, everything still works.

---

## Decision 5 — Runtime lifetime

`ardop_runtime` is ~317 kB and is `static` today (`shell/main.c:181`) because it
is far too large for a stack frame. In the app:

- **Heap-allocate it.** A GUI process may want a second one later (a monitor
  receiver, a test harness), Android's main thread stack will not take it, and
  `static` makes stop/restart clumsy.
- **The runtime outlives the audio backend.** Changing devices from the UI tears
  down and rebuilds the backend; the runtime, its configuration, and the sample
  clock `lp.t` continue. Time is a monotonic sample count and nothing requires it
  to correspond to a single device.
- **A device change requires `DISC`.** An in-flight ARQ session cannot survive
  the capture stream restarting. Either refuse the change or abort the session
  first — refuse, and say why.

---

## Decision 6 — Build system

**CMake builds the app. The root `Makefile` remains the authority for the
mechanical guarantees.**

`make check-pure` and `check-standalone` are `objdump`/`nm`-based and Linux-hosted.
They prove properties of *source that the app shares*, so running them once on
Linux CI proves them for every platform's build. Porting them to CMake would buy
nothing and risk losing them — and [`core/README.md`](../core/README.md) is
explicit that a rule which cannot be mechanically enforced is only guidance.

Required CI shape:

| Job | Proves |
|---|---|
| Linux `make test-core golden-* check-*` | The existing guarantees, unchanged |
| Linux/Windows/macOS/Android CMake build of the app | The portable set really is portable |

One known blocker: `gui/CMakeLists.txt:55-56` passes `-std=c11;-Wall;-Wextra;-Wconversion`
as literal strings, which MSVC rejects. Needs a compiler-ID guard or a generator
expression. `core/common/mustuse.h` already degrades to a no-op on compilers
without `warn_unused_result`, so it is fine.

---

## What this changes in `shell/`

| Change | Size |
|---|---|
| `host_tcp.c` → portable sockets (Winsock init, `SOCKET`, `closesocket`), `SIGPIPE`, short writes, second-client refusal | M |
| `telemetry_tcp.c` → same socket abstraction | S |
| `backend_null.c` → portable sleep | S |
| New backends + PTT abstraction + enumeration | L — see [15](15-platform-audio-and-ptt.md) |
| `loop.h:45-48` doc drift: it says the caller wires `rt->on_ptt`, and no such field exists — PTT is routed by the observer (`shell/main.c:59-64`) | trivial |
| Consider making `ARDOP_LOOP_BLOCK` (fixed 1200 samples / 100 ms, `shell/loop.h:30`) settable per backend | S — see 15 §7 |

Nothing in `core/` changes. Nothing in `apps/` changes.

---

## Prerequisites still open from 13

Stated plainly, because the app inherits them:

1. **There is no Windows audio backend at all.** 13's W2.2 shipped ALSA and a
   null device. Document 15 is that work; it is on the critical path, not
   adjacent to it.
2. **Memory-ARQ is not ported** (13 W1.5). Low-SNR performance is therefore below
   the reference implementation's, and an application aimed at ordinary operators
   will be judged on exactly that.
3. **W3's on-air interop validation against a real `ardopcf` peer has not been
   done.** The golden corpus proves the waveform and the decode; it does not
   prove a live negotiated session with a foreign implementation.

**Recommendation: do not ship this app to non-technical operators before (3).**
An app is a promise that it works; a daemon is a tool that assumes you can debug
it. The credibility cost of the first live failure is not recoverable by a later
fix.

---

## Workstreams

- **A — The embedding spine.** Heap runtime, the app loop, the command queue,
  the two upward queues, the single observer, the app's own `main`. Depends on
  nothing. Provable headless, with a fake backend, before any UI exists.
- **B — Platform layer.** [15](15-platform-audio-and-ptt.md): backends, device
  enumeration, PTT, Android. Independent of A; they meet at `ardop_platform_ops`.
- **C — The UI.** [16](16-user-interface.md). Depends on A for its data source
  and on B for the device picker's content.
- **D — TNC hosting.** Portable socket layer, the ownership arbiter, the three
  transport defects. Depends on A.
- **E — The application protocol.** [17](17-application-protocol.md). Depends on
  A; independent of B, C, D. Specifiable now, testable against the in-process
  loopback harness (`test/core/test_loopback.c`) with no radio and no UI.

```
A (spine) ─┬─▶ C (UI) ────┐
           ├─▶ D (TNC)    ├─▶ app
           └─▶ E (proto)  │
B (platform) ─────────────┘
```

Recommended start: **A**, headless, with the null backend and a scripted command
queue — it is the piece every other workstream depends on and the only one whose
failure mode is subtle (a data race that shows up as a corrupted frame once an
hour). Then **B** for one non-Linux platform, because it is the largest unknown
and the only one that can invalidate Decision 1.

### Effort

| | Workstream | Size |
|---|---|---|
| A | Embedding spine: loop, queues, lifetime | M |
| B | Platform audio + enumeration + PTT (15) | L |
| C | UI (16) | L |
| D | Portable TNC hosting + arbitration | M |
| E | Application protocol (17) | M |

---

## Open decisions

1. **The program's name and where it lives in the tree.** `app/` alongside
   `gui/`, or does `gui/` become it? If 16 keeps Qt, they are the same project
   and `gui/` grows; if 16 chooses otherwise, `gui/` stays as the standalone
   remote panel and the app is new.
2. **Whether the standalone panel survives.** Attaching a read-only display to a
   *remote* station's telemetry is a real capability (`gui/README.md`) that the
   embedded app does not replace. Keeping it costs a build target.
3. **Lossless-queue overflow policy.** Abort the session (proposed) versus
   applying backpressure by refusing new host data earlier. The second is better
   behaviour and needs a path from the queue's depth back to the TX-queue
   admission check.
4. **Whether guests may hold `LISTEN` while the app is idle.** Answering an
   incoming Pat connection while the app shows a chat window is a plausible and
   useful configuration, and it stretches "one session owner".
5. **Config persistence format and location** per platform, including what
   happens on Android when the app is uninstalled.

## Exit criteria

- The app runs headless on the null backend, driven by a scripted command queue,
  and completes a connect → data → disconnect ARQ session with both upward queues
  under load. No `ardop_runtime` field is touched off the modem thread —
  demonstrated under ThreadSanitizer.
- `make check-pure`, `check-headers`, `check-standalone`, `test-core`,
  `golden-core`, `golden-shell` and `golden-tx` all still pass, unmodified.
- Pat completes a full session against the app's hosted TNC ports, and the UI
  correctly shows the guest as the session owner throughout.
- A second guest connecting is refused with a message rather than hanging.
- The app builds in CI on Linux, Windows, macOS and Android.

---

## Amendments made during implementation

1. **Decision 4's transport defects were already fixed; the missing half was
   visibility.** All three -- the unserved second client, the bare `write()` with
   no `SIGPIPE` handling, the discarded short write -- had been dealt with by the
   time workstream D started: `shell/host_tcp.c` has outbound queues, an explicit
   refusal, and `MSG_NOSIGNAL`.

   What had not been done is the part Decision 4 spends most of its words on. A
   guest's configuration commands are applied rather than refused, and the
   decision promises that "the UI shows the value changed and which client
   changed it" -- but guest commands went straight to `ardop_host_command` and the
   transport announced itself through six `fprintf(stderr)` calls. Correct for a
   daemon whose operator is reading a terminal; useless inside a window, where the
   events that most need seeing went to a stream nobody was watching.

   `shell/host_tcp.c` now has an observer, and `ardop_net_accept` returns the peer
   address, because a screen listing attached clients has to say *which* machine
   is holding the transmitter.

2. **Following a guest's changes turned out to be the other half of the promise.**
   Recording that Pat set `MYCALL` in an activity log, while the Station screen
   goes on displaying the previous callsign, keeps the letter of Decision 4 and
   not its point: a settings screen showing a value that is not the modem's is
   worse than one showing nothing. The Station screen now follows the canonical
   `KEY now VALUE` replies.

   **Displayed, not saved.** The running modem takes the guest's value; the
   settings file keeps the operator's, so a restart returns to what the operator
   chose. A guest borrows this station, it does not reconfigure it permanently.
   That distinction is not in Decision 4 and probably should have been.

3. **Two documented host commands were missing, and one of them was the first
   thing a client says.** `docs/Host_Interface_Commands.md` calls `INITIALIZE`
   "the first command that needs to be issued to the modem" and opens all three
   of its example handshakes with it; `ardopb` answered `FAULT CMD INITIALIZE not
   recoginized`. Now accepted as the no-op it is -- the runtime is initialised
   long before any socket exists.

   `BUSYDET` was reachable from this application's own settings and not from a
   guest's, because `app/spine.c` carried a private direct write with a note
   saying to delete it if the command ever reached `shell/host.c`. It has, and
   that note has been acted on: one validator, one path, and a client can set it
   as ARIM expects.

4. **`ARQTIMEOUT` is still refused, deliberately.** It is documented, it appears
   in the reference handshake, and `core/link` has **no idle-disconnect
   mechanism at all** -- so accepting it would mean storing a number that changes
   nothing, and a station that reports `ARQTIMEOUT now 30` while never timing out
   is worse than one that admits it does not know the command.

   Implementing it honestly means an idle timer, and the right place is
   `shell/runtime.c` rather than `core/` -- the runtime already drives the link
   and knows the elapsed sample count, so no protocol code has to change. That is
   its own piece of work and is **not** part of D.

5. **The application hosts the TNC on the modem thread, with no request ring.**
   `app/devices.c` needs one because a device rebuild is slow and has states to
   sequence. Opening a listener is neither, so one atomic carries the whole
   intent and the modem thread acts on it -- which keeps `app_set_tnc` on the
   thread its header says owns it.

   **It does not listen until asked.** The listener binds `INADDR_ANY`, so
   turning it on makes the station reachable from the whole network; that is a
   decision an operator makes on purpose. `ardopb` sets the same precedent by
   requiring an explicit `--host PORT`.
