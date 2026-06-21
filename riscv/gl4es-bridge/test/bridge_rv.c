/*
 * Bare-metal RISC-V capstone: boot with no OS, drive desktop OpenGL through
 * gl4es, which translates to GLES2 served by softgles2 (RVV rasteriser), and
 * render into a framebuffer shown over the serial console.
 *
 * Chain: kmain -> gl4es (GL1.x -> GLES2 + generated GLSL) -> bridge
 *        (set_getprocaddress) -> softgles2 (GLSL interpreter + RVV raster).
 */
#include <stdint.h>
#include "libc/libc.h"          /* printf (resolved to newlib at link) */
#include "boot/runtime.h"
#include "sgl.h"
#include "bridge.h"
#include "GL/gl.h"              /* gl4es desktop GL */

int kmain(void)
{
    printf("\n== gl4es -> softgles2 bare-metal bridge (no OS) ==\n");

    const int W = 800, H = 600;   /* bare-metal default resolution */
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[bridge] framebuffer alloc failed\n"); return 1; }

    sgl_bridge_init(fb);                 /* set_getprocaddress + initialize_gl4es */

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    uint64_t t0 = timer_ticks();
    glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex3f( 0.0f,  0.8f, 0.0f);
        glColor3f(0, 1, 0); glVertex3f(-0.8f, -0.8f, 0.0f);
        glColor3f(0, 0, 1); glVertex3f( 0.8f, -0.8f, 0.0f);
    glEnd();
    glFinish();
    uint64_t t1 = timer_ticks();

    printf("[bridge] frame in %lu ticks; checksum=0x%08X center=0x%08X corner=0x%08X\n",
           (unsigned long)(t1 - t0), sgl_fnv1a(fb),
           sgl_pixel(fb, W / 2, H / 2), sgl_pixel(fb, 1, 1));
    sgl_dump_ascii(fb, 64, 24);

    printf("[ok] bare-metal gl4es -> softgles2 render complete; powering off.\n");
    sgl_fb_destroy(fb);
    return 0;
}
