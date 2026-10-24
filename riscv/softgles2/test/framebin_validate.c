/* FrameBin equivalence gate.
 *
 * FrameBin records programmable draws and clears into per-tile bins and replays
 * them, per tile, in submission order. That ordering is chosen specifically so
 * the result is PIXEL-IDENTICAL to immediate mode, so any difference is a bug
 * rather than a tradeoff. This renders the same scene BOTH WAYS IN ONE PROCESS
 * and compares the colour buffers byte for byte.
 *
 * Why in-process, when riscv/scripts/framebin_ab.sh already exists: that script
 * compares per-frame [fbhash] digests between two whole-engine QEMU runs, and
 * that comparison cannot work. Frame content there is a function of WALL CLOCK,
 * not frame index -- com_fixedTic fixes how many usercmds run per frame but
 * latchedTicNumber is still com_ticNumber, which advances with real time. Proved
 * by control experiment: the SAME binary run twice diverges from frame 6 (13 of
 * 19 common frames differed), so the script's "FAIL" carries no information
 * about FrameBin at all. Handover 20 §3.3's "6 identical, frame 6 differs" is
 * that same baseline noise.
 *
 * Here there is no clock and no engine: one process, one scene, two modes,
 * exact comparison. A difference is unambiguously FrameBin.
 *
 * The scene deliberately exercises what binning can reorder or drop:
 *   - many overlapping draws, so per-tile replay order matters
 *   - draws that straddle tile boundaries (binning splits them)
 *   - a mid-scene clear (recorded into bins, must not wipe earlier tiles)
 *   - depth testing (per-tile depth must match global depth)
 *   - blending, which is order-DEPENDENT: any reorder shows up immediately
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

extern int sgl_framebin_enabled;   /* src/sg_framebin.c: 0 = immediate, 1 = deferred */

/* Deliberately not a multiple of the bin/tile size, so draws straddle tiles. */
#define W 149
#define H 101

static const char *VS =
    "attribute vec2 aPos;\n"
    "attribute vec4 aCol;\n"
    "varying vec4 vCol;\n"
    "uniform vec2 uOfs;\n"
    "void main(){ vCol = aCol; gl_Position = vec4(aPos + uOfs, 0.0, 1.0); }\n";

static const char *FS =
    "precision mediump float;\n"
    "varying vec4 vCol;\n"
    "void main(){ gl_FragColor = vCol; }\n";

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

/* One quad as two triangles, positions in clip space. */
static void quad(float x0, float y0, float x1, float y1,
                 const float col[4], GLuint prog, float ox, float oy)
{
    float pos[12] = { x0,y0,  x1,y0,  x1,y1,   x0,y0,  x1,y1,  x0,y1 };
    float cols[24];
    int i;
    for (i = 0; i < 6; ++i) {
        cols[i*4+0] = col[0]; cols[i*4+1] = col[1];
        cols[i*4+2] = col[2]; cols[i*4+3] = col[3];
    }
    glUniform2f(glGetUniformLocation(prog, "uOfs"), ox, oy);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, cols);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* The scene. Identical calls in both modes -- no time, no randomness. */
static void draw_scene(GLuint prog)
{
    int i;
    glViewport(0, 0, W, H);
    glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Opaque overlapping quads, depth-tested. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    for (i = 0; i < 12; ++i) {
        float t = (float)i / 12.0f;
        float c[4] = { t, 1.0f - t, 0.25f + 0.5f * t, 1.0f };
        float s = 0.85f - 0.05f * (float)i;
        quad(-s, -s, s, s, c, prog, -0.30f + 0.05f * (float)i, 0.20f - 0.03f * (float)i);
    }

    /* Mid-scene clear: recorded into the bins, must not erase earlier tiles
     * out of order. This is the case a naive binner gets wrong. */
    glEnable(GL_SCISSOR_TEST);
    glScissor(W / 4, H / 4, W / 2, H / 2);
    glClearColor(0.02f, 0.30f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    /* Order-DEPENDENT blended pass: any replay reorder changes these pixels. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (i = 0; i < 10; ++i) {
        float t = (float)i / 10.0f;
        float c[4] = { 0.9f - 0.6f * t, 0.2f + 0.7f * t, t, 0.35f };
        float s = 0.30f + 0.06f * (float)i;
        quad(-s, -s, s, s, c, prog, 0.35f - 0.07f * (float)i, -0.25f + 0.05f * (float)i);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    /* Thin slivers that land in only one or two tiles each. */
    for (i = 0; i < 8; ++i) {
        float x = -0.9f + 0.22f * (float)i;
        float c[4] = { 1.0f, 0.85f, 0.1f + 0.1f * (float)i, 1.0f };
        quad(x, -0.95f, x + 0.03f, 0.95f, c, prog, 0.0f, 0.0f);
    }
    glFinish();
}

ENTRY
{
    sgl_framebuffer *fb;
    GLuint prog;
    uint32_t *imm;
    size_t n = (size_t)W * (size_t)H, i;
    int diffs = 0, first = -1;

    fb = sgl_fb_create(W, H);
    if (!fb) { printf("[framebin] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    prog = make_prog();

    /* ---- pass 1: immediate ---- */
    sgl_framebin_enabled = 0;
    draw_scene(prog);

    imm = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!imm) { printf("[framebin] copy alloc failed\n"); return 1; }
    for (i = 0; i < n; ++i) imm[i] = fb->color[i];

    /* ---- pass 2: deferred (FrameBin) ---- */
    sgl_framebin_enabled = 1;
    draw_scene(prog);

    for (i = 0; i < n; ++i) {
        if (fb->color[i] != imm[i]) {
            if (first < 0) first = (int)i;
            ++diffs;
        }
    }

    if (diffs == 0) {
        printf("[framebin] %dx%d  30 draws + mid-scene scissored clear + blend"
               "  immediate vs deferred: PASS (bit-exact)\n", W, H);
    } else {
        printf("[framebin] %dx%d  MISMATCH: %d of %zu pixels differ, first at "
               "(%d,%d) imm=%08x bin=%08x\n", W, H, diffs, n,
               first % W, first / W,
               (unsigned)imm[first], (unsigned)fb->color[first]);
    }

    /* Self-check: the comparison must be able to FAIL. Corrupt one pixel of the
     * saved copy and re-run the compare -- if that still reports equal, the
     * gate is measuring nothing (handover 20 §9: confirm a check can fail). */
    imm[n / 2] ^= 0x00ff00ffu;
    {
        int d2 = 0;
        for (i = 0; i < n; ++i) if (fb->color[i] != imm[i]) ++d2;
        printf("[framebin] self-check (1 pixel deliberately corrupted): "
               "%s (%d diff%s)\n",
               d2 == diffs + 1 ? "comparator DETECTS it, OK" : "BROKEN COMPARATOR",
               d2, d2 == 1 ? "" : "s");
    }

    free(imm);
    printf("[framebin] %s\n", diffs ? "FAIL" : "ALL PASS");
    sgl_fb_destroy(fb);
    return diffs ? 1 : 0;
}
