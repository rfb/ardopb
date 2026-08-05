# 20 — Field results: what the radio said

[19](19-field-testing.md) is a request. This is the answer coming back, and it is
a running log: one section per session, newest last, with every result attributed
to the station that produced it. A finding here is only as good as the hardware
it came from, so the hardware is named first.

The headline is that **three of the four "never met hardware" rows in 19 have now
met hardware**, and the one that looked safest -- USB detection, verified against
fabricated `sysfs` trees -- was the one that did not work at all.

---

## Session 1 — 2026-08-04, VA7BFR

Build `b7470de`. Linux (Debian, kernel 6.12), PulseAudio.

### The station

| | |
|---|---|
| **Radio** | Icom **IC-746PRO** |
| **Interface** | **DigiRig Mobile**, a later revision |
| **Audio** | C-Media `0d8c:0012`, the DigiRig's own codec |
| **Keying, as configured** | `civ:/dev/ttyUSB0:9600@56` |

The DigiRig detail that cost the most time: this revision uses a **CP2102N**
(`10c4:ea60`), *not* the CH340 that every guide and forum post describes. The
codec and the bridge sit behind an internal Genesys hub as two separate USB
devices:

```
Bus 001 hub (05e3:0608)          the DigiRig's internal hub
  ├─ 1-2.1  CP2102N 10c4:ea60  → /dev/ttyUSB0    keying
  └─ 1-2.2  C-Media 0d8c:0012  → the sound card, plus a HID interface
```

An unrelated CH340 on another bus held `/dev/ttyUSB1`, which is exactly the
confusion `--detect` exists to remove. **Identify a DigiRig by the CP2102N and
C-Media pair behind one hub, never by `ttyUSB` number**, which moves on replug.

### What worked

| 19's step | Result |
|---|---|
| Part 1, `--list-devices` | Correct; both codec streams listed with usable ids |
| Part 1, `--detect` | **Found nothing.** See finding 1 |
| Part 2, audio | `backend: miniaudio PulseAudio, 48000 Hz (4x 12000 Hz), 300-sample block, ptt none` -- no rate refusal |
| Part 2, level meter | Works, and the operator called it the most useful thing on the page |
| Part 3, `cm108:` | Opens, writes, reports no fault, **does not key.** See finding 3 |
| Part 3, `civ:` | **Keys and unkeys**, at `9600@56`. See findings 5 and 6 |
| Part 3, `rts:` | **Keys and unkeys**, on the stock cable set with no rewiring. See finding 4 |
| Parts 4 and 5 | Not attempted this session |

---

## Findings

### 1. Detection never worked on Linux at all (fixed)

`ardop_usb_scan` returned **zero nodes on every Linux machine**, so the "Detected
radios" list was always empty and `--detect` always printed "No radios detected"
-- the answer that blames the operator's hardware.

`scan_class` read `/sys/class/<cls>/<name>/device` with `readlink()`. The kernel
stores that link *relative and short*:

```
/sys/class/sound/card0/device  ->  ../../../1-2.2:1.0
```

so the walk got an interface name with no device path above it, `usb_device_of`
found no `bus-port[.port]` component, and every entry of every class was
skipped. Fixed by resolving with `realpath()` (`shell/usbtopo.c:246`).

**Why the tests missed it.** The fixtures build plain directories with a
`usbpath` file, because the reader supports that specifically so tests need not
create a symlink farm. Every test therefore took the hint-file branch and the
symlink branch had no coverage at all. `test_scan_follows_relative_device_links`
now builds real relative symlinks.

### 2. The sound-card match would have paired the wrong card (fixed)

Found while verifying finding 1, and it would have made the fix worse than the
bug. The walk names a card by index (`/dev/card0`); the application selects one
by whatever string the backend renders. The two were matched with
`strstr(devs[j].id, card + 4)` -- that is, **the substring `"0"`** -- which the
first PulseAudio id in the list satisfied. On this machine that was an HDMI
monitor source.

A card index does not appear in a Pulse id at all, so there was nothing to
salvage. Now matched by udev's name for the USB device (manufacturer, product and
serial, spaces turned to underscores), which every Pulse id embeds, or by index
where the backend uses one (`hw:1,0`); monitors are skipped; and an unmatched
candidate is **blanked rather than guessed**, which both callers already render
as "(not resolved)".

