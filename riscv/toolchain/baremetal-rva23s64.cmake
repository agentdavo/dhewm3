# ===========================================================================
# baremetal-rva23s64.cmake — CMake toolchain file for dhewm3 bare-metal RISC-V64
#
# Phase 5 (ROADMAP.md). Cross-compiles the engine + idLib + hardlinked game
# (and the gl4es/softgles2 bridge) for QEMU `virt` with NO operating system,
# booted by OpenSBI in S-mode (see riscv/boot/).
#
# Usage:
#   cmake -S neo -B build-rv \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/riscv/toolchain/baremetal-rva23s64.cmake \
#         -DBAREMETAL=ON -DHARDLINK_GAME=ON -DBASE=ON -DD3XP=OFF \
#         -DDEDICATED=OFF -DSDL2=OFF -DSDL3=OFF -DIMGUI=OFF -DONATIVE=OFF
#
# Ground truth (verified on this machine):
#   * Toolchain : /opt/riscv/bin/riscv64-unknown-elf-* , GCC 16.1.0 (newlib).
# * ISA/ABI   : -march=$(BM_MARCH)
#                 -mabi=lp64d -mcmodel=medany  (matches the riscv/Makefile ARCH).
#   * The ISA above with -mabi=lp64d resolves to the TOP-LEVEL (non-multilib)
#     libgcc / libc / libm / libsupc++ / libstdc++ under /opt/riscv — those are
#     the libraries this build links against.
#   * EXCEPTIONS STAY ON (doc 15 §1.2 / doc 00 §C.9): common->Error() throws
#     idException caught in Frame()/Init(); hence -lsupc++ -lgcc_eh and the
#     engine linker script must KEEP .eh_frame(_hdr) (boot/linker.ld discards
#     them — engine.ld must not).
# ===========================================================================

# --- Generic = no target OS (bare metal). Disables CMake's OS feature probing. ---
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(CMAKE_CROSSCOMPILING   TRUE)

# --- Compiler / binutils (absolute paths; override -DRISCV_TOOLCHAIN_PREFIX) ---
if(NOT DEFINED RISCV_TOOLCHAIN_PREFIX)
	set(RISCV_TOOLCHAIN_PREFIX "/opt/riscv/bin/riscv64-unknown-elf-")
endif()

set(CMAKE_C_COMPILER   "${RISCV_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${RISCV_TOOLCHAIN_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${RISCV_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_AR           "${RISCV_TOOLCHAIN_PREFIX}ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${RISCV_TOOLCHAIN_PREFIX}ranlib"  CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${RISCV_TOOLCHAIN_PREFIX}objcopy" CACHE FILEPATH "")
set(CMAKE_SIZE         "${RISCV_TOOLCHAIN_PREFIX}size"    CACHE FILEPATH "")

# --- The compiler check must NOT try to link a hosted executable (we have no
#     OS startup files / syscalls yet at configure time). A static-library
#     try-compile only needs the assembler + archiver. ---
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# --- ISA / ABI. medany is mandatory: static window at 0x80200000, heap far
#     above (doc 00 §A.10). ---
set(BM_MARCH "rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfmin_zvfbfwma_zvbc_zvkg_zvkned_zvknha_zvknhb_zvksc_zvksed_zvksg_zvksh_zvl1024b")
set(BM_ARCH_FLAGS "-march=${BM_MARCH} -mabi=lp64d -mcmodel=medany")

# Existing build directories cache CMAKE_*_FLAGS after the first configure, so
# changing *_INIT above is not enough. Migrate only the previous exact -march
# token and leave any locally-added flags intact.
function(bm_migrate_cached_march var_name)
	if(DEFINED ${var_name})
		set(_bm_value "${${var_name}}")
		string(REPLACE "-march=rva23s64_zvl256b " "-march=${BM_MARCH} " _bm_value "${_bm_value} ")
		string(REPLACE "-march=rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl1024b " "-march=${BM_MARCH} " _bm_value "${_bm_value} ")
		string(REPLACE "-march=rva23s64 " "-march=${BM_MARCH} " _bm_value "${_bm_value} ")
		# Fixed-VLEN codegen (RVV audit §2): QEMU pins -cpu rva23s64,vlen=1024, so
		# let GCC treat the vector width as the compile-time constant zvl minimum
		# (1024b). Folds every `csrr vlenb` + shift into immediates: measured
		# simt_run_ir -8.8%, sgl_raster_tri_prog -22.6%. C/C++ only (the assembler
		# does not need it); safe because runtime VLEN never differs from 1024.
		if((NOT "${var_name}" STREQUAL "CMAKE_ASM_FLAGS") AND (NOT "${_bm_value}" MATCHES "mrvv-vector-bits"))
			string(APPEND _bm_value " -mrvv-vector-bits=zvl")
		endif()
		string(STRIP "${_bm_value}" _bm_value)
		set(${var_name} "${_bm_value}" CACHE STRING "" FORCE)
	endif()
