# 16 — The user interface: toolkit and design

[14](14-station-application.md) settled that the application embeds the modem and
owns a C modem thread. This document chooses what draws the pixels, decides what
survives from [`gui/`](../gui/), and specifies the screens.

---

## 1. The toolkit

### Criteria

1. **Linux, Windows, macOS, Android from one codebase**, with iOS not precluded.
2. **The C side is the host.** The app is a C program with ~317 kB of modem state
   and a real-time thread; the UI is the guest. A toolkit that inverts that
   relationship pays a marshalling tax on every event.
3. **Custom real-time drawing.** A scrolling spectrogram and a scatter plot,
   updated ~12 times a second, on battery.
4. **Packaging** that produces something an operator can install.
5. **What already exists.** 1518 lines of working Qt in `gui/`, and a telemetry
   client that is already a clean seam.

### The evaluation

| | 5 platforms | C as host | Drawing | Packaging | Verdict |
|---|---|---|---|---|---|
| **Qt 6 Quick (QML)** | yes, all five | native — no FFI at all | `QQuickPaintedItem` (QPainter) or scene graph | `qt_add_executable` does APK/bundle | **chosen** |
| Qt 6 Widgets | desktop only in practice | native | `paintEvent` | desktop only | rejected |
| Flutter + `dart:ffi` | yes, all five | inverted — Dart hosts | `CustomPainter`, GPU, excellent | best-in-class | rejected, narrowly |
| Rust + Slint/egui | mobile immature | second language boundary | good | immature | rejected |
| Web view (Tauri) | mobile beta | inverted, plus IPC | canvas, fine | webview divergence | rejected |
| Native per platform | yes | native | best | best | four UIs; no |

**Chosen: Qt 6 Quick with C++ backend classes.**

The deciding argument is criterion 2. This application is a C program that
happens to have a UI — a modem thread, a 317 kB runtime struct, borrowed pointers
valid only for the duration of a callback, and a payload path where dropping
bytes corrupts a file. Qt is the only candidate where the UI lives *inside* that
program's address space with no marshalling layer, where a QML view can be backed
by a C++ class that includes `link/link.h` directly, and where the modem thread is
an ordinary `QThread` with a queued connection to the GUI thread.

Everything else on the list requires the C core to become a library that a
foreign runtime calls into. That is a real and well-trodden design — it is
[09](09-embedding-and-bindings.md)'s whole subject — but it means every telemetry
record and every observer event crosses a language boundary, and the constraints
09 lists for a bindable C API (no `exit()`, no varargs, no stdout, caller-owned
buffers) become binding on code that currently ignores some of them.

**Flutter is the real alternative and loses narrowly.** Its touch UI and its
mobile packaging are better than Qt's, its licence is friendlier, and
`CustomPainter` would draw the waterfall beautifully. It loses on the C
relationship, on Linux desktop maturity — which matters because Linux is this
project's primary platform — and on discarding the 1518 lines that already work.

### The cost to name: LGPL

Qt's open-source licence is LGPLv3. Dynamic linking, which is what desktop
builds and Qt's Android deployment do, is unproblematic for an MIT application.
**iOS is where this bites**: Qt links statically there, and LGPL §4 then requires
shipping relinkable object files. That is solvable and others have solved it, but
it is a real obligation attached to the deferred platform, and it should be known
now rather than discovered later.

---

## 2. What carries over from `gui/`

The split is clean and worth stating precisely, because the temptation is to
treat 1518 lines of working code as either all reusable or all disposable.

### Disposable: the layout and the shell

- `QMainWindow` + `QStatusBar` is a desktop metaphor. `mainwindow.cpp:74-81`
  puts connection state and frame mode in a status bar, which on a phone collides
  with the gesture bar.
