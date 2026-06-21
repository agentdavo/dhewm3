/* Bare-metal RISC-V driver: render the GLSL-shader scene (interpreter +
 * bilinear) and show it over serial. */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sgl_gl.h"
#include "glsl_scene.h"

int kmain(void)
{
    printf("\n== softgles2 GLSL-ES interpreter scene (bare metal) ==\n");

    const int W = 192, H = 96;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[gl] framebuffer alloc failed\n"); return 1; }

    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);

    uint64_t t0 = timer_ticks();
    glsl_scene();
    uint64_t t1 = timer_ticks();

    printf("[gl] renderer = \"%s\"\n", (const char *)glGetString(GL_RENDERER));
    printf("[gl] frame in %lu ticks, glGetError=0x%04X, checksum=0x%08X\n",
           (unsigned long)(t1 - t0), glGetError(), sgl_fnv1a(fb));
    sgl_dump_ascii(fb, 64, 24);

    printf("[ok] GLSL interpreter draw complete; powering off.\n");
    sgl_fb_destroy(fb);
    return 0;
}