endfunction()

foreach(_bm_flag_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_ASM_FLAGS)
	bm_migrate_cached_march(${_bm_flag_var})
endforeach()

# --- Freestanding-friendly codegen shared by C and C++. ---
#     * -ffreestanding         : no hosted-environment assumptions.
#     * -fno-pic / -fno-common : fixed-address static image, no GOT/COMMON.
#     * -fno-stack-protector   : no __stack_chk_guard / TLS canary.
#     * -fno-strict-aliasing   : the SW rasteriser type-puns RGBA8 <-> uint32.
set(BM_COMMON_FLAGS "${BM_ARCH_FLAGS} -mrvv-vector-bits=zvl -ffreestanding -fno-pic -fno-common -fno-stack-protector -fno-strict-aliasing")

# --- C++ extras. CORRECTION vs the original plan (verified building the engine,
#     see riscv/docs/design/31-engine-crossbuild-status.md §"toolchain"):
#     * -fno-rtti is NOT usable: the dhewm3 renderer/framework use dynamic_cast<>
#       extensively and CVarSystem.h uses typeid(). RTTI must stay ON.
#     * -ffreestanding is NOT usable for C++: GCC 16's libstdc++ refuses its
#       hosted headers (<cmath>/<typeinfo>/...) when __STDC_HOSTED__==0
#       ("This header is not available in freestanding mode."). newlib here is
#       effectively hosted (we supply the syscalls), so C++ must be compiled
#       HOSTED. The C flags keep -ffreestanding (SDL3's C build relies on it and
#       is already prebuilt; gl4es/softgles2/boot are C too).
#     * -fno-threadsafe-statics : single cooperative hart -> trivial guards (kept).
#     * NOT -fno-exceptions — exceptions are load-bearing (common->Error throws).
set(BM_CXX_COMMON_FLAGS "${BM_ARCH_FLAGS} -mrvv-vector-bits=zvl -fno-pic -fno-common -fno-stack-protector -fno-strict-aliasing")
set(BM_CXX_EXTRA "-fno-threadsafe-statics")

set(CMAKE_C_FLAGS_INIT   "${BM_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${BM_CXX_COMMON_FLAGS} ${BM_CXX_EXTRA}")
set(CMAKE_ASM_FLAGS_INIT "${BM_ARCH_FLAGS}")

# Pin -O2. CMake's default Release flags are "-O3 -DNDEBUG", but -O3 is MEASURED
# slower than -O2 for our RVV software renderer on the actual target: it
# vectorises more but at the cost of extra register spills + ~60% code bloat, and
# the RV/QEMU SIMT path runs ~8-20% SLOWER (host x86 benefits, RV does not). See
# riscv/docs/perf/02-rvv-codegen.md. Keep -DNDEBUG (asserts off in release).
set(CMAKE_C_FLAGS_RELEASE_INIT   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")

# --- Link model. -nostdlib: we supply CRT (riscv/boot), libc/libm (newlib over
#     SBI), softgles2, gl4es and the C++ ABI glue ourselves. The engine linker
#     script (riscv/engine/engine.ld) raises the RAM window above boot/linker.ld
#     and KEEPS .eh_frame(_hdr). -lgcc/-lgcc_eh/-lsupc++ provide the unwinder +
#     minimal C++ support lib (added after our objects by the engine CMake). ---
set(BM_LINKER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../engine/engine.ld")
set(CMAKE_EXE_LINKER_FLAGS_INIT
	"-nostdlib -static -Wl,--gc-sections -Wl,--build-id=none")
# (The engine target itself adds  -T <engine.ld>  and the lib group; see
#  neo/CMakeLists.txt BAREMETAL branch. We keep the script path discoverable.)
set(BAREMETAL_ENGINE_LD "${BM_LINKER_SCRIPT}" CACHE FILEPATH "engine linker script")

# --- Do not search the host sysroot for programs/libs/headers. ---
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# --- Static archives only; no shared libs / no -rdynamic on bare metal. ---
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")

# Marker the engine CMake can test instead of re-deriving the target.
set(BAREMETAL ON CACHE BOOL "Bare-metal RISC-V64 cross build" FORCE)
