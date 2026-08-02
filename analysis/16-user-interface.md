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

## 9. Proposal: a horizontal level meter

*Not built. Proposed here so the reasoning survives whoever picks it up.*

### Two problems, and the second is the real one

**Space.** `GaugeWidget` is a semicircular dial with a `minimumSizeHint` of
120×104 (`gui/gaugewidget.h:41`), and there are two of them side by side. That is
roughly 240×104 spent on two numbers, in the row above the waterfall — and the
waterfall is the instrument that actually earns vertical space, because it is the
only one showing something an operator cannot get from a text label.

**The scale lies, and that matters more.** The VU gauge is configured
`setScale(-60, 0, -12, -6)`: green below −12 dBFS, yellow to −6, red above. So
everything from −60 to −12 is green — including −50, which is unusable. The
meter reports "fine" for a signal the demodulator can do nothing with.

That is the wrong question. For a modem it is not "is this loud enough to hear"
but **"is this inside the window the demodulator wants"**, and the window has two
edges:

- too quiet and the S/N estimate and the constellation both degrade, with nothing
  on screen to say why;
- too loud and clipping destroys the 16QAM amplitude decision *before* it shows
  up as a decode failure — the fault that looks like a bad channel and is not.

A meter with one bad edge cannot express that. The most common real fault in
digital-mode operating is a mis-set input level, and this instrument is the one
that should catch it.

### What a VU meter is, and why we want more than one

The reference is the classic horizontal panel VU — a Hoyt 685VU and its
relatives. Two things are worth taking from it and one worth leaving.

Take the **horizontal format**: a bar reads at a glance, sits in a strip rather
than a square, and puts the scale where an operator already expects it.

Take the **marked target region**: a VU face is not a bare gradient. It is a
scale with a place you are supposed to be, and the red zone is a statement about
consequences rather than a colour ramp.

Leave the **ballistics**. A true VU meter is a 300 ms averaging instrument and
deliberately hides peaks — which is exactly the information needed to know
whether the input is clipping. Hence the two indicators asked for: an
instantaneous reading for drive level, and a peak that follows it.

### The design

```
  RX   ·40      ·30      ·20      ·12      ·6     0 dBFS
      ┌──────────────────────────────────────────────────┐
      │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░│  ▏        │
      └────────────────────────┬──────────────┴──────────┘
                          target band            peak
       -16.2 dBFS   pk -9.4
```

| | |
|---|---|
| filled bar | the instantaneous level, smoothed |
| shaded band | where the level is supposed to sit, drawn behind the bar |
| thin vertical line | the peak, held and then falling |
| colour | below the band dim, inside it green, above it amber, clipping red |

The colour follows the *band*, not a single threshold, so "too quiet" finally has
a reading. The numeric caption keeps both figures, because RMS is what the
demodulator sees and peak is what clips, and an operator setting a level needs
both.

### Ballistics

Two time constants, and **both driven by wall time rather than by telemetry
records** — the audio record arrives once per captured block, and the block size
is a property of the backend (300 samples at 48 kHz, 1200 by default), so a
meter counting records would decay at different speeds on different sound cards.

| | |
|---|---|
| level, attack | fast — within one update |
| level, decay | ~300 ms, the VU integration time, so the bar is readable rather than flickering |
| peak, attack | instant |
| peak, hold | ~1.5 s, long enough to read |
| peak, fall | ~20 dB/s once the hold expires, the broadcast PPM convention |

### What it replaces, and what it does not

One widget with two configurations replaces both dials: the same bar serves S/N
with a different scale and band, where the "target" is simply "above the floor
the mode needs". That is the space saving — one strip of perhaps 40 px where
there were two squares of 104.

It does **not** solve the transmit side. `ardop_runtime_telemetry_audio` is
called from the loop's receive path only (`shell/loop.c`), so there is no
transmit-level record at all and the meter can only ever be labelled RX. A TX
level meter would need the runtime to emit the same record from the modulator
pull, which is a change in `shell/`, not here.

