/*
 * hiz_bench — measures the hierarchical-Z whole-triangle occlusion cull.
 *
 * Front-to-back overdraw, the case hi-Z is built for: draw one near full-screen
 * occluder (writes depth), then N far full-screen quads that are fully behind it.
 * With hi-Z every far quad is rejected at triangle entry (zero fragment work);
 * without it, every far fragment is rastered and depth-rejected one by one.
 *
 * Reports ticks + cull count for hi-Z OFF then ON over the same scene, so the
 * speedup is the ratio. Output checksum is printed for both: it MUST match
 * (the cull only removes fragments that would have failed the depth test).
 *
 * Build/run:  make -C riscv/softgles2 hizrun
 */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sgl_gl.h"

extern int sgl_hiz_enabled;
extern volatile unsigned long sgl_dbg_tris_hiz_culled;
extern volatile unsigned long sgl_dbg_tris_onscreen;
extern int sgl_diag_enabled;

static const char *VS =
    "#version 100\n attribute vec4 position;\n attribute vec4 color;\n uniform mat4 mvp;\n"
    "varying vec4 v_color;\n attribute vec2 texcoord;\n varying vec2 v_uv;\n"
    "void main(){ gl_Position = mvp * position; v_color = color; v_uv = texcoord; }\n";
/* Deliberately non-trivial per-fragment cost (texture + a little math) so the
 * cull saving reflects a realistic shaded pixel, not just a store. */
static const char *FS =
    "#version 100\n precision mediump float;\n uniform sampler2D tex0;\n"
    "varying vec4 v_color;\n varying vec2 v_uv;\n"
    "void main(){ vec4 t = texture2D(tex0, v_uv);\n"
    "  gl_FragColor = clamp(v_color * t * (0.5 + 0.5*v_uv.x), 0.0, 1.0); }\n";

static GLuint compile_prog(const char *vs, const char *fs)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER);   glShaderSource(v, 1, &vs, 0); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fs, 0); glCompileShader(f);
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    return p;
}

