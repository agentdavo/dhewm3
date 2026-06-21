/*
 * softgles2 "full" demo — drives the WHOLE software renderer directly (no gl4es,
 * no SDL, no engine): real GLSL ES shaders (A2 bytecode IR + A3 SIMT-on-RVV
 * fragment path), VBOs, two textures (bilinear + nearest), depth-tested overdraw,
 * per-fragment lambert lighting, and an alpha-blended pass. Renders to a 256x256
 * framebuffer and prints an ASCII-art preview + checksum over serial.
 *
 * Build/run:  make -C riscv/softgles2 full
 *             riscv/scripts/run.sh riscv/softgles2/build/full_rv.elf
 */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sgl_gl.h"

/* Lit + textured shader. FS uses texture2D / normalize / dot / max / clamp / mul
 * — all SIMT-vectorisable, so glsl_fs_simt_ok() should report 1 and the A3
 * SIMT-on-RVV fragment executor runs it. */
static const char *LIT_VS =
    "#version 100\n"
    "attribute vec4 position;\n attribute vec4 color;\n attribute vec2 texcoord;\n attribute vec3 normal;\n"
    "uniform mat4 mvp;\n"
    "varying vec4 v_color;\n varying vec2 v_uv;\n varying vec3 v_normal;\n"
    "void main(){ gl_Position = mvp * position; v_color = color; v_uv = texcoord; v_normal = normal; }\n";

static const char *LIT_FS =
    "#version 100\n precision mediump float;\n"
    "uniform sampler2D tex0;\n uniform vec4 lightdir;\n"
    "varying vec4 v_color;\n varying vec2 v_uv;\n varying vec3 v_normal;\n"
    "void main(){\n"
    "  float lambert = max(dot(normalize(v_normal), lightdir.xyz), 0.25);\n"
    "  vec4 t = texture2D(tex0, v_uv);\n"
    "  gl_FragColor = clamp(v_color * t * lambert, 0.0, 1.0);\n"
    "}\n";

/* Procedural-colour, constant-alpha shader for the translucent pass. */
static const char *BLEND_VS =
    "#version 100\n attribute vec4 position;\n attribute vec4 color;\n uniform mat4 mvp;\n"
    "varying vec4 v_color;\n void main(){ gl_Position = mvp * position; v_color = color; }\n";
static const char *BLEND_FS =
    "#version 100\n precision mediump float;\n varying vec4 v_color;\n"
    "void main(){ gl_FragColor = vec4(v_color.rgb, 0.45); }\n";

static GLuint compile_prog(const char *vs, const char *fs)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER);   glShaderSource(v, 1, &vs, 0); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fs, 0); glCompileShader(f);
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    return p;
}

/* 32x32 RGBA texture: checker if checker!=0, else a smooth gradient. */
static GLuint make_tex(int checker, int filter)
{
    static unsigned char px[32 * 32 * 4];
    const int s = 32;
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            unsigned char *p = &px[(y * s + x) * 4];
            if (checker) {
                int c = (((x >> 2) ^ (y >> 2)) & 1);
                if (c) { p[0] = 240; p[1] = 240; p[2] = 240; } else { p[0] = 50; p[1] = 60; p[2] = 110; }
            } else {
                p[0] = (unsigned char)(x * 255 / (s - 1));
                p[1] = (unsigned char)(y * 255 / (s - 1));
                p[2] = (unsigned char)(160);
            }
            p[3] = 255;
        }
    GLuint t;
    glGenTextures(1, &t); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* One textured+lit quad: 6 verts x (pos3,col4,uv2,nrm3)=12 floats, stride 48. */
static void quad(int lp, int lc, int lt, int ln, float cx, float cy, float z,
                 float r, float g, float b, float nx, float ny, float nz)
{
    const float h = 0.42f;
    float v[6 * 12];
    const float corner[6][4] = { /* x,y, u,v  (two tris) */
        {-h,-h, 0,1}, { h,-h, 1,1}, { h, h, 1,0},
        {-h,-h, 0,1}, { h, h, 1,0}, {-h, h, 0,0},
    };
    for (int i = 0; i < 6; ++i) {
        float *o = &v[i * 12];
        o[0]=cx+corner[i][0]; o[1]=cy+corner[i][1]; o[2]=z;     /* position */
        o[3]=r; o[4]=g; o[5]=b; o[6]=1.0f;                      /* color    */
        o[7]=corner[i][2]; o[8]=corner[i][3];                   /* texcoord */
        o[9]=nx; o[10]=ny; o[11]=nz;                            /* normal   */
    }
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 12*4, (void*)0);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 12*4, (void*)(3*4));
    glVertexAttribPointer(lt, 2, GL_FLOAT, GL_FALSE, 12*4, (void*)(7*4));
    glVertexAttribPointer(ln, 3, GL_FLOAT, GL_FALSE, 12*4, (void*)(9*4));
    glEnableVertexAttribArray(lp); glEnableVertexAttribArray(lc);
    glEnableVertexAttribArray(lt); glEnableVertexAttribArray(ln);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

int kmain(void)
{
    printf("\n== softgles2 FULL renderer demo (bare metal, rva23s64) ==\n");
    const int W = 256, H = 256;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[full] fb alloc failed\n"); return 1; }
    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);

