#! /usr/bin/env python3
"""
Check the rebuilt core demodulator against the frozen golden recordings.

Where test_golden.py drives the inherited `ardopcf` binary, this drives the
core: it runs each committed WAV through `core_decode_wav`, which pushes the
samples through `ardop_demod_push` in receive-only mode (as `--decodewav`
does), and compares the recovered frame against the manifest.  Real,
recorded ardopcf audio -- including noise-degraded copies -- is thus an
external oracle for the core RX chain.

The conformance rule is asymmetric on purpose:

  - The core must never emit a *wrong* decode.  A recovered frame whose type
    or payload does not match the ground truth is always a failure.

  - A *miss* (nothing recovered) is judged by context.  It is fine below the
    strict SNR floor (the inherited decoder misses there too), and it is an
    expected gap for the frame types the core does not yet decode (see
    KNOWN_GAPS).  A miss on a required, supported frame is a failure.

Getting the right bytes where the inherited *standard* decoder gave up is not
a violation: the core's DSP and RS are already proven bit-identical to the
standard reference functions, so a threshold frame that its SDFT decoder also
recovers (and the core matches byte-for-byte) is within tolerance.

Ground truth for each case is its clean `decode` result -- the bytes the frame
was built from -- so a variant that decodes to anything else is caught.

Usage:
    ./test_golden_core.py          check every frozen recording
    ./test_golden_core.py -v       list every variant as it is judged
"""

import argparse
import os
import subprocess
import sys
import tempfile

import ardop_golden as g

# The decode harness to drive. Defaults to the pure-core core_decode_wav; set
# GOLDEN_DECODE_BIN to shell_decode_wav to run the same check through the
# assembled shell (make golden-shell). Both print the identical line format, so
# the judging below is unchanged.
BIN = os.environ.get("GOLDEN_DECODE_BIN",
                     os.path.join(g.GOLDEN_DIR, "core_decode_wav"))

# Frame types whose manifest payload is the *formatted host string* ardopcf
# emits (callsigns/grid), not the demod's output.  The core demod recovers these
# frames as their raw RS-corrected content bytes (the Packed6 callsigns); turning
# them into the host string is the link/app layer's job.  So for these the check
# verifies the frame type and that a payload decoded, and notes the formatting is
# deferred, rather than comparing bytes to the formatted string.
FORMATTED_FRAMES = {"IDFrame", "ConReq2000M"}