### Design detail

One widget, replacing both `GaugeWidget`s.

```c++
class LevelMeter : public QWidget {
public:
    LevelMeter(const QString &label, QWidget *parent = nullptr);

    /* The scale, in whatever unit the caller is measuring. */
    void setScale(double min, double max);

    /* Where the value is supposed to sit. Drawn behind the bar, and what the
     * colour is decided against -- not a single threshold. */
    void setBand(double lo, double hi);

    /* Values below `lo` are "too quiet" rather than "fine", which is the
     * defect this replaces. Pass the same number twice for a one-sided
     * scale, which is what S/N wants. */
    void setValue(double level, double peak);
    void setUnknown(const QString &caption);   /* no data yet */

    QSize minimumSizeHint() const override { return QSize(220, 34); }
    QSize sizeHint()        const override { return QSize(420, 40); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    double toX(double v) const;    /* value -> pixel, clamped to the scale */

    QString  m_label, m_caption;
    double   m_min = -60, m_max = 0, m_bandLo = -24, m_bandHi = -12;
    double   m_level = -1e9, m_peak = -1e9;   /* displayed, after ballistics */
    double   m_rawLevel = -1e9;
    QElapsedTimer m_clock;         /* wall time: see below */
    qint64   m_lastTickMs = 0, m_peakHoldUntilMs = 0;
    QTimer   m_repaint;            /* 30 Hz, independent of the data rate */
    bool     m_haveData = false;
};
```

**The ballistics run on a repaint timer, not on `setValue`.** That is the whole
reason for `QElapsedTimer`: the audio record arrives once per captured block, and
the block belongs to the backend — 300 samples at 48 kHz is 40 Hz, the 1200
default is 10 Hz. A decay applied per record would fall four times faster on one
sound card than another. So `setValue` only updates the raw figures, and a 30 Hz
timer advances the displayed ones by elapsed milliseconds:

```c++
void LevelMeter::tick()          /* every ~33 ms */
{
    const qint64 now = m_clock.elapsed();
    const double dt  = double(now - m_lastTickMs) / 1000.0;
    m_lastTickMs = now;

    /* Level: instant attack, ~300 ms decay -- the VU integration time, chosen
     * so the bar is readable rather than flickering. */
    if (m_rawLevel >= m_level)
        m_level = m_rawLevel;
    else
        m_level += (m_rawLevel - m_level) * std::min(1.0, dt / 0.300);

    /* Peak: instant attack, hold, then a straight fall in dB per second. */
    if (m_rawPeak >= m_peak) {
        m_peak = m_rawPeak;
        m_peakHoldUntilMs = now + 1500;
    } else if (now > m_peakHoldUntilMs) {
        m_peak -= 20.0 * dt;                 /* the broadcast PPM convention */
        m_peak = std::max(m_peak, m_level);  /* never below the bar */
    }
    update();
}
```

**Painting**, top to bottom in a 34-40 px strip:

| band | contents |
|---|---|
| top ~10 px | tick labels at the decade marks and at both band edges |
| middle ~16 px | the trough: band shading, then the filled bar, then the peak line |
| bottom ~12 px | `label` on the left, `-16.2 dBFS   pk -9.4` on the right |

The trough is drawn in three passes so the band remains visible where the bar has
not reached it: band rectangle first in a low-contrast fill, then the bar clipped
to its own width, then a 2 px vertical line for the peak. The bar's colour comes
from where `m_level` falls:

```c++
below m_bandLo      QColor(0x50, 0x60, 0x70)   /* dim: present but too quiet */
inside the band     QColor(0x2e, 0xa0, 0x4e)   /* green */
above m_bandHi      QColor(0xd0, 0x92, 0x1c)   /* amber: headroom going */
within 1 dB of max  QColor(0xc0, 0x39, 0x2b)   /* red: clipping */
```

