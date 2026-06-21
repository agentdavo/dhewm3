/* Host smoke test: drive *desktop* OpenGL through gl4es, which translates to
 * GLES2 calls served by softgles2, rendering into a softgles2 framebuffer.
 * Proves the gl4es -> softgles2 binding (clear), then an immediate-mode tri. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sgl.h"
#include "bridge.h"
#include "GL/gl.h"      /* gl4es desktop GL headers */

int main(void)
{
    const int W = 192, H = 96;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { fprintf(stderr, "fb alloc failed\n"); return 1; }

    sgl_bridge_init(fb);          /* set_getprocaddress + initialize_gl4es */

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    uint32_t corner = sgl_pixel(fb, 1, 1);
    printf("[bridge] after clear: corner=0x%08X (expect ~0xFF663319)\n", corner);

    /* immediate mode -> gl4es buffers + generates a shader + glDrawArrays */
    glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex3f( 0.0f,  0.8f, 0.0f);
        glColor3f(0, 1, 0); glVertex3f(-0.8f, -0.8f, 0.0f);
        glColor3f(0, 0, 1); glVertex3f( 0.8f, -0.8f, 0.0f);
    glEnd();
    glFinish();

    printf("[bridge] checksum=0x%08X center=0x%08X\n", sgl_fnv1a(fb), sgl_pixel(fb, W/2, H/2));
    sgl_dump_ascii(fb, 64, 24);
    sgl_fb_destroy(fb);
    return 0;
}
