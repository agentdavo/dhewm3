#!/usr/bin/env bash
#
# Build SDL3 as a static library for the bare-metal RISC-V target ("dOS").
#
# SDL3 has no Generic/bare-metal platform arm, so four small, well-understood
# patches are applied to the (gitignored) vendored tree before configuring; they
# are kept here, not in the SDL source (per SDL's CLAUDE.md, these are downstream
# patches for the dhewm3 fork, not upstream contributions). See
# riscv/docs/dos/20-sdl3-build-attempt.md for the full rationale.
#
#   ./build-sdl3.sh         fetch (if needed) + patch + configure + build
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDL="$HERE/../sdl3"
TOOLCHAIN="$HERE/../toolchain/baremetal-rva23s64.cmake"

if [ ! -f "$SDL/CMakeLists.txt" ]; then
    echo "[sdl3] fetching SDL3 3.4.10..."
    mkdir -p "$SDL"; cd "$SDL"
    curl -fsSL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.10.tar.gz -o sdl3.tar.gz
    tar xzf sdl3.tar.gz --strip-components=1 && rm sdl3.tar.gz
fi

echo "[sdl3] copying dos video driver into src/video/dos/ ..."
mkdir -p "$SDL/src/video/dos"
cp "$HERE/video-dos/"*.c "$HERE/video-dos/"*.h "$SDL/src/video/dos/"

echo "[sdl3] applying bare-metal patches (idempotent)..."
python3 - "$SDL" <<'PY'
import sys
sdl = sys.argv[1]
cm = sdl + "/CMakeLists.txt"
s = open(cm).read(); changed = False

# 1. threads: let BAREMETAL take SDL's vetted threads-disabled (generic) branch
if "EMSCRIPTEN OR NGAGE OR BAREMETAL" not in s:
    assert "if(EMSCRIPTEN OR NGAGE)" in s, "threads branch anchor not found"
    s = s.replace("if(EMSCRIPTEN OR NGAGE)", "if(EMSCRIPTEN OR NGAGE OR BAREMETAL)", 1); changed = True

# 2. timers/time/dynapi/GL: Generic sets none; add them before config generation
if "DHEWM3_BAREMETAL timers" not in s:
    block = '''# --- DHEWM3_BAREMETAL timers/time/dynapi/GL (Generic platform sets none) ---
if(BAREMETAL)
  if(NOT HAVE_SDL_TIMERS)
    set(SDL_TIMER_UNIX 1)
    sdl_glob_sources("${SDL3_SOURCE_DIR}/src/timer/unix/*.c")
    set(HAVE_SDL_TIMERS TRUE)
  endif()
  if(NOT HAVE_SDL_TIME)
    set(SDL_TIME_UNIX 1)
    sdl_glob_sources("${SDL3_SOURCE_DIR}/src/time/unix/*.c")
    set(HAVE_SDL_TIME TRUE)
  endif()
  set(DYNAPI_NEEDS_DLOPEN 1)        # flips SDL_DYNAMIC_API off via SDL's own hook
  # NOTE: SDL_VIDEO_OPENGL_EGL/ES2 are deliberately NOT set here — they drag in
  # the Khronos EGL platform headers (eglplatform.h "Platform not recognized").
  # The dos video driver phase sets them with a platform shim + routes GL to gl4es.
endif()

'''
    anchor = "# We always need to have threads and timers"
    assert anchor in s, "threads/timers section anchor not found"
    s = s.replace(anchor, block + anchor, 1); changed = True

# 3. guard the POSIX process backend (no dirent/posix_spawn on newlib)
if "if(NOT BAREMETAL AND HAVE_POSIX_SPAWN" not in s:
    proc = "if(HAVE_POSIX_SPAWN AND (APPLE OR LIBC_HAS_VFORK))"
    assert proc in s, "process backend anchor not found"
    s = s.replace(proc, "if(NOT BAREMETAL AND HAVE_POSIX_SPAWN AND (APPLE OR LIBC_HAS_VFORK))", 1); changed = True

if changed: open(cm, "w").write(s); print("  patched CMakeLists.txt")
else: print("  CMakeLists.txt already patched")

# --- 3: src/time/unix/SDL_systime.c — newlib has no tm_gmtoff/_POSIX_VERSION ---
st = sdl + "/src/time/unix/SDL_systime.c"
s = open(st).read()
old = "#if (_POSIX_VERSION >= 202405L) || (!defined(sun) && !defined(__sun))"
new = ("#if defined(SDL_PLATFORM_DHEWM3_BAREMETAL)\n"
       "        dt->utc_offset = 0;\n"
       "#elif (_POSIX_VERSION >= 202405L) || (!defined(sun) && !defined(__sun))")
if "SDL_PLATFORM_DHEWM3_BAREMETAL" not in s:
    assert old in s, "systime #if anchor not found"
    s = s.replace(old, new, 1)
    open(st, "w").write(s)
    print("  patched SDL_systime.c")
else:
    print("  SDL_systime.c already patched")

# --- 4: register the "dos" video driver (driver phase) ---

