cmake_minimum_required(VERSION 3.20)
project(glmark2_doomos LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GLMARK2_DATA_PATH "/glmark2" CACHE STRING "Runtime path to glmark2 data")
set(GLMARK2_VERSION "2023.01-doomos" CACHE STRING "glmark2 version string")
set(GLMARK2_DOOMOS_SIZE "320x240" CACHE STRING "DoomOS glmark2 --size argument")
set(GLMARK2_DOOMOS_BENCHMARK "build:use-vbo=false:duration=1" CACHE STRING "DoomOS glmark2 --benchmark argument")
set(GLMARK2_DOOMOS_FRAMEBIN "1" CACHE STRING "Enable softgles2 FrameBin for the DoomOS backend")
set(GLMARK2_DOOMOS_PROFILE "0" CACHE STRING "Enable softgles2 per-frame profiler output")

set(GLMARK2_COMMON_SOURCES
    src/benchmark-collection.cpp
    src/benchmark.cpp
    src/gl-headers.cpp
    src/gl-visual-config.cpp
    src/image-reader.cpp
    src/libmatrix/log.cc
    src/libmatrix/mat.cc
    src/libmatrix/program.cc
    src/libmatrix/shader-source.cc
    src/libmatrix/util.cc
    src/main-loop.cpp
    src/mesh.cpp
    src/model.cpp
    src/options.cpp
    src/results-file.cpp
    src/scene-buffer.cpp
    src/scene-build.cpp
    src/scene-bump.cpp
    src/scene-clear.cpp
    src/scene-conditionals.cpp
    src/scene.cpp
    src/scene-default-options.cpp
    src/scene-desktop.cpp
    src/scene-effect-2d.cpp
    src/scene-function.cpp
    src/scene-grid.cpp
    src/scene-ideas/a.cc
    src/scene-ideas.cpp
    src/scene-ideas/d.cc
    src/scene-ideas/e.cc
    src/scene-ideas/i.cc
    src/scene-ideas/lamp.cc
    src/scene-ideas/logo.cc
    src/scene-ideas/m.cc
    src/scene-ideas/n.cc
    src/scene-ideas/o.cc
    src/scene-ideas/s.cc
    src/scene-ideas/splines.cc
    src/scene-ideas/table.cc
    src/scene-ideas/t.cc
    src/scene-jellyfish.cpp
    src/scene-loop.cpp
    src/scene-pulsar.cpp
    src/scene-refract.cpp
    src/scene-shading.cpp
    src/scene-shadow.cpp
    src/scene-terrain/base-renderer.cpp
    src/scene-terrain/blur-renderer.cpp
    src/scene-terrain/copy-renderer.cpp
    src/scene-terrain.cpp
    src/scene-terrain/luminance-renderer.cpp
    src/scene-terrain/normal-from-height-renderer.cpp
    src/scene-terrain/overlay-renderer.cpp
    src/scene-terrain/renderer-chain.cpp
    src/scene-terrain/simplex-noise-renderer.cpp
    src/scene-terrain/terrain-renderer.cpp
    src/scene-terrain/texture-renderer.cpp
    src/scene-texture.cpp
    src/text-renderer.cpp
    src/texture.cpp
)

add_library(glmark2_zlib STATIC
    src/zlib/adler32.c src/zlib/compress.c src/zlib/crc32.c src/zlib/deflate.c
    src/zlib/infback.c src/zlib/inffast.c src/zlib/inflate.c src/zlib/inftrees.c
    src/zlib/trees.c src/zlib/uncompr.c src/zlib/zutil.c
)
target_include_directories(glmark2_zlib PUBLIC src/zlib)

add_library(glmark2_png STATIC
    src/libpng/png.c src/libpng/pngerror.c src/libpng/pnggccrd.c
    src/libpng/pngget.c src/libpng/pngmem.c src/libpng/pngpread.c
    src/libpng/pngread.c src/libpng/pngrio.c src/libpng/pngrtran.c
    src/libpng/pngrutil.c src/libpng/pngset.c src/libpng/pngtrans.c
    src/libpng/pngvcrd.c src/libpng/pngwio.c src/libpng/pngwrite.c
    src/libpng/pngwtran.c src/libpng/pngwutil.c
)
target_include_directories(glmark2_png PUBLIC src/libpng)
target_link_libraries(glmark2_png PUBLIC glmark2_zlib)

add_library(glmark2_jpeg STATIC
    src/libjpeg-turbo/jcapimin.c src/libjpeg-turbo/jcapistd.c
    src/libjpeg-turbo/jccoefct.c src/libjpeg-turbo/jccolor.c
    src/libjpeg-turbo/jcdctmgr.c src/libjpeg-turbo/jchuff.c
    src/libjpeg-turbo/jcinit.c src/libjpeg-turbo/jcmainct.c
    src/libjpeg-turbo/jcmarker.c src/libjpeg-turbo/jcmaster.c
    src/libjpeg-turbo/jcomapi.c src/libjpeg-turbo/jcparam.c
    src/libjpeg-turbo/jcphuff.c src/libjpeg-turbo/jcprepct.c
    src/libjpeg-turbo/jcsample.c src/libjpeg-turbo/jdapimin.c
    src/libjpeg-turbo/jdapistd.c src/libjpeg-turbo/jdatadst.c
    src/libjpeg-turbo/jdatasrc.c src/libjpeg-turbo/jdcoefct.c
    src/libjpeg-turbo/jdcolor.c src/libjpeg-turbo/jddctmgr.c
    src/libjpeg-turbo/jdhuff.c src/libjpeg-turbo/jdinput.c
    src/libjpeg-turbo/jdmainct.c src/libjpeg-turbo/jdmarker.c
    src/libjpeg-turbo/jdmaster.c src/libjpeg-turbo/jdmerge.c
    src/libjpeg-turbo/jdphuff.c src/libjpeg-turbo/jdpostct.c
    src/libjpeg-turbo/jdsample.c src/libjpeg-turbo/jerror.c
    src/libjpeg-turbo/jfdctflt.c src/libjpeg-turbo/jfdctfst.c
    src/libjpeg-turbo/jfdctint.c src/libjpeg-turbo/jidctflt.c
    src/libjpeg-turbo/jidctfst.c src/libjpeg-turbo/jidctint.c
    src/libjpeg-turbo/jidctred.c src/libjpeg-turbo/jmemmgr.c
    src/libjpeg-turbo/jmemnobs.c src/libjpeg-turbo/jquant1.c
    src/libjpeg-turbo/jquant2.c src/libjpeg-turbo/jsimd_none.c
    src/libjpeg-turbo/jutils.c
)
target_include_directories(glmark2_jpeg PUBLIC src/libjpeg-turbo)

