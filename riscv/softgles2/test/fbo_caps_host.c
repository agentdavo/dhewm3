#include <stdio.h>
#include <string.h>
#include "sgl.h"
#include "sgl_gl.h"

static int fail(const char *msg)
{
    fprintf(stderr, "fbo_caps_host: FAIL: %s glerr=0x%04x\n", msg, glGetError());
    return 1;
}

static int expect_u8(const char *msg, unsigned got, unsigned want)
{
    if (got != want) {
        fprintf(stderr, "fbo_caps_host: FAIL: %s got=%u want=%u\n", msg, got, want);
        return 1;
    }
    return 0;
}

int main(void)
{
    sgl_framebuffer *fb = sgl_fb_create(4, 4);
    if (!fb) return fail("fb alloc");
    sglMakeCurrent(fb);

    GLint v = -1;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &v);
    if (v != 0) return fail("vertex texture units should be zero");
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &v);
    if (v != GL_RGBA) return fail("read format query");
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &v);
    if (v != GL_UNSIGNED_BYTE) return fail("read type query");

    glClearColor(1.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("color-only FBO incomplete");

    glClearColor(0.f, 0.f, 1.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4];
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (expect_u8("fbo blue r", px[0], 0)) return 1;
    if (expect_u8("fbo blue b", px[2], 255)) return 1;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (expect_u8("default stayed red r", px[0], 255)) return 1;
    if (expect_u8("default stayed red b", px[2], 0)) return 1;

    GLenum err = glGetError();
    sgl_fb_destroy(fb);
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "fbo_caps_host: FAIL: final glerr=0x%04x\n", err);
        return 1;
    }
    printf("fbo_caps_host: ok\n");
    return 0;
}
