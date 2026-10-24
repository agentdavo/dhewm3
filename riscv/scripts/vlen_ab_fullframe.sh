#!/usr/bin/env bash
#
# Full-frame VLEN A/B — the measurement the microbench can't make.
#
# tcg_vlen_bench.sh measures ns/frag on the interaction kernel in isolation and
# says VLEN=1024 wins by 1.17-1.30x. That does NOT settle the shipping default,
# because lane count also sets the MT tile width (tile W = SGL_SIMT_LANES =
# VLEN/16), which changes binning granularity, tile-edge waste, HiZ cull rate
# and sparse-pack occupancy. Those only show up at full-frame scale.
#
# So: same map, same resolution, same SMP/pinning, ONLY VLEN varies. Runs are
# strictly serial (they share a scratch disk and would contend for host cores).
#
# CAVEAT, and it is a real one: gl4es is a prebuilt static archive whose -march
# hardcodes zvl256b (gl4es-bridge/build-gl4es.sh:16), and demorun.sh only rebuilds
# it on source mtime -- so changing RVV_VLEN does NOT rebuild it. One of the four
# linked components therefore stays at VLEN=256 across the whole sweep. gl4es is
# mostly control flow so the numeric effect is likely small, but "only VLEN
# varies" is not strictly true until build-gl4es.sh takes RV_MARCH from RVV_VLEN
# and the archive is force-rebuilt per arm.
#
# Usage:
#   riscv/scripts/vlen_ab_fullframe.sh
#   VLENS="256 1024" FB=512x288 TIMEOUT=2700 SMP=6 riscv/scripts/vlen_ab_fullframe.sh
#
set -uo pipefail

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
VLENS="${VLENS:-256 1024}"
FB="${FB:-512x288}"
FB_W="${FB%x*}"
FB_H="${FB#*x}"
TIMEOUT="${TIMEOUT:-2700}"
SMP="${SMP:-6}"
PIN_HARTS="${PIN_HARTS:-1}"
OUTDIR="${OUTDIR:-$PROJ/riscv/docs/perf}"
mkdir -p "$OUTDIR"

# FB must stay legal for EVERY VLEN under test: width % lanes == 0 and
# height % (lanes/4) == 0, where lanes = VLEN/16. Fail loudly rather than
# silently rendering a different pixel count per arm (which would void the A/B).
for vlen in $VLENS; do
    lanes=$(( vlen / 16 ))
    if [ $(( FB_W % lanes )) -ne 0 ] || [ $(( FB_H % (lanes / 4) )) -ne 0 ]; then
        echo "error: FB=${FB} not aligned for VLEN=$vlen (lanes=$lanes)" >&2
        exit 1
    fi
done

echo "[vlen-ab] arms='$VLENS' fb=$FB timeout=${TIMEOUT}s smp=$SMP pin=$PIN_HARTS"
echo "[vlen-ab] serial by design; total wall clock up to $(( TIMEOUT * $(echo $VLENS | wc -w) / 60 )) min"

for vlen in $VLENS; do
    log="$OUTDIR/vlen-ab-${vlen}-${FB}.log"
    echo "[vlen-ab] ===== VLEN=$vlen -> $log ====="
    # Full rebuild of the softgles2 objects is implied by the VLEN change (it
    # moves -march zvl<N>b), so let CMake see the changed cache var and redo it.
    env RVV_VLEN="$vlen" FB_WIDTH="$FB_W" FB_HEIGHT="$FB_H" \
        RENDER_PROF=1 PIN_HARTS="$PIN_HARTS" SMP="$SMP" \
        TIMEOUT="$TIMEOUT" LOG="$log" \
        "$PROJ/riscv/demorun.sh" >"$OUTDIR/vlen-ab-${vlen}-driver.log" 2>&1
    echo "[vlen-ab] VLEN=$vlen exit=$? rprof_samples=$(grep -ac '\[rprof\]' "$log" 2>/dev/null || echo 0)"
done

echo "[vlen-ab] parsing ..."
python3 "$PROJ/riscv/scripts/rprof_stats.py" "$OUTDIR"/vlen-ab-*-"$FB".log \
    | tee "$OUTDIR/vlen-ab-summary-${FB}.txt"
echo "[vlen-ab] done -> $OUTDIR/vlen-ab-summary-${FB}.txt"
