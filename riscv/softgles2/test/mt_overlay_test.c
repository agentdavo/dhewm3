/*
 * mt_overlay_test — verifies the coverage-aware MT gate through the REAL GL
 * path (gles2 -> sg_draw -> sg_mt_prog_dispatch), not a mock.
 *
 * The gl4es 2D overlay is a 4-vertex GL_TRIANGLE_FAN that fills the screen.
 * Before the fix the MT gate rejected it on vertex count (4 < 192) and it was
 * shaded single-hart. This test draws exactly that shape and asserts the gate
 * now ENGAGES the multi-hart path (sgl_dbg_mt_engaged++), while a genuinely
 * tiny draw still stays single-hart.
 *
 * Runs single-hart (no SMP boot), which is fine: the gate DECISION fires before
 * the worker fan-out, so the engaged counter proves the fix independently of how
 * many harts actually exist. The 4-hart speedup itself is an engine measurement.
 *
 * Build/run:  make -C riscv/softgles2 mtoverlayrun
 */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sgl_gl.h"

extern volatile unsigned long sgl_dbg_mt_engaged;
extern volatile unsigned long sgl_dbg_mt_cover_admit;

static const char *VS =
    "#version 100\n attribute vec4 position;\n attribute vec4 color;\n attribute vec2 texcoord;\n"
    "uniform mat4 mvp;\n varying vec4 v_color;\n varying vec2 v_uv;\n"
    "void main(){ gl_Position = mvp * position; v_color = color; v_uv = texcoord; }\n";
static const char *FS =
    "#version 100\n precision mediump float;\n uniform sampler2D tex0;\n"
    "varying vec4 v_color;\n varying vec2 v_uv;\n"
    "void main(){ gl_FragColor = v_color * texture2D(tex0, v_uv); }\n";

static GLuint compile_prog(void)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER);   glShaderSource(v, 1, &VS, 0); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &FS, 0); glCompileShader(f);
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    return p;
}

static GLuint make_tex(void)
{
    static unsigned char px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; ++i) { px[i*4]=200; px[i*4+1]=120; px[i*4+2]=60; px[i*4+3]=255; }
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* Draw a quad spanning NDC [-s,+s] as a 4-vertex GL_TRIANGLE_FAN — exactly how
 * gl4es submits a 2D blit (blit.c: glDrawArrays(GL_TRIANGLE_FAN, 0, 4)). */
static void blit_quad(int pp, int pc, int pt, float s)
{
    float v[4 * 10] = {    /* 10 floats/vertex: position(4) color(4) texcoord(2) */
        -s,-s, 0,1,  1,1,1,1,  0,0,
         s,-s, 0,1,  1,1,1,1,  1,0,
         s, s, 0,1,  1,1,1,1,  1,1,
        -s, s, 0,1,  1,1,1,1,  0,1,
    };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(pp, 4, GL_FLOAT, GL_FALSE, 10*4, (void*)0);
    glVertexAttribPointer(pc, 4, GL_FLOAT, GL_FALSE, 10*4, (void*)(4*4));
    glVertexAttribPointer(pt, 2, GL_FLOAT, GL_FALSE, 10*4, (void*)(8*4));
    glEnableVertexAttribArray(pp); glEnableVertexAttribArray(pc); glEnableVertexAttribArray(pt);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);   /* 4 vertices, like the gl4es blit */
}

int kmain(void)
{
    printf("\n== softgles2 MT coverage-gate test (gl4es-style 2D blit) ==\n");
    const int W = 800, H = 600;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[mt] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);

    GLuint prog = compile_prog();
    glUseProgram(prog);
    float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_FALSE, mvp);
    glUniform1i(glGetUniformLocation(prog, "tex0"), 0);
    make_tex();
    int pp = glGetAttribLocation(prog, "position");
    int pc = glGetAttribLocation(prog, "color");
    int pt = glGetAttribLocation(prog, "texcoord");

    int fails = 0;

    /* --- fullscreen blit (the overlay): must ENGAGE the multi-hart path --- */
    glClear(GL_COLOR_BUFFER_BIT);
    sgl_dbg_mt_engaged = 0; sgl_dbg_mt_cover_admit = 0;
    blit_quad(pp, pc, pt, 1.0f);                 /* fills 800x600 */
    glFlush();                                    /* FrameBin dispatch boundary */
    unsigned long full_eng = sgl_dbg_mt_engaged, full_blit = sgl_dbg_mt_cover_admit;
    printf("[mt] fullscreen 4-vert blit : mt_engaged=%lu cover_admit=%lu  -> %s\n",
           full_eng, full_blit, (full_eng >= 1 && full_blit >= 1) ? "ENGAGED (ok)" : "SINGLE-HART (FAIL)");
    if (!(full_eng >= 1 && full_blit >= 1)) fails++;

    /* --- tiny blit: must STAY single-hart (below the coverage floor) --- */
    sgl_dbg_mt_engaged = 0; sgl_dbg_mt_cover_admit = 0;
    blit_quad(pp, pc, pt, 0.05f);                /* ~40x30 px, well under 64x64 */
    glFlush();
    unsigned long tiny_eng = sgl_dbg_mt_engaged;
    printf("[mt] tiny 4-vert blit       : mt_engaged=%lu              -> %s\n",
           tiny_eng, (tiny_eng == 0) ? "single-hart (ok)" : "ENGAGED (FAIL)");
    if (tiny_eng != 0) fails++;

    printf("[mt] glGetError=0x%04X\n", glGetError());
    printf("[mt] %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    printf("[ok] mt overlay test complete; powering off.\n");
    sgl_fb_destroy(fb);
    return fails;
}
