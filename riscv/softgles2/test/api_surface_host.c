#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sgl.h"
#include "sgl_gl.h"

static int fail(const char *msg)
{
    fprintf(stderr, "api_surface_host: FAIL: %s glerr=0x%04x\n", msg, glGetError());
    return 1;
}

static int expect_u8(const char *msg, unsigned got, unsigned want)
{
    if (got != want) {
        fprintf(stderr, "api_surface_host: FAIL: %s got=%u want=%u\n", msg, got, want);
        return 1;
    }
    return 0;
}

static void make_depth_stencil(GLuint *depth, GLuint *stencil, int w, int h)
{
    glGenRenderbuffers(1, depth);
    glBindRenderbuffer(GL_RENDERBUFFER, *depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glGenRenderbuffers(1, stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, *stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, w, h);
}

static int attach_texture_fbo(GLuint tex, GLuint depth, GLuint stencil, GLuint *fbo)
{
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("FBO incomplete");
    return 0;
}

int main(void)
{
    sgl_framebuffer *fb = sgl_fb_create(4, 4);
    if (!fb) return fail("fb alloc");
    sglMakeCurrent(fb);

    /* Core ES 2.0 error and clamping semantics.  These are easy for a renderer
     * focused on the hot draw path to miss, but applications rely on sticky
     * first-error behaviour and on invalid calls leaving state unchanged. */
    glViewport(1, 2, 3, 4);
    glViewport(9, 9, -1, 4);
    if (glGetError() != GL_INVALID_VALUE) return fail("negative viewport");
    glDepthFunc(0xDEADu);
    if (glGetError() != GL_INVALID_ENUM) return fail("invalid depth func");
    glEnable(0xDEADu);
    if (glGetError() != GL_INVALID_ENUM) return fail("invalid enable cap");
    if (glGetString(0xDEADu) != NULL || glGetError() != GL_INVALID_ENUM)
        return fail("invalid string name");
    glEnableVertexAttribArray(SGL_MAX_VERTEX_ATTRIBS);
    if (glGetError() != GL_INVALID_VALUE) return fail("invalid attrib index");
    glVertexAttribPointer(0, 5, GL_FLOAT, GL_FALSE, 0, NULL);
    if (glGetError() != GL_INVALID_VALUE) return fail("invalid attrib size");
    glClear(0x1u);
    if (glGetError() != GL_INVALID_VALUE) return fail("invalid clear mask");
    { GLfloat m2[4] = { 1, 0, 0, 1 };
      glUniformMatrix2fv(-1, 1, GL_TRUE, m2);
      if (glGetError() != GL_INVALID_VALUE) return fail("matrix transpose must be false"); }
    glViewport(0, 0, 4, 4);

    glClearColor(1.f, 0.f, 0.f, 1.f);
    glClearDepthf(0.25f);
    glClearStencil(7);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    unsigned char px[16];
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (expect_u8("read rgba r", px[0], 255)) return 1;
    if (expect_u8("read rgba g", px[1], 0)) return 1;
    if (expect_u8("read rgba b", px[2], 0)) return 1;
    unsigned char st = 0;
    glReadPixels(0, 0, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &st);
    if (expect_u8("read stencil", st, 7)) return 1;
    float z = 0.f;
    glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z);
    if (z < 0.249f || z > 0.251f) return fail("read depth");

    GLint v = 0;
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glBlendEquation(GL_FUNC_ADD);
    glGetIntegerv(GL_BLEND_SRC_RGB, &v);
    if (v != GL_SRC_ALPHA) return fail("blend src rgb query");
    glGetIntegerv(GL_BLEND_DST_ALPHA, &v);
    if (v != GL_ZERO) return fail("blend dst alpha query");

    GLuint depth = 0, stencil = 0;
    make_depth_stencil(&depth, &stencil, 4, 4);

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    if (attach_texture_fbo(tex, depth, stencil, &fbo)) return 1;
    glClearColor(0.f, 1.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    if (expect_u8("fbo read r", px[0], 0)) return 1;
    if (expect_u8("fbo read g", px[1], 255)) return 1;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLuint copied = 0, fbo2 = 0;
    glGenTextures(1, &copied);
    glBindTexture(GL_TEXTURE_2D, copied);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 0, 0, 4, 4, 0);
    if (attach_texture_fbo(copied, depth, stencil, &fbo2)) return 1;
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    if (expect_u8("copy read r", px[0], 255)) return 1;
    if (expect_u8("copy read g", px[1], 0)) return 1;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int row = 0; row < fb->h; ++row) {
        for (int col = 0; col < fb->w; ++col) {
            fb->color[(size_t)row * (size_t)fb->w + (size_t)col] =
                (uint32_t)(col * 40) | ((uint32_t)(row * 50) << 8) | 0xFF000000u;
        }
    }
    glBindTexture(GL_TEXTURE_2D, copied);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, 2, 2);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
    memset(px, 0, sizeof(px));
    glReadPixels(1, 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    if (expect_u8("subcopy read r", px[0], 40)) return 1;
    if (expect_u8("subcopy read g", px[1], 100)) return 1;

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "api_surface_host: FAIL: final glerr=0x%04x\n", err);
        return 1;
    }
    sgl_fb_destroy(fb);
    printf("api_surface_host: ok\n");
    return 0;
}
