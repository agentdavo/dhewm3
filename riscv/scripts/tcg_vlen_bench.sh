#!/usr/bin/env bash
#
# A/B softgles2 interaction-kernel microbench across RVV VLEN values under the
# local QEMU 11 build. Measures wall-clock ns/frag for the register-resident
# interaction kernel (the Doom 3 light path) at VLEN=256/512/1024.
#
# Usage:
#   riscv/scripts/tcg_vlen_bench.sh
#   VLENS="256 512" TIMEOUT=60 SMP=4 PIN_HARTS=1 riscv/scripts/tcg_vlen_bench.sh
#
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
SGL="$PROJ/riscv/softgles2"
VLENS="${VLENS:-256 512 1024}"
TIMEOUT="${TIMEOUT:-45}"
SMP="${SMP:-4}"
PIN_HARTS="${PIN_HARTS:-1}"
TCG_TB_SIZE="${TCG_TB_SIZE:-256}"
OUT="${OUT:-$PROJ/riscv/docs/perf/vlen-bench-latest.txt}"

mkdir -p "$(dirname "$OUT")"
{
  echo "# softgles2 iakern VLEN A/B — $(date -Is)"
  echo "# host=$(uname -n) nproc=$(nproc) SMP=$SMP PIN_HARTS=$PIN_HARTS TCG_TB_SIZE=$TCG_TB_SIZE"
  echo
} | tee "$OUT"

for vlen in $VLENS; do
  echo "==== VLEN=$vlen ====" | tee -a "$OUT"
  make -C "$SGL" clean >/dev/null
  make -C "$SGL" iakgl4esrv RVV_VLEN="$vlen" -j"$(nproc)" 2>&1 | tail -8 | tee -a "$OUT"
  log="$(mktemp)"
  set +e
  RVV_VLEN="$vlen" SMP="$SMP" PIN_HARTS="$PIN_HARTS" TCG_TB_SIZE="$TCG_TB_SIZE" \
    TIMEOUT="$TIMEOUT" "$PROJ/riscv/run.sh" "$SGL/build/iakern_gl4es_validate.elf" \
    >"$log" 2>&1
  rc=$?
  set -e
  grep -aE '\[gik\]|\[gik-vs\]|VLEN|panic|trap|error' "$log" | tee -a "$OUT" || true
  if [ "$rc" -ne 0 ]; then
    echo "[vlen-bench] VLEN=$vlen exit=$rc (timeout/fail — see full log)" | tee -a "$OUT"
  fi
  rm -f "$log"
  echo | tee -a "$OUT"
done

echo "Wrote $OUT"
echo "Pick the VLEN with lowest kernel ns/frag for TCG iteration; keep 1024 as HW profile."