static GLuint make_tex(void)
{
    static unsigned char px[32 * 32 * 4];
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            unsigned char *p = &px[(y * 32 + x) * 4];
            int c = ((x ^ y) & 4) ? 220 : 90;
            p[0] = (unsigned char)c; p[1] = (unsigned char)(c / 2); p[2] = 180; p[3] = 255;
        }
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* Full-screen quad at clip depth z, vertex colour (r,g,b). */
static void fsquad(int lp, int lc, int lt, float z, float r, float g, float b)
{
    float v[6 * 8];
    const float corner[6][4] = {
        {-1,-1, 0,1}, { 1,-1, 1,1}, { 1, 1, 1,0},
        {-1,-1, 0,1}, { 1, 1, 1,0}, {-1, 1, 0,0},
    };
    for (int i = 0; i < 6; ++i) {
        float *o = &v[i * 8];
        o[0]=corner[i][0]; o[1]=corner[i][1]; o[2]=z; o[3]=1;
        o[4]=r; o[5]=g; o[6]=b; /* fits in color.rgb; a comes from texcoord pack below */
    }
    /* pack: position(0..2)+pad(3), color rgb(4..6), and reuse a 2nd buffer for uv */
    float pv[6 * 4], cv[6 * 4], tv[6 * 2];
    for (int i = 0; i < 6; ++i) {
        pv[i*4+0]=v[i*8+0]; pv[i*4+1]=v[i*8+1]; pv[i*4+2]=v[i*8+2]; pv[i*4+3]=1;
        cv[i*4+0]=v[i*8+4]; cv[i*4+1]=v[i*8+5]; cv[i*4+2]=v[i*8+6]; cv[i*4+3]=1;
        tv[i*2+0]=corner[i][2]; tv[i*2+1]=corner[i][3];
    }
    GLuint vb,cb,tb;
    glGenBuffers(1,&vb); glBindBuffer(GL_ARRAY_BUFFER,vb); glBufferData(GL_ARRAY_BUFFER,sizeof(pv),pv,GL_STATIC_DRAW);
    glVertexAttribPointer(lp,4,GL_FLOAT,GL_FALSE,0,(void*)0); glEnableVertexAttribArray(lp);
    glGenBuffers(1,&cb); glBindBuffer(GL_ARRAY_BUFFER,cb); glBufferData(GL_ARRAY_BUFFER,sizeof(cv),cv,GL_STATIC_DRAW);
    glVertexAttribPointer(lc,4,GL_FLOAT,GL_FALSE,0,(void*)0); glEnableVertexAttribArray(lc);
    glGenBuffers(1,&tb); glBindBuffer(GL_ARRAY_BUFFER,tb); glBufferData(GL_ARRAY_BUFFER,sizeof(tv),tv,GL_STATIC_DRAW);
    glVertexAttribPointer(lt,2,GL_FLOAT,GL_FALSE,0,(void*)0); glEnableVertexAttribArray(lt);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* One full scene: near occluder first, then N far quads behind it. */
static unsigned render_scene(sgl_framebuffer *fb, int lp, int lc, int lt, int far_count)
{
    glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    fsquad(lp, lc, lt, 0.20f, 1.0f, 0.7f, 0.5f);          /* near occluder */
    for (int i = 0; i < far_count; ++i)                    /* fully occluded */
        fsquad(lp, lc, lt, 0.80f, 0.2f, 0.4f, 0.9f);
    return sgl_fnv1a(fb);
}

int kmain(void)
{
    printf("\n== softgles2 hi-Z occlusion bench (rva23s64) ==\n");
    const int W = 512, H = 512;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[hiz] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);

    float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    GLuint prog = compile_prog(VS, FS);
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_FALSE, mvp);
    glUniform1i(glGetUniformLocation(prog, "tex0"), 0);
    make_tex();
    int lp = glGetAttribLocation(prog, "position");
    int lc = glGetAttribLocation(prog, "color");
    int lt = glGetAttribLocation(prog, "texcoord");

    const int FAR = 16;   /* overdraw depth behind the occluder */

    /* ---- production timings: diagnostics OFF (the engine default) ---- */
    sgl_diag_enabled = 0;
    sgl_hiz_enabled = 0;
    uint64_t t0 = timer_ticks();
    unsigned ck_off = render_scene(fb, lp, lc, lt, FAR);
    uint64_t t1 = timer_ticks();

    sgl_hiz_enabled = 1;
    uint64_t t2 = timer_ticks();
    unsigned ck_on = render_scene(fb, lp, lc, lt, FAR);
    uint64_t t3 = timer_ticks();

    /* ---- same full-raster scene with diagnostics armed: the delta vs the
     * diag-off OFF run is the per-batch diag overhead now gated out of the
     * production hot loop (nzout/nzstore masks + vcpop reductions). ---- */
    sgl_diag_enabled = 1;
    sgl_hiz_enabled = 0;
    uint64_t t4 = timer_ticks();
    unsigned ck_diag = render_scene(fb, lp, lc, lt, FAR);
    uint64_t t5 = timer_ticks();
    /* and one diag-on hi-Z run to read the cull count */
    sgl_hiz_enabled = 1;
    sgl_dbg_tris_hiz_culled = 0;
    render_scene(fb, lp, lc, lt, FAR);
    unsigned long culled = sgl_dbg_tris_hiz_culled;

    uint64_t off = t1 - t0, on = t3 - t2, diagoff = off, diagon = t5 - t4;
    printf("[hiz] %dx%d, 1 occluder + %d occluded full-screen quads\n", W, H, FAR);
    printf("[hiz] hi-Z OFF (diag off): %lu ticks  checksum=0x%08X\n", (unsigned long)off, ck_off);
    printf("[hiz] hi-Z ON  (diag off): %lu ticks  checksum=0x%08X  culled=%lu tris\n",
           (unsigned long)on, ck_on, culled);
    if (on) printf("[hiz] hi-Z speedup = %lu.%02lux\n",
                   (unsigned long)(off / on),
                   (unsigned long)((off * 100 / on) % 100));
    printf("[hiz] full raster diag ON : %lu ticks  checksum=0x%08X\n", (unsigned long)diagon, ck_diag);
    if (diagoff) printf("[hiz] diag hot-loop overhead = %lu%% (now gated out of production)\n",
                        (unsigned long)((diagon > diagoff ? (diagon - diagoff) * 100 / diagoff : 0)));
    printf("[hiz] correctness: checksums %s\n",
           (ck_off == ck_on && ck_off == ck_diag) ? "MATCH (ok)" : "DIFFER (BUG)");

    printf("[ok] hi-Z bench complete; powering off.\n");
    sgl_fb_destroy(fb);
    return 0;
}
