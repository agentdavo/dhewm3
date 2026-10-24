#!/usr/bin/env python3
"""Compare FrameBin on/off frame cost across interleaved rounds.

Reports the median of [rprof] total (which now INCLUDES the mt= parallel bucket)
per arm per round, plus a matched comparison restricted to frames with a similar
draws/tris signature -- because "equal frags" has already produced one retracted
claim on this project, and a median over whatever frames a run happened to emit
mixes cheap UI frames with expensive ones.

Per-round figures are printed separately and NOT pooled: if the two rounds
disagree, that disagreement is the error bar, and pooling would hide it.
"""
import re
import statistics
import sys
from collections import defaultdict

RPROF = re.compile(
    r"^\[rprof\] draws=(\d+) tris=(\d+) frags=(\d+).*?"
    r"vertex=([\d.]+)ms\s+dispatch=([\d.]+)ms\s+raster=([\d.]+)ms\s+"
    r"(?:mt=([\d.]+)ms\s+)?present=([\d.]+)ms\s+total=([\d.]+)ms")


def load(path):
    rows = []
    try:
        fh = open(path, "rb")
    except FileNotFoundError:
        return rows
    with fh:
        for raw in fh:
            m = RPROF.match(raw.decode("utf-8", "replace").strip())
            if not m:
                continue
            rows.append(dict(
                draws=int(m.group(1)), tris=int(m.group(2)), frags=int(m.group(3)),
                vertex=float(m.group(4)), raster=float(m.group(6)),
                mt=float(m.group(7) or 0.0), total=float(m.group(9))))
    return rows


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def main():
    SP = "/tmp/claude-1000/-home-djs-dhewm3/8b76b6ff-c14c-4592-95d0-eaa20d896738/scratchpad/perf"
    rounds = defaultdict(dict)
    for arm in ("on", "off"):
        for r in (1, 2):
            rows = load(f"{SP}/perf-{arm}-r{r}.log")
            if rows:
                rounds[r][arm] = rows

    print("=== FrameBin frame cost, [rprof] total (includes mt= parallel bucket)\n")
    ratios = []
    for r in sorted(rounds):
        line = f"round {r}: "
        vals = {}
        for arm in ("on", "off"):
            rows = rounds[r].get(arm, [])
            if not rows:
                line += f"{arm}=NO DATA  "
                continue
            t = med([x["total"] for x in rows])
            vals[arm] = t
            line += (f"{arm}: n={len(rows):<4} total_med={t:8.1f}ms  "
                     f"vertex_med={med([x['vertex'] for x in rows]):6.1f}ms  "
                     f"mt_med={med([x['mt'] for x in rows]):8.1f}ms  ")
        if "on" in vals and "off" in vals and vals["on"] > 0:
            ratio = vals["off"] / vals["on"]
            ratios.append(ratio)
            line += f"\n          => off/on = {ratio:.2f}x"
        print(line + "\n")

    # Matched subset: frames whose draws+tris signature appears in both arms.
    print("=== matched by (draws, tris/100) signature, pooled across rounds")
    def sig_map(arm):
        d = defaultdict(list)
        for r in rounds:
            for x in rounds[r].get(arm, []):
                d[(x["draws"], x["tris"] // 100)].append(x["total"])
        return d
    on_s, off_s = sig_map("on"), sig_map("off")
    common = sorted(set(on_s) & set(off_s))
    if not common:
        print("  no shared signatures -- cannot match")
    else:
        on_all, off_all = [], []
        for k in common:
            on_all += on_s[k]
            off_all += off_s[k]
        print(f"  {len(common)} shared signatures, {len(on_all)} on-frames vs "
              f"{len(off_all)} off-frames")
        print(f"  on  total_med = {med(on_all):8.1f}ms")
        print(f"  off total_med = {med(off_all):8.1f}ms")
        if med(on_all) > 0:
            print(f"  => off/on = {med(off_all)/med(on_all):.2f}x")

    if len(ratios) > 1:
        spread = max(ratios) / min(ratios)
        print(f"\nround-to-round spread in the ratio: {spread:.2f}x "
              f"({', '.join(f'{x:.2f}' for x in ratios)})")
        if spread > 1.15:
            print("  NOTE: rounds disagree by >15%. Treat the ratio as indicative "
                  "only -- the host was not quiet enough to state a number.")


if __name__ == "__main__":
    sys.exit(main())
