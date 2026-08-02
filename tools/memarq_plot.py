#!/usr/bin/env python3
"""
Render the Memory-ARQ measurement charts for analysis/18 as SVG.

SVG rather than PNG because these live in the repository: text, diffable, no
binary blob in the history. The font is the generic `sans-serif` rather than a
stack naming real families: every renderer resolves it, including the ones that
do not implement CSS font fallback and would otherwise drop the labels
entirely. Light and dark variants are emitted separately and
paired with <picture> in the markdown, which is how GitHub does theme-aware
images -- CSS media queries inside an <img>-embedded SVG are not dependable.

Encoding follows the project's data-viz rules:
  - three series, categorical slots 1-3, assigned in fixed order
  - identity is never colour alone: each series also carries its own dash
    pattern and marker, which covers tritanopia (where the dark palette's
    aqua/orange separation is weak), greyscale printing and forced-colors
  - a legend is always present, and the first panel is directly labelled
  - grid and axes are recessive; all text wears ink tokens, never series colour
  - the report ships a table and the raw CSV, which is the relief the light
    surface's aqua contrast (2.74:1) obliges
"""

import argparse
import csv
import os
from collections import defaultdict

# Categorical slots 1-3. Validated all-pairs in both modes.
THEME = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e",
                  grid="#e3e2dd", axis="#b9b8b2",
                  series=["#2a78d6", "#eb6834", "#1baf7a"]),
    "dark": dict(surface="#1a1a19", ink="#ffffff", ink2="#c3c2b7",
                 grid="#333331", axis="#4d4c49",
                 series=["#3987e5", "#d95926", "#199e70"]),
}

# (label, dash, marker) -- the secondary encoding.
def series_labels(copies):
    """Labels carry the copy count, so the chart is readable detached."""
    return [
        ("1 copy", "5 3", "circle"),
        ("%d copies, no averaging" % copies, "1.5 3", "square"),
        ("%d copies, averaged" % copies, "", "triangle"),
    ]


SERIES = series_labels(6)

CHANNEL_LABEL = {
    "awgn": "AWGN",
    "watterson-good": "Watterson good",
    "watterson-moderate": "Watterson moderate",
    "watterson-poor": "Watterson poor",
    "impulsive": "Impulsive (static)",
}


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            r["snr_db"] = float(r["snr_db"])
            for k in ("seeds", "accumulated", "control", "single"):
                r[k] = int(r[k])
            rows.append(r)
    return rows


def marker(kind, x, y, color, surface):
    """A marker with a 2px surface ring, so overlapping series stay readable."""
    if kind == "circle":
        return ('<circle cx="%.1f" cy="%.1f" r="4.5" fill="%s" '
                'stroke="%s" stroke-width="2"/>' % (x, y, color, surface))
    if kind == "square":
        return ('<rect x="%.1f" y="%.1f" width="9" height="9" fill="%s" '
                'stroke="%s" stroke-width="2"/>' % (x - 4.5, y - 4.5, color,
                                                    surface))
    pts = "%.1f,%.1f %.1f,%.1f %.1f,%.1f" % (x, y - 5.2, x + 4.8, y + 3.4,
                                             x - 4.8, y + 3.4)
    return ('<polygon points="%s" fill="%s" stroke="%s" stroke-width="2"/>'
            % (pts, color, surface))


