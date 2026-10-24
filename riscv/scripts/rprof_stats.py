#!/usr/bin/env python3
"""Summarise [rprof]/[rprof-mt] lines from bare-metal serial logs.

Splits frames into the two regimes the demorun harvest identified (load/UI vs
lit scenic) and reports medians, because the mean is useless here: a run mixes
sub-second UI frames with multi-second lit frames and any single average lands
in between and describes nothing.

Usage:  rprof_stats.py <log> [<log> ...]
"""
import re
import statistics
import sys

# [rprof] draws=3 tris=18 frags=112554 skipxp=0 ... | vertex=0.6ms dispatch=216.4ms
#   raster=46.3ms mt=0.0ms present=0.0ms total=263.4ms | ...
#
# `mt=` is OPTIONAL so this still parses logs written before that field existed.
# It carries the deferred (FrameBin) parallel section, which under FrameBin is
# nearly the whole frame -- older logs simply have no such field and score 0.
RPROF = re.compile(
    r"\[rprof\]\s+draws=(\d+)\s+tris=(\d+)\s+frags=(\d+).*?"
    r"vertex=([\d.]+)ms\s+dispatch=([\d.]+)ms\s+raster=([\d.]+)ms\s+"
    r"(?:mt=([\d.]+)ms\s+)?"
    r"present=([\d.]+)ms\s+total=([\d.]+)ms"
)
# [rprof-mt] bin=.. release=.. mainbusy=..ms wait=..ms | frags=.. | hart ... |
#   shade=556.1ms (44%) wrapper=690.2ms lanes=117979 occ=95%
RPROF_MT = re.compile(
    r"\[rprof-mt\].*?shade=([\d.]+)ms\s+\((\d+)%\)\s+wrapper=([\d.]+)ms\s+"
    r"lanes=(\d+)\s+occ=(\d+)%"
)

# Classify on TRIANGLE count, not draw count. demorun-lit-vs-load.md used
# draws>50 for "lit", but a lit frame can be as few as 19 draws carrying 4238
# triangles (vlen-ab-256-512x288.log:271) — that frame fell between a draws<=10
# and a draws>50 test and was silently dropped from both buckets, hiding an
# 11-second frame. Triangle count separates the two regimes cleanly: UI/load
# frames carry ~20 tris, lit frames thousands.
LIT_TRIS = 100


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def summarise(path):
    frames, mt = [], []
    with open(path, "rb") as fh:
        for raw in fh:
            line = raw.decode("utf-8", "replace")
            m = RPROF.search(line)
            if m:
                g = m.groups()
                frames.append(dict(
                    draws=int(g[0]), tris=int(g[1]), frags=int(g[2]),
                    vertex=float(g[3]), dispatch=float(g[4]),
                    raster=float(g[5]), mt=float(g[6] or 0.0),
                    present=float(g[7]), total=float(g[8]),
                ))
                continue
            m = RPROF_MT.search(line)
            if m:
                g = m.groups()
                mt.append(dict(shade=float(g[0]), shade_pct=int(g[1]),
                               wrapper=float(g[2]), lanes=int(g[3]),
                               occ=int(g[4])))

    print(f"\n=== {path}")
    if not frames:
        print("  no [rprof] samples (run did not reach a rendered frame?)")
        return

    # Buckets must PARTITION the frames: any frame that falls in no bucket is a
    # frame whose cost is invisible, which is how the 11s frame got missed.
    buckets = (("load/UI", lambda f: f["tris"] < LIT_TRIS),
               ("lit scenic", lambda f: f["tris"] >= LIT_TRIS))
    counted = 0
    for label, sel in buckets:
        sub = [f for f in frames if sel(f)]
        counted += len(sub)
        if not sub:
            print(f"  {label:<11} n=0")
            continue
        totals = [f["total"] for f in sub]
        frags = [f["frags"] for f in sub]
        # ns per fragment is the VLEN-comparable figure: it normalises away any
        # difference in how far into the map the two arms actually got.
        ns_frag = [f["total"] * 1e6 / f["frags"] for f in sub if f["frags"]]
        print(f"  {label:<11} n={len(sub):<4} "
              f"total_med={med(totals):9.1f}ms  "
              f"frags_med={med(frags):10.0f}  "
              f"ns/frag={med(ns_frag):8.1f}")
        print(f"  {'':<11}   vertex={med([f['vertex'] for f in sub]):7.1f} "
              f"dispatch={med([f['dispatch'] for f in sub]):8.1f} "
              f"raster={med([f['raster'] for f in sub]):8.1f} "
              f"mt={med([f['mt'] for f in sub]):8.1f} "
              f"present={med([f['present'] for f in sub]):6.1f}")
        # `raster` only accumulates when sg_mt_prog_dispatch DECLINED the draw
        # (sg_draw.c: the MT path returns before the raster timer starts), so it
        # is single-hart time. vertex is serial too. This ratio is the real
        # parallelism story and is not visible anywhere in the existing logs.
        serial = [f["vertex"] + f["raster"] for f in sub]
        share = [100.0 * s / f["total"]
                 for s, f in zip(serial, sub) if f["total"]]
        print(f"  {'':<11}   SERIAL (vertex+raster, 1 hart) = "
              f"{med(serial):.1f}ms = {med(share):.1f}% of frame")

    if counted != len(frames):
        print(f"  !! {len(frames) - counted} frame(s) matched no bucket "
              f"— classifier is dropping data, fix it before trusting this")

    if mt:
        print(f"  mt          n={len(mt):<4} "
              f"shade_med={med([m['shade'] for m in mt]):9.1f}ms "
              f"({med([m['shade_pct'] for m in mt]):.0f}%)  "
              f"wrapper_med={med([m['wrapper'] for m in mt]):9.1f}ms  "
              f"occ_med={med([m['occ'] for m in mt]):.0f}%")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        try:
            summarise(path)
        except OSError as exc:
            print(f"\n=== {path}\n  unreadable: {exc}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
