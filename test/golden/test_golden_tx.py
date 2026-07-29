#! /usr/bin/env python3
"""
Check the rebuilt shell's TRANSMIT audio against the frozen golden corpus.

Where test_golden.py regenerates TX audio from the inherited `ardopcf` binary,
this drives the rebuilt core through `shell_tx_wav`: for every data case in the
manifest it re-encodes the frame (ardop_encode_data_frame) and modulates it with
the same drive level (30) and leader (240 ms) the runtime's start_tx path uses,
writes a WAV in ardopcf's --writetxwav byte format, and compares the file's
SHA-256 to the manifest `tx_sha256`.

A match means the assembled program puts bit-identical audio on the air. This is
the TX half of the W3.1 cutover proof (the RX half is test_golden_core.py /
golden-shell). Control frames are out of scope here: their encoders are not yet
exposed as standalone calls, so only `kind == "data"` cases are checked -- every
data modulation (4FSK / 4PSK / 8PSK / 16QAM, 1..8 carriers, 200..2000 Hz).

Usage:
    ./test_golden_tx.py        check every data case
    ./test_golden_tx.py -v     list every case as it is judged
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

import ardop_golden as g

BIN = os.path.join(g.GOLDEN_DIR, "shell_tx_wav")


def modulate(frame_type, session_id, payload_hex, out_path):
    """Run shell_tx_wav for one case; raise on failure."""
    res = subprocess.run(
        [BIN, frame_type, session_id, payload_hex, out_path],
        capture_output=True, text=True)
    if res.returncode != 0:
        raise g.ArdopError(f"shell_tx_wav failed for {frame_type}:\n"
                           f"{res.stderr}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found; run `make golden-tx` (or `make "
                 f"test/golden/shell_tx_wav`) first")

    manifest = g.load_manifest()
    cases = [c for c in manifest["cases"] if c.get("kind") == "data"]

    passed = 0
    failures = []
    with tempfile.TemporaryDirectory(prefix="ardop-tx-") as tmp:
        out = os.path.join(tmp, "tx.wav")
        for case in cases:
            cid = case["id"]
            modulate(case["frame_type"], case["session_id"],
                     case["expected_payload"], out)
            with open(out, "rb") as f:
                data = f.read()
            sha = hashlib.sha256(data).hexdigest()
            nsamp = (len(data) - 44) // 2
            if sha != case["tx_sha256"]:
                detail = ""
                if nsamp != case["tx_samples"]:
                    detail = (f" (length {nsamp} samples, expected"
                              f" {case['tx_samples']})")
                failures.append(f"{cid}: tx audio differs{detail}")
                if args.verbose:
                    print(f"  BAD {cid}{detail}")
            else:
                passed += 1
                if args.verbose:
                    print(f"  OK  {cid}")

    print(f"\nshell TX vs {len(cases)} data case(s)\n")
    if failures:
        for f in failures:
            print(f"  FAIL {f}")
        print(f"\n{len(failures)} case(s) differ from the golden corpus.")
        sys.exit(1)
    print(f"{passed} checks passed.\n")
    print("Assembled shell transmits bit-identical golden audio.")


if __name__ == "__main__":
    main()