**Wiring** replaces the two dials in `StationWindow::buildPanel` with a vertical
pair, and the row above the waterfall loses ~64 px of height:

```c++
m_vu = new LevelMeter(tr("RX"), page);
m_vu->setScale(-60.0, 0.0);
m_vu->setBand(-24.0, -12.0);      /* was: green everywhere below -12 */

m_sn = new LevelMeter(tr("S/N"), page);
m_sn->setScale(-25.0, 30.0);
m_sn->setBand(5.0, 30.0);         /* one-sided: more is simply better */
```

`onAudio` and `onStatus` change only in the call they make. `GaugeWidget` stays
in the tree either way — the standalone remote panel uses it, and it is not this
proposal's business to change what `gui/` looks like.### Open questions

1. **Where do the band edges come from?** Hardcoded constants are the honest
   starting point, but the right values depend on the sound card's headroom and
   on whether the operator drives the radio's ALC. They may want to be settings.
2. **Does the band belong to the meter or to a diagnosis?** Open decision 5 above
   already notes that nothing joins S/N, level and constellation into "your mic
   gain is too high". This meter makes that diagnosis possible for one of the
   three; it does not make it.
3. **Whether the peak indicator should latch a clip.** A single sample at full
   scale is worth knowing about after it has gone, and a peak that has already
   decayed does not say it happened. A clip count, or a marker that stays until
   cleared, may be worth more than the moving peak.

---

## 10. Proposal: session history at two resolutions

*Not built. Proposed here so the reasoning survives whoever picks it up.*

### What it is for

An ARDOP session succeeds or fails slowly, and the interesting part is the
shape: how many frames were repeated, whether the link gear-shifted up or down,
how long each turnaround took, where the quality fell away. None of that is
visible in an instrument panel, which shows the present and forgets it.

Four questions this answers that nothing currently does:

- **Why is this slow?** A wall of repeats says "the channel", a clean run at a
  low data rate says "the negotiation", and they need different responses.
- **Is the link adapting?** Gear-shifting is a sequence, not a state.
- **What happened before it dropped?** A post-mortem needs the frames leading up
  to the failure, which by definition are gone by the time anyone looks.
- **Does our session look like `ardopcf`'s?** The interop validation
  [13](13-completing-the-rebuild.md) W3 still owes is far easier to judge from
  two histories side by side than from two logs.

### The gap: this data does not currently reach the interface

The runtime emits everything needed — `ARDOP_OBS_RX_FRAME` carries frame type,
quality and S/N, `ARDOP_OBS_TX_FRAME` the type, `ARDOP_OBS_RX_FRAME_BAD` a
failure. But neither queue delivers it:

- The **lossless event queue** deliberately excludes them. That was right: the
  state they describe is mirrored into `ARDOP_TLM_STATUS`, and duplicating it
  would have been two sources for one truth (`app/spine.c`).
- The **display queue** carries that mirror, and a mirror is *coalesced* — last
  value wins. Which is correct for a status lamp and destroys a history.

So a history needs a new record. It belongs on the **lossy display queue**,
following the `ARDOP_TLM_SPECTRUM` rule rather than the status rule: **deliver
every one, in order, and never coalesce** — each is a distinct event, and merging
two frames is exactly the information the view exists to show. Losing one under
extreme load is acceptable in a way that losing payload is not, which is why this
is not on the lossless queue.

```c
ARDOP_TLM_FRAME = 5

uint64_t at;          /* elapsed samples -- see "turn time" below */
uint8_t  frame_type;  /* everything else derives from this */
uint8_t  direction;   /* transmitted / received / failed to decode */
int16_t  quality;     /* 0..100, received only */
int16_t  sn;          /* dB, received only */
```

**The frame type is enough for the rest.** `ardop_frame_spec_for()` gives the
name, the modulation, the baud, the carrier count and the payload bytes
(`core/codec/frame.h`), so mode and data rate are computed at the display end
from a byte. Nothing about the protocol is written down in the interface — the
rule §5 already sets.

