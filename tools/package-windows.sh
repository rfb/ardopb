#!/usr/bin/env bash
#
# Assemble the Windows release zips. Run from the repository root inside an
# MSYS2 MINGW64 shell, after `make` and after the GUI has been built:
#
#   make
#   cmake -S gui -B gui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build gui/build
#   tools/package-windows.sh dist
#
# Produces two zips, not one:
#
#   ardopb-windows-x86_64.zip          ~600 KB  the command-line station program,
#                                               the modem and the host-client apps
#   ardop-station-windows-x86_64.zip   ~32 MB   the windowed application, the
#                                               remote panel, and their Qt DLLs
#
# The split is not cosmetic. Every byte over about a megabyte here is Qt and its
# dependency chain -- ICU alone is ~30 MB of locale data -- while the modem and
# all three apps are statically linked and come to under 300 KB zipped. An
# operator running headless behind Winlink Express needs the modem and nothing
# else, and should not download Qt to get it.
#
# This is what .github/workflows/test.yml uploads and what the rolling
# `continuous` prerelease serves.

set -euo pipefail

OUT="${1:-dist}"
MODEM_NAME="ardopb-windows-x86_64"
GUI_NAME="ardop-station-windows-x86_64"

# Under MSYS2 the plain names are the right ones. Overridable so the modem half
# of this script can be smoke-tested from a Linux cross-build:
#
#   make CC=x86_64-w64-mingw32-gcc
#   STRIP=x86_64-w64-mingw32-strip tools/package-windows.sh dist
STRIP="${STRIP:-strip}"

# zip has to be run from inside $OUT so the archive has one top-level folder
# rather than a loose spray of files. Resolve the destination first, so the
# zips land in the working directory whether $OUT was given as a relative path
# or an absolute one -- the workflow uploads them from here by name.
DEST=$PWD

if [ ! -f ardopb.exe ]; then
	echo "package-windows: ardopb.exe not found -- run make first" >&2
	exit 1
fi

rm -rf "$OUT" "$DEST/$MODEM_NAME.zip" "$DEST/$GUI_NAME.zip"
mkdir -p "$OUT/$MODEM_NAME"

# A build with no identity is a bug report nobody can act on. Computed once and
# copied into both zips, so a mixed pair is obvious from the two VERSION files.
VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo unknown)

# Both zips carry the project's own README and licence.
stamp() {
	printf '%s\n' "$VERSION" > "$1/VERSION"
	cp README.md LICENSE "$1/" 2>/dev/null || true
}

# --- the modem ------------------------------------------------------------
#
# These are linked -static (see the Makefile), so they are single copyable
# files with no DLL beside them: an operator can drop ardopb.exe anywhere and
# run it.
#
# Stripping is done here rather than by dropping -g from CFLAGS, so a developer
# who builds locally keeps a debuggable binary. It is worth roughly 3x: the
# four binaries go from 2.6 MB to 756 KB.
cp ardopb.exe "$OUT/$MODEM_NAME/"
cp apps/ardop-tx.exe apps/ardop-rx.exe apps/ardop-chat.exe "$OUT/$MODEM_NAME/"