- `resize(900, 560)` (`mainwindow.cpp:84`) and a four-across row at stretch
  `2:3:2:4` (`mainwindow.cpp:61-64`). **The minimums do not fit a phone**: two
  gauges at `QSize(120, 104)` (`gui/gaugewidget.h:41`) plus the lamp panel at
  `QSize(210, 104)` (`gui/statuslamps.h:31`) demand 450 pt before the
  constellation is given anything, against ~390 pt of portrait width. There is no
  breakpoint logic anywhere; it will overflow, not reflow.
- `QCommandLineParser --host` (`main.cpp:26-36`) is meaningless on Android.
- All pixel constants are physical, not DPI-scaled: `kAxisHeight = 16`, 12 pt
  lamp dots, 1.6 pt constellation points.
- **Zero touch handling.** No `mousePressEvent`, no gestures, no
  `WA_AcceptTouchEvents`. The panels are pure output. That is convenient —
  nothing touch-hostile to unpick — but pinch-zoom on the waterfall and tap-for-detail
  are new work.

### Reusable: the algorithms

These are the value, they are toolkit-independent, and they should be extracted
into plain C++ classes with no Qt painting in them, then driven by whatever draws.

**Waterfall** (`gui/waterfallwidget.cpp`):
- power → dB as `10*log10(mag + 1)`;
- noise floor from the **25th percentile** via `std::nth_element`
  (`waterfallwidget.cpp:136`);
- **asymmetric floor tracking** — `kFloorRise = 0.02f`, `kFloorFall = 0.25f`
  (`:27-28`), so the floor rises slowly and falls fast, which is what stops a
  passing signal from washing the display out;
- a 4-segment colour ramp black → blue → green → yellow → white (`:100`);
- the frequency axis is labelled **from the stream's announced geometry**
  (`m_firstBin * m_binHz`), not from hardcoded constants — 206 bins from bin 25
  at 11.719 Hz (`core/modem/busy.h:113-115`).

**Constellation** (`gui/constellationwidget.cpp`) — the most substantial piece,
and the one nobody would reconstruct from scratch correctly:
- 3-frame history with alpha fade: newest 235, older `max(40, 150 - age*55)` (`:265`);
- auto-scale so the strongest symbol of the newest frame reaches the edge
  (`:247-249`), because magnitudes are in gain-dependent demodulator units;
- decision overlays drawn against **the decoder's own thresholds**: 4PSK rays at
  `k·π/2`; 8PSK and 16QAM at `k·π/4 + π/8 + π/4` (`:230`);
- 16QAM additionally draws the **adaptive magnitude ring** from the frame's own
  `car_mag_threshold`, scaled by that frame's max (`:253-259`) — carried on the
  wire rather than guessed;
- 4FSK has no I/Q plane, so it gets four tone lanes with X = decision margin per
  mille, a dashed 25% confidence gate, and a deterministic de-overlap jitter
  `((i*37) % 11 - 5)/5.0` (`:145`).

**Gauge** (`gui/gaugewidget.cpp`): the coloured band is 60 discrete arc segments
rather than three arcs, so a reversed scale (`warn > bad`, which S/N needs) works
with no special case (`:90-99`).

**Lamps** (`gui/statuslamps.cpp`): the 10 → 5 collapse of protocol states onto
lamps.

### The port

`QQuickPaintedItem` takes a `QPainter`, so each widget's paint body moves nearly
verbatim. Start there for all four. The waterfall's `memmove` over `QImage::bits()`
plus a full-surface repaint at 11.7 Hz (`waterfallwidget.cpp:147-152`) is trivial
on desktop and a measurable battery cost on a phone; if measurement says so, move
that one to a `QSGSimpleTextureNode` with a ring-buffer image and a moving source
rect, which removes both the copy and the full repaint. Do not do that
speculatively.

---

## 3. Screens

