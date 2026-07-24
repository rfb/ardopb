#! /usr/bin/env python3
"""
Check the current ardopcf build against the committed golden-vector corpus.

This never writes to the corpus.  It reports, and exits non-zero on any
strict failure, so it can be run from CI unattended.

Three kinds of check are made, and they are not equally binding:

  Tier 1, always enforced
    - the modulator is bit-exact: re-modulating a case reproduces
      `tx_sha256` exactly
    - the receiver is correct: every case decodes, and reports the frame
      type, session ID and payload the manifest records
    - committed audio still decodes correctly, including degraded copies
      at or above the strict SNR floor

  Tier 2, reported but not enforced unless --strict
    - quality scores and Reed-Solomon correction counts.  These are
      heuristics, not protocol; drift in them is worth seeing but is not
      by itself a conformance failure.

  Corpus integrity, always enforced
    - committed audio files hash to what the manifest says, so a corrupted
      or partially checked-out corpus is reported as such rather than
      showing up as hundreds of decode failures.

Usage:
    ./test_golden.py                  Tier 1 + corpus integrity
    ./test_golden.py --strict         also enforce Tier 2
    ./test_golden.py --audio-only     skip re-modulation; check frozen audio
    ./test_golden.py -v               list every case as it passes
"""

import argparse
import os
import sys

import ardop_golden as g


class Report:
    """Collects failures by tier so the summary can distinguish them."""

    def __init__(self, verbose=False):
        self.strict = []
        self.soft = []
        self.verbose = verbose
        self.checked = 0

    def fail(self, case_id, message):
        self.strict.append(f"{case_id}: {message}")

    def drift(self, case_id, message):
        self.soft.append(f"{case_id}: {message}")

    def ok(self, case_id, note=""):
        self.checked += 1
        if self.verbose:
            print(f"  PASS  {case_id}{(' ' + note) if note else ''}")


def check_decode(report, case_id, label, got, expected, expected_payload):
    """
    Compare one decode result against the manifest.

    `expected` is the manifest's record of what this build used to do -- a
    regression baseline.  `expected_payload` is independent ground truth:
    the bytes that were fed to the modulator.  Both are checked, because
    a corpus that only compares against its own recorded output would keep
    passing if the recorded output were wrong to begin with.
    """
    if expected.get("decoded") and not got.get("decoded"):
        report.fail(case_id,
                    f"{label}: expected a decode, got failure"
                    f" ({got.get('failure')})")
        return False
    if not expected.get("decoded"):
        # The manifest records that this case did not decode.  Only report
        # if it now does -- that is an improvement, but still a change.
        if got.get("decoded"):
            report.drift(case_id, f"{label}: now decodes (manifest: failure)")
        return True

    for field in ("frame_type", "session_id", "frame_len"):
        if got.get(field) != expected.get(field):
            report.fail(case_id,
                        f"{label}: {field} {got.get(field)!r},"
                        f" expected {expected.get(field)!r}")
            return False

    if expected_payload is not None and got.get("payload") != expected_payload:
        report.fail(case_id, f"{label}: decoded payload does not match encoded")
        return False

    # Tier 2: heuristics.
    for field in ("quality", "rs_fixed"):
        if field in expected and got.get(field) != expected.get(field):
            report.drift(case_id,
                         f"{label}: {field} {got.get(field)},"
                         f" manifest {expected.get(field)}")
    return True