# ardop-spine ships too, and it is the reason a first-time operator downloads
# this at all: it is the only binary that can find a radio (--detect), and the
# only one that keeps a device selection across restarts.
cp app/ardop-spine.exe "$OUT/$MODEM_NAME/"
"$STRIP" "$OUT/$MODEM_NAME"/*.exe

# The field-test scripts and the guide that walks through them. Shipped together
# because the guide names the scripts by path.
mkdir -p "$OUT/$MODEM_NAME/scripts"
cp test/app/*.script "$OUT/$MODEM_NAME/scripts/" 2>/dev/null || true
cp analysis/19-field-testing.md "$OUT/$MODEM_NAME/FIELD-TESTING.md" 2>/dev/null || true

stamp "$OUT/$MODEM_NAME"

cat > "$OUT/$MODEM_NAME/README-WINDOWS.txt" <<'EOF'
ardopb for Windows
==================

  ardop-spine.exe   the station program: finds your radio, remembers the choice
  ardopb.exe        the modem
  ardop-chat.exe    keyboard-to-keyboard chat over a link
  ardop-tx.exe      pipe a file or stream into a link
  ardop-rx.exe      receive a stream to stdout

Nothing here needs installing and nothing needs a DLL beside it. Copy the
files anywhere and run them.

The windowed application -- device pickers, waterfall, constellation, gauges --
is a separate download, ardop-station-windows-x86_64.zip, because it carries
the Qt libraries and is about fifty times the size of this one. You do not need
it to run a link, and ardop-spine.exe does the same setting-up from a command
line.

Quick start
-----------

1. Ask it to find your radio:

       ardop-spine.exe --detect

   In the common case a radio is one USB cable carrying both audio and
   keying. Detection is not implemented on Windows yet, so this will report
   nothing for now -- use --list-devices and pick by hand.

2. See what sound devices you have:

       ardop-spine.exe --list-devices

3. Run the modem. Use the id or the name printed above; with no device
   arguments it takes the system defaults:

       ardopb.exe MYCALL --audio --ptt civ:COM3@a4 --host 8515 --telemetry

   PTT can be:  none            VOX, or no keying
                rts:COM3        assert RTS on a serial port
                dtr:COM3        assert DTR instead
                civ:COM3@a4     Icom CI-V, and every Xiegu
                kenwood:COM3    a Kenwood's own CAT command
                yaesu:COM3      a Yaesu's own CAT command
                cm108:auto      a C-Media GPIO dongle
                rigctld:HOST:PORT   key through a running rigctld

   The right one is a property of the radio, and picking wrong fails
   SILENTLY. A Xiegu or an Icom keys by CAT command and ignores RTS
   entirely; a DigiRig Mobile keys by RTS; a DigiRig Lite keys by CM108
   GPIO. All three look identically connected and only one transmits.

   Ports above COM9 work as written -- the \\.\ prefix is applied for you.

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

Sound card rates
----------------

The modem runs at 12000 Hz, and the sound card's rate must be a whole
multiple of that: 12000, 24000, 48000 or 96000. 48000 is the usual Windows
setting and needs nothing changed.

A rate like 44100 is refused with a message rather than resampled. That is
deliberate: in this modem the sample clock *is* the protocol clock, so an
approximate conversion would break the link's timing rather than just the
audio. If you see that message, set the device's format to 48000 Hz, 16-bit,
mono or stereo, in Sound Control Panel -> device -> Properties -> Advanced.

Antivirus
---------

These are unsigned binaries from a continuous build, so SmartScreen may warn
about them. Check the SHA-256 against the release page if that matters to you.

Bug reports
-----------

Please include the contents of VERSION (in this folder) and the modem's
console output.
EOF

(cd "$OUT" && zip -qr "$DEST/$MODEM_NAME.zip" "$MODEM_NAME")
echo "package-windows: wrote $MODEM_NAME.zip"

# --- the windowed application ---------------------------------------------
#
# Both Qt programs go in one download because they need the same 30 MB of Qt
# DLLs, and shipping that twice to give somebody two 1 MB executables would be a
# strange trade. ardop-station is the application; ardop-gui is the standalone
# panel for watching a modem on another machine, and survives until --remote
# makes it a mode of the same binary.
if [ -f app/ui/build/ardop-station.exe ] || [ -f gui/build/ardop-gui.exe ]; then
	mkdir -p "$OUT/$GUI_NAME"
	[ -f app/ui/build/ardop-station.exe ] \
		&& cp app/ui/build/ardop-station.exe "$OUT/$GUI_NAME/"
	[ -f gui/build/ardop-gui.exe ] \
		&& cp gui/build/ardop-gui.exe "$OUT/$GUI_NAME/"
	"$STRIP" "$OUT/$GUI_NAME"/*.exe

	# windeployqt copies Qt's own DLLs and the platform plugin.
	WDQ=""
	command -v windeployqt6 >/dev/null 2>&1 && WDQ=windeployqt6
	[ -z "$WDQ" ] && command -v windeployqt >/dev/null 2>&1 && WDQ=windeployqt
	if [ -n "$WDQ" ]; then
		# Once per executable, into the same folder. They share almost
		# every DLL, so the second run is nearly a no-op.
		for exe in "$OUT/$GUI_NAME"/*.exe; do
			"$WDQ" --release --no-translations \
				--no-system-d3d-compiler --no-opengl-sw "$exe"
		done
	else
		echo "package-windows: windeployqt not found; the GUI will not" >&2
		echo "                 run on a machine without Qt installed" >&2
	fi

	# windeployqt handles Qt but is unreliable about MinGW's own runtime and
	# Qt's transitive dependencies -- libgcc_s_seh-1, libstdc++-6,
	# libwinpthread-1, and the zlib/pcre2/harfbuzz/freetype chain. Sweep with
	# ldd, which reads PE under MSYS2, until nothing new appears. Missing one
	# of these is the classic "works on the build machine only" failure.
	#
	# Every step is failure-tolerant on purpose: ldd exits non-zero on a file
	# it cannot parse, and under `set -e` that would abort the packaging run
	# rather than the copy it actually affects.
	shopt -s nullglob
	for _pass in 1 2 3 4; do
		before=$(find "$OUT/$GUI_NAME" -maxdepth 1 -type f | wc -l)
		mapfile -t deps < <(
			for f in "$OUT/$GUI_NAME"/*.exe "$OUT/$GUI_NAME"/*.dll; do
				ldd "$f" 2>/dev/null || true
			done | awk '/=> \/(mingw64|clang64)/ {print $3}' | sort -u
		)
		for dll in "${deps[@]}"; do
			[ -f "$dll" ] && cp -n "$dll" "$OUT/$GUI_NAME/" 2>/dev/null || true
		done
		after=$(find "$OUT/$GUI_NAME" -maxdepth 1 -type f | wc -l)
		[ "$before" = "$after" ] && break
	done
	shopt -u nullglob

	stamp "$OUT/$GUI_NAME"

	cat > "$OUT/$GUI_NAME/README-WINDOWS.txt" <<'EOF'
ardop station for Windows
=========================

  ardop-station.exe   the application: a modem, its devices, and a window
  ardop-gui.exe       a read-only panel for watching a modem on another machine

Keep the DLLs in this folder next to them. They are Qt and its dependencies,
and neither program will start without them.

Start here
----------

Run ardop-station.exe. It opens whatever devices it used last time, or the
system default on a first run, and the Devices tab is where you change that.

  Panel     the waterfall, constellation, meters and the modem's own log
  Devices   pick a sound card and a way to key the radio

On the Devices tab, "Detected radios" tries to work out which serial port
belongs to your radio by looking for a keying interface on the same USB
hardware as a sound card. Detection is not implemented on Windows yet, so it
will say so; use the pickers underneath.

Choosing anything only fills the fields in. Apply opens the devices. Test PTT
is the only control that puts a signal on the air.

Before you key anything
-----------------------

The keying paths in this build have NEVER been run against a real radio.
They are unit-tested down to the byte, which is not the same thing.

Use a dummy load. Turn the power down. Ctrl-C and closing the window both
unkey; if you ever see a transmitter stay keyed after the program exits,
please report that ahead of anything else.

The right keying method is a property of the radio, and picking the wrong one
fails SILENTLY. A Xiegu or an Icom keys by CAT command and ignores RTS
entirely; a DigiRig Mobile keys by RTS; a DigiRig Lite keys by CM108 GPIO. All
three look identically connected and only one of them transmits.

FIELD-TESTING.md in the other download walks through it in order of risk and
says what to send back.

Watching another station
------------------------

ardop-gui.exe is the standalone panel. It contains no modem and cannot key a
radio -- it connects to a running ardopb somewhere and draws what that modem
reports:

    ardopb.exe MYCALL --audio --host 8515 --telemetry     (on the other machine)
    ardop-gui.exe --host 192.168.1.20:8517                (here)

Antivirus
---------

These are unsigned binaries from a continuous build, so SmartScreen may warn
about them. Check the SHA-256 against the release page if that matters to you.

Bug reports
-----------

Please include the contents of VERSION (in this folder) and whatever the
Panel tab's log said.
EOF

	(cd "$OUT" && zip -qr "$DEST/$GUI_NAME.zip" "$GUI_NAME")
	echo "package-windows: wrote $GUI_NAME.zip"
else
	echo "package-windows: gui/build/ardop-gui.exe not found; skipping the" >&2
	echo "                 GUI zip. Build it with cmake first." >&2
fi

ls -la "$OUT"/*/
