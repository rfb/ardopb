#!/usr/bin/env bash
#
# loopback.sh -- a virtual-audio-cable harness for on-air interop testing.
#
# Creates two 12 kHz mono PulseAudio/PipeWire null sinks that act as one-way
# "RF cables" between two ARDOP stations on the same machine, so the rebuilt
# ardopb can be run against the inherited ardopcf (or another ardopb) over a
# real audio path -- the W3 on-air interop step -- with no radio or sound card.
#
# ardopb reaches the sinks directly by name, so there is no ALSA config to
# generate and no ALSA_CONFIG_PATH to export. (ardopcf, which only speaks ALSA,
# still needs the plugin route -- see `ardopcf` in the interop note below.)
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
#   tools/loopback.sh up      create the cables; print how to run the stations
#   tools/loopback.sh down    remove the cables
#   tools/loopback.sh check   verify the cable carries audio (beacon -> listener)
#   tools/loopback.sh demo    up, run ardopb<->ardopb (connect + data), down
#   tools/loopback.sh pipe    up, transfer a file with ardop-tx/ardop-rx, down
#
# For a real interop test, `up`, then in two terminals (with the printed env):
#   ardopcf 8515 b2a_mon a2b -H "MYCALL N0AAA"      # inherited, station A
#   ardopb  N0BBB --listen --audio a2b.monitor b2a       # rebuilt,   station B
# then drive a connect from A's host port. `down` when finished.

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
ARDOPB="$HERE/ardopb"

sink_exists() { pactl list short sinks 2>/dev/null | grep -qw "$1"; }

# The cable rate. 12000 means the modem's resampler is the identity path, which
# is what every loopback run has used until now -- so the decimate/interpolate
# chain had never carried an ARQ session over cables at all. 48000 exercises it
# at m = 4, the ratio a real sound card almost always gives.
RATE="${RATE:-12000}"

make_cable() {
	local name="$1"
	if sink_exists "$name"; then
		echo "cable $name already present"
	else
		pactl load-module module-null-sink \
			sink_name="$name" rate="$RATE" channels=1 format=s16le \
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
	echo "cables at ${RATE} Hz (m = $((RATE / 12000)))"
	cat <<'EOF'

virtual cables ready. Run two stations (A initiates, B answers):
    # A: capture=b2a.monitor play=a2b   B: capture=a2b.monitor play=b2a
    ardopb N0AAA --audio b2a.monitor a2b --host 8600      # rebuilt A
    ardopb N0BBB --listen --audio a2b.monitor b2a         # rebuilt B
and drive a connect from A's host port (ARQCALL N0BBB 5).

To run against the inherited ardopcf, which speaks only ALSA, give it the
ALSA->pulse plugin route (needs libasound2-plugins) via an ~/.asoundrc
entry such as `pcm.b2a_mon { type pulse device "b2a.monitor" }`, then:
    ardopcf 8515 b2a_mon a2b -H "MYCALL N0AAA"            # inherited A
EOF
}

cmd_down() {
	drop_cable a2b
	drop_cable b2a
}

cmd_demo() {
	command -v python3 >/dev/null || { echo "demo needs python3" >&2; exit 1; }
	[ -x "$ARDOPB" ] || { echo "build first: make ardopb" >&2; exit 1; }
	cmd_up >/dev/null
	local log; log="$(mktemp -d)"
	echo "launching two ardopb stations over the cables..."
	# --trace so the NO-CONNECT diagnostics (tx/rx frame counts) are meaningful.
	setsid "$ARDOPB" N0AAA --trace --audio b2a.monitor a2b --host 18600 \
		>"$log/A.log" 2>&1 </dev/null &
	setsid "$ARDOPB" N0BBB --listen --trace --audio a2b.monitor b2a --host 18700 \
		>"$log/B.log" 2>&1 </dev/null &
	sleep 3   # let both ALSA devices open + host bind
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
	if grep -aqE "CONNECTED" "$log/A.log" "$log/B.log"; then
		grep -aE "CONNECTED|\[data\]" "$log/A.log" "$log/B.log"
		echo "SUCCESS: connected + data over the virtual cable."
		rm -rf "$log"
	else
		echo "NO CONNECT. Diagnostics below (logs kept in $log):"
		for s in A B; do
			echo "--- station $s ---"
			# backend open line + whether it transmitted / received anything
			grep -aE "backend:|failed|error|Error" "$log/$s.log" | head -3
			echo "  tx frames: $(grep -ac 'tx frame' "$log/$s.log")" \
			     "| rx frames: $(grep -ac 'rx frame' "$log/$s.log")" \
			     "| ptt keys: $(grep -ac 'ptt key' "$log/$s.log")"
			grep -aE "\[host\]|rx frame" "$log/$s.log" | tail -4
		done
		echo
		echo "Read it as: A should show 'tx frames' > 0 (it is sending);"
		echo "B should show 'rx frames' > 0 (audio is crossing the cable)."
		echo "If B's rx frames == 0, the cable is not carrying audio -- run"
		echo "  tools/loopback.sh check"
	fi
	cmd_down >/dev/null
	echo "cables removed."
}