### Turn time, and the clock to measure it against

Stamp each record with the **elapsed sample count**, not a wall clock.

Turn time then falls out as the difference between the end of the last received
frame and the start of the next transmission — and it is measured against the
*protocol* clock, which is the one the deadline is actually expressed in.
[15](15-platform-audio-and-ptt.md) §8 puts the ARQ turnaround budget at 250 ms
and ends "then measure it"; nothing has. This measures it, on every frame, for
free, as a side effect of a view somebody wanted anyway.

A wall clock would be the wrong instrument here. If the sound card runs at
11 997 Hz every protocol deadline stretches with it, and a turnaround measured in
milliseconds would drift against the budget it is being compared to while a
turnaround measured in samples would not.

### The macro view: one cell per frame

```
   ▉▉▉▉▉▉▉▉░▉▉  ▓▓  ▉▉▉▉▉▉▉▉▉▉▉▉  ▓▓  ▉▉▉▉▉✗▉▉▉▉▉▉  ▓▓  ▉▉▉▉
   └── receiving ──┘ └TX┘ └──── receiving ────┘ └TX┘  ↑
                                                   failed decode
```

One cell per frame rather than per unit of time, because ARDOP frames differ in
length by more than an order of magnitude and the question an operator has is
"how many repeats", which is a count. The grid wraps; a long session is a block
of texture whose *pattern* is readable before any individual cell is.

Encoding, and the constraint that shapes it is colour blindness:

| | |
|---|---|
| hue | direction — received one hue, transmitted another. **Not red/green.** |
| lightness | decode quality, received frames only |
| a distinct mark | a failed decode: different *shape* as well as colour, so it is not carried by hue alone |
| gaps | dead air, proportional to the turnaround |

A run of repeats reads as a stripe of one hue. A gear-shift reads as a texture
change. A retry storm reads as a rash of failure marks. None of that needs a
legend to notice, which is the point of a macro view.

### The micro view: the same events, as a table

| at | dir | mode | bytes | Q | S/N | turn |
|---|---|---|---|---|---|---|
| 00:41.3 | RX | 4PSK.200.100.E | 128 | 92 | 14 | — |
| 00:42.1 | TX | ACK | — | — | — | 180 ms |
| 00:42.4 | RX | 16QAM.500.100.E | 512 | 61 | 9 | — |
| 00:43.2 | RX | 16QAM.500.100.E | — | ✗ | 8 | — |

Sortable, scrollable, and selectable. Everything in it is derived from the five
fields above plus the frame table.

### Linking the two

Clicking a cell scrolls the table to that frame and selects it; selecting a row
highlights its cell. That is the whole of "two resolutions" — one dataset, two
projections, and a cursor shared between them. Without the link the macro view is
decoration.

### Bounded, like everything else here

A ring of frames with a fixed capacity, in the interface rather than the spine.
The spine's job is to deliver events, not to remember them, and keeping the
history at the display end means `--remote` gets it for nothing — the record
travels the telemetry wire like every other, so a panel watching a station across
the shack builds the same history as one embedding the modem.

At roughly sixteen bytes a record, ten thousand frames is 160 kB and covers a
long Winlink session. When it wraps, the oldest go; a session longer than the
ring is one where the recent part is what matters.

### Design detail

Four pieces: a record on the wire, a ring that holds them, and two views over the
same ring.

#### 1. The record, in `shell/telemetry.h`

```c
ARDOP_TLM_FRAME = 5,

/* added to ardop_telemetry, alongside the existing per-kind fields */
uint64_t frame_at;      /* elapsed samples, from the loop clock */
uint8_t  frame_dir;     /* ardop_tlm_dir */
/* frame_type, quality and sn already exist and are reused */

typedef enum {
    ARDOP_TLM_DIR_RX = 0,
    ARDOP_TLM_DIR_TX,
    ARDOP_TLM_DIR_RX_FAILED,
} ardop_tlm_dir;
```

