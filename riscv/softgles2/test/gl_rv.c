/* Bare-metal RISC-V GLES2 driver: render the scene via the gl* API and show it
 * over the serial console as ASCII + a checksum. */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sgl_gl.h"
#include "gl_scene.h"

int kmain(void)
{
    printf("\n== softgles2 GLES2 API test (bare metal, %s) ==\n",
           sgl_rvv_enabled() ? "RVV v1.0" : "scalar");

    const int W = 192, H = 96;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[gl] framebuffer alloc failed\n"); return 1; }

    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);

    uint64_t t0 = timer_ticks();
    gl_scene();
    uint64_t t1 = timer_ticks();

    printf("[gl] renderer = \"%s\"\n", (const char *)glGetString(GL_RENDERER));
    printf("[gl] frame in %lu ticks, glGetError=0x%04X, checksum=0x%08X\n",
           (unsigned long)(t1 - t0), glGetError(), sgl_fnv1a(fb));
    sgl_dump_ascii(fb, 64, 24);

    printf("[ok] GLES2 draw complete; powering off.\n");
    sgl_fb_destroy(fb);
    return 0;
}
