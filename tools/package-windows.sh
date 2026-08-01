#!/usr/bin/env bash
#
# Assemble the Windows release zip. Run from the repository root inside an
# MSYS2 MINGW64 shell, after `make` and after the GUI has been built:
#
#   make
#   cmake -S gui -B gui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build gui/build
#   tools/package-windows.sh dist
#
# Produces dist/ and dist.zip. This is what .github/workflows/test.yml uploads
# and what the rolling `continuous` prerelease serves.

set -euo pipefail

OUT="${1:-dist}"
NAME="ardopb-windows-x86_64"

if [ ! -f ardopb.exe ]; then
	echo "package-windows: ardopb.exe not found -- run make first" >&2
	exit 1
fi

rm -rf "$OUT" "$NAME.zip"
mkdir -p "$OUT"

# The modem and the host-client apps are linked -static (see the Makefile), so
# they are single copyable files with no DLL beside them. That matters for the
# daemon: an operator can drop ardopb.exe anywhere and run it.
cp ardopb.exe "$OUT/"
cp apps/ardop-tx.exe apps/ardop-rx.exe apps/ardop-chat.exe "$OUT/"

if [ -f gui/build/ardop-gui.exe ]; then
	cp gui/build/ardop-gui.exe "$OUT/"

	# windeployqt copies Qt's own DLLs and the platform plugin.
	if command -v windeployqt6 >/dev/null 2>&1; then
		windeployqt6 --release --no-translations --no-system-d3d-compiler \
			--no-opengl-sw "$OUT/ardop-gui.exe"
	elif command -v windeployqt >/dev/null 2>&1; then
		windeployqt --release --no-translations --no-system-d3d-compiler \
			--no-opengl-sw "$OUT/ardop-gui.exe"
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
		before=$(find "$OUT" -maxdepth 1 -type f | wc -l)
		mapfile -t deps < <(
			for f in "$OUT"/*.exe "$OUT"/*.dll; do
				ldd "$f" 2>/dev/null || true
			done | awk '/=> \/(mingw64|clang64)/ {print $3}' | sort -u
		)
		for dll in "${deps[@]}"; do
			[ -f "$dll" ] && cp -n "$dll" "$OUT/" 2>/dev/null || true
		done
		after=$(find "$OUT" -maxdepth 1 -type f | wc -l)
		[ "$before" = "$after" ] && break
	done
	shopt -u nullglob
fi

# A build with no identity is a bug report nobody can act on.
git describe --tags --always --dirty 2>/dev/null > "$OUT/VERSION" \
	|| echo "unknown" > "$OUT/VERSION"

cp README.md LICENSE "$OUT/" 2>/dev/null || true

cat > "$OUT/README-WINDOWS.txt" <<'EOF'
ardopb for Windows
==================

  ardopb.exe        the modem
  ardop-chat.exe    keyboard-to-keyboard chat over a link
  ardop-tx.exe      pipe a file or stream into a link
  ardop-rx.exe      receive a stream to stdout
  ardop-gui.exe     the instrument panel (waterfall, constellation, gauges)

Quick start
-----------

1. See what sound devices you have:

       ardopb.exe --list-devices

2. Run the modem. Use the id or the name printed above; with no device
   arguments it takes the system defaults:

       ardopb.exe MYCALL --audio --ptt rts:COM3 --host 8515 --telemetry

   PTT can be:  none            VOX, or no keying
                rts:COM3        assert RTS on a serial port
                dtr:COM3        assert DTR instead
                rigctld:HOST:PORT   key through a running rigctld

   Ports above COM9 work as written -- the \\.\ prefix is applied for you.

3. Attach the panel:

       ardop-gui.exe --host 127.0.0.1:8517

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

zip -qr "$NAME.zip" "$OUT"
echo "package-windows: wrote $NAME.zip"
ls -la "$OUT"