Sixteen payload bytes: `u64 at`, `u8 type`, `u8 dir`, `i16 quality`, `i16 sn`,
little-endian like every other record. `ardop_tlm_encode` and `ardop_tlm_parse`
each gain one case, and `test/core/test_telemetry.c` one round-trip.

#### 2. Emission, in `shell/runtime.c`

Three call sites, all next to observations that already fire, so no new
traversal and nothing new computed on the audio path:

| where | direction |
|---|---|
| the `ARDOP_EV_FRAME_DECODED` branch, beside `ARDOP_OBS_RX_FRAME` | `RX` |
| beside `ARDOP_OBS_RX_FRAME_BAD` | `RX_FAILED` |
| `start_tx`, beside `ARDOP_OBS_TX_FRAME` | `TX` |

Each fills the record and calls the existing sink. `rt->now` is the elapsed
sample count and is already current at all three points.

**Gated on the telemetry sink being present**, like the spectrum and the
constellation (`shell/runtime.h:133-135`), so a headless `ardopb` pays nothing.

#### 3. The ring, in the interface

```c++
struct FrameRecord {
    quint64 at;          /* elapsed samples */
    quint8  frameType;
    quint8  dir;
    qint16  quality;     /* RX only; -1 when not applicable */
    qint16  sn;
    quint32 turnMs;      /* computed on insert; 0 when not a turnaround */
};

class SessionHistory : public QObject {
public:
    void append(const FrameRecord &r);   /* computes turnMs, then stores */
    int  count() const;
    const FrameRecord &at(int i) const;  /* 0 is the oldest held */
    void mark(const QString &why);       /* a session boundary */

signals:
    void appended(int index);
    void wrapped();                      /* the view must rebase its indices */

private:
    QVector<FrameRecord> m_ring;         /* fixed capacity, never reallocated */
    int m_head = 0, m_count = 0;
    quint64 m_base = 0;                  /* samples at the first record held */
    quint64 m_lastRxEnd = 0;
};
```

**Turn time is computed on insert**, because it needs the *previous* record and
the view should not have to look backwards:

```c++
if (r.dir == TX && m_lastRxEnd)
    rec.turnMs = quint32((r.at - m_lastRxEnd) * 1000 / 12000);
if (r.dir != TX)
    m_lastRxEnd = r.at;
```

A frame's `at` is when it was *observed*, so this is the gap between one frame
being decoded and the next transmission starting — which is the quantity
[15](15-platform-audio-and-ptt.md) §8's 250 ms budget is about. Divide by 12000
only for display; the stored figure stays in samples.

Capacity 10 000: at 16 bytes plus the computed field, around 200 kB, and long
enough for a full Winlink session.

#### 4. The macro view

```c++
class HistoryGrid : public QWidget {
    /* Cells of kCell x kCell with kGap between, wrapped to the widget width.
     * No scrolling: the whole ring is always visible, which is the point --
     * the pattern is readable before any individual cell is. Cell size falls
     * as the ring fills, to a floor of 3 px, below which the view stops
     * showing the oldest rather than becoming unreadable. */
    static constexpr int kCell = 7, kGap = 1, kMinCell = 3;

    int indexAt(const QPoint &p) const;   /* hit test, for the shared cursor */

signals:
    void frameClicked(int index);

public slots:
    void setCursor(int index);            /* driven by the table */
};
```

Painting is one `fillRect` per cell, plus a two-pixel diagonal for
`RX_FAILED` — the shape difference that keeps failure off hue alone. Colour:

```c++
RX          lerp(QColor(0x1d,0x3a,0x5c), QColor(0x4f,0xa3,0xe8), quality/100.0)
TX          QColor(0xd8, 0x9b, 0x2e)      /* amber; no quality exists */
RX_FAILED   QColor(0xc0, 0x39, 0x2b) + the diagonal
cursor      a 1 px outline in the palette's highlight colour
```

