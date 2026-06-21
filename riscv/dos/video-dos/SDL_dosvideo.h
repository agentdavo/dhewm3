/*
 * SDL3 "dos" video driver for the bare-metal RISC-V dOS.
 *
 * dhewm3-private bring-up code, authored from the human-written design spec in
 * riscv/docs/dos/11-sdl3-video-dos-driver.md. NOT contributed upstream to SDL.
 *
 * The driver is a thin shim: it satisfies SDL's normal GL path
 * (SDL_GL_CreateContext / SDL_GL_GetProcAddress / SDL_GL_SwapWindow) by routing
 * to gl4es + softgles2, and a software-framebuffer path that blits to the dOS
 * framebuffer sink. It deliberately does NOT use SDL's EGL machinery
 * (SDL_VIDEO_OPENGL_EGL) — that pulls Khronos eglplatform.h which fails on bare
 * metal. The GL function pointers in SDL_video.c are called unconditionally (no
 * SDL_VIDEO_OPENGL* macro gate), so this driver works with zero GL macros set.
 *
 * The actual gl4es/softgles2 plumbing lives outside libSDL3 (linked into the
 * payload), reached through the dos_gl.h C surface so libSDL3 stays clean.
 */
#ifndef SDL_dosvideo_h_
#define SDL_dosvideo_h_

#include "SDL_internal.h"
#include "../SDL_sysvideo.h"

struct SDL_VideoData {
    int fb_w, fb_h;          /* physical framebuffer extents from dOS */
};

struct SDL_WindowData {
    SDL_Window *sdl_window;
    void       *gl_ctx;      /* opaque dos_gl context (NULL for SW windows) */
    bool        is_gl;       /* window->flags & SDL_WINDOW_OPENGL */
};

#endif /* SDL_dosvideo_h_ */