add_library(softgles2 STATIC
    ../softgles2/src/fb.c
    ../softgles2/src/sg_diag.c
    ../softgles2/src/sg_raster.c
    ../softgles2/src/gles2.c
    ../softgles2/src/glsl.c
    ../softgles2/src/glsl_lex.c
    ../softgles2/src/glsl_parse.c
    ../softgles2/src/glsl_ir.c
    ../softgles2/src/glsl_simt.c
    ../softgles2/src/sg_kernels.c
    ../softgles2/src/sg_mt.c
    ../softgles2/src/sg_mt_prim.c
    ../softgles2/src/sg_mt_tiles.c
    ../softgles2/src/sg_mt_raster.c
    ../softgles2/src/sg_mt_sched.c
    ../softgles2/src/sg_state.c
    ../softgles2/src/sg_vertex.c
    ../softgles2/src/sg_frag.c
    ../softgles2/src/sg_frame.c
    ../softgles2/src/sg_framebin.c
    ../softgles2/src/sg_prof.c
    ../softgles2/src/sg_buffer.c
    ../softgles2/src/sg_texture.c
    ../softgles2/src/sg_framebuffer.c
    ../softgles2/src/sg_shader.c
    ../softgles2/src/sg_draw.c
    ../softgles2/src/sg_clip.c
)
target_compile_definitions(softgles2 PUBLIC SGL_BAREMETAL)
target_include_directories(softgles2 PUBLIC
    ../softgles2/include
    ../softgles2/src
    ..
)

add_library(glmark2_gles_loader STATIC src/glad/src/gles2.c)
target_include_directories(glmark2_gles_loader PUBLIC src/glad/include)

add_library(doomos_runtime OBJECT
    ../boot/start.S
    ../boot/runtime.c
    ../boot/sbi.c
    ../boot/trap.c
    ../boot/irq.c
    ../engine/dos_hart_entry.S
    ../engine/dos_syscalls.c
    ../engine/dos_thread.c
    ../engine/dos_virtio_blk.c
    ../engine/dosfs.c
    ../engine/dos_ramfs.c
    ../engine/savefs.c
    ../engine/cxxrt.cpp
    ../libc/string.c
    src/doomos-compat.c
)
target_include_directories(doomos_runtime PRIVATE ..)

add_library(glmark2_common STATIC ${GLMARK2_COMMON_SOURCES})
target_compile_definitions(glmark2_common PUBLIC
    GLMARK2_USE_NULL=1
    GLMARK2_USE_GLESv2=1
    GLMARK2_USE_DOOMOS=1
    GLMARK_VERSION="${GLMARK2_VERSION}"
    GLMARK_DATA_PATH="${GLMARK2_DATA_PATH}"
    M_PI=3.14159265358979323846
    M_E=2.71828182845904523536
)
target_include_directories(glmark2_common PUBLIC
    src
    src/glad/include
    src/libjpeg-turbo
    src/libmatrix
    src/libpng
    src/scene-ideas
    src/scene-terrain
    src/zlib
)
add_dependencies(glmark2_common glmark2_gles_loader glmark2_png glmark2_jpeg softgles2)

add_executable(glmark2-es2-doomos
    $<TARGET_OBJECTS:doomos_runtime>
    src/doomos-kmain.cpp
    src/main.cpp
    src/canvas-generic.cpp
    src/native-state-null.cpp
    src/gl-state-doomos.cpp
)
target_compile_definitions(glmark2-es2-doomos PRIVATE
    GLMARK2_EXECUTABLE="glmark2-es2-doomos"
    GLMARK2_DOOMOS_SIZE="${GLMARK2_DOOMOS_SIZE}"
    GLMARK2_DOOMOS_BENCHMARK="${GLMARK2_DOOMOS_BENCHMARK}"
    GLMARK2_DOOMOS_FRAMEBIN=${GLMARK2_DOOMOS_FRAMEBIN}
    GLMARK2_DOOMOS_PROFILE=${GLMARK2_DOOMOS_PROFILE}
)
target_link_options(glmark2-es2-doomos PRIVATE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.ld"
)
target_link_libraries(glmark2-es2-doomos PRIVATE
    "-Wl,--start-group"
    glmark2_common
    glmark2_gles_loader
    glmark2_png
    glmark2_zlib
    glmark2_jpeg
    softgles2
    c
    m
    stdc++
    supc++
    gcc
    "-Wl,--end-group"
)
set_property(TARGET glmark2-es2-doomos APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.ld"
)