### 3. CM108 keying on a DigiRig Mobile is silent and wrong (fixed)

The operator's saved configuration was `cm108:/dev/hidraw2+3`, taken from the
detection suggestion. The C-Media codec in a DigiRig Mobile **does not have its
GPIO bonded to PTT** -- keying is the CP2102N's RTS. The HID write succeeds,
`ardop_ptt_fault` stays clean, and the PTT test reports "keyed and unkeyed. Did
the radio transmit?" while the radio never moves.

The cause is a ranking that answers the wrong question. `ARDOP_USB_SAME_DEVICE`
outranks `ARDOP_USB_SAME_HUB` because it is *certain* -- and it is, about **which
device this is**. It was then read as **which line keys**, and the codec's own
HID interface beat the CP2102N one level up, even though `shell/radios.c` carries
a row for `10c4:ea60` whose own provenance note reads "DigiRig Mobile keys by
RTS". A CM108-family chip presents that HID interface whether or not the pin goes
anywhere.

This also means **CM108 keying remains unverified**: a real dongle was written to
for the first time, the transfer succeeded, and we still do not know whether a
wired pin moves.

**Fixed** by offering every keying interface on a sound card's own hardware
rather than only the best-ranked one (`ardop_usb_pair`). The ranking survives as
an *ordering*; what it no longer does is discard the alternative. This station
now detects as:

```
  C-Media CM108B audio + GPIO
      ptt:   cm108:/dev/hidraw2+3   (from the hardware table)
      pair:  same USB device -- certain

  CP2102 serial bridge (DigiRig Mobile and others)
      ptt:   rts:/dev/ttyUSB0   (from the hardware table)
      pair:  same USB hub -- likely
```

-- the second of which is confirmed keying on this radio.
`test_digirig_offers_both_its_keying_lines` pins the shape, and both renderers
now say that one sound card appearing twice means two keying methods, not two
radios.

### 4. On a DigiRig, the PTT line is in the *audio* cable

**Both keying methods work on this station at once**, using DigiRig's stock Icom
CI-V cable set with no rewiring and no extra cable: `civ:` down the serial cable
into REMOTE, and `rts:` at the same time.

That is worth following through, because it says where the PTT conductor
actually is. REMOTE is a 2-conductor 3.5mm socket -- one CI-V data wire and
ground -- so it cannot carry a handshake line, and the serial cable is the only
thing plugged into it. Since RTS keys the radio anyway, the PTT must arrive
through the **other** cable: the DigiRig drives PTT from RTS on its *audio*
connector, which lands on the ACC socket where the radio's SEND input sits
alongside the audio lines.

So the interface's own wiring, not the radio's, decides which keying methods are
available -- and on this combination the answer is *both*. Reasoning from the
radio's connectors alone gets it wrong, which is how this section first came to
say an extra cable to ACC SEND would be needed. It would not.

The first `rts:` attempt in this session failed only because the cable was
disconnected at the time; nothing was wrong with the keying path. Worth naming
in the hint text all the same, because from the software's point of view an
unwired PTT conductor is indistinguishable from a working one: the line toggles,
the ioctl succeeds, and whether anything transmits is decided somewhere the
program cannot see.

### 5. The wrong CI-V rate is indistinguishable from a dead cable

The radio runs CI-V at **9600**; `ptt.c` defaults to **19200** (right for Xiegu,
which is why it is the default). At the wrong rate the radio simply does not
answer -- the exact failure `ptt.c:394` warns about in its own comment, now
observed.

Two usability consequences, neither of them in the code's control:

- The working spec is `civ:/dev/ttyUSB0:9600@56`, and that string has to be
  **typed into a field labelled "Port"**. The CI-V hint mentions `@a4` and `@94`
  but never that a rate can be given, let alone that this radio needs one.
- The address was not the model default either. `19 00` addressed to the
  broadcast address answered in one exchange:

  ```
  FE FE E0 56 19 00 66 FD      from 0x56; payload 0x66
  ```

  The **bus address is 0x56**; `0x66` is the factory model id for the 746PRO
  family, so this radio's address had been moved at some point. Addressing 0x66
  got echo and silence. A read-only probe of this shape identifies the radio, its
  address *and* its rate in about two seconds, which is more than the USB tree
  can ever say.

### 6. CI-V keying costs ~32 ms per transition (measured)

