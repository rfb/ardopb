#!/usr/bin/env python3
"""
Freeze a slice of a real, field-captured WAV into the golden corpus as a
"field" case.

Unlike the synthetic control/data cases (ardopcf's own modulator, a known
input payload, a checkable TX hash), a field case's provenance is a real
over-the-air recording: there is nothing to re-modulate and hash against
(kind="field" is deliberately not "data", so test_golden_tx.py's
`kind == "data"` filter skips it) and no SDFT comparison to freeze (that
decoder is not part of this rebuild). What a field case *does* buy, that no
synthetic case can: a real channel -- real frequency offset, real fading,
another implementation's transmitter -- with a real, verified outcome. See
README.md's "No real channels" / "Recordings from another implementation
would be strictly better evidence" notes; this is that addition.

Ground truth is not "the bytes the frame was built from" (there is no such
record for someone else's transmission) -- it is what the core demodulator
actually recovers from the slice, which this script requires to be exactly
one frame. That is a sanity check on the *slice boundaries*, not a proof the
recovered bytes are correct; confirm the payload against the session's own
capture.pcap (ardop-pcap-dump) by hand before trusting a new case.

Usage:
    ./add_field_case.py --wav FILE --start N --end N --id field/SLUG \
                         --source "one-line provenance note"
"""

import argparse
import subprocess
import sys
import tempfile
import os

import ardop_golden as g


def decode_slice(wav_bytes):
    """Run one WAV through core_decode_wav; return [(type, quality, hex)]."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        tf.write(wav_bytes)
        path = tf.name
    try:
        out = subprocess.run([g.tool("core_decode_wav"), path],
                             capture_output=True, text=True)
    finally:
        os.unlink(path)
    if out.returncode != 0:
        sys.exit(f"core_decode_wav failed:\n{out.stderr}")
    frames = []
    for line in out.stdout.splitlines():
        if not line:
            continue
        name, quality, payload = line.split("\t")
        frames.append((name, int(quality), payload))
    return frames


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--wav", required=True, help="source recording (12 kHz "
                   "mono 16-bit PCM, e.g. an ardopb --record/--log capture)")
    p.add_argument("--start", type=int, required=True, help="first sample")
    p.add_argument("--end", type=int, required=True,
                   help="one past the last sample")
    p.add_argument("--id", required=True, dest="case_id",
                   help='case id, e.g. "field/va7dep-rms-banner"')
    p.add_argument("--source", required=True,
                   help="one-line provenance: who, when, how captured")
    args = p.parse_args()

    if not os.path.exists(g.tool("core_decode_wav")):
        sys.exit("core_decode_wav not built; run `make golden-core` first")

    with open(args.wav, "rb") as f:
        samples, rate = g.wav_read(f.read())
    if rate != 12000:
        sys.exit(f"{args.wav}: {rate} Hz, expected 12000")
    if not (0 <= args.start < args.end <= len(samples)):
        sys.exit(f"bad range [{args.start}, {args.end}) for "
                 f"{len(samples)} samples")

    wav_bytes = g.wav_write(samples[args.start:args.end], rate=12000)

    frames = decode_slice(wav_bytes)
    if len(frames) != 1:
        sys.exit(f"slice decodes to {len(frames)} frame(s), need exactly 1: "
                 f"{frames}\n(narrow --start/--end to bracket one frame)")
    frame_type, quality, payload = frames[0]

    manifest = g.load_manifest()
    if any(c["id"] == args.case_id for c in manifest["cases"]):
        sys.exit(f"case {args.case_id!r} already exists")

    audio_file = g.audio_name(args.case_id)
    entry = {
        "id": args.case_id,
        "kind": "field",
        "source": args.source,
        "frame_type": frame_type,
        "sample_rate": 12000,
        "decode": {
            "decoded": True,
            "frame_type": frame_type,
            "payload": payload,
            "quality": quality,
        },
        "audio": {
            "clean": {
                "file": audio_file,
                "sha256": g.sha256(wav_bytes),
            }
        },
    }
    manifest["cases"].append(entry)
    manifest["cases"].sort(key=lambda c: c["id"])
    g.audio_write(audio_file, wav_bytes)
    g.save_manifest(manifest)

    print(f"added {args.case_id}: {frame_type} q={quality} "
          f"{len(payload) // 2} byte(s), {args.end - args.start} samples "
          f"({(args.end - args.start) / 12000:.1f}s)")
    print(f"  -> {os.path.join('audio', audio_file)}")


if __name__ == "__main__":
    sys.exit(main())
