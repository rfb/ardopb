#!/usr/bin/env python3
"""
Sweep Memory-ARQ decode success against impaired copies. Writes CSV.

For every (channel, mode, S/N, seed) the same impaired copies are decoded
twice -- once letting the demodulator accumulate, once resetting it between
copies -- so any difference is attributable to the averaging and to nothing
else. The copies are generated once and reused for both runs; regenerating
them would leave a doubt that the two arms simply got different noise.

  ./tools/memarq_sweep.py --out results.csv

Takes a few minutes. Feeds analysis/18.
"""

import argparse
import csv
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf_channel as hf

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(REPO, "tools", "memarq_bench")

# 4PSK.200.100.E and 4FSK.200.50S.E: one carrier each, so the phase/tone
# averaging is measured without the separate effect of combining carriers
# recovered from different copies.
MODES = {
    "4PSK.200.100.E": (0x40, 64),
    "4FSK.200.50S.E": (0x48, 16),
}

PAYLOAD = b"Memory ARQ measurement payload -- analysis/18"


def modulate(frame_type, payload_len, path):
    body = (PAYLOAD + bytes(payload_len))[:payload_len]
    subprocess.run(
        [BENCH, "--modulate", hex(frame_type), body.hex(), path],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return np.fromfile(path, dtype=np.int16)


def decode(paths, accumulate):
    cmd = [BENCH, "--decode"]
    if not accumulate:
        cmd.append("--no-accumulate")
    cmd += paths
    out = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return int(out.stdout.strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="memarq_results.csv")
    ap.add_argument("--copies", type=int, default=6)
    ap.add_argument("--seeds", type=int, default=12)
    ap.add_argument("--snr-min", type=float, default=-16.0)
    ap.add_argument("--snr-max", type=float, default=-2.0)
    ap.add_argument("--snr-step", type=float, default=1.0)
    args = ap.parse_args()

    if not os.path.exists(BENCH):
        sys.exit("build it first: make memarq-bench")

    snrs = np.arange(args.snr_min, args.snr_max + 1e-9, args.snr_step)
    rows = []

    with tempfile.TemporaryDirectory() as tmp:
        for mode, (ftype, plen) in MODES.items():
            clean_path = os.path.join(tmp, "clean.raw")
            clean = modulate(ftype, plen, clean_path)

            for channel in hf.CHANNELS:
                for snr in snrs:
                    acc_hits = ctrl_hits = one_hits = 0
                    acc_copy_sum = 0

                    for s in range(args.seeds):
                        base = hash((mode, channel, round(float(snr), 3), s))
                        base &= 0xFFFFFFFF

                        paths = []
                        for c in range(args.copies):
                            y = hf.apply_channel(clean, channel, float(snr),
                                                 base + c * 7919)
                            p = os.path.join(tmp, "c%d.raw" % c)
                            y.tofile(p)
                            paths.append(p)

                        a = decode(paths, True)
                        k = decode(paths, False)
                        # A single reception is the first copy on its own.
                        o = decode(paths[:1], False)

                        if a:
                            acc_hits += 1
                            acc_copy_sum += a
                        if k:
                            ctrl_hits += 1
                        if o:
                            one_hits += 1

                    rows.append(dict(
                        mode=mode, channel=channel, snr_db=round(float(snr), 2),
                        seeds=args.seeds, copies=args.copies,
                        accumulated=acc_hits, control=ctrl_hits,
                        single=one_hits,
                        mean_copy_to_decode=(round(acc_copy_sum / acc_hits, 2)
                                             if acc_hits else ""),
                    ))
                    print("%-16s %-20s %6.1f dB  acc %2d/%d  ctrl %2d/%d  "
                          "1copy %2d/%d" % (
                              mode, channel, snr, acc_hits, args.seeds,
                              ctrl_hits, args.seeds, one_hits, args.seeds),
                          flush=True)

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("\nwrote %s (%d rows)" % (args.out, len(rows)))


if __name__ == "__main__":
    main()
