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
RV_MARCH="${RV_MARCH:-rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl${RVV_VLEN:-1024}b}"

# MSYS login shells may omit Git for Windows from PATH.
if ! command -v git >/dev/null 2>&1 && [ -x '/c/Program Files/Git/cmd/git.exe' ]; then
    export PATH="/c/Program Files/Git/cmd:$PATH"
fi
command -v git >/dev/null 2>&1 || { echo '[gl4es] git is required to verify vendor patches' >&2; exit 1; }

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
        # Later patches may change context around an earlier applied hunk.
        if git -C "$GL4ES" apply -C0 --ignore-space-change --reverse --check "$p" 2>/dev/null; then
            echo "[gl4es] patch already applied: $(basename "$p")"
        elif git -C "$GL4ES" apply --ignore-space-change --check "$p" 2>/dev/null; then
            git -C "$GL4ES" apply --ignore-space-change "$p" && echo "[gl4es] applied patch: $(basename "$p")"
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
    cmake .. "${FLAGS[@]}" -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    rm -f "$GL4ES/lib/libGL.a"      # force relink (shared output path across targets)
    make -j"$(nproc)"
    cp "$GL4ES/lib/libGL.a" "$GL4ES/lib/libGL-host.a"   # distinct from the riscv lib
    echo "[gl4es] built $GL4ES/lib/libGL-host.a"
    ;;
riscv)
    # Bare-metal RISC-V static build (Phase 4b). Uses the repository-local GCC 16
    # cross toolchain + newlib; gl4es links against newlib libc/libm. We still
    # must provide a few libc stubs when finally linking the engine image:
    #   getenv -> hardcoded LIBGL_NOPSA=1 / LIBGL_NOBANNER=1 / LIBGL_ES=2,
    #   getcwd -> NULL, fopen/fread/fwrite/fclose -> no-op (shader cache off),
    #   gettimeofday -> 0 (or SBI rdtime). See README "bare-metal stubs".
    # Toolchain prefix: defaults to build/toolchain/install, override with RV=
    # (e.g. RV=riscv64-unknown-elf- with a medany cross GCC first on PATH — the
    # Windows/MSYS2 build uses C:\projects\dhewm3\build\toolchain\install, see riscv/build-win.sh).
    RV="${RV:-$HERE/../../build/toolchain/install/bin/riscv64-unknown-elf-}"
    # A vector libGL.a linked into a scalar rv64imafdc image faults on the
    # first vsetvli, so the two variants must not share an archive name. Ask
    # the compiler whether this -march really has V rather than pattern-
    # matching the string (every rv64* name contains the "v" of "rv").
    if "${RV}gcc" -march="$RV_MARCH" -mabi=lp64d -dM -E -x c - </dev/null 2>/dev/null \
         | grep -q "__riscv_vector"; then
        RV_LIB="libGL-riscv.a"
    else
        RV_LIB="libGL-riscv-scalar.a"
    fi
    # Keep the cross-build cache distinct from historical Windows-host caches.
    BDIR="${GL4ES_BUILD_DIR:-$HERE/../../build-rv/gl4es}"
    CC="${RV}gcc"; AR="${RV}ar"; RANLIB="${RV}ranlib"
    FREESTANDING="$HERE/freestanding"
    # Native Windows CMake needs executable suffixes and native include paths.
    case "$(uname -s)" in
        MSYS*|MINGW*|CYGWIN*)
            CC="$(cygpath -m "$(command -v "${CC}.exe")")"
            AR="$(cygpath -m "$(command -v "${AR}.exe")")"
            RANLIB="$(cygpath -m "$(command -v "${RANLIB}.exe")")"
            FREESTANDING="$(cygpath -m "$FREESTANDING")"
            ;;
    esac
    mkdir -p "$BDIR"; cd "$BDIR"
    cmake "$GL4ES" "${FLAGS[@]}" \
        -DCMAKE_SYSTEM_NAME=Generic \
        -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" \
        -DCMAKE_C_FLAGS="-march=${RV_MARCH} -mabi=lp64d -mcmodel=medany -ffreestanding -O2 -DBAREMETAL -D_FORTIFY_SOURCE=0 -I${FREESTANDING}" \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    rm -f "$GL4ES/lib/libGL.a"      # force relink (shared output path across targets)
    cmake --build . -j"${JOBS:-$(nproc)}"
    cp "$GL4ES/lib/libGL.a" "$GL4ES/lib/$RV_LIB"   # distinct from the host lib
    echo "[gl4es] built $GL4ES/lib/$RV_LIB (riscv64, -march=$RV_MARCH); link with the matching medany libc/libm + stubs"
    ;;
*)
    echo "usage: $0 host|riscv"; exit 1;;
esac