Blue for received and amber for transmitted, deliberately not red against green.
Quality rides *lightness* within the received hue, so the two dimensions do not
compete.

Dead air is a gap rather than a cell: a run whose `at` differs from its
predecessor's by more than one frame's duration leaves a blank of width
proportional to `log(gap)`, which keeps a two-second turnaround visible without
letting a thirty-second one dominate the row.

#### 5. The micro view

A `QAbstractTableModel` over the same `SessionHistory` — not a copy — with seven
columns, deriving everything from `frameType` through `ardop_frame_spec_for()`:

| column | source |
|---|---|
| at | `at / 12000`, as `mm:ss.s` |
| dir | `dir` |
| mode | `spec->name` |
| bytes | `ardop_frame_payload_bytes(type)`, blank for control frames |
| Q | `quality`, blank for TX |
| S/N | `sn`, blank for TX |
| turn | `turnMs`, blank when not a turnaround |

`rowCount` follows `SessionHistory::count()`; `appended` becomes
`beginInsertRows`, and `wrapped` a `beginRemoveRows` for row 0. Hosted in a
`QTableView` with `setUniformRowHeights(true)`, because ten thousand rows is
enough for the layout cost to show.

#### 6. The shared cursor

```c++
connect(m_grid,  &HistoryGrid::frameClicked, this, &HistoryPage::selectFrame);
connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
        this, [this](const QModelIndex &i) { selectFrame(i.row()); });

void HistoryPage::selectFrame(int index)
{
    if (index == m_cursor)   /* the guard that stops the two signals looping */
        return;
    m_cursor = index;
    m_grid->setCursor(index);
    m_table->selectRow(index);
    m_table->scrollTo(m_table->model()->index(index, 0),
                      QAbstractItemView::PositionAtCenter);
}
```

#### 7. What it costs elsewhere

| file | change |
|---|---|
| `shell/telemetry.{h,c}` | one record kind, one enum, encode and parse |
| `shell/runtime.c` | three emissions beside existing observations |
| `test/core/test_telemetry.c` | one round-trip case |
| `app/ui/spinesource.{h,cpp}` | one signal, one case in `drainDisplay` — **not coalesced**, unlike status |
| `gui/telemetryclient.{h,cpp}` | the same signal, so `--remote` gets the history too |
| `app/ui/` | `sessionhistory`, `historygrid`, `historytable`, `historypage` |

Nothing in `core/`. Nothing in the spine — `app/spine.c` does not learn what a
frame record is, because the display queue carries the wire format and passes it
through untouched, which is what §6 bought.

### Open questions

1. **Does the history survive a disconnect?** Per-session is the obvious
   framing, but the useful comparison is often *across* sessions — "it was fine
   yesterday". A session boundary marker in a continuous ring may be better than
   clearing.
2. **Should it be exportable?** A CSV of this is exactly what a bug report about
   a bad link should contain, and exactly what an interop comparison against
   `ardopcf` needs. Cheap to add, and easy to forget until somebody needs it.
3. **Does the TX side get a quality figure?** It cannot: a transmitter has no
   measurement of its own signal. The table's blank cells for TX rows are honest
   and will still look like missing data. Worth a legend rather than a workaround.
4. **What marks a repeat?** The link knows it is repeating a frame; the frame
   type does not say so. Distinguishing "sent twice" from "sent two frames of the
   same type" would need a flag from the link, which is a `core/` change and
   should not be made casually.

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


---

## Amendments made during implementation

Recorded here rather than silently worked around, following
[15](15-platform-audio-and-ptt.md)'s discipline: each is a place this document
and the code as it was built disagreed.