| Screen | Contents | Source |
|---|---|---|
| **Panel** | Waterfall, constellation, S/N, audio level, link lamps, PTT, busy | telemetry queue |
| **Devices** | Capture/playback pickers, PTT method and its settings, input level meter with a "your AGC is on" warning | [15](15-platform-audio-and-ptt.md) §4, §6, §7 |
| **Station** | Callsign, grid, aux calls, ARQ bandwidth, listen, busy-detect sensitivity, FEC mode and repeats | `rt->link.*` via the command queue |
| **Session** | Connect to a callsign, disconnect, abort, break; who currently owns the link | `ardop_host_cmd` |
| **Chat** | Message list, compose, FEC-broadcast vs ARQ-session mode | [17](17-application-protocol.md) |
| **Files** | Send queue, receive list, per-transfer progress, resume, verify | [17](17-application-protocol.md) |
| **Guests** | Attached TNC clients, which one holds the session, config they changed | [14](14-station-application.md) §Decision 4 |
| **Console** | Raw TNC command entry and the reply log | `ardop_host_command()` |

The Console is not a debug afterthought — it is how anyone diagnoses a Pat
interaction, and the parser is already linked, so it costs a text field.

---

## 4. Responsive layout

Two breakpoints, not a continuum.

**Wide (desktop, tablet landscape).** Essentially today's panel: instrument row
across the top, waterfall filling the rest, with the other screens as a side
navigation. The current stretch ratios are good and should be kept.

**Narrow (phone portrait).** Bottom tab navigation across Panel / Chat / Files /
Settings. The Panel tab becomes:

- waterfall filling most of the height — it is the display that tolerates being
  narrow, and the one an operator actually watches;
- a compact status strip: state, S/N as a number rather than a dial, PTT and busy
  as dots, remote callsign;
- constellation and the two gauges behind a swipe or a "Signal" sub-tab.

The gauges are the first thing to go. A semicircular needle dial is a poor use of
80 pt of phone width, and the number it encodes is more legible as a number.

---

## 5. Delete the duplicated protocol tables

`gui/` currently hand-copies protocol data in two places, and both become
unnecessary the moment the UI is in the same binary as the headers:

- `gui/statuslamps.cpp:19-30` — all ten `ardop_link_state` names and the three
  mode names.
- `gui/constellationwidget.cpp:34` — `enum { kMod4FSK = 0, kMod4PSK = 1, kMod8PSK = 2, kMod16QAM = 3 };`,
  with a comment arguing the copy is safe because the enum is frozen. That file
  already `#include`s `codec/frame.h` for `ardop_data_frame_name`, so the stated
  reason for the copy (not pulling in protocol headers) does not hold in that
  file.

`statuslamps.cpp`'s comment says an unknown index shows as `?`. That protects
against values appended past the end; it does not protect against anything
inserted or reordered in the middle, which would silently mislabel every state
after the insertion point. Include the headers.

---

## 6. The in-process telemetry path: keep the wire format

The obvious optimisation is to bypass `shell/telemetry.c` and hand
`ardop_telemetry` structs straight from the callback to the UI. **Do not.**

[14](14-station-application.md) Decision 2 requires that everything crossing from
the audio path be deep-copied into a bounded, drop-oldest, non-allocating queue.
Encoding a record into a byte ring *is* that deep copy — `ardop_tlm_encode()`
already does it, records are self-delimiting so the oldest can be dropped without
a side index, and `ardop_tlm_decoded` hands the UI thread an owned copy with its
own array storage. The queue in 14 is `shell/telemetry_tcp.c`'s ring with the
socket replaced by a `memcpy`.

What that buys:

- **One definition of the format**, already round-trip tested
  (`test/core/test_telemetry.c`), shared by the embedded path, the TCP server and
  any remote panel.
- **Remote attach keeps working with no second code path.** The same
  `TelemetryClient` seam, five signals over three transport-free structs, is fed
  either by a socket or by the in-process ring.
- Forward compatibility for free: `ardop_tlm_parse` sets `consumed` even for an
  unknown kind, so a mismatched pair skips rather than desyncs.

