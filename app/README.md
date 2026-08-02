# `app/` — the station application's embedding spine

The modem, embedded rather than talked to.

[`analysis/14`](../analysis/14-station-application.md) designs one installable
program an operator who does not own a terminal can use: it embeds the modem, it
picks audio devices from a settings screen, it shows the instrument panel, it
hosts the TNC ports Pat and WoAD attach to, and it carries chat and files. This
directory is **workstream A**, the piece every other one depends on — the seam
between the modem and everything built on top of it.

There is no user interface here, no chat, no file transfer, and no toolkit.
There is a runnable program, `ardop-spine`, but it is a harness rather than the
application: the graphical program will be a different binary linking the same
objects.

`ardopb` is unaffected. It stays the thing you run headless on a Pi, and the app
is a second consumer of the same core rather than a successor.

---

## Build and run

```sh
make app
```

Then a whole ARQ session between two stations in memory — connect, transfer,
verify, disconnect — in about two seconds:

```sh
./app/ardop-spine --loopback --telemetry --script test/app/session.script
```

Or a station on the TNC ports, to point a real client at:

```sh
./app/ardop-spine --null 600 --host 8515 --script test/app/idle.script
telnet localhost 8515
```

Or a real radio:

```sh
./app/ardop-spine --list-devices
./app/ardop-spine --audio 1 2 --ptt rts:/dev/ttyUSB0 --host 8515 \
    --script test/app/idle.script
```

Tests:

```sh
make test-core        # test/core/test_spine.c, with the rest of the suite
make test-app-tsan    # the two-thread ThreadSanitizer stress, Linux only
```

---

## What is here

| | |
|---|---|
| `spine.h` | **The public seam.** Workstreams C (the interface) and E (the protocol) include this and nothing else from here. |
| `spine.c` | The heap runtime, the loop body, the single observer, the telemetry sink, the transmit credit, the ownership gate. |
| `ring.h` `ring.c` | The single-producer/single-consumer ring of variable-length records that all three queues are built from. |
| `devices.{h,c}` | Who owns the sound card and the keying line, and when they are rebuilt. The only file in the tree that includes both `app/spine.h` and `shell/backend_ma.h`. |
| `tnc_host_tcp.c` | `shell/host_tcp.c` presented as an `app_tnc_ops`. The only file here that knows sockets exist. |
| `loopback.c` | Two spines wired to each other over an in-memory channel. |
| `script.c` | The scripted driver: TNC lines plus a handful of `@` directives. |
| `main.c` | `ardop-spine`: argument parsing, backend selection, and the loop. |

Three object groups, because they have three futures. `SPINE_OBJS`
(`spine.o`, `ring.o`) is the seam: portable C11 with no sockets, no feature-test
macros and no devices — what the eventual CMake application build compiles.
`SPINE_DEVICE_OBJS` (`devices.o`) owns the sound card and the keying line;
portable C11 too, but it names the miniaudio backend and the PTT object, so a
platform that gets its audio from somewhere else replaces this one file and keeps
the seam. `SPINE_HARNESS` is the phase-1 driver, and the shipping program will
not link it.

---

## The rules, and how each one is enforced

Following [`core/README.md`](../core/README.md): a convention that relies on
someone remembering it is a convention that decays. **A rule that cannot be
mechanically enforced is written as guidance and labelled as such.**

### 1. One runtime, one thread — *enforced by `make test-app-tsan`*

> Nothing outside the modem thread touches `ardop_runtime` or anything reachable
> from it, for any reason, ever.

Including configuration. `ardop_runtime` has no locks and no thread-safety
documentation, and `ardop_loop_step()` blocks for about 100 ms inside the capture
device, so this is not a style preference — a settings screen writing
`rt.link.bw_setting` from the interface thread is a data race against the audio
path, and it shows up as a corrupted frame once an hour.

The rule is a claim about what does *not* happen, so no single-threaded test can
support it. `test/core/stress_spine.c` runs a full session on one thread while
another submits, drains and polls flat out, and `make test-app-tsan` compiles the
*sources* — `core/`, `shell/runtime.c` and this directory — under
ThreadSanitizer, so that their silence is evidence rather than an artefact of
being uninstrumented.

`app_spine::tx_offered` and `tx_applied` are deliberately **not** atomic for the
same reason. Each belongs to exactly one thread; making them atomic would have
hidden a violation of that ownership instead of reporting it.

### 2. No sockets and no feature-test macros in `SPINE_OBJS` — *enforced by the build*

`app/%.o` gets no `-D_DEFAULT_SOURCE` and no `-D_GNU_SOURCE`, so a socket call
in `spine.c` fails to compile rather than being noticed in review. That is what
the `app_tnc_ops` table buys: the transport stays in `shell/`, a test supplies a
fake, and a platform where a listening socket is not dependable passes NULL.

