/*
 * dOS milestone: SDL3 -> gl4es -> softgles2 triangle on bare-metal RISC-V.
 *
 * Drops the forced "dummy" driver: SDL_Init(VIDEO) auto-selects the custom
 * "dos" video driver (DOS_bootstrap, registered first in SDL_video.c's
 * bootstrap[]). Creates an SDL_WINDOW_OPENGL window, an SDL GL context (which
 * brings up gl4es against a softgles2 framebuffer via the dos driver), draws a
 * triangle through gl4es *desktop* GL, then SDL_GL_SwapWindow presents it
 * (scans out to the virtio-gpu device), and powers off.
 *
 * Chain: SDL_GL_GetProcAddress -> DOS_GL_GetProcAddress -> gl4es_GetProcAddress
 *        SDL_GL_CreateContext  -> DOS_GL_CreateContext  -> sgl_bridge_init
 *        SDL_GL_SwapWindow     -> DOS_GL_SwapWindow     -> dvgpu_present_rgba
 */
#include <stdio.h>
#include "boot/runtime.h"
#define SDL_MAIN_HANDLED            /* we provide our own entry (kmain) */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "GL/gl.h"                  /* gl4es desktop GL (glBegin/glVertex/...) */
#include "video-dos/dos_gl.h"      /* dos_gl_present */
#include "sgl.h"                    /* sgl_framebuffer, sgl_fnv1a, sgl_pixel */

int kmain(void)
{
    printf("\n========================================\n");
    printf("  dOS: SDL3 -> gl4es -> softgles2 triangle\n");
    printf("========================================\n");

    int v = SDL_GetVersion();
    printf("[dos] SDL %d.%d.%d\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));

    printf("[dos] video drivers compiled in:");
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) printf(" %s", SDL_GetVideoDriver(i));
    printf("\n");

    SDL_SetMainReady();
    /* No SDL_HINT_VIDEO_DRIVER: "dos" must be auto-selected. */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[dos] SDL_Init(VIDEO) FAILED: %s\n", SDL_GetError());
        return 1;
    }
    const char *drv = SDL_GetCurrentVideoDriver();
    printf("[dos] SDL_Init(VIDEO) ok; current driver = \"%s\"\n", drv ? drv : "(null)");
    if (!drv || SDL_strcmp(drv, "dos") != 0) {
        printf("[dos] FAIL: expected auto-selected driver \"dos\"\n");
        SDL_Quit();
        return 1;
    }

    const int W = 800, H = 600;

    /* --- verify the software framebuffer present path (non-GL) first --- */
    /* A plain window -> SDL_GetWindowSurface routes through
     * DOS_CreateWindowFramebuffer; SDL_UpdateWindowSurface ->
     * DOS_UpdateWindowFramebuffer -> dos_fb_blit. (Done before the GL window so
     * it doesn't disturb the captured GL frame; dos_present is gated/armed.) */
    SDL_Window *sw = SDL_CreateWindow("dOS-sw", 64, 48, 0);
    if (sw) {
        SDL_Surface *s = SDL_GetWindowSurface(sw);
        printf("[dos] SW path: SDL_GetWindowSurface = %p%s%s\n", (void *)s,
               s ? "" : " : ", s ? "" : SDL_GetError());
        if (s) {
            SDL_FillSurfaceRect(s, NULL, 0x00FF8800);   /* XRGB8888 */
            bool ok = SDL_UpdateWindowSurface(sw);
            printf("[dos] SW path: SDL_UpdateWindowSurface (dos_fb_blit) = %s%s%s\n",
                   ok ? "ok" : "FAIL", ok ? "" : " : ", ok ? "" : SDL_GetError());
        }
        SDL_DestroyWindow(sw);
    }

    SDL_Window *w = SDL_CreateWindow("dOS", W, H, SDL_WINDOW_OPENGL);
    if (!w) {
        printf("[dos] SDL_CreateWindow(OPENGL) FAILED: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    printf("[dos] SDL_CreateWindow(OPENGL %dx%d) ok = %p\n", W, H, (void *)w);

    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx) {
        printf("[dos] SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
        SDL_DestroyWindow(w);
        SDL_Quit();
        return 1;
    }
    printf("[dos] SDL_GL_CreateContext ok = %p\n", (void *)ctx);

    /* Prove the SDL GL proc resolver reaches gl4es. */
    void *clearp = (void *)SDL_GL_GetProcAddress("glClear");
    printf("[dos] SDL_GL_GetProcAddress(\"glClear\") = %p\n", clearp);

    SDL_GL_SetSwapInterval(0);

    /* Draw a triangle through gl4es *desktop* GL -> GLES2 -> softgles2. */
    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex3f( 0.0f,  0.8f, 0.0f);
        glColor3f(0, 1, 0); glVertex3f(-0.8f, -0.8f, 0.0f);
        glColor3f(0, 0, 1); glVertex3f( 0.8f, -0.8f, 0.0f);
    glEnd();
    glFinish();

    /* Present scans out to virtio-gpu; capture host-side via QEMU screendump. */
    if (!SDL_GL_SwapWindow(w)) {
        printf("[dos] SDL_GL_SwapWindow FAILED: %s\n", SDL_GetError());
    } else {
        printf("[dos] SDL_GL_SwapWindow ok (presented to scanout)\n");
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("[ok] dOS rendered SDL3 -> gl4es -> softgles2 triangle; powering off.\n");
    return 0;
}
