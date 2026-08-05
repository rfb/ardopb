# 19 — Field testing: what only a radio can tell us

Everything in this project is tested except the parts that need hardware, and
this document is about closing that gap honestly. It is written to be handed to
somebody who owns a radio and does not otherwise work on the code.

The build has a golden corpus, a two-station loopback, a ThreadSanitizer run and
~150 unit tests. None of that proves a radio transmits. Four things are
implemented and have never met real hardware:

| | Why it is unverified |
|---|---|
| **CM108 GPIO keying** | The report bytes and chip table are unit-tested; nothing has been written to a real dongle |
| **Native CAT keying** (CI-V, Kenwood, Yaesu) | Frames pinned against Icom's published reference; no radio has answered one |
| **The rigctld byte exchange** | Testing it needs a server answering while `open()` blocks, and a thread `test-core` does not have |
| **USB device detection** | Verified against fabricated `sysfs` trees; this development machine has no USB audio at all |

**Three of those four have since met hardware.**
[20](20-field-results.md) is the running answer to this document -- what the
first radio said, and what it broke. Detection, the row that looked safest, did
not work on Linux at all. The table above stays as the standing request; 20
records each session against it.

There is also one thing no amount of local testing can establish, and it is the
most important of all: **on-air interoperability with a real `ardopcf` peer**
([13](13-completing-the-rebuild.md) W3). The golden corpus proves the waveform
and the decoder against frozen recordings. It does not prove a live negotiated
session with a foreign implementation.

---

## Safety first, and this is not boilerplate

Two of the things being tested are *keying* paths, and a keying bug transmits.

- **Use a dummy load** for everything up to and including the PTT test. There is
  no reason for any of this to reach an antenna.
- **Turn the power down** before the first on-air step.
- **`Ctrl-C` always unkeys.** The program installs its signal handlers before it
  opens any device, specifically so that this is true. If you ever see a
  transmitter stay keyed after the program exits, **that is the single most
  important bug you can report** — stop, note exactly what you were running, and
  say so.
- CAT keying has a failure mode a serial line does not: a rig keyed by command
  does not unkey when the controller dies, because it never learns the
  controller is gone. That is why `ardop_ptt_close` unkeys before releasing
  anything, and it is the property most worth confirming by hand.

---

## Part 1 — What does the computer see?

Nothing here transmits. It is safe with an antenna connected, though there is no
reason to.

Connect the radio and run:

```sh
./app/ardop-spine --list-devices
./app/ardop-spine --detect
```

**Please send back both outputs verbatim**, plus, on Linux:

```sh
lsusb                      # the whole list
ls -l /dev/serial/by-id/   # if it exists
```

or on Windows, the **Device Manager** entries under *Ports (COM & LPT)* and
*Sound, video and game controllers*.

### What we are hoping to learn

`--detect` pairs a sound card with the keying interface on the same USB
hardware, so that an operator does not have to work out which of `ttyUSB0`,
`ttyUSB1` and `ttyUSB2` is the radio. It handles two shapes:

- a **composite device**, where audio and CAT are two interfaces of one USB
  device — this is what a Xiegu X6200, an IC-7300 or an FT-991A should be;
- an **internal hub**, where they are two separate devices in one shell — a
  DigiRig Mobile.

If `--detect` finds your radio and suggests a keying method, say so. **If it
finds nothing, or suggests the wrong thing, that is more useful still** — it is
the case we cannot reproduce here.

### The specific gap for a Xiegu

`shell/radios.c` has no row for any Xiegu. Hamlib tells us the CI-V address
(`0xa4` for the X6100, X6200 and G90) but not the USB identifier, and this
project's rule is that an unsourced field is left blank rather than guessed.

**The `lsusb` line for the radio closes that gap**, and it becomes a one-line
patch. It looks something like:

```
Bus 003 Device 007: ID 10c4:ea60 Silicon Labs CP210x UART Bridge
                       ^^^^^^^^^ this is what we need
```

---

## Part 2 — Does the audio work?

Still no transmitting.

```sh
./app/ardop-spine --audio <CAPTURE> <PLAYBACK> --script test/app/idle.script
```

using the ids or names `--list-devices` printed. The first line it prints is the
one that matters:

```
backend: miniaudio PulseAudio, 48000 Hz (4x 12000 Hz), 300-sample block, ptt none
```

**Send that line.** It says what rate the device gave and what decimation ratio
the modem is running at.

If instead you get a refusal naming the rate, send that too — it means the
device offered something that is not a whole multiple of 12000 Hz, and the modem
refuses to approximate it because the sample clock *is* the protocol clock
([15](15-platform-audio-and-ptt.md) §5). The fix is usually to set the device's
format to 48000 Hz in the operating system's sound settings, and knowing which
devices need that is useful.

---

## Part 3 — Does it key? (dummy load)

**Dummy load. Low power.** This is the first step that transmits.

