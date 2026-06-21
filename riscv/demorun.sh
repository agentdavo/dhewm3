#!/usr/bin/env bash
#
# demorun.sh — build the bare-metal dhewm3 engine, boot demo_mars_city1 on QEMU,
# and capture the full instrumented serial log for timing/profiling. The demo map
# is baked into the engine cmdline (riscv/engine/engine_kmain.c: +map ...).
#
# For a *visual* frame capture use a GPU-enabled riscv/run.sh invocation, which
# scans the frame out to a virtio-gpu device and grabs it host-side with QEMU's
# monitor `screendump` (PPM -> PNG). This script is the timing/serial-log runner.
#
# Usage:   riscv/demorun.sh
# Env:     TIMEOUT=300  DISK=<doom.img>  LOG=<path>  NOBUILD=1
#          DEFER_PRECACHE=1  (defer render-model/sound/GUI precache to speed map load)
#          IO_STATS=1        (print low-frequency virtio/dosfs/extension timing summaries)
#          QUIET_ASSET_LOADS=1 (suppress high-volume per-asset load logs)
#          SIMD_BENCH=1      (run small idSIMDProcessor microbench after engine init)
#          AIA=aplic|aplic-imsic (forward QEMU virt AIA mode to riscv/run.sh)
#
set -uo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"             # /home/djs/dhewm3
ELF="${ELF:-$PROJ/build-rv/dhewm3}"
DISK="${DISK:-$PROJ/doom.img}"
LOG="${LOG:-$PROJ/riscv/demo_mars_city1_serial.log}"
TIMEOUT="${TIMEOUT:-300}"
DEFER_PRECACHE="${DEFER_PRECACHE:-1}"
IO_STATS="${IO_STATS:-1}"
QUIET_ASSET_LOADS="${QUIET_ASSET_LOADS:-1}"
SIMD_BENCH="${SIMD_BENCH:-0}"

# 0. gl4es is a PREBUILT static archive (riscv/gl4es/lib/libGL-riscv.a) linked by
#    the engine; the CMake engine build does NOT recompile its sources. So if any
#    gl4es source changed since the archive was built, rebuild it first — otherwise
#    edits to gl4es (extension strings, GetProcAddress, wrappers) silently don't
#    take effect. NOBUILD=1 or SKIP_GL4ES=1 skips this.
if [ "${NOBUILD:-0}" != "1" ] && [ "${SKIP_GL4ES:-0}" != "1" ]; then
    GL4ES_LIB="$PROJ/riscv/gl4es/lib/libGL-riscv.a"
    GL4ES_SRC="$PROJ/riscv/gl4es/src"
    if [ ! -f "$GL4ES_LIB" ] || [ -n "$(find "$GL4ES_SRC" -newer "$GL4ES_LIB" \( -name '*.c' -o -name '*.h' -o -name '*.inc' \) -print -quit 2>/dev/null)" ]; then
        echo "[demorun] gl4es sources changed (or archive missing) -> rebuilding libGL-riscv.a"
        "$PROJ/riscv/gl4es-bridge/build-gl4es.sh" riscv || { echo "[demorun] gl4es rebuild FAILED"; exit 1; }
    else
        echo "[demorun] gl4es archive up to date"
    fi
fi

# 1. build the engine (incremental CMake) unless NOBUILD=1
if [ "${NOBUILD:-0}" != "1" ]; then
    echo "[demorun] configuring: defer_precache=$DEFER_PRECACHE quiet_assets=$QUIET_ASSET_LOADS io_stats=$IO_STATS simd_bench=$SIMD_BENCH"
    cmake -S "$PROJ/neo" -B "$PROJ/build-rv" \
        -DBAREMETAL_DEFER_PRECACHE="$DEFER_PRECACHE" \
        -DBAREMETAL_QUIET_ASSET_LOADS="$QUIET_ASSET_LOADS" \
        -DBAREMETAL_IO_STATS="$IO_STATS" \
        -DBAREMETAL_SIMD_BENCH="$SIMD_BENCH" >/dev/null || { echo "[demorun] configure FAILED"; exit 1; }
    echo "[demorun] building engine (build-rv/dhewm3) ..."
    make -C "$PROJ/build-rv" dhewm3 || { echo "[demorun] build FAILED"; exit 1; }
else
    echo "[demorun] NOBUILD=1: using controls baked into $ELF"
fi
[ -f "$ELF" ]  || { echo "[demorun] engine ELF missing: $ELF"; exit 1; }
[ -f "$DISK" ] || { echo "[demorun] asset disk missing: $DISK"; exit 1; }

# 2. clone the asset disk to a private scratch image. The engine's savefs journal
#    WRITES to the disk, so QEMU takes an exclusive write lock — a shared image
#    can't be used by two runs at once. A per-pipeline copy decouples our run from
#    any concurrent run and never mutates the shared doom.img.
SCRATCH="${SCRATCH:-$PROJ/build-rv/demo_mars_city1_disk.img}"
if [ ! -f "$SCRATCH" ] || [ "$DISK" -nt "$SCRATCH" ]; then
    echo "[demorun] cloning asset disk -> $SCRATCH"
    cp --reflink=auto -f "$DISK" "$SCRATCH" || { echo "[demorun] disk clone FAILED"; exit 1; }
fi

# Auto-scale guest harts to the host (softgles2 fans across up to 8 harts; QEMU
# MTTCG maps each to a host thread, so clamp to nproc). Override with SMP=N.
if [ -z "${SMP:-}" ]; then
    HOST_CORES="$(nproc 2>/dev/null || echo 4)"
    SMP=$(( HOST_CORES < 8 ? HOST_CORES : 8 ))
    echo "[demorun] auto SMP=$SMP (host cores=$HOST_CORES, softgles2 cap=8); override with SMP=N"
fi

# 3. boot + capture serial. Headless -nographic (no GPU device); for a visual
#    frame use capture_screendump.sh (VIDEO=gpu + monitor screendump).
RUN_ENV=( "DISK=$SCRATCH" "TIMEOUT=$TIMEOUT" "SMP=$SMP" )
if [ -n "${AIA:-}" ]; then
    RUN_ENV+=( "AIA=$AIA" )
fi
echo "[demorun] booting demo_mars_city1 (timeout=${TIMEOUT}s, disk=$SCRATCH) -> $LOG"
env "${RUN_ENV[@]}" "$PROJ/riscv/run.sh" "$ELF" 2>&1 | tee "$LOG"

# 4. summarise reach
echo "[demorun] ---- run summary ----"
grep -aE '^\[(dos|frame|map|gl|demoflow)\]|dvgpu:|powering off|panic|trap|FATAL|hang' "$LOG" | tail -40
