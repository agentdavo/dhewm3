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
#          TCG_PROFILE=1     (RVV_VLEN=256, FB=512x288, RENDER_PROF=1, PIN_HARTS=1)
#          RVV_VLEN=256|512|1024  FB_WIDTH=  FB_HEIGHT=  RENDER_PROF=0|1
#          PIN_HARTS=1  TCG_TB_SIZE=256  SMP=N
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

# TCG_PROFILE=1: faster QEMU iteration defaults (narrower vectors + smaller FB +
# render prof). Real-HW / visual quality keeps BM_RVV_VLEN=1024 + 768x432.
RVV_VLEN="${RVV_VLEN:-}"
FB_WIDTH="${FB_WIDTH:-}"
FB_HEIGHT="${FB_HEIGHT:-}"
# RENDER_PROF: default 0, but TCG_PROFILE defaults it to 1 unless the caller
# already set RENDER_PROF in the environment (including explicit 0).
if [ "${TCG_PROFILE:-0}" = "1" ]; then
    RVV_VLEN="${RVV_VLEN:-256}"
    FB_WIDTH="${FB_WIDTH:-512}"
    FB_HEIGHT="${FB_HEIGHT:-288}"
    if [ -z "${RENDER_PROF+set}" ]; then RENDER_PROF=1; fi
    PIN_HARTS="${PIN_HARTS:-1}"
    echo "[demorun] TCG_PROFILE=1 -> RVV_VLEN=$RVV_VLEN FB=${FB_WIDTH}x${FB_HEIGHT} RENDER_PROF=${RENDER_PROF:-1} PIN_HARTS=${PIN_HARTS:-0}"
fi
RENDER_PROF="${RENDER_PROF:-0}"

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
    CMAKE_ARGS=(
        -DBAREMETAL_DEFER_PRECACHE="$DEFER_PRECACHE"
        -DBAREMETAL_QUIET_ASSET_LOADS="$QUIET_ASSET_LOADS"
        -DBAREMETAL_IO_STATS="$IO_STATS"
        -DBAREMETAL_SIMD_BENCH="$SIMD_BENCH"
        -DBAREMETAL_RENDER_PROF="$RENDER_PROF"
    )
    [ -n "$RVV_VLEN" ] && CMAKE_ARGS+=( "-DBM_RVV_VLEN=$RVV_VLEN" )
    [ -n "$FB_WIDTH" ] && CMAKE_ARGS+=( "-DBAREMETAL_FB_WIDTH=$FB_WIDTH" )
    [ -n "$FB_HEIGHT" ] && CMAKE_ARGS+=( "-DBAREMETAL_FB_HEIGHT=$FB_HEIGHT" )
    echo "[demorun] configuring: defer_precache=$DEFER_PRECACHE quiet_assets=$QUIET_ASSET_LOADS io_stats=$IO_STATS simd_bench=$SIMD_BENCH render_prof=$RENDER_PROF vlen=${RVV_VLEN:-cache} fb=${FB_WIDTH:-cache}x${FB_HEIGHT:-cache}"
    cmake -S "$PROJ/neo" -B "$PROJ/build-rv" "${CMAKE_ARGS[@]}" >/dev/null || { echo "[demorun] configure FAILED"; exit 1; }
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

# Match the physical target independently of host CPU count.
# SMP=N remains available for explicit scaling experiments.
SMP="${SMP:-4}"

# 3. boot + capture serial. Headless -nographic (no GPU device); for a visual
#    frame use capture_screendump.sh (VIDEO=gpu + monitor screendump).
RUN_ENV=( "DISK=$SCRATCH" "TIMEOUT=$TIMEOUT" "SMP=$SMP" )
[ -n "$RVV_VLEN" ] && RUN_ENV+=( "RVV_VLEN=$RVV_VLEN" )
[ -n "${PIN_HARTS:-}" ] && RUN_ENV+=( "PIN_HARTS=$PIN_HARTS" )
[ -n "${TCG_TB_SIZE:-}" ] && RUN_ENV+=( "TCG_TB_SIZE=$TCG_TB_SIZE" )
# run.sh can expose a QEMU monitor, but demorun never forwarded it -- so the one
# tool that can diagnose a hung guest (`info registers -a` gives every hart's PC)
# was unreachable from exactly the long runs that hang. Set MONITOR_SOCKET=<path>.
[ -n "${MONITOR_SOCKET:-}" ] && RUN_ENV+=( "MONITOR_SOCKET=$MONITOR_SOCKET" )
if [ -n "${AIA:-}" ]; then
    RUN_ENV+=( "AIA=$AIA" )
fi
echo "[demorun] booting demo_mars_city1 (timeout=${TIMEOUT}s, disk=$SCRATCH) -> $LOG"
env "${RUN_ENV[@]}" "$PROJ/riscv/run.sh" "$ELF" 2>&1 | tee "$LOG"

# 4. summarise reach
echo "[demorun] ---- run summary ----"
grep -aE '^\[(dos|frame|map|gl|demoflow|rprof)\]|dvgpu:|powering off|panic|trap|FATAL|hang' "$LOG" | tail -60
