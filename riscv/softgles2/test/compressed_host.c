#include <stdint.h>
#include <stdio.h>
#include "sgl.h"
#include "sgl_gl.h"

static int check_px(const char *label, uint32_t got, uint32_t want)
{
    if (got == want) return 0;
    printf("compressed_host: %s got=0x%08X want=0x%08X\n", label, got, want);
    return 1;
}

int main(void)
{
    sgl_framebuffer *fb = sgl_fb_create(4, 4);
    if (!fb) return 2;
    sglMakeCurrent(fb);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    GLint nfmt = 0;
    GLint fmts[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &nfmt);
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, fmts);
    if (nfmt != 4 || fmts[0] != GL_COMPRESSED_RGB_S3TC_DXT1_EXT) {
        printf("compressed_host: compressed format query failed n=%d first=0x%04X\n", nfmt, fmts[0]);
        return 1;
    }

    /* DXT1: color0=red 565, color1=green 565, all indices 0 => solid red. */
    const uint8_t red_dxt1[8] = { 0x00, 0xF8, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00 };
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
                           4, 4, 0, sizeof(red_dxt1), red_dxt1);
    if (glGetError() != GL_NO_ERROR) {
        printf("compressed_host: upload error\n");
        return 1;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("compressed_host: fbo incomplete\n");
        return 1;
    }

    uint8_t px[4] = {0, 0, 0, 0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t got = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
    int fail = check_px("solid red DXT1", got, 0xFF0000FFu);
    GLenum err = glGetError();
    printf("compressed_host: %s glGetError=0x%04X\n", !fail && err == GL_NO_ERROR ? "ok" : "fail", err);
    sgl_fb_destroy(fb);
    return (!fail && err == GL_NO_ERROR) ? 0 : 1;
}