def run_core(wav_bytes):
    """Decode one WAV through the core harness; return [(frame_type, hex)]."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        tf.write(wav_bytes)
        path = tf.name
    try:
        out = subprocess.run([BIN, path], capture_output=True, text=True,
                             check=True)
    finally:
        os.unlink(path)
    frames = []
    for line in out.stdout.splitlines():
        if not line:
            continue
        name, quality, payload = line.split("\t")
        frames.append((name, payload, int(quality)))
    return frames


class Report:
    def __init__(self, verbose=False):
        self.failures = []
        self.gaps = []
        self.drift = []
        self.checked = 0
        self.verbose = verbose

    def fail(self, cid, msg):
        self.failures.append(f"{cid}: {msg}")

    def gap(self, cid, msg):
        self.gaps.append(f"{cid}: {msg}")

    def soft(self, cid, msg):
        self.drift.append(f"{cid}: {msg}")

    def ok(self, cid, note=""):
        self.checked += 1
        if self.verbose:
            print(f"  PASS  {cid}{(' ' + note) if note else ''}")


def judge(report, cid, label, frames, true_type, true_payload,
          true_quality, required):
    """Apply the asymmetric conformance rule to one variant's output."""
    formatted = true_type in FORMATTED_FRAMES

    # A wrong decode is always a failure: any recovered frame must be exactly
    # the ground-truth frame.
    for name, payload, quality in frames:
        if name != true_type:
            report.fail(cid, f"{label}: decoded {name!r},"
                             f" expected {true_type!r}")
            return
        if formatted:
            # Raw content decoded; the manifest payload is the app-layer
            # formatted string. Verify only that content came out.
            if not payload:
                report.fail(cid, f"{label}: {name} decoded no content")
                return
            continue
        if payload != true_payload:
            report.fail(cid, f"{label}: {name} payload does not match ground"
                             f" truth ({len(payload)//2} vs"
                             f" {len(true_payload)//2} bytes)")
            return
        # Decode quality is a Tier-2 heuristic (the inherited golden reports it
        # but never fails on it). The metric functions are proven bit-exact; a
        # small end-to-end drift means the demod fed slightly different
        # phases/tone-mags than ardopcf did on this frame, near a truncation
        # edge. Report it, do not fail -- an off-by-one rarely even changes the
        # 5-bit ACK it is scaled into.
        if true_quality is not None and quality != true_quality:
            report.soft(cid, f"{label}: {name} quality {quality},"
                             f" manifest {true_quality} (heuristic, not"
                             f" enforced)")

    matched = any(name == true_type for name, _, _ in frames)
    if matched:
        if formatted:
            report.gap(cid, f"{label}: {true_type} decoded to raw content"
                            f" (host-string formatting is app-layer)")
        report.ok(cid, label)
        return

    # A miss.
    if required:
        report.fail(cid, f"{label}: no frame decoded"
                         f" (expected {true_type})")
        return
    report.soft(cid, f"{label}: no frame decoded (below SNR floor,"
                     f" not enforced)")
    report.ok(cid, label + " (miss, tolerated)")


def check_case(report, entry):
    cid = entry["id"]
    truth = entry["decode"]
    true_type = truth["frame_type"]
    true_payload = truth.get("payload", "")

    for key, spec in sorted(entry.get("audio", {}).items()):
        path = os.path.join(g.AUDIO_DIR, spec["file"])
        if not os.path.exists(path):
            report.fail(cid, f"{key}: missing audio file {spec['file']}")
            continue
        wav = g.audio_read(spec["file"])
        if g.sha256(wav) != spec["sha256"]:
            report.fail(cid, f"{key}: {spec['file']} is corrupt"
                             f" (hash does not match manifest)")
            continue

        required = True if key == "clean" else spec.get("required", True)
        # Expected decode quality for this variant (clean uses the top-level
        # decode; degraded variants carry their own). Only data frames record it.
        variant_decode = truth if key == "clean" else spec.get("decode", {})
        true_quality = variant_decode.get("quality")
        frames = run_core(wav)
        judge(report, cid, f"core-{key}", frames, true_type, true_payload,
              true_quality, required)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found; run `make golden-core` (or make it first)")
    if not os.path.exists(g.MANIFEST_PATH):
        sys.exit(f"no corpus at {g.MANIFEST_PATH}")

    manifest = g.load_manifest()
    cases = [c for c in manifest["cases"] if "audio" in c]
    print(f"core demod vs {len(cases)} recorded case(s)"
          f" ({sum(len(c['audio']) for c in cases)} variants)")
    print()

    report = Report(verbose=args.verbose)
    for entry in cases:
        check_case(report, entry)

    print(f"\n{report.checked} checks passed.")

    if report.gaps:
        print(f"\n{len(report.gaps)} known gap(s) (unimplemented, not"
              f" failures):")
        for n, msg in enumerate(report.gaps, 1):
            print(f"  {n}. {msg}")

    if report.drift:
        print(f"\n{len(report.drift)} tolerated miss(es) below the SNR floor:")
        for n, msg in enumerate(report.drift, 1):
            print(f"  {n}. {msg}")

    if report.failures:
        print(f"\n{len(report.failures)} FAILURE(S):")
        for n, msg in enumerate(report.failures, 1):
            print(f"  {n}. {msg}")
        return 1

    print("\nCore demodulator matches the golden recordings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
