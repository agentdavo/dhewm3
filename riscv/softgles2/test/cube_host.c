#include <stdio.h>
#include <stdint.h>
#include "sgl.h"
#include "sgl_gl.h"

static int close_u8(uint32_t got, uint32_t want)
{
    for (int i = 0; i < 3; ++i) {
        int g = (int)((got >> (i * 8)) & 0xffu);
        int w = (int)((want >> (i * 8)) & 0xffu);
        int d = g - w;
        if (d < 0) d = -d;
        if (d > 3) return 0;
    }
    return 1;
}

int main(void)
{
    sgl_framebuffer *fb = sgl_fb_create(8, 8);
    if (!fb) return 2;
    sglMakeCurrent(fb);
    glViewport(0, 0, 8, 8);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const uint8_t rgba[6][4] = {
        {255,   0,   0, 255},
        {  0, 255,   0, 255},
        {  0,   0, 255, 255},
        {255, 255,   0, 255},
        {255,   0, 255, 255},
        {  0, 255, 255, 255},
    };
    const uint32_t expect[6] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u,
        0xff00ffffu, 0xffff00ffu, 0xffffff00u,
    };
    for (int i = 0; i < 6; ++i)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)i, 0, GL_RGBA, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba[i]);

    const char *vs =
        "attribute vec4 position;\n"
        "void main(){ gl_Position = position; }\n";
    const char *fs =
        "precision mediump float;\n"
        "uniform samplerCube cube;\n"
        "uniform vec3 dir;\n"
        "void main(){ gl_FragColor = textureCube(cube, dir); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, 0);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(v);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "position");
    glLinkProgram(p);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "cube"), 0);

    const GLfloat tri[] = { -1.f, -1.f, 0.f, 1.f, 3.f, -1.f, 0.f, 1.f, -1.f, 3.f, 0.f, 1.f };
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);

    const GLfloat dirs[6][3] = {
        { 1.f,  0.f,  0.f},
        {-1.f,  0.f,  0.f},
        { 0.f,  1.f,  0.f},
        { 0.f, -1.f,  0.f},
        { 0.f,  0.f,  1.f},
        { 0.f,  0.f, -1.f},
    };
    GLint dir_loc = glGetUniformLocation(p, "dir");
    int ok = 1;
    for (int i = 0; i < 6; ++i) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform3fv(dir_loc, 1, dirs[i]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        uint32_t px = sgl_pixel(fb, 4, 4);
        if (!close_u8(px, expect[i])) {
            printf("cube face %d mismatch got=0x%08X want=0x%08X\n", i, px, expect[i]);
            ok = 0;
        }
    }
    GLenum err = glGetError();
    printf("cube_host: %s glGetError=0x%04X\n", ok && err == GL_NO_ERROR ? "ok" : "fail", err);
    sgl_fb_destroy(fb);
    return (ok && err == GL_NO_ERROR) ? 0 : 1;
}