def render(rows, mode, out_path):
    t = THEME[mode]
    modes = sorted({r["mode"] for r in rows})
    channels = [c for c in CHANNEL_LABEL if any(r["channel"] == c
                                                for r in rows)]

    by = defaultdict(list)
    for r in rows:
        by[(r["mode"], r["channel"])].append(r)
    for v in by.values():
        v.sort(key=lambda r: r["snr_db"])

    snrs = sorted({r["snr_db"] for r in rows})
    x0v, x1v = snrs[0], snrs[-1]

    # Geometry
    pad_l, pad_r, pad_t, pad_b = 62, 18, 92, 56
    gap_x, gap_y = 20, 46
    pw = 168
    ph = 132
    W = pad_l + len(channels) * pw + (len(channels) - 1) * gap_x + pad_r
    H = pad_t + len(modes) * ph + (len(modes) - 1) * gap_y + pad_b

    def sx(px, v):
        return px + (v - x0v) / (x1v - x0v) * pw

    def sy(py, frac):
        return py + ph - frac * ph

    o = []
    o.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
             'viewBox="0 0 %d %d" font-family="sans-serif">' % (W, H, W, H))
    o.append('<rect width="%d" height="%d" fill="%s"/>' % (W, H, t["surface"]))

    # Title
    o.append('<text x="%d" y="26" font-size="15" font-weight="600" fill="%s">'
             'Memory ARQ: frames recovered from repeated copies</text>'
             % (pad_l - 44, t["ink"]))
    seeds = rows[0]["seeds"]
    global SERIES
    SERIES = series_labels(int(rows[0]["copies"]))
    o.append('<text x="%d" y="46" font-size="11.5" fill="%s">'
             'Decode success over %s independent noise seeds per point. '
             'Higher is better; the gap between the solid and the dotted line '
             'is what averaging buys.</text>'
             % (pad_l - 44, t["ink2"], seeds))

    # Legend
    lx = pad_l - 44
    for i, (label, dash, mk) in enumerate(SERIES):
        c = t["series"][i]
        o.append('<line x1="%d" y1="66" x2="%d" y2="66" stroke="%s" '
                 'stroke-width="2" %s stroke-linecap="round"/>'
                 % (lx, lx + 26, c,
                    'stroke-dasharray="%s"' % dash if dash else ""))
        o.append(marker(mk, lx + 13, 66, c, t["surface"]))
        o.append('<text x="%d" y="70" font-size="11.5" fill="%s">%s</text>'
                 % (lx + 32, t["ink2"], label))
        lx += 32 + len(label) * 6.4 + 26

    for mi, mname in enumerate(modes):
        for ci, ch in enumerate(channels):
            px = pad_l + ci * (pw + gap_x)
            py = pad_t + mi * (ph + gap_y)
            data = by.get((mname, ch), [])

            # Panel title
            o.append('<text x="%.1f" y="%.1f" font-size="11.5" '
                     'font-weight="600" fill="%s">%s</text>'
                     % (px, py - 8, t["ink"], CHANNEL_LABEL[ch]))

            # Gridlines at 0 / 50 / 100 %
            for frac in (0.0, 0.5, 1.0):
                y = sy(py, frac)
                o.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                         'stroke="%s" stroke-width="1"/>'
                         % (px, y, px + pw, y, t["grid"]))
                if ci == 0:
                    o.append('<text x="%.1f" y="%.1f" font-size="10" '
                             'text-anchor="end" fill="%s">%d%%</text>'
                             % (px - 8, y + 3.5, t["ink2"], int(frac * 100)))

            # X axis
            o.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                     'stroke="%s" stroke-width="1"/>'
                     % (px, sy(py, 0.0), px + pw, sy(py, 0.0), t["axis"]))
            if mi == len(modes) - 1:
                for v in range(int(x0v), int(x1v) + 1, 4):
                    o.append('<text x="%.1f" y="%.1f" font-size="10" '
                             'text-anchor="middle" fill="%s">%d</text>'
                             % (sx(px, v), sy(py, 0.0) + 16, t["ink2"], v))

            # Series
            for si, key in enumerate(("single", "control", "accumulated")):
                c = t["series"][si]
                dash = SERIES[si][1]
                pts = [(sx(px, r["snr_db"]), sy(py, r[key] / r["seeds"]))
                       for r in data]
                if not pts:
                    continue
                d = "M " + " L ".join("%.1f %.1f" % p for p in pts)
                o.append('<path d="%s" fill="none" stroke="%s" '
                         'stroke-width="2" stroke-linejoin="round" '
                         'stroke-linecap="round" %s/>'
                         % (d, c, 'stroke-dasharray="%s"' % dash if dash
                            else ""))
                # Markers every other point, to stay thin.
                for pi, (mx, my) in enumerate(pts):
                    if pi % 2 == 0:
                        o.append(marker(SERIES[si][2], mx, my, c, t["surface"]))

            # Mode label down the left of the row
            if ci == 0:
                o.append('<text x="%.1f" y="%.1f" font-size="11.5" '
                         'font-weight="600" fill="%s" '
                         'transform="rotate(-90 %.1f %.1f)" '
                         'text-anchor="middle">%s</text>'
                         % (px - 42, py + ph / 2, t["ink"], px - 42,
                            py + ph / 2, mname))

    o.append('<text x="%.1f" y="%d" font-size="11" text-anchor="middle" '
             'fill="%s">wideband S/N (dB) &#8212; noise over the full 6 kHz '
             'Nyquist, about 15 dB below an in-band figure</text>'
             % (pad_l + (W - pad_l - pad_r) / 2, H - 14, t["ink2"]))
    o.append('</svg>')

    with open(out_path, "w") as f:
        f.write("\n".join(o))
    return out_path


def threshold(data, key, seeds, level=0.5):
    """S/N at which success first reaches `level`, linearly interpolated."""
    prev = None
    for r in data:
        frac = r[key] / seeds
        if prev is not None and prev[1] < level <= frac:
            (x0, y0), (x1, y1) = prev, (r["snr_db"], frac)
            if y1 == y0:
                return x1
            return x0 + (level - y0) * (x1 - x0) / (y1 - y0)
        prev = (r["snr_db"], frac)
    return None


def gains(rows):
    by = defaultdict(list)
    for r in rows:
        by[(r["mode"], r["channel"])].append(r)
    out = []
    for k, v in by.items():
        v.sort(key=lambda r: r["snr_db"])
        seeds = v[0]["seeds"]
        acc = threshold(v, "accumulated", seeds)
        ctrl = threshold(v, "control", seeds)
        one = threshold(v, "single", seeds)
        out.append(dict(mode=k[0], channel=k[1], acc=acc, ctrl=ctrl, one=one,
                        gain_vs_ctrl=(ctrl - acc) if (acc is not None and
                                                      ctrl is not None) else None,
                        gain_vs_one=(one - acc) if (acc is not None and
                                                    one is not None) else None))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--outdir", default="analysis/img")
    args = ap.parse_args()

    rows = load(args.csv)
    os.makedirs(args.outdir, exist_ok=True)
    for mode in ("light", "dark"):
        p = os.path.join(args.outdir, "memarq-channels-%s.svg" % mode)
        render(rows, mode, p)
        print("wrote", p)

    print("\n%-16s %-20s %8s %8s %8s %9s" % ("mode", "channel", "1copy50",
                                             "ctrl50", "acc50", "gain_dB"))
    for g in sorted(gains(rows), key=lambda g: (g["mode"], g["channel"])):
        fmt = lambda v: ("%.1f" % v) if v is not None else "  --"
        print("%-16s %-20s %8s %8s %8s %9s" % (
            g["mode"], g["channel"], fmt(g["one"]), fmt(g["ctrl"]),
            fmt(g["acc"]), fmt(g["gain_vs_ctrl"])))


if __name__ == "__main__":
    main()
