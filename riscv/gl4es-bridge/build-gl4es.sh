#!/usr/bin/env bash
#
# Build gl4es as a static library suitable for the softgles2 bridge.
#
#   ./build-gl4es.sh host    # native (x86) static lib for the host smoke test
#
# gl4es is vendored (gitignored) at ../gl4es; cloned on first use. The flags
# come from the integration analysis (docs/design/14): no dynamic loader, no
# EGL/X11/GBM, manual init — gl4es reaches the GLES2 driver only through
# set_getprocaddress(), which the bridge points at softgles2.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GL4ES="$HERE/../gl4es"
TARGET="${1:-host}"
RV_MARCH="${RV_MARCH:-rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl256b}"

if [ ! -d "$GL4ES/.git" ] && [ ! -f "$GL4ES/CMakeLists.txt" ]; then
    echo "[gl4es] cloning..."
    git clone --depth 1 https://github.com/ptitSeb/gl4es.git "$GL4ES"
fi

# gl4es is vendored + gitignored, so dhewm3-private modifications live as patches
# under patches/ and are (re)applied here — idempotently, so re-running is safe.
# Currently: GL_EXT_depth_bounds_test (advertise + GetProcAddress -> softgles2).
if [ -d "$GL4ES/.git" ]; then
    shopt -s nullglob
    for p in "$HERE/patches"/*.patch; do
        if git -C "$GL4ES" apply --reverse --check "$p" 2>/dev/null; then
            echo "[gl4es] patch already applied: $(basename "$p")"
        elif git -C "$GL4ES" apply --check "$p" 2>/dev/null; then
            git -C "$GL4ES" apply "$p" && echo "[gl4es] applied patch: $(basename "$p")"
        else
            echo "[gl4es] WARNING: patch does not apply cleanly: $(basename "$p")"
        fi
    done
    shopt -u nullglob
fi

# DEFAULT_ES=2 is essential: softgles2 is a GLES2 backend, so gl4es must target
# the GLES2 (shader/FPE) path, not its default GLES 1.1 backend.
# NOTE: no USE_CLOCK — it needs clock_gettime/CLOCK_MONOTONIC_RAW which newlib
# (bare metal) lacks; gl4es falls back to gettimeofday (link-stubbed).
FLAGS=(-DSTATICLIB=ON -DNO_LOADER=ON -DNOEGL=ON -DNO_INIT_CONSTRUCTOR=ON \
       -DNOX11=ON -DNO_GBM=ON -DDEFAULT_ES=2)

case "$TARGET" in
host)
    BDIR="$GL4ES/build-host"
    mkdir -p "$BDIR"; cd "$BDIR"
    cmake .. "${FLAGS[@]}" -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_BUILD_TYPE=Release >/dev/null
    rm -f "$GL4ES/lib/libGL.a"      # force relink (shared output path across targets)
    make -j"$(nproc)"
    cp "$GL4ES/lib/libGL.a" "$GL4ES/lib/libGL-host.a"   # distinct from the riscv lib
    echo "[gl4es] built $GL4ES/lib/libGL-host.a"
    ;;
riscv)
    # Bare-metal RISC-V static build (Phase 4b). Uses the /opt/riscv GCC 16
    # cross toolchain + newlib; gl4es links against newlib libc/libm. We still
    # must provide a few libc stubs when finally linking the engine image:
    #   getenv -> hardcoded LIBGL_NOPSA=1 / LIBGL_NOBANNER=1 / LIBGL_ES=2,
    #   getcwd -> NULL, fopen/fread/fwrite/fclose -> no-op (shader cache off),
    #   gettimeofday -> 0 (or SBI rdtime). See README "bare-metal stubs".
    RV=/opt/riscv/bin/riscv64-unknown-elf-
    BDIR="$GL4ES/build-riscv"
    mkdir -p "$BDIR"; cd "$BDIR"
    cmake .. "${FLAGS[@]}" \
        -DCMAKE_SYSTEM_NAME=Generic \
        -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_COMPILER="${RV}gcc" \
        -DCMAKE_AR="${RV}ar" -DCMAKE_RANLIB="${RV}ranlib" \
        -DCMAKE_C_FLAGS="-march=${RV_MARCH} -mabi=lp64d -mcmodel=medany -ffreestanding -O2 -D_FORTIFY_SOURCE=0 -I${HERE}/freestanding" \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    rm -f "$GL4ES/lib/libGL.a"      # force relink (shared output path across targets)
    make -j"$(nproc)"
    cp "$GL4ES/lib/libGL.a" "$GL4ES/lib/libGL-riscv.a"   # distinct from the host lib
    echo "[gl4es] built $GL4ES/lib/libGL-riscv.a (riscv64); link with /opt/riscv libc/libm + stubs"
    ;;
*)
    echo "usage: $0 host|riscv"; exit 1;;
esac