25 samples, `03` (read frequency) at 9600, which puts the same shape of frame on
the wire as a keying command:

```
min 31.3 ms   median 31.9 ms   max 32.4 ms
```

`cat_set` blocks the modem thread for about that on **every key and every
unkey** -- roughly 64 ms per over -- because it waits for the radio's `FB` ack.
That wait should stay: it is what turns a dead CAT link into a fault instead of a
silent no-transmission, which is precisely the failure mode of finding 3. But it
is a real cost, and it is the measured argument for preferring a serial keying
line where one exists: a `TIOCMSET` is a single USB control transfer, and it does
not share a bus with rig control.

Most of the 32 ms is USB buffering and the radio's turnaround rather than wire
time, so raising the CI-V rate would barely dent it.

### 7. A killed `rigctld` left the transmitter keyed

The one failure 19 puts ahead of everything else, and it happened. Recorded in
full because the cause is not where it looks.

```sh
rigctld -m 3046 -r /dev/ttyUSB0 -s 9600 -c 0x56
```

The radio **keyed on startup, before any command was sent**, and **stayed keyed
after `rigctld` was killed**. It was cleared by unplugging the interface.

The rig model was irrelevant -- `-m 3023` (IC-746) would have done the same.
**Hamlib raises RTS and DTR when it opens a serial port**, because many homebrew
CI-V level converters are powered from those lines. On a DigiRig, RTS *is* PTT
(finding 4), so opening the port keys the radio.

Why it survived the kill is the part that reaches our code. **The modem lines
fall on close only if `HUPCL` is set**, and a program that installs its own
termios can leave it clear. Then the process dies, nothing lowers the line, and
the transmitter stays on the air with no software left to stop it.

`ardop_ptt`'s `serial_open` did not configure termios at all for `rts:`/`dtr:` --
the comment reasoned that keying never touches the data lines, which is true and
beside the point. It therefore **inherited whatever the previous program left on
the port**, including a cleared `HUPCL`, and `ptt.h`'s promise that "a serial
line falls when the handle closes" was enforced by nothing. It now sets `HUPCL`
explicitly, and clears `CRTSCTS` while it is there: with hardware flow control
on, the cp210x driver owns RTS and silently ignores `TIOCMSET`, which is keying
that reports success and never happens.

Verified on the port, which was deliberately left in the dangerous state:

```
before:                -hupcl
after our open+close:   hupcl
```

To drive this station from `rigctld` at all, its line states have to be turned
off, and the radio should be off or disconnected the first time:

```sh
rigctld -m 3023 -r /dev/ttyUSB2 -s 9600 -c 0x56 \
        --set-conf=rts_state=OFF,dtr_state=OFF
```

With that, **the rigctld path is confirmed end to end**: the command keys and
unkeys the radio on startup as hamlib probes it, and `ardop-station` configured
with `rigctld:127.0.0.1:4532` keys and unkeys correctly. That is the last of
19's four "never met hardware" rows, and the only one whose byte exchange could
not be tested in process at all -- it needs a server answering while `open()`
blocks.

### 9. Two things the devices page does not do for an operator trying methods

Both reported while switching between keying methods in sequence, which is the
whole activity of setting up a radio and is therefore the case the page should
be best at.

> the user needs to press apply before test. that's confusing -- I would expect
> test to test the currently displayed values.

*Test PTT* keys through the device the modem thread has **open**, so an
unapplied change is silently not what gets tested. What the operator sees is
"PTT test: no device is open", which neither says "press Apply first" nor hints
that the two fields directly above the button are being ignored.

> when I change the keying method it should probably switch the port to whatever
> is the most obvious first. when I switched to rigctld, the port stayed as the
> usb until I changed the dropdown.

`fillPttTargets` preserves the previous text across a method change, so choosing
rigctld leaves a `/dev/ttyUSB2` in a field that now wants `host:port`. Keeping
what was typed is right when the same method's list is refreshed and wrong when
the method changed underneath it; the two intents share one code path.

Written up as user stories in [16](16-user-interface.md) §11 -- FU-1 and FU-4
for these two, FU-2 and FU-3 for the CAT-syntax and stable-port-name findings
above. FU-1 carries the constraint that makes it cheap to build: keying and
audio are independent, so testing a displayed spec need not open a sound card at
all.