1. **Qt Widgets, not Qt Quick.** §1 chose Quick and rejected Widgets as
   desktop-only. That was correct when the target was four platforms including
   Android — and the port was subsequently scoped to Linux and Windows, which
   removes the premise. With two desktop platforms, Widgets reuses `gui/`'s
   waterfall, constellation, gauge and lamp painting directly, where Quick means
   reimplementing all of it as `QQuickPaintedItem` or Canvas. §2's "the maths
   carries, the painting does not" cuts both ways: under Widgets the painting
   carries too.

   It also costs nothing in dependencies. `qt6-base` supplies Widgets and
   Network, so neither the development machine nor the MSYS2 CI job needs the
   declarative module.

   **Deferred, not reversed. The trigger for revisiting is an actual Android
   target**, at which point the instrument painting is what moves and the seam
   beneath it — `app/spine.h`, the device manager, the C library — does not.

2. **§5's "delete the duplicated protocol tables" is already done and is not a
   port concern.** `gui/constellationwidget.cpp` gets its frame names from
   `core/codec/frame.c` by compiling that file in, which `gui/CMakeLists.txt`
   has done since it was written.

3. **§8's `--remote` mode is implemented, and the second executable is gone from
   the download.** *(Amended again: this entry first recorded the mode as not
   built.)*

   The prediction held -- both sources already emitted the same signals, because
   the display queue carries the wire format (§6). What was missing was a *type*:
   the two classes agreed by convention, and a window cannot `connect` to a
   convention. `gui/panelsource.h` is that type, and it draws the line in the
   place that matters: the five signals a display needs are on the base, and
   everything that commands the modem is on `SpineSource` alone. So `--remote`
   shows only the Panel not because a check disables the other screens, but
   because the source it was handed cannot command anything.

   `ardop-gui` still builds and still runs in CI -- it is worth continuing to
   compile, since it proves the instrument widgets do not depend on the spine --
   but it is no longer shipped. One download, one executable, and nobody has to
   choose between two programs that look the same.

4. **The waterfall's one-to-one blit has to be written in device pixels, and
   was not.** The original scroll used a fixed 700-row image scaled into whatever
   height the widget happened to be, so every new row moved the picture by a
   fractional pixel and the resampling shifted which source row landed on which
   output row -- the display shimmered. That was fixed by growing the image to at
   least the canvas height, and it worked.

   It worked *at 100% scaling*. `QWidget::height()` is in logical pixels and the
   blit lands in device pixels, so on a display at 125% or 150% -- which is most
   Windows machines -- the vertical scale was still 1.25 or 1.5 and the shimmer
   was still there. It was reported from a Windows machine and could not be
   reproduced on the Linux one, because this display runs at 100%.

   The fix is one unit change: every row count is now in device pixels
   (`canvasRows()`), and `paintEvent` derives its target rectangle from the row
   count rather than from the widget, so the rectangle maps to a whole number of
   device pixels however the ratio divides.

   **The general form is worth more than the fix.** A geometry bug that is exact
   at one device pixel ratio and wrong at every other is invisible to whoever
   wrote it and obvious to the operator, and no amount of looking at it on the
   development machine will find it. So it is now tested rather than looked at:
   `app/ui/test_widgets.cpp` feeds the widget a single bright scan line and
   asserts it renders one row thick and stays that thickness as it scrolls, in a
   child process at each of 1.0, 1.25, 1.5 and 2.0. Against the previous code it
   passes at 1.0 and fails at all three others, which is exactly the signature.
   It runs offscreen, in CI, on both hosts.

   **Anything else drawn here inherits the same hazard** -- the §9 meter and the
   §10 history grid both paint at pixel scale, and both should get a case in that
   file rather than a comment.

