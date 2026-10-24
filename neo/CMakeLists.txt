cmake_minimum_required(VERSION 2.6...3.22 FATAL_ERROR)
project(dhewm3)

# TODO

# osx: place game .dylib's in the bundle (next to the binary)
# osx: -weak_framework ?

# maybe add these as options:
# TARGET_MONO
# SETUP
# SDK				-D_D3SDK

# don't add these as options, but document them?
# IDNET_HOST		-DIDNET_HOST=\\"%s\\"' % IDNET_HOST
# DEBUG_MEMORY		-DID_DEBUG_MEMORY', '-DID_REDIRECT_NEWDELETE
# LIBC_MALLOC		-DUSE_LIBC_MALLOC=0
# ID_NOLANADDRESS	-DID_NOLANADDRESS

# fallback for cmake versions without add_compile_options
if(NOT COMMAND add_compile_options)
	function(add_compile_options)
		foreach(arg ${ARGN})
			set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${arg}" PARENT_SCOPE)
			set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${arg}" PARENT_SCOPE)
		endforeach()
	endfunction()
endif()

option(LINUX_RELEASE_BINS "Set RPATH to \$ORIGIN/libs/ for Linux binary releases" OFF)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/sys/cmake")

if(LINUX_RELEASE_BINS)
	message(STATUS "Setting RPATH to \$ORIGIN/libs/ so you can put dependencies in there")
	set(CMAKE_SKIP_RPATH OFF CACHE BOOL "Skip RPATH" FORCE)
	set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
	set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
	set(CMAKE_INSTALL_RPATH "$ORIGIN/libs")
	set(ldflags "${ldflags} -Wl,-z,origin") # not sure this is even needed, but shouldn't hurt
else()
	set(CMAKE_SKIP_RPATH ON CACHE BOOL "Skip RPATH" FORCE)
endif()

set(DHEWM3BINARY "dhewm3")

include(CheckCXXCompilerFlag)
include(GNUInstallDirs OPTIONAL RESULT_VARIABLE GNUINSTALLDIRS)
include(TestBigEndian)

option(CORE			"Build the core" ON)
option(BASE			"Build the base game code" ON)
option(D3XP			"Build the d3xp game code" ON)
if(MSVC)
	option(TOOLS		"Build the tools game code (Visual Studio+SDL2/SDL3 only)" OFF)
endif()
option(DEDICATED	"Build the dedicated server" OFF)
option(ONATIVE		"Optimize for the host CPU" OFF)
option(SDL2			"Use SDL2 instead of SDL1.2" ON)
option(SDL3			"Use SDL3 instead of SDL2 or SDL1.2" OFF)
option(IMGUI		"Build with Dear ImGui integration - requires SDL2/SDL3 and C++11" ON)
option(REPRODUCIBLE_BUILD "Replace __DATE__ and __TIME__ by hardcoded values for reproducible builds" OFF)

option(TRACY		"Build and integrate the Tracy profiler, requires HARDLINK_GAME" OFF)

option(HARDLINK_GAME "Compile gamecode into executable (no game DLLs)" OFF)

if(NOT MSVC) # GCC/clang or compatible, hopefully
	option(FORCE_COLORED_OUTPUT "Always produce ANSI-colored compiler warnings/errors (GCC/Clang only; esp. useful with ninja)." OFF)
	option(ASAN		"Enable GCC/Clang Adress Sanitizer (ASan)" OFF) # TODO: MSVC might also support this, somehow?
	option(UBSAN	"Enable GCC/Clang Undefined Behavior Sanitizer (UBSan), implies HARDLINK_GAME" OFF)

	if(UBSAN AND NOT HARDLINK_GAME)
		message(STATUS "UBSAN requires linking the gamecode into the executable, will enable HARDLINK_GAME")
		set(HARDLINK_GAME ON)
	endif()
endif()

# ===========================================================================
# BAREMETAL — bare-metal RISC-V64 "dOS" cross build (Path B: SDL3-on-dOS).
# Normally set ON (FORCE) by riscv/toolchain/baremetal-rva23s64.cmake. All edits
# below are guarded by if(BAREMETAL); a normal desktop build is unaffected.
# See riscv/docs/design/30-engine-bringup.md and 31-engine-crossbuild-status.md.
# ===========================================================================
option(BAREMETAL "Bare-metal RISC-V64 cross build (no OS; uses prebuilt SDL3/gl4es .a)" OFF)
option(D3_GLPROF "Emit compact per-frame GL call profiling lines on the serial console" OFF)
if(D3_GLPROF)
	add_definitions(-DD3_GLPROF=1)
	message(STATUS "D3_GLPROF: compact GL call profiling enabled")
