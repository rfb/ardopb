#!/usr/bin/env bash
#
# Assemble the Linux release tarball. Run from the repository root after `make`:
#
#   make
#   tools/package-linux.sh dist
#
# Produces ardopb-linux-$(uname -m).tar.gz -- so ardopb-linux-x86_64.tar.gz on a
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
cp apps/ardop-tx apps/ardop-rx apps/ardop-chat "$OUT/$NAME/"

# Stripped here rather than by dropping -g from CFLAGS, so a developer who
# builds locally keeps a debuggable binary.
strip "$OUT/$NAME"/ardopb "$OUT/$NAME"/ardop-tx "$OUT/$NAME"/ardop-rx \
	"$OUT/$NAME"/ardop-chat

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

  ardopb        the modem
  ardop-chat    keyboard-to-keyboard chat over a link
  ardop-tx      pipe a file or stream into a link
  ardop-rx      receive a stream to stdout

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

1. See what sound devices you have:

       ./ardopb --list-devices

2. Run the modem. Use the id or the name printed above; with no device
   arguments it takes the system defaults:

       ./ardopb MYCALL --audio --ptt rts:/dev/ttyUSB0 --host 8515 --telemetry

   PTT can be:  none                 VOX, or no keying
                rts:/dev/ttyUSB0     assert RTS on a serial port
                dtr:/dev/ttyUSB0     assert DTR instead
                rigctld:HOST:PORT    key through a running rigctld

   GPIO and CM108 keying are not implemented yet. On a Pi with a GPIO PTT
   circuit, run rigctld and key through that for now.

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
