#!/usr/bin/env bash
# FrameBin before/after frame time (handover 20 §7 item 5).
#
# INTERLEAVED rounds (on, off, on, off), not "all on then all off". The host is
# not perfectly quiet -- an unrelated Verilator build is running -- and a block
# design would charge any drift in that background load entirely to whichever
# arm ran during it. Interleaving spreads it across both. Load is recorded
# before and after every arm so the conditions are auditable rather than assumed.
#
# Metric is [rprof] total, which NOW INCLUDES the mt= parallel bucket. Before
# that fix `total` was vertex+dispatch+raster+present, and since FrameBin moves
# raster into the deferred flush it read ~0 -- so diffing the OLD total across
# FrameBin showed a ~10x speedup that did not exist (handover 20 §3.2).
set -uo pipefail
PROJ=/home/djs/dhewm3
SP=/tmp/claude-1000/-home-djs-dhewm3/8b76b6ff-c14c-4592-95d0-eaa20d896738/scratchpad
OUT=$SP/perf
TIMEOUT=${TIMEOUT:-360}
ROUNDS=${ROUNDS:-2}
mkdir -p "$OUT"

run_arm() {
    local arm="$1" dir="$2" round="$3"
    local log="$OUT/perf-$arm-r$round.log"
    echo "[perf] round $round arm=$arm  load_before=$(cut -d' ' -f1 /proc/loadavg)"
    setsid env ELF="$dir/dhewm3" RVV_VLEN=256 FB_WIDTH=512 FB_HEIGHT=288 \
        RENDER_PROF=1 PIN_HARTS=1 SMP=6 TIMEOUT="$TIMEOUT" NOBUILD=1 \
        SCRATCH="$PROJ/build-rv/perf-$arm.img" LOG="$log" \
        "$PROJ/riscv/demorun.sh" >/dev/null 2>&1 </dev/null
    echo "[perf]   -> $(grep -c '^\[rprof\]' "$log" 2>/dev/null) profiled frames," \
         "load_after=$(cut -d' ' -f1 /proc/loadavg)"
}

for r in $(seq 1 "$ROUNDS"); do
    run_arm on  "$PROJ/build-rv-fb1" "$r"
    run_arm off "$PROJ/build-rv-fb0" "$r"
done
echo "[perf] done"
