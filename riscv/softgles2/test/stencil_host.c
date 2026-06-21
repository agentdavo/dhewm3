#include <stdio.h>
#include <stdint.h>
#include "sgl.h"
#include "sgl_gl.h"

static void draw_fullscreen(void)
{
    static const GLfloat tri[] = {
        -1.f, -1.f, 0.f, 1.f,
         3.f, -1.f, 0.f, 1.f,
        -1.f,  3.f, 0.f, 1.f,
    };
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static int check(int cond, const char *label)
{
    if (!cond) printf("stencil_host: failed %s\n", label);
    return cond;
}

int main(void)
{
    sgl_framebuffer *fb = sgl_fb_create(8, 8);
    if (!fb) return 2;
    sglMakeCurrent(fb);
    glViewport(0, 0, 8, 8);

    GLint stencil_bits = 0;
    GLint depth_bits = 0;
    GLint red_bits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    glGetIntegerv(GL_RED_BITS, &red_bits);

    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClearDepthf(1.f);
    glClearStencil(128);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glEnable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    draw_fullscreen();

    int ok = 1;
    ok &= check(stencil_bits == 8, "GL_STENCIL_BITS");
    ok &= check(depth_bits == 24, "GL_DEPTH_BITS");
    ok &= check(red_bits == 8, "GL_RED_BITS");
    ok &= check(fb->stencil[4 * fb->w + 4] == 1, "replace");
    ok &= check(sgl_pixel(fb, 4, 4) == 0xFF000000u, "color masked");

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glVertexAttrib4f(1, 1.f, 0.f, 0.f, 1.f);
    glStencilFunc(GL_GEQUAL, 128, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    draw_fullscreen();
    ok &= check(sgl_pixel(fb, 4, 4) == 0xFF000000u, "GEQUAL rejects low stencil");

    glClearStencil(128);
    glClear(GL_STENCIL_BUFFER_BIT);
    draw_fullscreen();
    ok &= check((sgl_pixel(fb, 4, 4) & 0x00FFFFFFu) == 0x000000FFu, "GEQUAL accepts clear value");

    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClearDepthf(0.25f);
    glClearStencil(7);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);
    draw_fullscreen();
    ok &= check(fb->stencil[4 * fb->w + 4] == 8, "zfail increment");

    GLenum err = glGetError();
    printf("stencil_host: %s glGetError=0x%04X\n", ok && err == GL_NO_ERROR ? "ok" : "fail", err);
    sgl_fb_destroy(fb);
    return (ok && err == GL_NO_ERROR) ? 0 : 1;
}
