#!/usr/bin/env bash
# Build the Windows-hosted medany compiler and all target runtimes locally.
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$PROJ/build/toolchain"
PREFIX="$ROOT/install"
mkdir -p "$ROOT/downloads" "$ROOT/src" "$ROOT/obj" "$PREFIX/bin" "$PREFIX/riscv64-unknown-elf/bin" "$ROOT/tmp"
export TMPDIR="$ROOT/tmp"
export PATH="/ucrt64/bin:/usr/bin:$PATH"
exec > >(tee -a "$ROOT/build.log") 2>&1
echo "[toolchain] $(date -Iseconds) prefix=$PREFIX"
for tool in gcc g++ make makeinfo curl tar; do command -v "$tool" >/dev/null; done
fetch() { if [ ! -f "$ROOT/downloads/$2" ]; then curl -fL --retry 3 "$1/$2" -o "$ROOT/downloads/$2.part"; mv "$ROOT/downloads/$2.part" "$ROOT/downloads/$2"; fi; }
fetch https://ftp.gnu.org/gnu/gcc/gcc-16.1.0 gcc-16.1.0.tar.xz
fetch https://sourceware.org/pub/newlib newlib-4.6.0.20260123.tar.gz
sha256sum "$ROOT/downloads/"*.tar.* > "$ROOT/downloads/SHA256SUMS.local"
if [ ! -f "$ROOT/src/gcc-16.1.0/configure" ]; then tar -xf "$ROOT/downloads/gcc-16.1.0.tar.xz" -C "$ROOT/src"; fi
if [ ! -f "$ROOT/src/newlib-4.6.0.20260123/configure" ]; then tar -xf "$ROOT/downloads/newlib-4.6.0.20260123.tar.gz" -C "$ROOT/src"; fi
for part in newlib libgloss; do
 if [ ! -d "$ROOT/src/gcc-16.1.0/$part" ]; then cp -a "$ROOT/src/newlib-4.6.0.20260123/$part" "$ROOT/src/gcc-16.1.0/"; fi
done
# Binutils are code-model-independent; reuse the installed MSYS2 package.
for tool in addr2line ar as c++filt elfedit gprof ld ld.bfd nm objcopy objdump ranlib readelf size strings strip; do
 cp "/ucrt64/bin/riscv64-unknown-elf-$tool.exe" "$PREFIX/bin/"
 cp "/ucrt64/bin/riscv64-unknown-elf-$tool.exe" "$PREFIX/riscv64-unknown-elf/bin/$tool.exe"
done
export PATH="$PREFIX/bin:$PATH"
cd "$ROOT/obj"
if [ ! -f Makefile ]; then
 ../src/gcc-16.1.0/configure \
  --prefix="$(cygpath -m "$PREFIX")" --build=x86_64-w64-mingw32 --host=x86_64-w64-mingw32 \
  --target=riscv64-unknown-elf --with-arch=rv64gc --with-abi=lp64d --with-cmodel=medany \
  --with-newlib --disable-multilib --disable-bootstrap --enable-languages=c,c++ \
  --disable-nls --disable-shared --disable-threads --disable-libssp --disable-libquadmath \
  --disable-libgomp --disable-libatomic --disable-libstdcxx-pch \
  --with-gmp=/ucrt64 --with-mpfr=/ucrt64 --with-mpc=/ucrt64
fi
make -j"${JOBS:-12}"
make install
"$PREFIX/bin/riscv64-unknown-elf-gcc" -Q --help=target | grep mcmodel
echo "[toolchain] build and install complete: $PREFIX"