def check_case(report, entry, audio_only):
    """Run every check for one manifest entry."""
    case_id = entry["id"]
    expected_payload = entry.get("expected_payload")

    if not audio_only:
        # Tier 1: re-modulate and compare bit-for-bit.
        try:
            if entry["kind"] == "control":
                wav = g.transmit(entry["frame_type"], entry["txframe_args"])
            else:
                payload = g.xorshift32_bytes(entry["payload_seed"],
                                             entry["payload_len"])
                wav = g.transmit(entry["frame_type"], payload.hex(),
                                 int(entry["session_id"], 16))
        except g.ArdopError as err:
            report.fail(case_id, f"transmit failed: {err}")
            return

        if g.sha256(wav) != entry["tx_sha256"]:
            samples, _ = g.wav_read(wav)
            detail = ""
            if samples and len(samples) != entry["tx_samples"]:
                detail = (f" (length {len(samples)} samples,"
                          f" expected {entry['tx_samples']})")
            report.fail(case_id, f"modulated audio differs from golden{detail}")
        else:
            report.ok(case_id, "tx")

        # Decode what we just produced, with both demodulators.
        for label, sdft, expected in (("rx", False, entry["decode"]),
                                      ("rx-sdft", True, entry["decode_sdft"])):
            try:
                got = g.decode(wav, sdft=sdft)
            except g.ArdopError as err:
                report.fail(case_id, f"{label}: decode errored: {err}")
                continue
            if check_decode(report, case_id, label, got, expected,
                            expected_payload):
                report.ok(case_id, label)

    # Frozen audio, including degraded copies.
    for key, spec in sorted(entry.get("audio", {}).items()):
        path = os.path.join(g.AUDIO_DIR, spec["file"])
        if not os.path.exists(path):
            report.fail(case_id, f"{key}: missing audio file {spec['file']}")
            continue
        wav = g.audio_read(spec["file"])
        if g.sha256(wav) != spec["sha256"]:
            report.fail(case_id,
                        f"{key}: {spec['file']} is corrupt"
                        f" (hash does not match manifest)")
            continue

        if key == "clean":
            checks = (("audio", False, entry["decode"]),
                      ("audio-sdft", True, entry["decode_sdft"]))
            required = True
        else:
            checks = ((f"audio-{key}", False, spec["decode"]),
                      (f"audio-{key}-sdft", True, spec["decode_sdft"]))
            required = spec.get("required", True)

        for label, sdft, expected in checks:
            try:
                got = g.decode(wav, sdft=sdft)
            except g.ArdopError as err:
                report.fail(case_id, f"{label}: decode errored: {err}")
                continue
            if required:
                if check_decode(report, case_id, label, got, expected,
                                expected_payload):
                    report.ok(case_id, label)
            else:
                # Below the strict SNR floor: record drift only.
                if got.get("decoded") != expected.get("decoded"):
                    report.drift(
                        case_id,
                        f"{label}: decoded={got.get('decoded')},"
                        f" manifest {expected.get('decoded')}"
                        f" (near threshold, not enforced)")
                report.ok(case_id, label)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true",
                        help="treat quality/RS drift as failure")
    parser.add_argument("--audio-only", action="store_true",
                        help="only check committed audio; skip re-modulation")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(g.APATH):
        sys.exit(f"ardopcf not found at {g.APATH}; run make first")
    if not os.path.exists(g.MANIFEST_PATH):
        sys.exit(f"no corpus at {g.MANIFEST_PATH}; run gen_golden.py first")

    manifest = g.load_manifest()
    gen = manifest["generated"]
    now = g.ardop_version()
    print(f"corpus:  {len(manifest['cases'])} cases,"
          f" generated {gen['date']} from ardopcf {gen['ardopcf_version']}"
          f" ({gen['git_commit']})")
    print(f"testing: ardopcf {now}")
    if now != gen["ardopcf_version"]:
        print("         note: version differs from the one that generated"
              " the corpus")
    print()

    report = Report(verbose=args.verbose)
    for entry in manifest["cases"]:
        check_case(report, entry, args.audio_only)

    print(f"\n{report.checked} checks passed.")

    if report.soft:
        print(f"\n{len(report.soft)} Tier 2 difference(s)"
              f"{' (enforced: --strict)' if args.strict else ''}:")
        for n, msg in enumerate(report.soft, 1):
            print(f"  {n}. {msg}")

    if report.strict:
        print(f"\n{len(report.strict)} FAILURE(S):")
        for n, msg in enumerate(report.strict, 1):
            print(f"  {n}. {msg}")
        return 1

    if report.soft and args.strict:
        return 1

    print("\nGolden vectors pass.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
