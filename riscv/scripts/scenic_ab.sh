#!/usr/bin/env bash
# Scenic A/B: TCG iterate profile vs shipping look. Short timeouts; compare
# median [rprof] total and frame bk from serial logs.
#
# Usage:
#   TIMEOUT=600 riscv/scripts/scenic_ab.sh
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
TIMEOUT="${TIMEOUT:-600}"
OUTDIR="${OUTDIR:-$PROJ/riscv/docs/perf}"
mkdir -p "$OUTDIR"

run_one() {
  local name="$1"; shift
  local log="$OUTDIR/scenic-ab-$name.log"
  echo "[scenic-ab] === $name === $*"
  env "$@" LOG="$log" TIMEOUT="$TIMEOUT" PIN_HARTS=1 SMP="${SMP:-6}" \
    "$PROJ/riscv/demorun.sh" || true
  echo "[scenic-ab] $name: rprof samples=$(grep -ac '\[rprof\]' "$log" || true)"
  grep -aE '\[rprof\]|^frame:' "$log" | tail -20 || true
}

# A: TCG profile (fast iterate)
run_one tcg TCG_PROFILE=1 RENDER_PROF=1

# B: shipping-like (only if time remains / explicit FULL=1)
if [ "${FULL:-0}" = "1" ]; then
  run_one ship RVV_VLEN=1024 FB_WIDTH=768 FB_HEIGHT=432 RENDER_PROF=1
else
  echo "[scenic-ab] skip ship profile (set FULL=1 for 768x432 VLEN=1024)"
fi

echo "[scenic-ab] done. Logs in $OUTDIR/scenic-ab-*.log"