The cost is an encode and a decode of at most ~8 kB at ~12 Hz. It is not
measurable.

The **event bus has no wire format** and does not need one — it crosses as a
small POD copy struct on the app side, with `ARDOP_OBS_RX_DATA`'s payload copied
inline. That queue is lossless (14 Decision 2); the telemetry ring is lossy. They
are separate for that reason.

---

## 7. The missing remote callsign

`ARDOP_TLM_STATUS` carries state, mode, busy, PTT, S/N, quality, bandwidth and
buffer length (`shell/telemetry.h:123-131`) — **but not who you are connected
to**, which is the first thing an operator looks for. Today that only arrives as
a `CONNECTED <call> <bw>` line on the host command channel.

Embedded, this is free: `ARDOP_OBS_STATE` carries `remote`
(`shell/runtime.h:69`), and the app is already consuming the event bus. Use it.

For the standalone remote panel it is not free, and the fix is worth stating
because the obvious one is wrong. **Do not extend the STATUS payload.** Every
parse path in `shell/telemetry.c` validates the payload length *exactly* before
reading, and the stream's forward compatibility is by record *kind*, not by
length — so a longer STATUS would be rejected outright by an existing reader,
while a **new record kind** (`ARDOP_TLM_PEER = 5`) is skipped harmlessly by one
and read by a new one. Same information, no version bump, no flag day.

---

## 8. What happens to `gui/`

`gui/` is promoted into the application rather than kept beside it, and the
standalone panel survives as a **mode of the same binary**: with no radio
configured, or with `--remote HOST:PORT`, the UI attaches a `TelemetryClient` to
a running `ardopb` instead of an embedded modem, and everything downstream of the
five signals is identical.

That keeps a genuinely useful capability — pointing a display at a remote
station's TNC, one-way, without handing anyone a transmitter (`gui/README.md:6-11`)
— without a second build target or a second copy of the drawing code. It also
settles [14](14-station-application.md)'s open decisions 1 and 2: the app lives
where `gui/` lives, and the panel is a mode, not a program.

---

## Open decisions

1. **QML for the instrument panel too, or only for the chrome?** The panels could
   remain C++ `QQuickPaintedItem`s hosted in a QML layout (proposed), or the
   layout could stay C++ with QML only for the settings screens. The first is
   more consistent; the second is less new syntax.
2. **Theming.** `gui/` hardcodes RGB everywhere except one
   `palette().color(QPalette::WindowText)` (`gaugewidget.cpp:122`). Dark is right
   for a radio panel; light mode on a phone in daylight is a real requirement.
   Decide whether both are supported or dark is declared.
3. **Whether the waterfall gets touch interaction** (pinch to zoom the frequency
   axis, drag to scrub history) or stays pure output. It is the one panel where
   interaction has obvious value.
4. **Localisation.** Qt has the machinery; adopting it later is more expensive
   than adopting it now, and the amateur radio audience is not English-only.
5. **Where the operator's "signal is bad" diagnosis lives.** The panel shows
   S/N, the input level, and the constellation, but nothing joins them into
   "your mic gain is too high" — which is the single most common real fault. A
   diagnostics view is out of scope here but should not be forgotten.

## Exit criteria

- One binary runs the full UI on Linux, Windows, macOS and Android, embedded, and
  in `--remote` mode against a running `ardopb`.
- All four instrument panels render correctly for every modulation — 4FSK 4/4
  tones, 4PSK 4/4 sectors, 8PSK 8/8, 16QAM 8/8 with a non-zero ring — matching
  the verification already done for `gui/` at commit `45f772b`.
- No protocol name or enum value is written down anywhere in the UI; all come
  from `core/` headers.
- The narrow layout renders usably at 360 × 640 with nothing clipped.
- The telemetry ring drops rather than blocks under a stalled UI, and the modem
  thread's timing is unaffected — measured, not assumed.