# Pipe demo: transfer a random file with the ardop-tx / ardop-rx apps over the
# cables and verify it arrives bit-exact.
cmd_pipe() {
	local tx="$HERE/apps/ardop-tx" rx="$HERE/apps/ardop-rx"
	[ -x "$ARDOPB" ] || { echo "build first: make ardopb" >&2; exit 1; }
	[ -x "$tx" ] && [ -x "$rx" ] || { echo "build first: make apps" >&2; exit 1; }
	cmd_up >/dev/null
	local d; d="$(mktemp -d)"
	echo "launching two ardopb daemons over the cables..."
	setsid "$ARDOPB" N0AAA --host 8600 --audio b2a.monitor a2b \
		>"$d/A.log" 2>&1 </dev/null &
	setsid "$ARDOPB" N0BBB --listen --host 8700 --audio a2b.monitor b2a \
		>"$d/B.log" 2>&1 </dev/null &
	sleep 3
	head -c 500 /dev/urandom > "$d/in.bin"
	( "$rx" --host 127.0.0.1:8700 > "$d/out.bin" 2>"$d/rx.err" ) &
	local rxpid=$!
	sleep 1
	echo "transferring 500 bytes N0AAA -> N0BBB over audio..."
	timeout 120 "$tx" --host 127.0.0.1:8600 N0BBB < "$d/in.bin" \
		>"$d/tx.err" 2>&1 || true
	wait "$rxpid" 2>/dev/null || true
	pkill -9 ardopb 2>/dev/null || true
	sleep 0.3
	echo "--- result ---"
	if cmp -s "$d/in.bin" "$d/out.bin"; then
		echo "SUCCESS: $(wc -c <"$d/in.bin") bytes transferred bit-exact."
	else
		echo "MISMATCH (in $(wc -c <"$d/in.bin") vs out $(wc -c <"$d/out.bin" 2>/dev/null || echo 0) bytes). Logs in $d:"
		tail -3 "$d/tx.err" "$d/rx.err" 2>/dev/null
		cmd_down >/dev/null
		echo "cables removed."
		return
	fi
	rm -rf "$d"
	cmd_down >/dev/null
	echo "cables removed."
}

# Cable sanity check: one station beacons an ID into a2b, another listens on
# a2b.monitor. If the listener logs a received frame, audio crosses the cable.
# Isolates "is the virtual cable working" from "does the protocol connect".
cmd_check() {
	command -v python3 >/dev/null || { echo "check needs python3" >&2; exit 1; }
	[ -x "$ARDOPB" ] || { echo "build first: make ardopb" >&2; exit 1; }
	cmd_up >/dev/null
	local log; log="$(mktemp -d)"
	echo "beaconing IDs from TX into cable a2b; RX listens on a2b.monitor..."
	# RX first, so it is capturing before TX keys. --trace so the ptt/rx-frame
	# observations this check counts are logged.
	setsid "$ARDOPB" N0RX --listen --trace --audio a2b.monitor b2a \
		>"$log/RX.log" 2>&1 </dev/null &
	sleep 1
	setsid "$ARDOPB" N0TX --trace --audio b2a.monitor a2b --host 18600 \
		>"$log/TX.log" 2>&1 </dev/null &
	sleep 3   # let ALSA open + host bind (slower under WSL)
	# Fire several IDs, spaced, so a single missed frame is not decisive.
	python3 - <<'PY' || true
import socket, time
try:
    s = socket.create_connection(("127.0.0.1", 18600), timeout=3)
    for _ in range(4):
        s.sendall(b"SENDID\r"); time.sleep(1.5)
    s.close()
except Exception as e:
    print("  (could not reach TX host port:", e, ")")
PY
	sleep 2
	pkill -9 ardopb 2>/dev/null || true
	sleep 0.5
	echo "--- result ---"
	grep -aE "backend:|failed" "$log/RX.log" "$log/TX.log" | head -4 || true
	local txk rxf
	txk=$(grep -ac 'ptt key' "$log/TX.log" || true)
	rxf=$(grep -ac 'rx frame' "$log/RX.log" || true)
	echo "TX keyed PTT: $txk time(s) | RX saw: $rxf frame(s)"
	if [ "$txk" -gt 0 ] && [ "$rxf" -gt 0 ]; then
		echo "OK: the virtual cable carries audio (TX -> a2b -> RX)."
	elif [ "$txk" -eq 0 ]; then
		echo "TX never keyed -- did it open the playback device? See:"
		grep -aE "backend:|failed|error" "$log/TX.log" | head || true
	else
		echo "TX transmitted but RX heard nothing: the a2b -> a2b.monitor"
		echo "path is not delivering audio to ardopb on this host."
		echo "Check: pactl list short sinks (a2b present?), and that pactl"
		echo "and ardopb use the SAME server (echo \$PULSE_SERVER)."
	fi
	rm -rf "$log"
	cmd_down >/dev/null
}

case "${1:-}" in
	up)    cmd_up ;;
	down)  cmd_down ;;
	demo)  cmd_demo ;;
	check) cmd_check ;;
	pipe)  cmd_pipe ;;
	*)     grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -40; exit 2 ;;
esac