#ifdef SGL_FORCE_FRAMEBIN
    /* A/B validation: defer the whole frame and drain it via glFinish() below.
     * Output must be byte-identical to the immediate path. */
    { extern int sgl_framebin_enabled; sgl_framebin_enabled = 1; }
#endif
#ifdef SGL_FORCE_RVVTEX
    /* A/B: route texture2D through the RVV gather sampler instead of the
     * per-lane scalar fallback (the engine default leaves this off). */
    { extern int glsl_rvv_texture2d_enabled; glsl_rvv_texture2d_enabled = 1; }
#endif

    glClearColor(0.07f, 0.10f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    /* scaled-identity MVP (column-major): a small margin around the scene */
    float mvp[16] = { 0.92f,0,0,0,  0,0.92f,0,0,  0,0,1,0,  0,0,0,1 };
    float lightdir[3] = { 0.40f, 0.45f, 0.80f };  /* roughly normalised */

    GLuint lit = compile_prog(LIT_VS, LIT_FS);
    glUseProgram(lit);
    glUniformMatrix4fv(glGetUniformLocation(lit, "mvp"), 1, GL_FALSE, mvp);
    glUniform4f(glGetUniformLocation(lit, "lightdir"), lightdir[0], lightdir[1], lightdir[2], 0.0f);
    glUniform1i(glGetUniformLocation(lit, "tex0"), 0);
    /* NB: the GL draw path runs the A2 bytecode IR (glsl_run_fragment); the A3
     * SIMT-on-RVV executor is validated separately by simt_validate.elf and is
     * not yet wired into sgl_raster_tri_prog. */

    int lp = glGetAttribLocation(lit, "position");
    int lc = glGetAttribLocation(lit, "color");
    int lt = glGetAttribLocation(lit, "texcoord");
    int ln = glGetAttribLocation(lit, "normal");

    GLuint tex_bilinear = make_tex(1, GL_LINEAR);   /* checker, bilinear */
    GLuint tex_nearest  = make_tex(0, GL_NEAREST);  /* gradient, nearest */

    uint64_t t0 = timer_ticks();

    /* Three overlapping quads at different depths + normals (depth test resolves
     * the overdraw; differing normals give visibly different lambert shading).
     * Alternate the two textures so both rvv_sample filter paths run. */
    glBindTexture(GL_TEXTURE_2D, tex_bilinear);
    quad(lp,lc,lt,ln, -0.20f,  0.18f, 0.70f,  1.0f,0.5f,0.4f,  0.0f,0.0f,1.0f);  /* far, facing light */
    glBindTexture(GL_TEXTURE_2D, tex_nearest);
    quad(lp,lc,lt,ln,  0.22f,  0.10f, 0.50f,  0.4f,0.9f,0.5f,  0.6f,0.0f,0.8f);  /* mid, tilted */
    glBindTexture(GL_TEXTURE_2D, tex_bilinear);
    quad(lp,lc,lt,ln, -0.02f, -0.20f, 0.30f,  0.5f,0.6f,1.0f,  -0.3f,0.4f,0.85f);/* near, occludes */

    /* Alpha-blended triangle on top (no depth write). */
    GLuint blend = compile_prog(BLEND_VS, BLEND_FS);
    glUseProgram(blend);
    glUniformMatrix4fv(glGetUniformLocation(blend, "mvp"), 1, GL_FALSE, mvp);
    glDisableVertexAttribArray(lt); glDisableVertexAttribArray(ln);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    int bp = glGetAttribLocation(blend, "position");
    int bc = glGetAttribLocation(blend, "color");
    float tri[3*12];
    const float tv[3][3] = { {0.0f,0.55f,0.1f}, {-0.5f,-0.45f,0.1f}, {0.5f,-0.45f,0.1f} };
    const float tcol[3][3] = { {1,1,1}, {1,0.9f,0.2f}, {0.2f,1,1} };
    for (int i=0;i<3;i++){ float*o=&tri[i*12]; o[0]=tv[i][0];o[1]=tv[i][1];o[2]=tv[i][2];
        o[3]=tcol[i][0];o[4]=tcol[i][1];o[5]=tcol[i][2];o[6]=1; o[7]=0;o[8]=0; o[9]=0;o[10]=0;o[11]=1; }
    GLuint tvbo; glGenBuffers(1,&tvbo); glBindBuffer(GL_ARRAY_BUFFER,tvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(bp, 3, GL_FLOAT, GL_FALSE, 12*4, (void*)0);
    glVertexAttribPointer(bc, 4, GL_FLOAT, GL_FALSE, 12*4, (void*)(3*4));
    glEnableVertexAttribArray(bp); glEnableVertexAttribArray(bc);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFinish();   /* drain deferred frame (FrameBin); no-op in immediate mode */

    uint64_t t1 = timer_ticks();

    printf("[full] renderer = \"%s\"  version = \"%s\"\n",
           (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    printf("[full] %dx%d frame in %lu ticks, glGetError=0x%04X, checksum=0x%08X rgb=0x%08X\n",
           W, H, (unsigned long)(t1 - t0), glGetError(), sgl_fnv1a(fb), sgl_fnv1a_rgb(fb));
    sgl_dump_ascii(fb, 64, 32);

    printf("[ok] softgles2 full demo complete; powering off.\n");
    sgl_fb_destroy(fb);
    return 0;
}
