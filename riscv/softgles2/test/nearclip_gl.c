/* Integration test for C3 near-plane clipping through the real GLES2
 * programmable pipeline (sg_draw.c draw_prog).
 *
 * Renders a triangle that straddles the near plane (one vertex behind it) and
 * checks the pipeline result equals directly rendering the SAME triangle's
 * pre-clipped polygon (sgl_clip_near_tri applied by hand, all vertices in
 * front). If draw_prog's detection / re-run-VS / rebuild / re-project wiring is
 * correct the two framebuffers are byte-identical — which also exercises varying
 * interpolation end to end (per-vertex colours interpolated at the cuts).
 *
 * Dual-mode: bare-metal QEMU (kmain) today; host once the GLCORE host build is
 * un-broken (timer_ticks + tex2d diag counters). */
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
#include "sg_clip.h"

static GLuint make_prog(void)
{
    static const char *vs =
        "#version 100\n"
        "attribute vec4 a_pos;\n"
        "attribute vec4 a_col;\n"
        "varying vec4 v_col;\n"
        "void main(){ gl_Position = a_pos; v_col = a_col; }\n";
    static const char *fs =
        "#version 100\n"
        "precision mediump float;\n"
        "varying vec4 v_col;\n"
        "void main(){ gl_FragColor = v_col; }\n";

    GLuint p = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, 0); glCompileShader(v); glAttachShader(p, v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, 0); glCompileShader(f); glAttachShader(p, f);
    glBindAttribLocation(p, SGL_ATTRIB_POSITION, "a_pos");
    glBindAttribLocation(p, SGL_ATTRIB_COLOR,    "a_col");
    glLinkProgram(p);
    glUseProgram(p);
    return p;
}

/* draw `nverts` vertices ({x,y,z,w, r,g,b,a} interleaved) as GL_TRIANGLES. */
static void draw_verts(const float *data, int nverts)
{
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 8 * nverts), data, GL_STATIC_DRAW);
    glVertexAttribPointer(SGL_ATTRIB_POSITION, 4, GL_FLOAT, GL_FALSE, 8 * 4, (void *)0);
    glVertexAttribPointer(SGL_ATTRIB_COLOR,    4, GL_FLOAT, GL_FALSE, 8 * 4, (void *)(4 * 4));
    glEnableVertexAttribArray(SGL_ATTRIB_POSITION);
    glEnableVertexAttribArray(SGL_ATTRIB_COLOR);
    glDrawArrays(GL_TRIANGLES, 0, nverts);
}

ENTRY
{
    const int W = 128, H = 128;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[nearclip-gl] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);
#ifdef SGL_FORCE_FRAMEBIN
    { extern int sgl_framebin_enabled; sgl_framebin_enabled = 1; }   /* exercise the deferred path */
#endif
    make_prog();

    /* Straddling triangle: v2 is behind the near plane (z + w = -1 < 0). */
    float tri[3 * 8] = {
        0.0f,  0.5f,  0.5f, 1.0f,   1.f, 0.f, 0.f, 1.f,   /* v0 in   red   */
       -0.5f, -0.5f,  0.5f, 1.0f,   0.f, 1.f, 0.f, 1.f,   /* v1 in   green */
        0.5f, -0.5f, -2.0f, 1.0f,   0.f, 0.f, 1.f, 1.f,   /* v2 out  blue  */
    };

    /* (A) Pipeline render: draw_prog clips internally. */
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_verts(tri, 3);
    glFinish();   /* drain deferred (FrameBin) draws before reading the framebuffer */
    uint32_t a_full = sgl_fnv1a(fb), a_rgb = sgl_fnv1a_rgb(fb);

    /* (B) Reference: clip by hand, then render the resulting polygon directly
     * (all vertices in front -> no pipeline clipping). */
    float in_pos[3][4], in_vary_buf[3][4];
    const float *in_vary[3];
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 4; ++c) in_pos[i][c]      = tri[i * 8 + c];
        for (int c = 0; c < 4; ++c) in_vary_buf[i][c] = tri[i * 8 + 4 + c];
        in_vary[i] = in_vary_buf[i];
    }
    float out_pos[4][4], out_vary[4 * 4];
    int n = sgl_clip_near_tri(in_pos, in_vary, 4, out_pos, out_vary);

    float ref[6 * 8];   /* up to 2 triangles */
    int rv = 0;
    for (int j = 1; j + 1 < n; ++j) {
        const int fan[3] = { 0, j, j + 1 };
        for (int k = 0; k < 3; ++k) {
            int s = fan[k];
            for (int c = 0; c < 4; ++c) ref[rv * 8 + c]     = out_pos[s][c];
            for (int c = 0; c < 4; ++c) ref[rv * 8 + 4 + c] = out_vary[s * 4 + c];
            ++rv;
        }
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (rv) draw_verts(ref, rv);
    glFinish();
    uint32_t b_full = sgl_fnv1a(fb), b_rgb = sgl_fnv1a_rgb(fb);

    printf("[nearclip-gl] clip out verts=%d ref tris=%d glGetError=0x%04X\n", n, rv / 3, glGetError());
    printf("[nearclip-gl] A(pipeline) full=0x%08X rgb=0x%08X\n", a_full, a_rgb);
    printf("[nearclip-gl] B(manual)   full=0x%08X rgb=0x%08X\n", b_full, b_rgb);
    sgl_dump_ascii(fb, 48, 24);

    int ok = (a_full == b_full) && (a_rgb == b_rgb) && (n == 4);
    printf(ok ? "[nearclip-gl] PASS\n" : "[nearclip-gl] FAIL\n");
    sgl_fb_destroy(fb);
    return ok ? 0 : 1;
}
