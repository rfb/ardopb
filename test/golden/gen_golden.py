#! /usr/bin/env python3
"""
Regenerate the golden-vector corpus from the current ardopcf build.

This rewrites `manifest.json` and the contents of `audio/`.  Running it is
how the corpus is *created*; `test_golden.py` is how a build is *checked*
against it.  Keeping those two jobs in separate scripts is deliberate --
a checker that can silently rewrite its own expectations is not a checker.

Usage:
    ./gen_golden.py                 regenerate everything
    ./gen_golden.py --dry-run       report what would change, write nothing

Run this only when you intend to move the baseline, and review the diff:
a change to `tx_sha256` means the modulator's output changed.
"""

import argparse
import datetime
import os
import subprocess
import sys

import ardop_golden as g


def git_commit():
    """Record which source tree produced the corpus, dirty state included."""
    try:
        rev = subprocess.run(
            ["git", "-C", g.REPO_ROOT, "rev-parse", "--short", "HEAD"],
            capture_output=True, check=True).stdout.decode().strip()
        dirty = subprocess.run(
            ["git", "-C", g.REPO_ROOT, "status", "--porcelain", "--", "src", "lib"],
            capture_output=True, check=True).stdout.decode().strip()
        return rev + ("-dirty" if dirty else "")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def build_case(case, keep_audio):
    """Generate one case: modulate, decode both ways, optionally freeze audio."""
    if case["kind"] == "control":
        wav = g.transmit(case["frame_type"], case["txframe_args"])
        expected_payload = None
    else:
        payload = g.xorshift32_bytes(case["payload_seed"], case["payload_len"])
        wav = g.transmit(case["frame_type"], payload.hex(), case["session_id"])
        expected_payload = payload.hex()

    samples, rate = g.wav_read(wav)
    entry = {
        "id": case["id"],
        "kind": case["kind"],
        "frame_type": case["frame_type"],
        "tx_sha256": g.sha256(wav),
        "tx_samples": len(samples),
        "sample_rate": rate,
    }
    if case["kind"] == "control":
        entry["txframe_args"] = case["txframe_args"]
    else:
        entry["payload_seed"] = case["payload_seed"]
        entry["payload_len"] = case["payload_len"]
        entry["capacity"] = case["capacity"]
        entry["session_id"] = f"{case['session_id']:02X}"
        entry["expected_payload"] = expected_payload

    # Decode with both demodulators.  The SDFT demodulator is experimental
    # and is used for 4FSK data and for every frame-type header, so it is a
    # genuinely different path through the receiver.
    entry["decode"] = g.decode(wav, sdft=False)
    entry["decode_sdft"] = g.decode(wav, sdft=True)

    if keep_audio and case["freeze_audio"]:
        entry["audio"] = {}
        name = g.audio_name(case["id"])
        g.audio_write(name, wav)
        entry["audio"]["clean"] = {"file": name, "sha256": g.sha256(wav)}

        noise_seed = (case["payload_seed"] if case["kind"] == "data"
                      else g.case_seed(case["id"]))
        for snr in (g.NOISE_SNR_DB if case["noise_audio"] else []):
            noisy_samples = g.add_awgn(samples, snr, noise_seed)
            noisy = g.wav_write(noisy_samples, rate)
            nname = g.audio_name(case["id"], snr)
            g.audio_write(nname, noisy)
            entry["audio"][f"snr{snr}db"] = {
                "file": nname,
                "sha256": g.sha256(noisy),
                "snr_db": snr,
                # Whether a rebuild is required to reproduce this outcome.
                # Near threshold, decode success is decided by a fraction of
                # a dB, so it is recorded rather than demanded.
                "required": snr >= g.ASSERT_STRICT_SNR_DB,
                "decode": g.decode(noisy, sdft=False),
                "decode_sdft": g.decode(noisy, sdft=True),
            }
    return entry


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true",
                        help="report differences without writing")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(g.APATH):
        sys.exit(f"ardopcf not found at {g.APATH}; run make first")

    old = None
    if os.path.exists(g.MANIFEST_PATH):
        try:
            old = {e["id"]: e for e in g.load_manifest()["cases"]}
        except Exception:
            old = None

    cases = list(g.enumerate_cases())
    entries = []
    changed = []
    failures = []

    for n, case in enumerate(cases, 1):
        if not args.quiet:
            print(f"[{n:3d}/{len(cases)}] {case['id']}", flush=True)
        try:
            entry = build_case(case, keep_audio=not args.dry_run)
        except g.ArdopError as err:
            failures.append(f"{case['id']}: {err}")
            continue
        entries.append(entry)

        if not entry["decode"]["decoded"]:
            failures.append(
                f"{case['id']}: clean signal did not decode"
                f" ({entry['decode'].get('failure')})")
        elif (entry["kind"] == "data"
              and entry["decode"].get("payload") != entry["expected_payload"]):
            failures.append(
                f"{case['id']}: decoded payload does not match encoded")

        if old is not None and case["id"] in old:
            if old[case["id"]].get("tx_sha256") != entry["tx_sha256"]:
                changed.append(case["id"])

    if old is not None:
        gone = sorted(set(old) - {e["id"] for e in entries})
        added = sorted({e["id"] for e in entries} - set(old))
        for i in gone:
            print(f"  REMOVED  {i}")
        for i in added:
            print(f"  ADDED    {i}")
        for i in changed:
            print(f"  TX CHANGED  {i}")

    if failures:
        print(f"\n{len(failures)} problem(s) generating the corpus:")
        for i, f in enumerate(failures, 1):
            print(f"  {i}. {f}")

    manifest = {
        "format_version": g.FORMAT_VERSION,
        "generated": {
            "ardopcf_version": g.ardop_version(),
            "git_commit": git_commit(),
            "date": datetime.date.today().isoformat(),
        },
        "config": {
            "drive_level": g.DRIVE_LEVEL,
            "sample_rate": g.SAMPLE_RATE,
            "strict_snr_db": g.ASSERT_STRICT_SNR_DB,
        },
        "cases": sorted(entries, key=lambda e: e["id"]),
    }

    if args.dry_run:
        print("\n--dry-run: nothing written")
    else:
        g.save_manifest(manifest)
        audio_bytes = sum(
            os.path.getsize(os.path.join(g.AUDIO_DIR, f))
            for f in os.listdir(g.AUDIO_DIR)) if os.path.isdir(g.AUDIO_DIR) else 0
        print(f"\nWrote {len(entries)} cases to {g.MANIFEST_PATH}")
        print(f"Frozen audio: {audio_bytes / 1024:.0f} KiB compressed")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
