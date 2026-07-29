#!/usr/bin/env bash
#
# loopback.sh -- a virtual-audio-cable harness for on-air interop testing.
#
# Creates two 12 kHz mono PulseAudio/PipeWire null sinks that act as one-way
# "RF cables" between two ARDOP stations on the same machine, so the rebuilt
# ardopb can be run against the inherited ardopcf (or another ardopb) over a
# real audio path -- the W3 on-air interop step -- with no radio or sound card.
# Works on native PulseAudio/PipeWire and under WSL (WSLg), where the ALSA
# devices are reached through the ALSA->pulse plugin (needs libasound2-plugins).
#
# Topology (two directed cables):
#
#     station A  --play-->  [ a2b ]  --monitor-->  capture  station B
#     station B  --play-->  [ b2a ]  --monitor-->  capture  station A
#
#   so A: capture=b2a_mon play=a2b ,  B: capture=a2b_mon play=b2a
#   Neither station captures its own transmission (no self-echo).
#
# Usage:
#   tools/loopback.sh up      create the cables + ALSA config; print how to run
#   tools/loopback.sh down    remove the cables
#   tools/loopback.sh demo    up, run ardopb<->ardopb (connect + data), down
#
# For a real interop test, `up`, then in two terminals (with the printed env):
#   ardopcf 8515 b2a_mon a2b -H "MYCALL N0AAA"      # inherited, station A
#   ardopb  N0BBB --listen --alsa a2b_mon b2a       # rebuilt,   station B
# then drive a connect from A's host port. `down` when finished.

set -euo pipefail

CONF="${TMPDIR:-/tmp}/ardop_loopback_asound.conf"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ARDOPB="$HERE/ardopb"

# Locate the system-wide ALSA config to include (varies by distro).
find_alsa_conf() {
	for c in /usr/share/alsa/alsa.conf /etc/alsa/alsa.conf; do
		[ -f "$c" ] && { echo "$c"; return; }
	done
	find /usr -name alsa.conf 2>/dev/null | head -1
}

write_conf() {
	local sys; sys="$(find_alsa_conf)"
	[ -n "$sys" ] || { echo "no system alsa.conf found" >&2; exit 1; }
	cat > "$CONF" <<EOF
<$sys>
pcm.a2b     { type pulse  device "a2b" }
pcm.a2b_mon { type pulse  device "a2b.monitor" }
pcm.b2a     { type pulse  device "b2a" }
pcm.b2a_mon { type pulse  device "b2a.monitor" }
EOF
}

sink_exists() { pactl list short sinks 2>/dev/null | grep -qw "$1"; }

make_cable() {
	local name="$1"
	if sink_exists "$name"; then
		echo "cable $name already present"
	else
		pactl load-module module-null-sink \
			sink_name="$name" rate=12000 channels=1 format=s16le \
			sink_properties=device.description="ardop_$name" >/dev/null
		echo "created cable $name (12 kHz mono)"
	fi
}

drop_cable() {
	local name="$1" id
	id="$(pactl list short modules 2>/dev/null \
		| grep "sink_name=$name" | awk '{print $1}' || true)"
	[ -n "$id" ] && { pactl unload-module "$id"; echo "removed cable $name"; }
}

cmd_up() {
	make_cable a2b
	make_cable b2a
	write_conf
	cat <<EOF

virtual cables ready. Point ALSA at them with:
    export ALSA_CONFIG_PATH=$CONF

then run two stations (A initiates, B answers):
    # A: capture=b2a_mon play=a2b     B: capture=a2b_mon play=b2a
    ardopb N0AAA --alsa b2a_mon a2b --host 8600      # rebuilt A
    ardopb N0BBB --listen --alsa a2b_mon b2a         # rebuilt B
  or against the inherited tree:
    ardopcf 8515 b2a_mon a2b -H "MYCALL N0AAA"       # inherited A
and drive a connect from A's host port (ARQCALL N0BBB 5).
EOF
}

cmd_down() {
	drop_cable a2b
	drop_cable b2a
	rm -f "$CONF"
}

cmd_demo() {
	command -v python3 >/dev/null || { echo "demo needs python3" >&2; exit 1; }
	[ -x "$ARDOPB" ] || { echo "build first: make ardopb" >&2; exit 1; }
	cmd_up >/dev/null
	export ALSA_CONFIG_PATH="$CONF"
	local log; log="$(mktemp -d)"
	echo "launching two ardopb stations over the cables..."
	setsid "$ARDOPB" N0AAA --alsa b2a_mon a2b --host 18600 \
		>"$log/A.log" 2>&1 </dev/null &
	setsid "$ARDOPB" N0BBB --listen --alsa a2b_mon b2a --host 18700 \
		>"$log/B.log" 2>&1 </dev/null &
	sleep 2
	python3 - <<'PY'
import socket, struct, time
ad = socket.create_connection(("127.0.0.1", 18601), timeout=3)   # A data
ac = socket.create_connection(("127.0.0.1", 18600), timeout=3)   # A cmd
payload = b"HELLO OVER THE AIR VIA A VIRTUAL CABLE"
ad.sendall(struct.pack(">H", len(payload)) + payload)
time.sleep(0.3)
ac.sendall(b"ARQCALL N0BBB 5\r")
print("  A queued %d bytes and dialed N0BBB" % len(payload))
ad.close(); ac.close()
PY
	echo "  waiting ~22s for connect + data over audio..."
	sleep 22
	pkill -9 ardopb 2>/dev/null || true
	sleep 0.5
	echo "--- result ---"
	grep -aE "CONNECTED|\[data\]" "$log/A.log" "$log/B.log" || echo "(no connect/data seen)"
	rm -rf "$log"
	cmd_down >/dev/null
	echo "cables removed."
}

case "${1:-}" in
	up)   cmd_up ;;
	down) cmd_down ;;
	demo) cmd_demo ;;
	*)    grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -40; exit 2 ;;
esac
