# `shell/` — the impure program around the core

Everything that opens a device, binds a socket, reads a clock or blocks. It is
where `core/`'s rules stop applying, deliberately: the point of holding the core
to no globals and no allocation is that *this* layer can be as impure as reality
requires without contaminating it.

Two halves that are worth telling apart:

- **The sans-I/O runtime** — `runtime.c`, `loop.c`, `host.c`, `telemetry.c`,
  `ring.c`, `resample.c`, `settings.c`, `fault.c`. Portable C11 with no
  feature-test macros and no OS headers. This is the set the station application
  links (`SHELL_OBJS` in the Makefile).
- **The device and transport layer** — `backend_ma.c`, `ptt*.c`,
  `audio_devices.c`, `usbtopo.c`, `net.c`, `sys.c`, `host_tcp.c`,
  `telemetry_tcp.c`. Each gets exactly one feature-test macro, named exactly once
  in the Makefile.

---

## The rules, and how each one is enforced

Following [`core/README.md`](../core/README.md): **a rule that cannot be
mechanically enforced is written as guidance and labelled as such.**

### 1. Feature-test macros appear in one place — *enforced by the Makefile*

`shell/%.o` builds with none. A file that needs `_DEFAULT_SOURCE` or
`_GNU_SOURCE` gets its own named rule, so the list of impure translation units is
the list of named rules and cannot drift from it.

### 2. One translation unit includes an OS socket header — *guidance*

`net.c`. `net.h` deliberately includes none, typing its handle from `<stdint.h>`
alone, so nothing above it pulls in `<windows.h>`. `sys.h` does the same for the
non-socket platform surface.

### 3. Capture is the only clock — *enforced by `ardop_platform_ops`*

`read_audio`'s return value is what advances time; there is no other source. A
device at 47988 Hz gives a modem clock at 11997 Hz and every protocol deadline
stretches by the same 0.025%, which is the property that dissolved the inherited
tree's `FixTiming`. It is also why a device rate that is not a whole multiple of
12000 is **refused** rather than approximated — see
[`analysis/15`](../analysis/15-platform-audio-and-ptt.md) §5.

### 4. Faults are latched and polled, never thrown — *enforced by the interface*

`ardop_platform_ops` has no error channel: `read_audio` returns a count and
`set_ptt` returns `void`, which is the right shape for a loop that should contain
no policy. So a backend latches (`fault.h`) and whoever owns it polls. The
reaction — abort the session, unkey, tell the operator, offer reselection — is
application policy and lives in `app/`.

### 5. `ardop_audio_enumerate` is not for the modem thread — *guidance*

It opens and closes its own device context and queries each device's native
formats, so it blocks for as long as card probing takes. On the modem thread that
stall would trip the backend's 250 ms capture watchdog and manufacture a device
fault out of a device listing.

---

## Three hazards that do not fail loudly

Each is commented where it is handled; together they are why
`test/core/test_backend_ma.c` exists.

**The transmit tail.** `write_audio` returns when samples are *queued*, not when
they have been played, so unkeying as soon as the loop finishes cuts the last few
hundred milliseconds off every transmission. On top of that the interpolator
holds `(ntaps-1)/2` samples that were accepted and never emitted — a second,
independent cut. So `set_ptt(false)` flushes the resampler *first*, then drains
the ring, then waits one device period plus a guard, and only then drops the
line. `test_transmission_tail_is_not_cut` asserts it in samples rather than by
ear.

**The station hearing itself.** ARDOP is half-duplex and the loop does not call
`read_audio` during transmit, so a capture ring left running quietly banks the
station's own signal and hands it to the demodulator afterwards, stale by the
length of the over. An atomic `tx_active` makes the capture callback drop instead
of enqueue. ALSA escaped this only by accident, via overrun recovery discarding
the backlog.

**A rig keyed over a link that dies.** A serial RTS line falls when the handle
closes; a rig keyed by CAT or rigctld does not, because it never learns the
controller is gone. `ardop_ptt_close` always unkeys first, and
`ardop_install_signal_handlers` runs before any device is opened so that Ctrl-C
can always reach it.

---

## Keying

| Spec | Method |
|---|---|
| `none`, `vox` | Nothing. Always available. |
| `rts:DEV`, `dtr:DEV` | Assert a serial control line. |
| `civ:DEV[@ADDR]` | Icom CI-V, **and every Xiegu**, which emulates it. |
| `kenwood:DEV`, `yaesu:DEV` | `TX;`/`RX;` and `TX1;`/`TX0;`. |
| `cm108[:PATH\|VID:PID\|auto][+N]` | C-Media GPIO over raw HID, pin N (default 3). |
| `rigctld:HOST:PORT` | `T 1`/`T 0` to a running rigctld. |
| `gpio:N` | Recognised and refused; needs libgpiod. |

**The right method is a property of the radio, and getting it wrong is silent.**
A Xiegu or an Icom keys by CAT command and ignores RTS entirely; a DigiRig Mobile
keys by RTS; a DigiRig Lite keys by CM108 GPIO. All three look identically
connected and only one of them transmits. `shell/radios.c` carries what is known
about particular interfaces so the application can suggest the right one.

For an IPv6 rigctld host, bracket it: `rigctld:[::1]:4532`.

### CM108 permissions on Linux

hidraw nodes are `root:root 0600`. Create
`/etc/udev/rules.d/99-ardop-cm108.rules`:

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0d8c", MODE="0660", \
    GROUP="plugdev", TAG+="uaccess"
```

then `sudo udevadm control --reload && sudo udevadm trigger`, and replug.
`TAG+="uaccess"` is enough on systemd systems without joining `plugdev`; the
`GROUP` is the fallback for others.

---

## What has not been run against hardware

Stated plainly, because the alternative is an operator finding out on the air.
[`analysis/19`](../analysis/19-field-testing.md) is the same list turned into
something you can hand to somebody who owns a radio.

- **CM108 keying.** The report bytes, the chip table, the auto-selection policy
  and the sysfs parser are all unit-tested; nothing has been written to a real
  dongle. A write that succeeds proves the report reached the chip, **not that
  the radio keyed** — this hardware has no feedback path, which is why the
  application offers a PTT test and why a human with a receiver is the only real
  confirmation.
- **Native CAT keying.** The frames and replies are pinned byte for byte against
  Icom's published CI-V reference; no radio has answered one here.
- **The rigctld byte exchange.** Covered by inspection only: testing it needs a
  server answering while `open()` blocks, and therefore a thread that `test-core`
  does not have. `test/core/test_ptt.c` says so rather than implying otherwise.
- **USB device detection.** Verified against fabricated sysfs trees, because this
  development machine has no USB audio at all. What that does not prove is that a
  real kernel lays sysfs out the way the fixtures do.
- **Windows device detection.** Returns nothing. Windows groups a physical
  device's functions under a shared Container ID, which is the same idea as the
  Linux ancestor walk with none of the walking; it is a reader to write, not a
  design to redo.

---

## Settings

`$XDG_CONFIG_HOME/ardop/station.conf`, falling back to `$HOME/.config`, and
`%APPDATA%\ardop\station.conf` on Windows. Plain `key=value` with `#` comments.

**Unknown keys are preserved**, which is the rule that makes the format
extensible: load reads everything and save writes everything back, so one part of
the program adding a key cannot delete another's. A missing file is success — first
run is not an error — and a malformed line is skipped and counted rather than
being fatal, because one bad hand edit must not cost an operator their callsign.