### 8. A replug renumbered the port and invalidated the saved configuration

The interface came back as `/dev/ttyUSB2`, and `station.conf` still said
`/dev/ttyUSB0` -- which by then did not exist. `/dev/serial/by-id/` carries the
CP2102N's serial number and does not move:

```
ptt.spec=rts:/dev/serial/by-id/usb-Silicon_Labs_CP2102N_..._-if00-port0
```

`ardop_ptt_parse` takes that path as-is, so nothing needed to change to support
it -- but the picker offers only `/dev/ttyUSBn`, so an operator has no way to
choose the stable name from the interface.

### 10. The status bar announced a connection that had not happened (fixed)

> when attempting a connection the status bar reads 'connected to CALLSIGN' -
> it should display a message saying it's attempting to connect

`onLinkState` decided what to say from **whether a remote callsign was known**,
not from the link state. A callsign is known from the moment the operator asks
to call one, and `ARDOP_LINK_ISS_CON_REQ` -- ConReq sent, ConAck awaited -- runs
for the whole calling sequence, retries included. So the bar claimed a link
during precisely the interval an operator is watching it to find out whether
there is one.

The cause underneath is that two independent facts shared one `QLabel`: audio
status writes it (`onConnectionChanged`), and the link wrote it too. The link
half could only avoid clobbering the audio text by leaving whatever was already
there when it had no callsign -- which is also why "connected to X" **outlived
the connection**, since a disconnect carries no callsign and so changed nothing.
One reported symptom, two wrong readings, one cause.

Now the link has its own status-bar label and reads the state: nothing when
`DISC`, "calling X..." during `ISS_CON_REQ`, "connected to X" otherwise.

Worth noting where this could *not* be fixed. `ardop_host_state_name` collapses
`ISS_CON_REQ` into `"ISS"` because that is the host protocol's vocabulary, which
Winlink Express and Pat parse; the station page shows that name and should keep
showing it. The distinction the operator wanted exists in the link state and
only ever needed reading, so the fix belongs in the interface and nowhere near
`shell/host.c`.

---

## What is still unverified

19's table, corrected by this session:

| | Status after session 1 |
|---|---|
| **CM108 GPIO keying** | A real dongle was written to; the pin was not wired, so nothing is proven. **Still open** |
| **Native CAT keying** | CI-V confirmed against an IC-746PRO. Kenwood and Yaesu still open |
| **The rigctld byte exchange** | **Confirmed.** `rigctld:127.0.0.1:4532` keys and unkeys this station from `ardop-station`; the first attempt keyed the radio on startup and never got as far as our code, which is finding 7 |
| **USB device detection** | Run on real hardware; was broken; fixed and re-run. Windows reader still not written |

And, unchanged and more important than any of the above: the unkey ordering in
`ma_set_ptt` (the drain that keeps the tail of an over from being cut), the
`Ctrl-C`-always-unkeys property, two-station decode, and live interoperability
with `ardopcf`. **A keying test proves the line, not the timing.**

---

## Follow-ups, and where each lands

| | Where it goes |
|---|---|
| ~~Offer *every* keying sibling, not only the best-ranked one~~ | **Done**, session 1: `ardop_usb_pair`, and both renderers |
| ~~"On a DigiRig the PTT line is in the audio cable" in the hint text~~ | **Done**, session 1: `app/ui/devicespage.cpp`, `kMethods` |
| A read-only CI-V probe during detection, reporting address and rate | `shell/`, new; finding 5. It answers what the USB tree cannot |
| Rate and address as their own fields for the CAT methods | `app/ui/devicespage.cpp`; a spec typed into "Port" is not discoverable |
| Offer `/dev/serial/by-id/` paths in the serial picker, preferring them | `shell/serialports.c`; finding 8. A `ttyUSBn` saved today is a different device tomorrow |
| Keep the diagnostics | Four throwaway programs were written this session -- line toggle, CAT probe, address scan, latency. A `--probe` in the application would have found the whole configuration in one command |

---

## Method note

Everything in session 1 was established on the operator's own machine: `lsusb`
and `sysfs` for the topology, `TIOCMGET`/`TIOCMSET` for the serial lines, and
read-only CI-V commands for the radio. Nothing was keyed by the assistant --
every transmission in this log was initiated by the operator, at the radio, into
a load they had checked.
