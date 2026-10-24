#!/usr/bin/env bash
# Compile/link a C++ runtime and RVV smoke payload at DoomOS's load address.
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$PROJ/build/toolchain/install"
OUT="$PROJ/build/toolchain/validation"
mkdir -p "$OUT"
export PATH="$PREFIX/bin:/ucrt64/bin:$PATH"
CXX="$PREFIX/bin/riscv64-unknown-elf-g++"
"$CXX" -Q --help=target > "$OUT/target-options.txt"
grep 'mcmodel=.*medany' "$OUT/target-options.txt"
cat > "$OUT/runtime.cpp" <<'CPP'
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <riscv_vector.h>
void *__dso_handle;
volatile double input = 0.75;
volatile int result;
extern "C" void _start() {
    double *p = new double[16];
    p[0] = std::sin(input);
    try { if (p[0] > 0) throw std::runtime_error("medany"); }
    catch (const std::exception &e) { result = e.what()[0]; }
    size_t vl = __riscv_vsetvl_e32m1(4);
    vuint32m1_t v = __riscv_vmv_v_x_u32m1(42, vl);
    unsigned lanes[4];
    __riscv_vse32_v_u32m1(lanes, v, vl);
    result += lanes[0];
    delete[] p;
    for (;;) {}
}
CPP
"$CXX" -march=rva23s64 -mabi=lp64d -mcmodel=medany -O2 \
 -nostartfiles --specs=nosys.specs -Wl,-Ttext=0x80200000 -Wl,-e,_start \
 "$OUT/runtime.cpp" -lm -o "$OUT/runtime.elf"
"$PREFIX/bin/riscv64-unknown-elf-readelf" -h -A "$OUT/runtime.elf" > "$OUT/elf.txt"
for lib in libc.a libm.a libgcc.a libstdc++.a libsupc++.a; do
 path="$("$CXX" -print-file-name="$lib")"
 [ "$path" != "$lib" ] || { echo "Missing $lib"; exit 1; }
 echo "$lib=$path"
done
echo "PASS: medany default; RVA23 + RVV + C++ exceptions/math/allocation linked at 0x80200000"