Pick the method your interface uses. If `--detect` suggested one, start with
that.

```sh
# A radio with built-in CAT: Icom, and every Xiegu, which emulates it.
./app/ardop-spine --audio <CAP> <PLAY> --ptt civ:/dev/ttyUSB0@a4 \
    --script test/app/idle.script

# ... a Kenwood, or a Yaesu
--ptt kenwood:/dev/ttyUSB0
--ptt yaesu:COM4

# A serial control line: DigiRig Mobile, and most simple interfaces
--ptt rts:/dev/ttyUSB0

# A C-Media GPIO dongle: DigiRig Lite, RA boards, cheap USB interfaces
--ptt cm108:auto
```

On Windows a port above COM9 works written plainly — the program applies the
`\\.\` prefix itself.

The program prints what it opened:

```
ptt: civ:/dev/ttyUSB0@a4
```

### What to report

1. **Did it open at all?** If not, the exact message. Every failure path here is
   supposed to say what to do about it; if one merely says something failed,
   that is a bug worth reporting on its own.
2. **Did the radio key?** Watch the transmitter, and listen on another receiver
   if you have one.
3. **Did it unkey?** Both when the program finished normally, and after `Ctrl-C`
   part-way through.

### CM108 on Linux needs a permission rule

hidraw devices belong to root. If keying fails with a permission error, the
program prints the rule to install; it is also in
[`shell/README.md`](../shell/README.md). Please confirm whether the printed
instructions actually worked — they have never been followed by anybody.

### The CI-V address

For a Xiegu, `@a4`. For most Icoms, `@94` — but it is a per-model setting the
operator can change, so if `@a4` does not work, the radio's menu will say what
it is set to. **If you had to change it, tell us which radio and which address**;
that is a table row.

There is also a broadcast form, `civ:/dev/ttyUSB0` with no `@`, which many rigs
answer. If the explicit address fails and broadcast works, or the other way
round, that is worth knowing.

---

## Part 4 — Does it decode? (still a dummy load)

Two stations, one radio each, is the real test. One station and a receiver is a
useful half of it.

```sh
# Station A -- transmits
./app/ardop-spine --audio <CAP> <PLAY> --ptt <SPEC> --host 8515 \
    --script test/app/idle.script
```

Then from another terminal, drive it over the host port the way Winlink Express
or Pat would:

```sh
apps/ardop-chat 127.0.0.1 8515
```

If you have a second station, have it call the first. A full ARQ connect and a
file transfer is the thing we most want to hear about.

---

## Part 5 — Interoperability with `ardopcf`

The one nothing else here can substitute for -- though [20](20-field-results.md)
session 2 has now exercised the software half of it off the air, over a
virtual-cable loopback rather than a radio: `ardopb` dialing a real `ardopcf`
connects and transfers a file bit-exact. **On-air is still untested**, and so is
the reverse direction cleanly -- session 2's `ardopcf`-dials-`ardopb` run lost
its payload, but so did `ardopcf` talking to itself with no `ardopb` involved,
which points at the test rig's live-audio bridge rather than settling anything
about the protocol. Read that session before repeating the experiment.

If you can run the original `ardopcf` on another machine (or another radio), a
connect and a data transfer **in both directions** would tell us more than
everything else in this document combined. The waveform is checked against a
frozen corpus on every build; a live negotiated session with a foreign
implementation over real RF is not, and until it is, this project should not be
recommended to anyone who cannot debug it.

---

## How to report

Whatever is easiest — an issue, an email, a paste. Useful things, roughly in
order:

1. The radio and interface, by model.
2. The output of `--list-devices` and `--detect`, verbatim.
3. The `lsusb` line (or Device Manager entry) for the radio.
4. The `backend:` line.
5. Which `--ptt` spec you used and whether it keyed and unkeyed.
6. Anything that failed, with the exact message.
7. **Anything that stayed keyed.** Before anything else.

A report saying "`--detect` found nothing and `civ:` did not key, here is the
`lsusb`" is more valuable than one saying it all worked, because the second only
confirms what we hoped.

---

## For whoever picks up the results

Each of these has a specific place to land:

| Result | Where it goes |
|---|---|
| A radio's VID:PID and keying method | a row in `shell/radios.c`, with the report as its provenance |
| A CI-V address that differs | the same row |
| A device rate that was refused | a note in [15](15-platform-audio-and-ptt.md) §5, and possibly a wider accepted set |
| CM108 keying working, or not | strike the "never run against hardware" line in [`shell/README.md`](../shell/README.md), or fix it |
| The five-byte report being wrong | [12](12-normative-accidents.md)'s CM108 entry says what evidence would justify changing it |
| A rig left keyed | a defect, ahead of everything else |
| An `ardopcf` session | [13](13-completing-the-rebuild.md) W3, and the release notes stop saying interop is unvalidated |
