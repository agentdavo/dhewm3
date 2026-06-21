/* #16 glPolygonOffset validation through the real GLES2 pipeline.
 *
 * Draw two coplanar fullscreen quads (same vertex z). The first writes depth;
 * the second is the "decal" drawn with GL_DEPTH_TEST=LESS, so by default it ties
 * and fails. glPolygonOffset then decides the outcome:
 *   - pull (units<0)  -> decal wins  (drawn in front)
 *   - push (units>0)  -> decal loses
 *   - disabled        -> offset ignored (decal loses), proving the enable gate
 *   - factor (sloped) -> factor*slope wins, proving the slope term is computed
 *
 * Dual-mode: QEMU (kmain) today; host once the GLCORE host build is fixed. */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#  include "boot/runtime.h"
#  define ENTRY int kmain(void)
#else
#  include <stdio.h>
#  define ENTRY int main(void)
#endif
#include <stdint.h>
#include "sgl.h"
#include "sgl_gl.h"

static GLuint g_loc_col;

static void make_prog(void)
{
    static const char *vs =
        "#version 100\n"
        "attribute vec4 a_pos;\n"
        "void main(){ gl_Position = a_pos; }\n";
    static const char *fs =
        "#version 100\n"
        "precision mediump float;\n"
        "uniform vec4 u_col;\n"
        "void main(){ gl_FragColor = u_col; }\n";
    GLuint p = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, 0); glCompileShader(v); glAttachShader(p, v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, 0); glCompileShader(f); glAttachShader(p, f);
    glBindAttribLocation(p, SGL_ATTRIB_POSITION, "a_pos");
    glLinkProgram(p); glUseProgram(p);
    g_loc_col = glGetUniformLocation(p, "u_col");
}

/* fullscreen quad; sloped => clip z = x*0.5 (depth slope != 0), else z = 0. */
static void draw_quad(float r, float g, float b, int sloped)
{
    float zc = 0.f; (void)zc;
    float v[4][4] = {
        { -1.f, -1.f, 0.f, 1.f }, { 1.f, -1.f, 0.f, 1.f },
        {  1.f,  1.f, 0.f, 1.f }, {-1.f,  1.f, 0.f, 1.f },
    };
    if (sloped) for (int i = 0; i < 4; ++i) v[i][2] = v[i][0] * 0.5f;
    float tri[6 * 4];
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) for (int c = 0; c < 4; ++c) tri[i * 4 + c] = v[idx[i]][c];
    glUniform4f(g_loc_col, r, g, b, 1.f);
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(SGL_ATTRIB_POSITION, 4, GL_FLOAT, GL_FALSE, 4 * 4, (void *)0);
    glEnableVertexAttribArray(SGL_ATTRIB_POSITION);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static int g_fails = 0;

/* expect_B: 1 if the decal (green) should win, 0 if the base (red) should win. */
static void test_case(const char *name, sgl_framebuffer *fb, int enable,
                      float factor, float units, int sloped, int expect_B)
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClearDepthf(1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.f, 0.f);
    draw_quad(1.f, 0.f, 0.f, sloped);          /* base: red, writes depth */

    glPolygonOffset(factor, units);
    if (enable) glEnable(GL_POLYGON_OFFSET_FILL);
    draw_quad(0.f, 1.f, 0.f, sloped);          /* decal: green */
    glFinish();

    uint32_t px = sgl_pixel(fb, fb->w / 2, fb->h / 2);
    int R = (int)(px & 0xFF), G = (int)((px >> 8) & 0xFF);
    int got_B = G > R;
    int ok = (got_B == expect_B);
    if (!ok) g_fails++;
    printf("[polyoff] %-14s R=%3d G=%3d -> %s (expect %s) %s\n", name, R, G,
           got_B ? "decal" : "base", expect_B ? "decal" : "base", ok ? "OK" : "FAIL");
}

ENTRY
{
    const int W = 64, H = 64;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[polyoff] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);
    make_prog();

    /* enable, factor, units, sloped, expect_decal_wins */
    test_case("control",   fb, 0,  0.f,  0.f, 0, 0);   /* tie, LESS -> base wins */
    test_case("pull",      fb, 1,  0.f, -1.f, 0, 1);   /* units<0 -> decal wins */
    test_case("push",      fb, 1,  0.f,  1.f, 0, 0);   /* units>0 -> decal loses */
    test_case("disabled",  fb, 0,  0.f, -1.f, 0, 0);   /* offset set but gate off */
    test_case("factor",    fb, 1, -10.f, 0.f, 1, 1);   /* factor*slope -> decal wins */

    printf(g_fails ? "[polyoff] %d CASE(S) FAILED\n" : "[polyoff] ALL PASS\n", g_fails);
    sgl_fb_destroy(fb);
    return g_fails ? 1 : 0;
}