# 4a-cmake. CMakeLists.txt: glob src/video/dos + set(SDL_VIDEO_DRIVER_DOS 1).
#   The dos driver implements the SDL_VideoDevice GL_* function pointers itself
#   (no SDL_egl.c) and routes GL to gl4es -> softgles2; SDL_video.c calls
#   _this->GL_* unconditionally (no SDL_VIDEO_OPENGL* macro gate on dispatch),
#   so the GL path works with NO SDL_VIDEO_OPENGL* macro defined.
s = open(cm).read()
if "SDL_VIDEO_DRIVER_DOS" not in s:
    anchor = ("  if(SDL_DUMMYVIDEO)\n"
              "    set(SDL_VIDEO_DRIVER_DUMMY 1)\n"
              "    sdl_glob_sources(\n"
              '      "${SDL3_SOURCE_DIR}/src/video/dummy/*.c"\n'
              '      "${SDL3_SOURCE_DIR}/src/video/dummy/*.h"\n'
              "    )\n"
              "    set(HAVE_DUMMYVIDEO TRUE)\n"
              "    set(HAVE_SDL_VIDEO TRUE)\n"
              "  endif()\n")
    assert anchor in s, "dummy video CMake block anchor not found"
    dosblk = (anchor +
              "  if(BAREMETAL)\n"
              "    set(SDL_VIDEO_DRIVER_DOS 1)\n"
              "    sdl_glob_sources(\n"
              '      "${SDL3_SOURCE_DIR}/src/video/dos/*.c"\n'
              '      "${SDL3_SOURCE_DIR}/src/video/dos/*.h"\n'
              "    )\n"
              "    set(HAVE_DOS TRUE)\n"
              "    set(HAVE_SDL_VIDEO TRUE)\n"
              "  endif()\n")
    s = s.replace(anchor, dosblk, 1)
    open(cm, "w").write(s)
    print("  patched CMakeLists.txt (SDL_VIDEO_DRIVER_DOS glob block)")
else:
    print("  CMakeLists.txt already has SDL_VIDEO_DRIVER_DOS")

# 4a. build_config template: #cmakedefine SDL_VIDEO_DRIVER_DOS 1
bc = sdl + "/include/build_config/SDL_build_config.h.cmake"
s = open(bc).read()
if "SDL_VIDEO_DRIVER_DOS" not in s:
    anchor = "#cmakedefine SDL_VIDEO_DRIVER_OFFSCREEN 1"
    assert anchor in s, "build_config OFFSCREEN anchor not found"
    s = s.replace(anchor, anchor + "\n#cmakedefine SDL_VIDEO_DRIVER_DOS 1", 1)
    open(bc, "w").write(s)
    print("  patched SDL_build_config.h.cmake (SDL_VIDEO_DRIVER_DOS)")
else:
    print("  SDL_build_config.h.cmake already has SDL_VIDEO_DRIVER_DOS")

# 4b. SDL_sysvideo.h: extern VideoBootStrap DOS_bootstrap;
sv = sdl + "/src/video/SDL_sysvideo.h"
s = open(sv).read()
if "extern VideoBootStrap DOS_bootstrap;" not in s:
    anchor = "extern VideoBootStrap OFFSCREEN_bootstrap;"
    assert anchor in s, "sysvideo OFFSCREEN_bootstrap anchor not found"
    s = s.replace(anchor, "extern VideoBootStrap DOS_bootstrap;\n" + anchor, 1)
    open(sv, "w").write(s)
    print("  patched SDL_sysvideo.h (extern DOS_bootstrap)")
else:
    print("  SDL_sysvideo.h already has extern DOS_bootstrap")

# 4c. SDL_video.c: register &DOS_bootstrap FIRST in bootstrap[] so the no-hint
#     auto-select picks it (DOS_CreateDevice unconditionally succeeds). Placed
#     right after the array opening brace, before all platform drivers.
vc = sdl + "/src/video/SDL_video.c"
s = open(vc).read()
if "&DOS_bootstrap," not in s:
    anchor = "static VideoBootStrap *bootstrap[] = {\n"
    assert anchor in s, "bootstrap[] array anchor not found"
    block = anchor + "#ifdef SDL_VIDEO_DRIVER_DOS\n    &DOS_bootstrap,\n#endif\n"
    s = s.replace(anchor, block, 1)
    open(vc, "w").write(s)
    print("  patched SDL_video.c (registered &DOS_bootstrap first)")
else:
    print("  SDL_video.c already registers &DOS_bootstrap")
PY

echo "[sdl3] configure..."
rm -rf "$SDL/build-rv"
cmake -S "$SDL" -B "$SDL/build-rv" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DBAREMETAL=ON \
  -DCMAKE_C_FLAGS="-DSDL_PLATFORM_DHEWM3_BAREMETAL=1" \
  -DSDL_STATIC=ON -DSDL_SHARED=OFF -DBUILD_SHARED_LIBS=OFF \
  -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF \
  -DSDL_LIBC=ON -DSDL_SYSTEM_ICONV=OFF -DSDL_LIBICONV=OFF \
  -DSDL_VIDEO=ON -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=ON \
  -DSDL_RENDER=OFF -DSDL_AUDIO=OFF -DSDL_GPU=OFF -DSDL_CAMERA=OFF \
  -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_POWER=OFF \
  -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VIVANTE=OFF \
  -DSDL_RPI=OFF -DSDL_ROCKCHIP=OFF -DSDL_VULKAN=OFF -DSDL_METAL=OFF -DSDL_OPENVR=OFF \
  -DSDL_OPENGL=ON -DSDL_OPENGLES=ON >/dev/null

echo "[sdl3] build..."
cmake --build "$SDL/build-rv" --target SDL3-static -j"$(nproc)"
LIB="$(find "$SDL/build-rv" -name 'libSDL3.a' | head -1)"
cp "$LIB" "$HERE/../sdl3/libSDL3-riscv.a"
echo "[sdl3] built libSDL3-riscv.a: $(du -h "$HERE/../sdl3/libSDL3-riscv.a" | cut -f1), $(ar t "$LIB" | wc -l) members"
