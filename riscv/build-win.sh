#!/usr/bin/env bash
#
# build-win.sh — configure + build the bare-metal dhewm3 engine on Windows (MSYS2 UCRT64).
#
# Needs a riscv64-unknown-elf GCC whose newlib/libgcc/libstdc++ were built with
# -mcmodel=medany. MSYS2's packaged toolchain (pacman) is medlow and CANNOT link
# this engine (image lives at 0x80200000, past medlow's ±2 GiB reach; every
# prebuilt archive — libc/libm/libgcc/libsupc++/libstdc++ — is medlow, so a
# "just rebuild newlib" shortcut does not work: C++ exceptions + RTTI pull the
# medlow libsupc++/libstdc++/libgcc too). A medany GCC 16.1.0 + newlib
# 4.6.0.20260123 is built from source into C:\projects\dhewm3\build\toolchain\install, reusing the
# MSYS2 UCRT64 binutils / host gcc / gmp-mpfr-mpc (recipe + build tree:
# C:\projects\dhewm3\riscv\build-toolchain-win.sh — combined gcc+newlib tree configured
# --with-cmodel=medany --with-arch=rv64gc --with-abi=lp64d --disable-multilib
# --disable-bootstrap --enable-languages=c,c++). See riscv/docs/windows-build.md.
#
#   riscv/build-win.sh                 # -> C:/projects/dhewm3/build-rv/dhewm3
#   RENDER_PROF=1 riscv/build-win.sh   # opt in to per-stage timing/shader dumps
#
#   ENGINE_LD=<path>  retarget the RAM window (default riscv/engine/engine.ld,
#                     1920 MiB, sized for QEMU virt -m 4G). engine-raven.ld is
#                     the 1020 MiB variant for a 1 GiB part. Use a SEPARATE
#                     BUILD dir: the script is a cache variable, so reusing a
#                     tree silently keeps whichever link it was configured with.
#   BUILD=C:/projects/dhewm3/build-rv TOOLCHAIN_BIN=C:/projects/dhewm3/build/toolchain/install/bin riscv/build-win.sh
#
# Same CMake option set as the Linux build-rv tree (demorun.sh defaults).
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-$PROJ/build/toolchain/install/bin}"
if [ ! -x "$TOOLCHAIN_BIN/riscv64-unknown-elf-gcc" ] && [ ! -x "$TOOLCHAIN_BIN/riscv64-unknown-elf-gcc.exe" ]; then
    RV_CC="$(command -v riscv64-unknown-elf-gcc || true)"
    [ -n "$RV_CC" ] || { echo "error: install a medany RISC-V compiler in $TOOLCHAIN_BIN or set TOOLCHAIN_BIN" >&2; exit 1; }
    TOOLCHAIN_BIN="$(dirname "$RV_CC")"
fi
# BM_VECTOR=0 selects the scalar rv64imafdc (== rv64gc) target: no V, no Zb*/
# Zk*/Zfh, softgles2 on its non-RVV reference paths. Separate default build
# dir so it never shares a CMake cache with the shipping vector tree.
BM_VECTOR="${BM_VECTOR:-1}"
if [ "$BM_VECTOR" = "0" ]; then
    BM_TOOLCHAIN="$PROJ/riscv/toolchain/baremetal-rv64imafdc.cmake"
    BM_DEFAULT_BUILD="$PROJ/build-rv-scalar"
    export RV_MARCH="${RV_MARCH:-rv64imafdc}"
else
    BM_TOOLCHAIN="$PROJ/riscv/toolchain/baremetal-rva23s64.cmake"
    BM_DEFAULT_BUILD="$PROJ/build-rv"
fi
BUILD="${BUILD:-$(cygpath -m "$BM_DEFAULT_BUILD")}"
export PATH="$TOOLCHAIN_BIN:$PATH"
"$TOOLCHAIN_BIN/riscv64-unknown-elf-gcc" -Q --help=target | grep -q 'mcmodel=.*medany' \
    || { echo "error: $TOOLCHAIN_BIN gcc does not default to medany" >&2; exit 1; }

# Keep the bridge archive current when its vendored DDS/texture paths change.
RV="$TOOLCHAIN_BIN/riscv64-unknown-elf-" GL4ES_BUILD_DIR="$BUILD/gl4es" \
  bash "$PROJ/riscv/gl4es-bridge/build-gl4es.sh" riscv

cmake -S "$(cygpath -m "$PROJ/neo")" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$BM_TOOLCHAIN")" \
  -DRISCV_TOOLCHAIN_PREFIX=riscv64-unknown-elf- \
  -DBAREMETAL=ON -DHARDLINK_GAME=ON -DBASE=ON -DD3XP=OFF -DDEDICATED=OFF \
  -DSDL2=OFF -DSDL3=ON -DIMGUI=OFF -DONATIVE=OFF \
  -DBAREMETAL_DEFER_PRECACHE="${DEFER_PRECACHE:-1}" \
  -DBAREMETAL_QUIET_ASSET_LOADS="${QUIET_ASSET_LOADS:-1}" \
  -DBAREMETAL_IO_STATS="${IO_STATS:-1}" \
  -DBAREMETAL_SIMD_BENCH="${SIMD_BENCH:-0}" \
  -DBAREMETAL_RENDER_PROF="${RENDER_PROF:-0}" \
  -DBAREMETAL_BENCH="${BENCH:-0}" \
  -DBAREMETAL_SIMDTEST="${SIMDTEST:-0}" \
  -DBAREMETAL_MACHINE_SPEC="${MACHINE_SPEC:-0}" \
  -DBM_RVV_VLEN="${RVV_VLEN:-1024}" -DBM_VECTOR="$BM_VECTOR" \
  ${FB_WIDTH:+-DBAREMETAL_FB_WIDTH=$FB_WIDTH} ${FB_HEIGHT:+-DBAREMETAL_FB_HEIGHT=$FB_HEIGHT} \
  ${SSAA:+-DBAREMETAL_SSAA=$SSAA} \
  ${ENGINE_LD:+-DBAREMETAL_ENGINE_LD=$ENGINE_LD}
ninja -C "$BUILD" -j"${JOBS:-16}" dhewm3
riscv64-unknown-elf-size "$BUILD/dhewm3"
echo "built: $BUILD/dhewm3   (boot with: DISK=<scratch.img> riscv/run-win.sh $BUILD/dhewm3)"
