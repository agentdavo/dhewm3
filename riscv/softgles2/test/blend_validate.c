/* C2 blend validation: drive real glBlendFunc/glBlendEquation/glBlendColor
 * combinations through the GLES2 pipeline and check each rendered pixel against
 * an INDEPENDENT GL-spec reference (a second factor table written here, not the
 * rasteriser's). Covers every factor + all three equations, RGB and alpha.
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

/* Independent reference factor coefficient (from the GLES2 spec table). */
static float ref_coeff(GLenum f, float cs, float cd, float sa, float da,
                       float cc, float ca, int is_alpha)
{
    switch (f) {
    case GL_ZERO:                     return 0.f;
    case GL_ONE:                      return 1.f;
    case GL_SRC_COLOR:                return cs;
    case GL_ONE_MINUS_SRC_COLOR:      return 1.f - cs;
    case GL_DST_COLOR:                return cd;
    case GL_ONE_MINUS_DST_COLOR:      return 1.f - cd;
    case GL_SRC_ALPHA:                return sa;
    case GL_ONE_MINUS_SRC_ALPHA:      return 1.f - sa;
    case GL_DST_ALPHA:                return da;
    case GL_ONE_MINUS_DST_ALPHA:      return 1.f - da;
    case GL_CONSTANT_COLOR:           return cc;
    case GL_ONE_MINUS_CONSTANT_COLOR: return 1.f - cc;
    case GL_CONSTANT_ALPHA:           return ca;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return 1.f - ca;
    case GL_SRC_ALPHA_SATURATE: {
        if (is_alpha) return 1.f;
        float t = 1.f - da; return sa < t ? sa : t;
    }
    default: return 0.f;
    }
}

static float ref_eq(GLenum eq, float s, float d)
{
    if (eq == GL_FUNC_SUBTRACT) return s - d;
    if (eq == GL_FUNC_REVERSE_SUBTRACT) return d - s;
    return s + d;
}

static int quant(float x) { if (x < 0.f) x = 0.f; if (x > 1.f) x = 1.f; return (int)(x * 255.f + 0.5f); }

static GLuint g_prog, g_loc_src;

static void make_prog(void)
{
    static const char *vs =
        "#version 100\n"
        "attribute vec4 a_pos;\n"
        "void main(){ gl_Position = a_pos; }\n";
    static const char *fs =
        "#version 100\n"
        "precision mediump float;\n"
        "uniform vec4 u_src;\n"
        "void main(){ gl_FragColor = u_src; }\n";
    g_prog = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, 0); glCompileShader(v); glAttachShader(g_prog, v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, 0); glCompileShader(f); glAttachShader(g_prog, f);
    glBindAttribLocation(g_prog, SGL_ATTRIB_POSITION, "a_pos");
    glLinkProgram(g_prog);
    glUseProgram(g_prog);
    g_loc_src = glGetUniformLocation(g_prog, "u_src");
}

static int g_fails = 0;

static void test_case(const char *name, sgl_framebuffer *fb,
                      const float src[4], const float dst[4], const float kc[4],
                      GLenum sf_rgb, GLenum df_rgb, GLenum sf_a, GLenum df_a,
                      GLenum eq_rgb, GLenum eq_a)
{
    /* dst into the framebuffer via clear (C5 stores its alpha too) */
    glDisable(GL_BLEND);
    glClearColor(dst[0], dst[1], dst[2], dst[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBlendColor(kc[0], kc[1], kc[2], kc[3]);
    glBlendFuncSeparate(sf_rgb, df_rgb, sf_a, df_a);
    glBlendEquationSeparate(eq_rgb, eq_a);
    glEnable(GL_BLEND);
    glUniform4f(g_loc_src, src[0], src[1], src[2], src[3]);

    float quad[4 * 4] = {   /* x,y,z,w fullscreen */
        -1.f, -1.f, 0.f, 1.f,   1.f, -1.f, 0.f, 1.f,
         1.f,  1.f, 0.f, 1.f,  -1.f,  1.f, 0.f, 1.f,
    };
    float tri[6 * 4];
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) for (int c = 0; c < 4; ++c) tri[i * 4 + c] = quad[idx[i] * 4 + c];
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(SGL_ATTRIB_POSITION, 4, GL_FLOAT, GL_FALSE, 4 * 4, (void *)0);
    glEnableVertexAttribArray(SGL_ATTRIB_POSITION);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    uint32_t px = sgl_pixel(fb, fb->w / 2, fb->h / 2);
    int got[4] = { (int)(px & 0xFF), (int)((px >> 8) & 0xFF), (int)((px >> 16) & 0xFF), (int)((px >> 24) & 0xFF) };

    int exp[4];
    for (int ch = 0; ch < 3; ++ch) {
        float fs = ref_coeff(sf_rgb, src[ch], dst[ch], src[3], dst[3], kc[ch], kc[3], 0);
        float fd = ref_coeff(df_rgb, src[ch], dst[ch], src[3], dst[3], kc[ch], kc[3], 0);
        exp[ch] = quant(ref_eq(eq_rgb, src[ch] * fs, dst[ch] * fd));
    }
    {
        float fs = ref_coeff(sf_a, src[3], dst[3], src[3], dst[3], kc[3], kc[3], 1);
        float fd = ref_coeff(df_a, src[3], dst[3], src[3], dst[3], kc[3], kc[3], 1);
        exp[3] = quant(ref_eq(eq_a, src[3] * fs, dst[3] * fd));
    }

    int ok = 1;
    for (int c = 0; c < 4; ++c) { int d = got[c] - exp[c]; if (d < 0) d = -d; if (d > 1) ok = 0; }
    if (!ok) g_fails++;
    printf("[blend] %-18s got(%3d,%3d,%3d,%3d) exp(%3d,%3d,%3d,%3d) %s\n", name,
           got[0], got[1], got[2], got[3], exp[0], exp[1], exp[2], exp[3], ok ? "OK" : "FAIL");
}

ENTRY
{
    const int W = 32, H = 32;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[blend] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);
    make_prog();

    float src[4] = { 0.80f, 0.40f, 0.20f, 0.50f };
    float dst[4] = { 0.20f, 0.60f, 0.40f, 0.90f };
    float kc[4]  = { 0.30f, 0.70f, 0.10f, 0.25f };

    test_case("alpha",       fb, src, dst, kc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("additive",    fb, src, dst, kc, GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("modulate",    fb, src, dst, kc, GL_DST_COLOR, GL_ZERO, GL_DST_ALPHA, GL_ZERO, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("premult",     fb, src, dst, kc, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("constant",    fb, src, dst, kc, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("subtract",    fb, src, dst, kc, GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_FUNC_SUBTRACT, GL_FUNC_SUBTRACT);
    test_case("revsubtract", fb, src, dst, kc, GL_ONE, GL_ONE, GL_ONE, GL_ONE, GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
    test_case("dstalpha",    fb, src, dst, kc, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("srcsaturate", fb, src, dst, kc, GL_SRC_ALPHA_SATURATE, GL_ONE, GL_SRC_ALPHA_SATURATE, GL_ONE, GL_FUNC_ADD, GL_FUNC_ADD);
    test_case("sep_rgb_a",   fb, src, dst, kc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_FUNC_ADD);

    printf(g_fails ? "[blend] %d CASE(S) FAILED\n" : "[blend] ALL PASS\n", g_fails);
    sgl_fb_destroy(fb);
    return g_fails ? 1 : 0;
}
