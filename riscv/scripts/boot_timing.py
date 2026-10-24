#!/usr/bin/env python3
"""Summarize the first DoomOS boot through the first map present from serial logs."""
import argparse
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    rows = []
    pattern = re.compile(r"\[boot-time\] total_ms=(\d+) delta_ms=(\d+) stage=(\S+)")
    for line in args.log.read_text(errors="replace").splitlines():
        match = pattern.search(line)
        if not match:
            continue
        total, delta, stage = match.groups()
        rows.append((int(total), int(delta), stage))
        if stage == "first-map-present":
            break
    if not rows:
        parser.error("No [boot-time] records found; boot the instrumented ELF")
    print("DoomOS C-runtime entry is t=0; firmware and host launch are excluded.")
    print("Times use the DTB timebase, independent of the game clock.\n")
    print(f"{'Total ms':>10} {'Delta ms':>10}  Milestone")
    for total, delta, stage in rows:
        print(f"{total:10d} {delta:10d}  {stage}")
    print("\nLargest elapsed intervals (not exclusive CPU profiles):")
    intervals = [(r[1], 'runtime-entry' if i == 0 else rows[i-1][2], r[2])
                 for i, r in enumerate(rows)]
    for delta, start, end in sorted(intervals, reverse=True)[:6]:
        print(f"{delta:10d} ms  {start} -> {end}")
    if rows[-1][2] != "first-map-present":
        print("\nINCOMPLETE: no successful post-map GPU present recorded yet.")


if __name__ == "__main__":
    main()