### 3. Every warning is an error — *enforced by `-Werror`*

The same `CORE_CFLAGS` as `core/` and `shell/`: `-Wall -Wextra -Werror`
`-Wconversion -Wshadow -Wcast-qual -Wwrite-strings` and the rest.

One wrinkle worth knowing: the ordinary build carries no `-O`, and several GCC
warnings — `-Wformat-truncation` among them — only fire with optimisation on.
`make test-app-tsan` builds at `-O1` and so covers this directory at a bar the
default build structurally cannot. It has already caught one real defect.

### 4. The device manager's shared surface is one ring and two atomics — *guidance*

`app_devices` crosses threads with a request ring — the same SPSC record ring the
spine's three queues use, so the ordering guarantee is the one
`make test-ring-tsan` already proves — plus a published state and a generation
counter. A third shared field needs its own argument.

`app/devices.c` is deliberately absent from `TSAN_SRCS`, for the same reason
`net.c` and `ring.c` are: it is unreachable from a loopback-backed, socket-free
spine, and adding it would drag miniaudio under the sanitizer.

### 5. Exactly one observer — *guidance*

`ARDOP_MAX_OBSERVERS` is 4, the fifth registration is dropped silently, and there
is no way to unregister. The spine takes one slot at `app_open` and fans out on
the far side of the event queue, where a slow consumer costs nothing. Do not
raise the constant, and do not register a second observer through
`app_runtime()`: observers run on the audio path, and the right number of
callbacks to make there is the smallest one.

Nothing enforces this. `app_runtime()` makes it reachable.

---

## Three things that are not obvious

**Borrowed pointers die sooner than the callback contract suggests.** An
`ardop_obs`'s `text` and `data` address buffers inside the link that the *next
link step* overwrites — not merely the next callback — and one step performs
several actions. An observer that stored a pointer and copied later would
therefore hand the consumer several copies of the last message of each step.
Everything is deep-copied at the callback.
`test_host_messages_are_deep_copied` is the regression test, and it asserts on
the *distinctness* of a session's notifications because a
one-message-per-step test could not see the bug.

**A `TX_FRAME` observation reports `ptt = false` while transmitting.**
`shell/runtime.c:64-71` emits the observation before setting `ptt_keyed`, so the
status mirror that rides with it is a frame behind. The very next emission
corrects it. It is cosmetic, it lives in `shell/`, and it is not this
directory's to fix — but it will look like a bug in a PTT indicator, so it is
written down here rather than filed twice.

**`app_snapshot` is not a snapshot of one instant.** Each field is an independent
atomic load. It is for display; `app_tx_credit()` is the only correct source of
transmit credit, and the only one that takes part in the release/acquire pairing.

---

## Status

Workstreams A through E. What works: the seam, the three queues, transmit
backpressure, the TNC takeover rule against a real client, a complete ARQ session
over the loopback, the sanitised two-thread proof, device selection, the window,
this station hosting other people's TNC clients, and chat and file transfer over
a real link.

An operator can choose a sound card and a keying method, have the choice survive
a restart, and recover from a device that disappears by selecting another rather
than restarting the program. `--detect` finds a radio by pairing a sound card
with the keying interface on the same USB hardware. See
[`shell/README.md`](../shell/README.md) for what has and has not been run against
real hardware.

The application protocol is three files that stack, and the split is the point:

| | Knows about |
|---|---|
| `asp_wire.c` | bytes. Pure — no state, no I/O, no allocation |
| `asp.c` | a session. No transport and no storage; driven through `asp_io` |
| `asp_app.c` | this spine and this filesystem. The only file naming both |

`app/ui/aspsession.cpp` is a fourth and thinner layer that puts the result on the
interface thread. `test/app/asp.script` moves a file between two spines through
the real modulator and demodulator and compares it byte for byte.

Not here yet:

- **macOS and Android.** Windows and Linux first. Nothing here precludes them —
  that is what rule 2 is for.
- **The FEC profile.** `TEXT_B` is specified, framed, tested and *decoded* — but
  nothing sends one, and `asp_app_rx` only accepts `ARQ`-tagged payload, so no
  `FEC`-tagged bytes reach the parser that would handle it. What is missing is
  the whole profile around the message: no session, no peer, `FECREPEATS`
  duplicates to deduplicate on `(callsign, msg_id)`, and a screen where a
  broadcast is not addressed to anyone.
- **`ARQTIMEOUT`.** Deliberately refused rather than faked; see
  [`analysis/14`](../analysis/14-station-application.md) amendment 4.

The tests live in `test/core/` rather than a `test/app/` of their own because
that directory is really "the in-process suite", and a second one would need a
duplicate of the generic build rule and a second CI step to run it. Only the
scripts, in `test/app/`, are separate.