endif()
if(BAREMETAL)
	add_definitions(-DBAREMETAL)
	# newlib has no LFS64 (fopen64/ftello64/fseeko64); use minizip's 32-bit FILE API.
	add_definitions(-DIOAPI_NO_64)
	message(STATUS "BAREMETAL: bare-metal RISC-V64 cross build (Path B: SDL3-on-dOS)")

	# Force the only supported feature matrix for the bare-metal target.
	set(BASE          ON  CACHE BOOL "" FORCE)   # hardlinked base game
	set(D3XP          OFF CACHE BOOL "" FORCE)
	set(SDL2          OFF CACHE BOOL "" FORCE)
	set(SDL3          ON  CACHE BOOL "" FORCE)   # Path B keeps SDL3 ON (prebuilt .a)
	set(IMGUI         OFF CACHE BOOL "" FORCE)   # -> -DIMGUI_DISABLE, C++98 default
	set(DEDICATED     OFF CACHE BOOL "" FORCE)
	set(HARDLINK_GAME ON  CACHE BOOL "" FORCE)   # no dlopen on bare metal
	set(ONATIVE       OFF CACHE BOOL "" FORCE)   # bare-metal toolchain -march must win
	set(TRACY         OFF CACHE BOOL "" FORCE)
	set(REPRODUCIBLE_BUILD ON CACHE BOOL "" FORCE)

	# Locations of the prebuilt static deps (do NOT rebuild; see riscv/dos/Makefile).
	# Overridable with -D on the cmake line.
	if(NOT DEFINED BAREMETAL_RISCV_DIR)
		set(BAREMETAL_RISCV_DIR "${CMAKE_SOURCE_DIR}/../riscv")
	endif()
	set(BAREMETAL_DEFER_PRECACHE "0" CACHE STRING "bare-metal demo: defer render-model/sound/GUI precache to first-use to speed map load")
	if(NOT BAREMETAL_DEFER_PRECACHE MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_DEFER_PRECACHE must be 0 or 1")
	endif()
	set(BAREMETAL_QUIET_ASSET_LOADS "0" CACHE STRING "bare-metal demo: suppress high-volume per-asset load logs")
	if(NOT BAREMETAL_QUIET_ASSET_LOADS MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_QUIET_ASSET_LOADS must be 0 or 1")
	endif()
	set(BAREMETAL_IO_STATS "0" CACHE STRING "bare-metal demo: print low-frequency I/O timing summaries")
	if(NOT BAREMETAL_IO_STATS MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_IO_STATS must be 0 or 1")
	endif()
	set(BAREMETAL_SIMD_BENCH "0" CACHE STRING "bare-metal demo: run a small idSIMDProcessor microbench after init")
	if(NOT BAREMETAL_SIMD_BENCH MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_SIMD_BENCH must be 0 or 1")
	endif()
	set(BAREMETAL_BOOT_TO_MENU "0" CACHE STRING "bare-metal demo: drop +map and boot to the main-menu GUI (reachable capture target)")
	if(NOT BAREMETAL_BOOT_TO_MENU MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_BOOT_TO_MENU must be 0 or 1")
	endif()
	set(BAREMETAL_RENDER_PROF "0" CACHE STRING "bare-metal demo: enable softgles2 per-stage [rprof] + [fssrc]/[fsdump]/[fswork] shader-workload profiler (Phase 0 archetype discovery)")
	if(NOT BAREMETAL_RENDER_PROF MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_RENDER_PROF must be 0 or 1")
	endif()
	set(BAREMETAL_BENCH "0" CACHE STRING "bare-metal DETERMINISTIC BENCH mode: pin the camera to a fixed vantage and freeze game time (g_stopTime) a few frames after the map spawns, so every frame renders identical geometry -> reliable renderer A/B (kills the ~18% frame-to-frame noise from md5 actor animation + scenicWarp). Not for captures/gameplay.")
	if(NOT BAREMETAL_BENCH MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_BENCH must be 0 or 1")
	endif()
	# On-screen stats/fps HUD, drawn into the framebuffer at present. Only sets
	# the BOOT DEFAULT of bm_sglOverlay -- the cvar is live, so it can be toggled
	# from the console without a rebuild. Off by default: it turns the render
	# profiler on to source its counters, and it paints over the frame, so it
	# must never be on for a capture or an [fbhash] A/B.
	set(BAREMETAL_SGL_OVERLAY "0" CACHE STRING "bare-metal demo: boot with the softgles2 stats/fps overlay enabled")
	if(NOT BAREMETAL_SGL_OVERLAY MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_SGL_OVERLAY must be 0 or 1")
	endif()
	# FrameBin A/B controls. FrameBin defers programmable draws and clears to a
	# per-tile replay at flush; it is meant to be PIXEL-IDENTICAL to immediate
	# mode, so the way to validate it is to build it both ways and compare frame
	# digests from otherwise identical runs:
	#   -DBAREMETAL_FRAMEBIN=1 -DBAREMETAL_FB_HASH=1   (deferred)
	#   -DBAREMETAL_FRAMEBIN=0 -DBAREMETAL_FB_HASH=1   (immediate, the reference)
	# then diff the [fbhash] lines.
	set(BAREMETAL_FRAMEBIN "1" CACHE STRING "bare-metal: defer draws into per-tile bins (0 = immediate mode)")
	if(NOT BAREMETAL_FRAMEBIN MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_FRAMEBIN must be 0 or 1")
	endif()
	# Post-transform vertex dedupe. Supposed to be pixel-neutral (the transform is
	# a pure function of index and uniforms, so computing once and copying gives
	# the same bits), so it is validated the same way FrameBin is: build both, diff
	# the [fbhash] lines.
	set(BAREMETAL_VS_DEDUP "1" CACHE STRING "bare-metal: dedupe indices before the SIMT vertex transform (0 = off)")
	if(NOT BAREMETAL_VS_DEDUP MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_VS_DEDUP must be 0 or 1")
	endif()
	set(BAREMETAL_FB_HASH "0" CACHE STRING "bare-metal: print an [fbhash] digest of every presented frame")
	if(NOT BAREMETAL_FB_HASH MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_FB_HASH must be 0 or 1")
	endif()
	# Fixed game timestep. WITHOUT this, frame content is a function of wall-clock
	# time, so two builds that run at different speeds render DIFFERENT content at
	# the same frame index and a digest comparison between them is meaningless --
	# it reports a difference that is really just "one arm was faster". Measured:
	# a FrameBin A/B on the animated menu matched for the static opening frames and
	# then diverged the moment animation started, which is indistinguishable from a
	# real rendering bug until you control the clock.
	# Defaults ON whenever frame hashing is on, because the two are only useful
	# together.
	set(BAREMETAL_FIXED_TIC "${BAREMETAL_FB_HASH}" CACHE STRING "bare-metal: com_fixedTic (fixes usercmds/frame; NOT sufficient for deterministic frames on its own)")
	# Makes frame content a function of frame INDEX rather than wall clock, which
	# is what a cross-run [fbhash] comparison needs -- com_fixedTic alone does not
	# achieve it (com_ticNumber still comes from real time). Engine-reported frame
	# times become fictional under this, so leave it off for perf runs.
	set(BAREMETAL_DETERMINISTIC_CLOCK "${BAREMETAL_FB_HASH}" CACHE STRING "bare-metal: virtual clock advancing one tic per presented frame")
	if(NOT BAREMETAL_DETERMINISTIC_CLOCK MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_DETERMINISTIC_CLOCK must be 0 or 1")
	endif()
	# Directory-wide, NOT target_compile_definitions(bm_boot ...): the clock lives
	# in riscv/engine/sys_dos.cpp, which is part of src_sys_base -> the engine
	# BINARY, not bm_boot. Defining it only on bm_boot compiles sys_dos.cpp with
	# the fallback 0 and the feature silently does nothing -- which is exactly
	# what happened on the first attempt, and the only symptom was a weak
	# BM_VClockTick resolving to a null pointer at the call site.
	add_compile_definitions("BM_DETERMINISTIC_CLOCK=${BAREMETAL_DETERMINISTIC_CLOCK}")
	# Directory-wide (not bm_boot-scoped) for the same reason as the clock above:
	# the bench trigger lives in riscv/engine/sys_dos.cpp, part of the engine BINARY.
	add_compile_definitions("BM_BENCH=${BAREMETAL_BENCH}")
	# SIMD correctness+timing micro-bench: boots pre-map, runs idSIMD_RVV vs
	# idSIMD_Generic diff + timing, powers off. Directory-wide (sys_dos.cpp).
	set(BAREMETAL_SIMDTEST "0" CACHE STRING "bare-metal: run the idSIMD_RVV vs idSIMD_Generic correctness+timing harness at boot, then power off")
	if(NOT BAREMETAL_SIMDTEST MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_SIMDTEST must be 0 or 1")
	endif()
	add_compile_definitions("BM_SIMDTEST=${BAREMETAL_SIMDTEST}")
	# Quality preset the bare-metal init pins (framework/Common.cpp forces this
	# twice, since the large detected heap would otherwise classify as Ultra and
	# the second pass would re-clobber any cfg/+set). 0=Low (default, cheapest for
	# the software rasteriser), 3=Ultra (full-res uncompressed textures). Higher
	# specs cost RAM/load-time and per-frame sample bandwidth; anisotropy/MSAA the
	# preset requests are no-ops in softgles2. Directory-wide (Common.cpp is engine).
	set(BAREMETAL_MACHINE_SPEC "0" CACHE STRING "bare-metal com_machineSpec pinned at init: 0=Low 1=Medium 2=High 3=Ultra")
	if(NOT BAREMETAL_MACHINE_SPEC MATCHES "^[0-3]$")
		message(FATAL_ERROR "BAREMETAL_MACHINE_SPEC must be 0..3")
	endif()
	add_compile_definitions("BM_MACHINE_SPEC=${BAREMETAL_MACHINE_SPEC}")
	if(NOT BAREMETAL_FIXED_TIC MATCHES "^[01]$")
		message(FATAL_ERROR "BAREMETAL_FIXED_TIC must be 0 or 1")
	endif()
	# Framebuffer resolution ladder for TCG iteration. Width should be a multiple
	# of SGL_SIMT_LANES (VLEN/16); height a multiple of SGL_MT_TILE_H (lanes/4).
	# Defaults keep the shipping 768x432 (16:9, VLEN=1024 / 64-lane aligned).
	set(BAREMETAL_FB_WIDTH "768" CACHE STRING "bare-metal demo: r_customWidth (SIMT-aligned)")
	set(BAREMETAL_FB_HEIGHT "432" CACHE STRING "bare-metal demo: r_customHeight (MT-tile-aligned)")
	if(NOT BAREMETAL_FB_WIDTH MATCHES "^[0-9]+$" OR NOT BAREMETAL_FB_HEIGHT MATCHES "^[0-9]+$")
		message(FATAL_ERROR "BAREMETAL_FB_WIDTH/HEIGHT must be positive integers")
	endif()
	# Supersampled anti-aliasing. BAREMETAL_FB_WIDTH/HEIGHT are the DISPLAY
	# resolution (the virtio-gpu scanout); with SSAA>1 the engine renders at
	# render-res = display * SSAA and dos_present.c box-downsamples back to the
	# display size. BM_FB_WIDTH/HEIGHT (which drive r_customWidth/Height and the
	# GL context size) therefore carry the supersampled render size, and BM_SSAA
	# carries the factor so the payload knows how much to shrink at present.
	# SSAA=1 is bit-identical to the old behaviour (render == display).
	set(BAREMETAL_SSAA "1" CACHE STRING "bare-metal demo: supersampling factor (1..4)")
	if(NOT BAREMETAL_SSAA MATCHES "^[1-4]$")
		message(FATAL_ERROR "BAREMETAL_SSAA must be 1..4")
	endif()
	math(EXPR BM_RENDER_WIDTH "${BAREMETAL_FB_WIDTH} * ${BAREMETAL_SSAA}")
	math(EXPR BM_RENDER_HEIGHT "${BAREMETAL_FB_HEIGHT} * ${BAREMETAL_SSAA}")
	# dhewm3 overwrites CMAKE_*_FLAGS_RELWITHDEBINFO further down and drops CMake's
	# usual -DNDEBUG, so the shipped bare-metal ELF carries ~2900 live assert()
	# call sites (idList/idVec3/idMat3 bounds checks inlined all over the frame and
	# game-tick paths). Restore it for the bare-metal image only -- desktop dhewm3
	# keeps upstream behaviour. Verified there are no asserts with side effects
	# (assignment or ++/--) in neo/ outside the vendored libs, so this removes
	# checks and nothing else. -g is unaffected; DWARF is still emitted.
	add_compile_definitions(NDEBUG)

	get_filename_component(BAREMETAL_RISCV_DIR "${BAREMETAL_RISCV_DIR}" ABSOLUTE)
	set(BM_SDL3_INCLUDE "${BAREMETAL_RISCV_DIR}/sdl3/include")
	set(BM_SDL3_LIB     "${BAREMETAL_RISCV_DIR}/sdl3/libSDL3-riscv.a")
	if(BM_VECTOR)
		set(BM_GL4ES_LIB "${BAREMETAL_RISCV_DIR}/gl4es/lib/libGL-riscv.a")
	else()
		# Scalar target: must not pick up the vector archive (see build-gl4es.sh).
		set(BM_GL4ES_LIB "${BAREMETAL_RISCV_DIR}/gl4es/lib/libGL-riscv-scalar.a")
	endif()
	set(BM_GL4ES_INCLUDE "${BAREMETAL_RISCV_DIR}/gl4es/include")
	set(BM_SGL_DIR      "${BAREMETAL_RISCV_DIR}/softgles2")
	set(BM_BRIDGE_DIR   "${BAREMETAL_RISCV_DIR}/gl4es-bridge")
	set(BM_BOOT_DIR     "${BAREMETAL_RISCV_DIR}/boot")
	set(BM_DOS_DIR      "${BAREMETAL_RISCV_DIR}/dos")
	set(BM_ENGINE_DIR   "${BAREMETAL_RISCV_DIR}/engine")
	set(BM_AUDIO_DIR    "${BAREMETAL_RISCV_DIR}/audio")

	foreach(f "${BM_SDL3_LIB}" "${BM_GL4ES_LIB}")
		if(NOT EXISTS "${f}")
			message(WARNING "BAREMETAL: prebuilt lib not found: ${f} (build it per riscv/dos/Makefile / build-gl4es.sh)")
		endif()
	endforeach()

	# riscv/engine/{sys_dos,cxxrt}.cpp include "boot/runtime.h" (and the AL/ stub
	# headers live under riscv/engine/include) -> put riscv/ on every TU's path.
	include_directories("${BAREMETAL_RISCV_DIR}")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR)
	message(FATAL_ERROR "No target CPU architecture set")
endif()

if(NOT CMAKE_SYSTEM_NAME)
	message(FATAL_ERROR "No target OS set")
endif()

# target cpu
set(cpu ${CMAKE_SYSTEM_PROCESSOR})

# Originally, ${CMAKE_SYSTEM_PROCESSOR} was supposed to contain the *target* CPU, according to CMake's documentation.
# As far as I can tell this has always been broken (always returns host CPU) at least on Windows
# (see e.g. https://cmake.org/pipermail/cmake-developers/2014-September/011405.html) and wasn't reliable on
# other systems either, for example on Linux with 32bit userland but 64bit kernel it returned the kernel CPU type
# (e.g. x86_64 instead of i686). Instead of fixing this, CMake eventually updated their documentation in 3.20,
# now it's officially the same as CMAKE_HOST_SYSTEM_PROCESSOR except when cross-compiling (where it's explicitly set)
# So we gotta figure out the actual target CPU type ourselves.. (why am I sticking to this garbage buildsystem?)
if(NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR))
	# special case: cross-compiling, here CMAKE_SYSTEM_PROCESSOR should be correct, hopefully
	# (just leave cpu at ${CMAKE_SYSTEM_PROCESSOR})
elseif(MSVC)
	# because all this wasn't ugly enough, it turned out that, unlike standalone CMake, Visual Studio's
	# integrated CMake doesn't set CMAKE_GENERATOR_PLATFORM, so I gave up on guessing the CPU arch here
	# and moved the CPU detection to MSVC-specific code in neo/sys/platform.h
else() # not MSVC and not cross-compiling, assume GCC or clang (-compatible), seems to work for MinGW as well
	execute_process(COMMAND ${CMAKE_C_COMPILER} "-dumpmachine"
	                RESULT_VARIABLE cc_dumpmachine_res
	                OUTPUT_VARIABLE cc_dumpmachine_out)
	if(cc_dumpmachine_res EQUAL 0)
		string(STRIP ${cc_dumpmachine_out} cc_dumpmachine_out) # get rid of trailing newline
		message(STATUS "`${CMAKE_C_COMPILER} -dumpmachine` says: \"${cc_dumpmachine_out}\"")
		# gcc -dumpmachine and clang -dumpmachine seem to print something like "x86_64-linux-gnu" (gcc)
		# or "x64_64-pc-linux-gnu" (clang) or "i686-w64-mingw32" (32bit mingw-w64) i.e. starting with the CPU,
		# then "-" and then OS or whatever - so use everything up to first "-"
		string(REGEX MATCH "^[^-]+" cpu ${cc_dumpmachine_out})
		message(STATUS "  => CPU architecture extracted from that: \"${cpu}\"")
	else()
		message(WARNING "${CMAKE_C_COMPILER} -dumpmachine failed with error (code) ${cc_dumpmachine_res}")
		message(WARNING "will use the (sometimes incorrect) CMAKE_SYSTEM_PROCESSOR (${cpu}) to determine D3_ARCH")
	endif()
endif()

if(cpu STREQUAL "powerpc")
	set(cpu "ppc")
elseif(cpu STREQUAL "aarch64")
	# "arm64" is more obvious, and some operating systems (like macOS) use it instead of "aarch64"
	set(cpu "arm64")
elseif(cpu MATCHES "i.86")
	set(cpu "x86")
elseif(cpu MATCHES "[aA][mM][dD]64" OR cpu MATCHES "[xX]64")
	set(cpu "x86_64")
elseif(cpu MATCHES "[aA][rR][mM].*") # some kind of arm..
	# On 32bit Raspbian gcc -dumpmachine returns sth starting with "arm-",
	# while clang -dumpmachine says "arm6k-..." - try to unify that to "arm"
	if(CMAKE_SIZEOF_VOID_P EQUAL 8) # sizeof(void*) == 8 => must be arm64
		set(cpu "arm64")
	else() # should be 32bit arm then (probably "armv7l" "armv6k" or sth like that)
		set(cpu "arm")
	endif()
endif()

# target os
if(APPLE)
	set(os "macosx")
else()
	string(TOLOWER "${CMAKE_SYSTEM_NAME}" os)
endif()

add_definitions(-DD3_OSTYPE="${os}" -DD3_SIZEOFPTR=${CMAKE_SIZEOF_VOID_P})

if(MSVC)
	# for MSVC D3_ARCH is set in code (in neo/sys/platform.h)
	message(STATUS "Setting -DD3_SIZEOFPTR=${CMAKE_SIZEOF_VOID_P} -DD3_OSTYPE=\"${os}\" - NOT setting D3_ARCH, because we're targeting MSVC (VisualC++)")
	# make sure ${cpu} isn't (cant't be) used by CMake when building with MSVC
	unset(cpu)
else()
	add_definitions(-DD3_ARCH="${cpu}")
	message(STATUS "Setting -DD3_ARCH=\"${cpu}\" -DD3_SIZEOFPTR=${CMAKE_SIZEOF_VOID_P} -DD3_OSTYPE=\"${os}\" ")

	if(cpu MATCHES ".*64.*" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
		# tough luck if some CPU architecture has "64" in its name but uses 32bit pointers
		message(SEND_ERROR "CMake thinks sizeof(void*) == 4, but the target CPU ${cpu} looks like a 64bit CPU!")
		message(FATAL_ERROR "If you're building in a 32bit chroot on a 64bit host, switch to it with 'linux32 chroot' or at least call cmake with linux32 (or your OSs equivalent)!")
	endif()
endif()

# build type
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# precompiled libraries from the dhewm3-libs repo
if(DHEWM3LIBS)
	if(CMAKE_CROSSCOMPILING)
		set(CMAKE_FIND_ROOT_PATH ${DHEWM3LIBS})
	else()
		set(ENV{CMAKE_PREFIX_PATH} ${DHEWM3LIBS})
	endif()

	# these are too stupid, give them a hint
	set(ENV{OPENALDIR} ${DHEWM3LIBS})
	set(ENV{SDLDIR} ${DHEWM3LIBS})
	set(ENV{SDL2DIR} ${DHEWM3LIBS})
endif()

if(CMAKE_MAJOR_VERSION LESS 3 OR ( CMAKE_MAJOR_VERSION EQUAL 3 AND CMAKE_MINOR_VERSION LESS 8 ))
	# cmake < 3.8 doesn't support source_group(TREE ...) so replace it with a dummy
	# (it's only cosmetical anyway, to make source files show up properly in Visual Studio)
	function(source_group)
	endfunction()
	message(STATUS "Using CMake < 3.8, doesn't support source_group(TREE ...), replacing it with a dummy")
	message(STATUS "  (this is only relevants for IDEs, doesn't matter for just compiling dhewm3)")
#else()
#	message(STATUS "Using CMake >= 3.8, supports source_group(TREE ...)")
endif()

# libs
if(BAREMETAL)
	# No system OpenAL on bare metal: the openal_stub.cpp provides the AL_* impls.
	# Headers (AL/al.h etc.) come from a vendored set under riscv/engine/include.
	# (find_package(OpenAL REQUIRED) would FATAL under CMAKE_SYSTEM_NAME=Generic.)
	set(OPENAL_LIBRARY "")
	include_directories("${BM_ENGINE_DIR}/include")  # vendored AL/ headers, if present
else()
	find_package(OpenAL REQUIRED)
	include_directories(${OPENAL_INCLUDE_DIR})
endif()

if(BAREMETAL)
	# Path B keeps SDL3 ON but uses the PREBUILT static libSDL3-riscv.a + headers
	# directly (the find_package(SDL3 ... COMPONENTS SDL3-shared) CONFIG lookup
	# would fail for a static, non-installed SDL3). idlib + every target just need
	# the headers on the include path and the .a in the link group (added later).
	include_directories(SYSTEM "${BM_SDL3_INCLUDE}")
	set(SDLx_LIBRARY "${BM_SDL3_LIB}")
	add_definitions(-DD3_SDL3=1)
elseif (SDL3)
	if(SDL2)
		message(WARNING "You enabled both SDL2 and SDL3. Only one can be used at a time, disabling SDL2")
		set(SDL2 OFF CACHE BOOL "Use SDL2 (make sure SDL3 is disabled!)" FORCE)
	endif()
	# 1. Look for a SDL3 package,
	# 2. look for the SDL3-shared component, and
	# 3. fail if the shared component cannot be found.
	find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3-shared)
	# TODO: include dirs?
	set(SDLx_LIBRARY SDL3::SDL3)
	add_definitions(-DD3_SDL3=1)
elseif (SDL2)
	# skip SDL2main
	if(APPLE OR WIN32)
		set(SDL2_BUILDING_LIBRARY TRUE)
	endif()
	find_package(SDL2 REQUIRED)
	include_directories(${SDL2_INCLUDE_DIR})
	set(SDLx_LIBRARY ${SDL2_LIBRARY})
else()
	# skip SDLmain
	if(APPLE OR WIN32)
		set(SDL_BUILDING_LIBRARY TRUE)
	endif()
	find_package(SDL REQUIRED)
	include_directories(${SDL_INCLUDE_DIR})
	set(SDLx_LIBRARY ${SDL_LIBRARY})
endif()

if(REPRODUCIBLE_BUILD)
	# don't use __DATE__ and __TIME__ macros so builds are reproducible
	add_definitions(-DID_REPRODUCIBLE_BUILD)
endif()

if(IMGUI)
	if(SDL2 OR SDL3)
		# we need C++11 for ImGui
		set (CMAKE_CXX_STANDARD 11)
		message(STATUS "Dear ImGui integration enabled")
		if(AROS)
			add_definitions(-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
		endif()
	else()
		message(WARNING "Disabling IMGUI because SDL1.2 is used - it needs SDL2 or SDL3!")
		set(IMGUI OFF)
		add_definitions(-DIMGUI_DISABLE)
	endif()
else()
	message(STATUS "Dear ImGui integration disabled")
	add_definitions(-DIMGUI_DISABLE)
endif()

if(NOT IMGUI)
	# without ImGui C++98 should suffices, preserves compatibility
	# with vintage system like PPC Macs
	set (CMAKE_CXX_STANDARD 98)
endif()

if(BAREMETAL)
	# The bare-metal engine + SDL3 headers + threads.cpp static_assert need C++11.
	# (IMGUI is OFF, which would otherwise drop us to C++98.)
	set(CMAKE_CXX_STANDARD 11)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(DEFINED ENV{MSYSTEM})
	message(STATUS "Detected MSYS or similar environment. Using alternate curl path")
	set(CURL_NO_CURL_CMAKE ON)
endif()

if(BAREMETAL)
	# Never use the HOST libcurl when cross-compiling for bare metal (it would be
	# found via the host sysroot and wrongly enable ID_ENABLE_CURL / link host .so).
	set(CURL_FOUND OFF)
endif()
if(NOT BAREMETAL)
	find_package(CURL QUIET)
endif()
if(CURL_FOUND)
	set(ID_ENABLE_CURL ON)
	include_directories(${CURL_INCLUDE_DIR})
	message(STATUS "libcurl found and enabled")
else()
	message(WARNING "libcurl not found, server downloads won't be available (apart from that dhewm3 will work)")
	set(ID_ENABLE_CURL OFF)
	set(CURL_LIBRARY "")
endif()

if(TRACY)
	if(CMAKE_MAJOR_VERSION LESS 3 OR ( CMAKE_MAJOR_VERSION EQUAL 3 AND CMAKE_MINOR_VERSION LESS 11 ))
		message(WARNING "To build with support for Tracy, CMake 3.11 or newer is required (for the FetchContent module)!")
		unset(TRACY)
	else()
		add_definitions(-DTRACY_ENABLE=1)
		# on demand has some overhead per sample, but when it's disabled and dhewm3 is started
		# without the viewer it'll cache all the measurement data until the viewer connects
		# which will eat lots of RAM (growing the longer the game runs)
		add_definitions(-DTRACY_ON_DEMAND=1)
		option(TRACY_ENABLE "" ON)
		include(FetchContent)
		FetchContent_Declare(
			tracy
			GIT_REPOSITORY https://github.com/wolfpld/tracy.git
			GIT_TAG v0.12.2
			#GIT_TAG master
			GIT_SHALLOW TRUE
			GIT_PROGRESS TRUE
		)
		FetchContent_MakeAvailable(tracy)

		# make sure tracy/Tracy.hpp can be included everywhere, not just in dhewm3(ded) source
		FetchContent_GetProperties(tracy SOURCE_DIR tracy_src)
		include_directories("${tracy_src}/public")

		# link it into the executables
		set(sys_libs ${sys_libs} TracyClient)

		# to get calltraces for every sampled "zone" (expensive!)
		#add_definitions(-DTRACY_CALLSTACK=10)
	endif()
endif()


set(D3_COMPILER_IS_CLANG FALSE)
set(D3_COMPILER_IS_GCC_OR_CLANG FALSE)

if(MSVC)
	# This is required for tools on windows.
	find_package(MFC)
	
	if(TOOLS AND NOT MFC_FOUND)
		message(SEND_ERROR "MFC ('Microsoft Foundation Classes for C++') couldn't be found, but is needed for TOOLS!")
		message(FATAL_ERROR "If you're using VS2013, you'll also need the 'Multibyte MFC Library for Visual Studio 2013': https://www.microsoft.com/en-us/download/details.aspx?id=40770 (VS2015 and 2017 include that in the default MFC package)")
	endif()

else() # not MSVC

if(NOT WIN32)
	# libbacktrace support - TODO: might work with MinGW? we don't have a crash handler for win32 though..
	include(CheckCSourceCompiles)
	set(CMAKE_REQUIRED_LIBRARIES backtrace)
	check_c_source_compiles( "#include <backtrace.h>
	int main() { backtrace_create_state(NULL, 0, NULL, NULL); return 0; }" HAVE_LIBBACKTRACE )
	unset(CMAKE_REQUIRED_LIBRARIES)

	if(HAVE_LIBBACKTRACE)
		set(sys_libs ${sys_libs} backtrace)
		add_definitions(-DD3_HAVE_LIBBACKTRACE)
		message(STATUS "Using libbacktrace")
	else()
		message(WARNING "libbacktrace wasn't found. It's not required but recommended, because it provides useful backtraces if dhewm3 crashes")
	endif()
endif() # NOT WIN32

# check if this is some kind of clang (Clang, AppleClang, whatever)
# (convert compiler ID to lowercase so we match Clang, clang, AppleClang etc, regardless of case)
string(TOLOWER ${CMAKE_CXX_COMPILER_ID} compiler_id_lower)
if(compiler_id_lower MATCHES ".*clang.*")
	message(STATUS "Compiler \"${CMAKE_CXX_COMPILER_ID}\" detected as some kind of clang")
	set(D3_COMPILER_IS_CLANG TRUE)
	set(D3_COMPILER_IS_GCC_OR_CLANG TRUE)
elseif(CMAKE_COMPILER_IS_GNUCC)
	set(D3_COMPILER_IS_GCC_OR_CLANG TRUE)
endif()
unset(compiler_id_lower)

endif() # not MSVC

TEST_BIG_ENDIAN(is_big_endian)
if(is_big_endian)
	message(STATUS "Detected Big Endian architecture, setting -DD3_IS_BIG_ENDIAN=1")
	add_definitions(-DD3_IS_BIG_ENDIAN=1)
else()
	message(STATUS "Detected Little Endian architecture, setting -DD3_IS_BIG_ENDIAN=0")
	add_definitions(-DD3_IS_BIG_ENDIAN=0)
endif()

# compiler specific flags
if(D3_COMPILER_IS_GCC_OR_CLANG)
	add_compile_options(-pipe)
	add_compile_options(-Wall)

	if(BAREMETAL)
		# Do NOT inject any -march/-mtune: the bare-metal toolchain's explicit
		# rva23s64 profile, -mabi=lp64d and -mcmodel=medany must win (already true
		# since this is a cross build with
		# ONATIVE off and cpu!=x86, but make it explicit and future-proof).
	elseif(NOT CMAKE_CROSSCOMPILING AND ONATIVE)
		add_compile_options(-march=native)
	elseif(NOT APPLE AND cpu STREQUAL "x86")
		add_compile_options(-march=pentium3)
	endif()

	if(FORCE_COLORED_OUTPUT)
		if(CMAKE_COMPILER_IS_GNUCC)
		   add_compile_options (-fdiagnostics-color=always)
		elseif (D3_COMPILER_IS_CLANG)
		   add_compile_options (-fcolor-diagnostics)
		endif ()
	endif ()

	if(cpu STREQUAL "e2k" AND CMAKE_COMPILER_IS_GNUCC)
		# O3 on E2K mcst-lcc approximately equal to O2 at X86/ARM gcc
		set(OPT_LEVEL "-O3")
	else()
		set(OPT_LEVEL "-O2")
	endif()

	set(CMAKE_C_FLAGS_DEBUG "-g -ggdb -D_DEBUG -O0")
	set(CMAKE_C_FLAGS_DEBUGALL "-g -ggdb -D_DEBUG")
	set(CMAKE_C_FLAGS_PROFILE "-g -ggdb -D_DEBUG -O1 -fno-omit-frame-pointer")
	set(CMAKE_C_FLAGS_RELEASE "${OPT_LEVEL} -fomit-frame-pointer")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO "-g -ggdb ${OPT_LEVEL} -fno-omit-frame-pointer")
	set(CMAKE_C_FLAGS_MINSIZEREL "-Os -fomit-frame-pointer")

	set(CMAKE_CXX_FLAGS_DEBUGALL ${CMAKE_C_FLAGS_DEBUGALL})
	set(CMAKE_CXX_FLAGS_PROFILE ${CMAKE_C_FLAGS_PROFILE})

	# these options (together with -ffinite-math-only) should make compilers use direct
	# CPU instructions (if available) instead of libm calls for many math operations
	add_compile_options(-fno-math-errno -fno-trapping-math)
	if( (NOT cpu STREQUAL "arm64") OR APPLE OR WIN32 )
		# RPi5 on Linux has a weird bug with -ffinite-math-only (#749)
		# so only enable it for other CPU architectures or Apple or Windows ARM64 devices,
		# that most probably use different CPUs that hopefully don't have this bug
		add_compile_options(-ffinite-math-only)
	endif()

	# Doom3 code wouldn't work with strict aliasing optimizations
	add_compile_options(-fno-strict-aliasing)

	# dear idiot compilers, don't fuck up math code with useless FMA "optimizations"
	# (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100839)
	CHECK_CXX_COMPILER_FLAG("-ffp-contract=off" cxx_has_fp-contract)
	if(cxx_has_fp-contract)
		add_compile_options(-ffp-contract=off)
	endif()

	if(ASAN)
		# if this doesn't work, ASan might not be available on your platform, don't set ASAN then..
		add_compile_options(-fsanitize=address)
		set(sys_libs ${sys_libs} -fsanitize=address)
	endif()
	if(UBSAN)
		# if this doesn't work, UBSan might not be available on your platform, don't set UBSAN then..
		add_compile_options(-fsanitize=undefined)
		set(sys_libs ${sys_libs} -fsanitize=undefined)
	endif()

	if(NOT AROS AND NOT WIN32)
		CHECK_CXX_COMPILER_FLAG("-fvisibility=hidden" cxx_has_fvisibility)
		if(NOT cxx_has_fvisibility)
			message(FATAL_ERROR "Compiler does not support -fvisibility")
		endif()
		add_compile_options(-fvisibility=hidden)
	endif()

	# TODO fix these warnings
	add_compile_options(-Wno-sign-compare)
	add_compile_options(-Wno-switch)

	CHECK_CXX_COMPILER_FLAG("-Wdangling-reference" cxx_has_Wdangling_reference)
	if(cxx_has_Wdangling_reference)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=dangling-reference")
	endif()

	CHECK_CXX_COMPILER_FLAG("-Woverloaded-virtual" cxx_has_Woverload_virtual)
	if(cxx_has_Woverload_virtual)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Woverloaded-virtual")
	endif()
	
	# shut up about using memcpy() on classes, in the cases doom3 uses it it seems to be fine
	CHECK_CXX_COMPILER_FLAG("-Wno-class-memaccess" cxx_has_Wno-class-memaccess)
	if(cxx_has_Wno-class-memaccess)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-class-memaccess")
	endif()

	# ignore warnings about variables named "requires" for now (in C++20 it's a keyword,
	#  but currently we barely use C++11 features)
	CHECK_CXX_COMPILER_FLAG("-Wno-c++20-compat" cxx_has_Wno-cpp20-compat)
	if(cxx_has_Wno-cpp20-compat)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++20-compat")
	endif()

	if(AROS)
		set(CMAKE_SHARED_LIBRARY_SUFFIX ".aros-${cpu}")
		add_definitions(-DIOAPI_NO_64)
	elseif(APPLE)
		add_definitions(-DMACOS_X=1)

		if(cpu STREQUAL "x86_64")
			add_compile_options(-arch x86_64 -mmacosx-version-min=10.7)
			set(ldflags "${ldflags} -arch x86_64 -mmacosx-version-min=10.7")
		elseif(cpu STREQUAL "arm64")
			add_compile_options(-arch arm64 -mmacosx-version-min=10.7)
			set(ldflags "${ldflags} -arch arm64 -mmacosx-version-min=10.7")
		elseif(cpu STREQUAL "x86")
			CHECK_CXX_COMPILER_FLAG("-arch i386" cxx_has_arch_i386)
			if(cxx_has_arch_i386)
				add_compile_options(-arch i386)
				set(ldflags "${ldflags} -arch i386")
			endif()

			add_compile_options(-mmacosx-version-min=10.4)
			set(ldflags "${ldflags} -mmacosx-version-min=10.4")
		elseif(cpu STREQUAL "ppc")
			CHECK_CXX_COMPILER_FLAG("-arch ppc" cxx_has_arch_ppc)
			if(cxx_has_arch_ppc)
				add_compile_options(-arch ppc -mone-byte-bool)
				set(ldflags "${ldflags} -arch ppc -mone-byte-bool")
			endif()

			add_compile_options(-mmacosx-version-min=10.4)
			set(ldflags "${ldflags} -mmacosx-version-min=10.4")
		else()
			message(FATAL_ERROR "Unsupported CPU architecture for OSX")
		endif()

		set(sys_libs ${sys_libs} "-framework Carbon -framework Cocoa -framework IOKit")
	elseif(WIN32)
		# use 8MB stack instead of 1MB, so big models don't overflow it with _alloca()
		set(ldflags "${ldflags} -static-libgcc -static-libstdc++ -Wl,--stack,8388608")
	elseif(os STREQUAL "linux")
		set(sys_libs ${sys_libs} dl)
	elseif(os STREQUAL "freebsd")
		find_library(EXECINFO_LIBRARIES execinfo /usr/lib /usr/local/lib)
		set(sys_libs ${sys_libs} ${EXECINFO_LIBRARIES})
	endif()
elseif(MSVC)
	add_compile_options(/MP) # parallel build (use all cores, or as many as configured in VS)
	
	add_compile_options(/W3) # TODO: was /W4, caused trouble with VS2019 (and/or its integrated CMake? or only HarrieVG's setup?)
	add_compile_options(/we4840) # treat as error when passing a class to a vararg-function (probably printf-like)
	# treat several kinds of truncating int<->pointer conversions as errors (for more 64bit-safety)
	add_compile_options(/we4306 /we4311 /we4312 /we4302)
	# ignore the following warnings:
	add_compile_options(/wd4100) # unreferenced formal parameter
	add_compile_options(/wd4127) # conditional expression is constant
	add_compile_options(/wd4244) # possible loss of data
	add_compile_options(/wd4245) # signed/unsigned mismatch
	add_compile_options(/wd4267) # possible loss of data
	add_compile_options(/wd4714) # 'function' marked as __forceinline not inlined
	add_compile_options(/wd4996) # 'function': was declared deprecated
	add_compile_options(/wd4068) # unknown pragma
	add_compile_options(/wd4458) # declaration of 'variable' hides class member
	add_definitions(-D_ALLOW_KEYWORD_MACROS) # because of the "#define private public" and "#define protected public" in TypeInfo.cpp
	set(CMAKE_C_FLAGS_DEBUG "-D_DEBUG /Od /Zi /MDd")
	set(CMAKE_C_FLAGS_RELEASE "/Ox /Oy /MD")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO "/Ox /Oy /Zi /MD")
	set(CMAKE_C_FLAGS_MINSIZEREL "/Ox /Oy /Os /MD")
	# use 8MB stack instead of 1MB, so big models don't overflow it with _alloca()
	set(ldflags "${ldflags} /STACK:8388608")
else()
	message(FATAL_ERROR "Unsupported compiler")
endif()

set(CMAKE_CXX_FLAGS_DEBUG ${CMAKE_C_FLAGS_DEBUG})
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO ${CMAKE_C_FLAGS_RELWITHDEBINFO})
# disable assertions in release builds
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG ${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-DNDEBUG ${CMAKE_C_FLAGS_MINSIZEREL}")

# mingw and msvc
if(WIN32)
	add_definitions(-DWINVER=0x0501)
	add_definitions(-D_WIN32_WINNT=0x0501)

	set(sys_libs ${sys_libs}
		winmm
		iphlpapi
		wsock32
		ole32
	)
endif()

# optimize this source file even in debug builds, to make speed (esp. loading times) more bearable
# used for libs integrated here like stb_* and miniz(ip)
if(CMAKE_MAJOR_VERSION LESS 3 OR ( CMAKE_MAJOR_VERSION EQUAL 3 AND CMAKE_MINOR_VERSION LESS 11 ))
	function(always_optimize_sourcefile srcfilename)
		if(MSVC)
			set_source_files_properties(${srcfilename} PROPERTIES COMPILE_FLAGS "/Ox")
		else()
			set_source_files_properties(${srcfilename} PROPERTIES COMPILE_FLAGS "-O2")
		endif()
	endfunction()
else()
	function(always_optimize_sourcefile srcfilename)
		if(MSVC)
			set_source_files_properties(${srcfilename} PROPERTIES COMPILE_OPTIONS "/Ox")
		else()
			set_source_files_properties(${srcfilename} PROPERTIES COMPILE_OPTIONS "-O2")
		endif()
	endfunction()
endif()

# fallback for cmake versions without GNUInstallDirs
if(GNUINSTALLDIRS MATCHES "NOTFOUND")
	set(CMAKE_INSTALL_BINDIR		"bin"
		CACHE PATH "user executables (bin)")
	set(CMAKE_INSTALL_LIBDIR		"lib${LIB_SUFFIX}"
		CACHE PATH "object code libraries (lib${LIB_SUFFIX})")
	set(CMAKE_INSTALL_DATAROOTDIR	"share"
		CACHE PATH "read-only architecture-independent data root (share)")
	set(CMAKE_INSTALL_DATADIR		"${CMAKE_INSTALL_DATAROOTDIR}"
		CACHE PATH "read-only architecture-independent data (DATAROOTDIR)")

	mark_as_advanced(CMAKE_INSTALL_BINDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_DATAROOTDIR CMAKE_INSTALL_DATADIR)
	foreach(dir BINDIR LIBDIR DATAROOTDIR DATADIR)
		if(NOT IS_ABSOLUTE ${CMAKE_INSTALL_${dir}})
			set(CMAKE_INSTALL_FULL_${dir} "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_${dir}}")
		else()
			set(CMAKE_INSTALL_FULL_${dir} "${CMAKE_INSTALL_${dir}}")
		endif()
	endforeach()
endif()

set(bindir		"${CMAKE_INSTALL_FULL_BINDIR}")
set(libdir		"${CMAKE_INSTALL_FULL_LIBDIR}/dhewm3")
set(datadir		"${CMAKE_INSTALL_FULL_DATADIR}/dhewm3")

configure_file(
	"${CMAKE_SOURCE_DIR}/config.h.in"
	"${CMAKE_BINARY_DIR}/config.h"
)

if(NOT MSVC)
message(STATUS "Building ${CMAKE_BUILD_TYPE} for ${os}-${cpu}")
endif()

if(NOT APPLE AND NOT WIN32)
	message(STATUS "The install target will use the following directories:")
	message(STATUS "  Binary directory:  ${bindir}")
	message(STATUS "  Library directory: ${libdir}")
	message(STATUS "  Data directory:    ${datadir}")
endif()

set(src_renderer
	renderer/Cinematic.cpp
	renderer/GuiModel.cpp
	renderer/Image_files.cpp
	renderer/Image_init.cpp
	renderer/Image_load.cpp
	renderer/Image_process.cpp
	renderer/Image_program.cpp
	renderer/Interaction.cpp
	renderer/Material.cpp
	renderer/MegaTexture.cpp
	renderer/Model.cpp
	renderer/ModelDecal.cpp
	renderer/ModelManager.cpp
	renderer/ModelOverlay.cpp
	renderer/Model_beam.cpp
	renderer/Model_ase.cpp
	renderer/Model_liquid.cpp
	renderer/Model_lwo.cpp
	renderer/Model_ma.cpp
	renderer/Model_md3.cpp
	renderer/Model_md5.cpp
	renderer/Model_prt.cpp
	renderer/Model_sprite.cpp
	renderer/RenderEntity.cpp
	renderer/RenderSystem.cpp
	renderer/RenderSystem_init.cpp
	renderer/RenderWorld.cpp
	renderer/RenderWorld_demo.cpp
	renderer/RenderWorld_load.cpp
	renderer/RenderWorld_portals.cpp
	renderer/VertexCache.cpp
	renderer/draw_arb2.cpp
	renderer/draw_common.cpp
	renderer/tr_backend.cpp
	renderer/tr_deform.cpp
	renderer/tr_font.cpp
	renderer/tr_guisurf.cpp
	renderer/tr_light.cpp
	renderer/tr_lightrun.cpp
	renderer/tr_main.cpp
	renderer/tr_orderIndexes.cpp
	renderer/tr_polytope.cpp
	renderer/tr_render.cpp
	renderer/tr_rendertools.cpp
	renderer/tr_shadowbounds.cpp
	renderer/tr_stencilshadow.cpp
	renderer/tr_subview.cpp
	renderer/tr_trace.cpp
	renderer/tr_trisurf.cpp
	renderer/tr_turboshadow.cpp

	renderer/stblib_impls.c
)

always_optimize_sourcefile(renderer/stblib_impls.c)

# I'm a bit sloppy with headers and just glob them in..
# they're only handled in CMake at all so they turn up in Visual Studio solutions..

# globs all the headers from ${PATHPREFIX}/ and adds them to ${SRCLIST}
function(add_globbed_headers SRCLIST PATHPREFIX)
	file(GLOB_RECURSE tmp_hdrs RELATIVE "${CMAKE_SOURCE_DIR}" "${PATHPREFIX}/*.h")
	set(${SRCLIST} ${tmp_hdrs} ${${SRCLIST}} PARENT_SCOPE)
endfunction()

add_globbed_headers(src_renderer "renderer")

set(src_framework
	framework/CVarSystem.cpp
	framework/CmdSystem.cpp
	framework/Common.cpp
	framework/Compressor.cpp
	framework/Console.cpp
	framework/DemoFile.cpp
	framework/DeclAF.cpp
	framework/DeclEntityDef.cpp
	framework/DeclFX.cpp
	framework/DeclManager.cpp
	framework/DeclParticle.cpp
	framework/DeclPDA.cpp
	framework/DeclSkin.cpp
	framework/DeclTable.cpp
	framework/Dhewm3SettingsMenu.cpp
	framework/EditField.cpp
	framework/EventLoop.cpp
	framework/File.cpp
	framework/FileSystem.cpp
	framework/KeyInput.cpp
	framework/UsercmdGen.cpp
	framework/Session_menu.cpp
	framework/Session.cpp
	framework/async/AsyncClient.cpp
	framework/async/AsyncNetwork.cpp
	framework/async/AsyncServer.cpp
	framework/async/MsgChannel.cpp
	framework/async/NetworkSystem.cpp
	framework/async/ServerScan.cpp
	framework/miniz/miniz.c
	framework/minizip/ioapi.c
	framework/minizip/unzip.cpp
)

always_optimize_sourcefile(framework/miniz/miniz.c)
always_optimize_sourcefile(framework/minizip/ioapi.c)
always_optimize_sourcefile(framework/minizip/unzip.cpp)

add_globbed_headers(src_framework "framework")

set(src_cm
	cm/CollisionModel_contacts.cpp
	cm/CollisionModel_contents.cpp
	cm/CollisionModel_debug.cpp
	cm/CollisionModel_files.cpp
	cm/CollisionModel_load.cpp
	cm/CollisionModel_rotate.cpp
	cm/CollisionModel_trace.cpp
	cm/CollisionModel_translate.cpp
)

add_globbed_headers(src_cm "cm")

set(src_dmap
	tools/compilers/dmap/dmap.cpp
	tools/compilers/dmap/facebsp.cpp
	tools/compilers/dmap/gldraw.cpp
	tools/compilers/dmap/glfile.cpp
	tools/compilers/dmap/leakfile.cpp
	tools/compilers/dmap/map.cpp
	tools/compilers/dmap/optimize.cpp
	tools/compilers/dmap/output.cpp
	tools/compilers/dmap/portals.cpp
	tools/compilers/dmap/shadowopt3.cpp
	tools/compilers/dmap/tritjunction.cpp
	tools/compilers/dmap/tritools.cpp
	tools/compilers/dmap/ubrush.cpp
	tools/compilers/dmap/usurface.cpp
)

add_globbed_headers(src_dmap "tools/compilers/dmap")

set(src_aas
	tools/compilers/aas/AASBuild.cpp
	tools/compilers/aas/AASBuild_file.cpp
	tools/compilers/aas/AASBuild_gravity.cpp
	tools/compilers/aas/AASBuild_ledge.cpp
	tools/compilers/aas/AASBuild_merge.cpp
	tools/compilers/aas/AASCluster.cpp
	tools/compilers/aas/AASFile.cpp
	tools/compilers/aas/AASFile_optimize.cpp
	tools/compilers/aas/AASFile_sample.cpp
	tools/compilers/aas/AASReach.cpp
	tools/compilers/aas/AASFileManager.cpp
	tools/compilers/aas/Brush.cpp
	tools/compilers/aas/BrushBSP.cpp
)

add_globbed_headers(src_aas "tools/compilers/aas")

set(src_roq
	tools/compilers/roqvq/NSBitmapImageRep.cpp
	tools/compilers/roqvq/codec.cpp
	tools/compilers/roqvq/roq.cpp
	tools/compilers/roqvq/roqParam.cpp
)

add_globbed_headers(src_roq "tools/compilers/roqvq")

set(src_renderbump
	tools/compilers/renderbump/renderbump.cpp
)

add_globbed_headers(src_renderbump "tools/compilers/renderbump")

set(src_snd
	sound/snd_cache.cpp
	sound/snd_decoder.cpp
	sound/snd_efxfile.cpp
	sound/snd_emitter.cpp
	sound/snd_shader.cpp
	sound/snd_system.cpp
	sound/snd_wavefile.cpp
	sound/snd_world.cpp

	sound/stbvorbis_impl.c
)

always_optimize_sourcefile(sound/stbvorbis_impl.c)

add_globbed_headers(src_snd "sound")

set(src_ui
	ui/BindWindow.cpp
	ui/ChoiceWindow.cpp
	ui/DeviceContext.cpp
	ui/EditWindow.cpp
	ui/FieldWindow.cpp
	ui/GameBearShootWindow.cpp
	ui/GameBustOutWindow.cpp
	ui/GameSSDWindow.cpp
	ui/GuiScript.cpp
	ui/ListGUI.cpp
	ui/ListWindow.cpp
	ui/MarkerWindow.cpp
	ui/RegExp.cpp
	ui/RenderWindow.cpp
	ui/SimpleWindow.cpp
	ui/SliderWindow.cpp
	ui/UserInterface.cpp
	ui/Window.cpp
	ui/Winvar.cpp
)

add_globbed_headers(src_ui "ui")

set(src_idlib
	idlib/bv/Bounds.cpp
	idlib/bv/Frustum.cpp
	idlib/bv/Sphere.cpp
	idlib/bv/Box.cpp
	idlib/geometry/DrawVert.cpp
	idlib/geometry/Winding2D.cpp
	idlib/geometry/Surface_SweptSpline.cpp
	idlib/geometry/Winding.cpp
	idlib/geometry/Surface.cpp
	idlib/geometry/Surface_Patch.cpp
	idlib/geometry/TraceModel.cpp
	idlib/geometry/JointTransform.cpp
	idlib/hashing/CRC32.cpp
	idlib/hashing/MD4.cpp
	idlib/hashing/MD5.cpp
	idlib/math/Angles.cpp
	idlib/math/Lcp.cpp
	idlib/math/Math.cpp
	idlib/math/Matrix.cpp
	idlib/math/Ode.cpp
	idlib/math/Plane.cpp
	idlib/math/Pluecker.cpp
	idlib/math/Polynomial.cpp
	idlib/math/Quat.cpp
	idlib/math/Rotation.cpp
	idlib/math/Simd.cpp
	idlib/math/Simd_Generic.cpp
	idlib/math/Simd_RVV.cpp
	idlib/math/Simd_AltiVec.cpp
	idlib/math/Simd_MMX.cpp
	idlib/math/Simd_3DNow.cpp
	idlib/math/Simd_SSE.cpp
	idlib/math/Simd_SSE2.cpp
	idlib/math/Simd_SSE3.cpp
	idlib/math/Vector.cpp
	idlib/BitMsg.cpp
	idlib/LangDict.cpp
	idlib/Lexer.cpp
	idlib/Lib.cpp
	idlib/containers/HashIndex.cpp
	idlib/Dict.cpp
	idlib/Str.cpp
	idlib/Parser.cpp
	idlib/MapFile.cpp
	idlib/CmdArgs.cpp
	idlib/Token.cpp
	idlib/Base64.cpp
	idlib/Timer.cpp
	idlib/Heap.cpp
)

add_globbed_headers(src_idlib "idlib")

if(IMGUI)

	if(SDL3)
		set(src_imgui libs/imgui/backends/imgui_impl_sdl3.cpp)
	else()
		set(src_imgui libs/imgui/backends/imgui_impl_sdl2.cpp)
	endif()

set(src_imgui ${src_imgui}
	libs/imgui/backends/imgui_impl_opengl2.cpp
	
	libs/imgui/imgui.h
	libs/imgui/imgui.cpp
	libs/imgui/imgui_draw.cpp
	libs/imgui/imgui_tables.cpp
	libs/imgui/imgui_widgets.cpp
	
	libs/imgui/imgui_demo.cpp
	
	sys/sys_imgui.h
	sys/sys_imgui.cpp
	sys/imgui_savestyle.cpp
)
else()
set(src_imgui sys/sys_imgui.h)
endif()


set(src_game
	game/AF.cpp
	game/AFEntity.cpp
	game/Actor.cpp
	game/Camera.cpp
	game/Entity.cpp
	game/BrittleFracture.cpp
	game/Fx.cpp
	game/GameEdit.cpp
	game/Game_local.cpp
	game/Game_network.cpp
	game/Item.cpp
	game/IK.cpp
	game/Light.cpp
	game/Misc.cpp
	game/Mover.cpp
	game/Moveable.cpp
	game/MultiplayerGame.cpp
	game/Player.cpp
	game/PlayerIcon.cpp
	game/PlayerView.cpp
	game/Projectile.cpp
	game/Pvs.cpp
	game/SecurityCamera.cpp
	game/SmokeParticles.cpp
	game/Sound.cpp
	game/Target.cpp
	game/Trigger.cpp
	game/Weapon.cpp
	game/WorldSpawn.cpp
	game/ai/AAS.cpp
	game/ai/AAS_debug.cpp
	game/ai/AAS_pathing.cpp
	game/ai/AAS_routing.cpp
	game/ai/AI.cpp
	game/ai/AI_events.cpp
	game/ai/AI_pathing.cpp
	game/ai/AI_Vagary.cpp
	game/gamesys/DebugGraph.cpp
	game/gamesys/Class.cpp
	game/gamesys/Event.cpp
	game/gamesys/SaveGame.cpp
	game/gamesys/SysCmds.cpp
	game/gamesys/SysCvar.cpp
	game/gamesys/TypeInfo.cpp
	game/anim/Anim.cpp
	game/anim/Anim_Blend.cpp
	game/anim/Anim_Import.cpp
	game/anim/Anim_Testmodel.cpp
	game/script/Script_Compiler.cpp
	game/script/Script_Interpreter.cpp
	game/script/Script_Program.cpp
	game/script/Script_Thread.cpp
	game/physics/Clip.cpp
	game/physics/Force.cpp
	game/physics/Force_Constant.cpp
	game/physics/Force_Drag.cpp
	game/physics/Force_Field.cpp
	game/physics/Force_Spring.cpp
	game/physics/Physics.cpp
	game/physics/Physics_AF.cpp
	game/physics/Physics_Actor.cpp
	game/physics/Physics_Base.cpp
	game/physics/Physics_Monster.cpp
	game/physics/Physics_Parametric.cpp
	game/physics/Physics_Player.cpp
	game/physics/Physics_RigidBody.cpp
	game/physics/Physics_Static.cpp
	game/physics/Physics_StaticMulti.cpp
	game/physics/Push.cpp
)

add_globbed_headers(src_game "game")

set(src_d3xp
	d3xp/AF.cpp
	d3xp/AFEntity.cpp
	d3xp/Actor.cpp
	d3xp/Camera.cpp
	d3xp/Entity.cpp
	d3xp/BrittleFracture.cpp
	d3xp/Fx.cpp
	d3xp/GameEdit.cpp
	d3xp/Game_local.cpp
	d3xp/Game_network.cpp
	d3xp/Item.cpp
	d3xp/IK.cpp
	d3xp/Light.cpp
	d3xp/Misc.cpp
	d3xp/Mover.cpp
	d3xp/Moveable.cpp
	d3xp/MultiplayerGame.cpp
	d3xp/Player.cpp
	d3xp/PlayerIcon.cpp
	d3xp/PlayerView.cpp
	d3xp/Projectile.cpp
	d3xp/Pvs.cpp
	d3xp/SecurityCamera.cpp
	d3xp/SmokeParticles.cpp
	d3xp/Sound.cpp
	d3xp/Target.cpp
	d3xp/Trigger.cpp
	d3xp/Weapon.cpp
	d3xp/WorldSpawn.cpp
	d3xp/ai/AAS.cpp
	d3xp/ai/AAS_debug.cpp
	d3xp/ai/AAS_pathing.cpp
	d3xp/ai/AAS_routing.cpp
	d3xp/ai/AI.cpp
	d3xp/ai/AI_events.cpp
	d3xp/ai/AI_pathing.cpp
	d3xp/ai/AI_Vagary.cpp
	d3xp/gamesys/DebugGraph.cpp
	d3xp/gamesys/Class.cpp
	d3xp/gamesys/Event.cpp
	d3xp/gamesys/SaveGame.cpp
	d3xp/gamesys/SysCmds.cpp
	d3xp/gamesys/SysCvar.cpp
	d3xp/gamesys/TypeInfo.cpp
	d3xp/anim/Anim.cpp
	d3xp/anim/Anim_Blend.cpp
	d3xp/anim/Anim_Import.cpp
	d3xp/anim/Anim_Testmodel.cpp
	d3xp/script/Script_Compiler.cpp
	d3xp/script/Script_Interpreter.cpp
	d3xp/script/Script_Program.cpp
	d3xp/script/Script_Thread.cpp
	d3xp/physics/Clip.cpp
	d3xp/physics/Force.cpp
	d3xp/physics/Force_Constant.cpp
	d3xp/physics/Force_Drag.cpp
	d3xp/physics/Force_Field.cpp
	d3xp/physics/Force_Spring.cpp
	d3xp/physics/Physics.cpp
	d3xp/physics/Physics_AF.cpp
	d3xp/physics/Physics_Actor.cpp
	d3xp/physics/Physics_Base.cpp
	d3xp/physics/Physics_Monster.cpp
	d3xp/physics/Physics_Parametric.cpp
	d3xp/physics/Physics_Player.cpp
	d3xp/physics/Physics_RigidBody.cpp
	d3xp/physics/Physics_Static.cpp
	d3xp/physics/Physics_StaticMulti.cpp
	d3xp/physics/Push.cpp
	d3xp/Grabber.cpp
	d3xp/physics/Force_Grab.cpp
)

add_globbed_headers(src_d3xp "d3xp")

set(src_debuggerServer
	tools/debugger/DebuggerBreakpoint.h
	tools/debugger/DebuggerBreakpoint.cpp
	tools/debugger/DebuggerServer.h
	tools/debugger/DebuggerServer.cpp
	tools/debugger/DebuggerScript.h
	tools/debugger/DebuggerScript.cpp
	tools/debugger/DebuggerMessages.h
	tools/debugger/debugger.cpp
)

set(src_core
	${src_renderer}
	${src_framework}
	${src_cm}
	${src_dmap}
	${src_aas}
	${src_roq}
	${src_renderbump}
	${src_snd}
	${src_ui}
	${src_tools}
)

set(src_stub_openal		sys/stub/openal_stub.cpp)
set(src_stub_gl			sys/stub/stub_gl.cpp)


set(src_tools
	tools/guied/GEWindowWrapper_stub.cpp
)

# TODO: add_globbed_headers(src_tools "tools/guied")

# Begin normal tools code
if (TOOLS AND MFC_FOUND AND MSVC)
	set(ALLOWRESOURCES ON)
	# Common files.
	file(GLOB src_comafx "tools/comafx/*.cpp")
	add_globbed_headers(src_comafx "tools/comafx")
	file(GLOB_RECURSE src_tools_common "tools/common/*.cpp")
	add_globbed_headers(src_tools_common "tools/common")

	#Compilers.
	# DG: this is always added, why add it again? file(GLOB src_compiler_renderbump "neo/tools/compilers/renderbump/*.cpp")
	file(GLOB src_compiler_roqvq	  "tools/compilers/roqvq/*.cpp") # FIXME: some of those were added before
	add_globbed_headers(src_compiler_roqvq "tools/compilers/roqvq")
	#tools
	# Articulated Frame editor.
	file(GLOB src_afeditor "tools/af/*.cpp")
	add_globbed_headers(src_afeditor "tools/af")
	# Declaration editor
	file(GLOB src_decleditor "tools/decl/*.cpp")
	add_globbed_headers(src_decleditor "tools/decl")
	# GUI Editor.
	file(GLOB src_gui_editor "tools/guied/*.cpp")
	add_globbed_headers(src_gui_editor "tools/guied")
	# Material editor
	file(GLOB src_materialeditor "tools/materialeditor/*.cpp")
	add_globbed_headers(src_materialeditor "tools/materialeditor")
	# Particle Editor
	file(GLOB src_particleeditor "tools/particle/*.cpp")
	add_globbed_headers(src_particleeditor "tools/particle")
	# PDA editor
	file(GLOB src_pdaeditor "tools/pda/*.cpp")
	add_globbed_headers(src_pdaeditor "tools/pda")
	# Radiant ( Map editor )
	file(GLOB src_map_editor "tools/radiant/*.cpp")
	add_globbed_headers(src_map_editor "tools/radiant")
	# Script editor
	file(GLOB src_script_editor "tools/script/*.cpp")
	add_globbed_headers(src_script_editor "tools/script")
	# Script Debugger
	set(src_debuggerClient
		tools/debugger/DebuggerClient.h
		tools/debugger/DebuggerClient.cpp
		tools/debugger/DebuggerApp.h
		tools/debugger/DebuggerApp.cpp
		tools/debugger/DebuggerQuickWatchDlg.h
		tools/debugger/DebuggerQuickWatchDlg.cpp
		tools/debugger/DebuggerWindow.h
		tools/debugger/DebuggerWindow.cpp
		tools/debugger/DebuggerFindDlg.h
		tools/debugger/DebuggerFindDlg.cpp
	)
	set(src_script_debugger
		${src_debuggerServer}
		${src_debuggerClient}
	)
	# sound editor?
	file(GLOB src_sound_editor "tools/sound/*.cpp")
	add_globbed_headers(src_sound_editor "tools/sound")
	

	# The numerous tools in a nice list.
	list(APPEND src_editor_tools
		${src_comafx}
		${src_tools_common}
		${src_compiler_renderbump}
		${src_afeditor}
		${src_decleditor}
		${src_gui_editor}
		${src_materialeditor}
		${src_particleeditor}
		${src_pdaeditor}
		${src_map_editor}
		${src_script_editor}
		${src_sound_editor}
		${src_script_debugger}
		"tools/edit_public.h"
		"tools/edit_gui_common.h"
		)
	SET(CMAKE_MFC_FLAG 2)
	set(TOOLS_DEFINES "ID_ALLOW_TOOLS;_AFXDLL")
else()
	set(src_editor_tools "tools/edit_stub.cpp" "tools/edit_public.h" "tools/debugger/debugger_common.h")
	list(APPEND src_editor_tools
		${src_debuggerServer}
	)
endif()


if(BAREMETAL)
	# Path B sys layer: keep the stock, portable, SDL3-based files (cpu/threads/
	# events/glimp/sys_local) and replace ONLY the POSIX-specific files
	# (posix_main.cpp + posix_net.cpp + linux/main.cpp) with riscv/engine/sys_dos.cpp.
	# The CRT (_start/_cstart), entry adapter (kmain) and newlib bottom-end live in
	# riscv/{boot,engine} and are added to the link group, not as engine TUs here.
	set(src_sys_base
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		"${BM_ENGINE_DIR}/sys_dos.cpp"   # Sys_* not covered by the SDL files + dhewm3_main
		"${BM_ENGINE_DIR}/cxxrt.cpp"     # operator new/delete, __cxa_* glue
		sys/platform.h
		sys/sys_local.h
		sys/sys_public.h
	)

	set(src_sys_core
		sys/glimp.cpp                    # stock SDL3 GLimp -> SDL_GL_* -> dos driver
	)
elseif(AROS)
	set(DHEWM3BINARY "ADoom3")
	set(sys_libs ${sys_libs} dynmod)

	set(src_sys_base
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/aros/aros_net.cpp
		sys/aros/aros_signal.cpp
		sys/aros/aros_main.cpp
		 sys/aros/aros_dynmodcommon.cpp
		sys/aros/aros_support.cpp
		sys/aros/aros_dos.cpp
	)

	set(src_sys_core
		sys/glimp.cpp
	)
elseif(APPLE)
	set(OSX_RESOURCE_FILES
		"${CMAKE_SOURCE_DIR}/sys/osx/Doom3.icns"
		"${CMAKE_SOURCE_DIR}/sys/osx/Doom 3.rsrc"
	)

	set_source_files_properties(${OSX_RESOURCE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

	set(src_sys_base
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/posix/posix_net.cpp
		sys/posix/posix_main.cpp
	)

	set(src_sys_core
		sys/glimp.cpp
		sys/osx/DOOMController.mm
		sys/osx/macosx_misc.mm
		sys/osx/SDLMain.m
		${OSX_RESOURCE_FILES}
	)
elseif(WIN32)

	set(src_sys_base
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/win32/win_input.cpp
		sys/win32/win_main.cpp
		sys/win32/win_net.cpp
		sys/win32/win_shared.cpp
		sys/win32/win_syscon.cpp
	)

	if(NOT SDL2 AND NOT SDL3)
		# only SDL1.2 still uses SDL_win32_main.c
		set(src_sys_base ${src_sys_base} sys/win32/SDL_win32_main.c)
	endif()

	# adding the few relevant headers in sys/ manually..
	set(src_sys_base ${src_sys_base}
		sys/platform.h
		sys/sys_local.h
		sys/sys_public.h
		sys/win32/win_local.h
	)

	set(src_sys_core
		sys/glimp.cpp
	)

	if(TOOLS)
		set(src_sys_core ${src_sys_core} "sys/win32/rc/doom.rc")
	else()
		# adding both .rc files breaks the build, but I think it only contains the icon
		# and doom.rc also contains that (+icons for editor etc, AFAIK), so this is fine
		set(src_sys_core ${src_sys_core} "${CMAKE_SOURCE_DIR}/sys/win32/rc/dhewm3.rc" )
	endif()

else()
	set(src_sys_base
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/posix/posix_net.cpp
		sys/posix/posix_main.cpp
		sys/linux/main.cpp
	)

	set(src_sys_core
		sys/glimp.cpp
	)
endif()

include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_SOURCE_DIR})

add_library(idlib STATIC ${src_idlib})
if(BAREMETAL)
	# Re-enable FMA contraction for idlib on the bare-metal target. The top-level
	# -ffp-contract=off is upstream's guard for desktop demo/physics determinism
	# (gcc PR100839); bare-metal DoomOS records no demos and replays no netgames,
	# and this is the library behind every matrix/vector/plane operation in map
	# load and the game tick. Scoped to BAREMETAL so desktop dhewm3 keeps
	# upstream's determinism guarantee untouched.
	target_compile_options(idlib PRIVATE -ffp-contract=fast)
endif()
if(SDL3 AND NOT BAREMETAL)
	target_link_libraries(idlib SDL3::Headers) # so it can use SDL_Endian.h
	# (BAREMETAL adds the prebuilt SDL3 headers via include_directories instead.)
endif()
if (AROS)
	if(CMAKE_SYSTEM_PROCESSOR STREQUAL "i386")
		set(AROS_ARCH "x86")
	else()
		set(AROS_ARCH ${CMAKE_SYSTEM_PROCESSOR})
	endif()
elseif(BAREMETAL)
	# No -fPIC on the static bare-metal image (fixed-address, -fno-pic toolchain).
else()
	if(D3_COMPILER_IS_GCC_OR_CLANG AND NOT MINGW)
		set_target_properties(idlib PROPERTIES COMPILE_FLAGS "-fPIC")
	endif()
endif()

if(BAREMETAL)
	# Some bare-metal sys/runtime sources live under riscv/ (outside neo/), which
	# breaks source_group(TREE neo ...) ("not a prefix"). source_group is purely
	# cosmetic IDE grouping, so make it a no-op for the bare-metal build.
	function(source_group)
	endfunction()
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_idlib})

if(BAREMETAL)
	# ----- bare-metal C runtime + renderer-backend static libs ---------------
	# Built as plain C with the toolchain's freestanding C flags. These mirror the
	# proven riscv/dos/Makefile recipe (boot + dos_syscalls + softgles2 backend +
	# gl4es bridge + dos_present). The prebuilt libSDL3-riscv.a + libGL-riscv.a are
	# linked directly (NOT rebuilt). See riscv/dos/Makefile "sdl-dos" target.
	enable_language(ASM)

	# 1) boot CRT (start.S/_start, runtime.c/_cstart, sbi.c, trap.c) + entry adapter.
	add_library(bm_boot STATIC
		"${BM_BOOT_DIR}/start.S"
		"${BM_BOOT_DIR}/runtime.c"
		"${BM_BOOT_DIR}/sbi.c"
		"${BM_BOOT_DIR}/trap.c"
		"${BM_BOOT_DIR}/irq.c"
		"${BM_BOOT_DIR}/fdt.c"
		"${BM_ENGINE_DIR}/engine_kmain.c"
		"${BM_ENGINE_DIR}/texture_jobs.c"
		"${BM_ENGINE_DIR}/dos_syscalls.c"
		"${BM_ENGINE_DIR}/dos_hart_entry.S"
		"${BM_ENGINE_DIR}/dos_thread.c"
		"${BM_ENGINE_DIR}/dos_virtio_blk.c"   # virtio-blk MMIO read+write driver (doc 10)
		"${BM_ENGINE_DIR}/dosfs.c"            # flat read-only dosfs reader  (doc 12)
		"${BM_ENGINE_DIR}/dos_ramfs.c"        # writable save/config RAM overlay
		"${BM_ENGINE_DIR}/savefs.c"           # persistent append-only journal (doc 13)
	)
	target_include_directories(bm_boot PRIVATE "${BAREMETAL_RISCV_DIR}")
	# runtime.c runs BEFORE .bss is live, so it must not execute vector code:
	# GCC autovectorised bm_zero_range()'s byte-alignment head loop into a
	# 30-instruction RVV early-break sequence (vid.v, vand/vmseq/vmor.mm, vcpop.m)
	# that ran before the region it is about to zero exists. Harmless in practice
	# because __bss_start is aligned, but it means _cstart issues vector
	# instructions on the strength of OpenSBI having left sstatus.VS on.
	set_source_files_properties("${BM_BOOT_DIR}/runtime.c"
		PROPERTIES COMPILE_OPTIONS "-fno-tree-vectorize")
	target_compile_definitions(bm_boot PRIVATE
		"BM_DEFER_PRECACHE=${BAREMETAL_DEFER_PRECACHE}"
		"BM_QUIET_ASSET_LOADS=${BAREMETAL_QUIET_ASSET_LOADS}"
		"BM_IO_STATS=${BAREMETAL_IO_STATS}"
		"BM_SIMD_BENCH=${BAREMETAL_SIMD_BENCH}"
		"BM_BOOT_TO_MENU=${BAREMETAL_BOOT_TO_MENU}"
		"BM_RENDER_PROF=${BAREMETAL_RENDER_PROF}"
		"BM_SGL_OVERLAY=${BAREMETAL_SGL_OVERLAY}"
		"BM_FIXED_TIC=${BAREMETAL_FIXED_TIC}"
		"BM_DETERMINISTIC_CLOCK=${BAREMETAL_DETERMINISTIC_CLOCK}"
		"BM_FB_WIDTH=${BM_RENDER_WIDTH}"
		"BM_FB_HEIGHT=${BM_RENDER_HEIGHT}"
		"BM_SSAA=${BAREMETAL_SSAA}"
	)

	# 2) softgles2 software-rasteriser backend, namespaced to sgl2_* (so it backs
	#    gl4es's GLES2 resolver), compiled exactly as riscv/dos/Makefile does.
	add_library(bm_sgl STATIC
		"${BM_SGL_DIR}/src/fb.c"
		"${BM_SGL_DIR}/src/sg_diag.c"
		"${BM_SGL_DIR}/src/sg_raster.c"
		"${BM_SGL_DIR}/src/gles2.c"
		"${BM_SGL_DIR}/src/glsl.c"
		"${BM_SGL_DIR}/src/glsl_lex.c"
		"${BM_SGL_DIR}/src/glsl_parse.c"
		"${BM_SGL_DIR}/src/glsl_ir.c"
		"${BM_SGL_DIR}/src/glsl_simt.c"
		"${BM_SGL_DIR}/src/sg_kernels.c"
		"${BM_SGL_DIR}/src/sg_mt.c"
		"${BM_SGL_DIR}/src/sg_mt_prim.c"
		"${BM_SGL_DIR}/src/sg_mt_tiles.c"
		"${BM_SGL_DIR}/src/sg_mt_raster.c"
		"${BM_SGL_DIR}/src/sg_mt_sched.c"
		"${BM_SGL_DIR}/src/sg_state.c"
		"${BM_SGL_DIR}/src/sg_vertex.c"
		"${BM_SGL_DIR}/src/sg_vs_range.c"
		"${BM_SGL_DIR}/src/sg_frag.c"
		"${BM_SGL_DIR}/src/sg_frame.c"
		"${BM_SGL_DIR}/src/sg_framebin.c"
		"${BM_SGL_DIR}/src/sg_prof.c"
		"${BM_SGL_DIR}/src/sg_overlay.c"
		"${BM_SGL_DIR}/src/sg_buffer.c"
		"${BM_SGL_DIR}/src/sg_texture.c"
		"${BM_SGL_DIR}/src/sg_framebuffer.c"
		"${BM_SGL_DIR}/src/sg_shader.c"
		"${BM_SGL_DIR}/src/sg_draw.c"
		"${BM_SGL_DIR}/src/sg_clip.c"
	)
	option(BAREMETAL_RASTER_PARETO "Sample RVV raster stages (diagnostic build only)" OFF)
	set_source_files_properties("${BM_SGL_DIR}/src/sg_raster.c" "${BM_SGL_DIR}/src/sg_prof.c"
		PROPERTIES COMPILE_DEFINITIONS "SGL_PARETO=$<BOOL:${BAREMETAL_RASTER_PARETO}>")
	# Re-enable FMA contraction for the software renderer only. The top-level
	# add_compile_options(-ffp-contract=off) above is upstream dhewm3's guard for
	# demo/physics determinism, but it lands on every target including this one,
	# where it costs real throughput: sg_raster.c compiles 89 FMA instructions
	# with contraction off against 128 with it on (+44%), and that file is the
	# per-fragment raster wrapper. softgles2 has no determinism contract with the
	# engine -- it consumes vertices and produces pixels. Target options are
	# emitted after directory options, so this wins.
	# NOTE: this can perturb the kernel-vs-VM bit-exactness gates (iakernrun,
	# ovkrun, atestrv); both sides are built with the same flag so they should
	# stay consistent, but re-run them after changing this.
	target_compile_options(bm_sgl PRIVATE
		-DSGL_BAREMETAL -ffp-contract=fast -fno-tree-loop-distribute-patterns -fno-math-errno
		-include "${BM_BRIDGE_DIR}/sgl2_names.h")
	# A huge minimum disables the dedupe without a second code path to keep in
	# sync -- the guard it already has becomes permanently false.
	if(BAREMETAL_VS_DEDUP)
		set(BM_VS_DEDUP_MIN 64)
	else()
		set(BM_VS_DEDUP_MIN 0x7FFFFFFF)
	endif()
	target_compile_definitions(bm_sgl PRIVATE
		"SGL_FRAMEBIN_DEFAULT=${BAREMETAL_FRAMEBIN}"
		"SGL_VS_DEDUP_MIN_VERTS=${BM_VS_DEDUP_MIN}")
	target_include_directories(bm_sgl PRIVATE
		"${BAREMETAL_RISCV_DIR}" "${BM_SGL_DIR}/include" "${BM_SGL_DIR}/src"
		"${BM_BRIDGE_DIR}" "${BM_GL4ES_INCLUDE}")

	# 3) gl4es <-> softgles2 bridge + the dos present/blit surface that the SDL3
	#    "dos" video driver forwards into (DOS_GL_CreateContext/SwapWindow/...).
	add_library(bm_bridge STATIC
		"${BM_BRIDGE_DIR}/bridge.c"
		"${BM_DOS_DIR}/dos_present.c"
		"${BM_DOS_DIR}/dos_virtio_gpu.c"
	)
	target_compile_options(bm_bridge PRIVATE -DSGL_BAREMETAL)
	target_compile_definitions(bm_bridge PRIVATE
		"BM_FB_HASH=${BAREMETAL_FB_HASH}" "BM_SSAA=${BAREMETAL_SSAA}")
	target_include_directories(bm_bridge PRIVATE
		"${BAREMETAL_RISCV_DIR}" "${BM_DOS_DIR}" "${BM_SGL_DIR}/include"
		"${BM_SGL_DIR}/src" "${BM_BRIDGE_DIR}" "${BM_GL4ES_INCLUDE}")

	# 4) dOS OpenAL ABI shim -> miniaudio device-less mixer -> serial PCM sink.
	# The virtio-snd backend is selected later by compiling dos_audio.c with
	# DOS_AUDIO_VIRTIO once the TX virtqueue driver is complete.
	add_library(bm_audio STATIC
		"${BM_AUDIO_DIR}/miniaudio_impl.c"
		"${BM_AUDIO_DIR}/ma_stubs.c"
		"${BM_AUDIO_DIR}/dos_audio.c"
		"${BM_AUDIO_DIR}/openal/al.c"
	)
	target_include_directories(bm_audio BEFORE PRIVATE
		"${BM_AUDIO_DIR}" "${BM_AUDIO_DIR}/openal" "${BAREMETAL_RISCV_DIR}")
endif()

if(HARDLINK_GAME)
	if(BASE)
		set(src_core ${src_core} ${src_game})
		set(game_includes "${CMAKE_SOURCE_DIR}/game")
		if(D3XP)
			message(WARNING "hardlinking the base game, d3xp will not be supported (disable BASE for D3XP support with HARDLINK_GAME)")
		endif()
		
	elseif(D3XP)
		set(src_core ${src_core} ${src_d3xp})
		set(game_includes "${CMAKE_SOURCE_DIR}/d3xp")
	endif()
endif()# HARDLINK_GAME

if(CORE)
	add_executable(${DHEWM3BINARY} WIN32 MACOSX_BUNDLE
		${src_core}
		${src_sys_base}
		${src_sys_core}
		${src_imgui}
		${src_editor_tools}
	)
	
	if(MSVC AND CMAKE_MAJOR_VERSION GREATER 3 OR ( CMAKE_MAJOR_VERSION EQUAL 3 AND CMAKE_MINOR_VERSION GREATER_EQUAL 6 ))
		# CMake >= 3.6 supports setting the default project started for debugging (instead of trying to launch ALL_BUILD ...)
		set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${DHEWM3BINARY})
	endif()

	source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_core} ${src_sys_base} ${src_sys_core} ${src_imgui} ${src_editor_tools})

	target_include_directories(${DHEWM3BINARY} PRIVATE "${CMAKE_SOURCE_DIR}/libs/imgui")

if(HARDLINK_GAME)
	set_target_properties(${DHEWM3BINARY} PROPERTIES COMPILE_DEFINITIONS "${TOOLS_DEFINES}")
	target_include_directories(${DHEWM3BINARY} PRIVATE ${game_includes})
else()
	set_target_properties(${DHEWM3BINARY} PROPERTIES COMPILE_DEFINITIONS "__DOOM_DLL__;${TOOLS_DEFINES}")
endif()
if(BAREMETAL)
	# -nostdlib static image at engine.ld's load address. The whole-program link
	# group resolves the mutual references between the engine objects, idlib, the
	# prebuilt SDL3 + gl4es, the softgles2 backend / bridge / dos present surface,
	# the boot CRT, and newlib + the C++ ABI support libs. The DWARF unwinder is
	# in libgcc.a (there is NO libgcc_eh.a for this newlib gcc16 - see status doc),
	# and libsupc++.a provides __cxa_throw / personality (exceptions are ON).
	set_target_properties(${DHEWM3BINARY} PROPERTIES
		LINK_FLAGS "-nostdlib -static -Wl,--gc-sections -Wl,--build-id=none -T \"${BAREMETAL_ENGINE_LD}\"")
	set_property(TARGET ${DHEWM3BINARY} APPEND PROPERTY
		LINK_DEPENDS "${BAREMETAL_ENGINE_LD}")
	# Use --start-group/--end-group around the archives so the link order between
	# idlib, the engine objects (the exe's own objects come first automatically),
	# SDL3, gl4es, softgles2, the bridge, the boot CRT and newlib is irrelevant.
	option(BAREMETAL_SHIM_PROFILE "Diagnostic GL boundary timing and gl4es allocation ownership" OFF)
	if(BAREMETAL_SHIM_PROFILE)
		find_package(Python3 REQUIRED COMPONENTS Interpreter)
		set(SHIM_PROFILE_DIR "${CMAKE_CURRENT_BINARY_DIR}/shim-profile")
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			"${BM_BRIDGE_DIR}/generate_profile.py" "${BM_BRIDGE_DIR}/bridge.c"
			"${BM_SGL_DIR}/include/sgl_gl.h" "${BM_GL4ES_INCLUDE}/GL/glext.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/renderer/qgl_proc.h" "${CMAKE_CURRENT_SOURCE_DIR}/renderer/qgl.h")
		execute_process(COMMAND "${Python3_EXECUTABLE}" "${BM_BRIDGE_DIR}/generate_profile.py"
			"${SHIM_PROFILE_DIR}" RESULT_VARIABLE SHIM_GEN_RESULT)
		if(NOT SHIM_GEN_RESULT EQUAL 0)
			message(FATAL_ERROR "Failed to generate shim profile wrappers")
		endif()
		include("${SHIM_PROFILE_DIR}/profile_link.cmake")
		add_custom_command(OUTPUT "${SHIM_PROFILE_DIR}/libGL-profile.a"
			COMMAND "${CMAKE_OBJCOPY}" "--redefine-syms=${SHIM_PROFILE_DIR}/profile_alloc.syms"
				"${BM_GL4ES_LIB}" "${SHIM_PROFILE_DIR}/libGL-profile.a"
			DEPENDS "${BM_GL4ES_LIB}" "${SHIM_PROFILE_DIR}/profile_alloc.syms" VERBATIM)
		add_custom_target(shim_profile_archive DEPENDS "${SHIM_PROFILE_DIR}/libGL-profile.a")
		add_dependencies(${DHEWM3BINARY} shim_profile_archive)
		set(BM_GL4ES_LIB "${SHIM_PROFILE_DIR}/libGL-profile.a")
		target_sources(bm_bridge PRIVATE "${BM_BRIDGE_DIR}/profile.c"
			"${SHIM_PROFILE_DIR}/profile_front.c" "${SHIM_PROFILE_DIR}/profile_driver.c")
		target_compile_definitions(bm_bridge PRIVATE BM_SHIM_PROFILE=1)
		target_link_libraries(${DHEWM3BINARY} ${SHIM_WRAP_FLAGS})
	endif()
	target_link_libraries(${DHEWM3BINARY}
		-Wl,--start-group
		idlib
		bm_sgl
		bm_bridge
		bm_audio
		"${BM_SDL3_LIB}"
		"${BM_GL4ES_LIB}"
		bm_boot
		-lc -lm -lstdc++ -lsupc++ -lgcc
		-Wl,--end-group
	)
else()
	set_target_properties(${DHEWM3BINARY} PROPERTIES LINK_FLAGS "${ldflags}")
	set_target_properties(${DHEWM3BINARY} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/sys/osx/Info.plist)

	target_link_libraries(${DHEWM3BINARY}
		idlib
		${OPENAL_LIBRARY}
		${CURL_LIBRARY}
		${SDLx_LIBRARY}
		${sys_libs}
	)
endif()

	if(NOT APPLE AND NOT WIN32 AND NOT BAREMETAL)
		install(TARGETS ${DHEWM3BINARY}
				RUNTIME DESTINATION "${bindir}"
				LIBRARY DESTINATION "${libdir}"
				ARCHIVE DESTINATION "${libdir}"
		)
	endif()

	if(BAREMETAL)
		# `cmake --build build-rv --target dist` rebuilds the engine, then stamps
		# every built bare-metal rv64 ELF payload into riscv/dist/ with a MANIFEST
		# (git hash, sizes, sha256, run commands). The script validates each ELF.
		add_custom_target(dist
			COMMAND bash "${BAREMETAL_RISCV_DIR}/scripts/stamp-dist.sh" "$<TARGET_FILE:${DHEWM3BINARY}>"
			DEPENDS ${DHEWM3BINARY}
			WORKING_DIRECTORY "${BAREMETAL_RISCV_DIR}/.."
			COMMENT "Stamping rv64 ELF payloads -> riscv/dist/"
			USES_TERMINAL VERBATIM)
	endif()
endif()

if(DEDICATED)
	add_executable(${DHEWM3BINARY}ded WIN32 MACOSX_BUNDLE
		${src_core}
		${src_stub_openal}
		${src_stub_gl}
		${src_sys_base}
		${src_debuggerServer}
	)
	
	source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_core} ${src_sys_base} ${src_stub_openal} ${src_stub_gl} ${src_debuggerServer})

if(HARDLINK_GAME)
	set_target_properties(${DHEWM3BINARY}ded PROPERTIES COMPILE_DEFINITIONS "IMGUI_DISABLE;ID_DEDICATED")
	target_include_directories(${DHEWM3BINARY}ded PRIVATE ${game_includes})
else()
	set_target_properties(${DHEWM3BINARY}ded PROPERTIES COMPILE_DEFINITIONS "IMGUI_DISABLE;ID_DEDICATED;__DOOM_DLL__")
endif()
	set_target_properties(${DHEWM3BINARY}ded PROPERTIES LINK_FLAGS "${ldflags}")
	target_link_libraries(${DHEWM3BINARY}ded
		idlib
		${CURL_LIBRARY}
		${SDLx_LIBRARY}
		${sys_libs}
	)

	if(NOT APPLE AND NOT WIN32)
		install(TARGETS ${DHEWM3BINARY}ded
				RUNTIME DESTINATION "${bindir}"
				LIBRARY DESTINATION "${libdir}"
				ARCHIVE DESTINATION "${libdir}"
		)
	endif()
endif()

if(BASE AND NOT HARDLINK_GAME)
	if (AROS)
		add_executable(base ${src_game} sys/aros/aros_dynmodule.cpp sys/aros/aros_dynmodcommon.cpp)
		target_link_libraries(base idlib dynmodglue)
		set_target_properties(base PROPERTIES OUTPUT_NAME "base.aros-${AROS_ARCH}")
	else()
		add_library(base SHARED ${src_game})
	endif()
	
	source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_game})
	
	set_target_properties(base PROPERTIES PREFIX "")
	set_target_properties(base PROPERTIES COMPILE_DEFINITIONS "GAME_DLL")
	target_include_directories(base PRIVATE "${CMAKE_SOURCE_DIR}/game")
	set_target_properties(base PROPERTIES LINK_FLAGS "${ldflags}")
	set_target_properties(base PROPERTIES INSTALL_NAME_DIR "@executable_path")
	if (AROS)
		target_link_libraries(base idlib dynmod)
	else()
		target_link_libraries(base idlib)
	endif()

	if(NOT APPLE AND NOT WIN32)
		install(TARGETS base
				RUNTIME DESTINATION "${bindir}"
				LIBRARY DESTINATION "${libdir}"
				ARCHIVE DESTINATION "${libdir}"
		)
	endif()
endif()

if(D3XP AND NOT HARDLINK_GAME)
	if (AROS)
		add_executable(d3xp ${src_d3xp} sys/aros/aros_dynmodule.cpp sys/aros/aros_dynmodcommon.cpp)
		target_link_libraries(d3xp dynmodglue)
		set_target_properties(d3xp PROPERTIES OUTPUT_NAME "d3xp.aros-${AROS_ARCH}")
	else()
		add_library(d3xp SHARED ${src_d3xp})
	endif()
	source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_d3xp})
	set_target_properties(d3xp PROPERTIES PREFIX "")
	set_target_properties(d3xp PROPERTIES COMPILE_DEFINITIONS "GAME_DLL;_D3XP;CTF")
	target_include_directories(d3xp PRIVATE "${CMAKE_SOURCE_DIR}/d3xp")
	set_target_properties(d3xp PROPERTIES LINK_FLAGS "${ldflags}")
	set_target_properties(d3xp PROPERTIES INSTALL_NAME_DIR "@executable_path")
	if (AROS)
		target_link_libraries(d3xp idlib dynmod)
	else()
		target_link_libraries(d3xp idlib)
	endif()

	if(NOT APPLE AND NOT WIN32)
		install(TARGETS d3xp
				RUNTIME DESTINATION "${bindir}"
				LIBRARY DESTINATION "${libdir}"
				ARCHIVE DESTINATION "${libdir}"
		)
	endif()
endif()