5. **§4's narrow layout, and two of the four platforms in the exit criteria, are
   out of scope.** This document was written when the target was Linux, Windows,
   macOS and Android. The port was subsequently scoped to Linux and Windows, and
   §4 -- the phone breakpoint, the bottom tab bar, "the gauges are the first thing
   to go" -- is entirely about a target that is no longer being built. So is the
   exit criterion "renders usably at 360 x 640 with nothing clipped".

   Recorded rather than deleted, for the same reason amendment 1 defers Qt Quick
   rather than reversing it: **the analysis is still correct, and it is what
   should be read first if Android comes back.** The observation that a
   semicircular needle dial is a poor use of 80 pt of phone width is what
   eventually produced §9, on a desktop, for a different reason.

   What remains live from the exit criteria: one binary embedded and in
   `--remote`; all four instruments verified against every modulation; no protocol
   name written down in the UI; and the telemetry ring dropping rather than
   blocking under a stalled UI, *measured*.

   Also settled: **open decision 1 is void** -- amendment 1 chose Widgets, so
   there is no QML/C++ split left to decide.

6. **§9's level meter is built, with two corrections that only appeared once it
   ran.**

   **The decay formula was not time-invariant.** §9 specified
   `m_level += (raw - m_level) * min(1, dt / 0.300)`, which is a first-order
   approximation: it agrees with an exponential for small `dt` and diverges badly
   for large. Half a second delivered as one step clamps the factor to 1 and
   snaps straight to the new value; the same half second in ten steps lands 6 dB
   short. The built widget uses `raw + (level - raw) * exp(-dt/tau)`, which is
   exact for any step.

   This matters more than it looks, because the step size is *precisely* what is
   not under our control -- a repaint can be late, and a loaded machine can
   deliver one 500 ms tick instead of fifteen. The entire reason the ballistics
   were moved onto a clock was so the meter behaves the same regardless, and the
   linear form quietly gave that property back. The test asserts it directly:
   one 500 ms step and ten 50 ms steps must land in the same place, and they now
   agree to two decimal places.

   **34 px was measured against a drawing, not against text.** The tick band was
   10 px, of which 3 is the tick mark, leaving a 7 px box for a line of type --
   so every scale number rendered cut in half. The bands are now 15/15/13 and the
   minimum height is 43.

   Both were found by running it and looking, which is worth recording: the first
   by a test written because amendment 4 said to write one, the second by a
   screenshot. Neither was visible in the source.

   Open question 3 (whether the peak should latch a clip rather than decay) is
   **not** built and remains open.

7. **§10's session history is built. Three things the design detail had wrong,
   all of them silent failures.**

   **The turn-time sentinel collided with a real value.** The ring used
   `m_lastRxEnd == 0` to mean "nothing heard yet", but `at` is elapsed samples
   and the first frame after a device opens can legitimately be at 0 -- so the
   first turnaround of every session reported 0 ms. A plausible-looking number,
   which is the worst kind of wrong for a figure an operator is going to tune
   against. A separate flag now carries the meaning.

   **Only the *first* transmission after a reception is a turnaround.** §10 said
   "the gap between one frame being decoded and the next transmission starting",
   which is right and which the first implementation did not do: it measured
   *every* transmission from the same reception, so a three-frame transmission
   reported three turn times, each larger than the last. A column of numbers that
   all look like turn times but mean different things is worse than a blank.

   **`clear()` needs its own signal.** §10 gave the ring `appended` and `wrapped`.
   A device change discards the history -- it must, because the timestamps are
   elapsed samples on a clock that belongs to the device, and keeping them across
   a restart would silently misorder the session -- and a model told about that as
   a "wrap" keeps handing the view rows that no longer address anything.

   Two layout defects found by rendering it: the grid asked for a fixed height and
   drew three rows of cells into a 150 px black rectangle, because a size hint
   that depends on the width is asked for before the width exists; and the summary
   label said "no frames yet" over a full table, because it was only updated on
   append and a page built after frames have arrived never gets one.

   **What §6 bought, collected.** Nothing in `core/` changed. `app/spine.c` never
   learns what a frame record is -- the display queue carries the wire format and
   passes it through untouched. The whole feature is one record kind, three
   emissions beside observations that already fire, and four files in `app/ui/`.
