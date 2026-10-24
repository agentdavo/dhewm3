#!/usr/bin/env bash
#
# framebin_ab.sh — validate FrameBin against immediate mode.
#
# FrameBin defers programmable draws and clears into per-tile bins and replays
# them, per tile, in submission order. That ordering is chosen specifically so
# the result is PIXEL-IDENTICAL to immediate mode -- so a difference is a bug,
# not a tradeoff. This runs the same deterministic vantage both ways and diffs
# the per-frame [fbhash] digests.
#
# Both arms must be configured with -DBAREMETAL_FB_HASH=1 and differ ONLY in
# -DBAREMETAL_FRAMEBIN. bm_benchVantage pins the camera at frame 8 (and defaults
# on whenever BM_RENDER_PROF is set), so the two runs render the same geometry.
#
# Usage: riscv/scripts/framebin_ab.sh [timeout_seconds]
set -uo pipefail

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
TIMEOUT="${TIMEOUT:-${1:-1800}}"
OUT="$PROJ/riscv/docs/perf"
mkdir -p "$OUT"

# `setsid` is load-bearing, not tidiness. A previous run of this comparison died
# with `qemu-system-riscv64: terminating on signal 15 from pid ... (bash)` after
# 13 of its 30 minutes: an unrelated wrapper in the same process group timed out
# and its SIGTERM reached QEMU. Handover 10 §3.1 lost a whole 40-minute run to
# exactly this. Putting each run in its own session means only this script's own
# TIMEOUT can end it.
run_arm() {
    local arm="$1" dir="$2"
    echo "[framebin-ab] arm=$arm  elf=$dir/dhewm3  timeout=${TIMEOUT}s"
    # Private scratch image PER ARM. demorun.sh defaults SCRATCH to one fixed
    # path, so despite its "decoupled from any concurrent run" comment, any other
    # demorun on this host holds that image's exclusive write lock and QEMU exits
    # instantly with `Failed to get "write" lock`. Both arms of this comparison
    # died that way against a leftover run -- and the failure is easy to misread,
    # because the arm still produces a log, just one with no [fbhash] in it.
    setsid env ELF="$dir/dhewm3" RVV_VLEN=256 FB_WIDTH=512 FB_HEIGHT=288 \
    RENDER_PROF=1 PIN_HARTS=1 SMP=6 TIMEOUT="$TIMEOUT" NOBUILD=1 \
    SCRATCH="$PROJ/build-rv/framebin-ab-$arm.img" \
    LOG="$OUT/framebin-$arm.log" "$PROJ/riscv/demorun.sh" >/dev/null 2>&1 < /dev/null

    # Confirm the arm actually rendered. Without this the comparator is handed
    # two empty logs and reports "no overlapping frames", which reads as a render
    # bug rather than "QEMU never started".
    if ! grep -q '^\[fbhash\]' "$OUT/framebin-$arm.log" 2>/dev/null; then
        echo "[framebin-ab] arm=$arm produced NO frames -- check $OUT/framebin-$arm.log"
        grep -m3 -E 'write" lock|Failed|error|missing' "$OUT/framebin-$arm.log" 2>/dev/null | sed 's/^/    /'
    fi
    echo "[framebin-ab] arm=$arm done: $(grep -c '^\[fbhash\]' "$OUT/framebin-$arm.log" 2>/dev/null || echo 0) frames hashed"
}

# Sequential, not parallel: two pinned 6-hart QEMUs on a 10-core host would
# oversubscribe and the perf counters in the same logs would be meaningless.
# ARMS=off re-runs a single arm when the other already produced a good log.
for arm in ${ARMS:-on off}; do
    case "$arm" in
        on)  run_arm on  "${ON_DIR:-$PROJ/build-rv-fb1}" ;;
        off) run_arm off "${OFF_DIR:-$PROJ/build-rv-fb0}" ;;
    esac
done

echo
echo "=== [fbhash] comparison (FrameBin on vs off) ==="
grep '^\[fbhash\]' "$OUT/framebin-on.log"  > "$OUT/fbhash-on.txt"  2>/dev/null
grep '^\[fbhash\]' "$OUT/framebin-off.log" > "$OUT/fbhash-off.txt" 2>/dev/null

# NOT `diff`. The two arms run at different speeds and are cut off by the same
# wall-clock TIMEOUT, so unequal frame counts are the NORMAL case -- a plain diff
# reports every surplus frame as a divergence and calls a good result a FAIL.
# fbhash_cmp.py compares the intersection and reports the non-overlap separately.
python3 "$PROJ/riscv/scripts/fbhash_cmp.py" \
    "$OUT/framebin-off.log" "$OUT/framebin-on.log" \
    "FrameBin on (candidate) vs off (reference)" | tee "$OUT/fbhash-verdict.txt"
cmp_rc=${PIPESTATUS[0]}

echo
# NOT mt_engaged. Under FrameBin, draws are recorded and replayed in ONE
# frame-level flush, so mt_engaged counts flushes, not draws -- it stays near 1
# on a perfectly balanced frame. Read hart balance and the frame total instead.
echo "=== hart balance (busy-ms/tiles per hart) ==="
for arm in on off; do
    echo "-- framebin=$arm"
    grep -oE 'h0=[0-9.]+ms/[0-9]+.*occ=[0-9]+%' "$OUT/framebin-$arm.log" 2>/dev/null | tail -3
done

echo
echo "=== frame cost ([rprof] total now INCLUDES the mt= parallel bucket) ==="
for arm in on off; do
    echo "-- framebin=$arm"
    grep -oE 'vertex=[0-9.]+ms .*total=[0-9.]+ms' "$OUT/framebin-$arm.log" 2>/dev/null | tail -3
done

exit "$cmp_rc"
