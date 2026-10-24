/* Post-transform vertex dedupe equivalence gate.
 *
 * sg_vertex_prog_simt_dedup() hashes an indexed draw's index list, transforms
 * each DISTINCT vertex once, then expands back to one entry per corner. That is
 * claimed bit-exact by construction: each vertex depends only on its own
 * attributes and the uniforms. "By construction" is an argument, not a
 * measurement -- this measures it.
 *
 * Method: render the SAME indexed mesh twice IN ONE PROCESS, once with dedupe
 * disabled and once enabled (sgl_vs_dedup_min_verts), and compare the colour
 * buffers byte for byte. Deterministic by construction: no wall clock, no
 * engine, no frame pacing. Contrast the whole-engine [fbhash] A/B, which cannot
 * answer questions like this at all -- frame content there depends on real time,
 * so even two runs of the IDENTICAL binary diverge (measured: from frame 6).
 *
 * The mesh is a closed indexed grid, so corners reuse vertices ~3:1 -- well
 * inside the path's SGL_VS_DEDUP_MAX_PCT=90 reuse gate, and far above
 * SGL_VS_DEDUP_MIN_VERTS=64, so the optimisation genuinely engages. A wrong
 * expansion (mis-mapped unique -> corner) shows up as displaced geometry.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#  include "boot/runtime.h"
#  define ENTRY int kmain(void)
#else
#  include <stdio.h>
#  include <stdlib.h>
#  define ENTRY int main(void)
#endif
#include <stdint.h>
#include "sgl.h"
#include "sgl_gl.h"

extern int sgl_vs_dedup_min_verts;   /* src/sg_vertex.c */

#define W    160
#define H    120
#define GRID 40                        /* 41x41 verts, 3200 tris, 9600 corners */

static const char *VS =
    "attribute vec3 aPos;\n"
    "attribute vec3 aCol;\n"
    "varying vec3 vCol;\n"
    "uniform mat4 uMVP;\n"
    "void main(){ vCol = aCol; gl_Position = uMVP * vec4(aPos, 1.0); }\n";

static const char *FS =
    "precision mediump float;\n"
    "varying vec3 vCol;\n"
    "void main(){ gl_FragColor = vec4(vCol, 1.0); }\n";

static float g_pos[(GRID + 1) * (GRID + 1) * 3];
static float g_col[(GRID + 1) * (GRID + 1) * 3];
static unsigned short g_idx[GRID * GRID * 6];

static void build_mesh(void)
{
    int x, y, i = 0, k = 0;
    for (y = 0; y <= GRID; ++y) {
        for (x = 0; x <= GRID; ++x) {
            float u = (float)x / (float)GRID, v = (float)y / (float)GRID;
            /* A little height variation so the transform is not degenerate. */
            g_pos[i * 3 + 0] = u * 1.8f - 0.9f;
            g_pos[i * 3 + 1] = v * 1.8f - 0.9f;
            g_pos[i * 3 + 2] = 0.25f * (u - 0.5f) * (v - 0.5f);
            g_col[i * 3 + 0] = u;
            g_col[i * 3 + 1] = v;
            g_col[i * 3 + 2] = 1.0f - u * v;
            ++i;
        }
    }
    for (y = 0; y < GRID; ++y) {
        for (x = 0; x < GRID; ++x) {
            unsigned short a = (unsigned short)(y * (GRID + 1) + x);
            unsigned short b = (unsigned short)(a + 1);
            unsigned short c = (unsigned short)(a + GRID + 1);
            unsigned short d = (unsigned short)(c + 1);
            g_idx[k++] = a; g_idx[k++] = b; g_idx[k++] = c;
            g_idx[k++] = b; g_idx[k++] = d; g_idx[k++] = c;
        }
    }
}

static GLuint make_prog(void)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint p  = glCreateProgram();
    glShaderSource(vs, 1, &VS, 0); glCompileShader(vs);
    glShaderSource(fs, 1, &FS, 0); glCompileShader(fs);
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aCol");
    glLinkProgram(p);
    glUseProgram(p);
    return p;
}

static void draw_scene(GLuint prog)
{
    /* A rotation+perspective-ish MVP so every component of the transform is
     * exercised, not just a pass-through. Constant: no time input. */
    static const float mvp[16] = {
        0.9f, 0.1f, 0.0f, 0.0f,
       -0.1f, 0.8f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.05f, -0.02f, 0.0f, 1.0f
    };
    glViewport(0, 0, W, H);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, g_pos);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, g_col);
    glEnableVertexAttribArray(1);
    glDrawElements(GL_TRIANGLES, GRID * GRID * 6, GL_UNSIGNED_SHORT, g_idx);
    glFinish();
}

ENTRY
{
    sgl_framebuffer *fb;
    GLuint prog;
    uint32_t *ref;
    size_t n = (size_t)W * (size_t)H, i;
    int diffs = 0, first = -1;
    const int corners = GRID * GRID * 6;

    build_mesh();
    fb = sgl_fb_create(W, H);
    if (!fb) { printf("[vsdedup] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    prog = make_prog();

    /* ---- reference: dedupe OFF (threshold above this draw's corner count) ---- */
    sgl_vs_dedup_min_verts = corners + 1;
    draw_scene(prog);
    ref = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!ref) { printf("[vsdedup] copy alloc failed\n"); return 1; }
    for (i = 0; i < n; ++i) ref[i] = fb->color[i];

    /* ---- candidate: dedupe ON ---- */
    sgl_vs_dedup_min_verts = 64;
    draw_scene(prog);

    for (i = 0; i < n; ++i)
        if (fb->color[i] != ref[i]) { if (first < 0) first = (int)i; ++diffs; }

    {   /* Integer maths: the bare-metal printf has no %f conversion -- using one
         * printed the format string literally. */
        int uniq = (GRID + 1) * (GRID + 1);
        int reuse_x10 = (corners * 10 + uniq / 2) / uniq;
        printf("[vsdedup] %dx%d  %d corners -> %d unique verts (%d.%dx reuse)\n",
               W, H, corners, uniq, reuse_x10 / 10, reuse_x10 % 10);
    }
    if (diffs == 0)
        printf("[vsdedup] dedupe off vs on: PASS (bit-exact)\n");
    else
        printf("[vsdedup] MISMATCH: %d of %zu pixels differ, first at (%d,%d) "
               "off=%08x on=%08x\n", diffs, n, first % W, first / W,
               (unsigned)ref[first], (unsigned)fb->color[first]);

    /* The gate must be able to fail (handover 20 §9). */
    ref[n / 2] ^= 0x00ff00ffu;
    {
        int d2 = 0;
        for (i = 0; i < n; ++i) if (fb->color[i] != ref[i]) ++d2;
        printf("[vsdedup] self-check (1 pixel corrupted): %s (%d diff%s)\n",
               d2 == diffs + 1 ? "comparator DETECTS it, OK" : "BROKEN COMPARATOR",
               d2, d2 == 1 ? "" : "s");
    }

    free(ref);
    printf("[vsdedup] %s\n", diffs ? "FAIL" : "ALL PASS");
    sgl_fb_destroy(fb);
    return diffs ? 1 : 0;
}
