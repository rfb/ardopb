#!/usr/bin/env bash
#
# Assemble the Linux release tarball. Run from the repository root after `make`:
#
#   make
#   tools/package-linux.sh dist
#
# Produces ardopb-linux-$(uname -m).tar.gz -- the station program, the modem and
# the host-client apps -- so ardopb-linux-x86_64.tar.gz on a
# PC and ardopb-linux-aarch64.tar.gz on a 64-bit Raspberry Pi.
#
# Two things make a portable Linux build of this modem unusually easy, and both
# are worth knowing before anyone "improves" the packaging:
#
#   * ardopb links libm and libc and nothing else. miniaudio dlopen()s ALSA,
#     PulseAudio and JACK at run time rather than linking them, so the tarball
#     needs no audio runtime to match the build machine's.
#
#   * Do not link it fully static. Static glibc plus dlopen() is precisely the
#     combination that breaks -- the audio backends would stop being found at
#     run time, on machines that have them installed.
#
# What decides how widely the tarball runs is therefore the glibc of the build
# machine alone. Build on the oldest runner still supported (ubuntu-22.04,
# glibc 2.35) and the floor lands at GLIBC_2.34 -- pthread symbols moved into
# libc there -- which covers Debian 12, Ubuntu 22.04 and later, and Raspberry
# Pi OS bookworm. See .github/workflows/test.yml.
#
# The GUI is not in here. It needs Qt 6, distro Qt is one apt line, and a
# self-contained Qt bundle for Linux is an AppImage-shaped project of its own.

set -euo pipefail

OUT="${1:-dist}"
ARCH=$(uname -m)
NAME="ardopb-linux-$ARCH"

if [ ! -f ardopb ]; then
	echo "package-linux: ardopb not found -- run make first" >&2
	exit 1
fi

rm -rf "$OUT/$NAME" "$NAME.tar.gz"
mkdir -p "$OUT/$NAME"

cp ardopb "$OUT/$NAME/"
cp apps/ardop-cat apps/ardop-chat "$OUT/$NAME/"

# ardop-spine ships too, and it is the reason a first-time operator downloads
# this at all: it is the only binary that can find a radio (--detect), and the
# only one that keeps a device selection across restarts. It is still labelled a
# harness rather than the application, because the graphical program will be a
# different binary -- but the device and keying work it carries is what somebody
# setting a station up for the first time needs.
cp app/ardop-spine "$OUT/$NAME/"

# Stripped here rather than by dropping -g from CFLAGS, so a developer who
# builds locally keeps a debuggable binary.
strip "$OUT/$NAME"/ardopb "$OUT/$NAME"/ardop-cat \
	"$OUT/$NAME"/ardop-chat "$OUT/$NAME"/ardop-spine

