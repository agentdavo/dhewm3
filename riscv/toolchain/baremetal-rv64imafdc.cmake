# ===========================================================================
# baremetal-rv64imafdc.cmake — scalar (no-vector) variant of the bare-metal
# RISC-V64 toolchain file.
#
# Same CRT, linker script, medany model and library set as
# baremetal-rva23s64.cmake; only the ISA changes:
#
#     -march=rv64imafdc  (== rv64gc: I M A F D C + Zicsr/Zifencei)
#
# NOTE ON THE NAME: "rv64imacfdc" is not a legal GCC -march string — GCC parses
# the single-letter extensions in canonical order and rejects the second 'c'
# ("extension 'c' appear more than one time"). rv64imafdc is the same ISA.
#
# No V, no Zb*/Zk*, no Zfh, no Zvfh, no fixed-VLEN codegen. softgles2, the boot
# runtime and dos_virtio_gpu select their existing non-RVV reference paths
# (guarded on __riscv_v_intrinsic), so this builds and runs, but the software
# renderer loses every vector kernel. It is a portability target, not the
# shipping configuration.
#
# Usage:
#   cmake -S neo -B build-rv-scalar \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/riscv/toolchain/baremetal-rv64imafdc.cmake ...
# ===========================================================================
set(BM_VECTOR OFF CACHE BOOL "Build with the RVA23S64 vector profile" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/baremetal-rva23s64.cmake")
