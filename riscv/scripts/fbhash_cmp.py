#!/usr/bin/env python3
"""
Compare per-frame framebuffer digests between two runs.

Why this is not just `diff`: two runs rarely present the SAME NUMBER of frames.
They are cut off by a wall-clock TIMEOUT, so whichever run is slower (or was
sharing the host with a build) simply gets fewer frames in. A plain diff of the
two hash lists then reports every surplus frame as a difference and calls a
perfectly good result a FAIL.

What actually matters is: for every frame index present in BOTH runs, is the
digest identical? That is the pixel-identity contract. Frames only one run
reached are not evidence of anything either way, and are reported separately so
a thin overlap cannot be mistaken for a strong pass.

Usage:  fbhash_cmp.py <reference.log> <candidate.log> [label]
Exit:   0 = every common frame identical, 1 = a real mismatch, 2 = nothing to
        compare (which is a FAIL: it means a run never reached a present).
"""
import re
import sys

FBHASH = re.compile(r"^\[fbhash\] frame=(\d+) (\d+)x(\d+) h=([0-9a-f]+)")


def load(path):
    """frame index -> (WxH, digest). Later duplicates win; there should be none.

    A missing log is a normal outcome (an arm that was never run, or one killed
    before it opened its log), not a crash. Returning empty lets main() report
    "no overlapping frames" -- which names the real problem -- instead of dying
    in a traceback that buries it.
    """
    out = {}
    try:
        fh = open(path, "rb")
    except FileNotFoundError:
        print(f"  note: {path} does not exist -- treating as 0 frames")
        return out
    with fh:
        for raw in fh:
            m = FBHASH.match(raw.decode("utf-8", "replace").strip())
            if m:
                out[int(m.group(1))] = (f"{m.group(2)}x{m.group(3)}", m.group(4))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ref_path, cand_path = sys.argv[1], sys.argv[2]
    label = sys.argv[3] if len(sys.argv) > 3 else "candidate vs reference"

    ref, cand = load(ref_path), load(cand_path)
    common = sorted(set(ref) & set(cand))

    print(f"=== {label}")
    print(f"  reference {ref_path}: {len(ref)} frames")
    print(f"  candidate {cand_path}: {len(cand)} frames")
    print(f"  frames present in both: {len(common)}")

    if not common:
        print("  FAIL: no overlapping frames -- did a run never reach a present?")
        return 2

    bad = [i for i in common if ref[i] != cand[i]]
    if not bad:
        print(f"  PASS: all {len(common)} common frames are bit-identical "
              f"(frames {common[0]}..{common[-1]})")
        only_ref = sorted(set(ref) - set(cand))
        only_cand = sorted(set(cand) - set(ref))
        if only_ref or only_cand:
            print(f"  note: {len(only_ref)} frame(s) only in reference, "
                  f"{len(only_cand)} only in candidate -- not compared. "
                  f"A run cut short by its TIMEOUT is the usual cause.")
        return 0

    print(f"  FAIL: {len(bad)} of {len(common)} common frames differ")
    for i in bad[:20]:
        rs, rh = ref[i]
        cs, ch = cand[i]
        size = "" if rs == cs else f"  SIZE DIFFERS {rs} vs {cs}"
        print(f"    frame {i}: ref={rh} cand={ch}{size}")
    if len(bad) > 20:
        print(f"    ... and {len(bad) - 20} more")
    print(f"  first divergence at frame {bad[0]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