# The field-test scripts and the guide that walks through them. Shipped together
# because the guide names the scripts by path and a download that has one without
# the other is a download somebody has to go and complete themselves.
mkdir -p "$OUT/$NAME/scripts"
cp test/app/*.script "$OUT/$NAME/scripts/" 2>/dev/null || true
cp analysis/19-field-testing.md "$OUT/$NAME/FIELD-TESTING.md" 2>/dev/null || true

# A build with no identity is a bug report nobody can act on.
git describe --tags --always --dirty 2>/dev/null > "$OUT/$NAME/VERSION" \
	|| echo "unknown" > "$OUT/$NAME/VERSION"

cp README.md LICENSE "$OUT/$NAME/" 2>/dev/null || true

# Record what this actually demands of the target system, from the binary
# rather than from memory: whoever picks up a bug report about a machine it
# would not start on will want this line and not a claim about it.
{
	echo "built on:  $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") ($ARCH)"
	printf 'glibc:     needs '
	objdump -T "$OUT/$NAME/ardopb" \
		| grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
	echo "links:"
	ldd "$OUT/$NAME/ardopb" | sed 's/^/  /'
} > "$OUT/$NAME/BUILD-INFO.txt"

cat > "$OUT/$NAME/README-LINUX.txt" <<'EOF'
ardopb for Linux
================

  ardop-spine   the station program: finds your radio, remembers the choice
  ardopb        the modem
  ardop-chat    keyboard-to-keyboard chat over a link
  ardop-cat     a raw byte pipe over a link, in either direction

Nothing here needs installing. Copy the files anywhere on your PATH, or run
them from this folder.

These binaries need glibc and nothing else -- see BUILD-INFO.txt for the
exact version floor. ALSA, PulseAudio and JACK are opened at run time if they
are present, so there is no audio package to install to match this build and
no wrong one to uninstall.

The graphical instrument panel is not included: it needs Qt 6, which is far
larger than the modem and already packaged by your distribution. Build it from
source when you want it:

    sudo apt install qt6-base-dev cmake ninja-build
    cmake -S gui -B gui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build gui/build

Quick start
-----------

1. Ask it to find your radio:

       ./ardop-spine --detect

   In the common case a radio is one USB cable carrying both audio and
   keying, so this can work out which serial port belongs to which sound
   card and suggest a keying method. It prints what it found and applies
   nothing.

2. See every sound device, whether or not it was paired:

       ./ardop-spine --list-devices

3. Run the modem:

       ./ardopb MYCALL --audio --ptt civ:/dev/ttyUSB0@a4 --host 8515 --telemetry

   PTT can be:  none                 VOX, or no keying
                rts:/dev/ttyUSB0     assert RTS on a serial port
                dtr:/dev/ttyUSB0     assert DTR instead
                civ:/dev/ttyUSB0@a4  Icom CI-V, and every Xiegu
                kenwood:/dev/ttyUSB0 a Kenwood's own CAT command
                yaesu:/dev/ttyUSB0   a Yaesu's own CAT command
                cm108:auto           a C-Media GPIO dongle
                rigctld:HOST:PORT    key through a running rigctld

   The right one is a property of the radio, and picking wrong fails
   SILENTLY. A Xiegu or an Icom keys by CAT command and ignores RTS
   entirely; a DigiRig Mobile keys by RTS; a DigiRig Lite keys by CM108
   GPIO. All three look identically connected and only one transmits.

   GPIO keying (a Pi header) is still not implemented; run rigctld and key
   through that.

   CM108 on Linux needs a udev rule. If keying fails with a permission
   error the program prints the rule to install.

Please read FIELD-TESTING.md first
----------------------------------

The keying paths in this build have never been run against a real radio.
They are unit-tested down to the byte, and that is not the same thing.

FIELD-TESTING.md walks through it in order of risk -- what the computer
sees, whether audio works, whether it keys into a DUMMY LOAD, and only then
anything on the air -- and says what to send back. If you have a radio and
ten minutes, that document is the most useful thing you can do for this
project.

Use a dummy load for the keying steps. Ctrl-C always unkeys; if you ever
see a transmitter stay keyed after the program exits, please report that
ahead of anything else.

3. If the serial port is refused, your user is not in the group that owns it:

       sudo usermod -aG dialout $USER

   Then log out and back in -- group membership is picked up at login.

PulseAudio and PipeWire
-----------------------

The backend is auto-selected, and on a desktop that usually means PulseAudio
or PipeWire's Pulse shim. If a sound server is adding latency or resampling
behind your back, take it out of the path:

    ./ardopb MYCALL --audio --audio-backend alsa ...

Sound card rates
----------------

The modem runs at 12000 Hz, and the sound card's rate must be a whole
multiple of that: 12000, 24000, 48000 or 96000.

A rate like 44100 is refused with a message rather than resampled. That is
deliberate: in this modem the sample clock *is* the protocol clock, so an
approximate conversion would break the link's timing rather than just the
audio.

Bug reports
-----------

Please include the contents of VERSION and BUILD-INFO.txt (in this folder)
and the modem's console output.
EOF

tar -czf "$NAME.tar.gz" -C "$OUT" "$NAME"
echo "package-linux: wrote $NAME.tar.gz"
ls -la "$OUT/$NAME"
